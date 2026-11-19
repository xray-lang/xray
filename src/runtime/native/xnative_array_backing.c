/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_array_backing.c - Typed scalar and inline rows in one Array backing
 *
 * KEY CONCEPT:
 *   Failed construction leaves its installed owners in the pending allocation
 *   for the common capacity-based iterative walker to retire.
 */
#include "xnative_array_backing.h"
#include "base/xchecks.h"
#include <stdalign.h>
#include <string.h>

typedef struct ArrayInput {
    XrtBackingArrayKind kind;
    const void *values;
    const XrtArrayBacking *copy;
    size_t count, capacity;
} ArrayInput;

static XrtManagedHeader *array_child(const XrtArrayBacking *array, size_t index) {
    XrtManagedHeader *child = NULL;
    memcpy(&child, array->elements + index * sizeof(child), sizeof(child));
    return child;
}
static XrtManagedStatus array_fill(XrtArrayBacking *array, ArrayInput input, size_t stride) {
    if (input.kind != XRT_BACKING_ARRAY_STRING) {
        const void *bytes = input.copy ? input.copy->elements : input.values;
        if (input.count) memmove(array->elements, bytes, input.count * stride);
    } else {
        XrtStringBacking *const *strings = input.values;
        for (size_t i = 0; i < input.count; ++i) {
            XrtManagedHeader *child = input.copy ? array_child(input.copy, i) : &strings[i]->managed;
            if (!xrt_managed_retain(child)) return XRT_MANAGED_REFCOUNT;
            memcpy(array->elements + i * stride, &child, sizeof(child));
        }
    }
    return XRT_MANAGED_OK;
}
static size_t array_bytes(size_t stride, size_t capacity) {
    return capacity <= (SIZE_MAX - sizeof(XrtArrayBacking)) / stride
        ? sizeof(XrtArrayBacking) + capacity * stride : 0;
}
static XrtManagedStatus array_prepare(XrtManagedFrame *frame, size_t slot,
    XrtManagedOps *ops, size_t bytes, size_t capacity, XrtManagedHeader **payload) {
    XrtManagedStatus status = xrt_managed_slot_prepare(frame, slot, ops, bytes, payload);
    XR_CHECK(status != XRT_MANAGED_OK || ((const XrtArrayBacking *)*payload)->capacity == capacity,
        "Array capacity comes from its actual allocated stride");
    return status;
}
static XrtManagedStatus array_finish(XrtManagedFrame *frame, size_t slot, XrtArrayBacking *array,
    size_t count, XrtManagedStatus status) {
    if (status == XRT_MANAGED_OK) {
        array->length = count;
        status = xrt_managed_slot_commit(frame, slot);
    }
    if (status != XRT_MANAGED_OK)
        XR_CHECK(xrt_managed_slot_abort(frame, slot) == XRT_MANAGED_OK, "failed Array construction retires every pending tail owner");
    return status;
}
static XrtManagedStatus array_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, ArrayInput input) {
    size_t stride = xrt_backing_array_stride(input.kind);
    if (!types || !stride || !types->arrays[input.kind] || input.count > input.capacity ||
        (!input.copy && input.count && !input.values)) return XRT_MANAGED_ARGUMENT;
    size_t bytes = array_bytes(stride, input.capacity);
    if (!bytes) return XRT_MANAGED_OVERFLOW;
    if (input.kind == XRT_BACKING_ARRAY_STRING && !input.copy) {
        XrtStringBacking *const *strings = input.values;
        for (size_t i = 0; i < input.count; ++i)
            if (!strings[i]) return XRT_MANAGED_ARGUMENT;
    }
    XrtManagedHeader *payload = NULL;
    XrtManagedStatus status = array_prepare(frame, slot, types->arrays[input.kind], bytes, input.capacity, &payload);
    if (status != XRT_MANAGED_OK) return status;
    XrtArrayBacking *array = (XrtArrayBacking *)payload;
    return array_finish(frame, slot, array, input.count, array_fill(array, input, stride));
}
XR_FUNCDEF XrtManagedStatus xrt_array_backing_i64_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const int64_t *values, size_t count, size_t capacity) {
    return array_create(frame, slot, types, (ArrayInput){XRT_BACKING_ARRAY_I64, values, NULL, count, capacity});
}
XR_FUNCDEF XrtManagedStatus xrt_array_backing_string_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, XrtStringBacking *const *values, size_t count, size_t capacity) {
    return array_create(frame, slot, types, (ArrayInput){XRT_BACKING_ARRAY_STRING, values, NULL, count, capacity});
}
XR_FUNCDEF XrtManagedStatus xrt_array_backing_u8_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const uint8_t *values, size_t count, size_t capacity) {
    return array_create(frame, slot, types, (ArrayInput){XRT_BACKING_ARRAY_U8, values, NULL, count, capacity});
}
XR_FUNCDEF XrtManagedStatus xrt_array_backing_u64_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const uint64_t *values, size_t count, size_t capacity) {
    return array_create(frame, slot, types, (ArrayInput){XRT_BACKING_ARRAY_U64, values, NULL, count, capacity});
}

/* These callbacks identify the Array producer, even when another physical
 * object independently owns a lease on the same inline plan. */
static bool array_element_retain(void *element) { return xrt_value_plan_retain(element); }
static void array_element_drop(void *element) { xrt_value_plan_drop(element); }

