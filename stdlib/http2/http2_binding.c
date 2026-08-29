/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http2_binding.c - HTTP/2 transport leaves
 *
 * KEY CONCEPT:
 *   Four leaves that move opaque bytes over one TLS connection negotiated
 *   with ALPN "h2", plus a build-capability probe. None of them knows what a
 *   frame is: HPACK, framing, the stream state machine and flow control all
 *   live in stdlib/http2/http2.xr. What is left here is the part Xray cannot
 *   state -- a socket, a TLS session, and the protocol the peer agreed to
 *   speak over it.
 *
 *   Connections are addressed by an integer handle rather than a pointer, so
 *   no address ever crosses into Xray. A handle carries a generation counter,
 *   which is what makes a stale handle fail cleanly instead of aliasing onto
 *   whatever connection later reused its slot.
 */

#include "../../stdlib/common.h"
#include "../../src/base/xmalloc.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/mem/xheap.h"
#include "../../src/os/os_thread.h"
#include "../../src/vm/xvm.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xstring.h"
#include "../../stdlib/net/io.h"
#include "../../stdlib/net/tls.h"
#include <string.h>

/* The protocol list offered on the wire: "h2" first, then "http/1.1" so a
 * server that speaks only HTTP/1.1 answers rather than failing the
 * handshake. Only "h2" is accepted below -- this module has no HTTP/1.1
 * fallback, and http.xr routes those requests elsewhere. */
static const unsigned char ALPN_PROTOS[] = "\x02h2\x08http/1.1";
#define ALPN_PROTOS_LEN 12

/* One read is capped here rather than at the caller: `maxBytes` arrives from
 * Xray and a peer must not be able to name an allocation size. */
#define XR_H2_MAX_READ 65536

/* A connection never outlives its slot, and slots are never freed, so the
 * table only grows. This bounds it. */
#define XR_H2_MAX_SLOTS 4096

typedef struct XrH2Slot {
    XrIOConn *io;
    XrTlsContext *tls_ctx;
    uint32_t generation;
    bool in_use;
    int32_t next_free;
} XrH2Slot;

typedef struct XrHttp2Context {
    XrH2Slot *slots;
    int32_t count;
    int32_t capacity;
    int32_t free_head;
    uint32_t next_generation;
    xr_mutex_t lock;
} XrHttp2Context;

static xr_mutex_t http2_context_init_lock = XR_MUTEX_INITIALIZER;
static void http2_context_destroy(void *handle);

/* Release a connection's resources. `io` must be closed before `tls_ctx`:
 * stdlib/net/io.h:73 requires the context to outlive every connection made
 * from it. */
static void h2_conn_close(XrIOConn *io, XrTlsContext *tls_ctx) {
    if (io)
        xr_io_close(io);
    if (tls_ctx)
        xr_tls_context_free(tls_ctx);
}

static XrHttp2Context *http2_get_context(XrVMRuntime *X) {
    XrModuleRegistry *registry = X ? (XrModuleRegistry *) X->module_registry : NULL;
    XrModule *module = registry && registry->loaded_modules
                           ? (XrModule *) xr_hashmap_get(registry->loaded_modules, "http2")
                           : NULL;
    if (!module)
        return NULL;

    xr_mutex_lock(&http2_context_init_lock);
    XrHttp2Context *ctx = (XrHttp2Context *) module->native_handle;
    if (!ctx) {
        ctx = (XrHttp2Context *) xr_calloc(1, sizeof(XrHttp2Context));
        if (ctx) {
            ctx->free_head = -1;
            xr_mutex_init(&ctx->lock);
            module->native_handle = ctx;
            module->native_handle_destroy = http2_context_destroy;
        }
    }
    xr_mutex_unlock(&http2_context_init_lock);
    return ctx;
}

static void http2_context_destroy(void *handle) {
    XrHttp2Context *ctx = (XrHttp2Context *) handle;
    if (!ctx)
        return;
    for (int32_t i = 0; i < ctx->count; i++) {
        if (ctx->slots[i].in_use)
            h2_conn_close(ctx->slots[i].io, ctx->slots[i].tls_ctx);
    }
    xr_free(ctx->slots);
    xr_mutex_destroy(&ctx->lock);
    xr_free(ctx);
}

/* Handles are (generation << 32) | (slot + 1). The generation is never zero,
 * so a handle is always >= 2^32: positive, and never the -1 that means
 * failure. */
static int64_t h2_handle_make(int32_t slot, uint32_t generation) {
    return ((int64_t) generation << 32) | (int64_t) (slot + 1);
}

