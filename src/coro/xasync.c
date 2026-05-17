/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xasync.c - Async thread pool implementation
 *
 * KEY CONCEPT:
 *   Fixed async thread count, lock-free task queue,
 *   per-Worker completion queues.
 */

#include "xasync.h"
#include "xworker.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../base/xlog.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ========== Internal Function Declarations ==========

static void *async_thread_main(void *arg);
static void async_queue_push(XrAsyncPool *pool, XrAsyncJob *job);
static XrAsyncJob *async_queue_pop(XrAsyncPool *pool);
static void ready_queue_push(XrAsyncReadyQueue *q, XrAsyncJob *job);
static XrAsyncJob *ready_queue_pop(XrAsyncReadyQueue *q);

// ========== Async Thread Main Loop ==========

// Async thread main function
// Loop: 1. Get task from queue, 2. Execute invoke (blocking op),
//       3. Put result in Worker's completion queue
//
// Shutdown is signalled by async_pool_destroy clearing pool->running
// under the queue mutex and broadcasting the cond. The pop then
// returns NULL, which is our single "go home" signal here; no extra
// running flag check is needed (and reading running outside the mutex
// would re-introduce a data race besides costing the MSan shadow
// propagation hole that motivated the _Atomic -> bool switch).
static void *async_thread_main(void *arg) {
    XrAsyncPool *pool = (XrAsyncPool *) arg;

    for (;;) {
        // Get task from queue
        XrAsyncJob *job = async_queue_pop(pool);
        if (!job) {
            break;  // Shutdown signalled (running=false under mutex)
        }

        // Execute task (blocking op here)
        if (job->invoke) {
            job->invoke(job->data);
        }

        // Put in Worker's completion queue
        if (job->worker_id >= 0 && job->worker_id < XR_MAX_WORKERS) {
            ready_queue_push(&pool->ready_queues[job->worker_id], job);

            // Wake Worker (if sleeping)
            if (pool->runtime && job->worker_id < pool->runtime->worker_count) {
                XrWorker *worker = &pool->runtime->workers[job->worker_id];
                atomic_store_explicit(&worker->m->park_state, XR_PARK_WOKEN, memory_order_release);
                xr_park_futex_wake(&worker->m->park_state);
            }
        } else {
            // Invalid worker_id, free directly
            xr_async_job_free(job);
        }
    }

    return NULL;
}

// ========== Task Queue Operations ==========

// Enqueue task
static void async_queue_push(XrAsyncPool *pool, XrAsyncJob *job) {
    job->next = NULL;

    xr_mutex_lock(&pool->queue_mutex);

    if (pool->queue_tail) {
        pool->queue_tail->next = job;
    } else {
        pool->queue_head = job;
    }
    pool->queue_tail = job;

    // Wake one waiting async thread
    xr_cond_signal(&pool->queue_cond);

    xr_mutex_unlock(&pool->queue_mutex);
}

// Dequeue task (blocking wait).
// Returns NULL only on shutdown (caller must treat that as terminal).
static XrAsyncJob *async_queue_pop(XrAsyncPool *pool) {
    xr_mutex_lock(&pool->queue_mutex);

    // Wait for task or shutdown signal. Both predicates are guarded by
    // queue_mutex; spurious cond_wake re-checks here.
    while (!pool->queue_head && pool->running) {
        xr_cond_wait(&pool->queue_cond, &pool->queue_mutex);
    }

    XrAsyncJob *job = NULL;
    if (pool->queue_head) {
        job = pool->queue_head;
        pool->queue_head = job->next;
        if (!pool->queue_head) {
            pool->queue_tail = NULL;
        }
        job->next = NULL;
    }

    xr_mutex_unlock(&pool->queue_mutex);
    return job;
}

// ========== Completion Queue Operations (lock-free MPSC) ==========
//
// Producers (async worker threads): CAS-prepend onto the head.
// Consumer (owner worker): atomic_exchange the entire chain to NULL, then
// walk it locally without further synchronization.

static void ready_queue_push(XrAsyncReadyQueue *q, XrAsyncJob *job) {
    XrAsyncJob *head;
    do {
        head = atomic_load_explicit(&q->head, memory_order_relaxed);
        job->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&q->head, &head, job, memory_order_release,
                                                    memory_order_relaxed));
}

// Pop one element (CAS). Used only by destroy path.
static XrAsyncJob *ready_queue_pop(XrAsyncReadyQueue *q) {
    for (int retry = 0; retry < 8; retry++) {
        XrAsyncJob *head = atomic_load_explicit(&q->head, memory_order_acquire);
        if (!head)
            return NULL;
        XrAsyncJob *next = head->next;
        if (atomic_compare_exchange_weak_explicit(&q->head, &head, next, memory_order_acq_rel,
                                                  memory_order_acquire)) {
            head->next = NULL;
            return head;
        }
    }
    return NULL;
}

// O(1) whole-list drain via atomic_exchange.
// Grabs the entire chain in a single atomic op; caller walks locally.
static XrAsyncJob *ready_queue_drain_all(XrAsyncReadyQueue *q) {
    return atomic_exchange_explicit(&q->head, NULL, memory_order_acq_rel);
}

// ========== Public API ==========

