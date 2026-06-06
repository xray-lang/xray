/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xworker_sched.c - Worker scheduling loop + park/unpark + stealing + I/O poll
 *
 * KEY CONCEPT:
 *   The main scheduler plumbing, including:
 *     - Idle worker stack (O(1) wake-up source, lock-free Treiber stack)
 *     - MPSC inbox enqueue (cross-worker delivery with Dekker fence)
 *     - I/O poll sources (netpoll, async pool completions, timer wheel)
 *     - Cross-worker migration (try_immigrate / xr_try_emigrate bridge)
 *     - Per-worker sleep timers (sleep() / timeout())
 *     - worker_park / worker_unpark (futex-based sleep with last-spinner
 *       notify protocol)
 *     - worker_loop (main scheduling loop, work-stealing, spinning)
 *
 * RELATED:
 *   - xworker_exec.c: worker_exec_with_cont_stealing (execution core)
 *   - xworker_handoff.c: handoff thread uses worker_poll_sources
 *   - xworker_blocked.c: unblock select path called from sleep callback
 */
#include "xworker_internal.h"
#include "../base/xchecks.h"
#include "xscheduler_policy.h"
#include "../base/xlog.h"
#include "xsched_trace.h"
#include "../os/os_thread.h"
#include <time.h>

// ========== Lock-free Idle Worker Stack ==========
//
// Treiber stack of parked XrMachine*. Replaces the prior
// fixed-size int[] + sched_lock design. Each M chains through its
// idle_link pointer, which is also used (mutually exclusively) by the
// idle_m_head stack — see xworker.h for the sharing invariant.
//
// SEMANTICS:
//   - idle_worker_push : worker about to park; pushes its bound M.
//   - idle_worker_pop  : pop any one parked M for wake-up. Returns NULL if
//                         empty. The popped M might already be on the way
//                         to running (see self-wake path), but a redundant
//                         futex wake is cheap and correct.
//   - idle_worker_count : approximate count for heuristics. Kept in a
//                         separate atomic (may lag the list by CAS race
//                         windows but consumers tolerate that).
//
// REMOVED: idle_worker_remove() — previously called on self-wake. In a
// singly-linked Treiber stack, O(1) removal of an arbitrary element is
// impossible without pointer-swap hazard. We rely instead on the wake
// path being idempotent: a self-woken worker simply leaves the stack on
// its next actual park (it CAS-pops when state != M_PARKING and
// re-pushes normally).

static void idle_worker_push(XrRuntime *rt, XrMachine *m) {
    XR_DCHECK(m != NULL, "idle_worker_push: NULL machine");

    // Idempotency guard: skip if this M is already in the list. Prevents
    // the double-push cycle described in XrMachine::in_idle_worker_list.
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&m->in_idle_worker_list, &expected, true,
                                                 memory_order_acq_rel, memory_order_relaxed)) {
        return;
    }

    XrMachine *head;
    do {
        head = atomic_load_explicit(&rt->idle_worker_list, memory_order_relaxed);
        m->idle_link = head;
    } while (!atomic_compare_exchange_weak_explicit(&rt->idle_worker_list, &head, m,
                                                    memory_order_release, memory_order_relaxed));
    atomic_fetch_add_explicit(&rt->idle_worker_count, 1, memory_order_relaxed);
    XrProc *p = atomic_load_explicit(&m->current_p, memory_order_relaxed);
    if (p) {
        xr_runtime_set_idle_worker_bit(rt, p->id, true);
    }
}

static XrMachine *idle_worker_pop(XrRuntime *rt) {
    for (int retry = 0; retry < 8; retry++) {
        XrMachine *head = atomic_load_explicit(&rt->idle_worker_list, memory_order_acquire);
        if (!head)
            return NULL;
        XrMachine *next = head->idle_link;
        if (atomic_compare_exchange_weak_explicit(&rt->idle_worker_list, &head, next,
                                                  memory_order_acq_rel, memory_order_acquire)) {
            head->idle_link = NULL;
            atomic_store_explicit(&head->in_idle_worker_list, false, memory_order_release);
            atomic_fetch_sub_explicit(&rt->idle_worker_count, 1, memory_order_relaxed);
            XrProc *p = atomic_load_explicit(&head->current_p, memory_order_relaxed);
            if (p) {
                xr_runtime_set_idle_worker_bit(rt, p->id, false);
            }
            return head;
        }
    }
    return NULL;
}

// Wake one idle worker (lock-free pop + unpark).
// No-op if the stack is empty.
void wake_idle_worker(XrRuntime *rt) {
    XrMachine *m = idle_worker_pop(rt);
    if (!m)
        return;
    atomic_store_explicit(&m->park_state, XR_PARK_WOKEN, memory_order_release);
    xr_park_futex_wake(&m->park_state);
}

void wake_idle_workers(XrRuntime *rt, int max_wakes) {
    if (!rt || max_wakes <= 0)
        return;

    int idle_count = atomic_load_explicit(&rt->idle_worker_count, memory_order_relaxed);
    if (idle_count <= 0)
        return;
    int wakes = max_wakes < idle_count ? max_wakes : idle_count;
    if (wakes > rt->worker_count)
        wakes = rt->worker_count;

    for (int i = 0; i < wakes; i++) {
        wake_idle_worker(rt);
    }
}

// Public API: wake one idle worker (for use by xcoro.c etc.)
void xr_runtime_wake_idle_worker(XrRuntime *runtime) {
    if (runtime)
        wake_idle_worker(runtime);
}

// Wake Worker (simple futex wake via park_state)
void worker_unpark(XrWorker *worker) {
    XrMachine *m = worker->m;
    if (!m)
        return;  // M detached during handoff
    atomic_store_explicit(&m->park_state, XR_PARK_WOKEN, memory_order_release);
    xr_park_futex_wake(&m->park_state);
}

void xr_runtime_wake_worker(XrRuntime *runtime, int worker_id) {
    if (!runtime || worker_id < 0 || worker_id >= runtime->worker_count)
        return;
    worker_unpark(&runtime->workers[worker_id]);
}

