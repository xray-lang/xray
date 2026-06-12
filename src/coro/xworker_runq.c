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
 *   One owner-local Chase-Lev work-stealing deque per P, plus a runtime-wide
 *   injection queue for owner-side bursts that outgrow the local deque.
 *   Owner push/pop is lock-free on the common path; thieves CAS the top
 *   pointer from remote workers.
 *
 * RELATED:
 *   - xproc.h: XrRunQueue / XrProc.runq
 *   - xsteal_queue.h: underlying Chase-Lev deque
 *   - xworker_sched.c: worker_loop consumes via xr_worker_pop
 */
#include "xworker_internal.h"
#include "../base/xchecks.h"

#define XR_LIFO_INJECT_BACKLOG_THRESHOLD XR_INJECT_POP_BATCH
/* Number of oldest local ready coroutines moved to global inject when the
 * owner deque is full. Mirrors the proven runqputslow shape: keep the fresh
 * wake local, share older work in one batch. */
#define XR_RUNQ_SPILL_BATCH (XR_LOCAL_QUEUE_SIZE / 2)
#define XR_RUNQ_STEAL_BATCH_MAX 32

typedef enum XrLifoGateDecision {
    XR_LIFO_GATE_ALLOW,
    XR_LIFO_GATE_BUDGET,
    XR_LIFO_GATE_BACKLOG,
} XrLifoGateDecision;

static bool worker_enqueue_runq_or_inject(XrWorker *worker, XrCoroutine *coro, bool count_spill);
static void injectq_push_batch_internal(XrRuntime *runtime, XrCoroutine *first, XrCoroutine *last,
                                        int count, bool prepare_items);

// ========== Run Queue Implementation (Chase-Lev deque) ==========

static int64_t worker_schedule_time(XrWorker *worker) {
    if (!worker)
        return xr_monotonic_ticks();
    if (worker->p.sched_time_budget == 0 || worker->p.sched_time_cache <= 0) {
        worker->p.sched_time_cache = xr_monotonic_ticks();
        worker->p.sched_time_budget = XR_SCHED_TIME_CACHE_BUDGET;
    }
    worker->p.sched_time_budget--;
    return worker->p.sched_time_cache;
}

static void prepare_scheduled_coro_at(XrCoroutine *coro, int64_t submit_time) {
    atomic_store_explicit(&coro->submit_time, submit_time, memory_order_relaxed);
    coro->schedule_count = 1;
    /* A re-enqueue ends any spawn burst. Resetting here (enqueuer owns the
     * coro) replaces the post-block reset in the executor result path, which
     * could race a concurrent wake once the coro was on a wait queue. */
    coro->spawn_burst_count = 0;
}

static void prepare_scheduled_coro(XrWorker *worker, XrCoroutine *coro) {
    prepare_scheduled_coro_at(coro, worker_schedule_time(worker));
}

static XrCoroutine *runq_pop_overflow(XrRunQueue *rq) {
    XrCoroutine *coro = rq->overflow_first;
    if (!coro)
        return NULL;
    rq->overflow_first = coro->sched_link;
    if (!rq->overflow_first)
        rq->overflow_last = NULL;
    rq->overflow_len--;
    coro->sched_link = NULL;
    return coro;
}

static XrCoroutine *runq_pop_local(XrRunQueue *rq) {
    XrCoroutine *coro = xr_steal_queue_pop(&rq->deque);
    if (coro)
        return coro;
    return runq_pop_overflow(rq);
}

static void runq_batch_append(XrCoroutine **first, XrCoroutine **last, XrCoroutine *coro) {
    XR_DCHECK(first != NULL, "runq_batch_append: NULL first");
    XR_DCHECK(last != NULL, "runq_batch_append: NULL last");
    XR_DCHECK(coro != NULL, "runq_batch_append: NULL coro");

    coro->sched_link = NULL;
    if (*last) {
        (*last)->sched_link = coro;
    } else {
        *first = coro;
    }
    *last = coro;
}

