/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xproc.c - Processor (P) implementation for P/M split scheduler
 *
 * KEY CONCEPT:
 *   P lifecycle: init -> acquire by M -> release -> idle pool -> acquire again
 *   P owns all scheduling resources; M provides the OS thread to run on.
 */

#include "xproc.h"
#include "../base/xchecks.h"
#include "xmachine.h"
#include "xworker.h"
#include "xtimer_wheel.h"
#include "xbalance.h"
#include <stdlib.h>
#include <string.h>

// ========== P Lifecycle ==========

void xr_proc_init(XrProc *p, int id, struct XrRuntime *runtime) {
    XR_DCHECK(p != NULL, "proc_init: NULL proc");
    XR_DCHECK(runtime != NULL, "proc_init: NULL runtime");
    memset(p, 0, sizeof(XrProc));
    p->id = id;
    p->runtime = runtime;
    atomic_store(&p->status, P_IDLE);
    atomic_store(&p->current_m, NULL);

    // Initialize run queues
    for (int i = 0; i < XR_RUNQ_COUNT; i++) {
        xr_runq_init(&p->runq[i]);
    }

    // MPSC inbox
    xr_mpsc_init(&p->inbox);

    // Channel wake command queue.
    xr_chan_wake_queue_init(&p->chan_wake_queue);

    // Timer wheel created later (needs runtime fully initialized)
    p->timer_wheel = NULL;
    p->last_timer_tick = 0;

    // Load balancing
    p->check_balance_reds = XR_CALL_CHECK_BALANCE_REDS;
    for (int i = 0; i < XR_RUNQ_COUNT; i++) {
        p->runq_reds[i] = 0;
        p->runq_max_len[i] = 0;
    }

    // Local coroutine pool
    p->local_free_list = NULL;
    p->local_free_count = 0;

    // Blocked queue
    memset(p->blocked_buckets, 0, sizeof(p->blocked_buckets));
    p->blocked_head = NULL;
    p->blocked_tail = NULL;
    p->blocked_count = 0;
    p->select_waiter_count = 0;

    // Per-worker canceled timer node freelist.
    p->cancel_node_free = NULL;
    p->cancel_node_free_count = 0;

    // Continuation stealing deque (small: scope depth is typically shallow)
    xr_steal_queue_init(&p->cont_deque, 64);
    p->yield_streak = 0;

    // Statistics
    p->stats.executed_count = 0;
    p->stats.stolen_count = 0;
    p->stats.yielded_count = 0;
    p->stats.cont_steal_count = 0;
    // RNG seed set later when runtime is fully initialized

    p->idle_link = NULL;
}

void xr_proc_destroy(XrProc *p) {
    if (!p) return;

    // Destroy run queues
    for (int i = 0; i < XR_RUNQ_COUNT; i++) {
        xr_runq_destroy(&p->runq[i]);
    }

    // Destroy timer wheel
    if (p->timer_wheel) {
        xr_timer_wheel_destroy(p->timer_wheel);
        p->timer_wheel = NULL;
    }

    // Destroy continuation deque
    xr_steal_queue_destroy(&p->cont_deque);

    // Destroy channel wake command queue
    xr_chan_wake_queue_destroy(&p->chan_wake_queue);

    // Drain canceled timer node freelist.
    {
        struct XrCanceledTimerNode *n = p->cancel_node_free;
        while (n) {
            struct XrCanceledTimerNode *next = n->next;
            xr_free(n);
            n = next;
        }
        p->cancel_node_free = NULL;
        p->cancel_node_free_count = 0;
    }
}

// ========== P/M Binding ==========

void xr_acquirep(XrMachine *m, XrProc *p) {
    XR_DCHECK(m != NULL, "acquirep: NULL machine");
    XR_DCHECK(p != NULL, "acquirep: NULL proc");
    XR_DCHECK(atomic_load(&m->current_p) == NULL, "acquirep: M already has a P");
    XR_DCHECK(atomic_load(&p->current_m) == NULL, "acquirep: P already bound to an M");

    atomic_store(&m->current_p, p);
    atomic_store(&p->current_m, m);
    atomic_store(&p->status, P_RUNNING);
}

