/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_array_backing.c - Typed bits and real String owners in fixed tail slots
 *
 * KEY CONCEPT:
 *   Failed construction leaves its installed owners in the pending allocation
 *   for the common capacity-based iterative walker to retire.
 */
#include "xnative_array_backing.h"
#include "base/xchecks.h"
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
    array->length = input.count;
    return XRT_MANAGED_OK;
}
static XrtManagedStatus array_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, ArrayInput input) {
    size_t stride = xrt_backing_array_stride(input.kind);
    if (!types || !stride || !types->arrays[input.kind] || input.count > input.capacity ||
        (!input.copy && input.count && !input.values)) return XRT_MANAGED_ARGUMENT;
    if (input.capacity > (SIZE_MAX - sizeof(XrtArrayBacking)) / stride) return XRT_MANAGED_OVERFLOW;
    if (input.kind == XRT_BACKING_ARRAY_STRING && !input.copy) {
        XrtStringBacking *const *strings = input.values;
        for (size_t i = 0; i < input.count; ++i)
            if (!strings[i]) return XRT_MANAGED_ARGUMENT;
    }
    XrtManagedHeader *payload = NULL;
    XrtManagedStatus status = xrt_managed_slot_prepare(frame, slot, types->arrays[input.kind],
        sizeof(XrtArrayBacking) + input.capacity * stride, &payload);
    if (status != XRT_MANAGED_OK) return status;
    XrtArrayBacking *array = (XrtArrayBacking *)payload;
    XR_CHECK(array->capacity == input.capacity, "Array capacity comes from its actual allocated stride");
    status = array_fill(array, input, stride);
    if (status == XRT_MANAGED_OK) status = xrt_managed_slot_commit(frame, slot);
    if (status != XRT_MANAGED_OK)
        XR_CHECK(xrt_managed_slot_abort(frame, slot) == XRT_MANAGED_OK, "failed Array construction retires every pending tail owner");
    return status;
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