// Enqueue coro to target worker's inbox with full Dekker synchronization + wake.
// This is the single correct path for all cross-worker inbox delivery.
void xr_worker_inbox_enqueue(XrRuntime *runtime, int target_id, XrCoroutine *coro) {
    XR_DCHECK(runtime != NULL, "inbox_enqueue: NULL runtime");
    XR_DCHECK(coro != NULL, "inbox_enqueue: NULL coro");
    XR_DCHECK(target_id >= 0 && target_id < runtime->worker_count,
              "inbox_enqueue: target_id out of range");

    XrWorker *target = &runtime->workers[target_id];

    // Step 1: Lock-free MPSC push
    xr_mpsc_push(&target->p.inbox, coro);
    atomic_fetch_add_explicit(&runtime->total_inbox_len, 1, memory_order_relaxed);

    // Step 2: Dekker fence — ensure inbox push is visible before reading
    // target state.  Pairs with seq_cst store of M_PARKING in worker_park.
    atomic_thread_fence(memory_order_seq_cst);

    // Step 3: Wake target worker if parked
    if (atomic_load(&target->m->state) == M_PARKING) {
        worker_unpark(target);
    }
}

void xr_worker_inbox_enqueue_batch(XrRuntime *runtime, int target_id, XrCoroutine *first,
                                   XrCoroutine *last, int count) {
    XR_DCHECK(runtime != NULL, "inbox_enqueue_batch: NULL runtime");
    XR_DCHECK(first != NULL, "inbox_enqueue_batch: NULL first");
    XR_DCHECK(last != NULL, "inbox_enqueue_batch: NULL last");
    XR_DCHECK(count > 0, "inbox_enqueue_batch: invalid count");
    XR_DCHECK(target_id >= 0 && target_id < runtime->worker_count,
              "inbox_enqueue_batch: target_id out of range");

    XrWorker *target = &runtime->workers[target_id];
    xr_mpsc_push_batch(&target->p.inbox, first, last);
    atomic_fetch_add_explicit(&runtime->total_inbox_len, count, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load(&target->m->state) == M_PARKING) {
        worker_unpark(target);
    }
}

// ========== Migration ==========

// try_immigrate - Pull from a previously selected high-load worker.
static void try_immigrate(XrWorker *worker) {
    XrRuntime *runtime = worker->p.runtime;
    XrMigrationPath *mp = &runtime->migration_paths[worker->p.id];
    int source_id = mp->runq.target_worker;
    if (source_id < 0 || source_id >= runtime->worker_count)
        return;

    if (xr_runq_len(&worker->p.runq) > 0)
        return;

    XrWorker *source = &runtime->workers[source_id];
    if (xr_steal_queue_size(&source->p.runq.deque) <= 0)
        return;
    int stolen = xr_runq_steal(&source->p.runq, &worker->p.runq, 50);
    if (stolen > 0) {
        xr_proc_local_runq_dec(&source->p, stolen);
        xr_proc_local_runq_inc(&worker->p, stolen);
        xr_worker_refresh_runq_masks(source);
        xr_worker_refresh_runq_masks(worker);
        mp->runq.target_worker = -1;
    }
}

// ========== Shared Scheduling Helpers ==========
// Used by both worker_loop and xr_handoff_thread_entry to avoid duplication.

// Drain MPSC inbox into P's local run queue, maintaining global inbox counter.
void worker_drain_inbox(XrWorker *worker) {
    XrCoroutine *list = xr_mpsc_drain(&worker->p.inbox);
    int count = 0;
    XrCoroutine *ready_first = NULL;
    XrCoroutine *ready_last = NULL;
    while (list) {
        XrCoroutine *next = list->sched_link;
        list->sched_link = NULL;
        if (list->ext && list->ext->wait_bucket && list->ext->wait_bucket_owner == worker->p.id) {
            xr_worker_unblock(worker, list);
        }
        if (xr_coro_flags_has(list, XR_CORO_FLG_DONE)) {
            XrSelectWait *sw = xr_coro_select_wait(list);
            if (sw) {
                xr_worker_unblock_select(worker, list);
                xr_select_wait_cancel(sw);
                xr_coro_clear_select_wait(list);
            }
            list = next;
            count++;
            continue;
        }
        if (ready_last) {
            ready_last->sched_link = list;
        } else {
            ready_first = list;
        }
        ready_last = list;
        list = next;
        count++;
    }
    if (ready_first) {
        (void) xr_worker_push_batch(worker, ready_first);
    }
    if (count > 0 && worker->p.runtime) {
        worker->p.stats.inbox_drain_count++;
        atomic_fetch_sub_explicit(&worker->p.runtime->total_inbox_len, count, memory_order_relaxed);
    }
}

int worker_pull_inject(XrWorker *worker, int max_count) {
    if (!worker || max_count <= 0)
        return 0;
    XrRuntime *runtime = worker->p.runtime;
    if (!runtime)
        return 0;

    if (!atomic_load_explicit(&runtime->injectq_nonempty, memory_order_acquire))
        return 0;

    int total = xr_injectq_pop_batch(runtime, worker, max_count);
    if (total > 0) {
        worker->p.stats.inject_pull_count += (uint64_t) total;
    }
    return total;
}

static void worker_io_ready_append(XrCoroutine **first, XrCoroutine **last, XrCoroutine *coro) {
    XR_DCHECK(first != NULL, "io_ready_append: NULL first");
    XR_DCHECK(last != NULL, "io_ready_append: NULL last");
    XR_DCHECK(coro != NULL, "io_ready_append: NULL coro");

    coro->sched_link = NULL;
    if (*last) {
        (*last)->sched_link = coro;
    } else {
        *first = coro;
    }
    *last = coro;
}

static XrCoroutine *worker_claim_io_ready_coro(XrWorker *worker, XrCoroutine *coro) {
    (void) worker;
    if (!coro)
        return NULL;
    if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE))
        return NULL;
    SCHED_TRACE_CORO(worker, coro, "io_wake");
    if (!xr_coro_claim_wake(coro))
        return NULL;
    xr_coro_resume_store(coro, XR_RESUME_IO_READY);
    coro->sched_link = NULL;
    return coro;
}

static XrCoroutine *worker_claim_io_ready_list(XrWorker *worker, XrCoroutine *head) {
    XrCoroutine *claimed_first = NULL;
    XrCoroutine *claimed_last = NULL;
    XrCoroutine *io_coro = head;
    while (io_coro) {
        XrCoroutine *next = io_coro->sched_link;
        io_coro->sched_link = NULL;
        XrCoroutine *claimed = worker_claim_io_ready_coro(worker, io_coro);
        if (claimed)
            worker_io_ready_append(&claimed_first, &claimed_last, claimed);
        io_coro = next;
    }
    return claimed_first;
}