static void runq_enqueue_at(XrRunQueue *rq, XrCoroutine *coro, int64_t submit_time) {
    atomic_store_explicit(&coro->submit_time, submit_time, memory_order_relaxed);
    if (!xr_steal_queue_push(&rq->deque, coro)) {
        // Deque full: overflow to linked list.
        coro->sched_link = NULL;
        if (rq->overflow_last)
            rq->overflow_last->sched_link = coro;
        else
            rq->overflow_first = coro;
        rq->overflow_last = coro;
        rq->overflow_len++;
    }
}

static int runq_spill_oldest_batch(XrRunQueue *rq, int max_count, XrCoroutine **out_first,
                                   XrCoroutine **out_last) {
    XR_DCHECK(rq != NULL, "runq_spill_oldest_batch: NULL runq");
    XR_DCHECK(out_first != NULL, "runq_spill_oldest_batch: NULL first");
    XR_DCHECK(out_last != NULL, "runq_spill_oldest_batch: NULL last");

    *out_first = NULL;
    *out_last = NULL;
    int count = 0;
    while (count < max_count) {
        XrCoroutine *coro = NULL;
        XrStealQueueStatus status = xr_steal_queue_steal_status(&rq->deque, &coro);
        if (status != XR_STEAL_QUEUE_SUCCESS)
            break;
        runq_batch_append(out_first, out_last, coro);
        count++;
    }
    return count;
}

static bool worker_backlog_should_gate_lifo(XrWorker *worker, XrRuntime *runtime) {
    if (!runtime)
        return false;
    // Own-inbox check only: cross-worker deliveries pending on THIS worker
    // must not starve behind a LIFO chain. Other workers drain their own
    // inboxes; a global signal here would just be a shared-cacheline read
    // of state this worker cannot act on.
    if (!xr_mpsc_empty(&worker->p.inbox))
        return true;
    if (!atomic_load_explicit(&runtime->injectq_nonempty, memory_order_acquire))
        return false;
    int pending = atomic_load_explicit(&runtime->injectq.len, memory_order_relaxed);
    return pending >= XR_LIFO_INJECT_BACKLOG_THRESHOLD;
}

static XrLifoGateDecision worker_lifo_gate_decision(XrWorker *worker, bool consume_poll_budget) {
    if (consume_poll_budget && worker->p.lifo_polls >= XR_MAX_LIFO_POLLS)
        return XR_LIFO_GATE_BUDGET;

    XrRuntime *runtime = worker->p.runtime;
    if (worker_backlog_should_gate_lifo(worker, runtime))
        return XR_LIFO_GATE_BACKLOG;

    return XR_LIFO_GATE_ALLOW;
}

static void worker_record_lifo_gate(XrWorker *worker, XrLifoGateDecision decision) {
    if (!worker)
        return;
    switch (decision) {
        case XR_LIFO_GATE_BUDGET:
            worker->p.stats.lifo_gate_budget_count++;
            break;
        case XR_LIFO_GATE_BACKLOG:
            worker->p.stats.lifo_gate_backlog_count++;
            break;
        case XR_LIFO_GATE_ALLOW:
            break;
        default:
            XR_CHECK(false, "record_lifo_gate: invalid gate decision");
            break;
    }
}

XrCoroutine *xr_worker_try_pop_lifo(XrWorker *worker, bool consume_poll_budget) {
    XrCoroutine *lifo = atomic_load_explicit(&worker->p.lifo_slot, memory_order_relaxed);
    if (!lifo)
        return NULL;

    XrLifoGateDecision gate = worker_lifo_gate_decision(worker, consume_poll_budget);
    if (gate == XR_LIFO_GATE_ALLOW) {
        atomic_store_explicit(&worker->p.lifo_slot, NULL, memory_order_relaxed);
        if (consume_poll_budget)
            worker->p.lifo_polls++;
        XrRuntime *runtime = worker->p.runtime;
        if (XR_UNLIKELY(runtime && runtime->sched_stats_enabled)) {
            xr_proc_stats_record_runnable_wait(&worker->p.stats, lifo, xr_monotonic_ticks());
        }
        xr_proc_local_runq_dec(&worker->p, 1);
        worker->p.stats.lifo_hit_count++;
        return lifo;
    }

    worker_record_lifo_gate(worker, gate);
    atomic_store_explicit(&worker->p.lifo_slot, NULL, memory_order_relaxed);
    if (!worker_enqueue_runq_or_inject(worker, lifo, true)) {
        xr_proc_local_runq_dec(&worker->p, 1);
    }
    worker->p.stats.lifo_flush_count++;
    return NULL;
}

