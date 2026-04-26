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
 *   Chase-Lev work-stealing deque per priority + linked-list overflow
 *   for unbounded capacity. Owner thread is lock-free on push/pop;
 *   thieves CAS the bottom pointer from remote threads.
 *
 * RELATED:
 *   - xproc.h: XrRunQueue / XrProc.runq[]
 *   - xsteal_queue.h: underlying Chase-Lev deque
 *   - xworker_sched.c: worker_loop consumes via xr_worker_pop
 */
#include "xworker_internal.h"
#include "../base/xchecks.h"

// ========== Run Queue Implementation (Chase-Lev deque) ==========

void xr_runq_init(XrRunQueue *rq) {
    xr_steal_queue_init(&rq->deque, XR_LOCAL_QUEUE_SIZE);
    rq->overflow_first = NULL;
    rq->overflow_last = NULL;
    rq->overflow_len = 0;
}

void xr_runq_destroy(XrRunQueue *rq) {
    xr_steal_queue_destroy(&rq->deque);
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
            return c;
        }
        // Starvation prevention: flush LIFO slot to run queue
        XrCoroutine *evicted = _lifo;
        atomic_store_explicit(&worker->p.lifo_slot, NULL, memory_order_relaxed);
        int evict_idx = (worker->p.lifo_slot_prio == CORO_PRIORITY_HIGH) ? 1 : 0;
        xr_runq_enqueue(&worker->p.runq[evict_idx], evicted);
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
// If LIFO slot occupied, evict previous occupant to normal run queue.
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
            // Evict previous occupant to normal run queue (already counted)
            int runq_idx = (prev_prio == CORO_PRIORITY_HIGH) ? 1 : 0;
            xr_runq_enqueue(&worker->p.runq[runq_idx], prev);
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
// - Lock-free push via Chase-Lev deque
void xr_worker_push(XrWorker *worker, XrCoroutine *coro) {
    XR_DCHECK(worker != NULL, "worker_push: NULL worker");
    XR_DCHECK(coro != NULL, "worker_push: NULL coro");
    // Chase-Lev push is owner-thread-only. Catch residual
    // cross-worker callers that should go through inbox instead.
    // Skip check when TLS is not yet initialized (startup / single-thread).
    XR_DCHECK(xr_current_worker() == NULL || xr_current_worker() == worker,
              "worker_push: cross-worker push detected (use inbox)");
    int priority = xr_coro_get_priority(xr_coro_flags_load(coro));
    if (priority < 0)
        priority = 0;
    if (priority >= XR_CORO_PRIORITY_COUNT)
        priority = XR_CORO_PRIORITY_COUNT - 1;

    // LOW goes to NORMAL queue, controlled by schedule_count
    int runq_idx;
    if (priority == CORO_PRIORITY_LOW) {
        coro->schedule_count = XR_RESCHEDULE_LOW;  // 8 times delay
        runq_idx = 0;                              // NORMAL queue
    } else if (priority == CORO_PRIORITY_NORMAL) {
        coro->schedule_count = 1;
        runq_idx = 0;  // NORMAL queue
    } else {
        coro->schedule_count = 1;
        runq_idx = 1;  // HIGH queue
    }

    // unbounded linked list, direct enqueue
    xr_runq_enqueue(&worker->p.runq[runq_idx], coro);

    worker->p.local_runq_len++;
    XrRuntime *rt = worker->p.runtime;

    // wake: if no spinner is scanning for work, wake a parked worker.
    // This ensures newly enqueued coros are discovered promptly.
    if (rt && atomic_load_explicit(&rt->spinning_count, memory_order_relaxed) == 0) {
        wake_idle_worker(rt);
    }
}