// Poll all I/O sources and drain MPSC inbox into P's local run queue.
// Returns a fast-path IO coroutine (single wakeup with affinity to this
// worker) that the caller should execute directly, bypassing the queue.
// Returns NULL when no fast-path candidate is available.
XrCoroutine *worker_poll_sources(XrWorker *worker) {
    XrRuntime *runtime = worker->p.runtime;
    XrProc *p = &worker->p;
    XrCoroutine *fast_coro = NULL;
    int total_io_events = 0;

    // ===== Fast path: per-worker local poll (zero contention) =====
    if (p->local_poll.poll_fd >= 0) {
        XrReadyList local_ready = {0};
        xr_local_poll_events(&p->local_poll, 0, &local_ready);
        total_io_events += local_ready.count;
        XrCoroutine *claimed = worker_claim_io_ready_list(worker, local_ready.head);
        if (claimed) {
            (void) xr_worker_push_lifo_batch(worker, claimed);
        }
    }

    // ===== Shared netpoll (all workers, handles unbound fds) =====
    {
        XrReadyList ready = xr_netpoll_poll(&runtime->netpoll, 0);
        total_io_events += ready.count;

        // Zero-copy fast path: single IO wakeup targeting this worker
        // — skip queue push/pop, return directly for execution.
        // Thread-locked coros must match this worker to use the fast path.
        if (ready.count == 1 && ready.head) {
            XrCoroutine *io_coro = ready.head;
            int aff = xr_coro_wake_target_id(io_coro);
            if (aff == p->id) {
                fast_coro = worker_claim_io_ready_coro(worker, io_coro);
                goto after_netpoll;
            }
        }

        // Normal path: enqueue all ready coroutines to LIFO slot.
        XrCoroutine *claimed = worker_claim_io_ready_list(worker, ready.head);
        if (claimed) {
            (void) xr_worker_push_lifo_batch(worker, claimed);
        }
    }

after_netpoll:
    // Adaptive poll_skip feedback: EWMA of I/O event frequency.
    // Decay 7/8: io_ewma = io_ewma * 7/8 + sample * 1/8
    // Sample: 256 if events, 0 if none. Range [0, 256].
    p->io_poll_ewma = p->io_poll_ewma - (p->io_poll_ewma >> 3) + (total_io_events > 0 ? 32 : 0);

    // Async thread pool completions
    if (runtime->async_pool) {
        xr_async_check_ready(runtime->async_pool, p->id);
    }

    // Advance timers (callbacks wake sleeping coroutines directly).
    // Loop until all expired timers are drained: the timer wheel yields
    // after ~100 timeouts per bump call (XR_TW_COST_TIMEOUT=100), so a
    // burst of 10000 timers requires ~100 iterations.
    int64_t now = xr_monotonic_ticks();
    if (p->timer_wheel && (xr_timer_cancel_pending(p->timer_wheel) || now > p->last_timer_tick)) {
        int32_t inbox_before =
            atomic_load_explicit(&runtime->total_inbox_len, memory_order_relaxed);
        int timer_passes = 0;
        do {
            xr_bump_timers(p->timer_wheel, now);
            timer_passes++;
        } while (p->timer_wheel->yield_slot != XR_TW_SLOT_INACTIVE);
        p->stats.timer_bump_count += (uint64_t) timer_passes;
        if (timer_passes > 1) {
            p->stats.timer_burst_count++;
        }
        if (now > p->last_timer_tick) {
            p->last_timer_tick = now;
        }
        // After timer batch: wake idle workers to help process burst.
        // Wake count = min(new_items, idle_workers) — no point waking
        // more workers than available work or idle capacity.
        int32_t new_items =
            atomic_load_explicit(&runtime->total_inbox_len, memory_order_relaxed) - inbox_before;
        if (new_items > 0) {
            int idle_count =
                atomic_load_explicit(&runtime->idle_worker_count, memory_order_relaxed);
            if (idle_count < 0)
                idle_count = 0;
            int wakes = new_items < idle_count ? new_items : idle_count;
            if (wakes < 1)
                wakes = 1;
            wake_idle_workers(runtime, wakes);
        }
    }

    // Drain deferred free queue (cross-worker PollDesc cleanup)
    // Must run after timer bump so zombie timers are already cleaned up.
    xr_netpoll_drain_deferred(&runtime->netpoll, p);

    // Drain MPSC inbox
    worker_drain_inbox(worker);
    worker_pull_inject(worker, XR_INJECT_POP_BATCH);

    // Drain channel wake command queue (ownership-safe routing).
    // Commands arrive from remote workers that need us to wake our local
    // blocked waiters on specific channels.
    xr_worker_drain_chan_wake_queue(worker);

    return fast_coro;
}

// ========== Per-Worker Sleep Timer ==========

