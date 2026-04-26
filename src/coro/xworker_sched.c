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
#include "../base/xlog.h"
#include "xsched_trace.h"
#include <sched.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

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
    if (!atomic_compare_exchange_strong_explicit(
            &m->in_idle_worker_list, &expected, true,
            memory_order_acq_rel, memory_order_relaxed)) {
        return;
    }

    XrMachine *head;
    do {
        head = atomic_load_explicit(&rt->idle_worker_list, memory_order_relaxed);
        m->idle_link = head;
    } while (!atomic_compare_exchange_weak_explicit(
        &rt->idle_worker_list, &head, m,
        memory_order_release, memory_order_relaxed));
    atomic_fetch_add_explicit(&rt->idle_worker_count, 1, memory_order_relaxed);
}

static XrMachine *idle_worker_pop(XrRuntime *rt) {
    for (int retry = 0; retry < 8; retry++) {
        XrMachine *head = atomic_load_explicit(&rt->idle_worker_list, memory_order_acquire);
        if (!head) return NULL;
        XrMachine *next = head->idle_link;
        if (atomic_compare_exchange_weak_explicit(
                &rt->idle_worker_list, &head, next,
                memory_order_acq_rel, memory_order_acquire)) {
            head->idle_link = NULL;
            atomic_store_explicit(&head->in_idle_worker_list, false,
                                   memory_order_release);
            atomic_fetch_sub_explicit(&rt->idle_worker_count, 1, memory_order_relaxed);
            return head;
        }
    }
    return NULL;
}

// Wake one idle worker (lock-free pop + unpark).
// No-op if the stack is empty.
void wake_idle_worker(XrRuntime *rt) {
    XrMachine *m = idle_worker_pop(rt);
    if (!m) return;
    atomic_store_explicit(&m->park_state, XR_PARK_WOKEN, memory_order_release);
    xr_park_futex_wake(&m->park_state);
}

// Public API: wake one idle worker (for use by xcoro.c etc.)
void xr_runtime_wake_idle_worker(XrRuntime *runtime) {
    if (runtime) wake_idle_worker(runtime);
}

