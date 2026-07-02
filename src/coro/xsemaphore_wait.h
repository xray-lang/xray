/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsemaphore_wait.h - internal Semaphore waiter cleanup helpers
 */

#ifndef XSEMAPHORE_WAIT_H
#define XSEMAPHORE_WAIT_H

#include "xsemaphore.h"

void xr_semaphore_waiter_unlink_locked(XrSemaphore *sem, struct XrCoroutine *coro);
bool xr_semaphore_waiter_remove_locked(XrSemaphore *sem, struct XrCoroutine *coro);

#endif  // XSEMAPHORE_WAIT_H
