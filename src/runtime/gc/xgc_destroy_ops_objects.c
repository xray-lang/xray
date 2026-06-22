/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xgc_destroy_ops_objects.c - Object RC destroy capability registration.
 */

#include "xgc_destroy_ops.h"
#include "xgc_internal.h"
#include "../core/xr_runtime_core.h"

void xr_runtime_core_enable_object_destroy_ops(struct XrRuntimeCore *core) {
    xr_runtime_core_set_destroy_op(core, XR_TARRAY, xr_gc_destroy_array);
    xr_runtime_core_set_destroy_op(core, XR_TMAP, xr_gc_destroy_map);
    xr_runtime_core_set_destroy_op(core, XR_TSET, xr_gc_destroy_set);
    xr_runtime_core_set_destroy_op(core, XR_TINSTANCE, xr_gc_destroy_instance);
    xr_runtime_core_set_destroy_op(core, XR_TFUNCTION, xr_gc_destroy_closure);
    xr_runtime_core_set_destroy_op(core, XR_TCELL, xr_gc_destroy_cell);
}
