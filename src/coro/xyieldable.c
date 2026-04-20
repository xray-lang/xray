/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xyieldable.c - C function yieldable protocol implementation (simplified)
 *
 * KEY CONCEPT:
 *   - Unified internal implementation, avoid duplicate code
 *   - Only two public APIs: xr_yield and xr_yield_for_io
 *   - All yield functions set XR_CALL_C | XR_CALL_HAS_CONT | XR_CALL_YIELDED
 */

#include "xyieldable.h"
#include "../base/xchecks.h"
#include "xcoroutine.h"
#include "xworker.h"
#include "../vm/xvm_internal.h"
#include "xnetpoll.h"
#include "../runtime/xray_debug.h"
#include "xresume.h"
#include "xexec_frame.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/xisolate_internal.h"
#include <stdlib.h>
#include <string.h>

// ========== Internal Helper Functions ==========

// Get current coroutine via per-worker TLS (thread-safe for multi-worker)
static inline XrCoroutine *get_current_coro(XrayIsolate *X) {
    (void)X;
    XrWorker *worker = xr_current_worker();
    if (worker && worker->m && worker->m->current_coro)
        return worker->m->current_coro;
    return NULL;
}

// Get current time (microseconds)
static int64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// yield_setup_frame - Set frame state for yield.
// Replaces memset with targeted field writes (Opt2: ~48B memset eliminated).
static inline XrBcCallFrame *yield_setup_frame(XrayIsolate *X, XrCoroutine *coro,
                                                XrContinuation cont, void *user_data) {
    XrBcCallFrame *frames;
    int frame_count;

    // Priority 1: coro's own vm_ctx (single source of truth for cfunc coros)
    if (coro->vm_ctx.frame_count > 0 && coro->vm_ctx.frames) {
        frames = coro->vm_ctx.frames;
        frame_count = coro->vm_ctx.frame_count;
    }
    // Priority 2: isolate vm_ctx (for yieldable C funcs called from VM)
    else if (X && X->vm_ctx.frame_count > 0 && X->vm_ctx.frames) {
        frames = X->vm_ctx.frames;
        frame_count = X->vm_ctx.frame_count;
    } else {
        return NULL;
    }

    XrBcCallFrame *frame = &frames[frame_count - 1];

    // Save result_slot before overwriting u.c (u.c and u.l share memory)
    int16_t saved_result_slot = frame->u.c.result_slot;

    // Targeted field writes instead of memset(&frame->u, 0, sizeof(frame->u)).
    // Only write the fields we actually use — saves ~48B zero-fill per yield.
    frame->u.c.continuation = (void*)cont;
    frame->u.c.continuation_ctx = user_data;
    frame->u.c.has_cfunc_result = false;
    frame->u.c.cfunc_result = xr_null();

    frame->call_status |= XR_CALL_C | XR_CALL_HAS_CONT | XR_CALL_YIELDED;

    // Restore result_slot
    if (saved_result_slot >= 0) {
        frame->u.c.result_slot = saved_result_slot;
    } else if (coro->pending_result_slot >= 0) {
        frame->u.c.result_slot = coro->pending_result_slot;
    } else {
        frame->u.c.result_slot = -1;
    }
    coro->pending_result_slot = -1;

    return frame;
}

// ========== Public API ==========

