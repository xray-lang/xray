/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_task.h - AOT Task runtime bridge
 */

#ifndef XAOT_TASK_H
#define XAOT_TASK_H

#include "xaot_coro.h"

XR_FUNC XrValue xr_aot_task_cancel(const XrAotContext *ctx, XrValue task_value);
XR_FUNC XrValue xr_aot_task_done(const XrAotContext *ctx, XrValue task_value);
XR_FUNC XrValue xr_aot_task_cancelled(const XrAotContext *ctx, XrValue task_value);
XR_FUNC XrValue xr_aot_task_result(const XrAotContext *ctx, XrValue task_value);
XR_FUNC XrValue xr_aot_task_error(const XrAotContext *ctx, XrValue task_value);
XR_FUNC XrValue xr_aot_coro_set_priority(const XrAotContext *ctx, XrValue target_value,
                                         XrValue priority_value);

#endif  // XAOT_TASK_H