// Wake Worker (simple futex wake via park_state)
void worker_unpark(XrWorker *worker) {
    XrMachine *m = worker->m;
    if (!m) return;  // M detached during handoff
    atomic_store_explicit(&m->park_state, XR_PARK_WOKEN, memory_order_release);
    xr_park_futex_wake(&m->park_state);
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

// ========== Migration ==========

// try_immigrate - Per-priority pull from high-load Worker
static void try_immigrate(XrWorker *worker) {
    XrRuntime *runtime = worker->p.runtime;

    for (int p = 0; p < XR_RUNQ_COUNT; p++) {
        XrMigrationPath *mp = &runtime->migration_paths[worker->p.id];
        int source_id = mp->prio[p].target_worker;
        if (source_id < 0 || source_id >= runtime->worker_count) continue;

        // Only immigrate if our queue for this priority is empty
        if (xr_runq_len(&worker->p.runq[p]) > 0) continue;

        XrWorker *source = &runtime->workers[source_id];
        int stolen = xr_runq_steal(&source->p.runq[p], &worker->p.runq[p], 50);
        if (stolen > 0) {
            worker->p.local_runq_len += stolen;
            mp->prio[p].target_worker = -1;
        }
    }
}

// ========== Shared Scheduling Helpers ==========
// Used by both worker_loop and xr_handoff_thread_entry to avoid duplication.

// Drain MPSC inbox into P's local run queue, maintaining global inbox counter.
void worker_drain_inbox(XrWorker *worker) {
    XrCoroutine *list = xr_mpsc_drain(&worker->p.inbox);
    int count = 0;
    while (list) {
        XrCoroutine *next = list->sched_link;
        list->sched_link = NULL;
        xr_worker_push(worker, list);
        list = next;
        count++;
    }
    if (count > 0 && worker->p.runtime) {
        atomic_fetch_sub_explicit(&worker->p.runtime->total_inbox_len, count, memory_order_relaxed);
    }
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
        XrCoroutine *io_coro = local_ready.head;
        while (io_coro) {
            XrCoroutine *next = io_coro->sched_link;
            io_coro->sched_link = NULL;
            XR_DCHECK(!xr_coro_flags_has(io_coro, XR_CORO_FLG_DONE),
                      "poll_sources: waking DONE coroutine from local IO");
            SCHED_TRACE_CORO(worker, io_coro, "local_io_wake");
            xr_coro_resume_store(io_coro, XR_RESUME_IO_READY);
            xr_coro_transition_wake(io_coro);
            xr_worker_push_lifo(worker, io_coro);
            io_coro = next;
        }
    }

    // ===== Shared netpoll (all workers, handles unbound fds) =====
    {
        XrReadyList ready = xr_netpoll_poll(&runtime->netpoll, 0);
        total_io_events += ready.count;

        // Zero-copy fast path: single IO wakeup with affinity to this
        // worker — skip queue push/pop, return directly for execution.
        if (ready.count == 1 && ready.head) {
            XrCoroutine *io_coro = ready.head;
            int aff = atomic_load_explicit(&io_coro->affinity_p,
                                           memory_order_relaxed);
            if (aff == p->id) {
                io_coro->sched_link = NULL;
                XR_DCHECK(!xr_coro_flags_has(io_coro, XR_CORO_FLG_DONE),
                          "poll_sources: waking DONE coroutine from IO");
                SCHED_TRACE_CORO(worker, io_coro, "io_wake_fast");
                xr_coro_resume_store(io_coro, XR_RESUME_IO_READY);
                xr_coro_transition_wake(io_coro);
                fast_coro = io_coro;
                goto after_netpoll;
            }
        }

        // Normal path: enqueue all ready coroutines to LIFO slot.
        XrCoroutine *io_coro = ready.head;
        while (io_coro) {
            XrCoroutine *next = io_coro->sched_link;
            io_coro->sched_link = NULL;
            XR_DCHECK(!xr_coro_flags_has(io_coro, XR_CORO_FLG_DONE),
                      "poll_sources: waking DONE coroutine from IO");
            SCHED_TRACE_CORO(worker, io_coro, "io_wake");
            xr_coro_resume_store(io_coro, XR_RESUME_IO_READY);
            xr_coro_transition_wake(io_coro);
            xr_worker_push_lifo(worker, io_coro);
            io_coro = next;
        }
    }

after_netpoll:
    // Adaptive poll_skip feedback: EWMA of I/O event frequency.
    // Decay 7/8: io_ewma = io_ewma * 7/8 + sample * 1/8
    // Sample: 256 if events, 0 if none. Range [0, 256].
    p->io_poll_ewma = p->io_poll_ewma - (p->io_poll_ewma >> 3)
                    + (total_io_events > 0 ? 32 : 0);

    // Async thread pool completions
    if (runtime->async_pool) {
        xr_async_check_ready(runtime->async_pool, p->id);
    }

    // Advance timers (callbacks wake sleeping coroutines directly).
    // Loop until all expired timers are drained: the timer wheel yields
    // after ~100 timeouts per bump call (XR_TW_COST_TIMEOUT=100), so a
    // burst of 10000 timers requires ~100 iterations.
    int64_t now = xr_monotonic_ticks();
    if (p->timer_wheel && now > p->last_timer_tick) {
        int32_t inbox_before = atomic_load_explicit(
            &runtime->total_inbox_len, memory_order_relaxed);
        do {
            xr_bump_timers(p->timer_wheel, now);
        } while (p->timer_wheel->yield_slot != XR_TW_SLOT_INACTIVE);
        p->last_timer_tick = now;
        // After timer batch: wake idle workers to help process burst.
        // Wake count = min(new_items, idle_workers) — no point waking
        // more workers than available work or idle capacity.
        int32_t new_items = atomic_load_explicit(
            &runtime->total_inbox_len, memory_order_relaxed) - inbox_before;
        if (new_items > 0) {
            int idle_count = atomic_load_explicit(
                &runtime->idle_worker_count, memory_order_relaxed);
            if (idle_count < 0) idle_count = 0;
            int wakes = new_items < idle_count ? new_items : idle_count;
            if (wakes < 1) wakes = 1;
            for (int _w = 0; _w < wakes; _w++)
                wake_idle_worker(runtime);
        }
    }

    // Drain deferred free queue (cross-worker PollDesc cleanup)
    // Must run after timer bump so zombie timers are already cleaned up.
    xr_netpoll_drain_deferred(&runtime->netpoll, p);

    // Drain MPSC inbox
    worker_drain_inbox(worker);

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
    XrCoroutine *coro = (XrCoroutine *)arg;
    if (!coro) return;

    XR_DBG_TIMER("Worker callback triggered: coro=%d, timer_active=%d", coro->id,
                 coro->ext ? (int)atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed) : 0);

    // Check if timer was cancelled (coroutine recycle cancels timer)
    if (!coro->ext || !atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed)) {
        return;
    }

    // Note: coro_gc may be NULL legitimately (lazy allocation).
    // Do NOT check coro_gc here — it's not a valid recycle indicator.

    // Check if coroutine already done (avoid waking completed coroutine)
    if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) {
        atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
        return;
    }

    // Get Runtime (from current Worker)
    XrWorker *worker = xr_current_worker();
    XrRuntime *runtime = worker ? worker->p.runtime : NULL;
    if (!runtime) return;

    // Check if runtime is running (avoid waking during exit)
    if (!atomic_load(&runtime->running)) {
        atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
        return;
    }

    // Check if select wait (use CAS to prevent duplicate wake)
    XrSelectWait *sw = coro->select_wait;
    if (sw) {
        bool expected = false;
        if (!atomic_compare_exchange_strong(&sw->triggered, &expected, true)) {
            // Already woken by another channel, ignore this timer
            return;
        }
        // Record timer timeout
        coro->select_ready_case = sw->timer_case_index;

        // Remove from blocked queue
        xr_worker_unblock_select(worker, coro);
    } else {
        // Check if waiting on channel (sendTimeout/recvTimeout)
        if (coro->wait_channel) {
            // Remove from channel wait queue
            XrChannel *ch = (XrChannel*)coro->wait_channel;
            xr_channel_remove_waiter(ch, coro);
            coro->wait_channel = NULL;
        }
        // Remove from blocked queue (unified via xr_worker_unblock)
        xr_worker_unblock(worker, coro);
    }

    // Clear blocked flags
    xr_coro_flags_clear(coro, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_MASK);
    atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);

    // Set resume status to timeout
    xr_coro_resume_store(coro, XR_RESUME_TIMEOUT);

    // Set ready state
    xr_coro_flags_set(coro, XR_CORO_FLG_READY);

    // Enqueue to coroutine's affinity worker inbox (with Dekker sync + wake)
    int target_id = atomic_load_explicit(&coro->affinity_p, memory_order_relaxed);
    if (target_id < 0 || target_id >= runtime->worker_count) {
        target_id = 0;
    }
    xr_worker_inbox_enqueue(runtime, target_id, coro);
}

