/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xresult_group_wait.c - ResultGroup waiter cleanup/cancellation
 */

#include "xresult_group_wait.h"

#include "../base/xchecks.h"
#include "xcoroutine.h"

void xr_result_group_waiter_unlink_locked(XrResultGroup *g, XrCoroutine *coro) {
    XR_DCHECK(g != NULL, "result_group_waiter_unlink_locked: NULL group");
    XR_DCHECK(coro != NULL, "result_group_waiter_unlink_locked: NULL coro");
    XR_DCHECK(coro->ext != NULL, "result_group_waiter_unlink_locked: NULL ext");

    XrCoroutine *prev = coro->ext->wait_prev;
    XrCoroutine *next = coro->ext->wait_link;
    if (prev) {
        prev->ext->wait_link = next;
    } else {
        g->wait_first = next;
    }
    if (next) {
        next->ext->wait_prev = prev;
    } else {
        g->wait_last = prev;
    }
    coro->ext->wait_link = NULL;
    coro->ext->wait_prev = NULL;
    atomic_fetch_sub_explicit(&g->waiter_count, 1, memory_order_relaxed);
}

bool xr_result_group_waiter_remove_locked(XrResultGroup *g, XrCoroutine *coro) {
    XR_DCHECK(g != NULL, "result_group_waiter_remove_locked: NULL group");
    XR_DCHECK(coro != NULL, "result_group_waiter_remove_locked: NULL coro");
    if (!coro->ext)
        return false;
    if (!coro->ext->wait_prev && g->wait_first != coro)
        return false;
    xr_result_group_waiter_unlink_locked(g, coro);
    return true;
}

void xr_result_group_cancel_waiter(XrCoroutine *coro) {
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (!wait)
        return;
    XrResultGroup *g = (XrResultGroup *) atomic_load_explicit(&wait->result_group_token.group,
                                                              memory_order_acquire);
    if (!g) {
        xr_result_group_wait_token_cancel(&wait->result_group_token);
        return;
    }

    xr_amutex_lock(&g->lock);
    bool removed = xr_result_group_waiter_remove_locked(g, coro);
    xr_amutex_unlock(&g->lock);
    if (removed)
        xr_result_group_wait_token_cancel(&wait->result_group_token);
}
