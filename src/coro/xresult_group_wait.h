/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xresult_group_wait.h - internal ResultGroup waiter cleanup helpers
 */

#ifndef XRESULT_GROUP_WAIT_H
#define XRESULT_GROUP_WAIT_H

#include <stdbool.h>

#include "xresult_group.h"

struct XrCoroutine;

void xr_result_group_waiter_unlink_locked(XrResultGroup *g, struct XrCoroutine *coro);
bool xr_result_group_waiter_remove_locked(XrResultGroup *g, struct XrCoroutine *coro);

#endif  // XRESULT_GROUP_WAIT_H
