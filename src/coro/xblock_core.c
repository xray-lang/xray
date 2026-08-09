/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xblock_core.c - Minimal coroutine blocking primitives.
 */

#include "xblock.h"

#include <stdatomic.h>

#include "../os/os_time.h"
#include "../runtime/value/xvalue.h"
#include "xcoroutine.h"
#include "xnetpoll.h"
#include "xworker.h"
#include "xyieldable.h"

static inline XrCoroBlockResult block_result(XrCoroBlockKind kind, XrValue value, bool ok) {
    XrCoroBlockResult result = {kind, value, ok};
    return result;
}

/* Owner-thread park latch (see XrProc.suspend_park_pending).
 *
 * Publishing BLOCKED makes the coroutine claimable by wakers/cancellers on
 * the spot, so everything the suspender still owes — deferred spawn
 * submission included — happens in the publish helpers below, BEFORE the
 * state transition. After publication nothing on the suspending worker may
 * touch the coroutine again; in particular the backend's finalize must not
 * run its RUNNING→BLOCKED CAS, which would observe a RESUMER's RUNNING and
 * re-park an executing coroutine (a second waker could then double-run it).
 *
 * The one park shape that does NOT publish is the worker-local timer park
 * (xr_coro_sleep): its wake fires on this same worker after the slice ends,
 * so the coroutine stays RUNNING until finalize performs the transition.
 * That path announces itself through this latch; finalize acts only when it
 * is set and treats every other BLOCKED result as hands-off. */
static void block_note_park_pending(void) {
    XrWorker *worker = xr_current_worker();
    if (worker)
        worker->p.suspend_park_pending = true;
}

bool xr_coro_publish_wait_block(XrCoroutine *coro) {
    if (!coro)
        return false;
    xr_coro_submit_deferred_spawns(coro);
    xr_coro_transition_to_blocked(coro);
    return true;
}

XrCoroBlockSnapshot xr_coro_begin_reversible_block(XrCoroutine *coro) {
    XrCoroBlockSnapshot snapshot = {XR_CORO_STATE_NONE, false};
    if (!coro)
        return snapshot;
    xr_coro_submit_deferred_spawns(coro);
    snapshot.previous_state =
        xr_flag_to_state(atomic_load_explicit(&coro->flags, memory_order_acquire));
    snapshot.active = true;
    xr_coro_transition_to_blocked(coro);
    return snapshot;
}

void xr_coro_rollback_reversible_block(XrCoroutine *coro, XrCoroBlockSnapshot snapshot) {
    if (!coro || !snapshot.active)
        return;

    uint32_t restore_flag = xr_state_to_flag(snapshot.previous_state);
    if (restore_flag) {
        xr_coro_flags_swap(coro, XR_CORO_STATE_FLAG_MASK, restore_flag);
        return;
    }

    xr_coro_flags_clear(coro, XR_CORO_STATE_FLAG_MASK);
}

bool xr_coro_finalize_blocked_suspend(XrCoroutine *coro) {
    if (!coro)
        return false;
    XrWorker *worker = xr_current_worker();
    if (!worker) {
        /* No worker context (embedder- or test-driven resume): scheduler
         * wakers cannot race this thread, so keep the legacy semantics of
         * parking a frame that returned BLOCKED on its own. */
        xr_coro_submit_deferred_spawns(coro);
        return xr_coro_try_transition_to_blocked(coro);
    }
    /* Only the announced worker-local park (sleep) still owns the coroutine
     * here. Every published suspension (wait queues, channels, netpoll,
     * select, awaits) already submitted its deferred spawns and completed
     * its frame state before the coroutine became claimable — it may be
     * running on another worker by now, so: hands off, report parked. */
    bool park_pending = worker->p.suspend_park_pending;
    worker->p.suspend_park_pending = false;
    if (!park_pending)
        return true;
    xr_coro_submit_deferred_spawns(coro);
    return xr_coro_try_transition_to_blocked(coro);
}

void xr_coro_finish_backend_resume_tokens(XrCoroutine *coro, int resume_status) {
    if (!coro || !coro->ext || resume_status != XR_RESUME_TIMEOUT)
        return;

    int wait_reason = xr_coro_get_wait_reason(xr_coro_flags_load(coro));
    if (wait_reason == (XR_CORO_WAIT_SLEEP >> XR_CORO_WAIT_SHIFT)) {
        xr_timer_wait_token_finish(&coro->ext->wait.timer_token);
    }
}

static void coro_io_wait_finish(XrCoroutine *coro) {
    if (!coro)
        return;
    if (coro->ext)
        xr_io_wait_token_finish(&coro->ext->wait.io_token);
    xr_coro_flags_clear(coro, XR_CORO_WAIT_MASK);
}

