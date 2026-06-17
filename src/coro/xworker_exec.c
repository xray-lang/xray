/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xworker_exec.c - Coroutine execution core: dispatch, run result, cont-stealing
 *
 * KEY CONCEPT:
 *   This file hosts the hot path where a coroutine is actually executed
 *   on a worker. It covers:
 *     - xr_coro_run_on_worker: backend-neutral dispatch via XrCoroBackendVTable.
 *     - worker_exec_with_cont_stealing: push-parent / exec-child loop for
 *       continuation stealing, plus BLOCKED fast re-dispatch.
 *     - worker_handle_run_result: dispatch of backend outcomes (done, yield,
 *       blocked, cancelled, error) including Task state + monitor hooks.
 */
#include "xworker_internal.h"
#include "../base/xchecks.h"
#include "xblock.h"
#include "xdeep_copy.h"
#include "xsched_trace.h"
#include "xtask.h"

static inline bool worker_blocked_post_check(XrRuntime *runtime, XrCoroutine *coro) {
    int wr = xr_coro_get_wait_reason(xr_coro_flags_load(coro));
    if (wr == (XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT)) {
        XrCoroWaitState *wait = xr_coro_wait_state(coro);
        XrTask *task = wait ? atomic_load_explicit(&wait->await_task, memory_order_acquire) : NULL;
        if (task) {
            int astate = atomic_load_explicit(&task->await_state, memory_order_acquire);
            if (astate == XR_AWAIT_RESOLVED) {
                xr_await_wait_token_resolve(&wait->await_token);
                xr_coro_ready(runtime->isolate, coro, true);
                return true;
            }
        }
    } else if (wr == (XR_CORO_WAIT_AWAIT_ALL >> XR_CORO_WAIT_SHIFT)) {
        XrCoroWaitState *wait = xr_coro_wait_state(coro);
        if (!wait || atomic_load(&wait->wait_count) == 0) {
            if (wait)
                xr_multi_await_wait_token_resolve(&wait->multi_await_token);
            xr_coro_ready(runtime->isolate, coro, true);
            return true;
        }
    } else if (wr == (XR_CORO_WAIT_SCOPE >> XR_CORO_WAIT_SHIFT)) {
        XrScopeContext *scope = atomic_load_explicit(&coro->current_scope, memory_order_relaxed);
        if (!scope || atomic_load(&scope->count) == 0) {
            XrCoroWaitState *wait = xr_coro_wait_state(coro);
            if (wait)
                xr_scope_wait_token_resolve(&wait->scope_token);
            xr_coro_ready(runtime->isolate, coro, true);
            return true;
        }
    } else if (wr == (XR_CORO_WAIT_AWAIT_ANY >> XR_CORO_WAIT_SHIFT)) {
        XrCoroWaitState *wait = xr_coro_wait_state(coro);
        if (!wait || atomic_load(&wait->any_done) || atomic_load(&wait->wait_count) == 0) {
            if (wait)
                xr_multi_await_wait_token_resolve(&wait->multi_await_token);
            xr_coro_ready(runtime->isolate, coro, true);
            return true;
        }
    }
    return false;
}

static inline bool worker_can_recycle_completed_coro(XrCoroutine *coro) {
    if (!coro || !(coro->gc_flags & XR_CORO_GC_RECYCLABLE) ||
        xr_coro_flags_has(coro, XR_CORO_FLG_MAIN)) {
        return false;
    }

    XrTask *task = coro->task;
    if (!task)
        return true;
    if (task->parent || task->first_child || task->links)
        return false;
    if (atomic_load_explicit(&task->on_completion, memory_order_acquire))
        return false;
    return atomic_load_explicit(&task->await_state, memory_order_acquire) != XR_AWAIT_WAITING;
}

