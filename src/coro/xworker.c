/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xworker.c - Multi-core runtime Worker implementation
 *
 * KEY CONCEPT:
 *   Worker thread pool with work-stealing scheduler.
 *   Basic multi-threading with mutex for correctness.
 */

#include "xworker_internal.h"
#include "xtask.h"
#include "xdeep_copy.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"
#include "xsched_trace.h"
#ifdef XRAY_HAS_JIT
#include "../jit/xir_jit.h"
#include "../jit/xjit_compile_queue.h"
#include "../jit/xir_jit_debug.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <sys/mman.h> // mmap

#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

// Thread-local: current Worker and Machine pointers
__thread XrWorker *tls_current_worker = NULL;
__thread XrMachine *tls_current_machine = NULL;

// ========== Forward Declarations ==========
bool worker_blocked_list_remove(XrWorker *worker, XrCoroutine *coro);
void *sysmon_thread_func(void *arg);

// ========== Utility Functions ==========

// Simple PRNG (for steal target selection)
static inline uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

// Get current thread's Worker
XrWorker *xr_current_worker(void) {
    return tls_current_worker;
}

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
        if (rq->overflow_last) rq->overflow_last->sched_link = coro;
        else rq->overflow_first = coro;
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
    if (actual_max > 32) actual_max = 32;
    if (actual_max > max_steal) actual_max = max_steal;
    if (actual_max <= 0) actual_max = 1;

    int stolen = 0;
    for (int i = 0; i < actual_max; i++) {
        XrCoroutine *c = xr_steal_queue_steal(&src->deque);
        if (!c) break;
        if (!xr_steal_queue_push(&dst->deque, c)) {
            // Destination full: put back via overflow
            c->sched_link = NULL;
            if (dst->overflow_last) dst->overflow_last->sched_link = c;
            else dst->overflow_first = c;
            dst->overflow_last = c;
            dst->overflow_len++;
        }
        stolen++;
    }
    return stolen;
}

// ========== Active Migration Load Balancing ==========

// Forward declaration
void worker_unpark(XrWorker *worker);

// ========== O(1) Idle Worker Stack (sched_lock protected) ==========

// Push worker index to idle stack (called before parking)
static void idle_worker_push(XrRuntime *rt, int worker_id) {
    pthread_mutex_lock(&rt->sched_lock);
    if (rt->idle_worker_count < XR_MAX_WORKERS) {
        rt->idle_worker_stack[rt->idle_worker_count++] = worker_id;
    }
    pthread_mutex_unlock(&rt->sched_lock);
}

// Pop worker index from idle stack (returns -1 if empty)
static int idle_worker_pop(XrRuntime *rt) {
    int idx = -1;
    pthread_mutex_lock(&rt->sched_lock);
    if (rt->idle_worker_count > 0) {
        idx = rt->idle_worker_stack[--rt->idle_worker_count];
    }
    pthread_mutex_unlock(&rt->sched_lock);
    return idx;
}

// Remove specific worker from idle stack (called on self-wake)
static void idle_worker_remove(XrRuntime *rt, int worker_id) {
    pthread_mutex_lock(&rt->sched_lock);
    for (int i = 0; i < rt->idle_worker_count; i++) {
        if (rt->idle_worker_stack[i] == worker_id) {
            rt->idle_worker_stack[i] = rt->idle_worker_stack[--rt->idle_worker_count];
            break;
        }
    }
    pthread_mutex_unlock(&rt->sched_lock);
}

// Wake one idle worker (O(1) pop + unpark)
void wake_idle_worker(XrRuntime *rt) {
    int idx = idle_worker_pop(rt);
    if (idx >= 0 && idx < rt->worker_count) {
        worker_unpark(&rt->workers[idx]);
    }
}