XrCoroIoWaitKind xr_coro_io_wait_resume(XrCoroutine *coro) {
    if (!coro || !coro->ext)
        return XR_CORO_IO_WAIT_READY;
    int state = atomic_load_explicit(&coro->ext->wait.io_token.state, memory_order_acquire);
    XrCoroIoWaitKind result;
    switch (state) {
        case XR_IO_WAIT_IDLE:
        case XR_IO_WAIT_READY:
            result = XR_CORO_IO_WAIT_READY;
            break;
        case XR_IO_WAIT_TIMED_OUT:
            result = XR_CORO_IO_WAIT_TIMEOUT;
            break;
        case XR_IO_WAIT_CANCELLED:
            result = XR_CORO_IO_WAIT_CANCELLED;
            break;
        default:
            return XR_CORO_IO_WAIT_ERROR;
    }
    coro_io_wait_finish(coro);
    return result;
}

XrCoroIoWaitKind xr_coro_io_wait(XrCoroutine *coro, int fd, int poll_mode, int64_t timeout_ms) {
    if (!coro || fd < 0 || (poll_mode & (XR_POLL_READ | XR_POLL_WRITE)) == 0)
        return XR_CORO_IO_WAIT_ERROR;
    if (timeout_ms == 0)
        return XR_CORO_IO_WAIT_TIMEOUT;

    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    XrWorker *worker = xr_current_worker();
    XrRuntime *runtime = (XrRuntime *) xr_coro_scheduler(coro);
    if (!ext || !worker || !runtime)
        return XR_CORO_IO_WAIT_ERROR;

    int token_state = atomic_load_explicit(&ext->wait.io_token.state, memory_order_acquire);
    if (token_state != XR_IO_WAIT_IDLE)
        return XR_CORO_IO_WAIT_ERROR;

    XrPollDesc *pd = xr_netpoll_open(&runtime->netpoll, fd);
    if (!pd)
        return XR_CORO_IO_WAIT_ERROR;

    int owner_id = pd->owner_worker_id;
    if (owner_id >= 0 && owner_id != worker->p.id) {
        ext->resume_target_worker = owner_id;
        return XR_CORO_IO_WAIT_YIELD;
    }

    xr_netpoll_bind_worker(pd);
    pd->user_data = coro;
    _Atomic uintptr_t *gpp = (poll_mode & XR_POLL_READ) ? &pd->rg : &pd->wg;
    for (;;) {
        uintptr_t old = atomic_load_explicit(gpp, memory_order_acquire);
        if (old == XR_PD_READY) {
            if (atomic_compare_exchange_strong_explicit(gpp, &old, XR_PD_NIL, memory_order_acq_rel,
                                                        memory_order_acquire))
                return XR_CORO_IO_WAIT_READY;
            continue;
        }
        if (old != XR_PD_NIL)
            return XR_CORO_IO_WAIT_ERROR;

        xr_coro_set_wait_reason(coro, XR_CORO_WAIT_IO >> XR_CORO_WAIT_SHIFT);
        xr_io_wait_token_prepare(&ext->wait.io_token, fd, poll_mode, worker->p.id, timeout_ms);
        XrCoroBlockSnapshot snapshot = xr_coro_begin_reversible_block(coro);
        xr_io_wait_token_commit(&ext->wait.io_token);
        if (atomic_compare_exchange_strong_explicit(gpp, &old, (uintptr_t) coro,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            atomic_fetch_add_explicit(&runtime->netpoll.waiters, 1, memory_order_relaxed);
            int mode = (poll_mode & XR_POLL_READ) ? XR_POLL_READ : XR_POLL_WRITE;
            xr_netpoll_arm_mode(pd, mode);
            if (timeout_ms > 0) {
                int64_t deadline_ns =
                    (int64_t) xr_time_monotonic_ns() + timeout_ms * INT64_C(1000000);
                xr_netpoll_set_deadline(&runtime->netpoll, pd, deadline_ns, mode,
                                        worker->p.timer_wheel);
            }
            return XR_CORO_IO_WAIT_BLOCKED;
        }

        coro_io_wait_finish(coro);
        xr_coro_rollback_reversible_block(coro, snapshot);
    }
}

XrCoroBlockResult xr_coro_sleep(XrCoroutine *coro, int64_t milliseconds) {
    if (milliseconds <= 0) {
        return block_result(XR_CORO_BLOCK_READY, xr_null(), true);
    }
    if (!coro) {
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }

    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext) {
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
    }

    xr_coro_set_wait_reason(coro, XR_CORO_WAIT_SLEEP >> XR_CORO_WAIT_SHIFT);

    XrWorker *worker = xr_current_worker();
    if (!worker || !worker->p.timer_wheel) {
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }

    xr_worker_add_sleep_timer(worker, coro, milliseconds);
    if (!atomic_load_explicit(&ext->timer_active, memory_order_relaxed)) {
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
    }
    /* Worker-local park: no BLOCKED published here — the timer fires on this
     * worker after the slice ends. Announce it so the backend finalize (the
     * only remaining owner-side step) performs the transition. */
    block_note_park_pending();
    return block_result(XR_CORO_BLOCK_BLOCKED, xr_null(), false);
}