static XrCoroutine *worker_pop_local(XrWorker *worker) {
    XrCoroutine *coro = runq_pop_local(&worker->p.runq);
    if (!coro)
        return NULL;
    worker->p.stats.local_runq_pop_count++;
    XrRuntime *runtime = worker->p.runtime;
    if (XR_UNLIKELY(runtime && runtime->sched_stats_enabled)) {
        xr_proc_stats_record_runnable_wait(&worker->p.stats, coro, xr_monotonic_ticks());
    }
    xr_proc_local_runq_dec(&worker->p, 1);
    return coro;
}

static bool worker_enqueue_runq_or_inject(XrWorker *worker, XrCoroutine *coro, bool count_spill) {
    XR_DCHECK(worker != NULL, "enqueue_runq_or_inject: NULL worker");
    XR_DCHECK(coro != NULL, "enqueue_runq_or_inject: NULL coro");
    prepare_scheduled_coro(worker, coro);

    if (xr_steal_queue_push(&worker->p.runq.deque, coro)) {
        return true;
    }

    XrRuntime *runtime = worker->p.runtime;
    if (!runtime) {
        xr_runq_enqueue(&worker->p.runq, coro);
        return true;
    }

    XrCoroutine *spill_first = NULL;
    XrCoroutine *spill_last = NULL;
    int spill_count =
        runq_spill_oldest_batch(&worker->p.runq, XR_RUNQ_SPILL_BATCH, &spill_first, &spill_last);
    if (spill_count > 0) {
        xr_proc_local_runq_dec(&worker->p, spill_count);
        if (xr_steal_queue_push(&worker->p.runq.deque, coro)) {
            if (count_spill) {
                xr_sched_metric_add(runtime, &runtime->sched_stats.inject_spill_count,
                                    (uint64_t) spill_count);
            }
            injectq_push_batch_internal(runtime, spill_first, spill_last, spill_count, false);
            return true;
        }

        runq_batch_append(&spill_first, &spill_last, coro);
        if (count_spill) {
            xr_sched_metric_add(runtime, &runtime->sched_stats.inject_spill_count,
                                (uint64_t) (spill_count + 1));
        }
        injectq_push_batch_internal(runtime, spill_first, spill_last, spill_count + 1, false);
        return false;
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
    XrInjectQueue *q = &runtime->injectq;
    xr_mutex_init(&q->lock);
    q->head = NULL;
    q->tail = NULL;
    atomic_store_explicit(&q->len, 0, memory_order_relaxed);
    atomic_store_explicit(&runtime->injectq_nonempty, false, memory_order_relaxed);
}

void xr_injectq_destroy(XrRuntime *runtime) {
    if (!runtime)
        return;
    XrInjectQueue *q = &runtime->injectq;
    xr_mutex_lock(&q->lock);
    q->head = NULL;
    q->tail = NULL;
    atomic_store_explicit(&q->len, 0, memory_order_relaxed);
    xr_mutex_unlock(&q->lock);
    xr_mutex_destroy(&q->lock);
    atomic_store_explicit(&runtime->injectq_nonempty, false, memory_order_relaxed);
}

static void injectq_push_batch_internal(XrRuntime *runtime, XrCoroutine *first, XrCoroutine *last,
                                        int count, bool prepare_items) {
    if (!runtime || !first)
        return;

    XrCoroutine *cur = first;
    XrCoroutine *actual_last = NULL;
    int actual_count = 0;
    int64_t submit_time = prepare_items ? xr_monotonic_ticks() : 0;
    while (cur && (count <= 0 || actual_count < count)) {
        XrCoroutine *next = cur->sched_link;
        if (prepare_items) {
            prepare_scheduled_coro_at(cur, submit_time);
        }
        actual_last = cur;
        actual_count++;
        if (cur == last)
            break;
        cur = next;
    }
    if (!actual_last)
        return;
    actual_last->sched_link = NULL;

    XrInjectQueue *q = &runtime->injectq;
    xr_mutex_lock(&q->lock);
    if (q->tail) {
        q->tail->sched_link = first;
    } else {
        q->head = first;
    }
    q->tail = actual_last;
    atomic_fetch_add_explicit(&q->len, actual_count, memory_order_relaxed);
    atomic_store_explicit(&runtime->injectq_nonempty, true, memory_order_release);
    xr_mutex_unlock(&q->lock);

    xr_sched_metric_add(runtime, &runtime->sched_stats.inject_push_count, (uint64_t) actual_count);
    xr_sched_metric_inc(runtime, &runtime->sched_stats.inject_push_batch_count);
    wake_idle_workers(runtime, actual_count);
}

void xr_injectq_push_batch(XrRuntime *runtime, XrCoroutine *first, XrCoroutine *last, int count) {
    injectq_push_batch_internal(runtime, first, last, count, true);
}

void xr_injectq_push(XrRuntime *runtime, XrCoroutine *coro) {
    if (!runtime || !coro)
        return;
    coro->sched_link = NULL;
    xr_injectq_push_batch(runtime, coro, coro, 1);
}

int xr_injectq_pop_batch(XrRuntime *runtime, XrWorker *worker, int max_count) {
    if (!runtime || !worker || max_count <= 0)
        return 0;
    XrInjectQueue *q = &runtime->injectq;

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
        atomic_store_explicit(&runtime->injectq_nonempty, false, memory_order_release);
    }
    if (count > 0) {
        atomic_fetch_sub_explicit(&q->len, count, memory_order_relaxed);
    }
    xr_mutex_unlock(&q->lock);

    if (count <= 0)
        return 0;

    XrCoroutine *cur = first;
    int64_t fallback_submit_time = worker_schedule_time(worker);
    while (cur) {
        XrCoroutine *next = cur->sched_link;
        cur->sched_link = NULL;
        int64_t prior = atomic_load_explicit(&cur->submit_time, memory_order_relaxed);
        int64_t submit_time = prior > 0 ? prior : fallback_submit_time;
        runq_enqueue_at(&worker->p.runq, cur, submit_time);
        cur = next;
    }
    xr_proc_local_runq_inc(&worker->p, count);
    xr_sched_metric_inc(runtime, &runtime->sched_stats.inject_pop_batch_count);
    xr_sched_metric_add(runtime, &runtime->sched_stats.inject_pop_count, (uint64_t) count);
    return count;
}