// Public API: wake one idle worker (for use by xcoro.c etc.)
void xr_runtime_wake_idle_worker(XrRuntime *runtime) {
    if (runtime) wake_idle_worker(runtime);
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

// Balance check interval (milliseconds)
#define XR_BALANCE_CHECK_INTERVAL_MS 100

// Migration threshold multiplier: emigrate when queue > 2x average
#define XR_MIGRATION_THRESHOLD_MULTIPLIER 2

// xr_check_balance moved to xbalance.c

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

// ========== No global queue ==========
// Global queue removed, all coroutines pass between Workers via MPSC inbox

// ========== Shared Scheduling Helpers ==========
// Used by both worker_loop and xr_handoff_thread_entry to avoid duplication.

// Drain MPSC inbox into P's local run queue, maintaining global inbox counter.
static void worker_drain_inbox(XrWorker *worker) {
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
static void worker_poll_sources(XrWorker *worker) {
    XrRuntime *runtime = worker->p.runtime;
    XrProc *p = &worker->p;

    // Netpoll: shared kqueue/epoll for all workers
    {
        XrReadyList ready = xr_netpoll_poll(&runtime->netpoll, 0);

        // Adaptive poll_skip feedback: EWMA of I/O event frequency.
        // Decay 7/8: io_ewma = io_ewma * 7/8 + sample * 1/8
        // Sample: 256 if events, 0 if none. Range [0, 256].
        p->io_poll_ewma = p->io_poll_ewma - (p->io_poll_ewma >> 3)
                        + (ready.count > 0 ? 32 : 0);

        XrCoroutine *io_coro = ready.head;
        while (io_coro) {
            XrCoroutine *next = io_coro->sched_link;
            io_coro->sched_link = NULL;
            // Invariant: IO wakeup must not target a DONE coroutine
            XR_DCHECK(!xr_coro_flags_has(io_coro, XR_CORO_FLG_DONE),
                      "poll_sources: waking DONE coroutine from IO");
            SCHED_TRACE_CORO(worker, io_coro, "io_wake");
            xr_coro_resume_store(io_coro, XR_RESUME_IO_READY);
            // Single CAS replaces flags_clear(BLOCKED) + flags_set(READY)
            xr_coro_flags_swap(io_coro, XR_CORO_FLG_BLOCKED, XR_CORO_FLG_READY);
            xr_worker_push_lifo(worker, io_coro);
            io_coro = next;
        }
    }

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
            int idle_count;
            pthread_mutex_lock(&runtime->sched_lock);
            idle_count = runtime->idle_worker_count;
            pthread_mutex_unlock(&runtime->sched_lock);
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
}

/*
 * post-check after setting BLOCKED flag.
 * Handles race between VM setting wait state and wake_waiter seeing BLOCKED=false.
 * Returns true if coro was re-readied (caller should not block it further).
 */
static inline bool worker_blocked_post_check(XrRuntime *runtime, XrCoroutine *coro) {
    int wr = xr_coro_get_wait_reason(xr_coro_flags_load(coro));
    if (wr == (XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT)) {
        XrTask *task = atomic_load_explicit(&coro->await_task, memory_order_acquire);
        if (task) {
            int astate = atomic_load_explicit(&task->await_state, memory_order_acquire);
            if (astate == XR_AWAIT_RESOLVED) {
                xr_coro_ready(runtime->isolate, coro, true);
                return true;
            }
        }
    } else if (wr == (XR_CORO_WAIT_AWAIT_ALL >> XR_CORO_WAIT_SHIFT) ||
               wr == (XR_CORO_WAIT_SCOPE >> XR_CORO_WAIT_SHIFT)) {
        if (atomic_load(&coro->wait_count) == 0) {
            xr_coro_ready(runtime->isolate, coro, true);
            return true;
        }
    } else if (wr == (XR_CORO_WAIT_AWAIT_ANY >> XR_CORO_WAIT_SHIFT)) {
        if (atomic_load(&coro->any_done)) {
            xr_coro_ready(runtime->isolate, coro, true);
            return true;
        }
    }
    return false;
}

/*
 * Unified BLOCKED post-processing (R5).
 *
 * Called after xr_coro_run_on_worker returns XR_VM_BLOCKED.
 * BLOCKED flag is already set by run_on_worker/run_cfunc_coro.
 *
 * Handles:
 *   1. Channel wake race (coro already re-readied by another thread)
 *   2. Dekker fence (pairs with wake_waiter)
 *   3. Full post-check for all wait reasons (await/await_all/scope/await_any)
 *   4. Select wait
 *   5. Timer → blocked queue
 *
 * Returns true if coro was already re-readied (caller must not touch it further).
 */
static inline bool worker_process_blocked(XrWorker *worker, XrCoroutine *coro) {
    XrRuntime *runtime = worker->p.runtime;

    // Race check: already woken by channel sender/closer
    int rs = xr_coro_resume_load(coro);
    if (rs == XR_RESUME_CHANNEL || rs == XR_RESUME_CHANNEL_CLOSED ||
        xr_coro_flags_has(coro, XR_CORO_FLG_READY)) {
        return true;
    }

    // Dekker fence: pairs with fence in wake_waiter after exchange(→RESOLVED)
    atomic_thread_fence(memory_order_seq_cst);

    // Full post-check for all wait reasons
    if (worker_blocked_post_check(runtime, coro)) {
        return true;
    }

    // Select wait: already handled by select infrastructure
    if (coro->select_wait) return false;

    // Timer active: add to blocked queue for tracking
    if (coro->ext && atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed))
        xr_worker_block(worker, coro);

    return false;
}

// Handle VM execution result for a coroutine. Returns true if runtime should stop.
static bool worker_handle_vm_result(XrWorker *worker, XrCoroutine *coro, XrVMResult result) {
    XrRuntime *runtime = worker->p.runtime;

    switch (result) {
        case XR_VM_OK: {
            worker->p.yield_streak = 0;
            // Result already saved in xr_coro_run_on_worker (coro->result).
            // flags_set uses release ordering, ensuring coro->result is visible
            // to other threads before FLG_DONE is observed.
            xr_coro_flags_set(coro, XR_CORO_FLG_DONE);

            /* Task/Executor separation: cache result in Task before wake.
             * Don't detach executor yet — vm_await will deep-copy result
             * to awaiting coro's heap, then detach + recycle executor. */
            if (coro->task) {
                xr_task_complete(coro->task, coro->result);
            }

            // Inline fast path: skip extern calls for anonymous coros without monitors
            if (__builtin_expect(coro->name != NULL || (coro->ext && coro->ext->watched_by), 0)) {
                XrScheduler *_s = (XrScheduler *)runtime->isolate->vm.scheduler;
                xr_coro_notify_monitors(runtime->isolate, _s ? _s->coro_registry : NULL, coro, "normal");
                xr_coro_on_exit(runtime->isolate, coro);
            }
            worker->p.completed_count++;
            xr_coro_wake_waiter(runtime->isolate, coro);
            if (xr_coro_flags_has(coro, XR_CORO_FLG_MAIN)) {
                atomic_store(&runtime->running, false);
                return true;
            }
            break;
        }
        case XR_VM_YIELD:
            xr_coro_resume_store(coro, XR_RESUME_OK);
            xr_coro_flags_clear(coro, XR_CORO_FLG_BLOCKED | XR_CORO_FLG_RUNNING);
            xr_coro_flags_set(coro, XR_CORO_FLG_READY);
            xr_worker_push(worker, coro);
            worker->p.yielded_count++;
            worker->p.yield_streak++;
            break;

        case XR_VM_BLOCKED:
            worker->p.yield_streak = 0;
            // BLOCKED flag already set by run_on_worker/run_cfunc_coro.
            // Unified post-processing: race check, fence, post_check, timer.
            worker_process_blocked(worker, coro);
            break;

        case XR_VM_DEBUG_BREAK:
            xr_coro_flags_set(coro, XR_CORO_FLG_BLOCKED);
            if (xr_coro_flags_has(coro, XR_CORO_FLG_MAIN)) {
                atomic_store(&runtime->running, false);
                return true;
            }
            break;

        case XR_VM_CANCELLED:
            worker->p.yield_streak = 0;
            xr_coro_flags_set(coro, XR_CORO_FLG_CANCELLED | XR_CORO_FLG_DONE);
            xr_coro_flags_clear(coro, XR_CORO_FLG_CANCEL_REQUESTED);
            /* Task/Executor separation: mark task cancelled.
             * Detach AFTER wake_waiter so task->waiter can be read. */
            if (coro->task) {
                xr_task_cancel(coro->task);
            }
            { XrScheduler *_s = (XrScheduler *)runtime->isolate->vm.scheduler;
            xr_coro_notify_monitors(runtime->isolate, _s ? _s->coro_registry : NULL, coro, "cancelled"); }
            xr_coro_on_exit(runtime->isolate, coro);
            worker->p.completed_count++;
            xr_coro_wake_waiter(runtime->isolate, coro);
            if (coro->task) {
                coro->task->coro = NULL;
                coro->task = NULL;
                coro->gc_flags |= XR_CORO_GC_RECYCLABLE;
            }
            if (xr_coro_flags_has(coro, XR_CORO_FLG_MAIN)) {
                atomic_store(&runtime->running, false);
                return true;
            }
            break;

        default:
            xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
            /* Task/Executor separation: mark task failed.
             * Detach AFTER wake_waiter so task->waiter can be read. */
            if (coro->task) {
                if (coro->task->link_mode == XR_LINK_LINKED && coro->task->parent) {
                    // linked go: propagate error to parent task
                    xr_task_fail_with_propagation(coro->task, coro->error);
                } else {
                    xr_task_fail(coro->task, coro->error);
                }
            }
            { XrScheduler *_s = (XrScheduler *)runtime->isolate->vm.scheduler;
            xr_coro_notify_monitors(runtime->isolate, _s ? _s->coro_registry : NULL, coro, "error"); }
            xr_coro_on_exit(runtime->isolate, coro);
            worker->p.completed_count++;
            xr_coro_wake_waiter(runtime->isolate, coro);
            if (coro->task) {
                coro->task->coro = NULL;
                coro->task = NULL;
                coro->gc_flags |= XR_CORO_GC_RECYCLABLE;
            }
            if (xr_coro_flags_has(coro, XR_CORO_FLG_MAIN)) {
                atomic_store(&runtime->running, false);
                return true;
            }
            break;
    }
    return false;
}

// Execute a coroutine with continuation stealing support.
// Handles the push-parent/exec-child/pop-parent loop.
// Also implements BLOCKED fast re-dispatch: when a coro blocks on channel
// and the LIFO slot has a just-woken coro, execute it inline without
// returning to worker_loop (avoids scheduling overhead for ping-pong patterns).
static void worker_exec_with_cont_stealing(XrWorker *worker, XrCoroutine *coro) {
    XrMachine *m = worker->m;
    XrProc *p = &worker->p;
    XrVMResult result;
    int fast_dispatch_budget = 64;  // Limit consecutive fast dispatches for fairness

cont_exec:
    // Invariant: coro must not be NULL or DONE when entering execution
    XR_DCHECK(coro != NULL, "cont_exec: NULL coroutine");
    XR_DCHECK(!xr_coro_flags_has(coro, XR_CORO_FLG_DONE),
              "cont_exec: executing DONE coroutine");
    SCHED_TRACE_CORO(worker, coro, "exec");
    m->current_coro = coro;
    p->local_active_coros++;
    // Update affinity so IO wakeups return to this worker
    atomic_store_explicit(&coro->affinity_p, p->id, memory_order_relaxed);

exec_fast:  // Fast re-dispatch entry: local_active_coros already correct
    result = xr_coro_run_on_worker(worker, coro);
    // Single-writer store: only owner thread writes, sysmon reads via relaxed load
    atomic_store_explicit(&m->heartbeat,
        atomic_load_explicit(&m->heartbeat, memory_order_relaxed) + 1,
        memory_order_relaxed);

    // Continuation stealing: child-first dispatch.
    // Push parent to cont_deque, switch to child for immediate execution.
    // Children that yield (compute-heavy) go back to run queue where workers
    // can steal them. Children that block (channel I/O) go to blocked queue
    // and are not stealable — preserving cache locality for channel patterns.
    if (result == XR_VM_SPAWN_CONT) {
        XrCoroutine *child = coro->pending_spawn;
        coro->pending_spawn = NULL;
        xr_coro_resume_store(coro, XR_RESUME_CONTINUATION);
        // Ensure worker threads are started (lazy init).
        // Without this, JIT-compiled children that never yield
        // won't trigger the yield_streak threshold that normally
        // starts workers, leaving only Worker 0 active.
        xr_runtime_ensure_workers(p->runtime);
        // Reset yield_streak: yields during spawn loop don't count
        // toward compute-bound pressure detection.
        p->yield_streak = 0;
        // Clear jit_ctx: parent exited JIT (SPAWN_CONT returns from interpreter).
        // Prevents GC from scanning stale jit_scratch (child will overwrite it).
        // Re-set by xr_coro_run_on_worker when parent resumes.
        coro->jit_ctx = NULL;
        if (!xr_steal_queue_push(&p->cont_deque, coro)) {
            xr_worker_push(worker, coro);
        } else {
            // Wake an idle worker so it can steal the continuation.
            // Without this, JIT-compiled children that never yield
            // monopolize the current worker, and parked workers
            // never discover the parent continuation in cont_deque.
            XrRuntime *_rt = p->runtime;
            if (_rt && atomic_load_explicit(&_rt->spinning_count,
                                            memory_order_relaxed) == 0) {
                wake_idle_worker(_rt);
            }
        }
        coro = child;
        m->current_coro = coro;
        goto exec_fast;
    }

    // BLOCKED fast re-dispatch: skip full handle_vm_result/reductions tracking
    // for maximum throughput. Optimal for serial message chains (pingpong, ring).
    // BLOCKED flag already set by run_on_worker/run_cfunc_coro.
    if (result == XR_VM_BLOCKED && atomic_load_explicit(&p->lifo_slot, memory_order_relaxed) && --fast_dispatch_budget > 0) {
        SCHED_TRACE_CORO(worker, coro, "fast_dispatch_blocked");
        p->yield_streak = 0;
        worker_process_blocked(worker, coro);

        // Periodic lightweight housekeeping during fast dispatch
        if ((fast_dispatch_budget & 7) == 0) {
            worker_drain_inbox(worker);  // O(1) if empty
        }
        if ((fast_dispatch_budget & 15) == 0) {
            int64_t _now = xr_monotonic_ticks();
            if (p->timer_wheel && _now > p->last_timer_tick) {
                xr_bump_timers(p->timer_wheel, _now);
                p->last_timer_tick = _now;
            }
        }

        p->executed_count++;
        XrCoroutine *next = atomic_load_explicit(&p->lifo_slot, memory_order_relaxed);
        // Invariant: LIFO slot must not be NULL here (checked in condition above)
        XR_DCHECK(next != NULL, "fast_dispatch: LIFO slot NULL after positive check");
        // Invariant: next coro must not be DONE
        XR_DCHECK(!xr_coro_flags_has(next, XR_CORO_FLG_DONE),
                  "fast_dispatch: LIFO slot contains DONE coroutine");
        atomic_store_explicit(&p->lifo_slot, NULL, memory_order_relaxed);
        p->local_runq_len--;
        m->current_coro = next;
        coro = next;
        goto exec_fast;  // Skip active_coros, reductions, full handle_vm_result
    }

    // Check cancel flag (sysmon may have marked it)
    if (xr_coro_flags_has(coro, XR_CORO_FLG_CANCEL_REQUESTED)) {
        result = XR_VM_CANCELLED;
    }

    m->current_coro = NULL;
    p->executed_count++;

    // Race guard: when VM returns BLOCKED after channel_recv/send, the coro
    // has already been added to channel waitq (spinlock released). Another
    // thread may have woken it and started executing it on a different worker.
    // In that case we must NOT touch any coro fields — the coro is "gone".
    if (result == XR_VM_BLOCKED &&
        (xr_coro_resume_load(coro) == XR_RESUME_CHANNEL ||
         xr_coro_resume_load(coro) == XR_RESUME_CHANNEL_CLOSED ||
         (xr_coro_flags_load(coro) & XR_CORO_FLG_READY))) {
        // Coro already woken by another thread — skip all coro field access
        p->local_active_coros--;
        goto pop_continuation;
    }

    // Reductions tracking (safe: coro is still owned by this worker)
    int reds_used = XR_CORO_REDUCTIONS - coro->reductions;
    if (reds_used < 0) reds_used = XR_CORO_REDUCTIONS;
    int prio = xr_coro_get_priority(xr_coro_flags_load(coro));
    xr_worker_reductions_executed(worker, prio, reds_used);

    // Handle VM result
    worker_handle_vm_result(worker, coro, result);

    // Deferred recycle: fire-and-forget coro completed, defer to next pool_get.
    // gc_flags bit 2 = recyclable (set by vm_spawn_cont for fire-and-forget go).
    // Push to pending linked list (via coro->next) — flushed in pool_get.
    if (result == XR_VM_OK && (coro->gc_flags & XR_CORO_GC_RECYCLABLE) &&
        !xr_coro_flags_has(coro, XR_CORO_FLG_MAIN)) {
        coro->next = p->pending_recycle_coro;
        p->pending_recycle_coro = coro;
    }

    p->local_active_coros--;

pop_continuation:
    // Pop parent continuation (LIFO)
    {
        XrCoroutine *parent = xr_steal_queue_pop(&p->cont_deque);
        if (parent) {
            coro = parent;
            goto cont_exec;
        }
    }
}

// ========== Worker Implementation ==========

// Initialize Worker (P fields + bind M)
void xr_worker_init(XrWorker *worker, int id, XrRuntime *runtime) {
    XR_DCHECK(worker != NULL, "worker_init: NULL worker");
    XR_DCHECK(runtime != NULL, "worker_init: NULL runtime");
    XR_DCHECK(id >= 0 && id < runtime->worker_count, "worker_init: invalid id");
    worker->p.id = id;
    worker->p.runtime = runtime;
    worker->p.executed_count = 0;
    worker->p.stolen_count = 0;
    worker->p.yielded_count = 0;

    // Bind pre-allocated M (1:1 with Worker at startup)
    worker->m = &runtime->machines[id];
    xr_machine_init(worker->m, id, runtime);

    // Initialize Chase-Lev deque run queues
    for (int p = 0; p < XR_RUNQ_COUNT; p++) {
        xr_runq_init(&worker->p.runq[p]);
    }

    // Initialize LIFO slot
    atomic_store_explicit(&worker->p.lifo_slot, NULL, memory_order_relaxed);
    worker->p.lifo_polls = 0;

    // Initialize random seed
    worker->p.rng_state = (uint32_t)(time(NULL) ^ (id * 0x9e3779b9));

    // Initialize local coroutine object pool
    worker->p.local_free_list = NULL;
    worker->p.local_free_count = 0;

 // Initialize Per-Worker Timer Wheel (lock-free, owner-private)
    worker->p.timer_wheel = xr_timer_wheel_create(runtime, id);
    worker->p.last_timer_tick = xr_monotonic_ticks();

    // Initialize MPSC inbox
    xr_mpsc_init(&worker->p.inbox);
    worker->p.check_balance_reds = XR_CALL_CHECK_BALANCE_REDS;

    // Initialize continuation stealing deque
    xr_steal_queue_init(&worker->p.cont_deque, 64);
    worker->p.cont_steal_count = 0;

    // Initialize Per-Worker blocked queue (lock-free)
    memset(worker->p.blocked_buckets, 0, sizeof(worker->p.blocked_buckets));
    worker->p.blocked_head = NULL;
    worker->p.blocked_tail = NULL;
    worker->p.blocked_count = 0;

    // Initialize run queue statistics
    for (int p = 0; p < XR_RUNQ_COUNT; p++) {
        worker->p.runq_reds[p] = 0;
        worker->p.runq_max_len[p] = 0;
    }

#ifdef XRAY_HAS_JIT
    // Allocate guard page for JIT safepoint (one per worker)
    worker->p.jit_scratch.safepoint_page = jit_guard_page_alloc();
    worker->p.jit_scratch.safepoint_return_pc = NULL;
#endif
}

// Destroy Worker
void xr_worker_destroy(XrWorker *worker) {
    XR_DCHECK(worker != NULL, "worker_destroy: NULL worker");
    // Destroy bound M (VM context, park mutex/cond, strbuf)
    if (worker->m) {
        xr_machine_destroy(worker->m);
    }

#ifdef XRAY_HAS_JIT
    // Free guard page for JIT safepoint
    if (worker->p.jit_scratch.safepoint_page) {
        jit_guard_page_free(worker->p.jit_scratch.safepoint_page);
        worker->p.jit_scratch.safepoint_page = NULL;
    }
#endif

    // Free Per-Worker Timer Wheel
    if (worker->p.timer_wheel) {
        xr_timer_wheel_destroy(worker->p.timer_wheel);
        worker->p.timer_wheel = NULL;
    }

    // Free Per-Worker blocked buckets (hash table of XrBlockedBucket)
    for (int i = 0; i < XR_BLOCKED_BUCKET_SIZE; i++) {
        XrBlockedBucket *bucket = worker->p.blocked_buckets[i];
        while (bucket) {
            XrBlockedBucket *next = bucket->next;
            xr_free(bucket);
            bucket = next;
        }
        worker->p.blocked_buckets[i] = NULL;
    }

    // Free Per-Worker CoroGC free list
    while (worker->p.gc_free_list) {
        XrCoroGC *gc = worker->p.gc_free_list;
        worker->p.gc_free_list = *(XrCoroGC**)gc;
        xr_free(gc);
    }
    worker->p.gc_free_count = 0;

    // Free Per-Worker stack slab free list
    while (worker->p.stack_slab_free) {
        void *block = worker->p.stack_slab_free;
        worker->p.stack_slab_free = *(void**)block;
        xr_free(block);
    }
    worker->p.stack_slab_count = 0;

}

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

 // Mark timer active and record ownership ()
    atomic_store_explicit(&text->timer_active, true, memory_order_relaxed);
    text->timer_wheel_owner = worker->p.id;  // Record which worker owns this timer

    // Calculate timeout position
    int64_t timeout_pos = xr_monotonic_ticks() + delay_ms;

    // Set timer (must be called from owner worker)
    XR_DBG_TIMER("Worker set_timer: tw=%p, timeout_pos=%lld, tw->pos=%lld, owner=%d",
                 (void*)tw, (long long)timeout_pos, (long long)tw->pos, worker->p.id);
    xr_twheel_set_timer(tw, timer, worker_sleep_timeout_callback, coro, timeout_pos);
}

