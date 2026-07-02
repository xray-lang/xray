/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xevent_count_wait.c - EventCount waiter cleanup/cancellation
 */

#include "xevent_count_wait.h"

#include "../base/xchecks.h"
#include "xcoroutine.h"

void xr_event_count_waiter_unlink_locked(XrEventCount *event, XrCoroutine *coro) {
    XR_DCHECK(event != NULL, "event_count_waiter_unlink_locked: NULL event");
    XR_DCHECK(coro != NULL, "event_count_waiter_unlink_locked: NULL coro");
    XR_DCHECK(coro->ext != NULL, "event_count_waiter_unlink_locked: NULL ext");

    XrCoroutine *prev = coro->ext->wait_prev;
    XrCoroutine *next = coro->ext->wait_link;
    if (prev) {
        prev->ext->wait_link = next;
    } else {
        event->wait_first = next;
    }
    if (next) {
        next->ext->wait_prev = prev;
    } else {
        event->wait_last = prev;
    }
    coro->ext->wait_link = NULL;
    coro->ext->wait_prev = NULL;
    atomic_fetch_sub_explicit(&event->waiter_count, 1, memory_order_relaxed);
}

bool xr_event_count_waiter_remove_locked(XrEventCount *event, XrCoroutine *coro) {
    XR_DCHECK(event != NULL, "event_count_waiter_remove_locked: NULL event");
    XR_DCHECK(coro != NULL, "event_count_waiter_remove_locked: NULL coro");
    if (!coro->ext)
        return false;
    if (!coro->ext->wait_prev && event->wait_first != coro)
        return false;
    xr_event_count_waiter_unlink_locked(event, coro);
    return true;
}

void xr_event_count_cancel_waiter(XrCoroutine *coro) {
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (!wait)
        return;
    XrEventCount *event =
        (XrEventCount *) atomic_load_explicit(&wait->event_count_token.event, memory_order_acquire);
    if (!event) {
        xr_event_count_wait_token_cancel(&wait->event_count_token);
        return;
    }

    xr_amutex_lock(&event->lock);
    bool removed = xr_event_count_waiter_remove_locked(event, coro);
    xr_amutex_unlock(&event->lock);
    if (removed)
        xr_event_count_wait_token_cancel(&wait->event_count_token);
}
