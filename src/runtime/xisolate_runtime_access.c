/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_runtime_access.c - runtime-only isolate field accessors
 */

#include "xisolate_api.h"
#include "xisolate_internal.h"
#include "core/xr_runtime_core.h"

XrRuntimeCore *xr_isolate_get_runtime_core(XrayIsolate *X) {
    return X ? X->core_rt : NULL;
}

XrRuntime *xr_isolate_get_scheduler_runtime(XrayIsolate *X) {
    return X ? X->scheduler_runtime : NULL;
}

XrGC *xr_isolate_get_gc(XrayIsolate *X) {
    return (X && X->core_rt) ? &X->core_rt->gc : NULL;
}

struct XrSystemHeap *xr_isolate_get_sys_heap(XrayIsolate *X) {
    return (X && X->core_rt) ? X->core_rt->sys_heap : NULL;
}

struct XrayCoreClasses *xr_isolate_get_core_classes(XrayIsolate *X) {
    return X ? X->core : NULL;
}

uint64_t xr_isolate_get_ext_finalize_bitmap(XrayIsolate *isolate) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    return core ? core->ext_finalize_bitmap : 0;
}

uint64_t xr_isolate_get_ext_has_refs_bitmap(XrayIsolate *isolate) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    return core ? core->ext_has_refs_bitmap : 0;
}

XrExtDestroyFn xr_isolate_get_ext_destroy(XrayIsolate *isolate, uint8_t type_id) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    if (!core || type_id >= XGC_MAX_TYPES)
        return NULL;
    return (XrExtDestroyFn) core->ext_destroy_funcs[type_id];
}

XrExtTraverseFn xr_isolate_get_ext_traverse(XrayIsolate *isolate, uint8_t type_id) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    if (!core || type_id >= XGC_MAX_TYPES)
        return NULL;
    return (XrExtTraverseFn) core->ext_traverse_funcs[type_id];
}