// Per-Worker sleep timer callback (lock-free wake)
//
// Two scenarios:
// 1. Normal sleep: directly wake coroutine
// 2. Select wait: use CAS to prevent duplicate wake, remove from blocked queue
static void worker_sleep_timeout_callback(void *arg) {
    XrCoroutine *coro = (XrCoroutine *) arg;
    if (!coro)
        return;

    XR_DBG_TIMER(
        "Worker callback triggered: coro=%d, timer_active=%d", coro->id,
        coro->ext ? (int) atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed) : 0);

    // Check if timer was cancelled (coroutine recycle cancels timer)
    if (!coro->ext || !atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed)) {
        return;
    }

    // Note: coro_gc may be NULL legitimately (lazy allocation).
    // Do NOT check coro_gc here — it's not a valid recycle indicator.

    // Check if coroutine already done (avoid waking completed coroutine)
    if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) {
        xr_timer_wait_token_cancel(&coro->ext->wait.timer_token);
        atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
        return;
    }

    // Get Runtime (from current Worker)
    XrWorker *worker = xr_current_worker();
    XrRuntime *runtime = worker ? worker->p.runtime : NULL;
    if (!runtime)
        return;

    // Check if runtime is running (avoid waking during exit)
    if (!atomic_load(&runtime->running)) {
        xr_timer_wait_token_cancel(&coro->ext->wait.timer_token);
        atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
        return;
    }

    // Check if select wait (use CAS to prevent duplicate wake)
    XrSelectWait *sw = xr_coro_select_wait(coro);
    if (sw) {
        bool expected = false;
        if (!atomic_compare_exchange_strong(&sw->triggered, &expected, true)) {
            // Already woken by another channel, ignore this timer
            xr_timer_wait_token_cancel(&coro->ext->wait.timer_token);
            atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
            return;
        }
        if (!xr_coro_claim_wake(coro)) {
            xr_timer_wait_token_cancel(&coro->ext->wait.timer_token);
            atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
            return;
        }
        xr_timer_wait_token_fire(&coro->ext->wait.timer_token);
        xr_select_wait_timeout(sw);
        atomic_store_explicit(&sw->selected_index, -1, memory_order_release);
        atomic_store_explicit(&sw->selected_status, XR_RESUME_TIMEOUT, memory_order_release);
        // Remove from blocked queue
        xr_worker_unblock_select(worker, coro);
    } else {
        int wait_reason = xr_coro_get_wait_reason(xr_coro_flags_load(coro));
        XrChannel *ch = (coro->ext) ? (XrChannel *) atomic_load_explicit(&coro->ext->wait_channel,
                                                                         memory_order_acquire)
                                    : NULL;
        if (!xr_coro_claim_wake(coro)) {
            xr_timer_wait_token_cancel(&coro->ext->wait.timer_token);
            atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
            return;
        }
        xr_timer_wait_token_fire(&coro->ext->wait.timer_token);
        if (wait_reason == (XR_CORO_WAIT_IO >> XR_CORO_WAIT_SHIFT)) {
            XrCoroWaitState *wait = xr_coro_wait_state(coro);
            if (wait)
                xr_io_wait_token_timeout(&wait->io_token);
        }
        if (wait_reason == (XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT)) {
            XrCoroWaitState *wait = xr_coro_wait_state(coro);
            if (wait)
                xr_await_wait_token_timeout(&wait->await_token);
        }
        // Check if waiting on channel (sendTimeout/recvTimeout)
        if (ch) {
            // Remove from channel wait queue
            xr_channel_remove_waiter(ch, coro);
            xr_channel_wait_token_timeout(&coro->ext->chan_wait_token);
            atomic_store_explicit(&coro->ext->wait_channel, NULL, memory_order_relaxed);
        }
        // Remove from blocked queue (unified via xr_worker_unblock)
        xr_worker_unblock(worker, coro);
    }

    atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);

    // Set resume status to timeout
    xr_coro_resume_store(coro, XR_RESUME_TIMEOUT);

    // Enqueue to coroutine's target worker inbox (with Dekker sync + wake).
    // Respects Coro.lockThread(): locked coros return to their locked worker.
    int target_id = xr_coro_wake_target_id(coro);
    if (target_id < 0 || target_id >= runtime->worker_count) {
        target_id = 0;
    }
    xr_worker_inbox_enqueue(runtime, target_id, coro);
}

// Add sleep timer to Worker's Timer Wheel (lock-free, owner-private)
void xr_worker_add_sleep_timer(XrWorker *worker, XrCoroutine *coro, int64_t delay_ms) {
    if (!worker || !coro || delay_ms < 0)
        return;

    XrTimerWheel *tw = worker->p.timer_wheel;
    if (!tw)
        return;

    // Use coroutine's ext timer node (lazy-alloc ext on first sleep)
    XrCoroExt *text = xr_coro_ensure_ext(coro);
    if (!text)
        return;
    XrTWheelTimer *timer = &text->timer;

    // Initialize timer node
    timer->prev = NULL;
    timer->next = NULL;
    atomic_store_explicit(&timer->cancel_next, NULL, memory_order_relaxed);
    timer->slot = XR_TW_SLOT_INACTIVE;

    // Increment sequence number (prevent stale notifications)
    atomic_fetch_add(&text->timer_seq, 1);

    // Calculate timeout position
    int64_t timeout_pos = xr_monotonic_ticks() + delay_ms;
    int wait_reason = xr_coro_get_wait_reason(xr_coro_flags_load(coro));
    xr_timer_wait_token_prepare(&text->wait.timer_token, worker->p.id, wait_reason, timeout_pos);

    // Mark timer active and record ownership
    atomic_store_explicit(&text->timer_active, true, memory_order_relaxed);
    text->timer_wheel_owner = worker->p.id;  // Record which worker owns this timer

    // Set timer (must be called from owner worker)
    XR_DBG_TIMER("Worker set_timer: tw=%p, timeout_pos=%lld, tw->pos=%lld, owner=%d", (void *) tw,
                 (long long) timeout_pos, (long long) tw->pos, worker->p.id);
    if (!xr_twheel_set_timer(tw, timer, worker_sleep_timeout_callback, coro, timeout_pos)) {
        atomic_store_explicit(&text->timer_active, false, memory_order_relaxed);
        xr_timer_wait_token_finish(&text->wait.timer_token);
        return;
    }
    xr_timer_wait_token_commit(&text->wait.timer_token);
}

// Cancel timer - handles cross-worker case via async queue
//
// Design:
// - If current worker owns the timer: direct cancel (lock-free)
// - If other worker owns the timer: push to owner's cancel stack
void xr_worker_cancel_timer(XrWorker *current_worker, XrCoroutine *coro) {
    if (!coro || !coro->ext)
        return;
    if (!atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed))
        return;

    int owner_id = coro->ext->timer_wheel_owner;
    XrRuntime *runtime = current_worker ? current_worker->p.runtime : NULL;
    if (!runtime)
        return;

    // Get owner worker's timer wheel
    if (owner_id < 0 || owner_id >= runtime->worker_count) {
        // Invalid owner, just mark inactive
        xr_timer_wait_token_cancel(&coro->ext->wait.timer_token);
        atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
        return;
    }

    XrTimerWheel *owner_tw = runtime->workers[owner_id].p.timer_wheel;
    if (!owner_tw) {
        xr_timer_wait_token_cancel(&coro->ext->wait.timer_token);
        atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
        return;
    }

    // Check if current worker is the owner
    if (current_worker && current_worker->p.id == owner_id) {
        // Same worker: direct cancel (lock-free, no mutex needed)
        xr_twheel_cancel_timer(owner_tw, &coro->ext->timer);
        xr_timer_wait_token_cancel(&coro->ext->wait.timer_token);
        atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
        XR_DBG_TIMER("Timer canceled locally: coro=%d, owner=%d", coro->id, owner_id);
    } else {
        // Cross-worker: push to owner's cancel stack.
        xr_timer_queue_cancel(owner_tw, &coro->ext->timer);
        xr_timer_wait_token_cancel(&coro->ext->wait.timer_token);
        atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
        XR_DBG_TIMER("Timer cancel queued: coro=%d, owner=%d, current=%d", coro->id, owner_id,
                     current_worker ? current_worker->p.id : -1);
    }
}

