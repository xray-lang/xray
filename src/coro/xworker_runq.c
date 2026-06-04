/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xworker_runq.c - Per-P run queue and worker push/pop operations
 *
 * KEY CONCEPT:
 *   Chase-Lev work-stealing deque per priority, with a runtime-wide
 *   injection queue for owner-side bursts that outgrow the local deque.
 *   Owner thread is lock-free on the common push/pop path; thieves CAS
 *   the bottom pointer from remote threads.
 *
 * RELATED:
 *   - xproc.h: XrRunQueue / XrProc.runq[]
 *   - xsteal_queue.h: underlying Chase-Lev deque
 *   - xworker_sched.c: worker_loop consumes via xr_worker_pop
 */
#include "xworker_internal.h"
#include "../base/xchecks.h"

// ========== Run Queue Implementation (Chase-Lev deque) ==========

static int normalize_coro_priority(int priority) {
    if (priority < 0)
        return 0;
    if (priority >= XR_CORO_PRIORITY_COUNT)
        return XR_CORO_PRIORITY_COUNT - 1;
    return priority;
}

static int runq_index_for_priority(int priority) {
    return priority == CORO_PRIORITY_HIGH ? 1 : 0;
}

static void prepare_scheduled_coro(XrCoroutine *coro, int priority) {
    coro->submit_time = xr_monotonic_ticks();
    coro->schedule_count = (priority == CORO_PRIORITY_LOW) ? XR_RESCHEDULE_LOW : 1;
}

static bool worker_enqueue_runq_or_inject(XrWorker *worker, XrCoroutine *coro, int priority,
                                          bool count_spill) {
    XR_DCHECK(worker != NULL, "enqueue_runq_or_inject: NULL worker");
    XR_DCHECK(coro != NULL, "enqueue_runq_or_inject: NULL coro");
    priority = normalize_coro_priority(priority);
    prepare_scheduled_coro(coro, priority);

    int runq_idx = runq_index_for_priority(priority);
    if (xr_steal_queue_push(&worker->p.runq[runq_idx].deque, coro)) {
        return true;
    }

    XrRuntime *runtime = worker->p.runtime;
    if (!runtime) {
        xr_runq_enqueue(&worker->p.runq[runq_idx], coro);
        return true;
    }
    if (count_spill) {
        xr_sched_metric_inc(runtime, &runtime->sched_stats.inject_spill_count);
    }
    xr_injectq_push(runtime, coro);
    return false;
}

void xr_runq_init(XrRunQueue *rq) {
    xr_steal_queue_init(&rq->deque, XR_LOCAL_QUEUE_SIZE);
    rq->overflow_first = NULL;
    rq->overflow_last = NULL;
    rq->overflow_len = 0;
}

void xr_runq_destroy(XrRunQueue *rq) {
    xr_steal_queue_destroy(&rq->deque);
}

// ========== Global Injection Queue ==========

void xr_injectq_init(XrRuntime *runtime) {
    if (!runtime)
        return;
    atomic_store_explicit(&runtime->nonempty_inject_mask, 0, memory_order_relaxed);
    for (int pi = 0; pi < XR_CORO_PRIORITY_COUNT; pi++) {
        XrInjectQueue *q = &runtime->injectq[pi];
        xr_mutex_init(&q->lock);
        q->head = NULL;
        q->tail = NULL;
        atomic_store_explicit(&q->len, 0, memory_order_relaxed);
    }
}

void xr_injectq_destroy(XrRuntime *runtime) {
    if (!runtime)
        return;
    for (int pi = 0; pi < XR_CORO_PRIORITY_COUNT; pi++) {
        XrInjectQueue *q = &runtime->injectq[pi];
        xr_mutex_lock(&q->lock);
        q->head = NULL;
        q->tail = NULL;
        atomic_store_explicit(&q->len, 0, memory_order_relaxed);
        xr_mutex_unlock(&q->lock);
        xr_mutex_destroy(&q->lock);
    }
    atomic_store_explicit(&runtime->nonempty_inject_mask, 0, memory_order_relaxed);
}

