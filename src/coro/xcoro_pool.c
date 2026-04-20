/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_pool.c - Coroutine structure pool implementation
 *
 * KEY CONCEPT:
 *   Pre-allocate coroutine structs to avoid malloc on each creation.
 */

#include "xcoro_pool.h"
#include "../base/xchecks.h"
#include "xcoroutine.h"
#include "xcoro_flags.h"
#include "xexec_frame.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../base/xmalloc.h"

// ========== Internal Functions ==========

// Create new pool block with embedded stack+frames slab
static XrCoroPoolBlock* xr_coro_pool_block_create(size_t capacity) {
    XR_DCHECK(capacity > 0, "coro_pool_block_create: zero capacity");
    XrCoroPoolBlock *block = xr_malloc(sizeof(XrCoroPoolBlock));
    if (!block) return NULL;

    block->coros = xr_malloc(capacity * sizeof(XrCoroutine));
    if (!block->coros) {
        xr_free(block);
        return NULL;
    }

    // Allocate slab for embedded stack+frames (one entry per coroutine)
    size_t stack_bytes = sizeof(XrValue) * XR_CORO_POOL_STACK_SLOTS;
    size_t frames_bytes = sizeof(XrBcCallFrame) * XR_CORO_POOL_FRAME_SLOTS;
    block->slab_entry_size = stack_bytes + frames_bytes;
    block->slab = xr_malloc(capacity * block->slab_entry_size);
    if (!block->slab) {
        xr_free(block->coros);
        xr_free(block);
        return NULL;
    }

    block->capacity = capacity;
    block->base_idx = 0;
    block->next = NULL;

    return block;
}

// Destroy pool block
static void xr_coro_pool_block_destroy(XrCoroPoolBlock *block) {
    if (!block) return;

    if (block->slab) {
        xr_free(block->slab);
    }
    if (block->coros) {
        xr_free(block->coros);
    }
    xr_free(block);
}

// Expand pool
static bool xr_coro_pool_grow(XrCoroStructPool *pool) {
    XrCoroPoolBlock *new_block = xr_coro_pool_block_create(XR_CORO_POOL_GROW_SIZE);
    if (!new_block) return false;

    // Add to block list head
    new_block->next = pool->blocks;
    pool->blocks = new_block;

    // Set base_idx to current alloc_idx so the new block's local indices
    // start from 0. alloc_idx is NEVER reset — it grows monotonically.
    // This prevents the TOCTOU race where a concurrent allocator reads
    // the old block pointer but gets a reset (small) alloc_idx, causing
    // duplicate allocation from the old block.
    new_block->base_idx = atomic_load(&pool->alloc_idx);

    // Set as current block (after base_idx is set)
    pool->current_block = new_block;

    return true;
}

// ========== Pool Lifecycle API ==========

bool xr_coro_pool_init(XrCoroStructPool *pool, size_t init_size) {
    if (!pool) return false;

    memset(pool, 0, sizeof(XrCoroStructPool));

    // Use default size
    if (init_size == 0) {
        init_size = XR_CORO_POOL_INIT_SIZE;
    }

    // Create initial block
    pool->blocks = xr_coro_pool_block_create(init_size);
    if (!pool->blocks) return false;

    pool->current_block = pool->blocks;
    atomic_store(&pool->alloc_idx, 0);

    // Initialize lock-free free list (Treiber stack).
    atomic_store(&pool->free_list, (XrCoroutine *)NULL);

    // Only grow_lock remains — protects block-list expansion (low-frequency).
    if (pthread_mutex_init(&pool->grow_lock, NULL) != 0) {
        xr_coro_pool_block_destroy(pool->blocks);
        pool->blocks = NULL;
        pool->current_block = NULL;
        return false;
    }

    // Initialize stats
    atomic_store_explicit(&pool->total_alloc, 0, memory_order_relaxed);
    atomic_store_explicit(&pool->fast_alloc, 0, memory_order_relaxed);
    atomic_store_explicit(&pool->free_alloc, 0, memory_order_relaxed);
    atomic_store_explicit(&pool->total_free, 0, memory_order_relaxed);

    pool->initialized = true;

    return true;
}

void xr_coro_pool_destroy(XrCoroStructPool *pool) {
    if (!pool || !pool->initialized) return;

    // Destroy all blocks
    XrCoroPoolBlock *block = pool->blocks;
    while (block) {
        XrCoroPoolBlock *next = block->next;
        xr_coro_pool_block_destroy(block);
        block = next;
    }

    // Destroy lock
    pthread_mutex_destroy(&pool->grow_lock);

    pool->initialized = false;
}

/* ========== Allocate/Free API ========== */

