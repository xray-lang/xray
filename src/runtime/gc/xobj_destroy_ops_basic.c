/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xobj_destroy_ops_basic.c - Basic RC destroy capability registration.
 */

#include "xobj_destroy_ops.h"
#include "xgc_internal.h"
#include "../core/xr_runtime_core.h"

void xr_runtime_core_enable_basic_destroy_ops(struct XrRuntimeCore *core) {
    xr_runtime_core_set_destroy_op(core, XR_TCOROUTINE, xr_obj_destroy_coroutine);
}
