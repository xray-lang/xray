/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xworker_pool.c - Per-worker coroutine object pool cache
 *
 * KEY CONCEPT:
 *   Each worker maintains a local free list of recycled coroutines
 *   (fast path, lock-free) and an arena-reservation cache (batch
 *   allocation from the global XrCoroStructPool to avoid per-shell
 *   atomic fetch_add on the pool alloc index).
 *
 * BUFFER HIERARCHY:
 *   1. Deferred recycle list (this coro and its siblings just finished)
 *   2. Local free list (fast reuse, lock-free)
 *   3. Global free list (lock-free batch steal)
 *   4. Local shell arena cache (slot range reserved from pool)
 *   5. Global shell arena (atomic fetch_add on pool->alloc_idx)
 *
 * Tuning constants (XR_CORO_BATCH_SIZE / XR_ARENA_BATCH_SIZE /
 * XR_CORO_LOCAL_FREE_MAX) live in xcoro_tuning.h and xworker.h.
 */
#include "xworker_internal.h"
#include "../base/xchecks.h"

// Get coroutine object from pool (per-Worker + batch steal)
XrCoroutine *xr_coro_pool_get(XrRuntime *runtime) {
    if (!runtime)
        return NULL;

    XrWorker *worker = xr_current_worker();

    // Deferred recycle: flush pending linked list (await fast path + fire-and-forget).
    // Must run BEFORE free list check to prevent unbounded accumulation.
    if (worker && worker->p.pending_recycle_coro) {
        XrCoroutine *_pend = worker->p.pending_recycle_coro;
        worker->p.pending_recycle_coro = NULL;
        while (_pend) {
            XrCoroutine *_next = _pend->next;
            _pend->next = NULL;
            xr_coro_recycle_local(worker, _pend);
            worker->p.stats.pool_deferred_recycle_count++;
            _pend = _next;
        }
    }

    // Fast path: get from local free list (lock-free)
    if (worker && worker->p.local_free_list) {
        XrCoroutine *coro = worker->p.local_free_list;
        worker->p.local_free_list = coro->next;
        worker->p.local_free_count--;
        coro->next = NULL;
        worker->p.stats.pool_local_get_count++;
        return coro;
    }

    // Local empty: bounded batch pop from the global free list.
    //
    // The global list can hold a full storm of recycled coroutines. Taking the
    // whole chain and pushing the remainder back would require finding the
    // remainder tail for every batch, which turns large reuse waves into O(n^2).
    // Pop only the batch we need from the Treiber stack.
    if (worker) {
        XrCoroStructPool *pool = xr_runtime_get_coro_pool(runtime);
        if (pool && pool->initialized) {
            int batch = 0;
            while (batch < XR_CORO_BATCH_SIZE) {
                XrCoroutine *head = atomic_load_explicit(&pool->free_list, memory_order_acquire);
                if (!head)
                    break;
                XrCoroutine *next = head->next;
                if (!atomic_compare_exchange_weak_explicit(&pool->free_list, &head, next,
                                                           memory_order_acq_rel,
                                                           memory_order_acquire)) {
                    continue;
                }
                head->next = worker->p.local_free_list;
                worker->p.local_free_list = head;
                worker->p.local_free_count++;
                batch++;
            }

            if (worker->p.local_free_list) {
                XrCoroutine *coro = worker->p.local_free_list;
                worker->p.local_free_list = coro->next;
                worker->p.local_free_count--;
                coro->next = NULL;
                worker->p.stats.pool_global_free_get_count++;
                return coro;
            }
        }
    }

    // Per-Worker batch arena allocation (avoids per-coro atomic_fetch_add on alloc_idx)
    if (worker) {
        XrCoroStructPool *pool = xr_runtime_get_coro_pool(runtime);
        if (pool && pool->initialized) {
            // Check local arena cache first (use cached block pointer)
            XrCoroPoolBlock *cached_block = (XrCoroPoolBlock *) worker->p.arena_cache_block;
            if (cached_block && worker->p.arena_cache_start < worker->p.arena_cache_end) {
                uint32_t idx = worker->p.arena_cache_start++;
                XrCoroutine *coro = &cached_block->coros[idx];
                coro->hdr = (XrObjHeader) {.type = XR_TCOROUTINE};
                xr_coro_init_from_pool_slot(coro, cached_block, idx);
                worker->p.stats.pool_arena_cache_get_count++;
                return coro;
            }

            // Claim a batch of arena slots (single atomic for N coroutines)
            XrCoroPoolBlock *block = pool->current_block;
            if (block) {
                uint32_t global_base = atomic_fetch_add(&pool->alloc_idx, XR_ARENA_BATCH_SIZE);
                uint32_t local_base = global_base - block->base_idx;
                uint32_t local_end = local_base + XR_ARENA_BATCH_SIZE;
                if (local_end > block->capacity)
                    local_end = block->capacity;
                if (local_base < block->capacity) {
                    // Cache the block and LOCAL range for future allocations
                    worker->p.arena_cache_block = block;
                    worker->p.arena_cache_start = local_base + 1;
                    worker->p.arena_cache_end = local_end;

                    XrCoroutine *coro = &block->coros[local_base];
                    coro->hdr = (XrObjHeader) {.type = XR_TCOROUTINE};
                    xr_coro_init_from_pool_slot(coro, block, local_base);
                    worker->p.stats.pool_arena_batch_get_count++;
                    return coro;
                }
                // Arena exhausted, invalidate cache
                worker->p.arena_cache_block = NULL;
                worker->p.arena_cache_start = 0;
                worker->p.arena_cache_end = 0;
            }
        }
    }

    // No available object, return NULL (caller allocates from global pool)
    if (worker)
        worker->p.stats.pool_miss_count++;
    return NULL;
}

