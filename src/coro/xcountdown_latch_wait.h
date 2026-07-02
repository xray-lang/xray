/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcountdown_latch_wait.h - internal CountdownLatch waiter cleanup helpers
 */

#ifndef XCOUNTDOWN_LATCH_WAIT_H
#define XCOUNTDOWN_LATCH_WAIT_H

#include "xcountdown_latch.h"

void xr_countdown_latch_waiter_unlink_locked(XrCountdownLatch *latch, struct XrCoroutine *coro);
bool xr_countdown_latch_waiter_remove_locked(XrCountdownLatch *latch, struct XrCoroutine *coro);

#endif  // XCOUNTDOWN_LATCH_WAIT_H
