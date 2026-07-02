/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xobj_destroy_ops_countdown_latch.c - CountdownLatch RC destroy capability registration.
 */

#include "xobj_destroy_ops.h"
#include "xobj_ops.h"
#include "../core/xr_runtime_core.h"

void xr_runtime_core_enable_countdown_latch_destroy_ops(struct XrRuntimeCore *core) {
    xr_runtime_core_set_destroy_op(core, XR_TCOUNTDOWNLATCH, xr_obj_destroy_countdown_latch);
}
