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
        XrScopeContext *scope = coro->current_scope;
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

static int worker_coro_priority(XrCoroutine *coro) {
    if (!coro)
        return CORO_PRIORITY_NORMAL;
    int priority = xr_coro_get_priority(xr_coro_flags_load(coro));
    if (priority < CORO_PRIORITY_LOW)
        return CORO_PRIORITY_LOW;
    if (priority > CORO_PRIORITY_HIGH)
        return CORO_PRIORITY_HIGH;
    return priority;
}

static bool worker_high_priority_work_visible(XrRuntime *runtime) {
    if (!runtime)
        return false;

    uint32_t high_inject_bit = (uint32_t) 1u << CORO_PRIORITY_HIGH;
    if ((atomic_load_explicit(&runtime->nonempty_inject_mask, memory_order_acquire) &
         high_inject_bit) != 0) {
        return true;
    }
    return atomic_load_explicit(&runtime->nonempty_p_mask[CORO_PRIORITY_HIGH],
                                memory_order_acquire) != 0;
}

static bool worker_should_inline_spawn_child(XrWorker *worker, XrCoroutine *parent,
                                             XrCoroutine *child) {
    int parent_priority = worker_coro_priority(parent);
    int child_priority = worker_coro_priority(child);
    if (child_priority < parent_priority)
        return false;

    if (child_priority >= CORO_PRIORITY_HIGH)
        return true;

    XrRuntime *runtime = worker ? worker->p.runtime : NULL;
    if (!runtime)
        return true;

    return !worker_high_priority_work_visible(runtime);
}

static bool worker_should_keep_parent_local(XrCoroutine *parent, XrCoroutine *child) {
    return worker_coro_priority(child) > worker_coro_priority(parent);
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

// Handle backend execution result for a coroutine. Returns true if runtime should stop.
static bool worker_handle_run_result(XrWorker *worker, XrCoroutine *coro, XrCoroRunResult result) {
    XrRuntime *runtime = worker->p.runtime;

    switch (result.kind) {
        case XR_CORO_RUN_DONE: {
            worker->p.yield_streak = 0;
            // Result already saved in the backend resume path (coro->result).
            // flags_set uses release ordering, ensuring coro->result is visible
            // to other threads before FLG_DONE is observed.
            xr_coro_flags_set(coro, XR_CORO_FLG_DONE);

            /* Task/Executor separation: cache result in Task before wake.
             * Await helpers copy the result to the awaiting coroutine's
             * heap before the executor detaches and becomes recyclable. */
            if (coro->task) {
                xr_task_complete(coro->task, coro->result);
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
            xr_coro_wake_waiter(runtime->isolate, coro);
            if (xr_coro_flags_has(coro, XR_CORO_FLG_MAIN)) {
                atomic_store(&runtime->running, false);
                return true;
            }
            break;
        }
        case XR_CORO_RUN_YIELD:
            xr_coro_resume_store(coro, XR_RESUME_OK);
            xr_coro_transition_to_ready(coro);
            xr_worker_push(worker, coro);
            worker->p.stats.yielded_count++;
            worker->p.yield_streak++;
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
                coro->task->coro = NULL;
                coro->task = NULL;
                coro->gc_flags |= XR_CORO_GC_RECYCLABLE;
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

static XrCoroutine *worker_pop_parent_continuation(XrWorker *worker) {
    XrCoroutine *parent = xr_steal_queue_pop(&worker->p.cont_deque);
    if (!parent)
        return NULL;

    xr_worker_refresh_runq_masks(worker);
    if (worker_coro_priority(parent) < CORO_PRIORITY_HIGH &&
        worker_high_priority_work_visible(worker->p.runtime)) {
        xr_worker_push(worker, parent);
        return NULL;
    }
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
    if (result.kind == XR_CORO_RUN_SPAWN_CHILD) {
        XrCoroutine *pending = xr_coro_take_pending_spawn(coro);
        XrCoroutine *child = result.child ? result.child : pending;
        XR_DCHECK(!result.child || !pending || result.child == pending,
                  "spawn child result does not match pending child");
        if (!child) {
            result = xr_coro_run_error(XR_NULL_VAL, false);
            goto normal_result_path;
        }
        if (!worker_should_inline_spawn_child(worker, coro, child)) {
            xr_coro_resume_store(coro, XR_RESUME_CONTINUATION);
            xr_worker_push(worker, child);
            p->yield_streak = 0;
            goto exec_fast;
        }
        xr_coro_resume_store(coro, XR_RESUME_CONTINUATION);
        // Ensure worker threads are started (lazy init).
        // Without this, JIT-compiled children that never yield
        // won't trigger the yield_streak threshold that normally
        // starts workers, leaving only Worker 0 active.
        xr_runtime_ensure_workers(p->runtime);
        // Reset yield_streak: yields during spawn loop don't count
        // toward compute-bound pressure detection.
        p->yield_streak = 0;
        // Detach worker-local backend state before running the child.
        // Re-bound by xr_coro_run_on_worker when the parent resumes.
        xr_coro_detach_worker_state(coro);
        if (worker_should_keep_parent_local(coro, child)) {
            xr_worker_push_lifo(worker, coro);
        } else if (!xr_steal_queue_push(&p->cont_deque, coro)) {
            xr_worker_push(worker, coro);
        } else {
            xr_worker_refresh_runq_masks(worker);
            // Wake an idle worker so it can steal the continuation.
            // Without this, JIT-compiled children that never yield
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
        worker_process_blocked(worker, coro);

        // Periodic lightweight housekeeping during fast dispatch
        if ((fast_dispatch_budget & 7) == 0) {
            worker_drain_inbox(worker);  // O(1) if empty
            worker_pull_inject(worker, XR_FAST_DISPATCH_INJECT_BATCH);
        }
        if ((fast_dispatch_budget & 15) == 0) {
            int64_t _now = xr_monotonic_ticks();
            if (p->timer_wheel &&
                (xr_timer_cancel_pending(p->timer_wheel) || _now > p->last_timer_tick)) {
                xr_bump_timers(p->timer_wheel, _now);
                p->stats.timer_bump_count++;
                if (_now > p->last_timer_tick) {
                    p->last_timer_tick = _now;
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

    // Reductions tracking (safe: coro is still owned by this worker)
    int reds_used = XR_CORO_REDUCTIONS - coro->reductions;
    if (reds_used < 0)
        reds_used = XR_CORO_REDUCTIONS;
    int prio = xr_coro_get_priority(xr_coro_flags_load(coro));
    xr_worker_reductions_executed(worker, prio, reds_used);

    // Handle backend result
    worker_handle_run_result(worker, coro, result);

    // Deferred recycle: fire-and-forget coro completed, defer to next pool_get.
    // gc_flags bit 2 = recyclable for fire-and-forget go.
    // Push to pending linked list (via coro->next) — flushed in pool_get.
    if (result.kind == XR_CORO_RUN_DONE && (coro->gc_flags & XR_CORO_GC_RECYCLABLE) &&
        !xr_coro_flags_has(coro, XR_CORO_FLG_MAIN)) {
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
