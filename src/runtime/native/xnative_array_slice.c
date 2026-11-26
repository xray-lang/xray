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
#include "xnative_host_nullable.h"
#include "xnative_managed_root.h"
#include "base/xchecks.h"
#include "xray_native_values.h"
#include <string.h>

static XrtManagedStatus slice_span(const XrtReadonlyWindow *slice, XrtBackingArrayKind kind,
    const unsigned char **output) {
    if (!slice || !slice->owner || (kind != XRT_BACKING_ARRAY_I64 && kind != XRT_BACKING_ARRAY_U8))
        return XRT_MANAGED_ARGUMENT;
    size_t stride = xrt_backing_array_stride(kind), length;
    const unsigned char *data;
    if (kind == XRT_BACKING_ARRAY_U8 && xrt_backing_string_matches(slice->owner)) {
        const XrtStringBacking *string = (const XrtStringBacking *)slice->owner;
        data = (const unsigned char *)string->bytes;
        length = string->length;
    } else {
        if (!xrt_backing_array_matches(slice->owner, kind)) return XRT_MANAGED_ARGUMENT;
        const XrtArrayBacking *array = (const XrtArrayBacking *)slice->owner;
        if (array->length > array->capacity ||
            array->capacity > (SIZE_MAX - sizeof(*array)) / stride) return XRT_MANAGED_ARGUMENT;
        data = array->elements;
        length = array->length;
    }
    if (length > INT64_MAX) return XRT_MANAGED_ARGUMENT;
    if (slice->start > length || slice->count > length - slice->start)
        return XRT_MANAGED_BOUNDS;
    if (output) *output = data + slice->start * stride;
    return XRT_MANAGED_OK;
}

XRAY_API XrtInlineValueSpec xrt_array_slice_bundle_spec(void) {
    static const size_t owned[] = {offsetof(XrtArraySliceBundle, hold)};
    return (XrtInlineValueSpec){sizeof(XrtArraySliceBundle), _Alignof(XrtArraySliceBundle), owned, 1};
}
static XrtManagedStatus slice_publish(const XrtInlineValueHome *home, const void *bundle) {
    XrtInlineValueSpec spec = xrt_array_slice_bundle_spec();
    return xrt_inline_value_replace(home, &spec, bundle);
}
static XrtManagedStatus window_publish(const XrtInlineValueHome *home, XrtReadonlyWindow window) {
    const XrtReadonlyWindowBundle bundle = {.value=window, .hold=(XrtManagedHeader *)window.owner};
    return slice_publish(home, &bundle);
}
static XrtManagedStatus array_publish(const XrtInlineValueHome *home, XrtReadonlyWindow window) {
    /* Only the Array API constructs this installed Array-specific C value. */
    const XrtArraySliceBundle bundle = {
        .value={(const XrtArrayBacking *)window.owner, window.start, window.count},
        .hold=(XrtManagedHeader *)window.owner};
    return slice_publish(home, &bundle);
}
static XrtReadonlyWindow array_window(const XrtReadonlyArraySlice *slice) {
    return slice ? (XrtReadonlyWindow){(const XrtManagedHeader *)slice->backing, slice->start, slice->count} :
        (XrtReadonlyWindow){0};
}
static XrtManagedStatus slice_subwindow(const XrtReadonlyWindow *parent, size_t start, size_t count,
    XrtReadonlyWindow *output) {
    if (start > parent->count || count > parent->count - start) return XRT_MANAGED_BOUNDS;
    *output = (XrtReadonlyWindow){parent->owner, parent->start + start, count};
    return XRT_MANAGED_OK;
}
static XrtManagedStatus slice_range_reborrow(const XrtReadonlyWindow *parent, XrtBackingArrayKind kind,
    int64_t start, int64_t limit, bool open, XrtReadonlyWindow *output) {
    XrtManagedStatus status = slice_span(parent, kind, NULL);
    if (status != XRT_MANAGED_OK) return status;
    if (open) limit = (int64_t)parent->count;
    if (start < 0 || limit < start) return XRT_MANAGED_BOUNDS;
    return slice_subwindow(parent, (size_t)start, (size_t)(limit - start), output);
}

