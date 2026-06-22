/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xgc_destroy_ops_result_group.c - ResultGroup RC destroy capability registration.
 */

#include "xgc_destroy_ops.h"
#include "xgc_internal.h"
#include "../core/xr_runtime_core.h"

void xr_runtime_core_enable_result_group_destroy_ops(struct XrRuntimeCore *core) {
    xr_runtime_core_set_destroy_op(core, XR_TRESULTGROUP, xr_gc_destroy_result_group);
}