// Enqueue: owner thread lock-free push, overflow to linked list if deque full.
void xr_runq_enqueue(XrRunQueue *rq, XrCoroutine *coro) {
    runq_enqueue_at(rq, coro, xr_monotonic_ticks());
}

// Dequeue: owner thread lock-free pop.
XrCoroutine *xr_runq_dequeue(XrRunQueue *rq) {
    return runq_pop_local(rq);
}

static void runq_push_stolen(XrRunQueue *dst, XrCoroutine *coro) {
    XR_DCHECK(dst != NULL, "runq_push_stolen: NULL dst");
    XR_DCHECK(coro != NULL, "runq_push_stolen: NULL coro");
    XR_DCHECK(coro->sched_link == NULL, "runq_push_stolen: linked stolen coro");

    if (xr_steal_queue_push(&dst->deque, coro))
        return;

    coro->sched_link = NULL;
    if (dst->overflow_last)
        dst->overflow_last->sched_link = coro;
    else
        dst->overflow_first = coro;
    dst->overflow_last = coro;
    dst->overflow_len++;
}

// Work stealing via CAS. Steal count: min(victim_len / 2, 32).
int xr_runq_steal(XrRunQueue *src, XrRunQueue *dst, int max_steal) {
    int src_len = xr_steal_queue_size(&src->deque);
    int actual_max = src_len / 2;
    if (actual_max > XR_RUNQ_STEAL_BATCH_MAX)
        actual_max = XR_RUNQ_STEAL_BATCH_MAX;
    if (actual_max > max_steal)
        actual_max = max_steal;
    if (actual_max <= 0)
        actual_max = 1;

    int stolen = 0;
    for (int i = 0; i < actual_max; i++) {
        XrCoroutine *c = NULL;
        XrStealQueueStatus status = xr_steal_queue_steal_status(&src->deque, &c);
        if (status != XR_STEAL_QUEUE_SUCCESS)
            break;
        XR_DCHECK(c->sched_link == NULL, "runq_steal: stolen coro sched_link must be NULL");
        runq_push_stolen(dst, c);
        stolen++;
    }
    return stolen;
}

