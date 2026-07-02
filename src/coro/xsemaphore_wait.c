/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsemaphore_wait.c - Semaphore waiter cleanup/cancellation
 */

#include "xsemaphore_wait.h"

#include "../base/xchecks.h"
#include "xcoroutine.h"

void xr_semaphore_waiter_unlink_locked(XrSemaphore *sem, XrCoroutine *coro) {
    XR_DCHECK(sem != NULL, "semaphore_waiter_unlink_locked: NULL sem");
    XR_DCHECK(coro != NULL, "semaphore_waiter_unlink_locked: NULL coro");
    XR_DCHECK(coro->ext != NULL, "semaphore_waiter_unlink_locked: NULL ext");

    XrCoroutine *prev = coro->ext->wait_prev;
    XrCoroutine *next = coro->ext->wait_link;
    if (prev) {
        prev->ext->wait_link = next;
    } else {
        sem->wait_first = next;
    }
    if (next) {
        next->ext->wait_prev = prev;
    } else {
        sem->wait_last = prev;
    }
    coro->ext->wait_link = NULL;
    coro->ext->wait_prev = NULL;
    atomic_fetch_sub_explicit(&sem->waiter_count, 1, memory_order_relaxed);
}

bool xr_semaphore_waiter_remove_locked(XrSemaphore *sem, XrCoroutine *coro) {
    XR_DCHECK(sem != NULL, "semaphore_waiter_remove_locked: NULL sem");
    XR_DCHECK(coro != NULL, "semaphore_waiter_remove_locked: NULL coro");
    if (!coro->ext)
        return false;
    if (!coro->ext->wait_prev && sem->wait_first != coro)
        return false;
    xr_semaphore_waiter_unlink_locked(sem, coro);
    return true;
}

void xr_semaphore_cancel_waiter(XrCoroutine *coro) {
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (!wait)
        return;
    XrSemaphore *sem = (XrSemaphore *) atomic_load_explicit(&wait->semaphore_token.semaphore,
                                                            memory_order_acquire);
    if (!sem) {
        xr_semaphore_wait_token_cancel(&wait->semaphore_token);
        return;
    }

    xr_amutex_lock(&sem->lock);
    bool removed = xr_semaphore_waiter_remove_locked(sem, coro);
    xr_amutex_unlock(&sem->lock);
    if (removed)
        xr_semaphore_wait_token_cancel(&wait->semaphore_token);
}