// Add sleep timer to Worker's Timer Wheel (lock-free, owner-private)
void xr_worker_add_sleep_timer(XrWorker *worker, XrCoroutine *coro, int64_t delay_ms) {
    if (!worker || !coro || delay_ms < 0) return;

    XrTimerWheel *tw = worker->p.timer_wheel;
    if (!tw) return;

    // Use coroutine's ext timer node (lazy-alloc ext on first sleep)
    XrCoroExt *text = xr_coro_ensure_ext(coro);
    if (!text) return;
    XrTWheelTimer *timer = &text->timer;

    // Initialize timer node
    timer->prev = NULL;
    timer->next = NULL;
    timer->slot = XR_TW_SLOT_INACTIVE;

    // Increment sequence number (prevent stale notifications)
    atomic_fetch_add(&text->timer_seq, 1);

    // Mark timer active and record ownership
    atomic_store_explicit(&text->timer_active, true, memory_order_relaxed);
    text->timer_wheel_owner = worker->p.id;  // Record which worker owns this timer

    // Calculate timeout position
    int64_t timeout_pos = xr_monotonic_ticks() + delay_ms;

    // Set timer (must be called from owner worker)
    XR_DBG_TIMER("Worker set_timer: tw=%p, timeout_pos=%lld, tw->pos=%lld, owner=%d",
                 (void*)tw, (long long)timeout_pos, (long long)tw->pos, worker->p.id);
    xr_twheel_set_timer(tw, timer, worker_sleep_timeout_callback, coro, timeout_pos);
}

