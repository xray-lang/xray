/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xwork_queue_wait.h - internal WorkQueue waiter cleanup helpers
 */

#ifndef XWORK_QUEUE_WAIT_H
#define XWORK_QUEUE_WAIT_H

#include <stdbool.h>

#include "xwork_queue.h"

struct XrCoroutine;

void xr_work_queue_waiter_unlink_locked(XrWorkQueue *q, struct XrCoroutine *coro);
bool xr_work_queue_waiter_remove_locked(XrWorkQueue *q, struct XrCoroutine *coro);

#endif  // XWORK_QUEUE_WAIT_H
