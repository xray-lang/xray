/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xevent_count_wait.h - internal EventCount waiter cleanup helpers
 */

#ifndef XEVENT_COUNT_WAIT_H
#define XEVENT_COUNT_WAIT_H

#include "xevent_count.h"

XR_FUNC void xr_event_count_waiter_unlink_locked(XrEventCount *event, struct XrCoroutine *coro);
XR_FUNC bool xr_event_count_waiter_remove_locked(XrEventCount *event, struct XrCoroutine *coro);

#endif  // XEVENT_COUNT_WAIT_H