// ========== Worker Park / Work Detection ==========

// Check for runnable work through approximate runtime bitsets. False positives
// are fine: the caller will re-enter the scheduler and discover there is no
// work. Timers use timer_p_mask to shorten timed park instead of reporting
// runnable work before their deadline.
static bool runtime_has_work(XrRuntime *runtime) {
    if (atomic_load_explicit(&runtime->injectq_nonempty, memory_order_acquire))
        return true;
    if (atomic_load_explicit(&runtime->nonempty_p_mask, memory_order_acquire) != 0)
        return true;
    if (atomic_load_explicit(&runtime->total_inbox_len, memory_order_relaxed) > 0)
        return true;
    return false;
}

// Worker Park with last-spinner-notify protocol
//
// Key protocol:
// 1. Decrement spinning_count first (store), then StoreLoad barrier
// 2. Recheck all work sources (load) — prevents losing notifications
// 3. Last spinner sets needspinning flag so parked workers can react
// 4. Condition variable for final sleep with timer-based timeout
static void worker_park(XrWorker *worker) {
    XrRuntime *runtime = worker->p.runtime;

    // Step 1: exit spinning state before parking
    if (worker->m->spinning) {
        worker->m->spinning = false;
        int prev = atomic_fetch_sub(&runtime->spinning_count, 1);
        if (prev <= 0) {
            atomic_store(&runtime->spinning_count, 0);
        }

        // Step 2: StoreLoad barrier then recheck work
        atomic_thread_fence(memory_order_seq_cst);

        if (prev == 1) {
            // Last spinner protocol: we were the last spinning worker.
            // Must do a comprehensive check to avoid losing notifications.
            if (runtime_has_work(runtime)) {
                worker->m->spinning = true;
                atomic_fetch_add(&runtime->spinning_count, 1);
                return;
            }
            // Signal parked workers that a spinner is needed
            atomic_store_explicit(&runtime->needspinning, 1, memory_order_release);
        } else if (runtime_has_work(runtime)) {
            worker->m->spinning = true;
            atomic_fetch_add(&runtime->spinning_count, 1);
            return;
        }
    }

    // Push to idle stack before parking (O(1) wake-up source).
    // idle_worker_push is idempotent via XrMachine::in_idle_worker_list,
    // so a self-woken M that is still in the list will simply no-op here.
    idle_worker_push(runtime, worker->m);

    atomic_store(&worker->m->state, M_PARKING);
    atomic_fetch_sub(&runtime->active_workers, 1);

    // Check needspinning flag (set by last spinner).
    // We do NOT pop ourselves from the idle stack here; the next
    // wake_idle_worker CAS-pop may harmlessly wake us again (no-op since
    // state != M_PARKING). This matches the simplification that removed
    // the O(n) idle_worker_remove helper.
    if (atomic_exchange_explicit(&runtime->needspinning, 0, memory_order_acquire)) {
        atomic_fetch_add(&runtime->active_workers, 1);
        atomic_store(&worker->m->state, M_IDLE);
        worker->m->spinning = true;
        atomic_fetch_add(&runtime->spinning_count, 1);
        return;
    }

    // Last-chance IO poll before sleep (avoids unnecessary futex wait)
    {
        XrReadyList ready = xr_netpoll_poll(&runtime->netpoll, 0);
        if (ready.count > 0) {
            XrCoroutine *claimed = worker_claim_io_ready_list(worker, ready.head);
            if (!claimed)
                goto park_recheck_work;
            (void) xr_worker_push_lifo_batch(worker, claimed);
            // Found IO work, abort park. M remains in idle_worker_list until
            // a later wake_idle_worker pops it — tolerable because the wake
            // is idempotent and in_idle_worker_list guards re-entry.
            atomic_fetch_add(&runtime->active_workers, 1);
            atomic_store(&worker->m->state, M_IDLE);
            return;
        }
    }

park_recheck_work:
    // Recheck for work before sleeping
    if (!runtime_has_work(runtime) && atomic_load(&runtime->running)) {
        // Adaptive timeout: IO-heavy workloads use shorter sleep (faster response),
        // CPU-heavy workloads use longer sleep (less futex overhead).
        // io_poll_ewma > 128 means >50% of polls had IO events.
        int64_t timeout_ms;
        if (worker->p.io_poll_ewma > 128) {
            timeout_ms = 2;  // IO heavy: wake quickly for IO events
        } else {
            timeout_ms = 10;  // CPU heavy: longer sleep, less overhead
        }
        if (atomic_load_explicit(&runtime->timer_p_mask, memory_order_acquire) != 0 &&
            timeout_ms > 2) {
            timeout_ms = 2;
        }

        // Timer-aware: clamp timeout to next timer expiry
        if (worker->p.timer_wheel && xr_timer_cancel_pending(worker->p.timer_wheel)) {
            timeout_ms = 1;
        } else if (worker->p.timer_wheel) {
            int64_t next = xr_check_next_timeout_time(worker->p.timer_wheel);
            int64_t now_ticks = xr_monotonic_ticks();
            if (next > now_ticks) {
                int64_t delta = next - now_ticks;
                if (delta < timeout_ms)
                    timeout_ms = delta;
            } else {
                timeout_ms = 1;
            }
        }
        if (timeout_ms < 1)
            timeout_ms = 1;

        // Futex-based sleep with timeout
        uint32_t timeout_us = (uint32_t) (timeout_ms * 1000);
        atomic_store_explicit(&worker->m->park_state, XR_PARK_IDLE, memory_order_release);
        worker->p.stats.park_count++;
        xr_park_futex_wait(&worker->m->park_state, XR_PARK_IDLE, timeout_us);
        worker->p.stats.unpark_count++;
    }

    // Self-wake from futex timeout/signal: no explicit removal needed.
    // The idle_worker_list may still reference our M; a later wake will
    // pop us as a no-op and in_idle_worker_list will prevent re-push
    // until a subsequent idle_worker_pop clears it.

    // Check if runtime is stopping before accessing runtime structures
    if (atomic_load(&runtime->running)) {
        atomic_fetch_add(&runtime->active_workers, 1);
        atomic_store(&worker->m->state, M_IDLE);
    }
}

// ========== Worker Main Loop Helpers ==========

// Set CPU affinity for the worker's thread (best effort; advisory on macOS).
static void worker_bind_cpu(XrWorker *worker) {
    (void) xr_thread_pin_to_cpu((unsigned int) worker->p.id);
}