// Cancel timer - handles cross-worker case via async queue ()
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

// Max consecutive LIFO slot pops before flushing to run queue.
// Prevents starvation in ping-pong workloads.
#define XR_MAX_LIFO_POLLS 3

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
        if (!nq->overflow_first) nq->overflow_last = NULL;
        nq->overflow_len--;
        c->sched_link = NULL;
        worker->p.local_runq_len--;
        return c;
    }

    // 3. NORMAL deque (with retry loop for LOW priority delay)
retry:
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
        goto retry;
    }

    // 4. Drain overflow back into deque when deque has space
    while (!c && nq->overflow_first) {
        c = nq->overflow_first;
        nq->overflow_first = c->sched_link;
        if (!nq->overflow_first) nq->overflow_last = NULL;
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

    if (c) worker->p.local_runq_len--;
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
    int priority = xr_coro_get_priority(xr_coro_flags_load(coro));
    if (priority < 0) priority = 0;
    if (priority >= XR_CORO_PRIORITY_COUNT) priority = XR_CORO_PRIORITY_COUNT - 1;

    // LOW goes to NORMAL queue, controlled by schedule_count
    int runq_idx;
    if (priority == CORO_PRIORITY_LOW) {
        coro->schedule_count = XR_RESCHEDULE_LOW;  // 8 times delay
        runq_idx = 0;  // NORMAL queue
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

// Worker work stealing integrated into worker_loop, using P's queues

// Worker park wait
//
// Key fix: recheck for work after acquiring lock
// This eliminates race window between check and sleep

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

// Forward declaration
void *sysmon_thread_func(void *arg);

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

    // Push to idle stack before parking (O(1) wake-up source)
    idle_worker_push(runtime, worker->p.id);

    atomic_store(&worker->m->state, M_PARKING);
    atomic_fetch_sub(&runtime->active_workers, 1);

    // Check needspinning flag (set by last spinner)
    if (atomic_exchange_explicit(&runtime->needspinning, 0, memory_order_acquire)) {
        idle_worker_remove(runtime, worker->p.id);
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
                xr_coro_flags_swap(io_coro, XR_CORO_FLG_BLOCKED, XR_CORO_FLG_READY);
                xr_worker_push_lifo(worker, io_coro);
                io_coro = next;
            }
            // Found IO work, abort park
            idle_worker_remove(runtime, worker->p.id);
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

    // Remove from idle stack on self-wake (timedwait timeout or signaled)
    idle_worker_remove(runtime, worker->p.id);

    // Check if runtime is stopping before accessing runtime structures
    if (atomic_load(&runtime->running)) {
        atomic_fetch_add(&runtime->active_workers, 1);
        atomic_store(&worker->m->state, M_IDLE);
    }
}

// Wake Worker
void worker_unpark(XrWorker *worker) {
    XrMachine *m = worker->m;
    if (!m) return;  // M detached during handoff
    atomic_store_explicit(&m->park_state, XR_PARK_WOKEN, memory_order_release);
    xr_park_futex_wake(&m->park_state);
}

// Worker main loop (GMP model)
//
// M (Worker) must acquire P (Processor) to execute G (Goroutine/coroutine)
// When M blocks, can release P to other M (Hand Off)
void *worker_loop(void *arg) {
    XrWorker *worker = (XrWorker *)arg;
    XrRuntime *runtime = worker->p.runtime;

    // Save pointers needed in exit_loop early (avoid access after runtime destroyed)
    _Atomic bool *running_ptr = &runtime->running;

    // Set TLS
    tls_current_worker = worker;
    tls_current_machine = worker->m;

    // CPU affinity: bind Worker to specific CPU core (reduce cache misses)
#ifdef __APPLE__
    // macOS: use thread affinity policy (advisory, not mandatory)
    thread_affinity_policy_data_t policy = { worker->p.id };
    thread_policy_set(pthread_mach_thread_np(pthread_self()),
                      THREAD_AFFINITY_POLICY,
                      (thread_policy_t)&policy, 1);
#elif defined(__linux__)
    // Linux: use CPU affinity
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(worker->p.id % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#elif defined(_WIN32)
    // Windows: use SetThreadAffinityMask
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    DWORD_PTR mask = 1ULL << (worker->p.id % sysinfo.dwNumberOfProcessors);
    SetThreadAffinityMask(GetCurrentThread(), mask);
#endif

    // Two counters: started_workers for startup sync, active_workers for GC coordination
    atomic_fetch_add(&runtime->started_workers, 1);
    while (atomic_load(&runtime->running)) {
        XrCoroutine *coro = NULL;

        // enter scheduling loop directly, no need to acquire P
        int poll_skip = 0;
        while (atomic_load(&runtime->running)) {
            coro = NULL;

            // Anti-starvation: probabilistically drain inbox BEFORE local pop.
            // With probability 1/(2*worker_count), check inbox first so that
            // cross-worker deliveries are not starved by a full local queue.
            if (xorshift32(&worker->p.rng_state) % (2 * runtime->worker_count) == 0) {
                worker_drain_inbox(worker);
            }

            // fast path: check local queue first.
            // If work is available, skip heavy polling but still drain inbox.
            coro = xr_worker_pop(worker);
            if (coro) {
                // Light housekeeping: drain inbox even on fast path
                // (cross-worker wake deliveries must not be delayed)
                worker_drain_inbox(worker);
                goto found_work;
            }

            // Slow path: no local work, do full housekeeping
            if (poll_skip <= 0) {
                worker_poll_sources(worker);
            } else {
                worker_drain_inbox(worker);
                poll_skip--;
            }

            // Load balancing (reductions-based, infrequent)
            if (worker->p.check_balance_reds <= 0) {
                xr_check_balance(runtime, worker);
            }

            // Per-priority migration check (only when no local work)
            {
                bool need_emigrate = false;
                bool need_immigrate = true;  // We already know queue is empty
                for (int p = 0; p < XR_RUNQ_COUNT; p++) {
                    XrMigrationLimit *ml = &runtime->migration_paths[worker->p.id].prio[p];
                    int len = xr_runq_len(&worker->p.runq[p]);
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
                }
                else if (need_immigrate) try_immigrate(worker);
            }

            if (!atomic_load(running_ptr)) goto exit_loop;

            // Re-check after housekeeping (inbox drain may have added work)
            coro = xr_worker_pop(worker);

            // ========== Work stealing (2 rounds, time-aware) ==========
            // Only steal tasks older than XR_STEAL_TIME_RESOLUTION_MS to
            // preserve cache locality. Fresh tasks stay on the submitting
            // worker for better L1/L2 hits.
            if (!coro && atomic_load(running_ptr)) {
                atomic_store(&worker->m->state, M_STEALING);
                int64_t steal_now = xr_monotonic_ticks();
                int64_t min_steal_delay = 0;

                for (int round = 0; round < 2 && !coro; round++) {
                    uint32_t start = xorshift32(&worker->p.rng_state) % runtime->worker_count;

                    for (int j = 0; j < runtime->worker_count && !coro; j++) {
                        if (!atomic_load(running_ptr)) goto exit_loop;
                        int i = (start + j) % runtime->worker_count;
                        if (i == worker->p.id) continue;

                        XrWorker *victim = &runtime->workers[i];

                        // Steal from all run queues (with freshness check)
                        for (int p = 0; p < XR_RUNQ_COUNT && !coro; p++) {
                            // Time-aware peek: skip if oldest item is too fresh
                            XrCoroutine *oldest = xr_steal_queue_peek_top(&victim->p.runq[p].deque);
                            if (oldest) {
                                int64_t age = steal_now - oldest->submit_time;
                                if (age < XR_STEAL_TIME_RESOLUTION_MS) {
                                    // Task is fresh, record delay for potential timed park
                                    int64_t delay = XR_STEAL_TIME_RESOLUTION_MS - age;
                                    if (min_steal_delay == 0 || delay < min_steal_delay)
                                        min_steal_delay = delay;
                                    continue;
                                }
                            }

                            int stolen = xr_runq_steal(
                                &victim->p.runq[p],
                                &worker->p.runq[p],
                                50);

                            if (stolen > 0) {
                                worker->p.local_runq_len += stolen;
                                worker->p.stolen_count += stolen;
                                coro = xr_worker_pop(worker);
                            }
                        }

                        // Steal from continuation deque (no freshness check).
                        // Critical for JIT parallelism: when a child runs
                        // JIT-compiled code without yielding, the parent
                        // continuation sits in cont_deque unreachable by
                        // runq stealing. Stealing it lets another worker
                        // spawn the next child in parallel.
                        if (!coro) {
                            XrCoroutine *cont = xr_steal_queue_steal(&victim->p.cont_deque);
                            if (cont) {
                                worker->p.cont_steal_count++;
                                coro = cont;
                            }
                        }
                    }
                }

                // If items exist but all too fresh, brief yield to avoid busy-wait
                if (!coro && min_steal_delay > 0) {
                    sched_yield();
                }
            }

            // 4. Spinning: enter spinning state to find work
            if (!coro) {
                // Enter spinning state (if not already)
                // Limit: at most half of non-idle workers may spin
                if (!worker->m->spinning) {
                    int cur_spin = atomic_load_explicit(&runtime->spinning_count, memory_order_relaxed);
                    int npidle = runtime->worker_count - cur_spin;
                    if (2 * cur_spin < npidle) {
                        worker->m->spinning = true;
                        atomic_fetch_add(&runtime->spinning_count, 1);
                    }
                }

                if (worker->m->spinning) {
                    #define XR_SPIN_COUNT 20
                    int64_t cached_now = xr_monotonic_ticks();

                    for (int spin = 0; spin < XR_SPIN_COUNT && !coro; spin++) {
                        if (!atomic_load(running_ptr)) {
                            goto exit_loop;
                        }

                        // Drain MPSC inbox (I/O ready + cross-worker deliveries)
                        worker_drain_inbox(worker);

                        // Advance Timer Wheel (callbacks may push to run queue)
                        if ((spin & 0x3) == 0) {
                            cached_now = xr_monotonic_ticks();
                        }
                        if (worker->p.timer_wheel && cached_now > worker->p.last_timer_tick) {
                            xr_bump_timers(worker->p.timer_wheel, cached_now);
                            worker->p.last_timer_tick = cached_now;
                        }

                        coro = xr_worker_pop(worker);
                    }
                }
            }

            // 5. If coroutine found, execute it
            found_work:
            if (coro) {
                // Check: prevent re-executing completed coroutine
                if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) {
                    SCHED_TRACE_CORO(worker, coro, "skip_done");
                    continue;  // Skip completed coroutine
                }

                // Go protocol: exit spinning state after finding work
                if (worker->m->spinning) {
                    worker->m->spinning = false;
                    int prev_spin = atomic_fetch_sub(&runtime->spinning_count, 1);
 // resetspinning: if we were the last spinner,
                    // wake another worker to ensure continuous work scanning.
                    // Without this, new work submitted while all workers are
                    // busy may sit in queues with no spinner to discover it.
                    if (prev_spin == 1) {
                        wake_idle_worker(runtime);
                    }
                }

                // Adaptive polling: EWMA-based continuous poll_skip.
                // High EWMA (IO heavy) → low skip, low EWMA (CPU heavy) → high skip.
                // io_poll_ewma range [0,256], poll_skip range [1,8].
                poll_skip = 8 - (int)(worker->p.io_poll_ewma >> 5);
                if (poll_skip < 1) poll_skip = 1;

                atomic_store(&worker->m->state, M_RUNNING);

                worker_exec_with_cont_stealing(worker, coro);
                // Detect compute-bound pressure: if coroutines keep yielding
                // (not blocking), the run queue stays full. Start workers
                // to steal and parallelize. Threshold = worker_count ensures
                // only sustained compute pressure triggers, not brief bursts.
                if (worker->p.yield_streak >= runtime->worker_count) {
                    xr_runtime_ensure_workers(runtime);
                    worker->p.yield_streak = 0;
                }
                continue;
            }

            // No work found: ensure next iteration polls
            poll_skip = 0;

            // 6. No task after spinning, release P and sleep
            // Check if exit needed first (avoid accessing runtime during exit)
            if (!atomic_load(running_ptr)) {
                // Stopping, exit directly
                goto exit_loop;
            }

            // no P, sleep directly
            worker_park(worker);

            // Check if exit needed after waking
            if (!atomic_load(running_ptr)) {
                goto exit_loop;
            }

            // Continue scheduling loop
        }
    }

exit_loop:
    // Signal that this worker has fully exited
    // This must be done BEFORE returning so xr_runtime_stop can wait for all workers
    atomic_fetch_add(&runtime->exited_workers, 1);

    return NULL;
}

// ========== Runtime Implementation ==========

// Create Runtime
XrRuntime *xr_runtime_create(XrayIsolate *isolate, int num_workers) {
    XR_DCHECK(isolate != NULL, "runtime_create: NULL isolate");
    if (num_workers <= 0) {
        // Allow override via environment variable (for benchmarking)
        const char *env = getenv("XRAY_WORKERS");
        if (env && atoi(env) > 0) {
            num_workers = atoi(env);
        } else {
            // Default to CPU core count
            num_workers = (int)sysconf(_SC_NPROCESSORS_ONLN);
            if (num_workers <= 0) num_workers = 1;
        }
    }
    if (num_workers > XR_MAX_WORKERS) {
        num_workers = XR_MAX_WORKERS;
    }

    XrRuntime *runtime = (XrRuntime *)xr_calloc(1, sizeof(XrRuntime));
    if (!runtime) return NULL;

    runtime->isolate = isolate;
    runtime->worker_count = num_workers;
    atomic_store(&runtime->running, false);
    atomic_store(&runtime->threads_started, false);
    atomic_store(&runtime->started_workers, 0);
    atomic_store(&runtime->exited_workers, 0);
    atomic_store(&runtime->active_workers, 0);

    // Initialize idle P/M management
    pthread_mutex_init(&runtime->sched_lock, NULL);
    runtime->idle_p_head = NULL;
    atomic_store(&runtime->idle_p_count, 0);
    runtime->idle_m_head = NULL;
    atomic_store(&runtime->idle_m_count, 0);
    atomic_store(&runtime->m_count, num_workers);
    runtime->idle_worker_count = 0;

    // Initialize Spinning mechanism
    atomic_store(&runtime->spinning_count, 0);
    atomic_store(&runtime->wake_spinner, 0);
    atomic_store(&runtime->needspinning, 0);
    // no global queue, use Worker local queue + MPSC inbox directly

    // active_coros/spawned now tracked per-Worker, no global init needed
    atomic_store(&runtime->total_inbox_len, 0);

    // main thread enqueues via inbox, no dedicated P needed

    // coroutine pool fully Per-Worker, no global init needed

    // Initialize Netpoll (I/O multiplexing)
    memset(&runtime->netpoll, 0, sizeof(XrNetpoll));
    if (xr_netpoll_init(&runtime->netpoll) < 0) {
        // Netpoll init failure is not fatal, continue running
    }

    // blocked queue fully Per-Worker, no global init needed
    atomic_store(&runtime->next_coro_id, 1);  // ID starts from 1
    runtime->current_scope = NULL;

    // Allocate Machines (M) array — pre-allocated 1:1 with Workers
    runtime->machines = (XrMachine *)xr_calloc(num_workers, sizeof(XrMachine));
    if (!runtime->machines) {
        xr_free(runtime);
        return NULL;
    }

    // Allocate Workers (P + M* pointer)
    runtime->workers = (XrWorker *)xr_calloc(num_workers, sizeof(XrWorker));
    if (!runtime->workers) {
        xr_free(runtime->machines);
        xr_free(runtime);
        return NULL;
    }

    // Initialize Workers (binds each Worker to its pre-allocated M)
    for (int i = 0; i < num_workers; i++) {
        xr_worker_init(&runtime->workers[i], i, runtime);
    }

    // Per-Worker Timer Wheel, lock-free high performance
    // Sysmon heartbeat monitoring runs on netpoll thread

    // Create async thread pool
    // For blocking syscalls (file I/O, DNS, etc.)
    runtime->async_pool = (XrAsyncPool *)xr_calloc(1, sizeof(XrAsyncPool));
    if (runtime->async_pool) {
        xr_async_pool_init(runtime->async_pool, runtime, XR_ASYNC_THREAD_COUNT);
    }

    // initialize load balancing module
    xr_balance_init(runtime);

    return runtime;
}

// Destroy Runtime
void xr_runtime_destroy(XrRuntime *runtime) {
    if (!runtime) return;

    // Stop running (internally calls pthread_join to wait for all Workers to exit)
    xr_runtime_stop(runtime);

    // Leak detection: check for unreleased coroutines after all workers stopped
    int active = xr_runtime_active_coros(runtime);
    if (active > 0) {
        uint64_t spawned = 0;
        for (int _wi = 0; _wi < runtime->worker_count; _wi++)
            spawned += runtime->workers[_wi].p.spawned_count;
        uint64_t completed = 0;
        for (int _wi = 0; _wi < runtime->worker_count; _wi++)
            completed += runtime->workers[_wi].p.completed_count;
        xr_log_warning("runtime", "%d coroutine(s) leaked "
                "(spawned=%llu, completed=%llu)",
                active, (unsigned long long)spawned,
                (unsigned long long)completed);
        for (int i = 0; i < runtime->worker_count; i++) {
            XrProc *p = &runtime->workers[i].p;
            int runq_len = xr_proc_total_queue_len(p);
            if (runq_len > 0 || p->blocked_count > 0) {
                xr_log_warning("runtime", "  W%d: runq=%d blocked=%d",
                        i, runq_len, p->blocked_count);
            }
        }
    }

    // Channel leak detection: compare create vs close counts
    {
        uint64_t ch_closed = xr_channel_get_close_count();
        if (runtime->isolate && runtime->isolate->sys_heap) {
            uint64_t ch_created = atomic_load(
                &runtime->isolate->sys_heap->stats.channel_create_count);
            if (ch_created > ch_closed) {
                xr_log_warning("runtime", "%llu channel(s) not closed "
                        "(created=%llu, closed=%llu)",
                        (unsigned long long)(ch_created - ch_closed),
                        (unsigned long long)ch_created,
                        (unsigned long long)ch_closed);
            }
        }
    }

    // Drain all worker MPSC inboxes (coroutines pushed after running=false)
    for (int i = 0; i < runtime->worker_count; i++) {
        XrCoroutine *orphan = xr_mpsc_drain(&runtime->workers[i].p.inbox);
        while (orphan) {
            XrCoroutine *next = orphan->sched_link;
            orphan->sched_link = NULL;
            orphan = next;
        }
    }

    // Destroy Workers (also destroys bound M via xr_worker_destroy)
    for (int i = 0; i < runtime->worker_count; i++) {
        xr_worker_destroy(&runtime->workers[i]);
    }
    xr_free(runtime->workers);
    xr_free(runtime->machines);

    // blocked queue cleaned up when Worker destroyed

    // Cleanup Netpoll
    xr_netpoll_cleanup(&runtime->netpoll);

    // Destroy async thread pool
    if (runtime->async_pool) {
        xr_async_pool_destroy(runtime->async_pool);
        xr_free(runtime->async_pool);
        runtime->async_pool = NULL;
    }

    xr_free(runtime);
}

// Start Runtime (state only, no threads created)
//
// Threads are created lazily by xr_runtime_ensure_workers() on first spawn.
// Worker 0 runs on main thread via xr_main_thread_run().
void xr_runtime_start(XrRuntime *runtime) {
    XR_DCHECK(runtime != NULL, "runtime_start: NULL runtime");
    if (atomic_load(&runtime->running)) return;
    atomic_store(&runtime->running, true);
}

// Lazy start: create sysmon + Worker 1~N + async pool threads.
// Called on first spawn. Thread-safe via atomic CAS.
void xr_runtime_ensure_workers(XrRuntime *runtime) {
    XR_DCHECK(runtime != NULL, "runtime_ensure_workers: NULL runtime");
    // Fast path: already started
    if (atomic_load_explicit(&runtime->threads_started, memory_order_acquire)) {
        return;
    }

    // Slow path: CAS to claim the right to start
    bool expected = false;
    if (!atomic_compare_exchange_strong(&runtime->threads_started, &expected, true)) {
        // Another thread won the race, wait for them to finish
        int expected_workers = runtime->worker_count - 1;
        while (atomic_load(&runtime->started_workers) < expected_workers) {
            sched_yield();
        }
        return;
    }

    // Start sysmon thread (heartbeat monitoring + stuck detection)
    pthread_create(&runtime->sysmon_thread, NULL, sysmon_thread_func, runtime);

    // Start async pool threads
    if (runtime->async_pool) {
        xr_async_pool_start_threads(runtime->async_pool);
    }

    // Worker threads need larger stack for nested run() calls (e.g. module import)
    // and ASan instrumentation which greatly inflates stack frame sizes.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);  // 8MB

    // Create Worker 1~N threads (Worker 0 runs on main thread)
    for (int i = 1; i < runtime->worker_count; i++) {
        pthread_create(&runtime->workers[i].m->thread, &attr, worker_loop, &runtime->workers[i]);
    }
    pthread_attr_destroy(&attr);

    // Wait for Worker 1~N to become ready
    int expected_workers = runtime->worker_count - 1;
    while (atomic_load(&runtime->started_workers) < expected_workers) {
        sched_yield();
    }
}

// Stop Runtime
//
// Worker 0 runs on main thread, only need to join Worker 1~N
void xr_runtime_stop(XrRuntime *runtime) {
    XR_DCHECK(runtime != NULL, "runtime_stop: NULL runtime");
    atomic_store(&runtime->running, false);

    // Fast path: no threads were ever created
    if (!atomic_load(&runtime->threads_started)) {
        return;
    }

    // Wake netpoll thread
    if (atomic_load(&runtime->netpoll.inited)) {
        xr_netpoll_break(&runtime->netpoll);
    }
    pthread_join(runtime->sysmon_thread, NULL);

    // Wake all Workers to check running flag and exit
    for (int i = 0; i < runtime->worker_count; i++) {
        worker_unpark(&runtime->workers[i]);
    }

    // Wake all idle M threads parked in handoff_thread_entry
    {
        pthread_mutex_lock(&runtime->sched_lock);
        XrMachine *idle = runtime->idle_m_head;
        while (idle) {
            atomic_store_explicit(&idle->park_state, XR_PARK_WOKEN, memory_order_release);
            xr_park_futex_wake(&idle->park_state);
            idle = idle->idle_link;
        }
        pthread_mutex_unlock(&runtime->sched_lock);
    }

    // Join Worker 1~N (guard against NULL m during handoff)
    for (int i = 1; i < runtime->worker_count; i++) {
        XrMachine *wm = runtime->workers[i].m;
        if (wm) pthread_join(wm->thread, NULL);
    }
}

// Print per-worker scheduling statistics (for performance tuning)
void xr_runtime_print_stats(XrRuntime *runtime) {
    if (!runtime) return;

    fprintf(stderr, "\n=== Xray Runtime Statistics ===\n");
    fprintf(stderr, "Workers: %d\n", runtime->worker_count);
    { uint64_t _tc = 0;
    for (int _wi = 0; _wi < runtime->worker_count; _wi++)
        _tc += runtime->workers[_wi].p.completed_count;
    uint64_t _ts = 0;
    for (int _wi2 = 0; _wi2 < runtime->worker_count; _wi2++)
        _ts += runtime->workers[_wi2].p.spawned_count;
    fprintf(stderr, "Total spawned: %llu, completed: %llu\n",
            (unsigned long long)_ts,
            (unsigned long long)_tc); }
    fprintf(stderr, "Active coros: %d\n",
            xr_runtime_active_coros(runtime));
    fprintf(stderr, "\n%-8s %12s %12s %12s %12s %8s\n",
            "Worker", "Executed", "Stolen", "Yielded", "ContSteal", "Blocked");
    fprintf(stderr, "%-8s %12s %12s %12s %12s %8s\n",
            "------", "--------", "------", "-------", "---------", "-------");

    uint64_t total_exec = 0, total_steal = 0, total_yield = 0, total_cont = 0;
    for (int i = 0; i < runtime->worker_count; i++) {
        XrProc *p = &runtime->workers[i].p;
        fprintf(stderr, "W%-7d %12llu %12llu %12llu %12llu %8d\n",
                i,
                (unsigned long long)p->executed_count,
                (unsigned long long)p->stolen_count,
                (unsigned long long)p->yielded_count,
                (unsigned long long)p->cont_steal_count,
                p->blocked_count);
        total_exec += p->executed_count;
        total_steal += p->stolen_count;
        total_yield += p->yielded_count;
        total_cont += p->cont_steal_count;
    }
    fprintf(stderr, "%-8s %12llu %12llu %12llu %12llu\n",
            "TOTAL", (unsigned long long)total_exec,
            (unsigned long long)total_steal,
            (unsigned long long)total_yield,
            (unsigned long long)total_cont);
    fprintf(stderr, "===========================\n\n");
}

// Force-stop Runtime without joining threads.
// Safe to call from external watchdog thread while main thread is in worker_loop.
// Also sets CANCEL_REQUESTED on all currently running coroutines so that
// JIT safepoints and interpreter back-edges can detect and bail out.
void xr_runtime_force_stop(XrRuntime *runtime) {
    if (!runtime) return;
    atomic_store(&runtime->running, false);
    for (int i = 0; i < runtime->worker_count; i++) {
        XrCoroutine *coro = runtime->workers[i].m
            ? runtime->workers[i].m->current_coro : NULL;
        if (coro) {
            xr_coro_flags_set(coro, XR_CORO_FLG_CANCEL_REQUESTED);
        }
        worker_unpark(&runtime->workers[i]);
    }
}

// Spawn coroutine into Runtime scheduling
//
// Affinity design:
// - New coroutines prefer creator's P local queue
// - Improves cache hit rate, reduces coroutine migration overhead
void xr_runtime_spawn(XrRuntime *runtime, XrCoroutine *coro) {
    XR_DCHECK(runtime != NULL, "runtime_spawn: NULL runtime");
    XR_DCHECK(coro != NULL, "runtime_spawn: NULL coro");
    // Lazy start: ensure worker threads are running before spawning
    xr_runtime_ensure_workers(runtime);

 // runnext: new coroutine goes to LIFO slot for DFS execution.
    // Previous occupant evicted to FIFO queue (available for work stealing).
    XrWorker *current = xr_current_worker();
    if (current && current->p.runtime == runtime) {
        current->p.spawned_count++;
        atomic_store_explicit(&coro->affinity_p, current->p.id, memory_order_relaxed);
        xr_worker_push_lifo(current, coro);
        XR_DBG_CORO("spawn: coro id=%d enqueued to Worker %d", coro->id, current->p.id);
        return;
    }

    // select lowest load Worker, enqueue via inbox

    // no global queue, always enqueue via inbox
    // Select target Worker (lowest load), fallback to Worker 0 if none available
    XrWorker *target = xr_choose_target_worker(runtime, -1);
    if (!target) {
        // All Workers unavailable, fallback to Worker 0
        target = &runtime->workers[0];
    }

    atomic_store_explicit(&coro->affinity_p, target->p.id, memory_order_relaxed);

    // Enqueue via unified inbox path (MPSC push + Dekker fence + wake)
    xr_worker_inbox_enqueue(runtime, target->p.id, coro);
    XR_DBG_CORO("spawn: coro id=%d enqueued to Worker %d inbox", coro->id, target->p.id);
}

// Spawn coroutine into specified Worker's local queue
void xr_runtime_spawn_local(XrWorker *worker, XrCoroutine *coro) {
    XR_DCHECK(worker != NULL, "runtime_spawn_local: NULL worker");
    XR_DCHECK(coro != NULL, "runtime_spawn_local: NULL coro");
    worker->p.spawned_count++;
    xr_worker_push(worker, coro);
}

// ========== Worker Coroutine Execution ==========

// ---- Stackful coroutine helpers ----

// stackful infrastructure removed — fully stackless now

// Execute Yieldable C function coroutine (supports I/O wait and rescheduling)
static XrVMResult run_cfunc_coro(XrWorker *worker, XrCoroutine *coro,
                                  XrayIsolate *isolate) {
    XrVMContext *ctx = &worker->m->vm_ctx;
    XrVMContext *coro_ctx = &coro->vm_ctx;

    // Set current_coro so worker scheduling can find it.
    ctx->current_coro = coro;
    coro_ctx->current_coro = coro;

    // Single atomic store replaces: flags_clear(READY|BLOCKED) + flags_set(RUNNING)
    uint32_t cur_flags = atomic_load_explicit(&coro->flags, memory_order_relaxed);

    XrVMResult result;

    if (!(cur_flags & XR_CORO_FLG_STARTED)) {
        // First execution: initialize frame and call C function
        atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_RUNNING, memory_order_release);
        atomic_store_explicit(&coro->flags,
            (cur_flags & ~(XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED))
            | XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED,
            memory_order_release);

        // Initialize first frame (for Yieldable support)
        coro_ctx->frame_count = 1;
        XrBcCallFrame *frame = &coro_ctx->frames[0];
        frame->closure = NULL;
        frame->pc = NULL;
        frame->base_offset = 1;  // Reserve stack[0] for return value
        frame->flags = 0;
        frame->u.l.pending_operator_check = false;
        frame->call_status = XR_CALL_C;
        frame->u.c.continuation = NULL;
        frame->u.c.continuation_ctx = NULL;
        frame->u.c.result_slot = 0;
        frame->u.c.has_cfunc_result = false;

        // Copy arguments to frame
        XrValue *base = coro_ctx->stack + frame->base_offset;
        for (int i = 0; i < coro->arg_count && i < 4; i++) {
            base[i] = coro->args[i];
        }

        coro_ctx->stack_top = coro_ctx->stack + 1 + coro->arg_count;

        // Call C function
        XrValue cfunc_result = xr_null();
        XrCFuncResult status = coro->entry.cfunc(isolate, coro->args, coro->arg_count, &cfunc_result);

        if (status == XR_CFUNC_DONE) {
            coro_ctx->stack[0] = cfunc_result;
            result = XR_VM_OK;
        } else if (status == XR_CFUNC_BLOCKED || status == XR_CFUNC_YIELD) {
            result = (status == XR_CFUNC_BLOCKED) ? XR_VM_BLOCKED : XR_VM_YIELD;
        } else if (status == XR_CFUNC_CALL_CLOSURE) {
            // Closure frame pushed by xr_yield_call_closure, execute via VM
            coro_ctx->module_base_frame = 0;
            result = run(isolate, &coro->vm_ctx);
        } else {
            result = XR_VM_RUNTIME_ERROR;
        }
    } else {
        // Resume: single atomic store for flags
        atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_RUNNING, memory_order_release);
        atomic_store_explicit(&coro->flags,
            (cur_flags & ~(XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED))
            | XR_CORO_FLG_RUNNING,
            memory_order_release);

        int resume_status = xr_coro_resume_load(coro);
        if (!resume_status) resume_status = XR_RESUME_IO_READY;

        /* ============================================================
         * INLINE FAST PATH: single C frame with continuation.
         * Covers 99%+ of cfunc IO resumes (HTTP/WS handlers).
         * Avoids function call to xr_coro_resume_with_unroll + while
         * loop + switch overhead.
         * ============================================================ */
        if (coro_ctx->frame_count == 1) {
            XrBcCallFrame *frame = &coro_ctx->frames[0];
            uint8_t need = XR_CALL_C | XR_CALL_HAS_CONT | XR_CALL_YIELDED;
            if ((frame->call_status & need) == need && frame->u.c.continuation) {
                XrContinuation cont = (XrContinuation)frame->u.c.continuation;
                void *user_ctx = frame->u.c.continuation_ctx;
                XrValue cfunc_result;
                XrCFuncResult status = cont(isolate, resume_status, user_ctx, &cfunc_result);

                switch (status) {
                case XR_CFUNC_DONE:
                    // Continuation completed — store result, tear down frame
                    coro_ctx->stack[0] = cfunc_result;
                    frame->call_status &= ~(XR_CALL_C | XR_CALL_HAS_CONT | XR_CALL_YIELDED);
                    frame->u.c.continuation = NULL;
                    coro_ctx->frame_count = 0;
                    result = XR_VM_OK;
                    goto cfunc_handle_result;
                case XR_CFUNC_BLOCKED:
                    result = XR_VM_BLOCKED;
                    goto cfunc_handle_result;
                case XR_CFUNC_YIELD:
                    result = XR_VM_YIELD;
                    goto cfunc_handle_result;
                case XR_CFUNC_CALL_CLOSURE:
                    // Closure frame pushed by continuation, execute via VM
                    coro_ctx->module_base_frame = 0;
                    result = run(isolate, &coro->vm_ctx);
                    goto cfunc_handle_result;
                default:
                    result = XR_VM_RUNTIME_ERROR;
                    goto cfunc_handle_result;
                }
            }
        }

        /* SLOW PATH: multi-frame, bytecode frame, or no continuation.
         * Falls back to full unroll mechanism. */
        result = xr_coro_resume_with_unroll(isolate, coro, resume_status);

        if (result == XR_VM_OK) {
            if (coro_ctx->frame_count == 1) {
                XrBcCallFrame *top = &coro_ctx->frames[0];
                if (top->u.c.has_cfunc_result) {
                    coro_ctx->stack[0] = top->u.c.cfunc_result;
                    coro_ctx->frame_count = 0;
                    result = XR_VM_OK;
                    goto cfunc_handle_result;
                }
            }
            coro_ctx->module_base_frame = 0;
            if (coro_ctx->frame_count == 0) {
                result = XR_VM_OK;
            } else {
                result = run(isolate, &coro->vm_ctx);
            }
        }
    }

