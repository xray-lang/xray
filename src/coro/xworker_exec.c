/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xworker_exec.c - Coroutine execution core: dispatch, VM result, cont-stealing
 *
 * KEY CONCEPT:
 *   This file hosts the hot path where a coroutine is actually executed
 *   on a worker. It covers:
 *     - xr_coro_run_on_worker: single dispatch entry for closure / native /
 *       cfunc / JIT-resume / unroll-resume.
 *     - worker_exec_with_cont_stealing: push-parent / exec-child loop for
 *       continuation stealing, plus BLOCKED fast re-dispatch.
 *     - worker_handle_vm_result: dispatch of XR_VM_* outcomes (done, yield,
 *       blocked, cancelled, error) including Task state + monitor hooks.
 *     - run_cfunc_coro: Yieldable C-function coroutine execution with
 *       inline fast path for single-frame continuations.
 *
 * KNOWN OVERSIZE:
 *   xr_coro_run_on_worker (~440 lines) currently exceeds the 150-line
 *   function limit. It is the dispatch hub for closure-first / resume
 *   / cfunc / jit paths and is intentionally kept inline for branch
 *   predictability; future split into run_closure_first / _resume /
 *   _jit helpers would not change semantics.
 */
#include "xworker_internal.h"
#include "xtask.h"
#include "xdeep_copy.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"
#include "xsched_trace.h"
#include "xjit_hooks.h"
#include "../runtime/object/xexception.h"

// ========== Forward Declarations ==========

static XrVMResult run_finalize(XrayIsolate *isolate, XrWorker *worker, XrCoroutine *coro,
                               XrVMContext *ctx, XrVMContext *coro_ctx, XrVMResult result);

static XrVMResult run_first_exec(XrayIsolate *isolate, XrWorker *worker, XrCoroutine *coro,
                                 XrVMContext *ctx, XrVMContext *coro_ctx);

static XrVMResult run_resume_path(XrayIsolate *isolate, XrWorker *worker, XrCoroutine *coro,
                                  XrVMContext *ctx, XrVMContext *coro_ctx);

