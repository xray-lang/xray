/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsystem_heap.h - System-level object management
 *
 * KEY CONCEPT:
 *   Manages objects that outlive individual coroutines.
 *   These objects are NOT subject to per-coroutine GC.
 *
 * VS xcoro_gc (Per-Coroutine GC):
 *   - xcoro_gc: Per-coroutine Arena + Mark-Sweep, freed when coro ends
 *   - xsystem_heap: Global, Arena-allocated (never freed) or ref-counted
 *
 * OBJECT TYPES:
 *   - XrCoroutine: Pooled for reuse (XrCoroStructPool)
 *   - XrClass/XrModule: Arena-allocated (never freed individually)
 *   - shared objects: Reference counted, freed when refcount=0
 *
 * ALLOCATION STRATEGIES:
 *   - Coroutines: Object pool (fixed-size, fast alloc/free)
 *   - Classes/Modules: Arena (bulk free on shutdown)
 *   - shared: malloc + atomic refcount
 *
 * RELATED MODULES:
 *   - xcoro_pool.h: Coroutine structure pooling
 *   - xarena.h: Arena allocator used internally
 *   - xshared.h: Reference counting operations
 */

#ifndef XSYSTEM_HEAP_H
#define XSYSTEM_HEAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "../../base/xthread.h"
#include "../../base/xarena.h"

// Forward declarations
struct XrCoroutine;
struct XrClass;
struct XrModule;
struct XrCoroStructPool;
struct XrCoroGC;

/* ========== Configuration ========== */

typedef struct XrSysHeapConfig {
    size_t coro_pool_init_size;    // Initial coroutine pool size (default 1024)
    size_t class_arena_init_size;  // Initial class arena size (default 64KB)
    bool enable_stats;             // Enable statistics tracking
} XrSysHeapConfig;

// Defaults
#define XR_SYSHEAP_DEFAULT_CORO_POOL_SIZE 1024
#define XR_SYSHEAP_DEFAULT_CLASS_ARENA_SIZE (64 * 1024)

/* ========== Statistics ========== */

typedef struct XrSysHeapStats {
    // Coroutine pool stats
    uint64_t coro_alloc_count;
    uint64_t coro_free_count;
    uint64_t coro_reuse_count;

    // Arena stats
    _Atomic uint64_t class_alloc_count;
    _Atomic uint64_t class_alloc_bytes;
    _Atomic uint64_t module_alloc_count;

    // Shared object stats
    _Atomic uint64_t shared_alloc_count;
    _Atomic uint64_t shared_mmap_count;  // Large objects allocated via mmap

    // Channel stats (for leak detection)
    _Atomic uint64_t channel_create_count;
    _Atomic uint64_t channel_close_count;
} XrSysHeapStats;

/* ========== System Heap Manager ========== */

typedef struct XrSystemHeap {
    struct XrCoroStructPool *coro_pool;  // Coroutine object pool
    XrArena class_arena;                 // Class/module arena
    XrSysHeapStats stats;

    /* Per-isolate L2 pool for recycled XrCoroGC structs. The L1 cache
     * lives on each worker (XrProc.gc_free_list); on overflow or worker
     * teardown structs are pushed here, and L1 misses pop from here
     * before falling back to xr_malloc. Lock guards the linked stack
     * using the first sizeof(void*) bytes of each free struct. */
    xr_mutex_t gc_pool_mu;
    struct XrCoroGC *gc_pool_head;
    int gc_pool_count;

    bool initialized;
} XrSystemHeap;

/* ========== Lifecycle ========== */

// Initialize system heap (config=NULL for defaults)
XR_FUNC bool xr_sysheap_init(XrSystemHeap *heap, const XrSysHeapConfig *config);

// Destroy system heap and release all memory
XR_FUNC void xr_sysheap_destroy(XrSystemHeap *heap);

/* ========== Coroutine Allocation ========== */

// Allocate coroutine from pool (reuses freed coroutines)
XR_FUNC struct XrCoroutine *xr_sysheap_alloc_coro(XrSystemHeap *heap);

// Return coroutine to pool for reuse
XR_FUNC void xr_sysheap_free_coro(XrSystemHeap *heap, struct XrCoroutine *coro);

/* ========== Class/Module Allocation ========== */

// Allocate class from arena (freed when isolate destroyed)
XR_FUNC void *xr_sysheap_alloc_class(XrSystemHeap *heap, size_t size);

// Allocate module from arena
XR_FUNC void *xr_sysheap_alloc_module(XrSystemHeap *heap, size_t size);

/* ========== Shared Object Allocation ========== */

// Allocate shared object with atomic refcount (freed when refcount=0)
// Large objects (>=64KB) use mmap, small ones use malloc
XR_FUNC void *xr_sysheap_alloc_shared(XrSystemHeap *heap, size_t size, uint8_t type);

// Free shared object (handles both malloc and mmap)
XR_FUNC void xr_sysheap_free_shared(void *ptr, size_t size);

/* ========== XrCoroGC Struct Pool (L2) ==========
 *
 * Recycled XrCoroGC structs land here when the per-worker L1 cache is
 * full or a worker is destroyed. Pool capacity is bounded; structs that
 * don't fit are returned to malloc/free. */

#define XR_SYSHEAP_GC_POOL_MAX 256

XR_FUNC struct XrCoroGC *xr_sysheap_gc_pool_pop(XrSystemHeap *heap);
XR_FUNC bool xr_sysheap_gc_pool_push(XrSystemHeap *heap, struct XrCoroGC *gc);

// XR_GC_FLAG_MMAP now defined in xgc_header.h (extra bit 13, shared by both
// system heap and per-coro GC large objects).

/* ========== Statistics ========== */

XR_FUNC void xr_sysheap_get_stats(XrSystemHeap *heap, XrSysHeapStats *stats);
XR_FUNC void xr_sysheap_print_stats(XrSystemHeap *heap);

#endif  // XSYSTEM_HEAP_H