cfunc_handle_result:
    // Handle result
    if (result == XR_VM_OK) {
        // Result MUST be stored before DONE flag (release). Otherwise a
        // parent doing acquire-load on DONE may see DONE=true but read a
        // stale (null) result on weakly-ordered architectures (ARM64).
        coro->result = coro_ctx->stack[0];
        xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
    } else if (result == XR_VM_BLOCKED) {
        // Channel blocks: BLOCKED already set inside xr_channel_recv/send
        // under lock.  Non-channel blocks (await,
        // timer): coro is still RUNNING.  CAS handles both correctly.
        uint8_t _exp = XR_CORO_STATE_RUNNING;
        if (atomic_compare_exchange_strong_explicit(
                &coro->coro_state, &_exp, XR_CORO_STATE_BLOCKED,
                memory_order_release, memory_order_relaxed)) {
            atomic_fetch_and_explicit(&coro->flags,
                ~(uint32_t)XR_CORO_FLG_RUNNING, memory_order_relaxed);
            atomic_fetch_or_explicit(&coro->flags,
                (uint32_t)XR_CORO_FLG_BLOCKED, memory_order_release);
        }
    } else if (result == XR_VM_YIELD) {
        xr_coro_flags_swap(coro, XR_CORO_FLG_RUNNING, XR_CORO_FLG_READY);
    }

    ctx->current_coro = NULL;
    return result;
}

