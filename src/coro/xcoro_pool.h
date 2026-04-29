/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_pool.h - Coroutine structure pool
 *
 * KEY CONCEPT:
 *   Pre-allocate coroutine structs to avoid malloc on each creation.
 *   - Pre-allocated coroutine struct array, no malloc overhead
 *   - Free list manages released coroutine slots
 *   - Thread-safe access support
 */

#ifndef XCORO_POOL_H
#define XCORO_POOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "../os/os_thread.h"
#include "../base/xdefs.h"

// Forward declaration
struct XrCoroutine;

// ========== Configuration ==========

#define XR_CORO_POOL_INIT_SIZE 4096          // Initial pool size
#define XR_CORO_POOL_GROW_SIZE 4096          // Growth size per expansion
#define XR_CORO_POOL_MAX_SIZE (1024 * 1024)  // Max pool size

// VM stack and bytecode frames embedded in arena (zero malloc per coroutine)
#define XR_CORO_POOL_STACK_SLOTS 64  // Initial stack slots per coroutine
#define XR_CORO_POOL_FRAME_SLOTS 4   // Initial frame slots per coroutine

// gc_flags bit definitions (coroutine pool markers)
#define XR_CORO_GC_SLAB_STACK 0x0001      // VM stack and frames from slab (not malloc'd)
#define XR_CORO_GC_FROM_POOL 0x0002       // Struct allocated from pool block
#define XR_CORO_GC_RECYCLABLE 0x0004      // Fire-and-forget, eligible for deferred recycle
#define XR_CORO_GC_RECYCLED_CLEAN 0x0008  // Recycled with thorough field reset (skip memset)

// ========== Pool Block ==========

// XrCoroPoolBlock - Pool memory block
// Each block contains coroutine structs plus embedded VM stack and frame memory.
// Layout: [XrCoroutine array][VM stack and frame slab]
typedef struct XrCoroPoolBlock {
    struct XrCoroutine *coros;     // Coroutine array
    char *slab;                    // VM stack and frame slab memory
    size_t slab_entry_size;        // Size of each VM stack and frame entry
    size_t capacity;               // Number of coroutines
    uint32_t base_idx;             // Global alloc_idx base for this block
    struct XrCoroPoolBlock *next;  // Next block
} XrCoroPoolBlock;

// ========== Coroutine Structure Pool ==========

// XrCoroStructPool - Manages pre-allocated coroutine structs
//
// Allocation strategy:
//   1. Fast path: from pre-allocated array (lock-free atomic)
//   2. Recycle path: from free list (lock-free Treiber stack)
//   3. Expansion path: allocate new memory block (protected by grow_lock)
//
// free_list is a lock-free Treiber stack (no mutex). Link chains via
// coroutine->next (re-used; cleared immediately on pop by memset).
// ABA: a popped coroutine runs user code before being freed, so the
// re-push window is long enough that real
// ABA pressure requires sustained sub-μs churn — not observed so far.
typedef struct XrCoroStructPool {
    XrCoroPoolBlock *blocks;                  // Pre-allocated block list
    XrCoroPoolBlock *current_block;           // Current allocation block
    _Atomic uint32_t alloc_idx;               // Current block alloc index (lock-free fast path)
    _Atomic(struct XrCoroutine *) free_list;  // Lock-free recycled coroutines (Treiber stack)

    // Statistics (atomic: accessed from lock-free fast path by multiple threads)
    _Atomic uint64_t total_alloc;  // Total allocations
    _Atomic uint64_t fast_alloc;   // Fast path allocations
    _Atomic uint64_t free_alloc;   // Free list allocations
    _Atomic uint64_t total_free;   // Total frees

    xr_mutex_t grow_lock;  // Protects block-list growth only (low-frequency path)
    bool initialized;      // Initialized flag
} XrCoroStructPool;

// ========== Pool Lifecycle API ==========

// Initialize coroutine structure pool
// Pre-allocates initial coroutine array
// Returns true on success
XR_FUNC bool xr_coro_pool_init(XrCoroStructPool *pool, size_t init_size);

// Destroy coroutine structure pool
// Releases all pre-allocated memory
XR_FUNC void xr_coro_pool_destroy(XrCoroStructPool *pool);

// ========== Allocate/Free API ==========

// Allocate coroutine struct from pool
// Strategy: 1. Fast path from pre-alloc array (lock-free)
//           2. From free list (needs lock)
//           3. Expand pool
// Returns coroutine pointer, NULL on failure
XR_FUNC struct XrCoroutine *xr_coro_pool_alloc(XrCoroStructPool *pool);

// Free coroutine struct back to pool
// Adds to free list for later reuse
XR_FUNC void xr_coro_struct_pool_free(XrCoroStructPool *pool, struct XrCoroutine *coro);

// ========== Query API ==========

// Get pool statistics
XR_FUNC void xr_coro_pool_stats(XrCoroStructPool *pool, uint64_t *total_alloc, uint64_t *fast_alloc,
                                uint64_t *free_alloc, uint64_t *total_free);

// Print pool statistics
XR_FUNC void xr_coro_pool_print_stats(XrCoroStructPool *pool);

/* ========== Slab Init Helper ========== */

/*
 * Initialize coroutine vm_ctx fields from a pool block's slab.
 * Eliminates repeated slab setup code across xcoro_pool.c and xworker.c.
 */
#include "xcoroutine.h"
#include "xexec_frame.h"
static inline void xr_coro_init_from_slab(struct XrCoroutine *coro, XrCoroPoolBlock *block,
                                          uint32_t local_idx) {
    coro->vm_ctx.handlers = NULL;
    coro->vm_ctx.handler_capacity = 0;
    coro->coro_gc = NULL;
    coro->ext = NULL;
    coro->jit_suspend = NULL;
    if (block->slab) {
        char *entry = block->slab + local_idx * block->slab_entry_size;
        size_t stack_bytes = sizeof(XrValue) * XR_CORO_POOL_STACK_SLOTS;
        coro->vm_ctx.stack = (XrValue *) entry;
        coro->vm_ctx.stack_capacity = XR_CORO_POOL_STACK_SLOTS;
        coro->vm_ctx.frames = (XrBcCallFrame *) (entry + stack_bytes);
        coro->vm_ctx.frame_capacity = XR_CORO_POOL_FRAME_SLOTS;
        coro->gc_flags = XR_CORO_GC_SLAB_STACK | XR_CORO_GC_FROM_POOL;
    } else {
        coro->vm_ctx.stack = NULL;
        coro->vm_ctx.stack_capacity = 0;
        coro->vm_ctx.frames = NULL;
        coro->vm_ctx.frame_capacity = 0;
        coro->gc_flags = XR_CORO_GC_FROM_POOL;
    }
}

#endif  // XCORO_POOL_H
