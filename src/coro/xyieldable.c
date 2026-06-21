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
#include "xblock.h"
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
    const XrCoroBackendVTable *backend = coro ? coro->backend : NULL;
    if (!backend || !backend->setup_yield_continuation)
        return false;
    return backend->setup_yield_continuation(X, coro, (void *) cont, user_data);
}

static bool yield_prepare_io_wait(XrCoroutine *coro, int fd, int events, int64_t timeout_ms) {
    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext)
        return false;
    XrWorker *worker = xr_current_worker();
    int owner_id = worker ? worker->p.id : -1;
    xr_coro_set_wait_reason(coro, XR_CORO_WAIT_IO >> XR_CORO_WAIT_SHIFT);
    xr_io_wait_token_prepare(&ext->wait.io_token, fd, events, owner_id, timeout_ms);
    return true;
}

static void yield_commit_io_wait(XrCoroutine *coro) {
    if (coro && coro->ext)
        xr_io_wait_token_commit(&coro->ext->wait.io_token);
}

static void yield_finish_io_wait(XrCoroutine *coro) {
    if (coro && coro->ext)
        xr_io_wait_token_finish(&coro->ext->wait.io_token);
}

static void yield_abort_io_wait(XrCoroutine *coro, XrCoroBlockSnapshot block_snapshot) {
    yield_finish_io_wait(coro);
    xr_coro_rollback_reversible_block(coro, block_snapshot);
    if (coro)
        xr_coro_flags_clear(coro, XR_CORO_WAIT_MASK);
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
    // Wait tokens are prepared only on the actual-yield path so the IO-ready
    // fast path avoids touching the coroutine extension.
    if (fd >= 0) {
        XrRuntime *runtime = (XrRuntime *) X->scheduler_runtime;
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
                            if (xr_coro_consume_reds(coro, 100) <= 0) {
                                xr_coro_set_reds(coro, XR_CORO_REDUCTIONS);
                                if (!yield_setup_continuation(X, coro, cont, user_data))
                                    return XR_CFUNC_ERROR;
                                return XR_CFUNC_YIELD;
                            }
                            return cont(X, XR_RESUME_IO_READY, xr_null(), user_data, result);
                        }
                        continue;
                    }

                    if (old == XR_PD_NIL) {
                        if (!yield_prepare_io_wait(coro, fd, events, timeout_ms))
                            return XR_CFUNC_ERROR;
                        // Actually yielding — set up frame now
                        if (!yield_setup_continuation(X, coro, cont, user_data)) {
                            yield_abort_io_wait(coro, (XrCoroBlockSnapshot) {0});
                            return XR_CFUNC_ERROR;
                        }
                        XrCoroBlockSnapshot block_snapshot = xr_coro_begin_reversible_block(coro);
                        yield_commit_io_wait(coro);

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
                        yield_abort_io_wait(coro, block_snapshot);
                        continue;
                    }

                    // Another coro already waiting — error
                    return XR_CFUNC_ERROR;
                }
            }
        }
    } else if (timeout_ms >= 0) {
        // Pure timeout (no fd): set up frame + register timer in timer wheel
        if (!yield_prepare_io_wait(coro, -1, 0, timeout_ms))
            return XR_CFUNC_ERROR;
        if (!yield_setup_continuation(X, coro, cont, user_data)) {
            yield_abort_io_wait(coro, (XrCoroBlockSnapshot) {0});
            return XR_CFUNC_ERROR;
        }
        XrCoroBlockSnapshot block_snapshot = xr_coro_begin_reversible_block(coro);
        XrWorker *worker = xr_current_worker();
        if (worker) {
            xr_worker_add_sleep_timer(worker, coro, timeout_ms);
            if (coro->ext && atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed)) {
                yield_commit_io_wait(coro);
            } else {
                yield_abort_io_wait(coro, block_snapshot);
                return XR_CFUNC_ERROR;
            }
        } else {
            yield_abort_io_wait(coro, block_snapshot);
            return XR_CFUNC_ERROR;
        }
    }

    return XR_CFUNC_BLOCKED;
}

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
// io_uring completion-mode park: same coroutine registration as the I/O branch
// of xr_yield_for_io, but submits a recv/send op (with optional linked timeout)
// in place of arming a readiness poll-add. The op's CQE wakes the coro; the
// continuation reads the byte count via xr_netpoll_uring_xfer_result.
bool xr_yield_for_uring_io(XrayIsolate *X, struct XrPollDesc *pd, int mode,
                           const struct XrUringReq *req, XrContinuation cont, void *user_data,
                           XrValue *result, XrCFuncResult *out) {
    (void) result;
    XrCoroutine *coro = get_current_coro(X);
    if (!coro || !pd || !req)
        return false;

    int fd = pd->fd;
    int events = (mode == XR_POLL_WRITE) ? XR_WAIT_WRITE : XR_WAIT_READ;
    _Atomic uintptr_t *gpp = (mode == XR_POLL_WRITE) ? &pd->wg : &pd->rg;
    XrUringOp *op = (mode == XR_POLL_WRITE) ? &pd->uring_wop : &pd->uring_rop;

    xr_netpoll_bind_worker(pd);
    pd->user_data = coro;

    for (;;) {
        uintptr_t old = atomic_load(gpp);

        if (old == XR_PD_READY) {
            // No poll-add is armed for a completion direction, so a stale READY
            // is unexpected; consume it and submit the op anyway (we want the
            // recv/send result, not a bare readiness signal).
            atomic_compare_exchange_strong(gpp, &old, XR_PD_NIL);
            continue;
        }
        if (old != XR_PD_NIL) {
            *out = XR_CFUNC_ERROR;  // another coro already parked on this direction
            return true;
        }

        // old == NIL: set up the wait + continuation frame, then CAS NIL -> coro.
        // timeout_ms is delivered to the kernel via the op's linked timeout, so
        // no timer-wheel deadline is armed here (-1).
        if (!yield_prepare_io_wait(coro, fd, events, -1)) {
            *out = XR_CFUNC_ERROR;
            return true;
        }
        if (!yield_setup_continuation(X, coro, cont, user_data)) {
            yield_abort_io_wait(coro, (XrCoroBlockSnapshot) {0});
            *out = XR_CFUNC_ERROR;
            return true;
        }
        XrCoroBlockSnapshot block_snapshot = xr_coro_begin_reversible_block(coro);
        yield_commit_io_wait(coro);

        // Claim the completion direction BEFORE publishing the coro on gpp. The
        // fd stays in the worker's local epoll for readiness, so a readiness edge
        // can race this submit; xr_netpoll_ready strips readiness for a direction
        // with an active op (the CQE is the sole waker). Setting op->active only
        // AFTER the gpp CAS left a window where a readiness edge woke the coro
        // while its recv/send op was still in flight — the coro then re-parked
        // and submitted a SECOND op reusing this same XrUringOp, so one CQE was
        // lost and the coroutine hung. Claiming the direction first closes it.
        atomic_store(&op->active, true);

        uintptr_t expect = XR_PD_NIL;
        if (!atomic_compare_exchange_strong(gpp, &expect, (uintptr_t) coro)) {
            atomic_store(&op->active, false);
            yield_abort_io_wait(coro, block_snapshot);
            continue;  // raced to READY — retry
        }

        atomic_fetch_add(&pd->netpoll->waiters, 1);
        if (xr_netpoll_uring_op_submit(pd, mode, req) != 0) {
            // Submission queue exhausted (or not io_uring): unwind cleanly and
            // let the caller fall back to the readiness path.
            atomic_store(&op->active, false);
            atomic_fetch_sub(&pd->netpoll->waiters, 1);
            atomic_exchange(gpp, XR_PD_NIL);
            yield_abort_io_wait(coro, block_snapshot);
            return false;
        }
        *out = XR_CFUNC_BLOCKED;
        return true;
    }
}
#endif

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

    if (!yield_setup_continuation(X, coro, cont, user_data)) {
        return XR_CFUNC_ERROR;
    }

    // Voluntary yield has no wait token because it is not externally woken.
    return XR_CFUNC_YIELD;
}

// Native-stackful yield API was removed; suspension uses VM frames and continuations.

// ========== Coroutine Helper Functions ==========

// xr_coro_has_continuation - Check if coroutine has pending continuation
bool xr_coro_has_continuation(XrCoroutine *coro) {
    const XrCoroBackendVTable *backend = coro ? coro->backend : NULL;
    if (!backend || !backend->has_continuation) {
        return false;
    }
    return backend->has_continuation(coro);
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

    const XrCoroBackendVTable *backend = coro ? coro->backend : NULL;
    if (!backend || !backend->call_closure)
        return XR_CFUNC_ERROR;
    return backend->call_closure(X, coro, closure, args, nargs, (void *) on_complete, user_ctx,
                                 result);
}
