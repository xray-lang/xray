/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_buffer.c - Self-growing HTTP buffer implementation
 *
 * KEY CONCEPT:
 *   Exponential-growth buffer with consume/reserve pattern.
 *   Thread-local recycle pool eliminates lock contention.
 */

#include "../../src/base/xmalloc.h"
#include "http_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ========== Buffer Core ========== */

bool http_buffer_init(XrHttpBuffer *buf, size_t initial_capacity) {
    assert(buf);
    if (initial_capacity == 0)
        initial_capacity = XR_HTTP_BUFFER_DEFAULT_CAP;

    char *mem = (char *) xr_malloc(initial_capacity);
    if (!mem) {
        memset(buf, 0, sizeof(*buf));
        return false;
    }

    buf->_base = mem;
    buf->bytes = mem;
    buf->size = 0;
    buf->capacity = initial_capacity;
    return true;
}

void http_buffer_free(XrHttpBuffer *buf) {
    if (!buf)
        return;
    xr_free(buf->_base);
    buf->_base = NULL;
    buf->bytes = NULL;
    buf->size = 0;
    buf->capacity = 0;
}

char *http_buffer_reserve(XrHttpBuffer *buf, size_t min_avail) {
    assert(buf && buf->_base);

    size_t consumed = http_buffer_consumed(buf);
    size_t tail_avail = buf->capacity - consumed - buf->size;

    if (tail_avail >= min_avail) {
        return buf->bytes + buf->size;
    }

    // Try compact first: if consumed > half capacity, memmove is worthwhile
    if (consumed > 0 && consumed >= buf->capacity / 2) {
        http_buffer_compact(buf);
        tail_avail = buf->capacity - buf->size;
        if (tail_avail >= min_avail) {
            return buf->bytes + buf->size;
        }
    }

    // Need realloc: exponential growth
    size_t needed = buf->size + min_avail;
    size_t new_cap = buf->capacity;
    while (new_cap < needed) {
        new_cap = (new_cap < 1024 * 1024) ? new_cap * 2 : new_cap + new_cap / 4;
    }

    char *new_base = (char *) xr_malloc(new_cap);
    if (!new_base)
        return NULL;

    if (buf->size > 0) {
        memcpy(new_base, buf->bytes, buf->size);
    }

    xr_free(buf->_base);
    buf->_base = new_base;
    buf->bytes = new_base;
    buf->capacity = new_cap;

    return buf->bytes + buf->size;
}

void http_buffer_advance(XrHttpBuffer *buf, size_t n) {
    assert(buf);
    buf->size += n;
}

void http_buffer_consume(XrHttpBuffer *buf, size_t n) {
    assert(buf);
    assert(n <= buf->size);

    buf->bytes += n;
    buf->size -= n;

    // Auto-compact when consumed portion exceeds half of capacity
    if (buf->size == 0) {
        // Empty: just reset pointers
        buf->bytes = buf->_base;
    } else if (http_buffer_consumed(buf) > buf->capacity / 2) {
        http_buffer_compact(buf);
    }
}

void http_buffer_compact(XrHttpBuffer *buf) {
    assert(buf);
    size_t consumed = http_buffer_consumed(buf);
    if (consumed == 0)
        return;

    if (buf->size > 0) {
        memmove(buf->_base, buf->bytes, buf->size);
    }
    buf->bytes = buf->_base;
}

void http_buffer_reset(XrHttpBuffer *buf) {
    assert(buf);
    buf->bytes = buf->_base;
    buf->size = 0;
}

/* ========== Thread-Local Recycle Pool ========== */

/*
 * Simple per-thread free list of XrHttpBuffer structs.
 * No mutex, no atomic: pure TLS.
 */
typedef struct {
    XrHttpBuffer *slots[XR_HTTP_BUFFER_MAX_RECYCLE];
    int count;
} XrHttpBufferPool;

static _Thread_local XrHttpBufferPool tls_pool = {.count = 0};

XrHttpBuffer *http_buffer_acquire(size_t initial_capacity) {
    if (initial_capacity == 0)
        initial_capacity = XR_HTTP_BUFFER_DEFAULT_CAP;

    // Try TLS pool first
    if (tls_pool.count > 0) {
        XrHttpBuffer *buf = tls_pool.slots[--tls_pool.count];
        // If existing allocation is sufficient, reuse it
        if (buf->capacity >= initial_capacity) {
            http_buffer_reset(buf);
            return buf;
        }
        // Otherwise free the undersized allocation and reallocate
        http_buffer_free(buf);
        if (!http_buffer_init(buf, initial_capacity)) {
            xr_free(buf);
            return NULL;
        }
        return buf;
    }

    // Allocate new
    XrHttpBuffer *buf = (XrHttpBuffer *) xr_malloc(sizeof(XrHttpBuffer));
    if (!buf)
        return NULL;

    if (!http_buffer_init(buf, initial_capacity)) {
        xr_free(buf);
        return NULL;
    }
    return buf;
}

void http_buffer_release(XrHttpBuffer *buf) {
    if (!buf)
        return;

    // Return to TLS pool if room and not oversized
    if (tls_pool.count < XR_HTTP_BUFFER_MAX_RECYCLE &&
        buf->capacity <= XR_HTTP_BUFFER_RECYCLE_MAXCAP) {
        http_buffer_reset(buf);
        tls_pool.slots[tls_pool.count++] = buf;
        return;
    }

    // Pool full or oversized: free
    http_buffer_free(buf);
    xr_free(buf);
}

void http_buffer_pool_cleanup(void) {
    for (int i = 0; i < tls_pool.count; i++) {
        http_buffer_free(tls_pool.slots[i]);
        xr_free(tls_pool.slots[i]);
        tls_pool.slots[i] = NULL;
    }
    tls_pool.count = 0;
}