void xr_injectq_push_batch(XrRuntime *runtime, XrCoroutine *first, XrCoroutine *last, int count,
                           int priority) {
    if (!runtime || !first)
        return;

    priority = normalize_coro_priority(priority);
    XrCoroutine *cur = first;
    XrCoroutine *actual_last = NULL;
    int actual_count = 0;
    while (cur && (count <= 0 || actual_count < count)) {
        XrCoroutine *next = cur->sched_link;
        prepare_scheduled_coro(cur, priority);
        actual_last = cur;
        actual_count++;
        if (cur == last)
            break;
        cur = next;
    }
    if (!actual_last)
        return;
    actual_last->sched_link = NULL;

    XrInjectQueue *q = &runtime->injectq[priority];
    xr_mutex_lock(&q->lock);
    if (q->tail) {
        q->tail->sched_link = first;
    } else {
        q->head = first;
    }
    q->tail = actual_last;
    atomic_fetch_add_explicit(&q->len, actual_count, memory_order_relaxed);
    atomic_fetch_or_explicit(&runtime->nonempty_inject_mask, (uint32_t) 1u << priority,
                             memory_order_release);
    xr_mutex_unlock(&q->lock);

    xr_sched_metric_add(runtime, &runtime->sched_stats.inject_push_count, (uint64_t) actual_count);
    wake_idle_worker(runtime);
}

void xr_injectq_push(XrRuntime *runtime, XrCoroutine *coro) {
    if (!runtime || !coro)
        return;
    int priority = normalize_coro_priority(xr_coro_get_priority(xr_coro_flags_load(coro)));
    coro->sched_link = NULL;
    xr_injectq_push_batch(runtime, coro, coro, 1, priority);
}

XrCoroutine *xr_injectq_pop_one(XrRuntime *runtime, int priority) {
    if (!runtime)
        return NULL;
    priority = normalize_coro_priority(priority);
    XrInjectQueue *q = &runtime->injectq[priority];

    xr_mutex_lock(&q->lock);
    XrCoroutine *coro = q->head;
    if (coro) {
        q->head = coro->sched_link;
        if (!q->head) {
            q->tail = NULL;
            atomic_fetch_and_explicit(&runtime->nonempty_inject_mask, ~((uint32_t) 1u << priority),
                                      memory_order_release);
        }
        atomic_fetch_sub_explicit(&q->len, 1, memory_order_relaxed);
    } else {
        atomic_fetch_and_explicit(&runtime->nonempty_inject_mask, ~((uint32_t) 1u << priority),
                                  memory_order_release);
    }
    xr_mutex_unlock(&q->lock);

    if (!coro)
        return NULL;
    coro->sched_link = NULL;
    xr_sched_metric_inc(runtime, &runtime->sched_stats.inject_pop_count);
    return coro;
}

int xr_injectq_pop_batch(XrRuntime *runtime, XrWorker *worker, int priority, int max_count) {
    if (!runtime || !worker || max_count <= 0)
        return 0;
    priority = normalize_coro_priority(priority);
    XrInjectQueue *q = &runtime->injectq[priority];

    XrCoroutine *first = NULL;
    XrCoroutine *last = NULL;
    int count = 0;
    xr_mutex_lock(&q->lock);
    while (q->head && count < max_count) {
        XrCoroutine *coro = q->head;
        q->head = coro->sched_link;
        if (!q->head)
            q->tail = NULL;
        coro->sched_link = NULL;
        if (last) {
            last->sched_link = coro;
        } else {
            first = coro;
        }
        last = coro;
        count++;
    }
    if (!q->head) {
        atomic_fetch_and_explicit(&runtime->nonempty_inject_mask, ~((uint32_t) 1u << priority),
                                  memory_order_release);
    }
    if (count > 0) {
        atomic_fetch_sub_explicit(&q->len, count, memory_order_relaxed);
    }
    xr_mutex_unlock(&q->lock);

    XrCoroutine *cur = first;
    while (cur) {
        XrCoroutine *next = cur->sched_link;
        cur->sched_link = NULL;
        xr_runq_enqueue(&worker->p.runq[runq_index_for_priority(priority)], cur);
        worker->p.local_runq_len++;
        cur = next;
    }
    xr_sched_metric_add(runtime, &runtime->sched_stats.inject_pop_count, (uint64_t) count);
    return count;
}

