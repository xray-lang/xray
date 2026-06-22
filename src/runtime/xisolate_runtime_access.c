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

XrRuntimeCore *xr_isolate_get_runtime_core(XrVMRuntime *X) {
    return X ? X->core_rt : NULL;
}

XrRuntime *xr_isolate_get_scheduler_runtime(XrVMRuntime *X) {
    return X ? X->vm.scheduler : NULL;
}

XrFixedHeap *xr_isolate_get_fixed_heap(XrVMRuntime *X) {
    return (X && X->core_rt) ? &X->core_rt->fixed_heap : NULL;
}

struct XrSystemHeap *xr_isolate_get_sys_heap(XrVMRuntime *X) {
    return (X && X->core_rt) ? X->core_rt->sys_heap : NULL;
}

struct XrayCoreClasses *xr_isolate_get_core_classes(XrVMRuntime *X) {
    return X ? X->core : NULL;
}

XrTypeRegistry *xr_isolate_get_type_registry(XrVMRuntime *X) {
    return (X && X->core_rt) ? X->core_rt->type_registry : NULL;
}

void xr_isolate_set_type_registry(XrVMRuntime *X, XrTypeRegistry *registry) {
    if (X && X->core_rt)
        X->core_rt->type_registry = registry;
}

void *xr_isolate_get_symbol_table(XrVMRuntime *isolate) {
    return (isolate && isolate->core_rt) ? isolate->core_rt->symbol_table : NULL;
}

XrClass *xr_isolate_get_native_type_class(XrVMRuntime *X, uint8_t type_id) {
    if (!X || !X->core_rt || type_id >= XR_NATIVE_TYPE_MAX)
        return NULL;
    return X->core_rt->native_type_classes[type_id];
}

void xr_isolate_set_native_type_class(XrVMRuntime *X, uint8_t type_id, XrClass *cls) {
    if (X && X->core_rt && type_id < XR_NATIVE_TYPE_MAX)
        X->core_rt->native_type_classes[type_id] = cls;
}

uint64_t xr_isolate_get_ext_finalize_bitmap(XrVMRuntime *isolate) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    return core ? core->ext_finalize_bitmap : 0;
}

uint64_t xr_isolate_get_ext_has_refs_bitmap(XrVMRuntime *isolate) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    return core ? core->ext_has_refs_bitmap : 0;
}

XrExtDestroyFn xr_isolate_get_ext_destroy(XrVMRuntime *isolate, uint8_t type_id) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    if (!core || type_id >= XR_OBJ_TYPE_MAX)
        return NULL;
    return (XrExtDestroyFn) core->ext_destroy_funcs[type_id];
}

XrExtTraverseFn xr_isolate_get_ext_traverse(XrVMRuntime *isolate, uint8_t type_id) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    if (!core || type_id >= XR_OBJ_TYPE_MAX)
        return NULL;
    return (XrExtTraverseFn) core->ext_traverse_funcs[type_id];
}