// Initialize async thread pool (data structures only, no threads created)
void xr_async_pool_init(XrAsyncPool *pool, struct XrRuntime *runtime, int thread_count) {
    XR_DCHECK(runtime != NULL, "async_pool_init: NULL runtime");
    if (!pool)
        return;

    memset(pool, 0, sizeof(XrAsyncPool));

    pool->runtime = runtime;
    pool->thread_count = (thread_count > 0) ? thread_count : XR_ASYNC_THREAD_COUNT;

    // Limit thread count
    if (pool->thread_count > 64) {
        pool->thread_count = 64;
    }

    // Initialize task queue
    pool->queue_head = NULL;
    pool->queue_tail = NULL;
    xr_mutex_init(&pool->queue_mutex);
    xr_cond_init(&pool->queue_cond);

    // Initialize completion queues (lock-free MPSC; no mutex).
    for (int i = 0; i < XR_MAX_WORKERS; i++) {
        atomic_store(&pool->ready_queues[i].head, (XrAsyncJob *) NULL);
    }

    // Set running flag. No store fence needed: threads do not exist
    // yet, and xr_async_pool_start_threads is called only after this
    // returns. By the time async_thread_main runs, the pthread_create
    // happens-before edge already publishes pool->running == true.
    pool->running = true;
}

// Create async threads (called lazily on first spawn)
void xr_async_pool_start_threads(XrAsyncPool *pool) {
    if (!pool)
        return;

    pool->threads = (xr_thread_t *) xr_calloc(pool->thread_count, sizeof(xr_thread_t));
    if (!pool->threads) {
        xr_log_warning("async", "failed to allocate thread array");
        return;
    }

    for (int i = 0; i < pool->thread_count; i++) {
        if (!xr_thread_create_ex(&pool->threads[i], async_thread_main, pool, XR_ASYNC_STACK_SIZE)) {
            xr_log_warning("async", "failed to create async thread %d", i);
        }
    }
}

// Destroy async thread pool
void xr_async_pool_destroy(XrAsyncPool *pool) {
    if (!pool)
        return;

    // Set shutdown flag *inside* the mutex, paired with the broadcast,
    // so that any thread sitting in cond_wait observes the flip on the
    // same happens-before edge it uses to relock the mutex. The previous
    // ordering (store outside, broadcast inside) opened a window where
    // a waiter that had just popped from cond_wait could re-check the
    // loop predicate at xasync.c:97 against a value MSan tracked as
    // released by xr_runtime_destroy's free(pool), surfacing the
    // shutdown UAF reported on 1148_scope_race_stress.xr.
    xr_mutex_lock(&pool->queue_mutex);
    pool->running = false;
    xr_cond_broadcast(&pool->queue_cond);
    xr_mutex_unlock(&pool->queue_mutex);

    // Wait for all threads to exit
    if (pool->threads) {
        for (int i = 0; i < pool->thread_count; i++) {
            if (xr_thread_is_valid(pool->threads[i])) {
                xr_thread_join(pool->threads[i], NULL);
            }
        }
        xr_free(pool->threads);
        pool->threads = NULL;
    }

    // Clean up remaining tasks in queue
    XrAsyncJob *job;
    while ((job = async_queue_pop(pool)) != NULL) {
        xr_async_job_free(job);
    }

    // Clean up remaining tasks in completion queues.
    // No mutex to destroy in the MPSC design.
    for (int i = 0; i < XR_MAX_WORKERS; i++) {
        while ((job = ready_queue_pop(&pool->ready_queues[i])) != NULL) {
            xr_async_job_free(job);
        }
    }

    // Destroy sync primitives
    xr_mutex_destroy(&pool->queue_mutex);
    xr_cond_destroy(&pool->queue_cond);
}

// Submit async task
// Coroutine will be suspended until task completes
void xr_async_submit(XrAsyncPool *pool, XrAsyncJob *job) {
    if (!pool || !job)
        return;

    // Mark coroutine as blocked
    if (job->coro) {
        xr_coro_flags_set(job->coro, XR_CORO_FLG_BLOCKED);
    }

    // Submit to task queue
    async_queue_push(pool, job);
}

// Check completion queue, wake coroutines
// Returns number of tasks processed
int xr_async_check_ready(XrAsyncPool *pool, int worker_id) {
    if (!pool || worker_id < 0 || worker_id >= XR_MAX_WORKERS) {
        return 0;
    }

    XrAsyncReadyQueue *q = &pool->ready_queues[worker_id];

    // O(1) drain — grab entire list in one atomic_exchange,
    // then walk locally without further CAS contention.
    XrAsyncJob *list = ready_queue_drain_all(q);
    if (!list)
        return 0;

    int count = 0;
    while (list) {
        XrAsyncJob *job = list;
        list = job->next;
        job->next = NULL;
        count++;

        // Wake coroutine
        if (job->coro) {
            // Clear blocked flag, set ready flag
            xr_coro_flags_clear(job->coro, XR_CORO_FLG_BLOCKED);
            xr_coro_flags_set(job->coro, XR_CORO_FLG_READY);

            // Put in coroutine's target Worker inbox (no global queue).
            // Respects Coro.lockThread(): locked coros return to their locked worker.
            if (pool->runtime) {
                XrRuntime *runtime = pool->runtime;
                int target_id = xr_coro_wake_target_id(job->coro);
                if (target_id < 0 || target_id >= runtime->worker_count) {
                    target_id = 0;  // Fallback: Worker 0
                }
                xr_worker_inbox_enqueue(runtime, target_id, job->coro);
            }
        }

        // Free task
        xr_async_job_free(job);
    }

    return count;
}

// Create async task
XrAsyncJob *xr_async_job_create(struct XrCoroutine *coro, int worker_id, void (*invoke)(void *),
                                void *data) {
    XrAsyncJob *job = (XrAsyncJob *) xr_calloc(1, sizeof(XrAsyncJob));
    if (!job)
        return NULL;

    job->coro = coro;
    job->worker_id = worker_id;
    job->invoke = invoke;
    job->data = data;
    job->result = XR_NULL_VAL;
    job->error = 0;

    return job;
}

// Free async task
void xr_async_job_free(XrAsyncJob *job) {
    if (job) {
        xr_free(job);
    }
}
