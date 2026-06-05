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
 *   - Continuation storage is delegated to the active coroutine backend
 */

#include "xyieldable.h"
#include "../base/xchecks.h"
#include "../os/os_time.h"
#include "xcoroutine.h"
#include "xworker.h"
#include "xnetpoll.h"
#include "xexec_frame.h"
#include "../runtime/xisolate_internal.h"

// ========== Internal Helper Functions ==========

// Get current coroutine via per-worker TLS (thread-safe for multi-worker)
static inline XrCoroutine *get_current_coro(XrayIsolate *X) {
    (void) X;
    XrWorker *worker = xr_current_worker();
    if (worker && worker->m) {
        XrCoroutine *c = atomic_load_explicit(&worker->m->current_coro, memory_order_relaxed);
        if (c)
            return c;
    }
    return NULL;
}

// Get current time (microseconds)
static int64_t get_time_us(void) {
    return (int64_t) (xr_time_monotonic_ns() / 1000ULL);
}

static inline bool yield_setup_continuation(XrayIsolate *X, XrCoroutine *coro, XrContinuation cont,
                                            void *user_data) {
    if (!coro || !coro->backend || !coro->backend->setup_yield_continuation)
        return false;
    return coro->backend->setup_yield_continuation(X, coro, (void *) cont, user_data);
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
                              XrContinuation cont, void *user_data, XrValue *result) {
    XR_DCHECK(X != NULL, "yield_for_io: NULL isolate");
    XrCoroutine *coro = get_current_coro(X);
    if (!coro)
        return XR_CFUNC_ERROR;

    // Register with netpoll (single-direction, Go runtime netpoll design).
    // ensure_ext / yield_info writes are DEFERRED to the actual-yield path
    // so the IO-ready fast path pays zero overhead (Opt6).
    if (fd >= 0) {
        XrRuntime *runtime = (XrRuntime *) X->vm.runtime;
        if (runtime) {
            XrPollDesc *pd = xr_netpoll_open(&runtime->netpoll, fd);
            if (pd) {
                xr_netpoll_bind_worker(pd);
                pd->user_data = coro;
                _Atomic uintptr_t *gpp = (events & XR_WAIT_READ) ? &pd->rg : &pd->wg;

                // Two-phase CAS (Go netpollblock design):
                // Step 1: consume pdReady or confirm NIL
                // Step 2: CAS NIL → coro (not atomic_store!)
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
                                if (xr_coro_jit_try_mode(coro))
                                    return XR_CFUNC_WOULD_BLOCK;
                                if (!yield_setup_continuation(X, coro, cont, user_data))
                                    return XR_CFUNC_ERROR;
                                return XR_CFUNC_YIELD;
                            }
                            return cont(X, XR_RESUME_IO_READY, xr_null(), user_data, result);
                        }
                        continue;
                    }

                    if (old == XR_PD_NIL) {
                        // JIT try-mode: IO not ready, return without side effects
                        if (xr_coro_jit_try_mode(coro))
                            return XR_CFUNC_WOULD_BLOCK;
                        // Actually yielding — set up frame now
                        if (!yield_setup_continuation(X, coro, cont, user_data))
                            return XR_CFUNC_ERROR;

                        // CAS NIL → coro (prevents overwriting concurrent READY)
                        if (atomic_compare_exchange_strong(gpp, &old, (uintptr_t) coro)) {
                            atomic_fetch_add(&runtime->netpoll.waiters, 1);
                            int mode = (events & XR_WAIT_READ) ? XR_POLL_READ : XR_POLL_WRITE;
                            xr_netpoll_arm_mode(pd, mode);
                            if (timeout_ms > 0) {
                                int64_t deadline_ns = get_time_us() * 1000 + timeout_ms * 1000000LL;
                                XrWorker *worker = xr_current_worker();
                                XrTimerWheel *tw = worker ? worker->p.timer_wheel : NULL;
                                xr_netpoll_set_deadline(&runtime->netpoll, pd, deadline_ns, mode,
                                                        tw);
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
        if (xr_coro_jit_try_mode(coro))
            return XR_CFUNC_WOULD_BLOCK;
        // Pure timeout (no fd): set up frame + register timer in timer wheel
        if (!yield_setup_continuation(X, coro, cont, user_data))
            return XR_CFUNC_ERROR;
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
XrCFuncResult xr_yield_for_timeout(XrayIsolate *X, int64_t timeout_ms, XrContinuation cont,
                                   void *user_data, XrValue *result) {
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
    if (xr_coro_jit_try_mode(coro)) {
        return XR_CFUNC_WOULD_BLOCK;
    }

    if (!yield_setup_continuation(X, coro, cont, user_data)) {
        return XR_CFUNC_ERROR;
    }

    // Voluntary yield: no IO wait info needed (yield_info is debug-only)
    return XR_CFUNC_YIELD;
}

// Native-stackful yield API was removed; suspension uses VM frames and continuations.

// ========== Coroutine Helper Functions ==========

// xr_coro_has_continuation - Check if coroutine has pending continuation
bool xr_coro_has_continuation(XrCoroutine *coro) {
    if (!coro || !coro->backend || !coro->backend->has_continuation) {
        return false;
    }
    return coro->backend->has_continuation(coro);
}

// ========== Call Closure from C Layer ==========
//
// xr_call_closure() allows C-layer code to invoke a user closure that may
// itself yield (channel/await/sleep). The closure is pushed as a new call
// frame on the coroutine's VM backend stack. When the closure returns, the VM
// detects CLOSURE_PENDING on the caller frame and invokes the continuation
// with XR_RESUME_CLOSURE_DONE and the closure's return value delivered
// through the resume_value parameter (or XR_RESUME_CLOSURE_ERROR plus the
// uncaught exception value on failure).
XrCFuncResult xr_call_closure(XrayIsolate *X, XrClosure *closure, XrValue *args, int nargs,
                              XrContinuation on_complete, void *user_ctx, XrValue *result) {
    XR_DCHECK(X != NULL, "call_closure: NULL isolate");
    XR_DCHECK(closure != NULL, "call_closure: NULL closure");
    XR_DCHECK(closure->proto != NULL, "call_closure: NULL proto");
    XR_DCHECK(on_complete != NULL, "call_closure: NULL continuation");

    XrCoroutine *coro = get_current_coro(X);
    /* No current coroutine is a real misuse, but the C API surface
     * already returns XR_CFUNC_ERROR for caller-side errors, so prefer
     * the runtime path over an unconditional abort. */
    if (!coro)
        return XR_CFUNC_ERROR;

    if (!coro->backend || !coro->backend->call_closure)
        return XR_CFUNC_ERROR;
    return (XrCFuncResult) coro->backend->call_closure(X, coro, closure, args, nargs,
                                                       (void *) on_complete, user_ctx, result);
}
