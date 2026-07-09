/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_buffer.h - Internal HTTP buffer with consume/reserve pattern
 *
 * KEY CONCEPT:
 *   Replaces fixed size-class buffer pool (xbuffer_pool) with a
 *   self-growing buffer that supports efficient consume (front skip)
 *   and reserve (back expand) operations. Combined with thread-local
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
 *   - Thread-local recycle pool: zero lock contention
 *
 * This is a native HTTP data-plane helper used by http_client.c and
 * http_listen.c. Keep it out of net public headers; protocol semantics live
 * in stdlib/http/http.xr.
 */

#ifndef XR_STDLIB_HTTP_BUFFER_H
#define XR_STDLIB_HTTP_BUFFER_H

#include <stddef.h>
#include <stdbool.h>

/* ========== Default Sizes ========== */

#define XR_HTTP_BUFFER_DEFAULT_CAP 4096      // Initial capacity
#define XR_HTTP_BUFFER_MAX_RECYCLE 16        // Max buffers per thread-local recycle slot
#define XR_HTTP_BUFFER_RECYCLE_MAXCAP 65536  // Don't recycle buffers larger than this

/* ========== Self-Growing Buffer ========== */

typedef struct XrHttpBuffer {
    char *_base;      // Allocated memory base
    char *bytes;      // Data start (>= _base after consume)
    size_t size;      // Valid data length from bytes
    size_t capacity;  // Total allocated size from _base
} XrHttpBuffer;

/*
 * Initialize buffer with given initial capacity.
 * buf must be caller-allocated (stack or embedded struct).
 * Returns false on allocation failure.
 */
bool http_buffer_init(XrHttpBuffer *buf, size_t initial_capacity);

/*
 * Free buffer memory. Safe to call on zeroed or already-freed buffer.
 */
void http_buffer_free(XrHttpBuffer *buf);

/*
 * Ensure at least min_avail bytes of writable space after bytes+size.
 * May reallocate or compact. Uses exponential growth.
 * Returns pointer to writable area, or NULL on allocation failure.
 */
char *http_buffer_reserve(XrHttpBuffer *buf, size_t min_avail);

/*
 * Advance write cursor after filling n bytes into reserved area.
 * Caller must ensure n <= available space from last reserve().
 */
void http_buffer_advance(XrHttpBuffer *buf, size_t n);

/*
 * Consume n bytes from the front of the buffer.
 * Adjusts bytes pointer forward without memmove.
 * Auto-compacts when consumed portion exceeds half of capacity.
 */
void http_buffer_consume(XrHttpBuffer *buf, size_t n);

/*
 * Force compact: memmove remaining data to _base.
 */
void http_buffer_compact(XrHttpBuffer *buf);

/*
 * Reset buffer to empty state, keeping allocation.
 */
void http_buffer_reset(XrHttpBuffer *buf);

/*
 * Available writable space at the end of the buffer.
 */
static inline size_t http_buffer_available(const XrHttpBuffer *buf) {
    return buf->capacity - (size_t) (buf->bytes - buf->_base) - buf->size;
}

/*
 * Total consumed offset from _base.
 */
static inline size_t http_buffer_consumed(const XrHttpBuffer *buf) {
    return (size_t) (buf->bytes - buf->_base);
}

/* ========== Thread-Local Recycle Pool ========== */

/*
 * Acquire a buffer from the thread-local recycle pool, or allocate a new one.
 * Returns heap-allocated XrHttpBuffer*, or NULL on failure.
 */
XrHttpBuffer *http_buffer_acquire(size_t initial_capacity);

/*
 * Release buffer back to the thread-local pool (or free if full / oversized).
 * Safe to call with NULL.
 */
void http_buffer_release(XrHttpBuffer *buf);

/*
 * Cleanup recycle pool for current thread.
 * Call during thread shutdown to avoid leaks.
 */
void http_buffer_pool_cleanup(void);

#endif  // XR_STDLIB_HTTP_BUFFER_H
