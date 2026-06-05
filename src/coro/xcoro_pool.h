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
 *   Pool scheduler-owned coroutine shells without assuming a VM backend.
 *   - Pre-allocated coroutine shell array
 *   - VM state and VM stacks are attached lazily by the VM backend
 *   - Free list manages released coroutine shells
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

// Initial VM stack and bytecode frame sizes used by lazy VM backend allocation.
#define XR_CORO_POOL_STACK_SLOTS 64  // Initial stack slots per coroutine
#define XR_CORO_POOL_FRAME_SLOTS 4   // Initial frame slots per coroutine

// gc_flags bit definitions (coroutine pool markers)
#define XR_CORO_GC_SLAB_STACK 0x0001      // VM stack is embedded in a pool block
#define XR_CORO_GC_FROM_POOL 0x0002       // Struct allocated from pool block
#define XR_CORO_GC_RECYCLABLE 0x0004      // Fire-and-forget, eligible for deferred recycle
#define XR_CORO_GC_RECYCLED_CLEAN 0x0008  // Recycled with thorough field reset (skip memset)
#define XR_CORO_GC_VM_STATE_OWNED 0x0010  // VM state allocated separately from pool block
#define XR_CORO_GC_LIGHTWEIGHT 0x0020     // Struct allocated without VM backend state/slab

// ========== Pool Block ==========

// XrCoroPoolBlock - Pool memory block
// Each block contains coroutine shells only. Backend state is attached lazily so
// AOT/native coroutine shells do not inherit VM stack costs.
typedef struct XrCoroPoolBlock {
    struct XrCoroutine *coros;     // Coroutine array
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

/* ========== Pool Slot Init Helper ========== */

/*
 * Initialize a fresh coroutine shell from a pool block slot.
 * Backend state remains NULL until a backend-specific creator attaches it.
 */
#include "xcoroutine.h"
static inline void xr_coro_init_from_pool_slot(struct XrCoroutine *coro, XrCoroPoolBlock *block,
                                               uint32_t local_idx) {
    (void) block;
    (void) local_idx;
    XR_DCHECK(coro != NULL, "coro_init_from_pool_slot: NULL coro");
    coro->backend = NULL;
    coro->backend_state = NULL;
    coro->coro_gc = NULL;
    coro->ext = NULL;
    coro->gc_flags = XR_CORO_GC_FROM_POOL;
}

#endif  // XCORO_POOL_H