XrRunqStealStatus xr_runq_steal_direct_status(XrRunQueue *src, XrRunQueue *dst, int max_steal,
                                              int *out_stolen, XrCoroutine **out_direct) {
    if (out_direct)
        *out_direct = NULL;
    if (out_stolen)
        *out_stolen = 0;

    int src_len = xr_steal_queue_size(&src->deque);
    int actual_max = src_len / 2;
    if (actual_max > XR_RUNQ_STEAL_BATCH_MAX)
        actual_max = XR_RUNQ_STEAL_BATCH_MAX;
    if (actual_max > max_steal)
        actual_max = max_steal;
    if (actual_max <= 0)
        actual_max = 1;

    XrCoroutine *direct = NULL;
    int stolen = 0;
    XrRunqStealStatus final_status = XR_RUNQ_STEAL_EMPTY;
    for (int i = 0; i < actual_max; i++) {
        XrCoroutine *c = NULL;
        XrStealQueueStatus status = xr_steal_queue_steal_status(&src->deque, &c);
        if (status != XR_STEAL_QUEUE_SUCCESS) {
            if (status == XR_STEAL_QUEUE_RETRY && stolen == 0)
                final_status = XR_RUNQ_STEAL_RETRY;
            break;
        }
        XR_DCHECK(c->sched_link == NULL, "runq_steal_direct: stolen coro sched_link must be NULL");
        if (direct)
            runq_push_stolen(dst, direct);
        direct = c;
        stolen++;
    }

    if (direct) {
        XR_DCHECK(direct->sched_link == NULL,
                  "runq_steal_direct: direct coro sched_link must be NULL");
    }
    if (out_direct)
        *out_direct = direct;
    else if (direct)
        runq_push_stolen(dst, direct);
    if (out_stolen)
        *out_stolen = stolen;
    return stolen > 0 ? XR_RUNQ_STEAL_SUCCESS : final_status;
}

// ========== Worker Pop / Push ==========

// Worker pop from local queue. LIFO slot wins only while locality gates allow it.
XrCoroutine *xr_worker_pop(XrWorker *worker) {
    XrCoroutine *coro = xr_worker_try_pop_lifo(worker, true);
    if (coro)
        return coro;

    worker->p.lifo_polls = 0;
    return worker_pop_local(worker);
}

// Push to LIFO slot for cache locality.
// If LIFO slot occupied, evict previous occupant to a shared-ready path.
// Only effective when called from the owning worker thread.
void xr_worker_push_lifo(XrWorker *worker, XrCoroutine *coro) {
    XR_DCHECK(worker != NULL, "worker_push_lifo: NULL worker");
    XR_DCHECK(coro != NULL, "worker_push_lifo: NULL coro");
    if (xr_current_worker() == worker) {
        XrCoroutine *prev = atomic_load_explicit(&worker->p.lifo_slot, memory_order_relaxed);
        prepare_scheduled_coro(worker, coro);
        atomic_store_explicit(&worker->p.lifo_slot, coro, memory_order_relaxed);
        xr_proc_local_runq_inc(&worker->p, 1);
        if (prev) {
            // Evict previous occupant while preserving local count semantics.
            if (!worker_enqueue_runq_or_inject(worker, prev, true)) {
                xr_proc_local_runq_dec(&worker->p, 1);
            }
        }
        return;
    }
    // Cross-worker: fall back to normal push.
    xr_worker_push(worker, coro);
}