// Run one round of housekeeping when local queue is empty: poll sources
// (netpoll + async + timer), reductions-based balance check, and migration.
// Returns false if runtime is shutting down.
static bool worker_housekeeping(XrWorker *worker, XrRuntime *runtime, int *poll_skip_io,
                                XrCoroutine **io_fast_out) {
    *io_fast_out = NULL;
    if (*poll_skip_io <= 0) {
        *io_fast_out = worker_poll_sources(worker);
    } else {
        worker_drain_inbox(worker);
        worker_pull_inject(worker, XR_INJECT_POP_BATCH);
        (*poll_skip_io)--;
    }

    if (worker->p.check_balance_reds <= 0) {
        xr_check_balance(runtime, worker);
    }

    XrMigrationLimit *ml = &runtime->migration_paths[worker->p.id].runq;
    int len = xr_runq_len(&worker->p.runq);
    bool need_emigrate = len > ml->limit_here && ml->target_worker >= 0;
    bool need_immigrate = len == 0;
    if (need_emigrate) {
        int migrated = xr_try_emigrate(worker);
        if (migrated > 0) {
            xr_proc_local_runq_dec(&worker->p, migrated);
            wake_idle_worker(runtime);
        }
    } else if (need_immigrate) {
        try_immigrate(worker);
    }

    return atomic_load(&runtime->running);
}

