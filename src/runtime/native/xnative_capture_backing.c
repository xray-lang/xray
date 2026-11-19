/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_capture_backing.c - Fixed captures under independent plan/code leases
 *
 * KEY CONCEPT:
 *   Instances retain Ops, never their creator setup. Declared child slots use
 *   the kernel's iterative retirement without a recursive payload destructor.
 */
#include "xnative_capture_backing.h"
#include "base/xchecks.h"
#include <stdalign.h>
#include <string.h>

struct XrtCaptureType {
    XrtBillingOwner *billing;
    XrtManagedOps *ops;
    const XrtValuePlan *plan;
    size_t allocation_bytes, object_bytes;
    XrtValuePlanSpec shape, payload;
};
_Static_assert(sizeof(XrtCaptureType) % alignof(size_t) == 0,
    "cold shifted-offset storage follows the setup with native alignment");

/* Only the environment factory installs this producer pair. The context stays
 * the independent immutable plan, never a creator setup or bound code image. */
static bool environment_plan_retain(void *context) {
    XrtValuePlanSpec shape;
    return xrt_value_plan_view(context, &shape) == XRT_MANAGED_OK &&
        shape.kind == XRT_VALUE_ENVIRONMENT && xrt_value_plan_retain(context);
}
static void environment_plan_drop(void *context) { xrt_value_plan_drop(context); }

