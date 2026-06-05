/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_await.h - AOT aggregate await runtime bridge
 */

#ifndef XAOT_AWAIT_H
#define XAOT_AWAIT_H

#include <stdbool.h>

#include "xaot_coro.h"

XR_FUNC XrAotResult xr_aot_await_all_tasks(const XrAotContext *ctx, XrValue tasks_value,
                                           XrSlotRef out_slot);
XR_FUNC XrAotResult xr_aot_await_all_tasks_resume(const XrAotContext *ctx, XrValue tasks_value,
                                                  XrSlotRef out_slot);
XR_FUNC XrAotResult xr_aot_await_any_task(const XrAotContext *ctx, XrValue tasks_value,
                                          XrSlotRef out_slot, bool success_only);
XR_FUNC XrAotResult xr_aot_await_any_task_resume(const XrAotContext *ctx, XrSlotRef out_slot);

#endif  // XAOT_AWAIT_H
