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
 *   allocation from the global XrCoroStructPool to avoid per-coro
 *   atomic fetch_add on the pool alloc index).
 *
 * BUFFER HIERARCHY:
 *   1. Deferred recycle list (this coro and its siblings just finished)
 *   2. Local free list (fast reuse, lock-free)
 *   3. Global free list (batch steal under pool->free_lock)
 *   4. Local arena cache (slab range reserved from pool)
 *   5. Global arena (atomic fetch_add on pool->alloc_idx)
 *
 * Tuning constants (XR_CORO_BATCH_SIZE / XR_ARENA_BATCH_SIZE /
 * XR_CORO_LOCAL_FREE_MAX) live in xcoro_tuning.h and xworker.h.
 */
#include "xworker_internal.h"
#include "../base/xchecks.h"

// Get coroutine object from pool (per-Worker + batch steal)
XrCoroutine *xr_coro_pool_get(XrRuntime *runtime) {
    if (!runtime) return NULL;

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
            _pend = _next;
        }
    }

    // Fast path: get from local free list (lock-free)
    if (worker && worker->p.local_free_list) {
        XrCoroutine *coro = worker->p.local_free_list;
        worker->p.local_free_list = coro->next;
        worker->p.local_free_count--;
        coro->next = NULL;
        return coro;
    }

    // Local empty: batch steal from global free list.
    //
    // Lock-free — atomic_exchange grabs the entire global list
    // in O(1), we keep the first XR_CORO_BATCH_SIZE for local use and push
    // the remainder back with a single CAS splice. This replaces the old
    // pthread_mutex_t pool->free_lock.
    if (worker && runtime->isolate && runtime->isolate->sys_heap) {
        XrCoroStructPool *pool = runtime->isolate->sys_heap->coro_pool;
        if (pool && pool->initialized) {
            // Steal entire chain.
            XrCoroutine *chain = atomic_exchange_explicit(
                &pool->free_list, (XrCoroutine *)NULL, memory_order_acquire);

            int batch = 0;
            XrCoroutine *tail_prev = NULL;  // last node we keep
            XrCoroutine *c = chain;
            while (c && batch < XR_CORO_BATCH_SIZE) {
                tail_prev = c;
                c = c->next;
                batch++;
            }

            // Splice remainder (c..end) back onto the global free list.
            if (c && tail_prev) {
                XrCoroutine *remainder = c;
                tail_prev->next = NULL;
                // Find remainder tail.
                XrCoroutine *rtail = remainder;
                while (rtail->next) rtail = rtail->next;
                // Treiber push the whole sub-chain: rtail->next = head; CAS.
                XrCoroutine *head;
                do {
                    head = atomic_load_explicit(&pool->free_list, memory_order_relaxed);
                    rtail->next = head;
                } while (!atomic_compare_exchange_weak_explicit(
                    &pool->free_list, &head, remainder,
                    memory_order_release, memory_order_relaxed));
            }

            // Adopt the kept chain as the worker's local free list (prepend).
            XrCoroutine *cur = chain;
            while (cur && batch > 0) {
                XrCoroutine *nxt = cur->next;
                cur->next = worker->p.local_free_list;
                worker->p.local_free_list = cur;
                worker->p.local_free_count++;
                cur = nxt;
                batch--;
            }

            if (worker->p.local_free_list) {
                XrCoroutine *coro = worker->p.local_free_list;
                worker->p.local_free_list = coro->next;
                worker->p.local_free_count--;
                coro->next = NULL;
                return coro;
            }
        }
    }

    // Per-Worker batch arena allocation (avoids per-coro atomic_fetch_add on alloc_idx)
    if (worker && runtime->isolate && runtime->isolate->sys_heap) {
        XrCoroStructPool *pool = runtime->isolate->sys_heap->coro_pool;
        if (pool && pool->initialized) {
            // Check local arena cache first (use cached block pointer)
            XrCoroPoolBlock *cached_block = (XrCoroPoolBlock *)worker->p.arena_cache_block;
            if (cached_block && worker->p.arena_cache_start < worker->p.arena_cache_end) {
                uint32_t idx = worker->p.arena_cache_start++;
                XrCoroutine *coro = &cached_block->coros[idx];
                coro->gc = (XrGCHeader){.type = XR_TCOROUTINE};
                xr_coro_init_from_slab(coro, cached_block, idx);
                return coro;
            }

            // Claim a batch of arena slots (single atomic for N coroutines)
            XrCoroPoolBlock *block = pool->current_block;
            if (block) {
                uint32_t global_base = atomic_fetch_add(&pool->alloc_idx, XR_ARENA_BATCH_SIZE);
                uint32_t local_base = global_base - block->base_idx;
                uint32_t local_end = local_base + XR_ARENA_BATCH_SIZE;
                if (local_end > block->capacity) local_end = block->capacity;
                if (local_base < block->capacity) {
                    // Cache the block and LOCAL range for future allocations
                    worker->p.arena_cache_block = block;
                    worker->p.arena_cache_start = local_base + 1;
                    worker->p.arena_cache_end = local_end;

                    XrCoroutine *coro = &block->coros[local_base];
                    coro->gc = (XrGCHeader){.type = XR_TCOROUTINE};
                    xr_coro_init_from_slab(coro, block, local_base);
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
    return NULL;
}

// Return coroutine object to pool (per-Worker + batch return)
void xr_coro_pool_put(XrRuntime *runtime, XrCoroutine *coro) {
    if (!runtime || !coro) return;

    // Reset coroutine state
    coro->entry_type = XR_CORO_ENTRY_CLOSURE;
    coro->entry.closure = NULL;
    coro->args = NULL;
    coro->arg_count = 0;
    coro->result = xr_null();
    coro->error = xr_null();
    coro->await_results = NULL;
    atomic_store(&coro->flags, 0);
    atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_NONE, memory_order_relaxed);

    XrWorker *worker = xr_current_worker();
    if (!worker) {
        // No worker context: return directly to global free list (lock-free).
        if (runtime->isolate && runtime->isolate->sys_heap) {
            XrCoroStructPool *pool = runtime->isolate->sys_heap->coro_pool;
            if (pool && pool->initialized) {
                XrCoroutine *head;
                do {
                    head = atomic_load_explicit(&pool->free_list, memory_order_relaxed);
                    coro->next = head;
                } while (!atomic_compare_exchange_weak_explicit(
                    &pool->free_list, &head, coro,
                    memory_order_release, memory_order_relaxed));
            }
        }
        return;
    }

    // Local not full: put directly (lock-free)
    if (worker->p.local_free_count < XR_CORO_LOCAL_FREE_MAX) {
        coro->next = worker->p.local_free_list;
        worker->p.local_free_list = coro;
        worker->p.local_free_count++;
        return;
    }

    // Local full: batch return half to global free list, then put locally.
    //
    // Lock-free splice. Detach the first `batch` nodes from
    // worker local list as a sub-chain (head=batch_head, tail=batch_tail),
    // then CAS-splice onto global free_list in a single step.
    if (runtime->isolate && runtime->isolate->sys_heap) {
        XrCoroStructPool *pool = runtime->isolate->sys_heap->coro_pool;
        if (pool && pool->initialized) {
            int batch = worker->p.local_free_count / 2;
            XrCoroutine *batch_head = NULL;
            XrCoroutine *batch_tail = NULL;
            for (int i = 0; i < batch; i++) {
                XrCoroutine *c = worker->p.local_free_list;
                if (!c) break;
                worker->p.local_free_list = c->next;
                worker->p.local_free_count--;
                c->next = batch_head;
                batch_head = c;
                if (!batch_tail) batch_tail = c;
            }
            if (batch_head) {
                XrCoroutine *head;
                do {
                    head = atomic_load_explicit(&pool->free_list, memory_order_relaxed);
                    batch_tail->next = head;
                } while (!atomic_compare_exchange_weak_explicit(
                    &pool->free_list, &head, batch_head,
                    memory_order_release, memory_order_relaxed));
            }
        }
    }

    // Now put current coro to local
    coro->next = worker->p.local_free_list;
    worker->p.local_free_list = coro;
    worker->p.local_free_count++;
}
