/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmalloc.h - Memory allocator abstraction (system malloc or mimalloc)
 *
 * KEY CONCEPT:
 *   Unified malloc interface with optional mimalloc backend.
 *   In debug mode, tracks alloc/free counts for symmetry checking.
 */

#ifndef XMALLOC_H
#define XMALLOC_H

#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "xdefs.h"

/* ========== Backend Selection (raw allocators) ========== */

#ifdef XR_USE_MIMALLOC
    #include <mimalloc.h>
    #define xr_malloc_raw(size)          mi_malloc(size)
    #define xr_calloc_raw(count, size)   mi_calloc(count, size)
    #define xr_realloc_raw(ptr, size)    mi_realloc(ptr, size)
    #define xr_free_raw(ptr)             mi_free(ptr)
    #define xr_malloc_aligned(size, align)  mi_malloc_aligned(size, align)
    #define xr_free_aligned(ptr, align)     mi_free_aligned(ptr, align)
    #define XR_ALLOCATOR_NAME        "mimalloc"
#else
    #include <stdlib.h>
    #define xr_malloc_raw(size)          malloc(size)
    #define xr_calloc_raw(count, size)   calloc(count, size)
    #define xr_realloc_raw(ptr, size)    realloc(ptr, size)
    #define xr_free_raw(ptr)             free(ptr)
    static inline void* xr_malloc_aligned(size_t size, size_t align) {
        void *ptr = NULL;
        if (posix_memalign(&ptr, align, size) != 0) return NULL;
        return ptr;
    }
    #define xr_free_aligned(ptr, align)  free(ptr)
    #define XR_ALLOCATOR_NAME        "system malloc"
#endif

static inline const char* xr_mem_get_allocator_name(void) {
    return XR_ALLOCATOR_NAME;
}

/* ========== Debug Allocation Tracking ========== */

#if XR_DEBUG
#include <stdatomic.h>

typedef struct {
    _Atomic int64_t alloc_count;
    _Atomic int64_t free_count;
    _Atomic int64_t alloc_bytes;
} XrMemStats;

static inline XrMemStats* xr_mem_stats(void) {
    static XrMemStats stats = {0};
    return &stats;
}

static inline void xr_mem_track_alloc(size_t size) {
    atomic_fetch_add_explicit(&xr_mem_stats()->alloc_count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&xr_mem_stats()->alloc_bytes, (int64_t)size, memory_order_relaxed);
}

static inline void xr_mem_track_free(void) {
    atomic_fetch_add_explicit(&xr_mem_stats()->free_count, 1, memory_order_relaxed);
}

// Check allocation symmetry: alloc_count should equal free_count at shutdown.
// delta > 0 means leaks, delta < 0 means double-frees.
static inline int64_t xr_mem_check_balance(void) {
    int64_t a = atomic_load(&xr_mem_stats()->alloc_count);
    int64_t f = atomic_load(&xr_mem_stats()->free_count);
    return a - f;
}

static inline void xr_mem_dump_stats(void) {
    XrMemStats *s = xr_mem_stats();
    fprintf(stderr, "[MEM] alloc=%lld free=%lld delta=%lld bytes=%lld\n",
            (long long)atomic_load(&s->alloc_count),
            (long long)atomic_load(&s->free_count),
            (long long)xr_mem_check_balance(),
            (long long)atomic_load(&s->alloc_bytes));
}

// Tracked allocation wrappers (debug only)

static inline void* xr_malloc_tracked(size_t size) {
    void *p = xr_malloc_raw(size);
    if (p) xr_mem_track_alloc(size);
    return p;
}

static inline void* xr_calloc_tracked(size_t count, size_t size) {
    void *p = xr_calloc_raw(count, size);
    if (p) xr_mem_track_alloc(count * size);
    return p;
}

static inline void* xr_realloc_tracked(void *ptr, size_t size) {
    void *p = xr_realloc_raw(ptr, size);
    if (p && !ptr) xr_mem_track_alloc(size);
    return p;
}

static inline void xr_free_tracked(void *ptr) {
    if (ptr) { xr_mem_track_free(); xr_free_raw(ptr); }
}

#define xr_malloc(size)          xr_malloc_tracked(size)
#define xr_calloc(count, size)   xr_calloc_tracked(count, size)
#define xr_realloc(ptr, size)    xr_realloc_tracked(ptr, size)
#define xr_free(ptr)             xr_free_tracked(ptr)

#else // !XR_DEBUG

static inline void xr_mem_track_alloc(size_t size) { (void)size; }
static inline void xr_mem_track_free(void) {}
static inline int64_t xr_mem_check_balance(void) { return 0; }
static inline void xr_mem_dump_stats(void) {}

#define xr_malloc(size)          xr_malloc_raw(size)
#define xr_calloc(count, size)   xr_calloc_raw(count, size)
#define xr_realloc(ptr, size)    xr_realloc_raw(ptr, size)
#define xr_free(ptr)             xr_free_raw(ptr)

#endif // XR_DEBUG

/* ========== Convenience Macros ========== */

#define XR_GROW_CAPACITY(capacity) \
    ((capacity) < 8 ? 8 : (capacity) * 2)

#define XR_GROW_ARRAY(type, pointer, old_count, new_count) \
    (type*)xr_realloc(pointer, sizeof(type) * (new_count))

#define XR_FREE_ARRAY(type, pointer) \
    xr_free(pointer)

#define XR_ALLOCATE(type) \
    ((type*)xr_malloc(sizeof(type)))

#define XR_ALLOCATE_FLEX(type, array_type, count) \
    ((type*)xr_malloc(sizeof(type) + sizeof(array_type) * (count)))

static inline char* xr_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = (char*)xr_malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

// Function pointer wrapper (macros don't have addresses)
static inline void* xr_malloc_fn(size_t size) { return xr_malloc(size); }

#endif // XMALLOC_H