// Execute coroutine on Worker thread
//
// refactor: execute directly on coroutine stack, no copying
// - Coroutine has independent stack allocated at creation
// - Worker uses ctx but points to coroutine's stack
// - Eliminates state copy race conditions
XrVMResult xr_coro_run_on_worker(XrWorker *worker, XrCoroutine *coro) {
    if (!worker || !coro) return XR_VM_RUNTIME_ERROR;

    // Strict check: ensure coroutine is executable
    if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) {
        return XR_VM_OK;  // Already done, skip
    }

    // Assert check: ensure runtime and isolate are valid
    XR_DCHECK(worker->p.runtime != NULL, "worker thread: runtime is NULL");
    XR_DCHECK(worker->p.runtime->isolate != NULL, "worker thread: isolate is NULL");

    XrayIsolate *isolate = worker->p.runtime->isolate;
    XrVMContext *ctx = &worker->m->vm_ctx;

    // Set per-Worker JIT scratch pointer (JIT code accesses via coro->jit_ctx)
    coro->jit_ctx = &worker->p.jit_scratch;
    worker->p.jit_scratch.heartbeat_ptr = &worker->m->heartbeat;

    XrVMResult result;  // Declared early for fast path goto
    XrVMContext *coro_ctx = &coro->vm_ctx;  // Declared early: handle_result uses it

    // ========== Channel Resume Fast Path ==========
    // Most common resume case: channel wake of bytecode coroutine.
    // Skip redundant DONE/entry_type/stack checks and inline the unroll.
    //
    // acquire load of flags first: establishes happens-before with the sender's
    // xr_coro_flags_swap(release), which itself happens after the sender writes
    // *recv_slot. This guarantees recv_slot value is visible before we enter run().
    uint32_t _fast_flags = xr_coro_flags_load(coro);
    int _fast_resume = xr_coro_resume_load(coro);
    if (_fast_resume == XR_RESUME_CHANNEL &&
        (_fast_flags & XR_CORO_FLG_STARTED) &&
        !coro->jit_resume_entry) {  // JIT-suspended coros use JIT resume path
        ctx->current_coro = coro;
        coro_ctx->current_coro = coro;
        coro->next = NULL;
        coro->prev = NULL;
        xr_coro_flags_swap(coro, XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED, XR_CORO_FLG_RUNNING);
        // Inline unroll: bytecode frame just needs YIELDED cleared
        if (coro_ctx->frame_count > 0) {
            XrBcCallFrame *tf = &coro_ctx->frames[coro_ctx->frame_count - 1];
            if (tf->call_status & XR_CALL_C) goto slow_resume;  // C continuation: use full unroll
            tf->call_status &= ~XR_CALL_YIELDED;
        }
        coro_ctx->module_base_frame = 0;
        result = run(isolate, coro_ctx);
        goto handle_result;
    }

    // ========== Cfunc First-Run + Resume Fast Path ==========
    // Must check BEFORE closure path: entry union overlaps (cfunc == closure pointer).
    if (coro->entry_type == XR_CORO_ENTRY_CFUNC && coro->entry.cfunc) {
        return run_cfunc_coro(worker, coro, isolate);
    }

    // ========== Closure First Execution Fast Path ==========
    // New closure coroutine (not STARTED, resume==OK): skip DONE/native/stack checks.
    // Use atomic_store instead of CAS — no concurrent modifier for new coroutines.
    if (!(_fast_flags & XR_CORO_FLG_STARTED) && _fast_resume == 0) {
        XrClosure *closure = coro->entry.closure;
        if (closure && closure->proto) {
            ctx->current_coro = coro;
            coro_ctx->current_coro = coro;
            coro->next = NULL;
            coro->prev = NULL;
            // Single atomic_store replaces: flags_swap(CAS) + flags_has(load) + flags_set(OR)
            atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_RUNNING, memory_order_release);
            atomic_store_explicit(&coro->flags,
                (_fast_flags & ~(XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED))
                | XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED,
                memory_order_release);
            goto first_exec_setup;
        }
    }

    slow_resume: ;

    XR_DBG_FRAME("run_on_worker: coro=%p, id=%d, stack=%p, frames=%p, frame_count=%d",
                 (void*)coro, coro->id, (void*)coro->vm_ctx.stack, (void*)coro->vm_ctx.frames, coro->vm_ctx.frame_count);

    // Ensure vm_ctx.isolate matches runtime->isolate
    if (ctx->isolate != isolate) {
        ctx->isolate = isolate;
    }

    // Native coroutine: execute simple C callback (not Yieldable)
    if (coro->entry_type == XR_CORO_ENTRY_NATIVE && coro->entry.native.func) {
        xr_coro_flags_clear(coro, XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED);
        xr_coro_flags_set(coro, XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED);
        ctx->current_coro = coro;
        coro->entry.native.func(coro->entry.native.arg);
        coro->result = xr_null();
        xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
        ctx->current_coro = NULL;
        return XR_VM_OK;
    }

    // C function coroutine: delegate to run_cfunc_coro
    if (coro->entry_type == XR_CORO_ENTRY_CFUNC && coro->entry.cfunc) {
        return run_cfunc_coro(worker, coro, isolate);
    }

    // Safety check: skip completed coroutines (may be woken by timer during exit)
    if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) {
        return XR_VM_OK;
    }

    // Safety check: xray coroutine must have independent stack (allocated at creation)
    if (!coro->vm_ctx.stack || !coro->vm_ctx.frames) {
        xr_log_warning("coro", "coroutine stack not allocated (stack=%p, frames=%p, coro=%p)",
                (void*)coro->vm_ctx.stack, (void*)coro->vm_ctx.frames, (void*)coro);
        coro->result = xr_null();
        xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
        return XR_VM_RUNTIME_ERROR;
    }

    // Set current coroutine (needed for VM Channel operations)
    ctx->current_coro = coro;
    coro_ctx->current_coro = coro;  // Critical: coroutine's vm_ctx also needs setting

    // Ensure coroutine not in blocked queue (allow correct subsequent enqueue)
    coro->next = NULL;
    coro->prev = NULL;

    // Fast path for first execution: non-atomic check (sole owner at spawn time),
    // then single store replaces CAS + load + OR (3 atomics → 1 store).
    uint32_t _flags_snap = atomic_load_explicit(&coro->flags, memory_order_relaxed);
    if (!(_flags_snap & XR_CORO_FLG_STARTED)) {
        // First execution: set RUNNING+STARTED in one store
        atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_RUNNING, memory_order_release);
        atomic_store_explicit(&coro->flags,
            (_flags_snap & ~(uint32_t)(XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED))
            | XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED,
            memory_order_release);

        XrClosure *_slow_cl = coro->entry.closure;

        // Safety check: ensure closure is valid
        if (!_slow_cl || !_slow_cl->proto) {
            xr_log_warning("coro", "coroutine closure invalid (closure=%p, coro=%p)",
                    (void*)_slow_cl, (void*)coro);
            coro->result = xr_null();
            xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
            return XR_VM_RUNTIME_ERROR;
        }

    first_exec_setup: ;
        {
        // Reload closure after label (fast path jumps here with closure already validated)
        XrClosure *closure = coro->entry.closure;
        XrProto *proto = closure->proto;

        // Initialize frame directly on coroutine stack
        XrValue *stack_base = coro_ctx->stack;

        // Reserve return value position
        stack_base[0] = xr_null();
        XrValue *func_base = stack_base + 1;

        // Copy arguments
        for (int i = 0; i < coro->arg_count; i++) {
            func_base[i] = coro->args[i];
        }

        // Create coroutine's first frame
        coro_ctx->frame_count = 1;
        XrBcCallFrame *frame = &coro_ctx->frames[0];
        frame->closure = closure;
        frame->pc = PROTO_CODE_BASE(proto);
        frame->base_offset = (int)(func_base - coro_ctx->stack);
        frame->flags = 0;
        frame->u.l.pending_operator_check = false;
        frame->call_status = 0;
        frame->u.c.continuation = NULL;
        frame->u.c.continuation_ctx = NULL;
        frame->u.c.result_slot = -1;  // -1 means not set, avoid mistaken storage
        frame->u.c.has_cfunc_result = false;

        // Update stack top
        coro_ctx->stack_top = coro_ctx->stack + frame->base_offset + proto->maxstacksize;

        // Set module boundary (coroutine return boundary)
        coro_ctx->module_base_frame = 0;

#ifdef XRAY_HAS_JIT
        // JIT fast path for coroutine entry: call compiled code directly.
        //
        // WHY THIS DESIGN:
        //   go-spawned coroutines bypass OP_CALL, so they never check/install
        //   JIT code. Without this, coroutines always run in the interpreter
        //   even when JIT code exists for the entry function.
        //
        // SAFETY: only call JIT if deopt_count==0 (never deopted before).
        // If first coroutine deopts, deopt_count becomes >0 and all
        // subsequent coroutines skip the JIT call.
        if (proto->numparams == coro->arg_count) {
            // Install pending background JIT compilation.
            // Must use xir_jit_install_bg_result (CAS-protected) instead of
            // inline installation — multiple workers spawn coroutines with
            // the same proto concurrently, racing on jit_entry_pending.
            if (!proto->jit_entry) {
                void *pending = atomic_load_explicit(
                    &proto->jit_entry_pending, memory_order_acquire);
                if (pending && (uintptr_t)pending > 1) {
                    xir_jit_install_bg_result(proto);
                }
            }
            // Call JIT code only if verified safe (never deopted)
            if (proto->jit_entry && proto->deopt_count == 0) {
                coro->jit_ctx->call_proto = proto;
                coro->jit_ctx->call_closure = closure;
                coro->jit_ctx->call_base_offset = (int32_t)(func_base - coro_ctx->stack);
                XrValue jit_result;
                int _jrc5 = xir_jit_call(proto->jit_entry, coro, func_base,
                                 coro->arg_count, proto->return_type_info,
                                 &jit_result);
                if (_jrc5 == XIR_JIT_OK) {
                    coro_ctx->stack[0] = jit_result;
                    result = XR_VM_OK;
                    goto handle_result;
                }
                if (_jrc5 == XIR_JIT_SUSPEND) {
                    result = XR_VM_BLOCKED;
                    goto handle_result;
                }
                // JIT deopt: disable fast path for this proto
                proto->deopt_count++;
            }
        }
#endif

        // Execute - pass coroutine's independent vm_ctx
        result = run(isolate, &coro->vm_ctx);
        goto handle_result;  // Handle result uniformly
        }

    } else {
        // ========== Resume Execution ==========
        // state already on coroutine stack, continue directly
        // Resume path: use CAS (other threads may touch flags concurrently)
        xr_coro_flags_swap(coro, XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED, XR_CORO_FLG_RUNNING);

        XR_DBG_CORO("run_on_worker: resuming coro id=%d", coro->id);

#ifdef XRAY_HAS_JIT
        // JIT suspend/resume: if coro was suspended at an XIR_SUSPEND point,
        // jit_resume_entry is set. Re-enter JIT directly without interpreter unroll.
        if (coro->jit_resume_entry && coro->jit_ctx) {
            int _jit_rs = xr_coro_resume_load(coro);

            // Channel close wake: deopt to bytecode (rare edge case)
            if (_jit_rs == XR_RESUME_CHANNEL_CLOSED) {
                coro->jit_resume_entry = NULL;
                coro->jit_resume_proto = NULL;
                goto jit_resume_fallback;
            }

            // Channel recv resume: copy value from recv_slot (stack[0])
            // to jit_suspend_state.result where JIT continuation expects it.
            if (_jit_rs == XR_RESUME_CHANNEL) {
                XrValue rv = coro_ctx->stack[0];
                // Deep copy to receiver's heap if needed
                if (XR_IS_PTR(rv) && xr_value_needs_copy(rv)) {
                    rv = xr_deep_copy_to_coro(isolate, rv, coro);
                }
                coro->jit_suspend_state.result = rv.i;
                coro->jit_suspend_state.result_tag = rv.tag;
            }
            // AWAIT resume: copy task result to jit_suspend_state.result.
            // xr_task_wake_waiter only marks coro ready but does NOT write the
            // result into jit_suspend_state.  Without this, resume entry loads
            // stale data into the result register.
            if (_jit_rs != XR_RESUME_CHANNEL && _jit_rs != XR_RESUME_CHANNEL_CLOSED) {
                XrTask *await_task = atomic_load_explicit(&coro->await_task, memory_order_acquire);
                if (await_task) {
                    uint8_t tstate = atomic_load_explicit(&await_task->state, memory_order_acquire);
                    XrValue res = xr_null();
                    if (tstate == XR_TASK_COMPLETED) {
                        res = xr_deep_copy_to_coro(isolate, await_task->result, coro);
                    }
                    coro->jit_suspend_state.result = res.i;
                    coro->jit_suspend_state.result_tag = res.tag;
                    atomic_store_explicit(&coro->await_task, NULL, memory_order_relaxed);
                }
            }
            xr_coro_resume_store(coro, XR_RESUME_OK);

            XrValue jit_result;
            int _jrc_resume = xir_jit_resume(coro, &jit_result);
            if (_jrc_resume == XIR_JIT_OK) {
                // JIT resumed and completed successfully
                coro_ctx->stack[0] = jit_result;
                result = XR_VM_OK;
            } else if (_jrc_resume == XIR_JIT_SUSPEND) {
                // Nested suspend: blocked again at another channel/await.
                // Decision is on the stack — no racy read of jit_resume_entry.
                result = XR_VM_BLOCKED;
            } else {
                // Deopt or error: fall through to interpreter recovery
                goto jit_resume_fallback;
            }
            goto handle_result;
        }
jit_resume_fallback:
#endif

        // Continuation stealing resume: vm_ctx already set, just call run()
        if (xr_coro_resume_load(coro) == XR_RESUME_CONTINUATION) {
            xr_coro_resume_store(coro, 0);
            coro->vm_ctx.current_coro = coro;
            coro->vm_ctx.module_base_frame = 0;
            result = run(isolate, &coro->vm_ctx);
            goto handle_result;
        }

        // Check if this is a debug break resumption (no unroll needed)
        if (xr_coro_resume_load(coro) == XR_RESUME_DEBUG) {
            xr_coro_resume_store(coro, 0);
            coro->vm_ctx.current_coro = coro;
            coro->vm_ctx.module_base_frame = 0;
            result = run(isolate, &coro->vm_ctx);
            goto handle_result;
        }

        // ========== Use unroll mechanism to resume coroutine ==========
        int resume_status = xr_coro_resume_load(coro) ? xr_coro_resume_load(coro) : XR_RESUME_IO_READY;
        XrVMResult unroll_result = xr_coro_resume_with_unroll(isolate, coro, resume_status);

        XR_DBG_CORO("run_on_worker: coro id=%d, unroll result=%d, frame_count=%d",
                    coro->id, unroll_result, coro_ctx->frame_count);

        if (unroll_result == XR_VM_OK) {
            // Unroll complete, coroutine's independent vm_ctx is up to date
            coro_ctx->module_base_frame = 0;

            // Safety check: frame_count=0 means coroutine completed
            if (coro_ctx->frame_count == 0) {
                XR_DBG_CORO("run_on_worker: frame_count=0, coroutine completed");
                result = XR_VM_OK;
            } else {
                // Continuation return value handled at VM's startfunc label
                XR_DBG_CORO("run_on_worker: continue bytecode execution, frame_count=%d", coro_ctx->frame_count);
                result = run(isolate, &coro->vm_ctx);
            }
            XR_DBG_CORO("run_on_worker: bytecode execution complete, result=%d", result);
        } else if (unroll_result == XR_VM_BLOCKED) {
            // Blocked again
            result = XR_VM_BLOCKED;
        } else if (unroll_result == XR_VM_YIELD) {
            // Active yield
            result = XR_VM_YIELD;
        } else {
            // Error
            result = XR_VM_RUNTIME_ERROR;
        }
    }

