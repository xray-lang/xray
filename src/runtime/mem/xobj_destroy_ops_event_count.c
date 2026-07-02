/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xobj_destroy_ops_event_count.c - EventCount RC destroy capability registration.
 */

#include "xobj_destroy_ops.h"
#include "../../coro/xevent_count.h"
#include "../core/xr_runtime_core.h"

void xr_runtime_core_enable_event_count_destroy_ops(struct XrRuntimeCore *core) {
    xr_runtime_core_set_destroy_op(core, XR_TEVENTCOUNT, xr_obj_destroy_event_count);
}
