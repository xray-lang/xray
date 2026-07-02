/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcountdown_latch_wait.c - CountdownLatch waiter cleanup/cancellation
 */

#include "xcountdown_latch_wait.h"

#include "../base/xchecks.h"
#include "xcoroutine.h"

void xr_countdown_latch_waiter_unlink_locked(XrCountdownLatch *latch, XrCoroutine *coro) {
    XR_DCHECK(latch != NULL, "countdown_latch_waiter_unlink_locked: NULL latch");
    XR_DCHECK(coro != NULL, "countdown_latch_waiter_unlink_locked: NULL coro");
    XR_DCHECK(coro->ext != NULL, "countdown_latch_waiter_unlink_locked: NULL ext");

    XrCoroutine *prev = coro->ext->wait_prev;
    XrCoroutine *next = coro->ext->wait_link;
    if (prev) {
        prev->ext->wait_link = next;
    } else {
        latch->wait_first = next;
    }
    if (next) {
        next->ext->wait_prev = prev;
    } else {
        latch->wait_last = prev;
    }
    coro->ext->wait_link = NULL;
    coro->ext->wait_prev = NULL;
    atomic_fetch_sub_explicit(&latch->waiter_count, 1, memory_order_relaxed);
}

bool xr_countdown_latch_waiter_remove_locked(XrCountdownLatch *latch, XrCoroutine *coro) {
    XR_DCHECK(latch != NULL, "countdown_latch_waiter_remove_locked: NULL latch");
    XR_DCHECK(coro != NULL, "countdown_latch_waiter_remove_locked: NULL coro");
    if (!coro->ext)
        return false;
    if (!coro->ext->wait_prev && latch->wait_first != coro)
        return false;
    xr_countdown_latch_waiter_unlink_locked(latch, coro);
    return true;
}

void xr_countdown_latch_cancel_waiter(XrCoroutine *coro) {
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (!wait)
        return;
    XrCountdownLatch *latch = (XrCountdownLatch *) atomic_load_explicit(
        &wait->countdown_latch_token.latch, memory_order_acquire);
    if (!latch) {
        xr_countdown_latch_wait_token_cancel(&wait->countdown_latch_token);
        return;
    }

    xr_amutex_lock(&latch->lock);
    bool removed = xr_countdown_latch_waiter_remove_locked(latch, coro);
    xr_amutex_unlock(&latch->lock);
    if (removed)
        xr_countdown_latch_wait_token_cancel(&wait->countdown_latch_token);
}