// Cancel timer - handles cross-worker case via async queue
//
// Design:
// - If current worker owns the timer: direct cancel (lock-free)
// - If other worker owns the timer: enqueue to owner's canceled_queue
void xr_worker_cancel_timer(XrWorker *current_worker, XrCoroutine *coro) {
    if (!coro || !coro->ext) return;
    if (!atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed)) return;

    int owner_id = coro->ext->timer_wheel_owner;
    XrRuntime *runtime = current_worker ? current_worker->p.runtime : NULL;
    if (!runtime) return;

    // Get owner worker's timer wheel
    if (owner_id < 0 || owner_id >= runtime->worker_count) {
        // Invalid owner, just mark inactive
        atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
        return;
    }

    XrTimerWheel *owner_tw = runtime->workers[owner_id].p.timer_wheel;
    if (!owner_tw) {
        atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
        return;
    }

    // Check if current worker is the owner
    if (current_worker && current_worker->p.id == owner_id) {
        // Same worker: direct cancel (lock-free, no mutex needed)
        xr_twheel_cancel_timer(owner_tw, &coro->ext->timer);
        atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
        XR_DBG_TIMER("Timer canceled locally: coro=%d, owner=%d", coro->id, owner_id);
    } else {
        // Cross-worker: enqueue to owner's canceled_queue (async)
        xr_timer_queue_cancel(owner_tw, &coro->ext->timer, coro);
        atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
        XR_DBG_TIMER("Timer cancel queued: coro=%d, owner=%d, current=%d",
                     coro->id, owner_id, current_worker ? current_worker->p.id : -1);
    }
}

// ========== Worker Park / Work Detection ==========

