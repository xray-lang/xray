/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xwork_queue_wait.c - WorkQueue waiter cleanup/cancellation
 */

#include "xwork_queue_wait.h"

#include "../base/xchecks.h"
#include "xcoroutine.h"

void xr_work_queue_waiter_unlink_locked(XrWorkQueue *q, XrCoroutine *coro) {
    XR_DCHECK(q != NULL, "work_queue_waiter_unlink_locked: NULL queue");
    XR_DCHECK(coro != NULL, "work_queue_waiter_unlink_locked: NULL coro");
    XR_DCHECK(coro->ext != NULL, "work_queue_waiter_unlink_locked: NULL ext");

    XrCoroutine *prev = coro->ext->wait_prev;
    XrCoroutine *next = coro->ext->wait_link;
    if (prev) {
        prev->ext->wait_link = next;
    } else {
        q->wait_first = next;
    }
    if (next) {
        next->ext->wait_prev = prev;
    } else {
        q->wait_last = prev;
    }
    coro->ext->wait_link = NULL;
    coro->ext->wait_prev = NULL;
    coro->ext->work_queue_hint = -1;
    atomic_fetch_sub_explicit(&q->waiter_count, 1, memory_order_relaxed);
}

bool xr_work_queue_waiter_remove_locked(XrWorkQueue *q, XrCoroutine *coro) {
    XR_DCHECK(q != NULL, "work_queue_waiter_remove_locked: NULL queue");
    XR_DCHECK(coro != NULL, "work_queue_waiter_remove_locked: NULL coro");
    if (!coro->ext)
        return false;

    if (!coro->ext->wait_prev && q->wait_first != coro)
        return false;

    xr_work_queue_waiter_unlink_locked(q, coro);
    return true;
}

void xr_work_queue_cancel_waiter(XrCoroutine *coro) {
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (!wait)
        return;
    XrWorkQueue *q =
        (XrWorkQueue *) atomic_load_explicit(&wait->work_queue_token.queue, memory_order_acquire);
    if (!q) {
        xr_work_queue_wait_token_cancel(&wait->work_queue_token);
        return;
    }

    xr_amutex_lock(&q->wait_lock);
    bool removed = xr_work_queue_waiter_remove_locked(q, coro);
    xr_amutex_unlock(&q->wait_lock);
    if (removed)
        xr_work_queue_wait_token_cancel(&wait->work_queue_token);
}