/* Resolve a handle to its slot index, or -1 when it names nothing live. */
static int32_t h2_slot_index(const XrHttp2Context *ctx, int64_t handle) {
    if (handle <= 0)
        return -1;
    int32_t slot = (int32_t) ((handle & 0xffffffff) - 1);
    uint32_t generation = (uint32_t) ((uint64_t) handle >> 32);
    if (slot < 0 || slot >= ctx->count)
        return -1;
    if (!ctx->slots[slot].in_use || ctx->slots[slot].generation != generation)
        return -1;
    return slot;
}

/* Take a slot for a new connection. Answers the handle, or -1. */
static int64_t h2_slot_register(XrHttp2Context *ctx, XrIOConn *io, XrTlsContext *tls_ctx) {
    xr_mutex_lock(&ctx->lock);
    int32_t slot = ctx->free_head;
    if (slot >= 0) {
        ctx->free_head = ctx->slots[slot].next_free;
    } else {
        if (ctx->count == ctx->capacity) {
            int32_t grown = ctx->capacity == 0 ? 8 : ctx->capacity * 2;
            if (grown > XR_H2_MAX_SLOTS)
                grown = XR_H2_MAX_SLOTS;
            if (grown == ctx->capacity) {
                xr_mutex_unlock(&ctx->lock);
                return -1;
            }
            XrH2Slot *grown_slots =
                (XrH2Slot *) xr_realloc(ctx->slots, (size_t) grown * sizeof(XrH2Slot));
            if (!grown_slots) {
                xr_mutex_unlock(&ctx->lock);
                return -1;
            }
            memset(grown_slots + ctx->capacity, 0,
                   (size_t) (grown - ctx->capacity) * sizeof(XrH2Slot));
            ctx->slots = grown_slots;
            ctx->capacity = grown;
        }
        slot = ctx->count++;
    }
    uint32_t generation = ++ctx->next_generation;
    if (generation == 0)
        generation = ++ctx->next_generation;
    ctx->slots[slot].io = io;
    ctx->slots[slot].tls_ctx = tls_ctx;
    ctx->slots[slot].generation = generation;
    ctx->slots[slot].in_use = true;
    ctx->slots[slot].next_free = -1;
    xr_mutex_unlock(&ctx->lock);
    return h2_handle_make(slot, generation);
}

/* Detach a live handle's connection so the caller can close it outside the
 * lock. Answers false when the handle names nothing live. */
static bool h2_slot_take(XrHttp2Context *ctx, int64_t handle, XrIOConn **out_io,
                         XrTlsContext **out_ctx) {
    xr_mutex_lock(&ctx->lock);
    int32_t slot = h2_slot_index(ctx, handle);
    if (slot < 0) {
        xr_mutex_unlock(&ctx->lock);
        return false;
    }
    *out_io = ctx->slots[slot].io;
    *out_ctx = ctx->slots[slot].tls_ctx;
    ctx->slots[slot].io = NULL;
    ctx->slots[slot].tls_ctx = NULL;
    ctx->slots[slot].in_use = false;
    ctx->slots[slot].next_free = ctx->free_head;
    ctx->free_head = slot;
    xr_mutex_unlock(&ctx->lock);
    return true;
}

/* Answer a live handle's connection. The connection stays owned by its slot;
 * only h2_close detaches it, and Xray closes each connection exactly once at
 * the end of the request that opened it. */
static XrIOConn *h2_conn_for_handle(XrVMRuntime *X, int64_t handle) {
    XrHttp2Context *ctx = http2_get_context(X);
    if (!ctx)
        return NULL;
    xr_mutex_lock(&ctx->lock);
    int32_t slot = h2_slot_index(ctx, handle);
    XrIOConn *io = slot < 0 ? NULL : ctx->slots[slot].io;
    xr_mutex_unlock(&ctx->lock);
    return io;
}

/* ========== Leaves ========== */

XrValue h2_supported(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;
    return xr_bool(xr_tls_is_available());
}

/* http2.__connect(host, port, timeoutMs) -> i64; -1 when no h2 connection
 * could be made, for any reason. */