XR_FUNCDEF void xrt_capture_type_drop(XrtCaptureType **owned) {
    if (!owned || !*owned) return;
    XrtCaptureType *type = *owned;
    *owned = NULL;
    XrtBillingOwner *billing = type->billing;
    XrtManagedOps *ops = type->ops;
    size_t bytes = type->allocation_bytes;
    xrt_managed_metadata_free(billing, type, bytes);
    xrt_managed_ops_drop(ops);
    xrt_billing_owner_drop(billing);
}
XR_FUNCDEF XrtManagedStatus xrt_capture_type_new(XrtBillingOwner *billing,
    const XrtValuePlan *plan, XrtCaptureType **empty) {
    if (!billing || !plan || !empty || *empty) return XRT_MANAGED_ARGUMENT;
    XrtValuePlanSpec shape, payload;
    XrtManagedStatus status = xrt_value_plan_view(plan, &shape);
    if (status != XRT_MANAGED_OK) return status;
    payload = shape;
    if (shape.kind == XRT_VALUE_CELL) {
        status = xrt_value_plan_view(shape.element, &payload);
        if (status != XRT_MANAGED_OK || (payload.kind != XRT_VALUE_I64 && payload.kind != XRT_VALUE_MAP_STRING_I64))
            return XRT_MANAGED_ARGUMENT;
    } else if (shape.kind != XRT_VALUE_ENVIRONMENT) return XRT_MANAGED_ARGUMENT;
    if (payload.size > SIZE_MAX - sizeof(XrtManagedHeader) ||
        payload.owned_count > (SIZE_MAX - sizeof(XrtCaptureType)) / sizeof(size_t)) return XRT_MANAGED_OVERFLOW;
    if (!xrt_billing_owner_retain(billing)) return XRT_MANAGED_REFCOUNT;
    size_t bytes = sizeof(XrtCaptureType) + payload.owned_count * sizeof(size_t);
    void *memory = NULL;
    status = xrt_managed_metadata_allocate(billing, bytes, &memory);
    if (status != XRT_MANAGED_OK) { xrt_billing_owner_drop(billing); return status; }
    XrtCaptureType *type = memory;
    *type = (XrtCaptureType){.billing=billing, .plan=plan, .allocation_bytes=bytes,
        .object_bytes=sizeof(XrtManagedHeader) + payload.size, .shape=shape, .payload=payload};
    XrtManagedLease lease = shape.kind == XRT_VALUE_ENVIRONMENT ?
        (XrtManagedLease){(void *)plan, environment_plan_retain, environment_plan_drop} :
        xrt_value_plan_lease((XrtValuePlan *)plan);
    status = xrt_inline_payload_backing_bind(billing, &payload, (size_t *)(type + 1), lease, &type->ops);
    if (status != XRT_MANAGED_OK) { xrt_capture_type_drop(&type); return status; }
    *empty = type;
    return XRT_MANAGED_OK;
}
static XrtManagedStatus capture_finish(XrtManagedFrame *frame, size_t slot, XrtManagedStatus status) {
    if (status == XRT_MANAGED_OK) status = xrt_managed_slot_commit(frame, slot);
    if (status != XRT_MANAGED_OK)
        XR_CHECK(xrt_managed_slot_abort(frame, slot) == XRT_MANAGED_OK, "a capture factory aborts only its own prepared token");
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_environment_create(XrtManagedFrame *frame, size_t slot,
    const XrtCaptureType *type, const void *captures) {
    if (!type || type->shape.kind != XRT_VALUE_ENVIRONMENT ||
        (type->payload.size && (!captures || (uintptr_t)captures % type->payload.alignment))) return XRT_MANAGED_ARGUMENT;
    XrtManagedHeader *payload = NULL;
    XrtManagedStatus status = xrt_managed_slot_prepare(frame, slot, type->ops, type->object_bytes, &payload);
    if (status != XRT_MANAGED_OK) return status;
    status = xrt_value_copy_empty(type->plan, captures, payload + 1);
    return capture_finish(frame, slot, status);
}
XR_FUNCDEF XrtManagedStatus xrt_environment_borrow(const XrtCaptureType *type,
    const XrtManagedHeader *owner, XrtEnvironmentBorrow *empty) {
    if (!type || type->shape.kind != XRT_VALUE_ENVIRONMENT || !owner || owner->ops != type->ops ||
        !empty || empty->owner || empty->plan || empty->payload) return XRT_MANAGED_ARGUMENT;
    *empty = (XrtEnvironmentBorrow){owner, type->plan, owner + 1};
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_environment_borrow_owned(const XrtManagedHeader *owner,
    XrtEnvironmentBorrow *empty) {
    if (!empty || empty->owner || empty->plan || empty->payload) return XRT_MANAGED_ARGUMENT;
    const XrtRuntimeTypeSpec *physical = NULL;
    XrtManagedStatus status = xrt_managed_producer_borrow(owner,
        environment_plan_retain, environment_plan_drop, &physical);
    if (status != XRT_MANAGED_OK) return status;
    const XrtValuePlan *plan = physical->code.context;
    XrtValuePlanSpec shape;
    status = xrt_value_plan_view(plan, &shape);
    if (status != XRT_MANAGED_OK || shape.kind != XRT_VALUE_ENVIRONMENT ||
        shape.size > SIZE_MAX - sizeof(XrtManagedHeader)) return XRT_MANAGED_ARGUMENT;
    size_t alignment = shape.alignment > alignof(XrtManagedHeader) ? shape.alignment : alignof(XrtManagedHeader);
    if (physical->fixed_size != sizeof(XrtManagedHeader) + shape.size || physical->alignment != alignment ||
        physical->capacity_offset != SIZE_MAX || physical->stride || physical->drop_payload ||
        physical->tail_child_count || physical->child_count != shape.owned_count ||
        (physical->child_count && !physical->child_offsets)) return XRT_MANAGED_ARGUMENT;
    for (size_t i = 0; i < shape.owned_count; ++i)
        if (physical->child_offsets[i] != sizeof(XrtManagedHeader) + shape.owned_offsets[i])
            return XRT_MANAGED_ARGUMENT;
    *empty = (XrtEnvironmentBorrow){owner, plan, owner + 1};
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_environment_capture_borrow(const XrtEnvironmentBorrow *environment,
    size_t index, const XrtValuePlan **empty_type, const void **empty_payload) {
    if (!environment || !environment->owner || !environment->plan || !environment->payload ||
        !empty_type || *empty_type || !empty_payload || *empty_payload) return XRT_MANAGED_ARGUMENT;
    XrtValuePlanSpec shape;
    XrtManagedStatus status = xrt_value_plan_view(environment->plan, &shape);
    if (status != XRT_MANAGED_OK || shape.kind != XRT_VALUE_ENVIRONMENT) return XRT_MANAGED_ARGUMENT;
    if (index >= shape.capture_count) return XRT_MANAGED_BOUNDS;
    *empty_type = shape.captures[index].type;
    *empty_payload = (const unsigned char *)environment->payload + shape.captures[index].offset;
    return XRT_MANAGED_OK;
}
static bool cell_type(const XrtCaptureType *type, XrtValueKind kind) {
    return type && type->shape.kind == XRT_VALUE_CELL && type->payload.kind == kind;
}
XR_FUNCDEF XrtManagedStatus xrt_i64_cell_create(XrtManagedFrame *frame, size_t slot,
    const XrtCaptureType *type, int64_t initial) {
    if (!cell_type(type, XRT_VALUE_I64)) return XRT_MANAGED_ARGUMENT;
    XrtManagedHeader *payload = NULL;
    XrtManagedStatus status = xrt_managed_slot_prepare(frame, slot, type->ops, type->object_bytes, &payload);
    if (status != XRT_MANAGED_OK) return status;
    ((XrtI64Cell *)payload)->value = initial;
    return capture_finish(frame, slot, XRT_MANAGED_OK);
}
XR_FUNCDEF XrtManagedStatus xrt_map_cell_create(XrtManagedFrame *frame, size_t slot,
    const XrtCaptureType *type, const XrtMapBacking *initial) {
    if (!cell_type(type, XRT_VALUE_MAP_STRING_I64) || !initial) return XRT_MANAGED_ARGUMENT;
    XrtManagedHeader *payload = NULL;
    XrtManagedStatus status = xrt_managed_slot_prepare(frame, slot, type->ops, type->object_bytes, &payload);
    if (status != XRT_MANAGED_OK) return status;
    XrtManagedHeader *owned = (XrtManagedHeader *)&initial->managed;
    if (!xrt_managed_retain(owned)) return capture_finish(frame, slot, XRT_MANAGED_REFCOUNT);
    ((XrtMapCell *)payload)->value = owned;
    return capture_finish(frame, slot, XRT_MANAGED_OK);
}
XR_FUNCDEF XrtManagedStatus xrt_i64_cell_read(const XrtI64Cell *cell, int64_t *output) {
    if (!cell || !output) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlotLocation domain = {.cell_domain=(XrtManagedHeader *)&cell->managed};
    XrtManagedStatus status = xrt_managed_domain_enter(domain);
    if (status != XRT_MANAGED_OK) return status;
    *output = cell->value;
    xrt_managed_domain_leave(domain);
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_map_cell_borrow(const XrtMapCell *cell, const XrtMapBacking **empty) {
    if (!cell || !empty || *empty) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlotLocation domain = {.cell_domain=(XrtManagedHeader *)&cell->managed};
    XrtManagedStatus status = xrt_managed_domain_enter(domain);
    if (status != XRT_MANAGED_OK) return status;
    if (!cell->value) status = XRT_MANAGED_ARGUMENT;
    else *empty = (const XrtMapBacking *)cell->value;
    xrt_managed_domain_leave(domain);
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_map_cell_read_copy(XrtManagedFrame *frame, size_t slot, const XrtMapCell *cell) {
    XrtExternalOwner binding = {0};
    XrtManagedStatus status = xrt_map_cell_external((XrtMapCell *)cell, &binding);
    return status == XRT_MANAGED_OK ? xrt_place_copy_replace(
        (XrtPlace){.frame=frame, .slot=slot, .nulls=XRT_PLACE_NONNULL},
        (XrtPlace){.external=&binding, .nulls=XRT_PLACE_NONNULL}, NULL) : status;
}
XR_FUNCDEF XrtManagedStatus xrt_i64_cell_write_exclusive(XrtI64Cell *cell, int64_t value) {
    if (!cell) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlotLocation domain = {.cell_domain=&cell->managed};
    XrtManagedStatus status = xrt_managed_domain_enter(domain);
    if (status != XRT_MANAGED_OK) return status;
    if (xr_signed_rc_read(&cell->managed.references) <= 0) status = XRT_MANAGED_ARGUMENT;
    else cell->value = value;
    xrt_managed_domain_leave(domain);
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_map_cell_write_exclusive(XrtMapCell *cell, const XrtMapBacking *value) {
    if (!cell || !value) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlotLocation domain = {.cell_domain=&cell->managed};
    XrtManagedStatus status = xrt_managed_domain_enter(domain);
    if (status != XRT_MANAGED_OK) return status;
    XrtManagedHeader *owned = (XrtManagedHeader *)&value->managed;
    if (!cell->value || xr_signed_rc_read(&cell->managed.references) <= 0) status = XRT_MANAGED_ARGUMENT;
    else if (!xrt_managed_retain(owned)) status = XRT_MANAGED_REFCOUNT;
    else {
        XrtManagedHeader *old = cell->value;
        cell->value = owned;
        xrt_managed_drop(old);
    }
    xrt_managed_domain_leave(domain);
    return status;
}
static XrtManagedStatus map_cell_preflight(void *context, XrtPlaceAction action) {
    XrtMapCell *cell = context;
    if (!xrt_managed_domain_held((XrtManagedSlotLocation){.cell_domain=&cell->managed}) || !cell->value ||
        action == XRT_PLACE_MOVE_FROM ||
        (action == XRT_PLACE_REPLACE && xr_signed_rc_read(&cell->managed.references) <= 0))
        return XRT_MANAGED_ARGUMENT;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_map_cell_external(XrtMapCell *cell, XrtExternalOwner *empty) {
    if (!cell || !empty || empty->owner || empty->busy || empty->preflight || empty->context || empty->cell_domain)
        return XRT_MANAGED_ARGUMENT;
    *empty = (XrtExternalOwner){.owner=&cell->value, .preflight=map_cell_preflight,
        .context=cell, .cell_domain=&cell->managed};
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_map_cell_update_exclusive(XrtMapCell *cell, XrtManagedFrame *frame,
    size_t workspace, const XrtBackingTypes *types, const XrtMapUpdate *update, bool *removed) {
    XrtExternalOwner binding = {0};
    XrtManagedStatus status = xrt_map_cell_external(cell, &binding);
    return status == XRT_MANAGED_OK ? xrt_map_update_place(
        (XrtPlace){.external=&binding, .nulls=XRT_PLACE_NONNULL}, frame, workspace, types, update, removed) : status;
}
