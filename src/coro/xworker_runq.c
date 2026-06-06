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

#define XR_LIFO_INJECT_BACKLOG_THRESHOLD XR_INJECT_POP_BATCH
/* Number of oldest local ready coroutines moved to global inject when the
 * owner deque is full. Mirrors the proven runqputslow shape: keep the fresh
 * wake local, share older work in one batch. */
#define XR_RUNQ_SPILL_BATCH (XR_LOCAL_QUEUE_SIZE / 2)

typedef enum XrLifoGateDecision {
    XR_LIFO_GATE_ALLOW,
    XR_LIFO_GATE_BUDGET,
    XR_LIFO_GATE_BACKLOG,
    XR_LIFO_GATE_PRIORITY,
} XrLifoGateDecision;

// ========== Run Queue Implementation (Chase-Lev deque) ==========

static int normalize_coro_priority(int priority) {
    if (priority < 0)
        return 0;
    if (priority >= XR_CORO_PRIORITY_COUNT)
        return XR_CORO_PRIORITY_COUNT - 1;
    return priority;
}

static bool worker_enqueue_runq_or_inject(XrWorker *worker, XrCoroutine *coro, int priority,
                                          bool count_spill);
static void injectq_push_batch_internal(XrRuntime *runtime, XrCoroutine *first, XrCoroutine *last,
                                        int count, int priority, bool prepare_items);

static int runq_index_for_priority(int priority) {
    return normalize_coro_priority(priority);
}

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
    coro->submit_time = submit_time;
    coro->schedule_count = 1;
}