XrCFuncResult h2_connect(XrVMRuntime *X, XrValue *args, int nargs, XrValue *result) {
    *result = xr_int(-1);
    if (nargs < 3 || !XR_IS_STRING(args[0]) || !XR_IS_INT(args[1]) || !XR_IS_INT(args[2]))
        return XR_CFUNC_DONE;

    XrHttp2Context *ctx = http2_get_context(X);
    if (!ctx)
        return XR_CFUNC_DONE;

    int64_t port = XR_TO_INT(args[1]);
    int64_t timeout_value = XR_TO_INT(args[2]);
    if (port < 1 || port > 65535 || timeout_value <= 0 || timeout_value > INT32_MAX)
        return XR_CFUNC_DONE;
    const char *host = XR_STRING_CHARS(XR_TO_STRING(args[0]));
    if (!host)
        return XR_CFUNC_DONE;

    XrTlsContext *tls_ctx = xr_tls_context_new_client();
    if (!tls_ctx)
        return XR_CFUNC_DONE;
    xr_tls_context_set_alpn(tls_ctx, ALPN_PROTOS, ALPN_PROTOS_LEN);

    XrIOConn *io = xr_io_connect_tls_with_ctx(X, tls_ctx, host, (int) port, (int) timeout_value);
    if (!io) {
        h2_conn_close(NULL, tls_ctx);
        return XR_CFUNC_DONE;
    }

    /* A peer that did not choose h2 cannot be spoken to here, and answering a
     * handle would let the caller write frames at an HTTP/1.1 server. */
    const char *alpn = io->tls ? xr_tls_conn_get_alpn(io->tls) : NULL;
    if (!alpn || strcmp(alpn, "h2") != 0) {
        h2_conn_close(io, tls_ctx);
        return XR_CFUNC_DONE;
    }

    xr_io_set_timeout(io, (int) timeout_value);
    int64_t handle = h2_slot_register(ctx, io, tls_ctx);
    if (handle < 0) {
        h2_conn_close(io, tls_ctx);
        return XR_CFUNC_DONE;
    }
    *result = xr_int(handle);
    return XR_CFUNC_DONE;
}

/* http2.__send(handle, data) -> bool; writes the whole buffer or fails. */
XrCFuncResult h2_send(XrVMRuntime *X, XrValue *args, int nargs, XrValue *result) {
    *result = xr_bool(false);
    if (nargs < 2 || !XR_IS_INT(args[0]) || !XR_IS_ARRAY(args[1]))
        return XR_CFUNC_DONE;
    XrArray *data = XR_TO_ARRAY(args[1]);
    if (data->elem_type != XR_ELEM_U8 || data->length < 0)
        return XR_CFUNC_DONE;
    if (data->length == 0) {
        *result = xr_bool(true);
        return XR_CFUNC_DONE;
    }
    XrIOConn *io = h2_conn_for_handle(X, XR_TO_INT(args[0]));
    if (!io)
        return XR_CFUNC_DONE;
    int written = xr_io_write_all(io, data->data, (size_t) data->length);
    *result = xr_bool(written == data->length);
    return XR_CFUNC_DONE;
}

/* http2.__recv(handle, maxBytes, timeoutMs) -> Array<u8>?
 *
 * One read. A short read is normal and is answered as-is; reassembling frames
 * from short reads is http2.xr's job. Null means the peer closed or the read
 * failed -- the caller cannot act differently on those two, and conflating
 * them keeps errno out of Xray. */
XrCFuncResult h2_recv(XrVMRuntime *X, XrValue *args, int nargs, XrValue *result) {
    *result = xr_null();
    if (nargs < 3 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]) || !XR_IS_INT(args[2]))
        return XR_CFUNC_DONE;
    int64_t requested = XR_TO_INT(args[1]);
    if (requested <= 0)
        return XR_CFUNC_DONE;
    if (requested > XR_H2_MAX_READ)
        requested = XR_H2_MAX_READ;

    XrIOConn *io = h2_conn_for_handle(X, XR_TO_INT(args[0]));
    if (!io)
        return XR_CFUNC_DONE;
    int64_t timeout_value = XR_TO_INT(args[2]);
    if (timeout_value > 0 && timeout_value <= INT32_MAX)
        xr_io_set_timeout(io, (int) timeout_value);

    XrCoroutine *coro = xr_current_coro(X);
    XrArray *buffer = xr_byte_array_new(coro, (int32_t) requested);
    if (!buffer)
        return XR_CFUNC_DONE;

    int n = xr_io_read(io, buffer->data, (size_t) requested);
    if (n <= 0)
        return XR_CFUNC_DONE;
    buffer->length = (int32_t) n;
    *result = xr_value_from_array(buffer);
    return XR_CFUNC_DONE;
}

/* http2.__close(handle) -> bool; false when the handle named nothing live. */
XrValue h2_close(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_bool(false);
    XrHttp2Context *ctx = http2_get_context(X);
    if (!ctx)
        return xr_bool(false);
    XrIOConn *io = NULL;
    XrTlsContext *tls_ctx = NULL;
    if (!h2_slot_take(ctx, XR_TO_INT(args[0]), &io, &tls_ctx))
        return xr_bool(false);
    h2_conn_close(io, tls_ctx);
    return xr_bool(true);
}

#define XR_STDLIB_VM_BIND_MODULE_HTTP2 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_HTTP2