XrCoroutine* xr_coro_pool_alloc(XrCoroStructPool *pool) {
    if (!pool || !pool->initialized) return NULL;

    XrCoroutine *coro = NULL;

    // Strategy 1: fast path - allocate from current block (lock-free)
    XrCoroPoolBlock *block = pool->current_block;
    if (block) {
        uint32_t global_idx = atomic_fetch_add(&pool->alloc_idx, 1);
        uint32_t local_idx = global_idx - block->base_idx;
        if (local_idx < block->capacity) {
            coro = &block->coros[local_idx];
            atomic_fetch_add_explicit(&pool->fast_alloc, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&pool->total_alloc, 1, memory_order_relaxed);
            xr_coro_init_from_slab(coro, block, local_idx);
            return coro;
        }
    }

    // Strategy 2: lock-free pop from free list (Treiber stack).
    // Bounded retry: under extreme contention we may see persistent CAS
    // failures — fall through to expansion rather than spin forever.
    for (int retry = 0; retry < 8; retry++) {
        XrCoroutine *head = atomic_load_explicit(&pool->free_list, memory_order_acquire);
        if (!head) break;
        XrCoroutine *next = head->next;
        if (atomic_compare_exchange_weak_explicit(
                &pool->free_list, &head, next,
                memory_order_acq_rel, memory_order_acquire)) {
            coro = head;
            atomic_fetch_add_explicit(&pool->free_alloc, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&pool->total_alloc, 1, memory_order_relaxed);
            // Zero struct (next pointer may be dirty).
            memset(coro, 0, sizeof(XrCoroutine));
            return coro;
        }
    }

    // Strategy 3: expand pool
    pthread_mutex_lock(&pool->grow_lock);

    // Double check: other thread may have expanded
    block = pool->current_block;
    if (block) {
        uint32_t global_idx = atomic_fetch_add(&pool->alloc_idx, 1);
        uint32_t local_idx = global_idx - block->base_idx;
        if (local_idx < block->capacity) {
            pthread_mutex_unlock(&pool->grow_lock);

            coro = &block->coros[local_idx];
            atomic_fetch_add_explicit(&pool->fast_alloc, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&pool->total_alloc, 1, memory_order_relaxed);
            xr_coro_init_from_slab(coro, block, local_idx);
            return coro;
        }
    }

    // Actually need to expand
    if (!xr_coro_pool_grow(pool)) {
        pthread_mutex_unlock(&pool->grow_lock);

        // Expansion failed, fallback to malloc
        coro = xr_calloc(1, sizeof(XrCoroutine));
        if (coro) {
            atomic_fetch_add_explicit(&pool->total_alloc, 1, memory_order_relaxed);
        }
        return coro;
    }

    // Allocate from new block
    block = pool->current_block;
    uint32_t global_idx2 = atomic_fetch_add(&pool->alloc_idx, 1);
    pthread_mutex_unlock(&pool->grow_lock);

    uint32_t local_idx2 = global_idx2 - block->base_idx;
    if (local_idx2 < block->capacity) {
        coro = &block->coros[local_idx2];
        atomic_fetch_add_explicit(&pool->fast_alloc, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&pool->total_alloc, 1, memory_order_relaxed);
        xr_coro_init_from_slab(coro, block, local_idx2);
        return coro;
    }

    // Should not reach here
    return NULL;
}

void xr_coro_struct_pool_free(XrCoroStructPool *pool, XrCoroutine *coro) {
    if (!pool || !pool->initialized || !coro) return;

    // Check if from pool via gc_flags bit (O(1) instead of block list traversal)
    bool from_pool = (coro->gc_flags & XR_CORO_GC_FROM_POOL) != 0;

    if (from_pool) {
        // Lock-free Treiber push.
        XrCoroutine *head;
        do {
            head = atomic_load_explicit(&pool->free_list, memory_order_relaxed);
            coro->next = head;
        } while (!atomic_compare_exchange_weak_explicit(
            &pool->free_list, &head, coro,
            memory_order_release, memory_order_relaxed));
    } else {
        // From malloc: free directly
        xr_free(coro);
    }

    atomic_fetch_add_explicit(&pool->total_free, 1, memory_order_relaxed);
}

// ========== Query API ==========

void xr_coro_pool_stats(XrCoroStructPool *pool,
                        uint64_t *total_alloc,
                        uint64_t *fast_alloc,
                        uint64_t *free_alloc,
                        uint64_t *total_free) {
    if (!pool) return;

    if (total_alloc) *total_alloc = atomic_load_explicit(&pool->total_alloc, memory_order_relaxed);
    if (fast_alloc) *fast_alloc = atomic_load_explicit(&pool->fast_alloc, memory_order_relaxed);
    if (free_alloc) *free_alloc = atomic_load_explicit(&pool->free_alloc, memory_order_relaxed);
    if (total_free) *total_free = atomic_load_explicit(&pool->total_free, memory_order_relaxed);
}

void xr_coro_pool_print_stats(XrCoroStructPool *pool) {
    if (!pool) return;

    uint64_t total = atomic_load_explicit(&pool->total_alloc, memory_order_relaxed);
    uint64_t fast = atomic_load_explicit(&pool->fast_alloc, memory_order_relaxed);
    uint64_t from_free = atomic_load_explicit(&pool->free_alloc, memory_order_relaxed);
    uint64_t freed = atomic_load_explicit(&pool->total_free, memory_order_relaxed);

    printf("=== Coroutine Pool Stats ===\n");
    printf("  Total allocations: %llu\n", (unsigned long long)total);
    printf("  Fast path (no-lock): %llu (%.1f%%)\n",
           (unsigned long long)fast,
           total > 0 ? (double)fast * 100.0 / total : 0.0);
    printf("  From free list: %llu (%.1f%%)\n",
           (unsigned long long)from_free,
           total > 0 ? (double)from_free * 100.0 / total : 0.0);
    printf("  Total freed: %llu\n", (unsigned long long)freed);
    printf("  Currently in use: %llu\n", (unsigned long long)(total - freed));
}
