/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xgc_destroy_ops_full.c - Full RC destroy capability registration.
 */

#include "xgc_destroy_ops.h"

void xr_runtime_core_enable_full_destroy_ops(struct XrRuntimeCore *core) {
    xr_runtime_core_enable_basic_destroy_ops(core);
    xr_runtime_core_enable_object_destroy_ops(core);
    xr_runtime_core_enable_task_destroy_ops(core);
    xr_runtime_core_enable_channel_destroy_ops(core);
    xr_runtime_core_enable_work_queue_destroy_ops(core);
    xr_runtime_core_enable_result_group_destroy_ops(core);
}