XR_FUNCDEF XrtManagedStatus xrt_array_backing_inline_bind(XrtBillingOwner *billing,
    const XrtValuePlan *element, XrtManagedOps **empty) {
    XrtValuePlanSpec shape;
    if (!billing || !empty || *empty || xrt_value_plan_view(element, &shape) != XRT_MANAGED_OK ||
        !xrt_value_kind_has_fields(shape.kind) || !shape.size || sizeof(XrtArrayBacking) % shape.alignment)
        return XRT_MANAGED_ARGUMENT;
    size_t alignment = shape.alignment > alignof(XrtArrayBacking) ? shape.alignment : alignof(XrtArrayBacking);
    XrtRuntimeTypeSpec spec = {
        .fixed_size = sizeof(XrtArrayBacking), .alignment = alignment,
        .capacity_offset = offsetof(XrtArrayBacking, capacity), .stride = shape.size,
        .code = {(void *)element, array_element_retain, array_element_drop},
        .tail_child_offsets = shape.owned_offsets, .tail_child_count = shape.owned_count
    };
    XrtRuntimeTypePlan *plan = NULL;
    XrtManagedStatus status = xrt_runtime_type_plan_new(billing, &spec, &plan);
    if (status == XRT_MANAGED_OK) status = xrt_managed_ops_bind(billing, plan, empty);
    xrt_runtime_type_plan_drop(plan);
    return status;
}

XR_FUNCDEF XrtManagedStatus xrt_array_backing_inline_create(XrtManagedFrame *frame, size_t slot,
    XrtManagedOps *ops, const void *rows, size_t count, size_t capacity) {
    const XrtRuntimeTypeSpec *spec = NULL;
    XrtManagedStatus status = xrt_managed_ops_producer_borrow(ops, array_element_retain, array_element_drop, &spec);
    if (status != XRT_MANAGED_OK) return status;
    XrtValuePlanSpec element;
    status = xrt_value_plan_view(spec->code.context, &element);
    XR_CHECK(status == XRT_MANAGED_OK, "Array Ops keeps the bound element plan alive");
    if (count > capacity || (count && (!rows || (uintptr_t)rows % element.alignment))) return XRT_MANAGED_ARGUMENT;
    size_t bytes = array_bytes(spec->stride, capacity);
    if (!bytes) return XRT_MANAGED_OVERFLOW;
    XrtManagedHeader *payload = NULL;
    status = array_prepare(frame, slot, ops, bytes, capacity, &payload);
    if (status != XRT_MANAGED_OK) return status;
    XrtArrayBacking *array = (XrtArrayBacking *)payload;
    for (size_t row = 0; row < count && status == XRT_MANAGED_OK; ++row) {
        size_t offset = row * spec->stride;
        status = xrt_value_copy_empty(spec->code.context, (const unsigned char *)rows + offset, array->elements + offset);
    }
    return array_finish(frame, slot, array, count, status);
}

static const XrtRuntimeTypeSpec *array_inline_spec(const XrtManagedHeader *owner, const XrtValuePlan *element) {
    const XrtRuntimeTypeSpec *spec = NULL;
    if (!element || xrt_managed_producer_borrow(owner, array_element_retain, array_element_drop, &spec) != XRT_MANAGED_OK ||
        spec->code.context != element) return NULL;
    return spec;
}
XR_FUNCDEF bool xrt_array_backing_inline_matches(const XrtManagedHeader *owner, const XrtValuePlan *element) {
    return array_inline_spec(owner, element) != NULL;
}
XR_FUNCDEF XrtManagedStatus xrt_array_backing_inline_at(const XrtArrayBacking *array,
    const XrtValuePlan *element, int64_t index, const void **empty_borrow) {
    if (!array || !empty_borrow || *empty_borrow) return XRT_MANAGED_ARGUMENT;
    const XrtRuntimeTypeSpec *spec = array_inline_spec(&array->managed, element);
    if (!spec || array->length > array->capacity) return XRT_MANAGED_ARGUMENT;
    if (index < 0 || (uint64_t)index >= array->length) return XRT_MANAGED_BOUNDS;
    *empty_borrow = array->elements + (size_t)index * spec->stride;
    return XRT_MANAGED_OK;
}

XR_FUNCDEF XrtManagedStatus xrt_array_backing_copy_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, XrtBackingArrayKind kind, const XrtArrayBacking *source, size_t capacity) {
    if (!source || source->length > source->capacity) return XRT_MANAGED_ARGUMENT;
    return array_create(frame, slot, types, (ArrayInput){kind, NULL, source, source->length, capacity});
}
XR_FUNCDEF XrtManagedStatus xrt_array_backing_read_bits(const XrtArrayBacking *array,
    XrtBackingArrayKind kind, size_t index, uint64_t *output) {
    size_t stride = xrt_backing_array_stride(kind);
    if (!array || !output || !stride || kind == XRT_BACKING_ARRAY_STRING ||
        array->length > array->capacity || index >= array->length) return XRT_MANAGED_ARGUMENT;
    uint64_t bits = 0;
    if (kind == XRT_BACKING_ARRAY_U8) bits = array->elements[index];
    else memcpy(&bits, array->elements + index * stride, sizeof(bits));
    *output = bits;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_array_backing_string_at(const XrtArrayBacking *array,
    size_t index, const XrtStringBacking **output) {
    if (!array || !output || *output || array->length > array->capacity || index >= array->length)
        return XRT_MANAGED_ARGUMENT;
    XrtManagedHeader *child = array_child(array, index);
    if (!child) return XRT_MANAGED_ARGUMENT;
    *output = (const XrtStringBacking *)child;
    return XRT_MANAGED_OK;
}
