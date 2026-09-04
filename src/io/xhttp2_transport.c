/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhttp2_transport.c - Opaque TLS transport provider for http2.xr
 */

#include "xhttp2_transport.h"

#include <string.h>

#include "../base/xmalloc.h"
#include "../module/xstdlib_runtime_cache.h"
#include "../os/os_thread.h"
#include "xnet_transport.h"
#include "xtls_provider.h"

#define XR_H2_MAX_SLOTS 4096

typedef struct XrH2Slot {
    XrIOConn *io;
    XrTlsContext *tls_context;
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

static void http2_transport_release(XrIOConn *io, XrTlsContext *tls_context) {
    if (io)
        xr_io_close(io);
    if (tls_context)
        xr_tls_context_free(tls_context);
}

static void http2_context_destroy(void *handle) {
    XrHttp2Context *context = (XrHttp2Context *) handle;
    if (!context)
        return;
    for (int32_t i = 0; i < context->count; i++) {
        if (context->slots[i].in_use)
            http2_transport_release(context->slots[i].io, context->slots[i].tls_context);
    }
    xr_free(context->slots);
    xr_mutex_destroy(&context->lock);
    xr_free(context);
}

static XrHttp2Context *http2_context_get(XrVMRuntime *isolate) {
    xr_mutex_lock(&http2_context_init_lock);
    XrStdlibCache *cache = isolate ? xr_stdlib_cache_get(isolate) : NULL;
    XrHttp2Context *context = cache ? (XrHttp2Context *) cache->http2_state : NULL;
    if (!context) {
        context = (XrHttp2Context *) xr_calloc(1, sizeof(XrHttp2Context));
        if (context && cache) {
            context->free_head = -1;
            xr_mutex_init(&context->lock);
            cache->http2_state = context;
            cache->http2_state_cleanup = http2_context_destroy;
        } else if (context) {
            xr_free(context);
            context = NULL;
        }
    }
    xr_mutex_unlock(&http2_context_init_lock);
    return context;
}

static int32_t http2_slot_index(const XrHttp2Context *context, int64_t handle) {
    if (handle <= 0)
        return -1;
    int32_t slot = (int32_t) ((handle & INT64_C(0xffffffff)) - 1);
    uint32_t generation = (uint32_t) ((uint64_t) handle >> 32);
    if (slot < 0 || slot >= context->count)
        return -1;
    if (!context->slots[slot].in_use || context->slots[slot].generation != generation)
        return -1;
    return slot;
}

static int64_t http2_slot_register(XrHttp2Context *context, XrIOConn *io,
                                   XrTlsContext *tls_context) {
    xr_mutex_lock(&context->lock);
    int32_t slot = context->free_head;
    if (slot >= 0) {
        context->free_head = context->slots[slot].next_free;
    } else {
        if (context->count == context->capacity) {
            int32_t grown = context->capacity == 0 ? 8 : context->capacity * 2;
            if (grown > XR_H2_MAX_SLOTS)
                grown = XR_H2_MAX_SLOTS;
            if (grown == context->capacity) {
                xr_mutex_unlock(&context->lock);
                return -1;
            }
            XrH2Slot *grown_slots =
                (XrH2Slot *) xr_realloc(context->slots, (size_t) grown * sizeof(XrH2Slot));
            if (!grown_slots) {
                xr_mutex_unlock(&context->lock);
                return -1;
            }
            memset(grown_slots + context->capacity, 0,
                   (size_t) (grown - context->capacity) * sizeof(XrH2Slot));
            context->slots = grown_slots;
            context->capacity = grown;
        }
        slot = context->count++;
    }

    uint32_t generation = ++context->next_generation;
    if (generation == 0)
        generation = ++context->next_generation;
    context->slots[slot] = (XrH2Slot) {
        .io = io,
        .tls_context = tls_context,
        .generation = generation,
        .in_use = true,
        .next_free = -1,
    };
    xr_mutex_unlock(&context->lock);
    return ((int64_t) generation << 32) | (int64_t) (slot + 1);
}

static bool http2_slot_take(XrHttp2Context *context, int64_t handle, XrIOConn **io,
                            XrTlsContext **tls_context) {
    xr_mutex_lock(&context->lock);
    int32_t slot = http2_slot_index(context, handle);
    if (slot < 0) {
        xr_mutex_unlock(&context->lock);
        return false;
    }
    *io = context->slots[slot].io;
    *tls_context = context->slots[slot].tls_context;
    context->slots[slot].io = NULL;
    context->slots[slot].tls_context = NULL;
    context->slots[slot].in_use = false;
    context->slots[slot].next_free = context->free_head;
    context->free_head = slot;
    xr_mutex_unlock(&context->lock);
    return true;
}

static XrIOConn *http2_connection_get(XrVMRuntime *isolate, int64_t handle) {
    XrHttp2Context *context = http2_context_get(isolate);
    if (!context)
        return NULL;
    xr_mutex_lock(&context->lock);
    int32_t slot = http2_slot_index(context, handle);
    XrIOConn *io = slot < 0 ? NULL : context->slots[slot].io;
    xr_mutex_unlock(&context->lock);
    return io;
}

bool xr_http2_transport_supported(void) {
    return xr_tls_is_available();
}

int64_t xr_http2_transport_connect(XrVMRuntime *isolate, const char *host, int port, int timeout_ms,
                                   const char *alpn, size_t alpn_length) {
    if (!xr_tls_is_available())
        return -2;
    XrHttp2Context *context = http2_context_get(isolate);
    if (!context || !host || !alpn || alpn_length == 0 || alpn_length > UINT8_MAX)
        return -1;

    unsigned char alpn_wire[UINT8_MAX + 1];
    alpn_wire[0] = (unsigned char) alpn_length;
    memcpy(alpn_wire + 1, alpn, alpn_length);

    XrTlsContext *tls_context = xr_tls_context_new_client();
    if (!tls_context)
        return -1;
    if (xr_tls_context_set_alpn(tls_context, alpn_wire, alpn_length + 1) != 0) {
        http2_transport_release(NULL, tls_context);
        return -1;
    }

    XrIOConn *io = xr_io_connect_tls_with_ctx(isolate, tls_context, host, port, timeout_ms);
    if (!io) {
        http2_transport_release(NULL, tls_context);
        return -1;
    }
    const char *selected_alpn = io->tls ? xr_tls_conn_get_alpn(io->tls) : NULL;
    if (!selected_alpn || strlen(selected_alpn) != alpn_length ||
        memcmp(selected_alpn, alpn, alpn_length) != 0) {
        http2_transport_release(io, tls_context);
        return -3;
    }

    xr_io_set_timeout(io, timeout_ms);
    int64_t handle = http2_slot_register(context, io, tls_context);
    if (handle < 0)
        http2_transport_release(io, tls_context);
    return handle;
}

int xr_http2_transport_send(XrVMRuntime *isolate, int64_t handle, const uint8_t *data,
                            size_t length) {
    XrIOConn *io = http2_connection_get(isolate, handle);
    if (!io || !io->is_tls || !io->tls || !data)
        return -1;
    int written = xr_tls_conn_write(io->X, io->tls, data, length);
    if (written <= 0)
        io->last_error = XR_NERR_WRITE;
    return written;
}

int xr_http2_transport_recv(XrVMRuntime *isolate, int64_t handle, uint8_t *data, size_t length,
                            int timeout_ms) {
    XrIOConn *io = http2_connection_get(isolate, handle);
    if (!io || !io->is_tls || !io->tls || !data)
        return -1;
    xr_io_set_timeout(io, timeout_ms);
    int count = xr_tls_conn_read(io->X, io->tls, data, length);
    if (count < 0)
        io->last_error = XR_NERR_READ;
    else if (count == 0)
        io->last_error = XR_NERR_CLOSED;
    return count;
}

void xr_http2_transport_close(XrVMRuntime *isolate, int64_t handle) {
    XrHttp2Context *context = http2_context_get(isolate);
    if (!context)
        return;
    XrIOConn *io = NULL;
    XrTlsContext *tls_context = NULL;
    if (!http2_slot_take(context, handle, &io, &tls_context))
        return;
    http2_transport_release(io, tls_context);
}