static XrCoroEvent worker_event_from_coro(XrCoroutine *coro) {
    XrCoroEvent event;
    event.kind = XR_CORO_EVENT_START;
    event.value = XR_NULL_VAL;
    event.flags = 0;

    if (!coro)
        return event;

    uint32_t flags = xr_coro_flags_load(coro);
    event.flags = flags;
    if (flags & XR_CORO_FLG_CANCEL_REQUESTED) {
        event.kind = XR_CORO_EVENT_CANCEL;
        return event;
    }

    int resume = xr_coro_resume_load(coro);
    if (resume == XR_RESUME_DEBUG) {
        event.kind = XR_CORO_EVENT_DEBUG;
    } else if (resume == XR_RESUME_CHANNEL) {
        event.kind = XR_CORO_EVENT_CHANNEL;
    } else if (resume == XR_RESUME_CHANNEL_CLOSED) {
        event.kind = XR_CORO_EVENT_CHANNEL_CLOSED;
    } else if (resume == XR_RESUME_TIMEOUT) {
        event.kind = XR_CORO_EVENT_TIMEOUT;
    } else if (resume == XR_RESUME_IO_READY) {
        event.kind = XR_CORO_EVENT_IO_READY;
    } else if (resume == XR_RESUME_CANCELLED) {
        event.kind = XR_CORO_EVENT_CANCEL;
    } else if (flags & XR_CORO_FLG_STARTED) {
        event.kind = XR_CORO_EVENT_RESUME;
    }
    return event;
}

static bool worker_global_spawn_backlog_visible(XrRuntime *runtime) {
    if (!runtime)
        return false;
    if (xr_runtime_any_inbox_nonempty(runtime))
        return true;

    if (!atomic_load_explicit(&runtime->injectq_nonempty, memory_order_acquire))
        return false;

    int pending = atomic_load_explicit(&runtime->injectq.len, memory_order_relaxed);
    return pending >= XR_SPAWN_INLINE_GLOBAL_BACKLOG;
}

static bool worker_spawn_backlog_visible(XrWorker *worker, XrRuntime *runtime) {
    if (!worker || !runtime || runtime->worker_count <= 1)
        return false;
    if (xr_proc_local_runq_len(&worker->p) >= XR_SPAWN_INLINE_LOCAL_BACKLOG)
        return true;
    return worker_global_spawn_backlog_visible(runtime);
}

static int worker_spawn_share_backlog_limit(XrRuntime *runtime) {
    if (!runtime)
        return XR_SPAWN_INLINE_LOCAL_BACKLOG;
    int limit = runtime->worker_count * XR_SPAWN_SHARE_BACKLOG_PER_WORKER;
    int min_limit = XR_SPAWN_INLINE_LOCAL_BACKLOG * 2;
    if (limit < min_limit)
        limit = min_limit;
    int max_local = XR_LOCAL_QUEUE_SIZE / 2;
    if (limit > max_local)
        limit = max_local;
    return limit;
}

static bool worker_spawn_share_backlog_full(XrWorker *worker, XrRuntime *runtime) {
    if (!worker || !runtime)
        return false;
    return xr_proc_local_runq_len(&worker->p) >= worker_spawn_share_backlog_limit(runtime);
}

static uint16_t worker_record_spawn_burst(XrCoroutine *parent) {
    if (!parent)
        return 0;
    if (parent->spawn_burst_count < UINT16_MAX)
        parent->spawn_burst_count++;
    return parent->spawn_burst_count;
}

static void worker_reset_spawn_burst(XrCoroutine *coro) {
    if (coro)
        coro->spawn_burst_count = 0;
}

static bool worker_should_share_spawn_child(XrWorker *worker, XrRuntime *runtime,
                                            uint16_t burst_count) {
    if (!worker || !runtime || runtime->worker_count <= 1)
        return false;
    if (worker_global_spawn_backlog_visible(runtime))
        return false;
    if (worker_spawn_share_backlog_full(worker, runtime))
        return false;
    if (burst_count < XR_SPAWN_SHARE_BURST_THRESHOLD)
        return false;
    if (XR_SPAWN_SHARE_BURST_INTERVAL <= 1)
        return true;
    return ((burst_count - XR_SPAWN_SHARE_BURST_THRESHOLD) % XR_SPAWN_SHARE_BURST_INTERVAL) == 0;
}

