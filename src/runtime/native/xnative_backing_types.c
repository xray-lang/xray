/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_backing_types.c - Failure-atomic cold binding of fixed storage plans
 *
 * KEY CONCEPT:
 *   Hot factories receive stable typed descriptors instead of arbitrary Ops.
 */
#include "xnative_array_backing.h"
#include "base/xchecks.h"
#include <stdalign.h>

static bool backing_types_empty(const XrtBackingTypes *types) {
    if (!types || types->string) return false;
    for (size_t i = 0; i < XRT_BACKING_ARRAY_COUNT; ++i)
        if (types->arrays[i]) return false;
    return true;
}
static XrtManagedStatus backing_bind(XrtBillingOwner *billing, XrtRuntimeTypeSpec spec,
    XrtManagedOps **output) {
    XrtRuntimeTypePlan *plan = NULL;
    XrtManagedStatus status = xrt_runtime_type_plan_new(billing, &spec, &plan);
    if (status == XRT_MANAGED_OK) status = xrt_managed_ops_bind(billing, plan, output);
    xrt_runtime_type_plan_drop(plan);
    return status;
}
XR_FUNCDEF void xrt_backing_types_drop(XrtBackingTypes *types) {
    if (!types) return;
    XrtBackingTypes owned = *types;
    *types = (XrtBackingTypes){0};
    for (size_t i = XRT_BACKING_ARRAY_COUNT; i; --i) xrt_managed_ops_drop(owned.arrays[i - 1]);
    xrt_managed_ops_drop(owned.string);
}
XR_FUNCDEF XrtManagedStatus xrt_backing_types_init(XrtBillingOwner *billing, XrtBackingTypes *output) {
    if (!billing || !backing_types_empty(output)) return XRT_MANAGED_ARGUMENT;
    XrtBackingTypes types = {0};
    XrtRuntimeTypeSpec spec = {.fixed_size=sizeof(XrtStringBacking), .alignment=alignof(XrtStringBacking),
        .capacity_offset=offsetof(XrtStringBacking, capacity), .stride=1};
    XrtManagedStatus status = backing_bind(billing, spec, &types.string);
    size_t tail = 0;
    for (size_t i = 0; status == XRT_MANAGED_OK && i < XRT_BACKING_ARRAY_COUNT; ++i) {
        spec = (XrtRuntimeTypeSpec){.fixed_size=sizeof(XrtArrayBacking), .alignment=alignof(XrtArrayBacking),
            .capacity_offset=offsetof(XrtArrayBacking, capacity), .stride=xrt_backing_array_stride((XrtBackingArrayKind)i),
            .tail_child_offsets=i == XRT_BACKING_ARRAY_STRING ? &tail : NULL,
            .tail_child_count=i == XRT_BACKING_ARRAY_STRING ? 1 : 0};
        status = backing_bind(billing, spec, &types.arrays[i]);
    }
    if (status != XRT_MANAGED_OK) { xrt_backing_types_drop(&types); return status; }
    XR_CHECK(types.string && types.arrays[XRT_BACKING_ARRAY_STRING], "complete setup owns every typed descriptor");
    *output = types;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_backing_slot_copy(XrtManagedFrame *frame, size_t slot,
    const XrtManagedHeader *borrowed) {
    if (!borrowed) return XRT_MANAGED_ARGUMENT;
    // Payload constness does not freeze the live allocation's ownership counter.
    XrtManagedHeader *owned = (XrtManagedHeader *)borrowed;
    if (!xrt_managed_retain(owned)) return XRT_MANAGED_REFCOUNT;
    XrtManagedStatus status = xrt_managed_slot_adopt(frame, slot, &owned);
    xrt_managed_drop(owned);
    return status;
}