// Return coroutine object to pool (per-Worker + batch return)
void xr_coro_pool_put(XrRuntime *runtime, XrCoroutine *coro) {
    if (!runtime || !coro)
        return;

    if (!xr_coro_backend_reset_reusable(coro)) {
        xr_coro_destroy(coro);
        return;
    }

    // Reset coroutine state
    coro->result = xr_null();
    coro->error = xr_null();
    atomic_store(&coro->flags, 0);

    XrWorker *worker = xr_current_worker();
    if (!worker) {
        // No worker context: return directly to global free list (lock-free).
        {
            XrCoroStructPool *pool = xr_runtime_get_coro_pool(runtime);
            if (pool && pool->initialized) {
                XrCoroutine *head;
                do {
                    head = atomic_load_explicit(&pool->free_list, memory_order_relaxed);
                    coro->next = head;
                } while (!atomic_compare_exchange_weak_explicit(
                    &pool->free_list, &head, coro, memory_order_release, memory_order_relaxed));
            }
        }
        return;
    }

    // Local not full: put directly (lock-free)
    if (worker->p.local_free_count < XR_CORO_LOCAL_FREE_MAX) {
        coro->next = worker->p.local_free_list;
        worker->p.local_free_list = coro;
        worker->p.local_free_count++;
        worker->p.stats.pool_local_put_count++;
        return;
    }

    // Local full: batch return half to global free list, then put locally.
    //
    // Lock-free splice. Detach the first `batch` nodes from
    // worker local list as a sub-chain (head=batch_head, tail=batch_tail),
    // then CAS-splice onto global free_list in a single step.
    {
        XrCoroStructPool *pool = xr_runtime_get_coro_pool(runtime);
        if (pool && pool->initialized) {
            int batch = worker->p.local_free_count / 2;
            XrCoroutine *batch_head = NULL;
            XrCoroutine *batch_tail = NULL;
            for (int i = 0; i < batch; i++) {
                XrCoroutine *c = worker->p.local_free_list;
                if (!c)
                    break;
                worker->p.local_free_list = c->next;
                worker->p.local_free_count--;
                c->next = batch_head;
                batch_head = c;
                if (!batch_tail)
                    batch_tail = c;
            }
            if (batch_head) {
                XrCoroutine *head;
                do {
                    head = atomic_load_explicit(&pool->free_list, memory_order_relaxed);
                    batch_tail->next = head;
                } while (!atomic_compare_exchange_weak_explicit(&pool->free_list, &head, batch_head,
                                                                memory_order_release,
                                                                memory_order_relaxed));
                worker->p.stats.pool_global_return_count += (uint64_t) batch;
            }
        }
    }

    // Now put current coro to local
    coro->next = worker->p.local_free_list;
    worker->p.local_free_list = coro;
    worker->p.local_free_count++;
    worker->p.stats.pool_local_put_count++;
}