handle_result:
    // Handle execution result
    if (result == XR_VM_SPAWN_CONT) {
        // Continuation stealing: parent saved state, child ready to run inline.
        // vm_ctx is single source of truth, no sync needed.
        ctx->current_coro = NULL;
        return result;
    } else if (result == XR_VM_DEBUG_BREAK) {
        // Debug breakpoint hit: vm_ctx is single source of truth
        xr_coro_flags_clear(coro, XR_CORO_FLG_RUNNING);
        xr_coro_flags_set(coro, XR_CORO_FLG_BLOCKED);
        // Don't clear current_coro - let caller handle
        ctx->current_coro = NULL;
        return result;
    } else if (result == XR_VM_BLOCKED || result == XR_VM_YIELD) {
        if (result == XR_VM_YIELD) {
            xr_coro_flags_clear(coro, XR_CORO_FLG_RUNNING);
            xr_coro_flags_set(coro, XR_CORO_FLG_READY);
        } else {
            // Channel blocks: BLOCKED already set inside xr_channel_recv/send
            // under lock.  Non-channel blocks (await,
            // timer): coro is still RUNNING.  CAS handles both correctly.
            uint8_t _exp = XR_CORO_STATE_RUNNING;
            if (atomic_compare_exchange_strong_explicit(
                    &coro->coro_state, &_exp, XR_CORO_STATE_BLOCKED,
                    memory_order_release, memory_order_relaxed)) {
                atomic_fetch_and_explicit(&coro->flags,
                    ~(uint32_t)XR_CORO_FLG_RUNNING, memory_order_relaxed);
                atomic_fetch_or_explicit(&coro->flags,
                    (uint32_t)XR_CORO_FLG_BLOCKED, memory_order_release);
            }
        }

        // Channel/timer blocked: BLOCKED flag set by channel func or CAS above.
        if (result == XR_VM_BLOCKED &&
            coro->ext && atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed)) {
            XR_DBG_CORO("coro id=%d timer blocked, waiting for Timer Wheel callback", coro->id);
        }

    } else if (result == XR_VM_OK) {
        // Coroutine completed — save result but do NOT set FLG_DONE here.
        // FLG_DONE is set in worker_handle_vm_result right before wake_waiter,
        // to prevent race where another thread sees DONE, reads result,
        // and recycles coroutine before wake_waiter runs.
        coro->result = coro_ctx->stack[0];

        XR_DBG_CORO("run_on_worker: coro id=%d completed, result tag=%u",
                    coro->id, coro_ctx->stack[0].tag);

        // NOTE: coro_gc release is deferred to xr_coro_release_resources
        // (called in await/await_all path after deep copy completes).
        // Early release here is UNSAFE because coro->result may reference
        // objects on the Immix heap (arrays, strings, maps, etc.).

    } else {
        // Error: capture actual exception message from vm_ctx
        coro->result = xr_null();
        xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
        XrValue exc = coro_ctx->current_exception;
        if (XR_IS_EXCEPTION(exc)) {
            XrException *e = XR_AS_EXCEPTION(exc);
            // If exception wraps a user-thrown string (throw "msg"),
            // use the original string as error message
            if (!XR_IS_NULL(e->userData) && XR_IS_STRING(e->userData)) {
                coro->error = e->userData;
            } else if (e->message) {
                coro->error = xr_string_value(e->message);
            } else {
                XrString *s = xr_string_intern(isolate, "unknown error", 13, 0);
                coro->error = xr_string_value(s);
            }
        } else {
            XrString *s = xr_string_intern(isolate, "coroutine error", 15, 0);
            coro->error = xr_string_value(s);
        }
        coro_ctx->current_exception = xr_null();
    }

    // vm_ctx is the single source of truth, no sync needed

    // Clear current coroutine
    ctx->current_coro = NULL;

    XR_DBG_CORO("run_on_worker: return result=%d, coro id=%d", result, coro->id);
    return result;
}

