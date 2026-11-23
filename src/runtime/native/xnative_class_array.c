/*
 * xray - Lightweight typed scripting with native concurrency
 * https://xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_class_array.c - Array transactions on live Class fields
 *
 * KEY CONCEPT:
 *   Every alias shares its instance's field guard and stable backing slot.
 *   Allocation and retirement finish before leaving the guard.
 */
#include "xnative_class_array.h"
#include "xnative_array_ops.h"

XR_FUNCDEF XrtManagedStatus xrt_class_array_field_copy(const XrtClassFieldAccess *access,
    XrtManagedFrame *frame, size_t destination) {
    XrtExternalOwner field;
    XrtManagedStatus status = xrt_class_array_binding(access,&field,NULL);
    if (status != XRT_MANAGED_OK) return status;
    return xrt_place_copy_replace((XrtPlace){.frame=frame,.slot=destination,.nulls=XRT_PLACE_NONNULL},
        (XrtPlace){.external=&field,.nulls=XRT_PLACE_NONNULL}, NULL);
}
XR_FUNCDEF XrtManagedStatus xrt_class_array_field_replace(const XrtClassFieldAccess *access,
    const XrtManagedHeader *incoming) {
    XrtExternalOwner field;
    XrtBackingArrayKind kind;
    XrtManagedStatus status = xrt_class_array_binding(access,&field,&kind);
    if (status != XRT_MANAGED_OK) return status;
    if (!xrt_backing_array_matches(incoming,kind)) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlotLocation guard = {.busy=field.busy};
    status = xrt_managed_domain_enter(guard);
    if (status != XRT_MANAGED_OK) return status;
    status = field.preflight(field.context,XRT_PLACE_REPLACE);
    if (status == XRT_MANAGED_OK && *field.owner != incoming) {
        if (!xrt_managed_retain((XrtManagedHeader *)incoming)) status = XRT_MANAGED_REFCOUNT;
        else {
            XrtManagedHeader *old = *field.owner;
            *field.owner = (XrtManagedHeader *)incoming;
            xrt_managed_drop(old);
        }
    }
    xrt_managed_domain_leave(guard);
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_class_array_field_push(const XrtClassFieldAccess *access,
    XrtManagedFrame *frame, size_t workspace, const XrtBackingTypes *types, int64_t item) {
    XrtExternalOwner field;
    XrtBackingArrayKind kind;
    XrtManagedStatus status = xrt_class_array_binding(access,&field,&kind);
    if (status != XRT_MANAGED_OK) return status;
    XrtArrayUpdate update = {.operation=XRT_ARRAY_UPDATE_PUSH,.item={.kind=kind}};
    if (kind == XRT_BACKING_ARRAY_U8) {
        if (item < 0 || item > UINT8_MAX) return XRT_MANAGED_ARGUMENT;
        update.item.value.u8 = (uint8_t)item;
    } else update.item.value.i64 = item;
    return xrt_array_update_place((XrtPlace){.external=&field,.nulls=XRT_PLACE_NONNULL},
        kind, frame, workspace, types, &update);
}