XrProc *xr_releasep(XrMachine *m) {
    XR_DCHECK(m != NULL, "releasep: NULL machine");
    XrProc *p = atomic_exchange(&m->current_p, NULL);
    XR_DCHECK(p != NULL, "releasep: M has no P to release");

    atomic_store(&p->current_m, NULL);
    atomic_store(&p->status, P_IDLE);
    return p;
}

void xr_handoffp(XrProc *p) {
    XR_DCHECK(p != NULL, "handoffp: NULL proc");

    // Delegate to startm: wake idle M or create new M to run this P
    xr_startm(p, false);
}

// ========== P Run Queue Operations ==========

XrCoroutine *xr_proc_pop(XrProc *p) {
    if (!p) return NULL;

    // 1. HIGH queue first
    XrCoroutine *c = xr_steal_queue_pop(&p->runq[1].deque);
    if (c) {
        return c;
    }

    // 2. NORMAL overflow (delayed LOW coroutines ready to run)
    XrRunQueue *nq = &p->runq[0];
    if (nq->overflow_first && --nq->overflow_first->schedule_count <= 0) {
        c = nq->overflow_first;
        nq->overflow_first = c->sched_link;
        if (!nq->overflow_first) nq->overflow_last = NULL;
        nq->overflow_len--;
        c->sched_link = NULL;
        return c;
    }

    // 3. NORMAL deque
    c = xr_steal_queue_pop(&nq->deque);
    if (c && xr_coro_get_priority(xr_coro_flags_load(c)) == CORO_PRIORITY_LOW
        && c->schedule_count > 1) {
        // LOW coroutine with remaining delay: put to overflow
        c->schedule_count--;
        c->sched_link = NULL;
        if (nq->overflow_last) nq->overflow_last->sched_link = c;
        else nq->overflow_first = c;
        nq->overflow_last = c;
        nq->overflow_len++;
        return xr_proc_pop(p);  // Retry
    }
    return c;
}

void xr_proc_push(XrProc *p, XrCoroutine *coro) {
    if (!p || !coro) return;

    int priority = xr_coro_get_priority(xr_coro_flags_load(coro));
    if (priority < 0) priority = 0;
    if (priority >= XR_CORO_PRIORITY_COUNT) priority = XR_CORO_PRIORITY_COUNT - 1;

    int runq_idx;
    if (priority == CORO_PRIORITY_LOW) {
        coro->schedule_count = XR_RESCHEDULE_LOW;
        runq_idx = 0;
    } else if (priority == CORO_PRIORITY_NORMAL) {
        coro->schedule_count = 1;
        runq_idx = 0;
    } else {
        coro->schedule_count = 1;
        runq_idx = 1;
    }

    xr_runq_enqueue(&p->runq[runq_idx], coro);
}

// ========== Idle P Management ==========

// Idle P list is a lock-free Treiber stack.
// ABA: XrProc is 1:1 with Worker and never freed during runtime lifetime.
// idle_p_count is kept as a separate atomic for heuristics only.
XrProc *xr_get_idle_p(struct XrRuntime *runtime) {
    if (!runtime) return NULL;

    for (int retry = 0; retry < 8; retry++) {
        XrProc *head = atomic_load_explicit(&runtime->idle_p_head, memory_order_acquire);
        if (!head) return NULL;
        XrProc *next = head->idle_link;
        if (atomic_compare_exchange_weak_explicit(
                &runtime->idle_p_head, &head, next,
                memory_order_acq_rel, memory_order_acquire)) {
            head->idle_link = NULL;
            atomic_fetch_sub_explicit(&runtime->idle_p_count, 1, memory_order_relaxed);
            return head;
        }
    }
    return NULL;
}

void xr_put_idle_p(struct XrRuntime *runtime, XrProc *p) {
    if (!runtime || !p) return;

    atomic_store(&p->status, P_IDLE);
    atomic_store(&p->current_m, NULL);

    XrProc *head;
    do {
        head = atomic_load_explicit(&runtime->idle_p_head, memory_order_relaxed);
        p->idle_link = head;
    } while (!atomic_compare_exchange_weak_explicit(
        &runtime->idle_p_head, &head, p,
        memory_order_release, memory_order_relaxed));
    atomic_fetch_add_explicit(&runtime->idle_p_count, 1, memory_order_relaxed);
}