// Check for work by scanning actual deque sizes (accurate, used only before park)
static bool runtime_has_work(XrRuntime *runtime) {
    for (int i = 0; i < runtime->worker_count; i++) {
        if (atomic_load_explicit(&runtime->workers[i].p.lifo_slot, memory_order_relaxed)) return true;
        if (xr_proc_total_queue_len(&runtime->workers[i].p) > 0) return true;
        // Check continuation deque: parent coros waiting while child
        // runs JIT code without yielding are stealable work.
        if (xr_steal_queue_size(&runtime->workers[i].p.cont_deque) > 0) return true;
    }
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
            XrCoroutine *io_coro = ready.head;
            while (io_coro) {
                XrCoroutine *next = io_coro->sched_link;
                io_coro->sched_link = NULL;
                xr_coro_resume_store(io_coro, XR_RESUME_IO_READY);
                xr_coro_transition_wake(io_coro);
                xr_worker_push_lifo(worker, io_coro);
                io_coro = next;
            }
            // Found IO work, abort park. M remains in idle_worker_list until
            // a later wake_idle_worker pops it — tolerable because the wake
            // is idempotent and in_idle_worker_list guards re-entry.
            atomic_fetch_add(&runtime->active_workers, 1);
            atomic_store(&worker->m->state, M_IDLE);
            return;
        }
    }

    // Recheck for work before sleeping
    if (!runtime_has_work(runtime) && atomic_load(&runtime->running)) {
        // Adaptive timeout: IO-heavy workloads use shorter sleep (faster response),
        // CPU-heavy workloads use longer sleep (less futex overhead).
        // io_poll_ewma > 128 means >50% of polls had IO events.
        int64_t timeout_ms;
        if (worker->p.io_poll_ewma > 128) {
            timeout_ms = 2;    // IO heavy: wake quickly for IO events
        } else {
            timeout_ms = 10;   // CPU heavy: longer sleep, less overhead
        }

        // Timer-aware: clamp timeout to next timer expiry
        if (worker->p.timer_wheel) {
            int64_t next = xr_check_next_timeout_time(worker->p.timer_wheel);
            int64_t now_ticks = xr_monotonic_ticks();
            if (next > now_ticks) {
                int64_t delta = next - now_ticks;
                if (delta < timeout_ms) timeout_ms = delta;
            } else {
                timeout_ms = 1;
            }
        }
        if (timeout_ms < 1) timeout_ms = 1;

        // Futex-based sleep with timeout
        uint32_t timeout_us = (uint32_t)(timeout_ms * 1000);
        atomic_store_explicit(&worker->m->park_state, XR_PARK_IDLE, memory_order_release);
        xr_park_futex_wait(&worker->m->park_state, XR_PARK_IDLE, timeout_us);
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

// Set CPU affinity for the worker's thread (best effort; advisory on mac).
static void worker_bind_cpu(XrWorker *worker) {
#ifdef __APPLE__
    thread_affinity_policy_data_t policy = { worker->p.id };
    thread_policy_set(pthread_mach_thread_np(pthread_self()),
                      THREAD_AFFINITY_POLICY,
                      (thread_policy_t)&policy, 1);
#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(worker->p.id % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#elif defined(_WIN32)
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    DWORD_PTR mask = 1ULL << (worker->p.id % sysinfo.dwNumberOfProcessors);
    SetThreadAffinityMask(GetCurrentThread(), mask);
#else
    (void)worker;
#endif
}

// Run one round of housekeeping when local queue is empty: poll sources
// (netpoll + async + timer), reductions-based balance check, and per-priority
// migration. Returns false if runtime is shutting down.
static bool worker_housekeeping(XrWorker *worker, XrRuntime *runtime,
                                 int *poll_skip_io, XrCoroutine **io_fast_out) {
    *io_fast_out = NULL;
    if (*poll_skip_io <= 0) {
        *io_fast_out = worker_poll_sources(worker);
    } else {
        worker_drain_inbox(worker);
        (*poll_skip_io)--;
    }

    if (worker->p.check_balance_reds <= 0) {
        xr_check_balance(runtime, worker);
    }

    bool need_emigrate = false;
    bool need_immigrate = true;  // Local queue empty when we get here.
    for (int pi = 0; pi < XR_RUNQ_COUNT; pi++) {
        XrMigrationLimit *ml = &runtime->migration_paths[worker->p.id].prio[pi];
        int len = xr_runq_len(&worker->p.runq[pi]);
        if (len > 0) need_immigrate = false;
        if (len > ml->limit_here && ml->target_worker >= 0) {
            need_emigrate = true;
        }
    }
    if (need_emigrate) {
        int migrated = xr_try_emigrate(worker);
        if (migrated > 0) {
            worker->p.local_runq_len -= migrated;
            wake_idle_worker(runtime);
        }
    } else if (need_immigrate) {
        try_immigrate(worker);
    }

    return atomic_load(&runtime->running);
}

// Two-round time-aware work stealing. Returns a stolen coro or NULL.
// Sets *out_delay_hint to the shortest remaining-freshness window (used to
// decide a brief sched_yield when all candidates are too fresh to steal).
static XrCoroutine *worker_try_steal(XrWorker *worker, XrRuntime *runtime,
                                      _Atomic bool *running_ptr,
                                      int64_t *out_delay_hint,
                                      bool *should_exit) {
    *out_delay_hint = 0;
    *should_exit = false;
    atomic_store(&worker->m->state, M_STEALING);
    int64_t steal_now = xr_monotonic_ticks();
    XrCoroutine *coro = NULL;

    for (int round = 0; round < 2 && !coro; round++) {
        uint32_t start = xr_xorshift32(&worker->p.rng_state) % runtime->worker_count;

        for (int j = 0; j < runtime->worker_count && !coro; j++) {
            if (!atomic_load(running_ptr)) {
                *should_exit = true;
                return NULL;
            }
            int i = (start + j) % runtime->worker_count;
            if (i == worker->p.id) continue;

            XrWorker *victim = &runtime->workers[i];

            // Steal from all run queues (with freshness check).
            for (int p = 0; p < XR_RUNQ_COUNT && !coro; p++) {
                XrCoroutine *oldest = xr_steal_queue_peek_top(&victim->p.runq[p].deque);
                if (oldest) {
                    int64_t age = steal_now - oldest->submit_time;
                    if (age < XR_STEAL_TIME_RESOLUTION_MS) {
                        int64_t delay = XR_STEAL_TIME_RESOLUTION_MS - age;
                        if (*out_delay_hint == 0 || delay < *out_delay_hint) {
                            *out_delay_hint = delay;
                        }
                        continue;
                    }
                }

                int stolen = xr_runq_steal(&victim->p.runq[p],
                                            &worker->p.runq[p], 50);
                if (stolen > 0) {
                    worker->p.local_runq_len += stolen;
                    worker->p.stats.stolen_count += stolen;
                    coro = xr_worker_pop(worker);
                }
            }

            // Continuation deque: no freshness check. Critical for JIT
            // parallelism — parent continuation blocked behind a JIT child
            // is only reclaimable this way.
            if (!coro) {
                XrCoroutine *cont = xr_steal_queue_steal(&victim->p.cont_deque);
                if (cont) {
                    worker->p.stats.cont_steal_count++;
                    coro = cont;
                }
            }
        }
    }
    return coro;
}

// Limited spinning for new work after stealing failed. Handles spinning-count
// accounting and periodic inbox drain + timer wheel bump. Returns coro or NULL.
static XrCoroutine *worker_spin(XrWorker *worker, XrRuntime *runtime,
                                 _Atomic bool *running_ptr, bool *should_exit) {
    *should_exit = false;
    if (!worker->m->spinning) {
        int cur_spin = atomic_load_explicit(&runtime->spinning_count, memory_order_relaxed);
        int npidle = runtime->worker_count - cur_spin;
        if (2 * cur_spin < npidle) {
            worker->m->spinning = true;
            atomic_fetch_add(&runtime->spinning_count, 1);
        }
    }
    if (!worker->m->spinning) return NULL;

    XrCoroutine *coro = NULL;
    int64_t cached_now = xr_monotonic_ticks();
    for (int spin = 0; spin < XR_WORKER_SPIN_COUNT && !coro; spin++) {
        if (!atomic_load(running_ptr)) {
            *should_exit = true;
            return NULL;
        }
        worker_drain_inbox(worker);
        if ((spin & 0x3) == 0) {
            cached_now = xr_monotonic_ticks();
        }
        if (worker->p.timer_wheel && cached_now > worker->p.last_timer_tick) {
            xr_bump_timers(worker->p.timer_wheel, cached_now);
            worker->p.last_timer_tick = cached_now;
        }
        coro = xr_worker_pop(worker);
    }
    return coro;
}

// Resetspinning: called after finding work. If we were the last spinner,
// wake one idle worker so newly-arrived work does not sit undiscovered.
static inline void worker_reset_spinning(XrWorker *worker, XrRuntime *runtime) {
    if (!worker->m->spinning) return;
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
    XrWorker *worker = (XrWorker *)arg;
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
            if (xr_xorshift32(&worker->p.rng_state) % (2 * runtime->worker_count) == 0) {
                worker_drain_inbox(worker);
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
                coro = worker_try_steal(worker, runtime, running_ptr,
                                         &min_steal_delay, &exit_flag);
                if (exit_flag) goto exit_loop;
                if (!coro && min_steal_delay > 0) {
                    sched_yield();
                }
            }

            // Spinning: enter spinning state to find work.
            if (!coro) {
                bool exit_flag = false;
                coro = worker_spin(worker, runtime, running_ptr, &exit_flag);
                if (exit_flag) goto exit_loop;
            }

        found_work:
            if (coro) {
                if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) {
                    SCHED_TRACE_CORO(worker, coro, "skip_done");
                    continue;
                }
                worker_reset_spinning(worker, runtime);

                // Adaptive polling: EWMA-based continuous poll_skip.
                // io_poll_ewma range [0,256], poll_skip range [1,8].
                poll_skip = 8 - (int)(worker->p.io_poll_ewma >> 5);
                if (poll_skip < 1) poll_skip = 1;

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
            if (!atomic_load(running_ptr)) goto exit_loop;
            worker_park(worker);
            if (!atomic_load(running_ptr)) goto exit_loop;
        }
    }

exit_loop:
    atomic_fetch_add(&runtime->exited_workers, 1);
    return NULL;
}
