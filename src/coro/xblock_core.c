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

#include "../runtime/value/xvalue.h"
#include "xcoroutine.h"
#include "xworker.h"
#include "xyieldable.h"

static inline XrCoroBlockResult block_result(XrCoroBlockKind kind, XrValue value, bool ok) {
    XrCoroBlockResult result = {kind, value, ok};
    return result;
}

bool xr_coro_publish_wait_block(XrCoroutine *coro) {
    if (!coro)
        return false;
    xr_coro_transition_to_blocked(coro);
    return true;
}

XrCoroBlockSnapshot xr_coro_begin_reversible_block(XrCoroutine *coro) {
    XrCoroBlockSnapshot snapshot = {XR_CORO_STATE_NONE, false};
    if (!coro)
        return snapshot;
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
    return block_result(XR_CORO_BLOCK_BLOCKED, xr_null(), false);
}
