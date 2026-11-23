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
#include "xnative_map_backing.h"
#include "xnative_set_backing.h"
#include "base/xchecks.h"
#include <stdalign.h>

/* Builtin Array producer identities live for the process. These callbacks
 * certify the immutable family separately from its shared physical stride. */
static const XrtBackingArrayKind array_plan_families[] = {
    XRT_BACKING_ARRAY_I64, XRT_BACKING_ARRAY_STRING, XRT_BACKING_ARRAY_U8, XRT_BACKING_ARRAY_U64
};
_Static_assert(sizeof(array_plan_families) / sizeof(*array_plan_families) == XRT_BACKING_ARRAY_COUNT,
    "every scalar Array producer has an identity");
static bool array_family_retain(void *family) { return family != NULL; }
static void array_family_drop(void *family) { (void)family; }
XR_FUNCDEF bool xrt_backing_string_matches(const XrtManagedHeader *owner) {
    const XrtRuntimeTypeSpec spec = {.fixed_size=sizeof(XrtStringBacking),.alignment=alignof(XrtStringBacking),
        .capacity_offset=offsetof(XrtStringBacking, capacity),.stride=1};
    if (!xrt_managed_layout_matches(owner, &spec)) return false;
    const XrtStringBacking *string = (const XrtStringBacking *)owner;
    return string->capacity && string->capacity <= SIZE_MAX - sizeof(*string) &&
        string->length < string->capacity && string->runes <= string->length && !string->bytes[string->length];
}
XR_FUNCDEF bool xrt_backing_array_matches(const XrtManagedHeader *owner, XrtBackingArrayKind family) {
    const XrtRuntimeTypeSpec *spec = NULL;
    return (unsigned)family < XRT_BACKING_ARRAY_COUNT &&
        xrt_managed_producer_borrow(owner, array_family_retain, array_family_drop, &spec) == XRT_MANAGED_OK &&
        spec->code.context == &array_plan_families[family];
}

static bool backing_types_empty(const XrtBackingTypes *types) {
    if (!types || types->string || types->map || types->set) return false;
    for (size_t i = 0; i < XRT_BACKING_ARRAY_COUNT; ++i)
        if (types->arrays[i]) return false;
    return true;
}
static XrtManagedStatus backing_bind(XrtBillingOwner *billing, XrtRuntimeTypeSpec spec,
    size_t length_offset, XrtManagedOps **output) {
    XrtRuntimeTypePlan *plan = NULL;
    XrtManagedStatus status = length_offset == SIZE_MAX ? xrt_runtime_type_plan_new(billing, &spec, &plan) :
        xrt_runtime_type_plan_new_owned_prefix(billing, &spec, length_offset, &plan);
    if (status == XRT_MANAGED_OK) status = xrt_managed_ops_bind(billing, plan, output);
    xrt_runtime_type_plan_drop(plan);
    return status;
}
XR_FUNCDEF void xrt_backing_types_drop(XrtBackingTypes *types) {
    if (!types) return;
    XrtBackingTypes owned = *types;
    *types = (XrtBackingTypes){0};
    xrt_managed_ops_drop(owned.set);
    xrt_managed_ops_drop(owned.map);
    for (size_t i = XRT_BACKING_ARRAY_COUNT; i; --i) xrt_managed_ops_drop(owned.arrays[i - 1]);
    xrt_managed_ops_drop(owned.string);
}
XR_FUNCDEF XrtManagedStatus xrt_backing_types_init(XrtBillingOwner *billing, XrtBackingTypes *output) {
    if (!billing || !backing_types_empty(output)) return XRT_MANAGED_ARGUMENT;
    XrtBackingTypes types = {0};
    XrtRuntimeTypeSpec spec = {.fixed_size=sizeof(XrtStringBacking), .alignment=alignof(XrtStringBacking),
        .capacity_offset=offsetof(XrtStringBacking, capacity), .stride=1};
    XrtManagedStatus status = backing_bind(billing, spec, SIZE_MAX, &types.string);
    size_t tail = 0;
    for (size_t i = 0; status == XRT_MANAGED_OK && i < XRT_BACKING_ARRAY_COUNT; ++i) {
        spec = (XrtRuntimeTypeSpec){.fixed_size=sizeof(XrtArrayBacking), .alignment=alignof(XrtArrayBacking),
            .capacity_offset=offsetof(XrtArrayBacking, capacity), .stride=xrt_backing_array_stride((XrtBackingArrayKind)i),
            .tail_child_offsets=i == XRT_BACKING_ARRAY_STRING ? &tail : NULL,
            .tail_child_count=i == XRT_BACKING_ARRAY_STRING ? 1 : 0,
            .code={(void *)&array_plan_families[i],array_family_retain,array_family_drop}};
        size_t length_offset = i == XRT_BACKING_ARRAY_STRING ? offsetof(XrtArrayBacking,length) : SIZE_MAX;
        status = backing_bind(billing, spec, length_offset, &types.arrays[i]);
    }
    if (status == XRT_MANAGED_OK) {
        spec = (XrtRuntimeTypeSpec){.fixed_size=sizeof(XrtMapBacking), .alignment=alignof(XrtMapBacking),
            .capacity_offset=offsetof(XrtMapBacking, capacity), .stride=sizeof(XrtMapBackingRow),
            .tail_child_offsets=&tail, .tail_child_count=1};
        status = backing_bind(billing, spec, SIZE_MAX, &types.map);
    }
    if (status == XRT_MANAGED_OK) {
        spec = (XrtRuntimeTypeSpec){.fixed_size=sizeof(XrtSetBacking), .alignment=alignof(XrtSetBacking),
            .capacity_offset=offsetof(XrtSetBacking,capacity), .stride=sizeof(XrtSetBackingRow)};
        status = backing_bind(billing,spec,SIZE_MAX,&types.set);
    }
    if (status != XRT_MANAGED_OK) { xrt_backing_types_drop(&types); return status; }
    XR_CHECK(types.string && types.arrays[XRT_BACKING_ARRAY_STRING] && types.map && types.set,
        "complete setup owns every typed descriptor");
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