XRAY_API XrtManagedStatus xrt_string_bytes_bind(const XrtInlineValueHome *home, const XrtManagedHeader *owner) {
    if (!xrt_backing_string_matches(owner)) return XRT_MANAGED_ARGUMENT;
    const XrtStringBacking *string = (const XrtStringBacking *)owner;
    if (string->length > INT64_MAX) return XRT_MANAGED_ARGUMENT;
    return window_publish(home, (XrtReadonlyWindow){owner, 0, string->length});
}
XRAY_API XrtManagedStatus xrt_window_range_bind(const XrtInlineValueHome *home,
    XrtBackingArrayKind kind, const XrtManagedHeader *array, int64_t start, int64_t limit) {
    if (start < 0 || limit < start) return XRT_MANAGED_BOUNDS;
    if (!xrt_backing_array_matches(array, kind)) return XRT_MANAGED_ARGUMENT;
    const XrtReadonlyWindow window = {array, (size_t)start, (size_t)(limit - start)};
    return xrt_window_rebind(home, kind, &window);
}
XRAY_API XrtManagedStatus xrt_window_rebind(const XrtInlineValueHome *home,
    XrtBackingArrayKind kind, const XrtReadonlyWindow *source) {
    XrtManagedStatus status = slice_span(source, kind, NULL);
    return status == XRT_MANAGED_OK ? window_publish(home, *source) : status;
}
XRAY_API XrtManagedStatus xrt_window_range_reborrow(const XrtInlineValueHome *home,
    XrtBackingArrayKind kind, const XrtReadonlyWindow *parent, int64_t start, int64_t limit, bool open) {
    XrtReadonlyWindow window = {0};
    XrtManagedStatus status = slice_range_reborrow(parent, kind, start, limit, open, &window);
    return status == XRT_MANAGED_OK ? window_publish(home, window) : status;
}
XRAY_API XrtManagedStatus xrt_window_length(const XrtReadonlyWindow *slice, int64_t *output) {
    if (!slice || !slice->owner || !output || slice->count > INT64_MAX) return XRT_MANAGED_ARGUMENT;
    *output = (int64_t)slice->count;
    return XRT_MANAGED_OK;
}
XRAY_API XrtManagedStatus xrt_window_read_bits(const XrtReadonlyWindow *slice,
    XrtBackingArrayKind kind, int64_t index, uint64_t *output) {
    if (!output) return XRT_MANAGED_ARGUMENT;
    const unsigned char *data = NULL;
    XrtManagedStatus status = slice_span(slice, kind, &data);
    if (status != XRT_MANAGED_OK) return status;
    if (index < 0 || (uint64_t)index >= slice->count) return XRT_MANAGED_BOUNDS;
    size_t position = (size_t)index;
    uint64_t bits = 0;
    if (kind == XRT_BACKING_ARRAY_U8) bits = data[position];
    else memcpy(&bits, data + position * sizeof(int64_t), sizeof(bits));
    *output = bits;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_window_copy_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, XrtBackingArrayKind kind, const XrtReadonlyWindow *slice) {
    const unsigned char *data = NULL;
    XrtManagedStatus status = slice_span(slice, kind, &data);
    if (status != XRT_MANAGED_OK) return status;
    if (kind == XRT_BACKING_ARRAY_U8)
        return xrt_array_backing_u8_create(frame, slot, types, data, slice->count, slice->count);
    return xrt_array_backing_i64_create(frame, slot, types,
        (const int64_t *)(const void *)data, slice->count, slice->count);
}
XRAY_API void xrt_native_window_copy(XrtManagedFrame *frame, size_t slot,
    XrtBackingArrayKind kind, const XrtReadonlyWindow *slice, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = slice_span(slice, kind, NULL);
    if (status == XRT_MANAGED_OK)
        status = xrt_native_values_backings(xrt_value_root_current(source_id), &types);
    if (status == XRT_MANAGED_OK) status = xrt_window_copy_create(frame, slot, types, kind, slice);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}
XRAY_API XrtManagedStatus xrt_window_strict_utf8(XrtManagedFrame *frame, size_t destination,
    size_t temporary, size_t offset, size_t temporary_owner_offset,
    const XrtReadonlyWindow *bytes, uint32_t source) {
    const unsigned char *data = NULL;
    XrtManagedStatus status = slice_span(bytes, XRT_BACKING_ARRAY_U8, &data);
    if (status != XRT_MANAGED_OK) return status;

    const XrtHostNullableSlots output = {frame, destination, temporary, offset, temporary_owner_offset};
    XrtHostNullablePlaces places;
    status = xrt_host_nullable_prepare(&output, xrt_native_managed_root(source), &places);
    if (status != XRT_MANAGED_OK) return status;
    if (places.current && !xrt_backing_string_matches(places.current)) return XRT_MANAGED_ARGUMENT;

    const XrtBackingTypes *types = NULL;
    status = xrt_native_values_backings(xrt_value_root_current(source), &types);
    if (status != XRT_MANAGED_OK) return status;
    status = xrt_string_backing_create(places.temporary.frame, places.temporary.slot, types,
        (const char *)data, bytes->count, 0);
    if (status != XRT_MANAGED_OK && status != XRT_MANAGED_BAD_UTF8) return status;
    XR_CHECK(xrt_host_nullable_publish(&places, status == XRT_MANAGED_OK) == XRT_MANAGED_OK,
        "strict decoding keeps its preflighted nullable home");
    return XRT_MANAGED_OK;
}

XRAY_API XrtManagedStatus xrt_window_lossy_utf8(XrtManagedFrame *frame, size_t destination,
    const XrtReadonlyWindow *bytes, uint32_t source) {
    const unsigned char *data = NULL;
    XrtManagedStatus status = slice_span(bytes, XRT_BACKING_ARRAY_U8, &data);
    if (status != XRT_MANAGED_OK) return status;

    XrtValueRoot *execution = xrt_value_root_current(source);
    XrtManagedRoot *root = xrt_native_values_existing_root(execution);
    if (!root || xrt_managed_frame_root(frame) != root) return XRT_MANAGED_ARGUMENT;
    XrtManagedHeader *previous = NULL;
    status = xrt_managed_slot_borrow(frame, destination, &previous);
    if (status != XRT_MANAGED_OK) return status;
    if (previous && !xrt_backing_string_matches(previous)) return XRT_MANAGED_ARGUMENT;

    const XrtBackingTypes *types = NULL;
    status = xrt_native_values_backings(execution, &types);
    if (status != XRT_MANAGED_OK) return status;
    return xrt_string_backing_lossy_create(frame, destination, types, data, bytes->count);
}

