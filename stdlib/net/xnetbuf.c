/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnetbuf.c - Self-growing network buffer implementation
 *
 * KEY CONCEPT:
 *   Exponential-growth buffer with consume/reserve pattern.
 *   TLS-level recycle pool eliminates lock contention.
 */

#include "xnetbuf.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ========== Buffer Core ========== */

bool xr_netbuf_init(XrNetBuffer *buf, size_t initial_capacity) {
    assert(buf);
    if (initial_capacity == 0) initial_capacity = XR_NETBUF_DEFAULT_CAP;

    char *mem = (char *)malloc(initial_capacity);
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

void xr_netbuf_free(XrNetBuffer *buf) {
    if (!buf) return;
    free(buf->_base);
    buf->_base = NULL;
    buf->bytes = NULL;
    buf->size = 0;
    buf->capacity = 0;
}

char* xr_netbuf_reserve(XrNetBuffer *buf, size_t min_avail) {
    assert(buf && buf->_base);

    size_t consumed = xr_netbuf_consumed(buf);
    size_t tail_avail = buf->capacity - consumed - buf->size;

    if (tail_avail >= min_avail) {
        return buf->bytes + buf->size;
    }

    // Try compact first: if consumed > half capacity, memmove is worthwhile
    if (consumed > 0 && consumed >= buf->capacity / 2) {
        xr_netbuf_compact(buf);
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

    char *new_base = (char *)malloc(new_cap);
    if (!new_base) return NULL;

    if (buf->size > 0) {
        memcpy(new_base, buf->bytes, buf->size);
    }

    free(buf->_base);
    buf->_base = new_base;
    buf->bytes = new_base;
    buf->capacity = new_cap;

    return buf->bytes + buf->size;
}

void xr_netbuf_advance(XrNetBuffer *buf, size_t n) {
    assert(buf);
    buf->size += n;
}

void xr_netbuf_consume(XrNetBuffer *buf, size_t n) {
    assert(buf);
    assert(n <= buf->size);

    buf->bytes += n;
    buf->size -= n;

    // Auto-compact when consumed portion exceeds half of capacity
    if (buf->size == 0) {
        // Empty: just reset pointers
        buf->bytes = buf->_base;
    } else if (xr_netbuf_consumed(buf) > buf->capacity / 2) {
        xr_netbuf_compact(buf);
    }
}

void xr_netbuf_compact(XrNetBuffer *buf) {
    assert(buf);
    size_t consumed = xr_netbuf_consumed(buf);
    if (consumed == 0) return;

    if (buf->size > 0) {
        memmove(buf->_base, buf->bytes, buf->size);
    }
    buf->bytes = buf->_base;
}

void xr_netbuf_reset(XrNetBuffer *buf) {
    assert(buf);
    buf->bytes = buf->_base;
    buf->size = 0;
}

/* ========== TLS-Level Recycle Pool ========== */

/*
 * Simple per-thread free list of XrNetBuffer structs.
 * No mutex, no atomic — pure TLS.
 */
typedef struct {
    XrNetBuffer *slots[XR_NETBUF_MAX_RECYCLE];
    int count;
} XrNetBufPool;

static _Thread_local XrNetBufPool tls_pool = { .count = 0 };

XrNetBuffer* xr_netbuf_acquire(size_t initial_capacity) {
    if (initial_capacity == 0) initial_capacity = XR_NETBUF_DEFAULT_CAP;

    // Try TLS pool first
    if (tls_pool.count > 0) {
        XrNetBuffer *buf = tls_pool.slots[--tls_pool.count];
        // If existing allocation is sufficient, reuse it
        if (buf->capacity >= initial_capacity) {
            xr_netbuf_reset(buf);
            return buf;
        }
        // Otherwise free the undersized allocation and reallocate
        xr_netbuf_free(buf);
        if (!xr_netbuf_init(buf, initial_capacity)) {
            free(buf);
            return NULL;
        }
        return buf;
    }

    // Allocate new
    XrNetBuffer *buf = (XrNetBuffer *)malloc(sizeof(XrNetBuffer));
    if (!buf) return NULL;

    if (!xr_netbuf_init(buf, initial_capacity)) {
        free(buf);
        return NULL;
    }
    return buf;
}

void xr_netbuf_release(XrNetBuffer *buf) {
    if (!buf) return;

    // Return to TLS pool if room and not oversized
    if (tls_pool.count < XR_NETBUF_MAX_RECYCLE &&
        buf->capacity <= XR_NETBUF_RECYCLE_MAXCAP) {
        xr_netbuf_reset(buf);
        tls_pool.slots[tls_pool.count++] = buf;
        return;
    }

    // Pool full or oversized: free
    xr_netbuf_free(buf);
    free(buf);
}

void xr_netbuf_pool_cleanup(void) {
    for (int i = 0; i < tls_pool.count; i++) {
        xr_netbuf_free(tls_pool.slots[i]);
        free(tls_pool.slots[i]);
        tls_pool.slots[i] = NULL;
    }
    tls_pool.count = 0;
}