static XrVMResult run_cfunc_coro(XrWorker *worker, XrCoroutine *coro, XrayIsolate *isolate);

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
    } else if (wr == (XR_CORO_WAIT_AWAIT_ALL >> XR_CORO_WAIT_SHIFT)) {
        if (atomic_load(&coro->wait_count) == 0) {
            xr_coro_ready(runtime->isolate, coro, true);
            return true;
        }
    } else if (wr == (XR_CORO_WAIT_SCOPE >> XR_CORO_WAIT_SHIFT)) {
        XrScopeContext *scope = coro->current_scope;
        if (!scope || atomic_load(&scope->count) == 0) {
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
    if (coro->select_wait)
        return false;

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
        case XR_VM_YIELD:
            xr_coro_resume_store(coro, XR_RESUME_OK);
            xr_coro_flags_clear(coro, XR_CORO_FLG_BLOCKED | XR_CORO_FLG_RUNNING);
            xr_coro_flags_set(coro, XR_CORO_FLG_READY);
            xr_worker_push(worker, coro);
            worker->p.stats.yielded_count++;
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
            xr_coro_flags_clear(coro, XR_CORO_FLG_CANCEL_REQUESTED | XR_CORO_FLG_READY |
                                          XR_CORO_FLG_BLOCKED | XR_CORO_FLG_RUNNING);
            /* Task/Executor separation: mark task cancelled.
             * Detach AFTER wake_waiter so task->waiter can be read. */
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

// Execute a coroutine with continuation stealing support.
// Handles the push-parent/exec-child/pop-parent loop.
// Also implements BLOCKED fast re-dispatch: when a coro blocks on channel
// and the LIFO slot has a just-woken coro, execute it inline without
// returning to worker_loop (avoids scheduling overhead for ping-pong patterns).
void worker_exec_with_cont_stealing(XrWorker *worker, XrCoroutine *coro) {
    XrMachine *m = worker->m;
    XrProc *p = &worker->p;
    XrVMResult result;
    int fast_dispatch_budget = XR_FAST_DISPATCH_BUDGET;

cont_exec:
    // Invariant: coro must not be NULL or DONE when entering execution
    XR_DCHECK(coro != NULL, "cont_exec: NULL coroutine");
    XR_DCHECK(!xr_coro_flags_has(coro, XR_CORO_FLG_DONE), "cont_exec: executing DONE coroutine");
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
    if (result == XR_VM_GO_CHILD) {
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
        // Clear jit_ctx: parent exited JIT (OP_GO returns from interpreter).
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
            if (_rt && atomic_load_explicit(&_rt->spinning_count, memory_order_relaxed) == 0) {
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
    if (result == XR_VM_BLOCKED && atomic_load_explicit(&p->lifo_slot, memory_order_relaxed) &&
        --fast_dispatch_budget > 0) {
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

        p->stats.executed_count++;
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
    p->stats.executed_count++;

    // Race guard: when VM returns BLOCKED after channel_recv/send, the coro
    // has already been added to channel waitq (spinlock released). Another
    // thread may have woken it and started executing it on a different worker.
    // In that case we must NOT touch any coro fields — the coro is "gone".
    if (result == XR_VM_BLOCKED && (xr_coro_resume_load(coro) == XR_RESUME_CHANNEL ||
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

    // Handle VM result
    worker_handle_vm_result(worker, coro, result);

    // Deferred recycle: fire-and-forget coro completed, defer to next pool_get.
    // gc_flags bit 2 = recyclable (set by vm_go for fire-and-forget go).
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

// ========== Yieldable C-function coroutine ==========

// First call of a cfunc coroutine: build the C frame, run the body.
static XrVMResult run_cfunc_first_exec(XrayIsolate *isolate, XrCoroutine *coro,
                                       XrVMContext *coro_ctx, uint32_t cur_flags) {
    atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_RUNNING, memory_order_release);
    atomic_store_explicit(&coro->flags,
                          (cur_flags & ~(XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED)) |
                              XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED,
                          memory_order_release);

    // Initialize first frame (for Yieldable support).
    coro_ctx->frame_count = 1;
    XrBcCallFrame *frame = &coro_ctx->frames[0];
    frame->closure = NULL;
    frame->pc = NULL;
    frame->base_offset = 1;  // Reserve stack[0] for return value.
    frame->flags = 0;
    frame->u.l.pending_operator_check = false;
    frame->call_status = XR_CALL_C;
    frame->u.c.continuation = NULL;
    frame->u.c.continuation_ctx = NULL;
    frame->u.c.result_slot = 0;
    frame->u.c.has_cfunc_result = false;

    XrValue *base = coro_ctx->stack + frame->base_offset;
    for (int i = 0; i < coro->arg_count && i < 4; i++) {
        base[i] = coro->args[i];
    }
    coro_ctx->stack_top = coro_ctx->stack + 1 + coro->arg_count;

    XrValue cfunc_result = xr_null();
    XrCFuncResult status = coro->entry.cfunc(isolate, coro->args, coro->arg_count, &cfunc_result);
    switch (status) {
        case XR_CFUNC_DONE:
            coro_ctx->stack[0] = cfunc_result;
            return XR_VM_OK;
        case XR_CFUNC_BLOCKED:
            return XR_VM_BLOCKED;
        case XR_CFUNC_YIELD:
            return XR_VM_YIELD;
        case XR_CFUNC_CALL_CLOSURE:
            // Closure frame pushed by xr_yield_call_closure, execute via VM.
            coro_ctx->module_base_frame = 0;
            return run(isolate, coro_ctx);
        default:
            return XR_VM_RUNTIME_ERROR;
    }
}

// Resume a previously-suspended cfunc coroutine. Includes an inline fast
// path for the common "single C frame with continuation" case (HTTP/WS
// handlers); falls back to xr_coro_resume_with_unroll otherwise.
static XrVMResult run_cfunc_resume(XrayIsolate *isolate, XrCoroutine *coro, XrVMContext *coro_ctx,
                                   uint32_t cur_flags) {
    atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_RUNNING, memory_order_release);
    atomic_store_explicit(&coro->flags,
                          (cur_flags & ~(XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED)) |
                              XR_CORO_FLG_RUNNING,
                          memory_order_release);

    int resume_status = xr_coro_resume_load(coro);
    if (!resume_status)
        resume_status = XR_RESUME_IO_READY;

    // Inline fast path: single C frame with continuation.
    if (coro_ctx->frame_count == 1) {
        XrBcCallFrame *frame = &coro_ctx->frames[0];
        uint8_t need = XR_CALL_C | XR_CALL_HAS_CONT | XR_CALL_YIELDED;
        if ((frame->call_status & need) == need && frame->u.c.continuation) {
            XrContinuation cont = (XrContinuation) frame->u.c.continuation;
            void *user_ctx = frame->u.c.continuation_ctx;
            XrValue cfunc_result;
            XrCFuncResult status = cont(isolate, resume_status, user_ctx, &cfunc_result);
            switch (status) {
                case XR_CFUNC_DONE:
                    coro_ctx->stack[0] = cfunc_result;
                    frame->call_status &= ~(XR_CALL_C | XR_CALL_HAS_CONT | XR_CALL_YIELDED);
                    frame->u.c.continuation = NULL;
                    coro_ctx->frame_count = 0;
                    return XR_VM_OK;
                case XR_CFUNC_BLOCKED:
                    return XR_VM_BLOCKED;
                case XR_CFUNC_YIELD:
                    return XR_VM_YIELD;
                case XR_CFUNC_CALL_CLOSURE:
                    coro_ctx->module_base_frame = 0;
                    return run(isolate, coro_ctx);
                default:
                    return XR_VM_RUNTIME_ERROR;
            }
        }
    }

    // Slow path: full unroll.
    XrVMResult result = xr_coro_resume_with_unroll(isolate, coro, resume_status);
    if (result != XR_VM_OK)
        return result;

    // Single-frame result short-circuit after successful unroll.
    if (coro_ctx->frame_count == 1) {
        XrBcCallFrame *top = &coro_ctx->frames[0];
        if (top->u.c.has_cfunc_result) {
            coro_ctx->stack[0] = top->u.c.cfunc_result;
            coro_ctx->frame_count = 0;
            return XR_VM_OK;
        }
    }
    coro_ctx->module_base_frame = 0;
    if (coro_ctx->frame_count == 0)
        return XR_VM_OK;
    return run(isolate, coro_ctx);
}

// Execute Yieldable C function coroutine (supports I/O wait and rescheduling).
//
// First-exec and resume are factored into helpers above so the
// orchestration here stays small and obvious.
static XrVMResult run_cfunc_coro(XrWorker *worker, XrCoroutine *coro, XrayIsolate *isolate) {
    XrVMContext *ctx = &worker->m->vm_ctx;
    XrVMContext *coro_ctx = &coro->vm_ctx;

    ctx->current_coro = coro;
    coro_ctx->current_coro = coro;

    uint32_t cur_flags = atomic_load_explicit(&coro->flags, memory_order_relaxed);
    XrVMResult result;
    if (!(cur_flags & XR_CORO_FLG_STARTED)) {
        result = run_cfunc_first_exec(isolate, coro, coro_ctx, cur_flags);
    } else {
        result = run_cfunc_resume(isolate, coro, coro_ctx, cur_flags);
    }

    // ========== Shared Result Handling ==========
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
        if (atomic_compare_exchange_strong_explicit(&coro->coro_state, &_exp, XR_CORO_STATE_BLOCKED,
                                                    memory_order_release, memory_order_relaxed)) {
            atomic_fetch_and_explicit(&coro->flags, ~(uint32_t) XR_CORO_FLG_RUNNING,
                                      memory_order_relaxed);
            atomic_fetch_or_explicit(&coro->flags, (uint32_t) XR_CORO_FLG_BLOCKED,
                                     memory_order_release);
        }
    } else if (result == XR_VM_YIELD) {
        xr_coro_transition_to_ready(coro);
    }

    ctx->current_coro = NULL;
    return result;
}

// ========== Worker Coroutine Execution ==========
//
// Executes directly on the coroutine's own VM value stack — no state copying.
//   - Each coroutine owns an independent XrVMContext stack and frame array.
//   - No native stack switching is performed.
//   - Eliminates state-copy race conditions across stealing.
//
// xr_coro_run_on_worker delegates execution to per-mode helpers,
// a thin dispatch shell that hands to run_first_exec / run_resume_path /
// run_cfunc_coro / run_finalize. See bottom of file for the shell.

// ========== run_finalize: Result Handling ==========
//
// Centralises the post-run state transitions and result/error copy-outs that
// every execution path in xr_coro_run_on_worker must perform.
// Assumes ctx->current_coro == coro; clears it before returning (except for
// continuation-stealing / debug-break which leave the caller in charge).
static XrVMResult run_finalize(XrayIsolate *isolate, XrWorker *worker, XrCoroutine *coro,
                               XrVMContext *ctx, XrVMContext *coro_ctx, XrVMResult result) {
    (void) worker;
    if (result == XR_VM_GO_CHILD) {
        // Continuation stealing: parent saved state, child ready to run inline.
        ctx->current_coro = NULL;
        return result;
    }
    if (result == XR_VM_DEBUG_BREAK) {
        xr_coro_flags_clear(coro, XR_CORO_FLG_RUNNING);
        xr_coro_flags_set(coro, XR_CORO_FLG_BLOCKED);
        // Don't clear current_coro — caller handles debug break.
        ctx->current_coro = NULL;
        return result;
    }
    if (result == XR_VM_BLOCKED || result == XR_VM_YIELD) {
        if (result == XR_VM_YIELD) {
            xr_coro_flags_clear(coro, XR_CORO_FLG_RUNNING);
            xr_coro_flags_set(coro, XR_CORO_FLG_READY);
        } else {
            // Channel blocks set BLOCKED under lock; await/timer leave RUNNING.
            // CAS handles both: only swap if still RUNNING.
            uint8_t _exp = XR_CORO_STATE_RUNNING;
            if (atomic_compare_exchange_strong_explicit(&coro->coro_state, &_exp,
                                                        XR_CORO_STATE_BLOCKED, memory_order_release,
                                                        memory_order_relaxed)) {
                atomic_fetch_and_explicit(&coro->flags, ~(uint32_t) XR_CORO_FLG_RUNNING,
                                          memory_order_relaxed);
                atomic_fetch_or_explicit(&coro->flags, (uint32_t) XR_CORO_FLG_BLOCKED,
                                         memory_order_release);
            }
        }
        if (result == XR_VM_BLOCKED && coro->ext &&
            atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed)) {
            XR_DBG_CORO("coro id=%d timer blocked, waiting for Timer Wheel callback", coro->id);
        }
    } else if (result == XR_VM_OK) {
        // Coroutine completed — save result but do NOT set FLG_DONE here.
        // FLG_DONE is set in worker_handle_vm_result right before wake_waiter.
        coro->result = coro_ctx->stack[0];
        XR_DBG_CORO("run_on_worker: coro id=%d completed, result tag=%u", coro->id,
                    coro_ctx->stack[0].tag);
    } else {
        // Error: capture actual exception message from vm_ctx.
        coro->result = xr_null();
        xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
        XrValue exc = coro_ctx->current_exception;
        if (xr_value_is_exception(isolate, exc)) {
            XrValue data = xr_exception_get_data(isolate, exc);
            if (!XR_IS_NULL(data) && XR_IS_STRING(data)) {
                coro->error = data;
            } else {
                const char *msg = xr_exception_get_message(isolate, exc);
                if (msg && msg[0] != '\0') {
                    XrString *s = xr_string_intern(isolate, msg, strlen(msg), 0);
                    coro->error = xr_string_value(s);
                } else {
                    XrString *s = xr_string_intern(isolate, "unknown error", 13, 0);
                    coro->error = xr_string_value(s);
                }
            }
        } else {
            XrString *s = xr_string_intern(isolate, "coroutine error", 15, 0);
            coro->error = xr_string_value(s);
        }
        coro_ctx->current_exception = xr_null();
    }

    ctx->current_coro = NULL;
    XR_DBG_CORO("run_on_worker: return result=%d, coro id=%d", result, coro->id);
    return result;
}

// ========== run_first_exec: Frame Setup + JIT Entry + Interpreter ==========
//
// Builds the coroutine's first bytecode frame (VM stack/frame/args), then tries
// the JIT entry fast path (if proto has compiled code and hasn't deopted)
// before falling back to the interpreter.
//
// Precondition: caller has already set RUNNING|STARTED on coro->flags and
// bound current_coro on both ctx and coro_ctx. coro->entry.closure must be
// a non-NULL closure with a non-NULL proto (validated upstream).
static XrVMResult run_first_exec(XrayIsolate *isolate, XrWorker *worker, XrCoroutine *coro,
                                 XrVMContext *ctx, XrVMContext *coro_ctx) {
    (void) worker;
    XrClosure *closure = coro->entry.closure;
    XrProto *proto = closure->proto;

    // Ensure stack capacity covers the entry function's register file.
    // The module init proto may require more slots than the initial 64.
    int needed = 1 + proto->maxstacksize;  // 1 reserved return slot + registers
    if (needed > coro_ctx->stack_capacity) {
        int extra = needed - coro_ctx->stack_capacity + 64;
        if (!xr_coro_grow_stack(coro, extra)) {
            return XR_VM_RUNTIME_ERROR;
        }
    }

    // Initialize frame directly on coroutine stack.
    XrValue *stack_base = coro_ctx->stack;
    stack_base[0] = xr_null();  // Reserved return slot.
    XrValue *func_base = stack_base + 1;

    for (int i = 0; i < coro->arg_count; i++) {
        func_base[i] = coro->args[i];
    }

    coro_ctx->frame_count = 1;
    XrBcCallFrame *frame = &coro_ctx->frames[0];
    frame->closure = closure;
    frame->pc = PROTO_CODE_BASE(proto);
    frame->base_offset = (int) (func_base - coro_ctx->stack);
    frame->flags = 0;
    frame->u.l.pending_operator_check = false;
    frame->call_status = 0;
    frame->u.c.continuation = NULL;
    frame->u.c.continuation_ctx = NULL;
    frame->u.c.result_slot = -1;
    frame->u.c.has_cfunc_result = false;

    coro_ctx->stack_top = coro_ctx->stack + frame->base_offset + proto->maxstacksize;
    coro_ctx->module_base_frame = 0;

    XrVMResult result;

    // JIT fast path for coroutine entry: call compiled code directly.
    // go-spawned coroutines bypass OP_CALL and would otherwise always run
    // in the interpreter even when JIT code exists. deopt_count==0 guard
    // prevents replaying a proto whose first coroutine deopted.
    if (XR_JIT_AVAILABLE() && proto->numparams == coro->arg_count) {
        if (!proto->jit_entry) {
            void *pending = atomic_load_explicit(&proto->jit_entry_pending, memory_order_acquire);
            if (pending && (uintptr_t) pending > 1) {
                xr_jit_hooks->install_bg_result(proto);
            }
        }
        if (proto->jit_entry && proto->deopt_count < 20) {
            coro->jit_ctx->call_proto = proto;
            coro->jit_ctx->call_closure = closure;
            coro->jit_ctx->call_base_offset = (int32_t) (func_base - coro_ctx->stack);
            XrValue jit_result;
            int _jrc = xr_jit_hooks->call(proto->jit_entry, coro, func_base, coro->arg_count,
                                          proto->return_type_info, &jit_result);
            if (_jrc == XR_JIT_OK) {
                coro_ctx->stack[0] = jit_result;
                return run_finalize(isolate, worker, coro, ctx, coro_ctx, XR_VM_OK);
            }
            if (_jrc == XR_JIT_SUSPEND) {
                return run_finalize(isolate, worker, coro, ctx, coro_ctx, XR_VM_BLOCKED);
            }
            // JIT deopt: disable fast path for this proto.
            proto->deopt_count++;
        }
    }

    result = run(isolate, coro_ctx);
    return run_finalize(isolate, worker, coro, ctx, coro_ctx, result);
}

// ========== run_jit_resume: Extracted JIT Resume Logic ==========
//
// Prepares resume state (channel recv value or await task result) in
// jit_suspend, then re-enters compiled code via xm_jit_resume.
//
// Returns: XR_JIT_OK, XR_JIT_SUSPEND, XR_JIT_DEOPT (fall through), or
// -1 for channel-close (caller should clear jit_resume_entry and deopt).
static int run_jit_resume(XrayIsolate *isolate, XrCoroutine *coro, XrVMContext *coro_ctx,
                          XrValue *jit_result_out) {
    XR_DCHECK(coro->jit_resume_entry != NULL, "run_jit_resume: no resume entry");
    XR_DCHECK(coro->jit_ctx != NULL, "run_jit_resume: no jit_ctx");
    XR_DCHECK(XR_JIT_AVAILABLE(), "run_jit_resume: JIT hooks not registered");

    int resume_reason = xr_coro_resume_load(coro);

    // Channel close wake: deopt to bytecode (rare edge case).
    if (resume_reason == XR_RESUME_CHANNEL_CLOSED) {
        coro->jit_resume_entry = NULL;
        coro->jit_resume_proto = NULL;
        return -1;
    }

    // Channel recv resume: copy value from recv_slot (stack[0]) to
    // jit_suspend.result where the JIT continuation reads it.
    if (resume_reason == XR_RESUME_CHANNEL) {
        XrValue rv = coro_ctx->stack[0];
        if (XR_IS_PTR(rv) && xr_value_needs_copy(rv)) {
            rv = xr_deep_copy_to_coro(isolate, rv, coro);
        }
        coro->jit_suspend->result = rv.i;
        coro->jit_suspend->result_tag = rv.tag;
    }

    // AWAIT resume: xr_task_wake_waiter only marks coro ready but does
    // NOT propagate the result — do it here to avoid stale register reads.
    if (resume_reason != XR_RESUME_CHANNEL && resume_reason != XR_RESUME_CHANNEL_CLOSED) {
        XrTask *await_task = atomic_load_explicit(&coro->await_task, memory_order_acquire);
        if (await_task) {
            uint8_t tstate = atomic_load_explicit(&await_task->state, memory_order_acquire);
            XrValue res = xr_null();
            if (tstate == XR_TASK_COMPLETED) {
                res = xr_deep_copy_to_coro(isolate, await_task->result, coro);
            }
            coro->jit_suspend->result = res.i;
            coro->jit_suspend->result_tag = res.tag;
            atomic_store_explicit(&coro->await_task, NULL, memory_order_relaxed);
        }
    }

    xr_coro_resume_store(coro, XR_RESUME_OK);
    return xr_jit_hooks->resume(coro, jit_result_out);
}

// ========== run_resume_path: JIT Resume + Continuation + Unroll ==========
//
// Handles every resume case except the inline channel-resume fast path that
// xr_coro_run_on_worker handles directly.  Supports:
//   - JIT suspend/resume (run_jit_resume re-enters compiled code)
//   - XR_RESUME_CONTINUATION / XR_RESUME_DEBUG (run() directly)
//   - Default unroll via xr_coro_resume_with_unroll then run()
static XrVMResult run_resume_path(XrayIsolate *isolate, XrWorker *worker, XrCoroutine *coro,
                                  XrVMContext *ctx, XrVMContext *coro_ctx) {
    (void) worker;
    xr_coro_transition_to_running(coro);
    XR_DBG_CORO("run_on_worker: resuming coro id=%d", coro->id);

    XrVMResult result;

    if (XR_JIT_AVAILABLE() && coro->jit_resume_entry && coro->jit_ctx) {
        XrValue jit_result;
        int jrc = run_jit_resume(isolate, coro, coro_ctx, &jit_result);
        if (jrc == XR_JIT_OK) {
            coro_ctx->stack[0] = jit_result;
            return run_finalize(isolate, worker, coro, ctx, coro_ctx, XR_VM_OK);
        }
        if (jrc == XR_JIT_SUSPEND) {
            return run_finalize(isolate, worker, coro, ctx, coro_ctx, XR_VM_BLOCKED);
        }
        // -1 (channel close) or XR_JIT_DEOPT: fall through to interpreter.
    }

    // Continuation stealing resume: vm_ctx already set, just call run().
    if (xr_coro_resume_load(coro) == XR_RESUME_CONTINUATION) {
        xr_coro_resume_store(coro, 0);
        coro_ctx->current_coro = coro;
        coro_ctx->module_base_frame = 0;
        result = run(isolate, coro_ctx);
        return run_finalize(isolate, worker, coro, ctx, coro_ctx, result);
    }

    // Debug break resumption: no unroll needed.
    if (xr_coro_resume_load(coro) == XR_RESUME_DEBUG) {
        xr_coro_resume_store(coro, 0);
        coro_ctx->current_coro = coro;
        coro_ctx->module_base_frame = 0;
        result = run(isolate, coro_ctx);
        return run_finalize(isolate, worker, coro, ctx, coro_ctx, result);
    }

    // Default: unroll then run.
    int resume_status = xr_coro_resume_load(coro) ? xr_coro_resume_load(coro) : XR_RESUME_IO_READY;
    XrVMResult unroll_result = xr_coro_resume_with_unroll(isolate, coro, resume_status);
    XR_DBG_CORO("run_on_worker: coro id=%d, unroll result=%d, frame_count=%d", coro->id,
                unroll_result, coro_ctx->frame_count);

    if (unroll_result == XR_VM_OK) {
        coro_ctx->module_base_frame = 0;
        if (coro_ctx->frame_count == 0) {
            XR_DBG_CORO("run_on_worker: frame_count=0, coroutine completed");
            result = XR_VM_OK;
        } else {
            XR_DBG_CORO("run_on_worker: continue bytecode execution, frame_count=%d",
                        coro_ctx->frame_count);
            result = run(isolate, coro_ctx);
        }
        XR_DBG_CORO("run_on_worker: bytecode execution complete, result=%d", result);
    } else if (unroll_result == XR_VM_BLOCKED) {
        result = XR_VM_BLOCKED;
    } else if (unroll_result == XR_VM_YIELD) {
        result = XR_VM_YIELD;
    } else {
        result = XR_VM_RUNTIME_ERROR;
    }

    return run_finalize(isolate, worker, coro, ctx, coro_ctx, result);
}

// ========== Thin Dispatch Shell ==========
//
// xr_coro_run_on_worker is the dispatch hub combining
// entry checks, channel-resume fast path, first-execution setup, JIT+unroll
// resume, and result finalization. It is now a thin dispatch shell that
// hands off to run_first_exec / run_resume_path / run_cfunc_coro /
// run_finalize as appropriate.
XrVMResult xr_coro_run_on_worker(XrWorker *worker, XrCoroutine *coro) {
    if (!worker || !coro)
        return XR_VM_RUNTIME_ERROR;

    if (xr_coro_flags_has(coro, XR_CORO_FLG_CANCEL_REQUESTED)) {
        return XR_VM_CANCELLED;
    }

    if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) {
        return XR_VM_OK;
    }

    XR_DCHECK(worker->p.runtime != NULL, "worker thread: runtime is NULL");
    XR_DCHECK(worker->p.runtime->isolate != NULL, "worker thread: isolate is NULL");

    XrayIsolate *isolate = worker->p.runtime->isolate;
    XrVMContext *ctx = &worker->m->vm_ctx;
    XrVMContext *coro_ctx = &coro->vm_ctx;

    // Per-Worker JIT scratch pointer (JIT code accesses via coro->jit_ctx).
    coro->jit_ctx = &worker->p.jit_scratch;
    worker->p.jit_scratch.heartbeat_ptr = &worker->m->heartbeat;

    uint32_t _fast_flags = xr_coro_flags_load(coro);
    int _fast_resume = xr_coro_resume_load(coro);

    // ========== Channel Resume Fast Path ==========
    // Most common resume case: channel wake of a started coroutine. Acquire-
    // load of flags establishes happens-before with sender's flag swap so
    // recv_slot is visible before we enter run().
    // Handles both VM bytecode and JIT coroutines (the redundant separate
    // !jit_resume_entry exclusion — JIT now uses run_jit_resume directly).
    if (_fast_resume == XR_RESUME_CHANNEL && (_fast_flags & XR_CORO_FLG_STARTED)) {
        ctx->current_coro = coro;
        coro_ctx->current_coro = coro;
        coro->next = NULL;
        coro->prev = NULL;
        xr_coro_transition_to_running(coro);

        // JIT channel resume: propagate recv_slot → jit_suspend.result,
        // then re-enter compiled code directly (no detour via run_resume_path).
        if (XR_JIT_AVAILABLE() && coro->jit_resume_entry && coro->jit_ctx) {
            XrValue jit_result;
            int jrc = run_jit_resume(isolate, coro, coro_ctx, &jit_result);
            if (jrc == XR_JIT_OK) {
                coro_ctx->stack[0] = jit_result;
                return run_finalize(isolate, worker, coro, ctx, coro_ctx, XR_VM_OK);
            }
            if (jrc == XR_JIT_SUSPEND) {
                return run_finalize(isolate, worker, coro, ctx, coro_ctx, XR_VM_BLOCKED);
            }
            // Deopt or channel_closed (-1): jit_resume_entry already cleared.
            // Deopt needs full unroll recovery → delegate to run_resume_path.
            return run_resume_path(isolate, worker, coro, ctx, coro_ctx);
        }

        // VM bytecode channel resume
        if (coro_ctx->frame_count > 0) {
            XrBcCallFrame *tf = &coro_ctx->frames[coro_ctx->frame_count - 1];
            if (tf->call_status & XR_CALL_C) {
                // C continuation: fall through to the full unroll path.
                return run_resume_path(isolate, worker, coro, ctx, coro_ctx);
            }
            tf->call_status &= ~XR_CALL_YIELDED;
        }
        coro_ctx->module_base_frame = 0;
        XrVMResult r = run(isolate, coro_ctx);
        return run_finalize(isolate, worker, coro, ctx, coro_ctx, r);
    }

    // ========== Cfunc First-Run + Resume Fast Path ==========
    // Must check BEFORE closure path: entry union overlaps.
    if (coro->entry_type == XR_CORO_ENTRY_CFUNC && coro->entry.cfunc) {
        return run_cfunc_coro(worker, coro, isolate);
    }

    // ========== Closure First Execution Fast Path ==========
    if (!(_fast_flags & XR_CORO_FLG_STARTED) && _fast_resume == 0) {
        XrClosure *closure = coro->entry.closure;
        if (closure && closure->proto) {
            ctx->current_coro = coro;
            coro_ctx->current_coro = coro;
            coro->next = NULL;
            coro->prev = NULL;
            atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_RUNNING, memory_order_release);
            atomic_store_explicit(&coro->flags,
                                  (_fast_flags & ~(XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED)) |
                                      XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED,
                                  memory_order_release);
            return run_first_exec(isolate, worker, coro, ctx, coro_ctx);
        }
    }

    // ========== Slow Path ==========
    XR_DBG_FRAME("run_on_worker: coro=%p, id=%d, stack=%p, frames=%p, frame_count=%d",
                 (void *) coro, coro->id, (void *) coro->vm_ctx.stack, (void *) coro->vm_ctx.frames,
                 coro->vm_ctx.frame_count);

    if (ctx->isolate != isolate) {
        ctx->isolate = isolate;
    }

    // Native coroutine: execute simple C callback (not Yieldable).
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

    // Cfunc (after isolate check): same as fast path.
    if (coro->entry_type == XR_CORO_ENTRY_CFUNC && coro->entry.cfunc) {
        return run_cfunc_coro(worker, coro, isolate);
    }

    if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) {
        return XR_VM_OK;
    }

    if (!coro->vm_ctx.stack || !coro->vm_ctx.frames) {
        xr_log_warning("coro", "coroutine stack not allocated (stack=%p, frames=%p, coro=%p)",
                       (void *) coro->vm_ctx.stack, (void *) coro->vm_ctx.frames, (void *) coro);
        coro->result = xr_null();
        xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
        return XR_VM_RUNTIME_ERROR;
    }

    ctx->current_coro = coro;
    coro_ctx->current_coro = coro;
    coro->next = NULL;
    coro->prev = NULL;

    // First execution slow path: closure must be validated before run_first_exec.
    uint32_t _flags_snap = atomic_load_explicit(&coro->flags, memory_order_relaxed);
    if (!(_flags_snap & XR_CORO_FLG_STARTED)) {
        atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_RUNNING, memory_order_release);
        atomic_store_explicit(
            &coro->flags,
            (_flags_snap & ~(uint32_t) (XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED)) |
                XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED,
            memory_order_release);

        XrClosure *_slow_cl = coro->entry.closure;
        if (!_slow_cl || !_slow_cl->proto) {
            xr_log_warning("coro", "coroutine closure invalid (closure=%p, coro=%p)",
                           (void *) _slow_cl, (void *) coro);
            coro->result = xr_null();
            xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
            return XR_VM_RUNTIME_ERROR;
        }
        return run_first_exec(isolate, worker, coro, ctx, coro_ctx);
    }

    return run_resume_path(isolate, worker, coro, ctx, coro_ctx);
}