static bool worker_try_enter_search(XrRuntime *runtime) {
    int limit = runtime->worker_count / XR_SEARCHING_WORKER_DIVISOR;
    if (limit < 1)
        limit = 1;
    if (limit > XR_SEARCHING_WORKER_MAX)
        limit = XR_SEARCHING_WORKER_MAX;
    int cur = atomic_load_explicit(&runtime->searching_count, memory_order_relaxed);
    while (cur < limit) {
        if (atomic_compare_exchange_weak_explicit(&runtime->searching_count, &cur, cur + 1,
                                                  memory_order_acq_rel, memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

static uint64_t runtime_stealable_candidates(XrRuntime *runtime, uint64_t self_bit) {
    return atomic_load_explicit(&runtime->stealable_p_mask, memory_order_acquire) & ~self_bit;
}

static uint64_t worker_valid_mask(int worker_count) {
    if (worker_count <= 0)
        return 0;
    if (worker_count >= 64)
        return UINT64_MAX;
    return ((uint64_t) 1ull << worker_count) - 1;
}

static bool worker_steal_backoff_active(XrWorker *worker, XrRuntime *runtime, uint64_t self_bit,
                                        int64_t now, int64_t *out_delay_hint) {
    (void) runtime;
    (void) self_bit;
    if (worker->p.steal_backoff_until <= now)
        return false;

    *out_delay_hint = worker->p.steal_backoff_until - now;
    worker->p.stats.steal_backoff_count++;
    return true;
}

static void worker_update_steal_backoff(XrWorker *worker, bool found_work, int64_t now,
                                        int64_t delay_hint) {
    if (found_work || delay_hint <= 0) {
        worker->p.steal_backoff_until = 0;
        return;
    }
    if (delay_hint > XR_STEAL_BACKOFF_MAX_MS)
        delay_hint = XR_STEAL_BACKOFF_MAX_MS;
    worker->p.steal_backoff_until = now + delay_hint;
}

static void worker_wait_steal_delay(XrWorker *worker, XrRuntime *runtime, int64_t delay_ms) {
    if (!worker || !runtime || delay_ms <= 0)
        return;
    if (atomic_load_explicit(&runtime->injectq_nonempty, memory_order_acquire))
        return;
    if (atomic_load_explicit(&runtime->total_inbox_len, memory_order_relaxed) > 0)
        return;
    if (delay_ms > XR_STEAL_BACKOFF_MAX_MS)
        delay_ms = XR_STEAL_BACKOFF_MAX_MS;
    if (delay_ms < 1)
        delay_ms = 1;
    worker->p.stats.steal_throttle_wait_count++;
    atomic_store_explicit(&worker->m->park_state, XR_PARK_IDLE, memory_order_release);
    xr_park_futex_wait(&worker->m->park_state, XR_PARK_IDLE, (uint32_t) delay_ms * 1000u);
}

static void worker_record_direct_steal_dispatch(XrWorker *worker, XrCoroutine *coro, int64_t now) {
    if (!worker || !coro)
        return;
    worker->p.stats.steal_direct_dispatch_count++;
    xr_proc_stats_record_runnable_wait(&worker->p.stats, coro, now);
}

static int64_t worker_steal_freshness_ms(XrWorker *worker, XrRuntime *runtime, int victim_len) {
    if (!worker || !runtime || victim_len <= 0)
        return 0;
    int64_t min_freshness = XR_STEAL_TIME_RESOLUTION_MS;
    int idle_workers = atomic_load_explicit(&runtime->idle_worker_count, memory_order_relaxed);
    if (idle_workers * 2 >= runtime->worker_count)
        return min_freshness;
    if (worker->p.yield_streak >= runtime->worker_count / 2)
        return min_freshness;
    if (victim_len >= 64)
        return min_freshness;
    if (victim_len >= 8)
        return min_freshness;
    if (worker->p.io_poll_ewma > 128)
        return XR_STEAL_TIME_RESOLUTION_MS;
    return min_freshness;
}

typedef struct XrStealChoice {
    int worker_id;
    int victim_len;
    int64_t submit_time;
} XrStealChoice;

static void steal_choice_reset(XrStealChoice *choice) {
    choice->worker_id = -1;
    choice->victim_len = 0;
    choice->submit_time = 0;
}

static bool steal_choice_should_replace(const XrStealChoice *choice, int victim_len,
                                        int64_t submit_time) {
    if (choice->worker_id < 0)
        return true;
    if (victim_len > choice->victim_len)
        return true;
    if (victim_len == choice->victim_len && submit_time < choice->submit_time)
        return true;
    return false;
}

static void worker_scan_steal_candidates(XrWorker *worker, XrRuntime *runtime,
                                         _Atomic bool *running_ptr, uint64_t candidates,
                                         int64_t steal_now, int64_t *out_delay_hint,
                                         bool *should_exit, XrStealChoice *choice) {
    uint32_t start = xr_xorshift32(&worker->p.rng_state) % runtime->worker_count;
    uint64_t valid_candidates = candidates & worker_valid_mask(runtime->worker_count);
    uint64_t scan_rounds[2];
    scan_rounds[0] = valid_candidates & (~0ull << start);
    scan_rounds[1] = valid_candidates & (((uint64_t) 1ull << start) - 1);

    for (int round = 0; round < 2; round++) {
        uint64_t scan = scan_rounds[round];
        while (scan != 0) {
            if (!atomic_load(running_ptr)) {
                *should_exit = true;
                return;
            }

            int i = __builtin_ctzll(scan);
            scan &= scan - 1;
            worker->p.stats.steal_candidate_scan_count++;

            XrWorker *victim = &runtime->workers[i];
            int victim_len = xr_steal_queue_size(&victim->p.runq.deque);
            if (victim_len <= 0) {
                xr_worker_refresh_runq_masks(victim);
                continue;
            }

            XrCoroutine *oldest = xr_steal_queue_peek_top(&victim->p.runq.deque);

            int64_t freshness = worker_steal_freshness_ms(worker, runtime, victim_len);
            if (oldest && freshness > 0) {
                int64_t age = steal_now - oldest->submit_time;
                if (age < freshness) {
                    int64_t delay = freshness - age;
                    if (*out_delay_hint == 0 || delay < *out_delay_hint) {
                        *out_delay_hint = delay;
                    }
                    worker->p.stats.steal_fresh_reject_count++;
                    continue;
                }
            }

            int64_t submit_time = oldest ? oldest->submit_time : steal_now;
            if (steal_choice_should_replace(choice, victim_len, submit_time)) {
                choice->worker_id = i;
                choice->victim_len = victim_len;
                choice->submit_time = submit_time;
            }
        }
    }
}

static XrCoroutine *worker_try_steal_continuation(XrWorker *worker, XrRuntime *runtime,
                                                  uint64_t candidates) {
    uint32_t start = xr_xorshift32(&worker->p.rng_state) % runtime->worker_count;
    uint64_t valid_candidates = candidates & worker_valid_mask(runtime->worker_count);
    uint64_t scan_rounds[2];
    scan_rounds[0] = valid_candidates & (~0ull << start);
    scan_rounds[1] = valid_candidates & (((uint64_t) 1ull << start) - 1);

    for (int round = 0; round < 2; round++) {
        uint64_t scan = scan_rounds[round];
        while (scan != 0) {
            int i = __builtin_ctzll(scan);
            scan &= scan - 1;
            worker->p.stats.steal_candidate_scan_count++;

            XrWorker *victim = &runtime->workers[i];
            XrCoroutine *cont = xr_steal_queue_steal(&victim->p.cont_deque);
            if (!cont)
                continue;

            worker->p.stats.cont_steal_count++;
            xr_worker_refresh_runq_masks(victim);
            return cont;
        }
    }
    return NULL;
}

// Two-round time-aware work stealing. Returns a stolen coro or NULL.
// Sets *out_delay_hint to the shortest remaining-freshness window.
XR_FUNC XrCoroutine *xr_worker_try_steal_once(XrWorker *worker, XrRuntime *runtime,
                                              _Atomic bool *running_ptr, int64_t *out_delay_hint,
                                              bool *should_exit) {
    *out_delay_hint = 0;
    *should_exit = false;
    int64_t steal_now = xr_monotonic_ticks();
    uint64_t self_bit = xr_runtime_worker_bit(worker->p.id);
    if (runtime_stealable_candidates(runtime, self_bit) == 0) {
        worker->p.stats.steal_no_candidate_count++;
        return NULL;
    }
    if (worker_steal_backoff_active(worker, runtime, self_bit, steal_now, out_delay_hint))
        return NULL;
    if (!worker_try_enter_search(runtime)) {
        worker->p.stats.steal_skip_count++;
        *out_delay_hint = 1;
        return NULL;
    }
    worker->p.stats.steal_attempt_count++;
    atomic_store(&worker->m->state, M_STEALING);
    XrCoroutine *coro = NULL;
    uint64_t candidates = runtime_stealable_candidates(runtime, self_bit);

    for (int round = 0; round < 2 && !coro; round++) {
        XrStealChoice choice;
        steal_choice_reset(&choice);

        if (candidates != 0) {
            worker_scan_steal_candidates(worker, runtime, running_ptr, candidates, steal_now,
                                         out_delay_hint, should_exit, &choice);
            if (*should_exit)
                goto done;
        }

        if (choice.worker_id >= 0) {
            XrWorker *victim = &runtime->workers[choice.worker_id];
            XrCoroutine *direct = NULL;
            int stolen = xr_runq_steal_direct(&victim->p.runq, &worker->p.runq, 50, &direct);
            if (stolen > 0) {
                xr_proc_local_runq_dec(&victim->p, stolen);
                int queued = direct ? stolen - 1 : stolen;
                if (queued > 0) {
                    xr_proc_local_runq_inc(&worker->p, queued);
                }
                worker->p.stats.stolen_count += stolen;
                worker->p.stats.steal_success_count++;
                xr_worker_refresh_runq_masks(victim);
                xr_worker_refresh_runq_masks(worker);
                if (direct) {
                    worker_record_direct_steal_dispatch(worker, direct, steal_now);
                    coro = direct;
                } else {
                    coro = xr_worker_pop(worker);
                }
            } else {
                xr_worker_refresh_runq_masks(victim);
            }
        }

        // Continuation deque: no freshness check. Critical for JIT
        // parallelism when a parent waits behind a non-yielding child.
        if (!coro && candidates != 0) {
            coro = worker_try_steal_continuation(worker, runtime, candidates);
        }
    }
done:
    atomic_fetch_sub_explicit(&runtime->searching_count, 1, memory_order_acq_rel);
    worker_update_steal_backoff(worker, coro != NULL, steal_now, *out_delay_hint);
    return coro;
}

// Limited spinning for new work after stealing failed. Handles spinning-count
// accounting and periodic inbox drain + timer wheel bump. Returns coro or NULL.
static XrCoroutine *worker_spin(XrWorker *worker, XrRuntime *runtime, _Atomic bool *running_ptr,
                                bool *should_exit) {
    *should_exit = false;
    if (!worker->m->spinning) {
        int cur_spin = atomic_load_explicit(&runtime->spinning_count, memory_order_relaxed);
        int npidle = runtime->worker_count - cur_spin;
        if (2 * cur_spin < npidle) {
            worker->m->spinning = true;
            atomic_fetch_add(&runtime->spinning_count, 1);
        }
    }
    if (!worker->m->spinning)
        return NULL;

    XrCoroutine *coro = NULL;
    int64_t cached_now = xr_monotonic_ticks();
    for (int spin = 0; spin < XR_WORKER_SPIN_COUNT && !coro; spin++) {
        if (!atomic_load(running_ptr)) {
            *should_exit = true;
            return NULL;
        }
        worker_drain_inbox(worker);
        worker_pull_inject(worker, XR_INJECT_POP_BATCH);
        if ((spin & 0x3) == 0) {
            cached_now = xr_monotonic_ticks();
        }
        if (worker->p.timer_wheel && (xr_timer_cancel_pending(worker->p.timer_wheel) ||
                                      cached_now > worker->p.last_timer_tick)) {
            xr_bump_timers(worker->p.timer_wheel, cached_now);
            worker->p.stats.timer_bump_count++;
            if (cached_now > worker->p.last_timer_tick) {
                worker->p.last_timer_tick = cached_now;
            }
        }
        coro = xr_worker_pop(worker);
    }
    return coro;
}

// Resetspinning: called after finding work. If we were the last spinner,
// wake one idle worker so newly-arrived work does not sit undiscovered.
static inline void worker_reset_spinning(XrWorker *worker, XrRuntime *runtime) {
    if (!worker->m->spinning)
        return;
    worker->m->spinning = false;
    int prev_spin = atomic_fetch_sub(&runtime->spinning_count, 1);
    if (prev_spin == 1) {
        wake_idle_worker(runtime);
    }
}

// ========== Worker Main Loop ==========

// Worker main loop (GMP model)
//
// M (Worker) must acquire P (Processor) to execute G (Goroutine/coroutine)
// When M blocks, can release P to other M (Hand Off).
//
// The hot inner loop delegates to worker_housekeeping,
// worker_try_steal, worker_spin, and worker_reset_spinning. The main
// function remains the state machine but stays under the 150-line limit.
void *worker_loop(void *arg) {
    XrWorker *worker = (XrWorker *) arg;
    XrRuntime *runtime = worker->p.runtime;
    _Atomic bool *running_ptr = &runtime->running;

    tls_current_worker = worker;
    tls_current_machine = worker->m;
    worker_bind_cpu(worker);

    // Two counters: started_workers for startup sync, active_workers for GC coord.
    // Wake xr_runtime_ensure_workers that's futex-waiting on this counter.
    atomic_fetch_add_explicit(&runtime->started_workers, 1, memory_order_release);
    xr_park_futex_wake(&runtime->started_workers);

    while (atomic_load(&runtime->running)) {
        int poll_skip = 0;

        while (atomic_load(&runtime->running)) {
            XrCoroutine *coro = NULL;

            // Anti-starvation: probabilistically drain inbox BEFORE local pop
            // so cross-worker deliveries are not starved by a full local queue.
            uint32_t sched_sample = xr_xorshift32(&worker->p.rng_state);
            if (sched_sample % (2 * runtime->worker_count) == 0) {
                worker_drain_inbox(worker);
            }
            bool inject_nonempty =
                atomic_load_explicit(&runtime->injectq_nonempty, memory_order_acquire);
            if (inject_nonempty && (sched_sample & 3u) == 0 &&
                xr_proc_local_runq_len(&worker->p) < XR_INJECT_POP_BATCH) {
                worker_pull_inject(worker, XR_INJECT_POP_BATCH);
            }

            // Fast path: local queue first.
            coro = xr_worker_pop(worker);
            if (coro) {
                worker_drain_inbox(worker);
                goto found_work;
            }

            // Slow path: housekeeping (poll, balance, migrate).
            XrCoroutine *io_fast = NULL;
            if (!worker_housekeeping(worker, runtime, &poll_skip, &io_fast))
                goto exit_loop;

            // Fast-path IO coroutine: skip queue, execute directly.
            if (io_fast) {
                coro = io_fast;
                goto found_work;
            }

            // Recheck after housekeeping (inbox drain may have added work).
            coro = xr_worker_pop(worker);

            // Work stealing (2 rounds, time-aware).
            int64_t min_steal_delay = 0;
            if (!coro && atomic_load(running_ptr)) {
                bool exit_flag = false;
                coro = xr_worker_try_steal_once(worker, runtime, running_ptr, &min_steal_delay,
                                                &exit_flag);
                if (exit_flag)
                    goto exit_loop;
                if (!coro && min_steal_delay > 0) {
                    worker_wait_steal_delay(worker, runtime, min_steal_delay);
                }
            }

            // Spinning: enter spinning state to find work.
            if (!coro) {
                bool exit_flag = false;
                coro = worker_spin(worker, runtime, running_ptr, &exit_flag);
                if (exit_flag)
                    goto exit_loop;
            }

        found_work:
            if (coro) {
                if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) {
                    SCHED_TRACE_CORO(worker, coro, "skip_done");
                    continue;
                }
                // Thread-lock gate: if this coro is pinned to a different
                // worker (via Coro.lockThread()), forward it to the correct
                // worker's inbox and continue looking for local work.
                // This catches coros that arrived via work-stealing or
                // migration and must not execute on this worker.
                if (xr_coro_is_thread_locked(coro) && coro->ext->locked_worker != worker->p.id) {
                    int lw = coro->ext->locked_worker;
                    if (lw >= 0 && lw < runtime->worker_count) {
                        xr_worker_inbox_enqueue(runtime, lw, coro);
                    }
                    continue;
                }
                worker_reset_spinning(worker, runtime);

                // Adaptive polling: EWMA-based continuous poll_skip.
                // io_poll_ewma range [0,256], poll_skip range [1,8].
                poll_skip = 8 - (int) (worker->p.io_poll_ewma >> 5);
                if (poll_skip < 1)
                    poll_skip = 1;

                atomic_store(&worker->m->state, M_RUNNING);
                worker_exec_with_cont_stealing(worker, coro);

                // Compute-bound pressure detection.
                if (worker->p.yield_streak >= runtime->worker_count) {
                    xr_runtime_ensure_workers(runtime);
                    worker->p.yield_streak = 0;
                }
                continue;
            }

            // No work: ensure next iteration polls, then park.
            poll_skip = 0;
            if (!atomic_load(running_ptr))
                goto exit_loop;
            worker_park(worker);
            if (!atomic_load(running_ptr))
                goto exit_loop;
        }
    }

exit_loop:
    atomic_fetch_add(&runtime->exited_workers, 1);
    return NULL;
}