static void prepare_scheduled_coro(XrWorker *worker, XrCoroutine *coro, int priority) {
    (void) priority;
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
    coro->submit_time = submit_time;
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

static int runq_spill_oldest_batch(XrRunQueue *rq, int max_count, XrCoroutine **out_first,
                                   XrCoroutine **out_last) {
    XR_DCHECK(rq != NULL, "runq_spill_oldest_batch: NULL runq");
    XR_DCHECK(out_first != NULL, "runq_spill_oldest_batch: NULL first");
    XR_DCHECK(out_last != NULL, "runq_spill_oldest_batch: NULL last");

    *out_first = NULL;
    *out_last = NULL;
    int count = 0;
    while (count < max_count) {
        XrCoroutine *coro = xr_steal_queue_steal(&rq->deque);
        if (!coro)
            break;
        runq_batch_append(out_first, out_last, coro);
        count++;
    }
    return count;
}

static XrCoroutine *runq_oldest_candidate(XrRunQueue *rq) {
    XrCoroutine *deque_oldest = xr_steal_queue_peek_top(&rq->deque);
    XrCoroutine *overflow_oldest = rq->overflow_first;
    if (!deque_oldest)
        return overflow_oldest;
    if (!overflow_oldest)
        return deque_oldest;
    return overflow_oldest->submit_time <= deque_oldest->submit_time ? overflow_oldest
                                                                     : deque_oldest;
}

static int effective_priority_for_coro(XrCoroutine *coro, int64_t now) {
    if (!coro)
        return -1;
    int priority = normalize_coro_priority(xr_coro_get_priority(xr_coro_flags_load(coro)));
    int64_t age = now - coro->submit_time;
    if (age <= 0)
        return priority;
    int boost = (int) (age / XR_PRIO_AGING_MS);
    if (boost > XR_PRIO_AGING_MAX_BOOST)
        boost = XR_PRIO_AGING_MAX_BOOST;
    priority += boost;
    return normalize_coro_priority(priority);
}

static int runq_effective_priority(XrRunQueue *rq, int64_t now) {
    return effective_priority_for_coro(runq_oldest_candidate(rq), now);
}

static XrCoroutine *runq_pop_selected(XrRunQueue *rq, bool oldest_first) {
    XrCoroutine *coro = NULL;
    if (oldest_first) {
        coro = runq_pop_overflow(rq);
        if (coro)
            return coro;
        return xr_steal_queue_steal(&rq->deque);
    }

    coro = xr_steal_queue_pop(&rq->deque);
    if (coro)
        return coro;
    return runq_pop_overflow(rq);
}

static bool priority_budget_has_credit(XrPriorityBudget *budget) {
    for (int priority = 0; priority < XR_CORO_PRIORITY_COUNT; priority++) {
        if (budget->credit[priority] > 0)
            return true;
    }
    return false;
}

static void worker_record_runnable_wait(XrWorker *worker, XrCoroutine *coro, int64_t now) {
    if (!worker || !coro)
        return;
    if (coro->submit_time <= 0)
        return;
    int64_t wait_ms = now - coro->submit_time;
    if (wait_ms <= 0)
        return;
    uint64_t wait = (uint64_t) wait_ms;
    worker->p.stats.runnable_wait_ms += wait;
    if (wait > worker->p.stats.runnable_wait_max_ms) {
        worker->p.stats.runnable_wait_max_ms = wait;
    }
}

void xr_worker_refresh_runq_masks(XrWorker *worker) {
    if (!worker || !worker->p.runtime)
        return;
    XrRuntime *runtime = worker->p.runtime;
    XrCoroutine *lifo = atomic_load_explicit(&worker->p.lifo_slot, memory_order_relaxed);
    int lifo_priority =
        lifo ? normalize_coro_priority(xr_coro_get_priority(xr_coro_flags_load(lifo))) : -1;
    bool has_cont = xr_steal_queue_size(&worker->p.cont_deque) > 0;
    for (int priority = 0; priority < XR_CORO_PRIORITY_COUNT; priority++) {
        int deque_len = xr_steal_queue_size(&worker->p.runq[priority].deque);
        bool nonempty =
            deque_len > 0 || worker->p.runq[priority].overflow_len > 0 || lifo_priority == priority;
        bool stealable = deque_len > 0;
        if (priority == CORO_PRIORITY_NORMAL && has_cont) {
            nonempty = true;
            stealable = true;
        }
        xr_runtime_set_runq_nonempty(runtime, worker->p.id, priority, nonempty);
        xr_runtime_set_runq_stealable(runtime, worker->p.id, priority, stealable);
    }
}

static int worker_select_actual_priority(XrWorker *worker, int effective_priority, int64_t now,
                                         bool *oldest_first) {
    for (int actual = CORO_PRIORITY_HIGH; actual >= CORO_PRIORITY_LOW; actual--) {
        XrRunQueue *rq = &worker->p.runq[actual];
        if (xr_runq_len(rq) <= 0)
            continue;
        int effective = runq_effective_priority(rq, now);
        if (effective != effective_priority)
            continue;
        if (oldest_first)
            *oldest_first = effective > actual;
        return actual;
    }
    return -1;
}

static bool worker_has_effective_high_work(XrWorker *worker, int64_t now) {
    for (int actual = CORO_PRIORITY_HIGH; actual >= CORO_PRIORITY_LOW; actual--) {
        if (xr_runq_len(&worker->p.runq[actual]) <= 0)
            continue;
        if (runq_effective_priority(&worker->p.runq[actual], now) >= CORO_PRIORITY_HIGH)
            return true;
    }

    XrRuntime *runtime = worker->p.runtime;
    if (!runtime)
        return false;
    uint32_t high_bit = (uint32_t) 1u << CORO_PRIORITY_HIGH;
    if (atomic_load_explicit(&runtime->nonempty_p_mask[CORO_PRIORITY_HIGH], memory_order_acquire) !=
        0) {
        return true;
    }
    return (atomic_load_explicit(&runtime->nonempty_inject_mask, memory_order_acquire) &
            high_bit) != 0;
}

static bool runtime_backlog_should_gate_lifo(XrRuntime *runtime) {
    if (!runtime)
        return false;
    if (atomic_load_explicit(&runtime->total_inbox_len, memory_order_relaxed) > 0)
        return true;

    uint32_t mask = atomic_load_explicit(&runtime->nonempty_inject_mask, memory_order_acquire);
    if (mask == 0)
        return false;

    int pending = 0;
    for (int priority = 0; priority < XR_CORO_PRIORITY_COUNT; priority++) {
        if ((mask & ((uint32_t) 1u << priority)) == 0)
            continue;
        pending += atomic_load_explicit(&runtime->injectq[priority].len, memory_order_relaxed);
        if (pending >= XR_LIFO_INJECT_BACKLOG_THRESHOLD)
            return true;
    }
    return false;
}

static XrLifoGateDecision worker_lifo_gate_decision(XrWorker *worker, XrCoroutine *lifo,
                                                    bool consume_poll_budget) {
    if (consume_poll_budget && worker->p.lifo_polls >= XR_MAX_LIFO_POLLS)
        return XR_LIFO_GATE_BUDGET;

    XrRuntime *runtime = worker->p.runtime;
    if (runtime_backlog_should_gate_lifo(runtime))
        return XR_LIFO_GATE_BACKLOG;

    int lifo_priority = normalize_coro_priority(xr_coro_get_priority(xr_coro_flags_load(lifo)));
    if (lifo_priority >= CORO_PRIORITY_HIGH)
        return XR_LIFO_GATE_ALLOW;

    if (runtime) {
        uint32_t high_bit = (uint32_t) 1u << CORO_PRIORITY_HIGH;
        bool high_inject =
            (atomic_load_explicit(&runtime->nonempty_inject_mask, memory_order_acquire) &
             high_bit) != 0;
        bool high_worker = atomic_load_explicit(&runtime->nonempty_p_mask[CORO_PRIORITY_HIGH],
                                                memory_order_acquire) != 0;
        if (!high_inject && !high_worker && xr_proc_local_runq_len(&worker->p) <= 1) {
            return XR_LIFO_GATE_ALLOW;
        }
    }

    return worker_has_effective_high_work(worker, xr_monotonic_ticks()) ? XR_LIFO_GATE_PRIORITY
                                                                        : XR_LIFO_GATE_ALLOW;
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
        case XR_LIFO_GATE_PRIORITY:
            worker->p.stats.lifo_gate_priority_count++;
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

    int priority = normalize_coro_priority(xr_coro_get_priority(xr_coro_flags_load(lifo)));
    XrLifoGateDecision gate = worker_lifo_gate_decision(worker, lifo, consume_poll_budget);
    if (gate == XR_LIFO_GATE_ALLOW) {
        atomic_store_explicit(&worker->p.lifo_slot, NULL, memory_order_relaxed);
        if (consume_poll_budget)
            worker->p.lifo_polls++;
        worker_record_runnable_wait(worker, lifo, xr_monotonic_ticks());
        xr_proc_local_runq_dec(&worker->p, 1);
        worker->p.stats.lifo_hit_count++;
        xr_worker_refresh_runq_masks(worker);
        return lifo;
    }

    worker_record_lifo_gate(worker, gate);
    atomic_store_explicit(&worker->p.lifo_slot, NULL, memory_order_relaxed);
    if (!worker_enqueue_runq_or_inject(worker, lifo, priority, true)) {
        xr_proc_local_runq_dec(&worker->p, 1);
    }
    worker->p.stats.lifo_flush_count++;
    xr_worker_refresh_runq_masks(worker);
    return NULL;
}

static XrCoroutine *worker_pop_weighted(XrWorker *worker) {
    XrPriorityBudget *budget = &worker->p.prio_budget;
    int64_t now = xr_monotonic_ticks();

    for (int refill = 0; refill < 2; refill++) {
        if (!priority_budget_has_credit(budget)) {
            xr_priority_budget_refill(budget);
        }

        for (int effective = CORO_PRIORITY_HIGH; effective >= CORO_PRIORITY_LOW; effective--) {
            if (budget->credit[effective] <= 0)
                continue;
            bool oldest_first = false;
            int actual = worker_select_actual_priority(worker, effective, now, &oldest_first);
            if (actual < 0)
                continue;
            XrCoroutine *coro = runq_pop_selected(&worker->p.runq[actual], oldest_first);
            if (!coro)
                continue;
            budget->credit[effective]--;
            worker->p.stats.local_runq_pop_count++;
            worker_record_runnable_wait(worker, coro, now);
            if (effective > actual) {
                worker->p.stats.priority_boost_count++;
            }
            xr_proc_local_runq_dec(&worker->p, 1);
            budget->cursor = effective;
            xr_worker_refresh_runq_masks(worker);
            return coro;
        }

        xr_priority_budget_refill(budget);
    }
    return NULL;
}

static bool worker_enqueue_runq_or_inject(XrWorker *worker, XrCoroutine *coro, int priority,
                                          bool count_spill) {
    XR_DCHECK(worker != NULL, "enqueue_runq_or_inject: NULL worker");
    XR_DCHECK(coro != NULL, "enqueue_runq_or_inject: NULL coro");
    priority = normalize_coro_priority(priority);
    prepare_scheduled_coro(worker, coro, priority);

    int runq_idx = runq_index_for_priority(priority);
    if (xr_steal_queue_push(&worker->p.runq[runq_idx].deque, coro)) {
        xr_runtime_set_runq_nonempty(worker->p.runtime, worker->p.id, runq_idx, true);
        return true;
    }

    XrRuntime *runtime = worker->p.runtime;
    if (!runtime) {
        xr_runq_enqueue(&worker->p.runq[runq_idx], coro);
        xr_runtime_set_runq_nonempty(worker->p.runtime, worker->p.id, runq_idx, true);
        return true;
    }

    XrCoroutine *spill_first = NULL;
    XrCoroutine *spill_last = NULL;
    int spill_count = runq_spill_oldest_batch(&worker->p.runq[runq_idx], XR_RUNQ_SPILL_BATCH,
                                              &spill_first, &spill_last);
    if (spill_count > 0) {
        xr_proc_local_runq_dec(&worker->p, spill_count);
        if (xr_steal_queue_push(&worker->p.runq[runq_idx].deque, coro)) {
            if (count_spill) {
                xr_sched_metric_add(runtime, &runtime->sched_stats.inject_spill_count,
                                    (uint64_t) spill_count);
            }
            injectq_push_batch_internal(runtime, spill_first, spill_last, spill_count, priority,
                                        false);
            xr_runtime_set_runq_nonempty(worker->p.runtime, worker->p.id, runq_idx, true);
            return true;
        }

        runq_batch_append(&spill_first, &spill_last, coro);
        if (count_spill) {
            xr_sched_metric_add(runtime, &runtime->sched_stats.inject_spill_count,
                                (uint64_t) (spill_count + 1));
        }
        injectq_push_batch_internal(runtime, spill_first, spill_last, spill_count + 1, priority,
                                    false);
        xr_worker_refresh_runq_masks(worker);
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

static void injectq_push_batch_internal(XrRuntime *runtime, XrCoroutine *first, XrCoroutine *last,
                                        int count, int priority, bool prepare_items) {
    if (!runtime || !first)
        return;

    priority = normalize_coro_priority(priority);
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
    xr_sched_metric_inc(runtime, &runtime->sched_stats.inject_push_batch_count);
    wake_idle_workers(runtime, actual_count);
}

void xr_injectq_push_batch(XrRuntime *runtime, XrCoroutine *first, XrCoroutine *last, int count,
                           int priority) {
    injectq_push_batch_internal(runtime, first, last, count, priority, true);
}

void xr_injectq_push(XrRuntime *runtime, XrCoroutine *coro) {
    if (!runtime || !coro)
        return;
    int priority = normalize_coro_priority(xr_coro_get_priority(xr_coro_flags_load(coro)));
    coro->sched_link = NULL;
    xr_injectq_push_batch(runtime, coro, coro, 1, priority);
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

    if (count <= 0)
        return 0;

    XrCoroutine *cur = first;
    int runq_idx = runq_index_for_priority(priority);
    int64_t submit_time = worker_schedule_time(worker);
    while (cur) {
        XrCoroutine *next = cur->sched_link;
        cur->sched_link = NULL;
        runq_enqueue_at(&worker->p.runq[runq_idx], cur, submit_time);
        cur = next;
    }
    xr_proc_local_runq_inc(&worker->p, count);
    xr_worker_refresh_runq_masks(worker);
    xr_sched_metric_inc(runtime, &runtime->sched_stats.inject_pop_batch_count);
    xr_sched_metric_add(runtime, &runtime->sched_stats.inject_pop_count, (uint64_t) count);
    return count;
}

// Enqueue: owner thread lock-free push, overflow to linked list if deque full
void xr_runq_enqueue(XrRunQueue *rq, XrCoroutine *coro) {
    runq_enqueue_at(rq, coro, xr_monotonic_ticks());
}

// Dequeue: owner thread lock-free pop
XrCoroutine *xr_runq_dequeue(XrRunQueue *rq) {
    XrCoroutine *c = xr_steal_queue_pop(&rq->deque);
    return c;
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
        runq_push_stolen(dst, c);
        stolen++;
    }
    return stolen;
}

int xr_runq_steal_direct(XrRunQueue *src, XrRunQueue *dst, int max_steal,
                         XrCoroutine **out_direct) {
    if (out_direct)
        *out_direct = NULL;

    int src_len = xr_steal_queue_size(&src->deque);
    int actual_max = src_len / 2;
    if (actual_max > 32)
        actual_max = 32;
    if (actual_max > max_steal)
        actual_max = max_steal;
    if (actual_max <= 0)
        actual_max = 1;

    XrCoroutine *direct = NULL;
    int stolen = 0;
    for (int i = 0; i < actual_max; i++) {
        XrCoroutine *c = xr_steal_queue_steal(&src->deque);
        if (!c)
            break;
        XR_DCHECK(c->sched_link == NULL, "runq_steal_direct: stolen coro sched_link must be NULL");
        if (direct)
            runq_push_stolen(dst, direct);
        direct = c;
        stolen++;
    }

    if (out_direct)
        *out_direct = direct;
    else if (direct)
        runq_push_stolen(dst, direct);
    return stolen;
}

// ========== Worker Pop / Push ==========

// Worker pop from local queue (Chase-Lev deque)
//
// Design:
// - LIFO slot first only when the gate allows locality to win
// - Priority order is weighted: HIGH(8) > NORMAL(4) > LOW(1)
// - Aging can temporarily raise the effective priority of old waiters
XrCoroutine *xr_worker_pop(XrWorker *worker) {
    XrCoroutine *coro = xr_worker_try_pop_lifo(worker, true);
    if (coro)
        return coro;

    // Reset LIFO polls when falling through to priority queues.
    worker->p.lifo_polls = 0;
    return worker_pop_weighted(worker);
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
        prepare_scheduled_coro(worker, coro, new_prio);
        atomic_store_explicit(&worker->p.lifo_slot, coro, memory_order_relaxed);
        int prev_prio =
            prev ? xr_coro_get_priority(xr_coro_flags_load(prev)) : worker->p.lifo_slot_prio;
        worker->p.lifo_slot_prio = new_prio;
        xr_proc_local_runq_inc(&worker->p, 1);
        if (prev) {
            // Evict previous occupant while preserving local count semantics.
            if (!worker_enqueue_runq_or_inject(worker, prev, prev_prio, true)) {
                xr_proc_local_runq_dec(&worker->p, 1);
            }
        }
        xr_worker_refresh_runq_masks(worker);
        return;
    }
    // Cross-worker: fall back to normal push
    xr_worker_push(worker, coro);
}

int xr_worker_push_lifo_batch(XrWorker *worker, XrCoroutine *first) {
    if (!worker || !first)
        return 0;
    XR_DCHECK(xr_current_worker() == NULL || xr_current_worker() == worker,
              "worker_push_lifo_batch: cross-worker push detected (use inbox)");

    XrCoroutine *prev = atomic_load_explicit(&worker->p.lifo_slot, memory_order_relaxed);
    int prev_prio =
        prev ? xr_coro_get_priority(xr_coro_flags_load(prev)) : worker->p.lifo_slot_prio;

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

    int lifo_prio = xr_coro_get_priority(xr_coro_flags_load(lifo));
    prepare_scheduled_coro(worker, lifo, lifo_prio);
    atomic_store_explicit(&worker->p.lifo_slot, lifo, memory_order_relaxed);
    worker->p.lifo_slot_prio = lifo_prio;

    int local_delta = 1;
    coro = evict_first;
    while (coro) {
        XrCoroutine *next = coro->sched_link;
        coro->sched_link = NULL;
        bool is_prev = coro == prev;
        int priority =
            is_prev ? prev_prio
                    : normalize_coro_priority(xr_coro_get_priority(xr_coro_flags_load(coro)));
        if (worker_enqueue_runq_or_inject(worker, coro, priority, true)) {
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
    xr_worker_refresh_runq_masks(worker);
    return total_count;
}

// Worker push to local queue (Chase-Lev deque)
//
// Design:
// - LOW/NORMAL/HIGH coroutines each use their matching priority queue
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
        xr_proc_local_runq_inc(&worker->p, 1);
        xr_worker_refresh_runq_masks(worker);
    }
    XrRuntime *rt = worker->p.runtime;

    // wake: if no spinner is scanning for work, wake a parked worker.
    // This ensures newly enqueued coros are discovered promptly.
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
        int priority = normalize_coro_priority(xr_coro_get_priority(xr_coro_flags_load(coro)));
        if (worker_enqueue_runq_or_inject(worker, coro, priority, true)) {
            local_count++;
        }
        total_count++;
        coro = next;
    }

    if (local_count > 0) {
        xr_proc_local_runq_inc(&worker->p, local_count);
        xr_worker_refresh_runq_masks(worker);
        XrRuntime *rt = worker->p.runtime;
        if (rt && atomic_load_explicit(&rt->spinning_count, memory_order_relaxed) == 0) {
            wake_idle_worker(rt);
        }
    }
    return total_count;
}
