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

XR_FUNC int64_t xr_aot_await_all_tasks_count(const XrAotContext *ctx, XrValue tasks_value);
XR_FUNC XrAotResult xr_aot_await_all_tasks_wait(const XrAotContext *ctx, XrValue tasks_value);
XR_FUNC XrAotResult xr_aot_await_all_tasks_wait_resume(const XrAotContext *ctx,
                                                       XrValue tasks_value);
XR_FUNC bool xr_aot_await_all_tasks_collect_into_array(const XrAotContext *ctx, XrValue tasks_value,
                                                       XrValue results_value,
                                                       uint8_t result_elem_type,
                                                       bool aggregate_one_shot);
XR_FUNC XrAotResult xr_aot_await_all_tasks(const XrAotContext *ctx, XrValue tasks_value,
                                           XrSlotRef out_slot, uint8_t result_elem_type,
                                           bool aggregate_one_shot);
XR_FUNC XrAotResult xr_aot_await_all_tasks_resume(const XrAotContext *ctx, XrValue tasks_value,
                                                  XrSlotRef out_slot, uint8_t result_elem_type,
                                                  bool aggregate_one_shot);
XR_FUNC XrAotResult xr_aot_await_all_task_values(const XrAotContext *ctx,
                                                 const XrValue *task_values, int task_count,
                                                 XrSlotRef out_slot, uint8_t result_elem_type,
                                                 bool aggregate_one_shot);
XR_FUNC XrAotResult xr_aot_await_all_task_values_resume(const XrAotContext *ctx,
                                                        const XrValue *task_values, int task_count,
                                                        XrSlotRef out_slot,
                                                        uint8_t result_elem_type,
                                                        bool aggregate_one_shot);
XR_FUNC XrAotResult xr_aot_await_all_task_values_to_slots(
    const XrAotContext *ctx, const XrValue *task_values, int task_count,
    const XrSlotRef *result_slots, uint8_t result_elem_type, bool aggregate_one_shot);
XR_FUNC XrAotResult xr_aot_await_all_task_values_to_slots_resume(
    const XrAotContext *ctx, const XrValue *task_values, int task_count,
    const XrSlotRef *result_slots, uint8_t result_elem_type, bool aggregate_one_shot);
XR_FUNC XrAotResult xr_aot_await_any_task(const XrAotContext *ctx, XrValue tasks_value,
                                          XrSlotRef out_slot, bool success_only);
XR_FUNC XrAotResult xr_aot_await_any_task_resume(const XrAotContext *ctx, XrSlotRef out_slot);

#endif  // XAOT_AWAIT_H