// Enqueue: owner thread lock-free push, overflow to linked list if deque full
void xr_runq_enqueue(XrRunQueue *rq, XrCoroutine *coro) {
    coro->submit_time = xr_monotonic_ticks();
    if (!xr_steal_queue_push(&rq->deque, coro)) {
        // Deque full: overflow to linked list (never discard)
        coro->sched_link = NULL;
        if (rq->overflow_last)
            rq->overflow_last->sched_link = coro;
        else
            rq->overflow_first = coro;
        rq->overflow_last = coro;
        rq->overflow_len++;
    }
}

// Dequeue: owner thread lock-free pop
XrCoroutine *xr_runq_dequeue(XrRunQueue *rq) {
    XrCoroutine *c = xr_steal_queue_pop(&rq->deque);
    return c;
}

// Work stealing via CAS (no mutex needed)
// Steal count: min(victim_len / 2, 32)
int xr_runq_steal(XrRunQueue *src, XrRunQueue *dst, int max_steal) {
    int src_len = xr_steal_queue_size(&src->deque);
    int actual_max = src_len / 2;
    if (actual_max > 32)
        actual_max = 32;
    if (actual_max > max_steal)
        actual_max = max_steal;
    if (actual_max <= 0)
        actual_max = 1;

    int stolen = 0;
    for (int i = 0; i < actual_max; i++) {
        XrCoroutine *c = xr_steal_queue_steal(&src->deque);
        if (!c)
            break;
        // Stolen coros come out of the Chase-Lev deque, which never exposes
        // items that are also on the overflow linked-list.  Assert the
        // invariant so any future regression (e.g. sched_link aliasing via
        // MPSC inbox or delay list) surfaces immediately in debug builds.
        XR_DCHECK(c->sched_link == NULL, "runq_steal: stolen coro sched_link must be NULL");
        if (!xr_steal_queue_push(&dst->deque, c)) {
            // Destination full: put back via overflow
            c->sched_link = NULL;
            if (dst->overflow_last)
                dst->overflow_last->sched_link = c;
            else
                dst->overflow_first = c;
            dst->overflow_last = c;
            dst->overflow_len++;
        }
        stolen++;
    }
    return stolen;
}

// ========== Worker Pop / Push ==========

// Worker pop from local queue (Chase-Lev deque)
//
// Design:
// - LIFO slot first (cache locality for message passing)
// - Priority order: HIGH(1) > NORMAL(0)
// - LOW coroutines popped from NORMAL deque are checked for schedule_count
// - If schedule_count > 1, put to overflow list for delayed scheduling
XrCoroutine *xr_worker_pop(XrWorker *worker) {
    // 0. LIFO slot: prioritize last scheduled coroutine for cache locality
    XrCoroutine *_lifo = atomic_load_explicit(&worker->p.lifo_slot, memory_order_relaxed);
    if (_lifo) {
        if (worker->p.lifo_polls < XR_MAX_LIFO_POLLS) {
            XrCoroutine *c = _lifo;
            atomic_store_explicit(&worker->p.lifo_slot, NULL, memory_order_relaxed);
            worker->p.lifo_polls++;
            worker->p.local_runq_len--;
            worker->p.stats.lifo_hit_count++;
            return c;
        }
        // Starvation prevention: flush LIFO slot to a shared-ready path.
        XrCoroutine *evicted = _lifo;
        atomic_store_explicit(&worker->p.lifo_slot, NULL, memory_order_relaxed);
        if (!worker_enqueue_runq_or_inject(worker, evicted, worker->p.lifo_slot_prio, true)) {
            worker->p.local_runq_len--;
        }
        worker->p.stats.lifo_flush_count++;
    }
    // Reset LIFO polls when falling through to normal queues
    worker->p.lifo_polls = 0;

    // 1. HIGH queue first
    XrCoroutine *c = xr_steal_queue_pop(&worker->p.runq[1].deque);
    if (c) {
        worker->p.local_runq_len--;
        return c;
    }

    // 2. NORMAL overflow (delayed LOW coroutines ready to run)
    XrRunQueue *nq = &worker->p.runq[0];
    if (nq->overflow_first && --nq->overflow_first->schedule_count <= 0) {
        c = nq->overflow_first;
        nq->overflow_first = c->sched_link;
        if (!nq->overflow_first)
            nq->overflow_last = NULL;
        nq->overflow_len--;
        c->sched_link = NULL;
        worker->p.local_runq_len--;
        return c;
    }

    // 3. NORMAL deque (with retry loop for LOW priority delay)
retry:
    c = xr_steal_queue_pop(&nq->deque);
    if (c && xr_coro_get_priority(xr_coro_flags_load(c)) == CORO_PRIORITY_LOW &&
        c->schedule_count > 1) {
        // LOW coroutine with remaining delay: put to overflow
        c->schedule_count--;
        c->sched_link = NULL;
        if (nq->overflow_last)
            nq->overflow_last->sched_link = c;
        else
            nq->overflow_first = c;
        nq->overflow_last = c;
        nq->overflow_len++;
        goto retry;
    }

    // 4. Drain overflow back into deque when deque has space
    while (!c && nq->overflow_first) {
        c = nq->overflow_first;
        nq->overflow_first = c->sched_link;
        if (!nq->overflow_first)
            nq->overflow_last = NULL;
        nq->overflow_len--;
        c->sched_link = NULL;
        // Try to push back into deque for future stealing
        if (c->schedule_count > 0) {
            if (xr_steal_queue_push(&nq->deque, c)) {
                c = NULL;
                continue;
            }
            // Deque still full, return this coroutine
        }
        break;
    }

    if (c)
        worker->p.local_runq_len--;
    return c;
}

