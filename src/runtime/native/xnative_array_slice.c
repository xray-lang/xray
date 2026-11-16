/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_array_slice.c - Stable keeper publication for readonly natural windows
 */
#include "xnative_array_slice.h"
#include "xnative_values.h"
#include "xray_native_values.h"
#include <string.h>

static XrtManagedStatus slice_range(const XrtReadonlyArraySlice *slice, XrtBackingArrayKind kind) {
    if (!slice || !slice->backing || (kind != XRT_BACKING_ARRAY_I64 && kind != XRT_BACKING_ARRAY_U8))
        return XRT_MANAGED_ARGUMENT;
    size_t stride = kind == XRT_BACKING_ARRAY_I64 ? sizeof(int64_t) : sizeof(uint8_t);
    const XrtArrayBacking *array = slice->backing;
    if (array->length > array->capacity || array->length > INT64_MAX ||
        array->capacity > (SIZE_MAX - sizeof(*array)) / stride) return XRT_MANAGED_ARGUMENT;
    if (slice->start > array->length || slice->count > array->length - slice->start)
        return XRT_MANAGED_BOUNDS;
    return XRT_MANAGED_OK;
}
XRAY_API XrtInlineValueSpec xrt_array_slice_bundle_spec(void) {
    static const size_t owned[] = {offsetof(XrtArraySliceBundle, hold)};
    return (XrtInlineValueSpec){sizeof(XrtArraySliceBundle), _Alignof(XrtArraySliceBundle), owned, 1};
}
XRAY_API XrtManagedStatus xrt_array_slice_bind(const XrtInlineValueHome *home,
    XrtBackingArrayKind kind, const XrtArrayBacking *array, size_t start, size_t count) {
    XrtReadonlyArraySlice slice = {array, start, count};
    XrtManagedStatus status = slice_range(&slice, kind);
    if (status != XRT_MANAGED_OK) return status;
    XrtArraySliceBundle source = {.value=slice, .hold=(XrtManagedHeader *)&array->managed};
    XrtInlineValueSpec spec = xrt_array_slice_bundle_spec();
    return xrt_inline_value_replace(home, &spec, &source);
}
XRAY_API XrtManagedStatus xrt_array_slice_reborrow(const XrtInlineValueHome *home,
    XrtBackingArrayKind kind, const XrtReadonlyArraySlice *parent, size_t start, size_t count) {
    XrtManagedStatus status = slice_range(parent, kind);
    if (status != XRT_MANAGED_OK) return status;
    if (start > parent->count || count > parent->count - start) return XRT_MANAGED_BOUNDS;
    return xrt_array_slice_bind(home, kind, parent->backing, parent->start + start, count);
}
XRAY_API XrtManagedStatus xrt_array_slice_clear(const XrtInlineValueHome *home) {
    XrtInlineValueSpec spec = xrt_array_slice_bundle_spec();
    return xrt_inline_value_replace(home, &spec, NULL);
}
XRAY_API XrtManagedStatus xrt_array_slice_length(const XrtReadonlyArraySlice *slice, int64_t *output) {
    if (!slice || !slice->backing || !output || slice->count > INT64_MAX) return XRT_MANAGED_ARGUMENT;
    *output = (int64_t)slice->count;
    return XRT_MANAGED_OK;
}
XRAY_API XrtManagedStatus xrt_array_slice_read_bits(const XrtReadonlyArraySlice *slice,
    XrtBackingArrayKind kind, int64_t index, uint64_t *output) {
    if (!output) return XRT_MANAGED_ARGUMENT;
    if (!slice || !slice->backing || (kind != XRT_BACKING_ARRAY_I64 && kind != XRT_BACKING_ARRAY_U8))
        return XRT_MANAGED_ARGUMENT;
#if defined(XR_XIR_PARANOID) && XR_XIR_PARANOID
    XrtManagedStatus status = slice_range(slice, kind);
    if (status != XRT_MANAGED_OK) return status;
#endif
    if (index < 0 || (uint64_t)index >= slice->count) return XRT_MANAGED_BOUNDS;
    size_t position = slice->start + (size_t)index;
    uint64_t bits = 0;
    if (kind == XRT_BACKING_ARRAY_U8) bits = slice->backing->elements[position];
    else memcpy(&bits, slice->backing->elements + position * sizeof(int64_t), sizeof(bits));
    *output = bits;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_array_slice_copy_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, XrtBackingArrayKind kind, const XrtReadonlyArraySlice *slice) {
    XrtManagedStatus status = slice_range(slice, kind);
    if (status != XRT_MANAGED_OK) return status;
    if (kind == XRT_BACKING_ARRAY_U8)
        return xrt_array_backing_u8_create(frame, slot, types, slice->backing->elements + slice->start,
            slice->count, slice->count);
    return xrt_array_backing_i64_create(frame, slot, types,
        (const int64_t *)(const void *)(slice->backing->elements + slice->start * sizeof(int64_t)),
        slice->count, slice->count);
}
XRAY_API void xrt_native_array_slice_copy(XrtManagedFrame *frame, size_t slot,
    XrtBackingArrayKind kind, const XrtReadonlyArraySlice *slice, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = slice_range(slice, kind);
    if (status == XRT_MANAGED_OK)
        status = xrt_native_values_backings(xrt_value_root_current(source_id), &types);
    if (status == XRT_MANAGED_OK) status = xrt_array_slice_copy_create(frame, slot, types, kind, slice);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}