// ========== Coroutine Object Pool (ref Go gFree) ==========

// Batch size for stealing from / returning to global free list
#define XR_CORO_BATCH_SIZE 32

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

    // Local empty: batch steal from global free list
    if (worker && runtime->isolate && runtime->isolate->sys_heap) {
        XrCoroStructPool *pool = runtime->isolate->sys_heap->coro_pool;
        if (pool && pool->initialized) {
            pthread_mutex_lock(&pool->free_lock);
            int batch = 0;
            while (pool->free_list && batch < XR_CORO_BATCH_SIZE) {
                XrCoroutine *c = pool->free_list;
                pool->free_list = c->next;
                c->next = worker->p.local_free_list;
                worker->p.local_free_list = c;
                worker->p.local_free_count++;
                batch++;
            }
            pthread_mutex_unlock(&pool->free_lock);

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
            #define XR_ARENA_BATCH_SIZE 64
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
        // No worker context: return directly to global free list
        if (runtime->isolate && runtime->isolate->sys_heap) {
            XrCoroStructPool *pool = runtime->isolate->sys_heap->coro_pool;
            if (pool && pool->initialized) {
                pthread_mutex_lock(&pool->free_lock);
                coro->next = pool->free_list;
                pool->free_list = coro;
                pthread_mutex_unlock(&pool->free_lock);
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

    // Local full: batch return half to global free list, then put locally
    if (runtime->isolate && runtime->isolate->sys_heap) {
        XrCoroStructPool *pool = runtime->isolate->sys_heap->coro_pool;
        if (pool && pool->initialized) {
            int batch = worker->p.local_free_count / 2;
            pthread_mutex_lock(&pool->free_lock);
            for (int i = 0; i < batch; i++) {
                XrCoroutine *c = worker->p.local_free_list;
                if (!c) break;
                worker->p.local_free_list = c->next;
                worker->p.local_free_count--;
                c->next = pool->free_list;
                pool->free_list = c;
            }
            pthread_mutex_unlock(&pool->free_lock);
        }
    }

    // Now put current coro to local
    coro->next = worker->p.local_free_list;
    worker->p.local_free_list = coro;
    worker->p.local_free_count++;
}

// ========== Per-Worker Blocked Queue Operations (lock-free) ==========

// Hash function: Channel pointer -> bucket index
static inline int blocked_bucket_hash(void *channel) {
    uintptr_t h = (uintptr_t)channel;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    return (int)(h % XR_BLOCKED_BUCKET_SIZE);
}

// Per-Worker version: find or create blocked bucket for Channel (lock-free)
XrBlockedBucket *worker_blocked_bucket_find_or_create(XrWorker *worker, void *channel) {
    int idx = blocked_bucket_hash(channel);
    XrBlockedBucket *bucket = worker->p.blocked_buckets[idx];

    while (bucket) {
        if (bucket->channel == channel) return bucket;
        bucket = bucket->next;
    }

    bucket = (XrBlockedBucket *)xr_malloc(sizeof(XrBlockedBucket));
    if (!bucket) return NULL;

    memset(bucket, 0, sizeof(XrBlockedBucket));
    bucket->channel = channel;
    bucket->next = worker->p.blocked_buckets[idx];
    worker->p.blocked_buckets[idx] = bucket;

    return bucket;
}

// Per-Worker version: find blocked bucket for Channel (lock-free)
XrBlockedBucket *worker_blocked_bucket_find(XrWorker *worker, void *channel) {
    int idx = blocked_bucket_hash(channel);
    XrBlockedBucket *bucket = worker->p.blocked_buckets[idx];

    while (bucket) {
        if (bucket->channel == channel) return bucket;
        bucket = bucket->next;
    }
    return NULL;
}

// Per-Worker version: add coroutine to linear blocked queue (lock-free)
void worker_blocked_list_add(XrWorker *worker, XrCoroutine *coro) {
    if (!worker || !coro) return;

    coro->prev = worker->p.blocked_tail;
    coro->next = NULL;

    if (worker->p.blocked_tail) {
        worker->p.blocked_tail->next = coro;
    } else {
        worker->p.blocked_head = coro;
    }
    worker->p.blocked_tail = coro;
}

// Per-Worker version: remove coroutine from linear blocked queue (lock-free)
// Returns true if coro was actually in the list and removed
bool worker_blocked_list_remove(XrWorker *worker, XrCoroutine *coro) {
    if (!coro) return false;

    // Check if coro is actually in this worker's blocked list
    if (coro->prev == NULL && coro->next == NULL && worker->p.blocked_head != coro) {
        return false;  // Not in list
    }

    if (coro->prev) {
        coro->prev->next = coro->next;
    } else {
        worker->p.blocked_head = coro->next;
    }

    if (coro->next) {
        coro->next->prev = coro->prev;
    } else {
        worker->p.blocked_tail = coro->prev;
    }

    coro->prev = NULL;
    coro->next = NULL;
    return true;
}

// xr_worker_block - Add coroutine to current Worker's blocked queue (lock-free)
void xr_worker_block(XrWorker *worker, XrCoroutine *coro) {
    if (!worker || !coro) return;

    // Prevent duplicate add: if coroutine already in blocked queue, return
    if (coro->next != NULL || coro->prev != NULL || worker->p.blocked_head == coro) {
        return;
    }

    // Record Worker where coroutine is (for cross-Worker wake)
    atomic_store_explicit(&coro->affinity_p, worker->p.id, memory_order_relaxed);

    // Add to linear queue tail
    coro->prev = worker->p.blocked_tail;
    coro->next = NULL;

    if (worker->p.blocked_tail) {
        worker->p.blocked_tail->next = coro;
    } else {
        worker->p.blocked_head = coro;
    }
    worker->p.blocked_tail = coro;

    // If has Channel, add to hash table (use wait_link to avoid conflict with sched_link/MPSC)
    if (coro->wait_channel) {
        XrBlockedBucket *bucket = worker_blocked_bucket_find_or_create(worker, coro->wait_channel);
        if (bucket) {
            if (coro->wait_send) {
                coro->wait_link = NULL;
                if (bucket->send_tail) {
                    bucket->send_tail->wait_link = coro;
                } else {
                    bucket->send_head = coro;
                }
                bucket->send_tail = coro;
            } else {
                coro->wait_link = NULL;
                if (bucket->recv_tail) {
                    bucket->recv_tail->wait_link = coro;
                } else {
                    bucket->recv_head = coro;
                }
                bucket->recv_tail = coro;
            }
        }
    }

    worker->p.blocked_count++;
}

// xr_worker_unblock - Remove coroutine from Worker's blocked queue (lock-free)
void xr_worker_unblock(XrWorker *worker, XrCoroutine *coro) {
    if (!worker || !coro) return;

    if (worker_blocked_list_remove(worker, coro)) {
        worker->p.blocked_count--;
    }
}

// xr_worker_wake_one - Wake one coroutine waiting on specified Channel on current Worker (lock-free)
XrCoroutine *xr_worker_wake_one(XrWorker *worker, void *channel, bool wake_sender) {
    if (!worker || !channel) return NULL;

    XrBlockedBucket *bucket = worker_blocked_bucket_find(worker, channel);
    if (!bucket) return NULL;

    XrCoroutine *coro = NULL;
    if (wake_sender) {
        coro = bucket->send_head;
        if (coro) {
            bucket->send_head = coro->wait_link;
            if (!bucket->send_head) bucket->send_tail = NULL;
        }
    } else {
        coro = bucket->recv_head;
        if (coro) {
            bucket->recv_head = coro->wait_link;
            if (!bucket->recv_head) bucket->recv_tail = NULL;
        }
    }

    if (!coro) return NULL;

    // Remove from linear queue
    worker_blocked_list_remove(worker, coro);
    worker->p.blocked_count--;

    // Clear blocked info
    coro->wait_channel = NULL;
    coro->wait_link = NULL;

    // Cancel timer (sendTimeout/recvTimeout scenario)
    if (coro->ext && atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed)) {
        atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
    }

    // Critical: set resume status so instruction detects resume from Channel block
    xr_coro_resume_store(coro, XR_RESUME_CHANNEL);

    // Set ready state and add to this Worker's LIFO slot for locality
    xr_coro_flags_clear(coro, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_MASK);
    xr_coro_flags_set(coro, XR_CORO_FLG_READY);
    xr_worker_push_lifo(worker, coro);

    return coro;
}

// xr_worker_dequeue_blocked - Dequeue coroutine from blocked queue but don't enqueue to run queue
// For rendezvous value passing: caller needs to process value first then manually enqueue
XrCoroutine *xr_worker_dequeue_blocked(XrWorker *worker, void *channel, bool wake_sender) {
    if (!worker || !channel) return NULL;

    XrBlockedBucket *bucket = worker_blocked_bucket_find(worker, channel);
    if (!bucket) return NULL;

    XrCoroutine *coro = NULL;
    if (wake_sender) {
        coro = bucket->send_head;
        if (coro) {
            bucket->send_head = coro->wait_link;
            if (!bucket->send_head) bucket->send_tail = NULL;
        }
    } else {
        coro = bucket->recv_head;
        if (coro) {
            bucket->recv_head = coro->wait_link;
            if (!bucket->recv_head) bucket->recv_tail = NULL;
        }
    }

    if (!coro) return NULL;

    // Remove from linear queue
    worker_blocked_list_remove(worker, coro);
    worker->p.blocked_count--;

    // Clear blocked info, but don't enqueue (caller responsible)
    coro->wait_channel = NULL;
    coro->wait_link = NULL;
    xr_coro_flags_clear(coro, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_MASK);

    return coro;
}

// xr_worker_wake_all - Wake all coroutines waiting on specified Channel on current Worker (lock-free)
void xr_worker_wake_all(XrWorker *worker, void *channel) {
    if (!worker || !channel) return;

    XrBlockedBucket *bucket = worker_blocked_bucket_find(worker, channel);
    if (!bucket) return;

    // Wake all senders
    XrCoroutine *coro = bucket->send_head;
    while (coro) {
        XrCoroutine *next = coro->wait_link;
        worker_blocked_list_remove(worker, coro);
        worker->p.blocked_count--;

        coro->wait_channel = NULL;
        coro->wait_link = NULL;
        xr_coro_flags_clear(coro, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_MASK);
        xr_coro_flags_set(coro, XR_CORO_FLG_READY);
        xr_worker_push(worker, coro);

        coro = next;
    }
    bucket->send_head = bucket->send_tail = NULL;

    // Wake all receivers
    coro = bucket->recv_head;
    while (coro) {
        XrCoroutine *next = coro->wait_link;
        worker_blocked_list_remove(worker, coro);
        worker->p.blocked_count--;

        coro->wait_channel = NULL;
        coro->wait_link = NULL;
        xr_coro_flags_clear(coro, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_MASK);
        xr_coro_flags_set(coro, XR_CORO_FLG_READY);
        xr_worker_push(worker, coro);

        coro = next;
    }
    bucket->recv_head = bucket->recv_tail = NULL;
}

// Get current time (microseconds)
int64_t get_current_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// ========== Syscall Enter/Exit (P Handoff) ==========
//
// True P/M separation for blocking C functions:
//   entersyscall: detach M from P, hand off P to idle/new M
//   exitsyscall:  signal handoff M to release P, re-acquire P
//
// This preserves P's cache locality (run queue, timer wheel)
// while allowing the blocked M's OS thread to stay blocked.

void xr_worker_entersyscall(void) {
    XrWorker *worker = tls_current_worker;
    XrMachine *m = tls_current_machine;
    if (!worker || !m) return;

    XrRuntime *runtime = worker->p.runtime;
    if (!runtime) return;

    // Single worker: just mark P_SYSCALL, no handoff possible
    if (runtime->worker_count <= 1) {
        atomic_store(&worker->p.status, P_SYSCALL);
        return;
    }

    // Remember which Worker this M belongs to (for exitsyscall)
    m->blocked_worker = worker;

    // Reset handoff exit flag
    atomic_store(&worker->p.handoff_exit, false);

    // Detach M from P
    atomic_store(&m->current_p, NULL);
    atomic_store(&worker->p.current_m, NULL);
    worker->m = NULL;
    atomic_store(&m->state, M_BLOCKED);

    // Mark P as awaiting handoff (must be after detach)
    atomic_store(&worker->p.status, P_SYSCALL);

    // Hand off P to an idle or new M
    xr_handoffp(&worker->p);
}

void xr_worker_exitsyscall(void) {
    XrMachine *m = tls_current_machine;
    if (!m) return;

    XrWorker *worker = m->blocked_worker;
    if (!worker) {
        // Single-worker fallback path
        worker = tls_current_worker;
        if (worker) atomic_store(&worker->p.status, P_RUNNING);
        return;
    }
    m->blocked_worker = NULL;

    XrProc *p = &worker->p;

    // Fast path: try CAS P_SYSCALL → P_RUNNING (handoff M hasn't acquired yet)
    uint32_t expected = P_SYSCALL;
    if (atomic_compare_exchange_strong(&p->status, &expected, P_RUNNING)) {
        // Won the race: re-bind M directly
        atomic_store(&m->current_p, p);
        atomic_store(&p->current_m, m);
        worker->m = m;
        atomic_store(&m->state, M_RUNNING);
        return;
    }

    // Slow path: handoff M is running (P_HANDOFF) or already finished (P_IDLE)
    if (expected == P_HANDOFF) {
        // Signal handoff M to release P
        atomic_store(&p->handoff_exit, true);

        // Spin-wait for handoff M to fully release P
        int spin = 0;
        while (atomic_load(&p->current_m) != NULL) {
            if (++spin > 100) {
                struct timespec ts = {0, 100000};  // 100us
                nanosleep(&ts, NULL);
                spin = 0;
            }
            sched_yield();
        }
    }
    // else: P_IDLE (handoff M already finished and released)

    // Re-acquire P
    worker->m = m;
    xr_acquirep(m, p);
    atomic_store(&m->state, M_RUNNING);
}

// ========== Handoff Thread Entry ==========
//
// Runs P's scheduling loop on behalf of a blocked M.
// Exits when: handoff_exit is signaled, no work remains, or runtime stops.

void *xr_handoff_thread_entry(void *arg) {
    XrMachine *m = (XrMachine *)arg;
    XrRuntime *runtime = m->runtime;

handoff_restart:;
    XrProc *p = m->next_p;
    m->next_p = NULL;

    if (!p || !runtime) {
        goto handoff_park;
    }

    // Try CAS P_SYSCALL → P_HANDOFF (original M may have already returned)
    uint32_t expected = P_SYSCALL;
    if (!atomic_compare_exchange_strong(&p->status, &expected, P_HANDOFF)) {
        // Original M already reclaimed P — nothing to do
        goto handoff_park;
    }

    // Find Worker containing this P
    XrWorker *worker = NULL;
    for (int i = 0; i < runtime->worker_count; i++) {
        if (&runtime->workers[i].p == p) {
            worker = &runtime->workers[i];
            break;
        }
    }
    if (!worker) {
        atomic_store(&p->status, P_IDLE);
        goto handoff_park;
    }

    // Bind M to Worker and P
    worker->m = m;
    atomic_store(&m->current_p, p);
    atomic_store(&p->current_m, m);
    atomic_store(&m->state, M_RUNNING);
    tls_current_worker = worker;
    tls_current_machine = m;

    // ===== Handoff Scheduling Loop =====
    {
        int idle_iterations = 0;

        while (atomic_load(&runtime->running)) {
            if (atomic_load(&p->handoff_exit)) break;

            worker_poll_sources(worker);

            XrCoroutine *coro = xr_worker_pop(worker);

            if (!coro) {
                if (++idle_iterations > 100) break;
                struct timespec ts = {0, 100000};
                nanosleep(&ts, NULL);
                continue;
            }
            idle_iterations = 0;

            if (atomic_load(&p->handoff_exit)) {
                xr_worker_push(worker, coro);
                break;
            }

            if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) continue;

            worker_exec_with_cont_stealing(worker, coro);
        }
    }

    // Release P and unbind
    atomic_store(&m->current_p, NULL);
    atomic_store(&p->current_m, NULL);
    atomic_store(&p->status, P_IDLE);
    worker->m = NULL;
    m->current_coro = NULL;
    atomic_store(&m->state, M_PARKED);
    tls_current_worker = NULL;
    tls_current_machine = NULL;

handoff_park:
    // Park this M into idle pool, keeping thread alive for reuse
    atomic_store(&m->has_thread, true);
    xr_put_idle_m(runtime, m);

    // Wait for next_p to be set (thread reuse instead of exit+create)
    while (!m->next_p && atomic_load(&runtime->running)) {
        atomic_store_explicit(&m->park_state, XR_PARK_IDLE, memory_order_release);
        xr_park_futex_wait(&m->park_state, XR_PARK_IDLE, 0);
    }

    // Shutdown check
    if (!atomic_load(&runtime->running)) {
        atomic_store(&m->has_thread, false);
        return NULL;
    }

    // Reuse: jump back to process next_p
    goto handoff_restart;
}

// ========== Sysmon Heartbeat Monitoring ==========


