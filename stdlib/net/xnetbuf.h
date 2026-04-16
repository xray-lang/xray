/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnetbuf.h - Self-growing network buffer with consume/reserve pattern
 *
 * KEY CONCEPT:
 *   Replaces fixed size-class buffer pool (xbuffer_pool) with a
 *   self-growing buffer that supports efficient consume (front skip)
 *   and reserve (back expand) operations. Combined with TLS-level
 *   recycling for zero-lock buffer reuse.
 *
 *   Buffer layout:
 *     _base                bytes              bytes+size        _base+capacity
 *     |----consumed--------|----valid data----|----available----|
 *                          ^                  ^
 *                          read ptr           write ptr
 *
 * WHY THIS DESIGN:
 *   - Exponential growth avoids frequent realloc
 *   - consume() skips data without memmove (O(1))
 *   - Auto-compact when consumed > half capacity
 *   - TLS-level recycle pool: zero lock contention
 *
 * RELATED MODULES:
 *   - io.h: Network I/O layer uses this for read/write buffers
 *   - http_client.c: HTTP response receive buffer
 *   - ws.c: WebSocket message assembly buffer
 */

#ifndef XR_STDLIB_NETBUF_H
#define XR_STDLIB_NETBUF_H

#include <stddef.h>
#include <stdbool.h>

/* ========== Default Sizes ========== */

#define XR_NETBUF_DEFAULT_CAP    4096 // Initial capacity
#define XR_NETBUF_MAX_RECYCLE    16 // Max buffers per TLS recycle slot
#define XR_NETBUF_RECYCLE_MAXCAP 65536 // Don't recycle buffers larger than this

/* ========== Self-Growing Buffer ========== */

typedef struct XrNetBuffer {
    char  *_base; // Allocated memory base
    char  *bytes; // Data start (>= _base after consume)
    size_t size; // Valid data length from bytes
    size_t capacity; // Total allocated size from _base
} XrNetBuffer;

/*
 * Initialize buffer with given initial capacity.
 * buf must be caller-allocated (stack or embedded struct).
 * Returns false on allocation failure.
 */
bool xr_netbuf_init(XrNetBuffer *buf, size_t initial_capacity);

/*
 * Free buffer memory. Safe to call on zeroed or already-freed buffer.
 */
void xr_netbuf_free(XrNetBuffer *buf);

/*
 * Ensure at least min_avail bytes of writable space after bytes+size.
 * May reallocate or compact. Uses exponential growth.
 * Returns pointer to writable area, or NULL on allocation failure.
 */
char* xr_netbuf_reserve(XrNetBuffer *buf, size_t min_avail);

/*
 * Advance write cursor after filling n bytes into reserved area.
 * Caller must ensure n <= available space from last reserve().
 */
void xr_netbuf_advance(XrNetBuffer *buf, size_t n);

/*
 * Consume n bytes from the front of the buffer.
 * Adjusts bytes pointer forward without memmove.
 * Auto-compacts when consumed portion exceeds half of capacity.
 */
void xr_netbuf_consume(XrNetBuffer *buf, size_t n);

/*
 * Force compact: memmove remaining data to _base.
 */
void xr_netbuf_compact(XrNetBuffer *buf);

/*
 * Reset buffer to empty state, keeping allocation.
 */
void xr_netbuf_reset(XrNetBuffer *buf);

/*
 * Available writable space at the end of the buffer.
 */
static inline size_t xr_netbuf_available(const XrNetBuffer *buf) {
    return buf->capacity - (size_t)(buf->bytes - buf->_base) - buf->size;
}

/*
 * Total consumed offset from _base.
 */
static inline size_t xr_netbuf_consumed(const XrNetBuffer *buf) {
    return (size_t)(buf->bytes - buf->_base);
}

/* ========== TLS-Level Recycle Pool ========== */

/*
 * Acquire a buffer from TLS recycle pool, or allocate a new one.
 * Returns heap-allocated XrNetBuffer*, or NULL on failure.
 */
XrNetBuffer* xr_netbuf_acquire(size_t initial_capacity);

/*
 * Release buffer back to TLS recycle pool (or free if pool full / oversized).
 * Safe to call with NULL.
 */
void xr_netbuf_release(XrNetBuffer *buf);

/*
 * Cleanup TLS recycle pool for current thread.
 * Call during thread shutdown to avoid leaks.
 */
void xr_netbuf_pool_cleanup(void);

#endif // XR_STDLIB_NETBUF_H