static bool worker_should_inline_spawn_child(XrWorker *worker, XrCoroutine *parent,
                                             XrCoroutine *child, bool *out_burst_share,
                                             bool *out_backlog_share) {
    if (out_burst_share)
        *out_burst_share = false;
    if (out_backlog_share)
        *out_backlog_share = false;

    XrRuntime *runtime = worker ? worker->p.runtime : NULL;
    if (!runtime)
        return true;

    uint16_t burst_count = worker_record_spawn_burst(parent);
    if (worker_spawn_backlog_visible(worker, runtime)) {
        if (out_backlog_share)
            *out_backlog_share = true;
        return false;
    }
    if (child && worker_should_share_spawn_child(worker, runtime, burst_count)) {
        if (out_burst_share)
            *out_burst_share = true;
        return false;
    }

    return true;
}

/*
 * Unified BLOCKED post-processing.
 *
 * Called after a backend returns XR_CORO_RUN_BLOCKED.
 * BLOCKED flag is already set by the active backend.
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
bool worker_process_blocked(XrWorker *worker, XrCoroutine *coro) {
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
    if (xr_coro_select_wait(coro))
        return false;

    // Timer active: add to blocked queue for tracking
    if (coro->ext && atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed))
        xr_worker_block(worker, coro);

    return false;
}

// Handle backend execution result for a coroutine. Returns true if runtime
// should stop. *out_executor_reclaimed reports whether this worker kept
// ownership of the executor shell (eager reclaim) — only then may the caller
// touch the coroutine afterwards (deferred recycle); otherwise a concurrently
// woken awaiter may already have claimed and recycled it.
static bool worker_handle_run_result(XrWorker *worker, XrCoroutine *coro, XrCoroRunResult result,
                                     bool *out_executor_reclaimed) {
    XrRuntime *runtime = worker->p.runtime;
    *out_executor_reclaimed = false;

    switch (result.kind) {
        case XR_CORO_RUN_DONE: {
            worker->p.yield_streak = 0;
            XrTask *done_task = coro->task;
            // Result already saved in the backend resume path (coro->result).
            // flags_set uses release ordering, ensuring coro->result is visible
            // to other threads before FLG_DONE is observed.
            xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
            bool was_main = xr_coro_flags_has(coro, XR_CORO_FLG_MAIN);

            /* Eager executor reclaim is decided BEFORE the task state is
             * published: immediate results need no copy out of the executor
             * heap, so the shell can detach now and this worker keeps sole
             * ownership of it. Coro-side writes must also happen before
             * publication — afterwards a woken awaiter may already be
             * consuming the task on another worker. */
            bool reclaim_executor = false;
            if (done_task && !was_main && (done_task->flags & XR_TASK_FLG_RUNTIME_OWNED) &&
                !xr_value_needs_copy(coro->result)) {
                reclaim_executor = true;
                coro->task = NULL;
                coro->gc_flags |= XR_CORO_GC_RECYCLABLE;
                if (done_task->flags & XR_TASK_FLG_ONE_SHOT_AWAIT)
                    coro->gc_flags |= XR_CORO_GC_TRIM_BACKEND_STORAGE;
                /* Detach before publication so no awaiter can ever claim the
                 * shell this worker is about to recycle. Relaxed is enough:
                 * the publication CAS below carries the release. */
                atomic_store_explicit(&done_task->coro, NULL, memory_order_relaxed);
            } else if (!done_task && !was_main) {
                /* Task-less fire-and-forget go: no handle exists, so no
                 * awaiter can ever claim the shell — this worker keeps it.
                 * The deferred-recycle push is still gated on RECYCLABLE. */
                reclaim_executor = true;
            }

            // Inline fast path: skip extern calls for anonymous coros without monitors
            if (__builtin_expect(xr_coro_name(coro) != NULL || (coro->ext && coro->ext->watched_by),
                                 0)) {
                XrCoroState *_s = (XrCoroState *) runtime->isolate->vm.coro_state;
                xr_coro_notify_monitors(runtime->isolate, _s ? _s->coro_registry : NULL, coro,
                                        "normal");
                xr_coro_on_exit(runtime->isolate, coro);
            }
            worker->p.stats.completed_count++;

            /* Scope bookkeeping dereferences the coroutine, so it must run
             * before publication (after COMPLETED is visible an awaiter may
             * claim and recycle the shell). It does not depend on task
             * state, so the early wake is benign. */
            xr_coro_wake_scope_waiter(runtime->isolate, coro);

            if (done_task) {
                /* Publication point: cache result in Task, CAS the state to
                 * COMPLETED, fire completion listeners. */
                xr_task_complete(done_task, coro->result);
                /* Task-side await wake. Reads task fields — safe while
                 * completer_done is still 0, one-shot destroy waits for it.
                 * Uses done_task directly: coro->task is already detached
                 * in the reclaim case, and the shell itself must not be
                 * dereferenced post-publication in the non-reclaim case. */
                xr_task_wake_waiter(runtime->isolate, done_task);
                atomic_store_explicit(&done_task->completer_done, 1, memory_order_release);
                /* No access to done_task (or, unless reclaimed, coro)
                 * beyond this point. */
            }

            *out_executor_reclaimed = reclaim_executor;
            if (was_main) {
                atomic_store(&runtime->running, false);
                return true;
            }
            break;
        }
        case XR_CORO_RUN_YIELD:
            xr_coro_resume_store(coro, XR_RESUME_OK);
            xr_coro_transition_to_ready(coro);
            worker->p.stats.yielded_count++;
            worker->p.yield_streak++;
            if (runtime && runtime->worker_count > 1 &&
                worker->p.yield_streak >= runtime->worker_count / 2) {
                xr_injectq_push(runtime, coro);
                worker->p.yield_streak = 0;
                break;
            }
            xr_worker_push(worker, coro);
            break;

        case XR_CORO_RUN_BLOCKED:
            worker->p.yield_streak = 0;
            // BLOCKED flag already set by the backend resume path.
            // Unified post-processing: race check, fence, post_check, timer.
            worker_process_blocked(worker, coro);
            break;

        case XR_CORO_RUN_DEBUG_BREAK:
            (void) xr_coro_try_transition_to_blocked(coro);
            if (xr_coro_flags_has(coro, XR_CORO_FLG_MAIN)) {
                atomic_store(&runtime->running, false);
                return true;
            }
            break;

        case XR_CORO_RUN_CANCELLED:
            worker->p.yield_streak = 0;
            /* A coroutine cancelled while parked in select (woken by the
             * cooperative-cancel xr_coro_ready, not by a ready case) returns
             * CANCELLED straight from the resume entry without passing through
             * the select recheck that normally tears down its wait state. Its
             * select bucket links and timer channel are still live, so tear them
             * down here on the owner worker before the shell can be recycled —
             * shared with the DONE drain path via xr_worker_teardown_select_wait
             * (includes the `after` timer channel dispose, design/885). No-op for
             * the common case of a coroutine cancelled while running. */
            xr_worker_teardown_select_wait(worker, coro);
            xr_coro_flags_set(coro, XR_CORO_FLG_CANCELLED | XR_CORO_FLG_DONE);
            xr_coro_flags_clear(coro, XR_CORO_FLG_CANCEL_REQUESTED | XR_CORO_FLG_READY |
                                          XR_CORO_FLG_BLOCKED | XR_CORO_FLG_RUNNING);
            /* Task/Executor separation: mark task cancelled.
             * Detach after wake dispatch so task completion waiters can be read. */
            if (coro->task) {
                xr_task_cancel(coro->task);
            }
            {
                XrCoroState *_s = (XrCoroState *) runtime->isolate->vm.coro_state;
                xr_coro_notify_monitors(runtime->isolate, _s ? _s->coro_registry : NULL, coro,
                                        "cancelled");
            }
            xr_coro_on_exit(runtime->isolate, coro);
            worker->p.stats.completed_count++;
            xr_coro_wake_waiter(runtime->isolate, coro);
            if (coro->task) {
                XrTask *t = coro->task;
                /* Exchange-claim: the task.cancel() API path may detach the
                 * executor concurrently; only the claim winner recycles. */
                if (xr_task_claim_executor(t) == coro) {
                    coro->task = NULL;
                    coro->gc_flags |= XR_CORO_GC_RECYCLABLE;
                }
                atomic_store_explicit(&t->completer_done, 1, memory_order_release);
            }
            if (xr_coro_flags_has(coro, XR_CORO_FLG_MAIN)) {
                atomic_store(&runtime->running, false);
                return true;
            }
            break;

        case XR_CORO_RUN_ERROR:
        default:
            if (!XR_IS_NULL(result.error)) {
                coro->error = result.error;
                coro->error_is_value = result.error_is_value;
            }
            xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
            /* Task/Executor separation: mark task failed.
             * Detach after wake dispatch so task completion waiters can be read. */
            if (coro->task) {
                if (coro->task->link_mode == XR_LINK_LINKED && coro->task->parent) {
                    // linked go: propagate error to parent task
                    xr_task_fail_with_propagation(coro->task, coro->error);
                } else {
                    xr_task_fail(coro->task, coro->error);
                }
            }
            {
                XrCoroState *_s = (XrCoroState *) runtime->isolate->vm.coro_state;
                xr_coro_notify_monitors(runtime->isolate, _s ? _s->coro_registry : NULL, coro,
                                        "error");
            }
            xr_coro_on_exit(runtime->isolate, coro);
            worker->p.stats.completed_count++;
            xr_coro_wake_waiter(runtime->isolate, coro);
            if (coro->task) {
                XrTask *t = coro->task;
                if (xr_task_claim_executor(t) == coro) {
                    coro->task = NULL;
                    coro->gc_flags |= XR_CORO_GC_RECYCLABLE;
                }
                atomic_store_explicit(&t->completer_done, 1, memory_order_release);
            }
            if (xr_coro_flags_has(coro, XR_CORO_FLG_MAIN)) {
                atomic_store(&runtime->running, false);
                return true;
            }
            break;
    }
    return false;
}