// xr_yield_for_io - Wait for I/O event and yield (core function)
//
// Unified handling of all blocking wait scenarios:
//   - fd >= 0, timeout_ms < 0:  pure I/O wait
//   - fd < 0, timeout_ms >= 0:  pure timeout wait
//   - fd >= 0, timeout_ms >= 0: I/O + timeout
//
// Params:
//   fd: file descriptor (-1 means no I/O wait)
//   events: wait events (XR_WAIT_READ / XR_WAIT_WRITE)
//   timeout_ms: timeout (ms, -1 means no timeout)
//   cont: continuation function
//   user_data: user data
//
// Returns: XR_CFUNC_BLOCKED
XrCFuncResult xr_yield_for_io(XrayIsolate *X, int fd, int events, int64_t timeout_ms,
                               XrContinuation cont, void *user_data,
                               XrValue *result) {
    XR_DCHECK(X != NULL, "yield_for_io: NULL isolate");
    XrCoroutine *coro = get_current_coro(X);
    if (!coro) return XR_CFUNC_ERROR;

    // Register with netpoll (single-direction, Go runtime netpoll design).
    // ensure_ext / yield_info writes are DEFERRED to the actual-yield path
    // so the IO-ready fast path pays zero overhead (Opt6).
    if (fd >= 0) {
        XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
        if (runtime) {
            XrPollDesc *pd = xr_netpoll_open(&runtime->netpoll, fd);
            if (pd) {
                pd->user_data = coro;
                _Atomic uintptr_t *gpp = (events & XR_WAIT_READ) ? &pd->rg : &pd->wg;

                // Two-phase CAS (Go netpollblock design):
                // Phase 1: consume pdReady or confirm NIL
                // Phase 2: CAS NIL → coro (not atomic_store!)
                for (;;) {
                    uintptr_t old = atomic_load(gpp);

                    if (old == XR_PD_READY) {
                        if (atomic_compare_exchange_strong(gpp, &old, XR_PD_NIL)) {
                            // IO already ready — call continuation directly.
                            // Charge reductions to bound C-stack recursion.
                            coro->reductions -= 100;
                            if (coro->reductions <= 0) {
                                coro->reductions = XR_CORO_REDUCTIONS;
                                // JIT try-mode: can't recurse, bail out
                                if (coro->jit_try_mode)
                                    return XR_CFUNC_WOULD_BLOCK;
                                XrBcCallFrame *frame = yield_setup_frame(X, coro, cont, user_data);
                                if (!frame) return XR_CFUNC_ERROR;
                                return XR_CFUNC_YIELD;
                            }
                            return cont(X, XR_RESUME_IO_READY, user_data, result);
                        }
                        continue;
                    }

                    if (old == XR_PD_NIL) {
                        // JIT try-mode: IO not ready, return without side effects
                        if (coro->jit_try_mode)
                            return XR_CFUNC_WOULD_BLOCK;
                        // Actually yielding — set up frame now
                        XrBcCallFrame *frame = yield_setup_frame(X, coro, cont, user_data);
                        if (!frame) return XR_CFUNC_ERROR;

                        // CAS NIL → coro (prevents overwriting concurrent READY)
                        if (atomic_compare_exchange_strong(gpp, &old, (uintptr_t)coro)) {
                            atomic_fetch_add(&runtime->netpoll.waiters, 1);
                            if (timeout_ms > 0) {
                                int64_t deadline_ns = get_time_us() * 1000 + timeout_ms * 1000000LL;
                                int mode = (events & XR_WAIT_READ) ? XR_POLL_READ : XR_POLL_WRITE;
                                XrWorker *worker = xr_current_worker();
                                XrTimerWheel *tw = worker ? worker->p.timer_wheel : NULL;
                                xr_netpoll_set_deadline(&runtime->netpoll, pd, deadline_ns, mode, tw);
                            }
                            return XR_CFUNC_BLOCKED;
                        }
                        // CAS failed: state changed (likely READY), retry loop
                        continue;
                    }

                    // Another coro already waiting — error
                    return XR_CFUNC_ERROR;
                }
            }
        }
    } else if (timeout_ms >= 0) {
        // JIT try-mode: timeout always requires yield
        if (coro->jit_try_mode)
            return XR_CFUNC_WOULD_BLOCK;
        // Pure timeout (no fd): set up frame + register timer in timer wheel
        XrBcCallFrame *frame = yield_setup_frame(X, coro, cont, user_data);
        if (!frame) return XR_CFUNC_ERROR;
        XrWorker *worker = xr_current_worker();
        if (worker) {
            xr_worker_add_sleep_timer(worker, coro, timeout_ms);
        }
    }

    return XR_CFUNC_BLOCKED;
}

// xr_yield_for_timeout - Wait for timeout and yield (convenience function)
//
// Equivalent to xr_yield_for_io(X, -1, 0, timeout_ms, cont, user_data)
XrCFuncResult xr_yield_for_timeout(XrayIsolate *X, int64_t timeout_ms,
                                    XrContinuation cont, void *user_data,
                                    XrValue *result) {
    return xr_yield_for_io(X, -1, 0, timeout_ms, cont, user_data, result);
}

// xr_yield - Voluntary yield (no wait condition)
//
// For C functions needing multi-step execution without I/O wait.
// Returns XR_CFUNC_YIELD, scheduler immediately reschedules this coroutine.
//
// Params:
//   cont: continuation function
//   user_data: user data
//
// Returns: XR_CFUNC_YIELD
XrCFuncResult xr_yield(XrayIsolate *X, XrContinuation cont, void *user_data) {
    XR_DCHECK(X != NULL, "yield: NULL isolate");
    XrCoroutine *coro = get_current_coro(X);
    if (!coro) {
        return XR_CFUNC_ERROR;
    }

    // JIT try-mode: voluntary yield cannot complete inline
    if (coro->jit_try_mode) {
        return XR_CFUNC_WOULD_BLOCK;
    }

    // Use unified frame setup function
    XrBcCallFrame *frame = yield_setup_frame(X, coro, cont, user_data);
    if (!frame) {
        return XR_CFUNC_ERROR;
    }

    // Voluntary yield: no IO wait info needed (yield_info is debug-only)
    return XR_CFUNC_YIELD;
}

// Stackful yield API removed — fully stackless now

// ========== Coroutine Helper Functions ==========