int xr_worker_push_lifo_batch(XrWorker *worker, XrCoroutine *first) {
    if (!worker || !first)
        return 0;
    XR_DCHECK(xr_current_worker() == NULL || xr_current_worker() == worker,
              "worker_push_lifo_batch: cross-worker push detected (use inbox)");

    XrCoroutine *prev = atomic_load_explicit(&worker->p.lifo_slot, memory_order_relaxed);

    XrCoroutine *evict_first = NULL;
    XrCoroutine *evict_last = NULL;
    if (prev) {
        runq_batch_append(&evict_first, &evict_last, prev);
    }

    int total_count = 0;
    XrCoroutine *lifo = NULL;
    XrCoroutine *coro = first;
    while (coro) {
        XrCoroutine *next = coro->sched_link;
        coro->sched_link = NULL;
        total_count++;
        if (next) {
            runq_batch_append(&evict_first, &evict_last, coro);
        } else {
            lifo = coro;
        }
        coro = next;
    }
    if (!lifo)
        return 0;

    prepare_scheduled_coro(worker, lifo);
    atomic_store_explicit(&worker->p.lifo_slot, lifo, memory_order_relaxed);

    int local_delta = 1;
    coro = evict_first;
    while (coro) {
        XrCoroutine *next = coro->sched_link;
        coro->sched_link = NULL;
        bool is_prev = coro == prev;
        if (worker_enqueue_runq_or_inject(worker, coro, true)) {
            if (!is_prev)
                local_delta++;
        } else if (is_prev) {
            local_delta--;
        }
        coro = next;
    }

    if (local_delta > 0) {
        xr_proc_local_runq_inc(&worker->p, local_delta);
    } else if (local_delta < 0) {
        xr_proc_local_runq_dec(&worker->p, -local_delta);
    }
    return total_count;
}

// Worker push to local queue. Local deque full spills older work to injectq.
void xr_worker_push(XrWorker *worker, XrCoroutine *coro) {
    XR_DCHECK(worker != NULL, "worker_push: NULL worker");
    XR_DCHECK(coro != NULL, "worker_push: NULL coro");
    // Chase-Lev push is owner-thread-only. Catch residual
    // cross-worker callers that should go through inbox instead.
    // Skip check when TLS is not yet initialized (startup / single-thread).
    XR_DCHECK(xr_current_worker() == NULL || xr_current_worker() == worker,
              "worker_push: cross-worker push detected (use inbox)");
    bool queued_locally = worker_enqueue_runq_or_inject(worker, coro, true);
    if (queued_locally) {
        xr_proc_local_runq_inc(&worker->p, 1);
    }
    XrRuntime *rt = worker->p.runtime;

    // Wake if no spinner is scanning for work.
    if (queued_locally && rt &&
        atomic_load_explicit(&rt->spinning_count, memory_order_relaxed) == 0) {
        wake_idle_worker(rt);
    }
}

int xr_worker_push_batch(XrWorker *worker, XrCoroutine *first) {
    if (!worker || !first)
        return 0;
    XR_DCHECK(xr_current_worker() == NULL || xr_current_worker() == worker,
              "worker_push_batch: cross-worker push detected (use inbox)");

    int local_count = 0;
    int total_count = 0;
    XrCoroutine *coro = first;
    while (coro) {
        XrCoroutine *next = coro->sched_link;
        coro->sched_link = NULL;
        if (worker_enqueue_runq_or_inject(worker, coro, true)) {
            local_count++;
        }
        total_count++;
        coro = next;
    }

    if (local_count > 0) {
        xr_proc_local_runq_inc(&worker->p, local_count);
        XrRuntime *rt = worker->p.runtime;
        if (rt && atomic_load_explicit(&rt->spinning_count, memory_order_relaxed) == 0) {
            wake_idle_worker(rt);
        }
    }
    return total_count;
}