static XrCoroutine *worker_pop_parent_continuation(XrWorker *worker) {
    XrCoroutine *parent = xr_steal_queue_pop(&worker->p.cont_deque);
    if (!parent)
        return NULL;

    return parent;
}

// Execute a coroutine with continuation stealing support.
// Handles the push-parent/exec-child/pop-parent loop.
// Also implements BLOCKED fast re-dispatch: when a coro blocks on channel
// and the LIFO slot has a just-woken coro, execute it inline without
// returning to worker_loop (avoids scheduling overhead for ping-pong patterns).
void worker_exec_with_cont_stealing(XrWorker *worker, XrCoroutine *coro) {
    XrMachine *m = worker->m;
    XrProc *p = &worker->p;
    XrCoroRunResult result;
    int fast_dispatch_budget = XR_FAST_DISPATCH_BUDGET;

cont_exec:
    // Invariant: coro must not be NULL or DONE when entering execution
    XR_DCHECK(coro != NULL, "cont_exec: NULL coroutine");
    XR_DCHECK(!xr_coro_flags_has(coro, XR_CORO_FLG_DONE), "cont_exec: executing DONE coroutine");
    SCHED_TRACE_CORO(worker, coro, "exec");
    atomic_store_explicit(&m->current_coro, coro, memory_order_relaxed);
    p->local_active_coros++;
    // In-dispatch direct switch chain budget: refilled per scheduler visit.
    // Complements the fast re-dispatch budget below; the LIFO backlog gate
    // bounds starvation on every pop in both forms.
    p->direct_switch_budget = XR_FAST_DISPATCH_BUDGET;
    // Update affinity so IO wakeups return to this worker
    atomic_store_explicit(&coro->affinity_p, p->id, memory_order_relaxed);

exec_fast:  // Fast re-dispatch entry: local_active_coros already correct
    result = xr_coro_run_on_worker(worker, coro);
    // In-dispatch direct switches settle execution on whichever coroutine ran
    // last inside run(); rebind before any result handling below. The
    // switched-away coroutines were fully suspended by the switch helper and
    // must not be touched here (they may already run on another worker).
    {
        XrCoroutine *settled = p->vm_settled_coro;
        p->vm_settled_coro = NULL;
        if (settled && settled != coro)
            coro = settled;
    }
    // Single-writer store: only owner thread writes, sysmon reads via relaxed load
    atomic_store_explicit(&m->heartbeat,
                          atomic_load_explicit(&m->heartbeat, memory_order_relaxed) + 1,
                          memory_order_relaxed);

    // Continuation stealing: child-first dispatch.
    // Push parent to cont_deque, switch to child for immediate execution.
    // Children that yield (compute-heavy) go back to run queue where workers
    // can steal them. Children that block (channel I/O) go to blocked queue
    // and are not stealable — preserving cache locality for channel patterns.
    if (result.kind == XR_CORO_RUN_SPAWN_CHILD) {
        XrCoroutine *pending = xr_coro_take_pending_spawn(coro);
        XrCoroutine *child = result.child ? result.child : pending;
        XR_DCHECK(!result.child || !pending || result.child == pending,
                  "spawn child result does not match pending child");
        if (!child) {
            result = xr_coro_run_error(XR_NULL_VAL, false);
            goto normal_result_path;
        }
        bool burst_share = false;
        bool backlog_share = false;
        bool inline_child =
            worker_should_inline_spawn_child(worker, coro, child, &burst_share, &backlog_share);
        // Queued and inline children both need worker threads visible before
        // the current worker continues the parent.
        xr_runtime_ensure_workers(p->runtime);
        p->stats.spawned_count++;
        if (!inline_child && worker_spawn_share_backlog_full(worker, p->runtime)) {
            inline_child = true;
            p->stats.spawn_backlog_full_inline_count++;
        }
        if (inline_child) {
            p->stats.spawn_inline_child_count++;
        } else {
            p->stats.spawn_shared_child_count++;
            if (burst_share)
                p->stats.spawn_burst_shared_count++;
            if (backlog_share)
                p->stats.spawn_backlog_shared_count++;
        }
        if (!inline_child) {
            xr_coro_resume_store(coro, XR_RESUME_CONTINUATION);
            xr_worker_push(worker, child);
            p->yield_streak = 0;
            goto exec_fast;
        }
        xr_coro_resume_store(coro, XR_RESUME_CONTINUATION);
        // Reset yield_streak: yields during spawn loop don't count
        // toward compute-bound pressure detection.
        p->yield_streak = 0;
        if (!xr_steal_queue_push(&p->cont_deque, coro)) {
            xr_worker_push(worker, coro);
        } else {
            // Wake an idle worker so it can steal the continuation.
            // Without this, children that never yield
            // monopolize the current worker, and parked workers
            // never discover the parent continuation in cont_deque.
            XrRuntime *_rt = p->runtime;
            if (_rt && atomic_load_explicit(&_rt->spinning_count, memory_order_relaxed) == 0) {
                wake_idle_worker(_rt);
            }
        }
        coro = child;
        atomic_store_explicit(&m->current_coro, coro, memory_order_relaxed);
        goto exec_fast;
    }

    // BLOCKED fast re-dispatch: skip full result handling/reductions tracking
    // for maximum throughput. Optimal for serial message chains (pingpong, ring).
    // BLOCKED flag already set by the active backend.
    if (result.kind == XR_CORO_RUN_BLOCKED && fast_dispatch_budget <= 1) {
        p->stats.fast_dispatch_budget_stop_count++;
    }
    if (result.kind == XR_CORO_RUN_BLOCKED && fast_dispatch_budget > 1) {
        XrCoroutine *next = xr_worker_try_pop_lifo(worker, false);
        if (!next) {
            p->stats.fast_dispatch_empty_count++;
            goto normal_result_path;
        }
        p->stats.fast_dispatch_count++;
        fast_dispatch_budget--;
        SCHED_TRACE_CORO(worker, coro, "fast_dispatch_blocked");
        p->yield_streak = 0;
        if (!worker_process_blocked(worker, coro)) {
            worker_reset_spawn_burst(coro);
        }

        // Periodic lightweight housekeeping during fast dispatch
        if ((fast_dispatch_budget & 7) == 0) {
            worker_drain_inbox(worker);  // O(1) if empty
            worker_pull_inject(worker, XR_FAST_DISPATCH_INJECT_BATCH);
        }
        if ((fast_dispatch_budget & 15) == 0) {
            int64_t _now = xr_monotonic_ticks();
            if (p->timer_wheel &&
                (xr_timer_cancel_pending(p->timer_wheel) || _now > xr_proc_last_timer_tick(p))) {
                xr_bump_timers(p->timer_wheel, _now);
                p->stats.timer_bump_count++;
                if (_now > xr_proc_last_timer_tick(p)) {
                    xr_proc_set_last_timer_tick(p, _now);
                }
            }
        }

        p->stats.executed_count++;
        XR_DCHECK(!xr_coro_flags_has(next, XR_CORO_FLG_DONE),
                  "fast_dispatch: LIFO slot contains DONE coroutine");
        atomic_store_explicit(&m->current_coro, next, memory_order_relaxed);
        coro = next;
        goto exec_fast;  // Skip active_coros, reductions, full result handling
    }

normal_result_path:
    // Check cancel flag (sysmon may have marked it)
    if (xr_coro_flags_has(coro, XR_CORO_FLG_CANCEL_REQUESTED)) {
        result = xr_coro_run_result(XR_CORO_RUN_CANCELLED);
    }

    atomic_store_explicit(&m->current_coro, NULL, memory_order_relaxed);
    p->stats.executed_count++;

    // Race guard: when a backend returns BLOCKED after channel_recv/send, the coro
    // has already been added to channel waitq (spinlock released). Another
    // thread may have woken it and started executing it on a different worker.
    // In that case we must NOT touch any coro fields — the coro is "gone".
    if (result.kind == XR_CORO_RUN_BLOCKED &&
        (xr_coro_resume_load(coro) == XR_RESUME_CHANNEL ||
         xr_coro_resume_load(coro) == XR_RESUME_CHANNEL_CLOSED ||
         (xr_coro_flags_load(coro) & XR_CORO_FLG_READY))) {
        // Coro already woken by another thread — skip all coro field access
        p->local_active_coros--;
        goto pop_continuation;
    }

    /* Spawn-burst and reductions are read from the coroutine, so they are
     * only safe while this worker still owns it. A BLOCKED result means the
     * coro was already published to a wait queue inside the backend — the
     * race guard above catches the detected subset, but a wake can land
     * between that check and here, so blocked coros must not be touched at
     * all. Both values are scheduling heuristics; skipping them for blocked
     * coros (which blocked early in their quantum) is acceptable. */
    if (result.kind != XR_CORO_RUN_BLOCKED) {
        worker_reset_spawn_burst(coro);
        int reds_used = XR_CORO_REDUCTIONS - xr_coro_reds(coro);
        if (reds_used < 0)
            reds_used = XR_CORO_REDUCTIONS;
        xr_worker_reductions_executed(worker, reds_used);
    }

    // Handle backend result
    bool executor_reclaimed = false;
    worker_handle_run_result(worker, coro, result, &executor_reclaimed);

    // Deferred recycle: completed coro this worker still owns, defer to next
    // pool_get. Push to pending linked list (via coro->next).
    // executor_reclaimed gating is mandatory: without it the shell may have
    // been claimed and recycled by a woken awaiter on another worker, and
    // even reading coro fields here would race its next lifetime.
    if (result.kind == XR_CORO_RUN_DONE && executor_reclaimed &&
        worker_can_recycle_completed_coro(coro)) {
        coro->next = p->pending_recycle_coro;
        p->pending_recycle_coro = coro;
    }

    p->local_active_coros--;

pop_continuation:
    // Pop parent continuation (LIFO)
    {
        XrCoroutine *parent = worker_pop_parent_continuation(worker);
        if (parent) {
            coro = parent;
            goto cont_exec;
        }
    }
}

XrCoroRunResult xr_coro_run_on_worker(XrWorker *worker, XrCoroutine *coro) {
    XrCoroRunContext run_ctx;
    run_ctx.worker = worker;
    run_ctx.isolate = (worker && worker->p.runtime) ? worker->p.runtime->isolate : NULL;

    XrCoroEvent event = worker_event_from_coro(coro);
    if (!coro || !coro->backend || !coro->backend->resume)
        return xr_coro_run_error(XR_NULL_VAL, false);
    xr_coro_finish_backend_resume_tokens(coro, xr_coro_resume_load(coro));
    return coro->backend->resume(coro, &event, &run_ctx);
}