// xr_coro_has_continuation - Check if coroutine has pending continuation
bool xr_coro_has_continuation(XrCoroutine *coro) {
    if (!coro || coro->vm_ctx.frame_count == 0) {
        return false;
    }
    XrBcCallFrame *frame = &coro->vm_ctx.frames[coro->vm_ctx.frame_count - 1];
    return (frame->call_status & XR_CALL_HAS_CONT) && frame->u.c.continuation;
}

// ========== Yield-Style Call Closure ==========
//
// xr_yield_call_closure() allows C-layer code to invoke a user closure
// that may itself yield (channel/await/sleep). The closure is pushed as a
// new call frame on the coroutine's VM stack. When the closure returns,
// the VM detects CLOSURE_PENDING on the caller frame and invokes the
// continuation with XR_RESUME_CLOSURE_DONE.
//
// Previously this lived in its own 131-line xyield_closure.c — folded in
// here as part of Phase 3.4 file-count cleanup; it shares get_current_coro.
XrCFuncResult xr_yield_call_closure(
    XrayIsolate *X,
    XrClosure *closure,
    XrValue *args, int nargs,
    XrContinuation on_complete,
    void *user_ctx,
    XrValue *result)
{
    XR_DCHECK(X != NULL, "yield_call_closure: NULL isolate");
    XR_DCHECK(closure != NULL, "yield_call_closure: NULL closure");
    XR_DCHECK(closure->proto != NULL, "yield_call_closure: NULL proto");
    XR_DCHECK(on_complete != NULL, "yield_call_closure: NULL continuation");

    XrCoroutine *coro = get_current_coro(X);
    XR_DCHECK(coro != NULL, "yield_call_closure: no current coroutine");
    if (!coro) return XR_CFUNC_ERROR;

    XrVMContext *ctx = &coro->vm_ctx;
    XrProto *proto = closure->proto;

    XR_DCHECK(ctx->frame_count > 0, "yield_call_closure: no active frame");
    XrBcCallFrame *caller = &ctx->frames[ctx->frame_count - 1];

    /* Calculate where the closure frame's registers start.
     * For bytecode caller: after caller's register file.
     * For cfunc coro frame-0: at current stack_top. */
    int closure_base_offset;
    if (caller->closure && caller->closure->proto) {
        closure_base_offset = caller->base_offset + caller->closure->proto->maxstacksize;
    } else {
        closure_base_offset = (int)(ctx->stack_top - ctx->stack);
    }

    // Ensure stack and frame capacity
    int needed = closure_base_offset + proto->maxstacksize + 1;
    if (needed > ctx->stack_capacity || ctx->frame_count + 1 >= ctx->frame_capacity) {
        int extra = needed - ctx->stack_capacity + 64;
        if (extra < 64) extra = 64;
        bool ok = xr_coro_gc_grow_stack(coro, extra);
        if (!ok) return XR_CFUNC_ERROR;
        // Re-read after potential realloc
        caller = &ctx->frames[ctx->frame_count - 1];
    }

    // OP_RETURN writes return value to base_offset - 1
    int return_slot_offset = closure_base_offset - 1;
    if (return_slot_offset >= 0 && return_slot_offset < ctx->stack_capacity) {
        ctx->stack[return_slot_offset] = xr_null();
    }

    // Mark caller frame: continuation will be called when closure returns
    caller->call_status |= XR_CALL_CLOSURE_PENDING | XR_CALL_HAS_CONT | XR_CALL_C;
    caller->u.c.continuation = (void *)on_complete;
    caller->u.c.continuation_ctx = user_ctx;
    caller->u.c.has_cfunc_result = false;
    caller->u.c.result_slot = (int16_t)(return_slot_offset - caller->base_offset);

    // Push closure call frame
    XrBcCallFrame *frame = &ctx->frames[ctx->frame_count++];
    memset(frame, 0, sizeof(XrBcCallFrame));
    frame->closure = closure;
    frame->pc = PROTO_CODE_BASE(proto);
    frame->base_offset = closure_base_offset;

    // Copy arguments into closure's register file
    XrValue *closure_base = ctx->stack + closure_base_offset;
    int copy_count = nargs < proto->numparams ? nargs : proto->numparams;
    for (int i = 0; i < copy_count; i++) {
        closure_base[i] = args[i];
    }
    for (int i = copy_count; i < proto->maxstacksize; i++) {
        closure_base[i] = xr_null();
    }

    // Update stack top
    ctx->stack_top = ctx->stack + closure_base_offset + proto->maxstacksize;

    (void)result;
    return XR_CFUNC_CALL_CLOSURE;
}

XrValue xr_get_closure_result(XrayIsolate *X) {
    XrCoroutine *coro = get_current_coro(X);
    if (coro) {
        return coro->pending_closure_result;
    }
    return xr_null();
}
