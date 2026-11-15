/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_value_plan.c - Metered immutable descriptions and inline leaf owners
 *
 * KEY CONCEPT:
 *   Plan dependencies are sealed metadata owners. Payload copying retains all
 *   leaves before publication; physical object retirement stays in the kernel.
 */
#include "xnative_value_plan.h"
#include "xray_native_function.h"
#include "base/xchecks.h"
#include "base/xmax_align.h"
#include <stdalign.h>
#include <string.h>

struct XrtValuePlan {
    _Atomic(int32_t) references;
    XrtBillingOwner *billing;
    size_t allocation_bytes, dependency_count;
    XrtValuePlan **dependencies;
    XrtValuePlanSpec view;
    /* Only a last-release metadata owner uses these continuation fields. */
    XrtValuePlan *retirement_parent;
    size_t retirement_next;
};
_Static_assert(sizeof(XrtValuePlan) % alignof(XrtValueSlotSpec) == 0 &&
    sizeof(XrtValueSlotSpec) % alignof(XrtValueParameterSpec) == 0 &&
    sizeof(XrtValueParameterSpec) % alignof(XrtValuePlan *) == 0,
    "one metadata block preserves all receiving vector alignments");

static bool lease_valid(XrtManagedLease lease) {
    return (lease.retain && lease.drop) || (!lease.context && !lease.retain && !lease.drop);
}
static bool signature_empty(XrtValueSignatureSpec signature) {
    return !signature.parameters && !signature.parameter_count && !signature.result && !signature.flags && !signature.throws;
}
static bool name_valid(const char *name, size_t length) { return (name != NULL) == (length != 0); }
static bool extent_add(size_t *bytes, size_t count, size_t stride) {
    if (count > (SIZE_MAX - *bytes) / stride) return false;
    *bytes += count * stride;
    return true;
}
static XrtManagedStatus plan_extent(const XrtValuePlanSpec *spec, size_t *bytes, size_t *dependencies) {
    *bytes = sizeof(XrtValuePlan);
    *dependencies = 0;
    if (!extent_add(bytes, spec->owned_count, sizeof(size_t)) ||
        !extent_add(bytes, spec->field_count, sizeof(XrtValueSlotSpec)) ||
        !extent_add(bytes, spec->capture_count, sizeof(XrtValueSlotSpec)) ||
        !extent_add(bytes, spec->signature.parameter_count, sizeof(XrtValueParameterSpec)) ||
        !extent_add(dependencies, spec->field_count, 1) || !extent_add(dependencies, spec->capture_count, 1) ||
        !extent_add(dependencies, spec->signature.parameter_count, 1) ||
        !extent_add(dependencies, spec->nullable.element != NULL, 1) ||
        !extent_add(dependencies, spec->signature.result != NULL, 1) ||
        !extent_add(dependencies, spec->entry.signature != NULL, 1) ||
        !extent_add(dependencies, spec->element != NULL, 1) ||
        !extent_add(bytes, *dependencies, sizeof(XrtValuePlan *))) return XRT_MANAGED_OVERFLOW;
    for (size_t i = 0; i < spec->field_count + spec->capture_count; ++i) {
        XrtValueSlotSpec slot = i < spec->field_count ? spec->fields[i] : spec->captures[i - spec->field_count];
        if (!name_valid(slot.name, slot.name_length)) return XRT_MANAGED_ARGUMENT;
        if (!extent_add(bytes, slot.name_length, 1)) return XRT_MANAGED_OVERFLOW;
    }
    for (size_t i = 0; i < spec->signature.parameter_count; ++i) {
        XrtValueParameterSpec parameter = spec->signature.parameters[i];
        if (!name_valid(parameter.name, parameter.name_length)) return XRT_MANAGED_ARGUMENT;
        if (!extent_add(bytes, parameter.name_length, 1)) return XRT_MANAGED_OVERFLOW;
    }
    return XRT_MANAGED_OK;
}
static bool pointer_kind(XrtValueKind kind) {
    return (kind >= XRT_VALUE_STRING && kind <= XRT_VALUE_MAP_STRING_I64) || kind == XRT_VALUE_CELL;
}
static bool scalar_layout(XrtValueKind kind, size_t *size, size_t *alignment) {
    switch (kind) {
        case XRT_VALUE_UNIT: *size = 0; *alignment = 1; return true;
        case XRT_VALUE_BOOL: case XRT_VALUE_I8: case XRT_VALUE_U8: *size = *alignment = 1; return true;
        case XRT_VALUE_I16: case XRT_VALUE_U16: *size = *alignment = 2; return true;
        case XRT_VALUE_I32: case XRT_VALUE_U32: case XRT_VALUE_F32: *size = *alignment = 4; return true;
        case XRT_VALUE_RUNE: case XRT_VALUE_I64: case XRT_VALUE_U64: case XRT_VALUE_F64:
            *size = *alignment = 8; return true;
        default: return false;
    }
}
static bool managed_nullable(const XrtValuePlan *plan) {
    while (plan->view.kind == XRT_VALUE_NULLABLE) plan = plan->view.nullable.element;
    return plan->view.kind == XRT_VALUE_STRING || plan->view.kind == XRT_VALUE_RECORD;
}
static bool record_field(const XrtValuePlan *plan) {
    XrtValueKind kind = plan->view.kind;
    return kind == XRT_VALUE_I64 || kind == XRT_VALUE_BOOL || kind == XRT_VALUE_F64 ||
        kind == XRT_VALUE_STRING || kind == XRT_VALUE_RECORD ||
        (kind == XRT_VALUE_NULLABLE && managed_nullable(plan));
}
static bool capture_type(const XrtValuePlan *plan) {
    XrtValueKind kind = plan->view.kind;
    return kind == XRT_VALUE_BOOL || kind == XRT_VALUE_I64 || kind == XRT_VALUE_U8 ||
        kind == XRT_VALUE_U64 || kind == XRT_VALUE_F64 || kind == XRT_VALUE_FUNCTION ||
        kind == XRT_VALUE_MAP_STRING_I64 || kind == XRT_VALUE_CELL;
}
static bool layout_slots(const XrtValuePlanSpec *spec, const XrtValueSlotSpec *slots, size_t count, bool record) {
    /* Portable C has no empty structure. Its one private padding byte carries
     * no language field and is shared with bytecode's physical home. */
    if (record && !count) return spec->size == 1 && spec->alignment == 1 && !spec->owned_count;
    size_t end = 0, alignment = 1, owned = 0;
    for (size_t i = 0; i < count; ++i) {
        XrtValueSlotSpec field = slots[i];
        if (!field.type || (record ? !record_field(field.type) || !field.name_length : !capture_type(field.type))) return false;
        const XrtValuePlanSpec *type = &field.type->view;
        if (type->alignment > alignment) alignment = type->alignment;
        if (end > SIZE_MAX - (type->alignment - 1)) return false;
        size_t offset = (end + type->alignment - 1) & ~(type->alignment - 1);
        if (field.offset != offset || offset > spec->size || type->size > spec->size - offset) return false;
        end = offset + type->size;
        for (size_t n = 0; n < type->owned_count; ++n) {
            if (owned == spec->owned_count || spec->owned_offsets[owned++] != offset + type->owned_offsets[n]) return false;
        }
    }
    return owned == spec->owned_count && alignment == spec->alignment &&
        end <= SIZE_MAX - (alignment - 1) && spec->size == ((end + alignment - 1) & ~(alignment - 1));
}
static bool signature_valid(const XrtValueSignatureSpec *signature) {
    if (!signature->result || signature->throws || (signature->flags & ~(XRT_VALUE_NO_SUSPEND | XRT_VALUE_C_ABI)) ||
        ((signature->flags & XRT_VALUE_C_ABI) && !(signature->flags & XRT_VALUE_NO_SUSPEND))) return false;
    for (size_t i = 0; i < signature->parameter_count; ++i) {
        XrtValueParameterSpec parameter = signature->parameters[i];
        if (!parameter.type || !xr_param_mode_is_valid(parameter.mode) ||
            (parameter.escaping && parameter.type->view.kind != XRT_VALUE_FUNCTION) ||
            ((parameter.mode == XR_PARAM_REF) != (parameter.type->view.kind == XRT_VALUE_CELL))) return false;
    }
    return true;
}
static bool nullable_valid(const XrtValuePlanSpec *spec) {
    const XrtValuePlan *element = spec->nullable.element;
    if (!element || spec->nullable.tag_offset || spec->nullable.payload_offset != element->view.alignment ||
        spec->alignment != element->view.alignment || element->view.size > SIZE_MAX - spec->nullable.payload_offset)
        return false;
    XrtValueKind kind = element->view.kind;
    size_t scalar_size, scalar_alignment;
    if (!scalar_layout(kind, &scalar_size, &scalar_alignment) && kind != XRT_VALUE_STRING && kind != XRT_VALUE_RECORD &&
        kind != XRT_VALUE_NULLABLE && kind != XRT_VALUE_FIXED_U8 && kind != XRT_VALUE_TUPLE_I64) return false;
    size_t end = spec->nullable.payload_offset + element->view.size;
    if (end > SIZE_MAX - (spec->alignment - 1) ||
        spec->size != ((end + spec->alignment - 1) & ~(spec->alignment - 1)) ||
        spec->owned_count != element->view.owned_count) return false;
    for (size_t i = 0; i < spec->owned_count; ++i)
        if (spec->owned_offsets[i] != spec->nullable.payload_offset + element->view.owned_offsets[i]) return false;
    return true;
}
static bool spec_valid(const XrtValuePlanSpec *spec) {
    if (!spec->alignment || spec->alignment > alignof(XrMaxAlignment) || (spec->alignment & (spec->alignment - 1)) ||
        spec->size % spec->alignment || !lease_valid(spec->code) ||
        (spec->kind != XRT_VALUE_RECORD && spec->field_count) ||
        (spec->kind != XRT_VALUE_ENVIRONMENT && (spec->capture_count || spec->entry.signature || spec->entry.ordinal)) ||
        (spec->kind != XRT_VALUE_FUNCTION && !signature_empty(spec->signature)) ||
        (spec->kind != XRT_VALUE_NULLABLE && (spec->nullable.element || spec->nullable.tag_offset || spec->nullable.payload_offset)) ||
        (spec->kind != XRT_VALUE_CELL && spec->element) ||
        (spec->kind != XRT_VALUE_FIXED_U8 && spec->kind != XRT_VALUE_TUPLE_I64 && spec->count)) return false;
    for (size_t i = 0; i < spec->owned_count; ++i) {
        size_t offset = spec->owned_offsets[i];
        if (spec->alignment < alignof(XrtManagedHeader *) || offset % alignof(XrtManagedHeader *) ||
            offset > spec->size || sizeof(XrtManagedHeader *) > spec->size - offset ||
            (i && offset - spec->owned_offsets[i - 1] < sizeof(XrtManagedHeader *)) ||
            (i && offset < spec->owned_offsets[i - 1])) return false;
    }
    if (pointer_kind(spec->kind))
        return spec->size == sizeof(XrtManagedHeader *) && spec->alignment == alignof(XrtManagedHeader *) &&
            spec->owned_count == 1 && !spec->owned_offsets[0] &&
            (spec->kind != XRT_VALUE_CELL || (spec->element &&
                (spec->element->view.kind == XRT_VALUE_I64 || spec->element->view.kind == XRT_VALUE_MAP_STRING_I64)));
    if (spec->kind == XRT_VALUE_FUNCTION) return spec->size == sizeof(XrtFunction) &&
        spec->alignment == alignof(XrtFunction) && spec->owned_count == 1 &&
        spec->owned_offsets[0] == offsetof(XrtFunction, owner) && signature_valid(&spec->signature);
    if (spec->kind == XRT_VALUE_RECORD) return layout_slots(spec, spec->fields, spec->field_count, true);
    if (spec->kind == XRT_VALUE_NULLABLE) return nullable_valid(spec);
    if (spec->kind == XRT_VALUE_ENVIRONMENT) return spec->entry.signature &&
        spec->entry.signature->view.kind == XRT_VALUE_FUNCTION &&
        (!(spec->entry.signature->view.signature.flags & XRT_VALUE_C_ABI) || !spec->capture_count) &&
        layout_slots(spec, spec->captures, spec->capture_count, false);
    if (spec->owned_count) return false;
    if (spec->kind == XRT_VALUE_FIXED_U8) return spec->count && spec->count <= 65535 &&
        spec->size == spec->count && spec->alignment == 1;
    if (spec->kind == XRT_VALUE_TUPLE_I64) return spec->count && spec->count <= SIZE_MAX / 8 &&
        spec->size == spec->count * 8 && spec->alignment == 8;
    size_t size = 0, alignment = 0;
    return scalar_layout(spec->kind, &size, &alignment) && spec->size == size && spec->alignment == alignment;
}
static char *copy_name(char **next, const char *name, size_t length) {
    if (!length) return NULL;
    char *result = *next;
    memcpy(result, name, length);
    *next += length;
    return result;
}
static bool dependency_take(XrtValuePlan *plan, const XrtValuePlan *dependency) {
    if (!dependency) return true;
    if (!xrt_value_plan_retain((XrtValuePlan *)dependency)) return false;
    plan->dependencies[plan->dependency_count++] = (XrtValuePlan *)dependency;
    return true;
}
static bool copy_dependencies(XrtValuePlan *plan, char *names) {
    XrtValuePlanSpec *view = &plan->view;
    XrtValueSlotSpec *fields = (XrtValueSlotSpec *)view->fields, *captures = (XrtValueSlotSpec *)view->captures;
    for (size_t i = 0; i < view->field_count + view->capture_count; ++i) {
        XrtValueSlotSpec *slot = i < view->field_count ? &fields[i] : &captures[i - view->field_count];
        slot->name = copy_name(&names, slot->name, slot->name_length);
        if (!dependency_take(plan, slot->type)) return false;
    }
    XrtValueParameterSpec *parameters = (XrtValueParameterSpec *)view->signature.parameters;
    for (size_t i = 0; i < view->signature.parameter_count; ++i) {
        parameters[i].name = copy_name(&names, parameters[i].name, parameters[i].name_length);
        if (!dependency_take(plan, parameters[i].type)) return false;
    }
    return dependency_take(plan, view->nullable.element) && dependency_take(plan, view->signature.result) &&
        dependency_take(plan, view->entry.signature) && dependency_take(plan, view->element);
}
XR_FUNCDEF XrtManagedStatus xrt_value_plan_new(XrtBillingOwner *billing, const XrtValuePlanSpec *spec, XrtValuePlan **empty) {
    if (!billing || !spec || !empty || *empty ||
        ((spec->owned_count != 0) != (spec->owned_offsets != NULL)) ||
        ((spec->field_count != 0) != (spec->fields != NULL)) ||
        ((spec->capture_count != 0) != (spec->captures != NULL)) ||
        ((spec->signature.parameter_count != 0) != (spec->signature.parameters != NULL))) return XRT_MANAGED_ARGUMENT;
    size_t bytes = 0, dependencies = 0;
    XrtManagedStatus status = plan_extent(spec, &bytes, &dependencies);
    if (status != XRT_MANAGED_OK) return status;
    if (!spec_valid(spec)) return XRT_MANAGED_ARGUMENT;
    if (!xrt_billing_owner_retain(billing)) return XRT_MANAGED_REFCOUNT;
    if (spec->code.retain && !spec->code.retain(spec->code.context)) {
        xrt_billing_owner_drop(billing);
        return XRT_MANAGED_REFCOUNT;
    }
    void *memory = NULL;
    status = xrt_managed_metadata_allocate(billing, bytes, &memory);
    if (status != XRT_MANAGED_OK) {
        if (spec->code.drop) spec->code.drop(spec->code.context);
        xrt_billing_owner_drop(billing);
        return status;
    }
    XrtValuePlan *plan = memory;
    atomic_init(&plan->references, -1);
    plan->billing = billing;
    plan->allocation_bytes = bytes;
    plan->dependency_count = 0;
    plan->view = *spec;
    plan->retirement_parent = NULL;
    plan->retirement_next = 0;
    unsigned char *next = (unsigned char *)(plan + 1);
    if (spec->owned_count) memcpy(next, spec->owned_offsets, spec->owned_count * sizeof(size_t));
    plan->view.owned_offsets = spec->owned_count ? (size_t *)next : NULL;
    next += spec->owned_count * sizeof(size_t);
    if (spec->field_count) memcpy(next, spec->fields, spec->field_count * sizeof(XrtValueSlotSpec));
    plan->view.fields = spec->field_count ? (XrtValueSlotSpec *)next : NULL;
    next += spec->field_count * sizeof(XrtValueSlotSpec);
    if (spec->capture_count) memcpy(next, spec->captures, spec->capture_count * sizeof(XrtValueSlotSpec));
    plan->view.captures = spec->capture_count ? (XrtValueSlotSpec *)next : NULL;
    next += spec->capture_count * sizeof(XrtValueSlotSpec);
    if (spec->signature.parameter_count) memcpy(next, spec->signature.parameters, spec->signature.parameter_count * sizeof(XrtValueParameterSpec));
    plan->view.signature.parameters = spec->signature.parameter_count ? (XrtValueParameterSpec *)next : NULL;
    next += spec->signature.parameter_count * sizeof(XrtValueParameterSpec);
    plan->dependencies = dependencies ? (XrtValuePlan **)next : NULL;
    next += dependencies * sizeof(XrtValuePlan *);
    if (!copy_dependencies(plan, (char *)next)) { xrt_value_plan_drop(plan); return XRT_MANAGED_REFCOUNT; }
    XR_CHECK(plan->dependency_count == dependencies, "every receiving metadata edge owns exactly one reference");
    *empty = plan;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF bool xrt_value_plan_retain(XrtValuePlan *plan) { return plan && xr_signed_rc_retain(&plan->references); }
static bool plan_last(XrtValuePlan *plan) {
    XrSignedRcRelease result = xr_signed_rc_release(&plan->references);
    XR_CHECK(result != XR_SIGNED_RC_INVALID, "immutable metadata references cannot underflow");
    return result == XR_SIGNED_RC_LAST;
}
XR_FUNCDEF void xrt_value_plan_drop(XrtValuePlan *plan) {
    if (!plan || !plan_last(plan)) return;
    XrtValuePlan *current = plan;
    while (current) {
        if (current->retirement_next < current->dependency_count) {
            XrtValuePlan *child = current->dependencies[current->retirement_next++];
            if (plan_last(child)) {
                child->retirement_parent = current;
                current = child;
            }
        } else {
            XrtValuePlan *parent = current->retirement_parent;
            XrtBillingOwner *billing = current->billing;
            XrtManagedLease code = current->view.code;
            size_t bytes = current->allocation_bytes;
            xrt_managed_metadata_free(billing, current, bytes);
            if (code.drop) code.drop(code.context);
            xrt_billing_owner_drop(billing);
            current = parent;
        }
    }
}
XR_FUNCDEF XrtManagedStatus xrt_value_plan_view(const XrtValuePlan *plan, XrtValuePlanSpec *view) {
    if (!plan || !view) return XRT_MANAGED_ARGUMENT;
    *view = plan->view;
    return XRT_MANAGED_OK;
}
static bool plan_lease_retain(void *plan) { return xrt_value_plan_retain(plan); }
static void plan_lease_drop(void *plan) { xrt_value_plan_drop(plan); }
XR_FUNCDEF XrtManagedLease xrt_value_plan_lease(XrtValuePlan *plan) {
    return plan ? (XrtManagedLease){plan, plan_lease_retain, plan_lease_drop} : (XrtManagedLease){0};
}
static bool payload_valid(const XrtValuePlan *plan, const void *payload) {
    return plan && (!plan->view.size || (payload && (uintptr_t)payload % plan->view.alignment == 0));
}
static XrtManagedHeader *payload_child(const void *payload, size_t offset) {
    XrtManagedHeader *child = NULL;
    memcpy(&child, (const unsigned char *)payload + offset, sizeof(child));
    return child;
}
static void payload_clear(void *payload, size_t offset) {
    XrtManagedHeader *empty = NULL;
    memcpy((unsigned char *)payload + offset, &empty, sizeof(empty));
}
XR_FUNCDEF XrtManagedStatus xrt_value_zero(const XrtValuePlan *plan, void *payload) {
    if (!payload_valid(plan, payload)) return XRT_MANAGED_ARGUMENT;
    if (plan->view.size) memset(payload, 0, plan->view.size);
    for (size_t i = 0; i < plan->view.owned_count; ++i) payload_clear(payload, plan->view.owned_offsets[i]);
    return XRT_MANAGED_OK;
}
static bool transfer_valid(const XrtValuePlan *plan, const void *source, void *empty) {
    if (!payload_valid(plan, source) || !payload_valid(plan, empty)) return false;
    uintptr_t left = (uintptr_t)source, right = (uintptr_t)empty;
    if (plan->view.size && (left < right ? right - left : left - right) < plan->view.size) return false;
    for (size_t i = 0; i < plan->view.owned_count; ++i)
        if (payload_child(empty, plan->view.owned_offsets[i])) return false;
    return true;
}
XR_FUNCDEF XrtManagedStatus xrt_value_copy_empty(const XrtValuePlan *plan, const void *source, void *empty) {
    if (!transfer_valid(plan, source, empty)) return XRT_MANAGED_ARGUMENT;
    size_t retained = 0;
    for (; retained < plan->view.owned_count; ++retained) {
        XrtManagedHeader *child = payload_child(source, plan->view.owned_offsets[retained]);
        if (child && !xrt_managed_retain(child)) break;
    }
    if (retained != plan->view.owned_count) {
        while (retained) xrt_managed_drop(payload_child(source, plan->view.owned_offsets[--retained]));
        XR_CHECK(xrt_value_zero(plan, empty) == XRT_MANAGED_OK, "a failed copy leaves a canonical empty receiving payload");
        return XRT_MANAGED_REFCOUNT;
    }
    if (plan->view.size) memcpy(empty, source, plan->view.size);
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_value_move_empty(const XrtValuePlan *plan, void *source, void *empty) {
    if (!transfer_valid(plan, source, empty)) return XRT_MANAGED_ARGUMENT;
    if (plan->view.size) memcpy(empty, source, plan->view.size);
    return xrt_value_zero(plan, source);
}
XR_FUNCDEF void xrt_value_drop(const XrtValuePlan *plan, void *payload) {
    XR_CHECK(payload_valid(plan, payload), "inline retirement borrows a live plan and matching payload");
    for (size_t i = 0; i < plan->view.owned_count; ++i) {
        size_t offset = plan->view.owned_offsets[i];
        XrtManagedHeader *child = payload_child(payload, offset);
        payload_clear(payload, offset);
        xrt_managed_drop(child);
    }
    XR_CHECK(xrt_value_zero(plan, payload) == XRT_MANAGED_OK, "retired inline values have no hidden owners");
}
static bool inline_spec_valid(const XrtInlineValueSpec *spec) {
    if (!spec || !spec->alignment || (spec->alignment & (spec->alignment - 1)) ||
        (spec->owned_count && !spec->owned_offsets)) return false;
    size_t previous = 0;
    for (size_t i = 0; i < spec->owned_count; ++i) {
        size_t offset = spec->owned_offsets[i];
        if (offset < previous || offset % _Alignof(XrtManagedHeader *) || offset > spec->size ||
            sizeof(XrtManagedHeader *) > spec->size - offset) return false;
        previous = offset + sizeof(XrtManagedHeader *);
    }
    return true;
}
static bool inline_overlaps(const void *left, const void *right, size_t size) {
    uintptr_t a = (uintptr_t)left, b = (uintptr_t)right;
    return size && (a < b ? b - a : a - b) < size;
}
static bool inline_within(const void *payload, size_t extent, const void *value, size_t size) {
    uintptr_t begin = (uintptr_t)payload, address = (uintptr_t)value;
    return address >= begin && address - begin <= extent && size <= extent - (address - begin);
}
static void inline_nonowners(const XrtInlineValueSpec *spec, void *to, const void *from) {
    size_t previous = 0;
    for (size_t i = 0; i <= spec->owned_count; ++i) {
        size_t end = i == spec->owned_count ? spec->size : spec->owned_offsets[i];
        if (end > previous) {
            if (from) memcpy((unsigned char *)to + previous, (const unsigned char *)from + previous, end - previous);
            else memset((unsigned char *)to + previous, 0, end - previous);
        }
        if (i < spec->owned_count) previous = end + sizeof(XrtManagedHeader *);
    }
}
static XrtManagedStatus inline_home_check(const XrtInlineValueHome *home, const XrtInlineValueSpec *spec, bool *domain) {
    void *payload = NULL; size_t extent = 0;
    XrtManagedStatus status = xrt_managed_frame_payload(home->frame, &payload, &extent);
    if (status != XRT_MANAGED_OK || !inline_within(payload, extent, home->value, spec->size) ||
        !inline_within(payload, extent, home->temporary, spec->size) ||
        home->first_owner > SIZE_MAX - spec->owned_count || home->temporary_first_owner > SIZE_MAX - spec->owned_count)
        return XRT_MANAGED_ARGUMENT;
    for (size_t i = 0; i < spec->owned_count; ++i) {
        XrtManagedSlotLocation value = {0}, temporary = {0};
        status = xrt_managed_slot_location(home->frame, home->first_owner + i, &value);
        if (status == XRT_MANAGED_OK)
            status = xrt_managed_slot_location(home->frame, home->temporary_first_owner + i, &temporary);
        if (status != XRT_MANAGED_OK || value.busy != domain || temporary.busy != domain ||
            (const void *)value.owner != (const unsigned char *)home->value + spec->owned_offsets[i] ||
            (const void *)temporary.owner != (const unsigned char *)home->temporary + spec->owned_offsets[i] ||
            !value.pending || !temporary.pending || value.pending->value || value.pending->allocation_bytes ||
            temporary.pending->value || temporary.pending->allocation_bytes || *temporary.owner)
            return XRT_MANAGED_ARGUMENT;
    }
    return XRT_MANAGED_OK;
}
XRAY_API XrtManagedStatus xrt_inline_value_replace(const XrtInlineValueHome *home,
    const XrtInlineValueSpec *spec, const void *source) {
    if (!home || !inline_spec_valid(spec) || (spec->size && (!home->value ||
        (uintptr_t)home->value % spec->alignment))) return XRT_MANAGED_ARGUMENT;
    if (!spec->owned_count) {
        if (spec->size) {
            if (source) memmove(home->value, source, spec->size);
            else memset(home->value, 0, spec->size);
        }
        return XRT_MANAGED_OK;
    }
    if (!home->temporary || (uintptr_t)home->temporary % spec->alignment ||
        inline_overlaps(home->value, home->temporary, spec->size) ||
        (source && inline_overlaps(source, home->temporary, spec->size))) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlotLocation first = {0};
    XrtManagedStatus status = xrt_managed_slot_location(home->frame, home->first_owner, &first);
    if (status != XRT_MANAGED_OK || !first.busy || *first.busy) return XRT_MANAGED_ARGUMENT;
    *first.busy = true;
    status = inline_home_check(home, spec, first.busy);
    size_t retained = 0;
    if (status == XRT_MANAGED_OK) {
        inline_nonowners(spec, home->temporary, source);
        for (; retained < spec->owned_count; ++retained) {
            size_t offset = spec->owned_offsets[retained];
            XrtManagedHeader *child = source ? payload_child(source, offset) : NULL;
            if (child && !xrt_managed_retain(child)) { status = XRT_MANAGED_REFCOUNT; break; }
            memcpy((unsigned char *)home->temporary + offset, &child, sizeof(child));
        }
        if (status == XRT_MANAGED_OK) {
            for (size_t i = 0; i < spec->owned_count; ++i) {
                size_t offset = spec->owned_offsets[i];
                XrtManagedHeader *next = payload_child(home->temporary, offset), *old = payload_child(home->value, offset);
                memcpy((unsigned char *)home->value + offset, &next, sizeof(next));
                memcpy((unsigned char *)home->temporary + offset, &old, sizeof(old));
            }
            inline_nonowners(spec, home->value, home->temporary);
        }
        inline_nonowners(spec, home->temporary, NULL);
        for (size_t i = 0; i < retained; ++i) {
            size_t offset = spec->owned_offsets[i];
            XrtManagedHeader *discard = payload_child(home->temporary, offset);
            payload_clear(home->temporary, offset);
            xrt_managed_drop(discard);
        }
    }
    *first.busy = false;
    return status;
}
