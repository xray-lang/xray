/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xgc_destroy_ops.h - Runtime-core owned RC destroy capability registration.
 */

#ifndef XGC_DESTROY_OPS_H
#define XGC_DESTROY_OPS_H

#include "../../base/xdefs.h"

struct XrRuntimeCore;

XR_FUNC void xr_runtime_core_enable_basic_destroy_ops(struct XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_enable_object_destroy_ops(struct XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_enable_task_destroy_ops(struct XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_enable_channel_destroy_ops(struct XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_enable_work_queue_destroy_ops(struct XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_enable_result_group_destroy_ops(struct XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_enable_full_destroy_ops(struct XrRuntimeCore *core);

#endif  // XGC_DESTROY_OPS_H