// Push to LIFO slot for cache locality.
// If LIFO slot occupied, evict previous occupant to a shared-ready path.
// Only effective when called from the owning worker thread.
void xr_worker_push_lifo(XrWorker *worker, XrCoroutine *coro) {
    XR_DCHECK(worker != NULL, "worker_push_lifo: NULL worker");
    XR_DCHECK(coro != NULL, "worker_push_lifo: NULL coro");
    // Only use LIFO slot when called from the owning worker
    if (xr_current_worker() == worker) {
        XrCoroutine *prev = atomic_load_explicit(&worker->p.lifo_slot, memory_order_relaxed);
        int new_prio = xr_coro_get_priority(xr_coro_flags_load(coro));
        atomic_store_explicit(&worker->p.lifo_slot, coro, memory_order_relaxed);
        int prev_prio = worker->p.lifo_slot_prio;
        worker->p.lifo_slot_prio = new_prio;
        worker->p.local_runq_len++;
        if (prev) {
            // Evict previous occupant while preserving local count semantics.
            if (!worker_enqueue_runq_or_inject(worker, prev, prev_prio, true)) {
                worker->p.local_runq_len--;
            }
        }
        return;
    }
    // Cross-worker: fall back to normal push
    xr_worker_push(worker, coro);
}

// Worker push to local queue (Chase-Lev deque)
//
// Design:
// - LOW coroutines go to NORMAL queue (index 0), schedule_count=8
// - HIGH coroutines go to HIGH queue (index 1)
// - Local deque full spills new work to the global injection queue
void xr_worker_push(XrWorker *worker, XrCoroutine *coro) {
    XR_DCHECK(worker != NULL, "worker_push: NULL worker");
    XR_DCHECK(coro != NULL, "worker_push: NULL coro");
    // Chase-Lev push is owner-thread-only. Catch residual
    // cross-worker callers that should go through inbox instead.
    // Skip check when TLS is not yet initialized (startup / single-thread).
    XR_DCHECK(xr_current_worker() == NULL || xr_current_worker() == worker,
              "worker_push: cross-worker push detected (use inbox)");
    int priority = normalize_coro_priority(xr_coro_get_priority(xr_coro_flags_load(coro)));
    bool queued_locally = worker_enqueue_runq_or_inject(worker, coro, priority, true);
    if (queued_locally) {
        worker->p.local_runq_len++;
    }
    XrRuntime *rt = worker->p.runtime;

    // wake: if no spinner is scanning for work, wake a parked worker.
    // This ensures newly enqueued coros are discovered promptly.
    if (queued_locally && rt &&
        atomic_load_explicit(&rt->spinning_count, memory_order_relaxed) == 0) {
        wake_idle_worker(rt);
    }
}