XRAY_API XrtManagedStatus xrt_array_slice_bind(const XrtInlineValueHome *home,
    XrtBackingArrayKind kind, const XrtArrayBacking *array, size_t start, size_t count) {
    const XrtReadonlyWindow window = {(const XrtManagedHeader *)array, start, count};
    if (!xrt_backing_array_matches(window.owner, kind)) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = slice_span(&window, kind, NULL);
    return status == XRT_MANAGED_OK ? array_publish(home, window) : status;
}
XRAY_API XrtManagedStatus xrt_array_slice_range_bind(const XrtInlineValueHome *home,
    XrtBackingArrayKind kind, const XrtArrayBacking *array, int64_t start, int64_t limit) {
    if (start < 0 || limit < start) return XRT_MANAGED_BOUNDS;
    return xrt_array_slice_bind(home, kind, array, (size_t)start, (size_t)(limit - start));
}
XRAY_API XrtManagedStatus xrt_array_slice_reborrow(const XrtInlineValueHome *home,
    XrtBackingArrayKind kind, const XrtReadonlyArraySlice *parent, size_t start, size_t count) {
    const XrtReadonlyWindow source = array_window(parent);
    if (!xrt_backing_array_matches(source.owner, kind)) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = slice_span(&source, kind, NULL);
    XrtReadonlyWindow window = {0};
    if (status == XRT_MANAGED_OK) status = slice_subwindow(&source, start, count, &window);
    return status == XRT_MANAGED_OK ? array_publish(home, window) : status;
}
XRAY_API XrtManagedStatus xrt_array_slice_range_reborrow(const XrtInlineValueHome *home,
    XrtBackingArrayKind kind, const XrtReadonlyArraySlice *parent, int64_t start, int64_t limit, bool open) {
    const XrtReadonlyWindow source = array_window(parent);
    if (!xrt_backing_array_matches(source.owner, kind)) return XRT_MANAGED_ARGUMENT;
    XrtReadonlyWindow window = {0};
    XrtManagedStatus status = slice_range_reborrow(&source, kind, start, limit, open, &window);
    return status == XRT_MANAGED_OK ? array_publish(home, window) : status;
}
XRAY_API XrtManagedStatus xrt_array_slice_clear(XrtManagedFrame *frame, void *value, void *temporary,
    size_t first_owner, size_t temporary_first_owner) {
    const XrtInlineValueHome home = {frame, value, temporary, first_owner, temporary_first_owner};
    return slice_publish(&home, NULL);
}
XRAY_API XrtManagedStatus xrt_array_slice_length(const XrtReadonlyArraySlice *slice, int64_t *output) {
    const XrtReadonlyWindow window = array_window(slice);
    return xrt_window_length(&window, output);
}
XRAY_API XrtManagedStatus xrt_array_slice_read_bits(const XrtReadonlyArraySlice *slice,
    XrtBackingArrayKind kind, int64_t index, uint64_t *output) {
    const XrtReadonlyWindow window = array_window(slice);
    if (!xrt_backing_array_matches(window.owner, kind)) return XRT_MANAGED_ARGUMENT;
    return xrt_window_read_bits(&window, kind, index, output);
}
XR_FUNCDEF XrtManagedStatus xrt_array_slice_copy_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, XrtBackingArrayKind kind, const XrtReadonlyArraySlice *slice) {
    const XrtReadonlyWindow window = array_window(slice);
    if (!xrt_backing_array_matches(window.owner, kind)) return XRT_MANAGED_ARGUMENT;
    return xrt_window_copy_create(frame, slot, types, kind, &window);
}
XRAY_API void xrt_native_array_slice_copy(XrtManagedFrame *frame, size_t slot,
    XrtBackingArrayKind kind, const XrtReadonlyArraySlice *slice, uint32_t source_id) {
    const XrtReadonlyWindow window = array_window(slice);
    if (!xrt_backing_array_matches(window.owner, kind)) xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source_id);
    xrt_native_window_copy(frame, slot, kind, &window, source_id);
}
XRAY_API XrtManagedStatus xrt_array_slice_strict_utf8(XrtManagedFrame *frame, size_t destination,
    size_t temporary, size_t offset, size_t temporary_owner_offset,
    const XrtReadonlyArraySlice *bytes, uint32_t source) {
    const XrtReadonlyWindow window = array_window(bytes);
    if (!xrt_backing_array_matches(window.owner, XRT_BACKING_ARRAY_U8)) return XRT_MANAGED_ARGUMENT;
    return xrt_window_strict_utf8(frame, destination, temporary, offset, temporary_owner_offset, &window, source);
}
