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
 *   with a caller-selected ALPN protocol, plus a build-capability probe. None
 *   of them knows what a frame is: HPACK, framing, the stream state machine
 *   and flow control all live in stdlib/http2/http2.xr. What is left here is
 *   the part Xray cannot state -- a socket, a TLS session, and the protocol
 *   the peer agreed to speak over it.
 *
 *   Connections are addressed by an integer handle rather than a pointer, so
 *   no address ever crosses into Xray. A handle carries a generation counter,
 *   which is what makes a stale handle fail cleanly instead of aliasing onto
 *   whatever connection later reused its slot.
 */

#include "../../stdlib/common.h"
#include "../../src/module/xstdlib_runtime_cache.h"
#include "../../src/base/xmalloc.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/mem/xheap.h"
#include "../../src/os/os_thread.h"
#include "../../src/vm/xvm.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xstring.h"
#include "../../stdlib/net/io.h"
#include "../../stdlib/net/tls.h"
#include <string.h>

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
    xr_mutex_lock(&http2_context_init_lock);
    XrStdlibCache *cache = X ? xr_stdlib_cache_get(X) : NULL;
    XrHttp2Context *ctx = cache ? (XrHttp2Context *) cache->http2_state : NULL;
    if (!ctx) {
        ctx = (XrHttp2Context *) xr_calloc(1, sizeof(XrHttp2Context));
        if (ctx && cache) {
            ctx->free_head = -1;
            xr_mutex_init(&ctx->lock);
            cache->http2_state = ctx;
            cache->http2_state_cleanup = http2_context_destroy;
        } else if (ctx) {
            xr_free(ctx);
            ctx = NULL;
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
    /* The generation is never zero, so the encoded handle is positive and
     * cannot collide with a private negative provider outcome. */
    return ((int64_t) generation << 32) | (int64_t) (slot + 1);
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

static XrValue h2_supported(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    if (argc != 0)
        return xr_bool(false);
    return xr_bool(xr_tls_is_available());
}

/* http2.__connect(host, port, timeoutMs, alpn) -> i64. Negative values are
 * raw provider outcomes that http2.xr maps to typed public errors:
 * -1 transport failure, -2 TLS unavailable, -3 ALPN mismatch. */
static XrCFuncResult h2_connect(XrVMRuntime *X, XrValue *args, int nargs, XrValue *result) {
    *result = xr_int(-1);
    if (nargs != 4 || !XR_IS_STRING(args[0]) || !XR_IS_INT(args[1]) || !XR_IS_INT(args[2]) ||
        !XR_IS_STRING(args[3]))
        return XR_CFUNC_DONE;

    if (!xr_tls_is_available()) {
        *result = xr_int(-2);
        return XR_CFUNC_DONE;
    }

    XrHttp2Context *ctx = http2_get_context(X);
    if (!ctx)
        return XR_CFUNC_DONE;

    int64_t port = XR_TO_INT(args[1]);
    int64_t timeout_value = XR_TO_INT(args[2]);
    if (port < 1 || port > 65535 || timeout_value <= 0 || timeout_value > INT32_MAX)
        return XR_CFUNC_DONE;
    const char *host = XR_STRING_CHARS(XR_TO_STRING(args[0]));
    XrString *requested_alpn = XR_TO_STRING(args[3]);
    if (!host || requested_alpn->length == 0 || requested_alpn->length > UINT8_MAX)
        return XR_CFUNC_DONE;

    unsigned char alpn_wire[UINT8_MAX + 1];
    alpn_wire[0] = (unsigned char) requested_alpn->length;
    memcpy(alpn_wire + 1, XR_STRING_CHARS(requested_alpn), requested_alpn->length);

    XrTlsContext *tls_ctx = xr_tls_context_new_client();
    if (!tls_ctx)
        return XR_CFUNC_DONE;
    if (xr_tls_context_set_alpn(tls_ctx, alpn_wire, requested_alpn->length + 1) != 0) {
        h2_conn_close(NULL, tls_ctx);
        return XR_CFUNC_DONE;
    }

    XrIOConn *io = xr_io_connect_tls_with_ctx(X, tls_ctx, host, (int) port, (int) timeout_value);
    if (!io) {
        h2_conn_close(NULL, tls_ctx);
        return XR_CFUNC_DONE;
    }

    const char *selected_alpn = io->tls ? xr_tls_conn_get_alpn(io->tls) : NULL;
    if (!selected_alpn || strlen(selected_alpn) != requested_alpn->length ||
        memcmp(selected_alpn, XR_STRING_CHARS(requested_alpn), requested_alpn->length) != 0) {
        h2_conn_close(io, tls_ctx);
        *result = xr_int(-3);
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

/* http2.__send(handle, data, offset) -> i64; performs one TLS write and
 * answers the accepted byte count, or a negative value on failure. */
static XrCFuncResult h2_send(XrVMRuntime *X, XrValue *args, int nargs, XrValue *result) {
    *result = xr_int(-1);
    if (nargs != 3 || !XR_IS_INT(args[0]) || !XR_IS_ARRAY(args[1]) || !XR_IS_INT(args[2]))
        return XR_CFUNC_DONE;
    XrArray *data = XR_TO_ARRAY(args[1]);
    int64_t offset = XR_TO_INT(args[2]);
    if (data->elem_type != XR_ELEM_U8 || data->length <= 0 || offset < 0 || offset >= data->length)
        return XR_CFUNC_DONE;
    XrIOConn *io = h2_conn_for_handle(X, XR_TO_INT(args[0]));
    if (!io)
        return XR_CFUNC_DONE;
    if (!io->is_tls || !io->tls || !data->data)
        return XR_CFUNC_DONE;
    int written = xr_tls_conn_write(io->X, io->tls, (const uint8_t *) data->data + offset,
                                    (size_t) (data->length - offset));
    if (written <= 0)
        io->last_error = XR_NERR_WRITE;
    *result = xr_int(written);
    return XR_CFUNC_DONE;
}

/* http2.__recv(handle, maxBytes, timeoutMs) -> Array<u8>?
 *
 * One read. A short read is normal and is answered as-is; reassembling frames
 * from short reads is http2.xr's job. An empty array means EOF; null means a
 * transport failure. The source layer maps those outcomes to public errors. */
static XrCFuncResult h2_recv(XrVMRuntime *X, XrValue *args, int nargs, XrValue *result) {
    *result = xr_null();
    if (nargs != 3 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]) || !XR_IS_INT(args[2]))
        return XR_CFUNC_DONE;
    int64_t requested = XR_TO_INT(args[1]);
    int64_t timeout_value = XR_TO_INT(args[2]);
    if (requested <= 0 || requested > INT32_MAX || timeout_value <= 0 || timeout_value > INT32_MAX)
        return XR_CFUNC_DONE;

    XrIOConn *io = h2_conn_for_handle(X, XR_TO_INT(args[0]));
    if (!io)
        return XR_CFUNC_DONE;
    xr_io_set_timeout(io, (int) timeout_value);

    XrCoroutine *coro = xr_current_coro(X);
    XrArray *buffer = xr_byte_array_new(coro, (int32_t) requested);
    if (!buffer)
        return XR_CFUNC_DONE;

    if (!io->is_tls || !io->tls || !buffer->data)
        return XR_CFUNC_DONE;
    int n = xr_tls_conn_read(io->X, io->tls, buffer->data, (size_t) requested);
    if (n < 0) {
        io->last_error = XR_NERR_READ;
        return XR_CFUNC_DONE;
    }
    if (n == 0)
        io->last_error = XR_NERR_CLOSED;
    buffer->length = (int32_t) n;
    *result = xr_value_from_array(buffer);
    return XR_CFUNC_DONE;
}

/* http2.__close(handle) -> (); stale handles are already closed. */
static XrValue h2_close(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc != 1 || !XR_IS_INT(args[0]))
        return XR_NULL_VAL;
    XrHttp2Context *ctx = http2_get_context(X);
    if (!ctx)
        return XR_NULL_VAL;
    XrIOConn *io = NULL;
    XrTlsContext *tls_ctx = NULL;
    if (!h2_slot_take(ctx, XR_TO_INT(args[0]), &io, &tls_ctx))
        return XR_NULL_VAL;
    h2_conn_close(io, tls_ctx);
    return XR_NULL_VAL;
}

#define XR_STDLIB_VM_BIND_MODULE_HTTP2 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_HTTP2
