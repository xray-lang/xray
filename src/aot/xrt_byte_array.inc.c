/* AOT Array capacity and byte-array helpers. */

#include "../shared/xr_raw_scalar_core.h"

static inline XrValue xrt_array_new_typed_exact(int64_t cap, uint8_t etype) {
    if (cap < 0)
        cap = 0;
    xrt_array_t *a = xrt_array_alloc_inline(cap, etype, 1, "xrt_array_new_typed_exact");
    return xr_mkptr(a, XR_TAG_ARRAY);
}

static inline void xrt_array_reserve_raw(xrt_array_t *a, int64_t cap) {
    if (!a || a->data_storage == XR_ARRAY_DATA_BORROWED || cap <= a->capacity)
        return;
    size_t old_bytes = (size_t) a->capacity * (size_t) a->elem_size;
    xrt_array_data_grow(a, cap);
    memset((uint8_t *) a->data + old_bytes, 0, (size_t) cap * (size_t) a->elem_size - old_bytes);
}

static inline XrValue xrt_array_with_capacity_value(XrValue cap_value, uint8_t etype) {
    if (!XR_IS_INT(cap_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_ARRAY_CAPACITY_EXPECTS_MSG);
    return xrt_array_new_typed_exact(XR_TO_INT(cap_value), etype);
}

static inline int xrt_array_fill_typed_fast(xrt_array_t *a, XrValue v, int64_t start, int64_t end);

static inline int xrt_array_fill_value_is_zero_bits(XrValue value, uint8_t etype) {
    switch (etype) {
        case XR_ELEM_I8:
        case XR_ELEM_U8:
        case XR_ELEM_I16:
        case XR_ELEM_U16:
        case XR_ELEM_I32:
        case XR_ELEM_U32:
        case XR_ELEM_I64:
        case XR_ELEM_U64:
            return XR_IS_INT(value) && XR_TO_INT(value) == 0;
        case XR_ELEM_F32:
        case XR_ELEM_F64:
            return (XR_IS_INT(value) && XR_TO_INT(value) == 0) ||
                   (XR_IS_FLOAT(value) && XR_TO_FLOAT(value) == 0.0);
        case XR_ELEM_BOOL:
            return XR_IS_FALSE(value) || XR_IS_NULL(value) ||
                   (XR_IS_INT(value) && XR_TO_INT(value) == 0) ||
                   (XR_IS_FLOAT(value) && XR_TO_FLOAT(value) == 0.0);
        case XR_ELEM_RUNE:
            return XR_IS_RUNE(value) && XR_TO_RUNE(value) == 0;
        default:
            return 0;
    }
}

static inline XrValue xrt_array_resize_value(XrValue arr_value, XrValue len_value,
                                             XrValue fill_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr)
        return arr_value;
    if (!XR_IS_INT(len_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_ARRAY_RESIZE_EXPECTS_MSG);
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    xrt_array_check_store_or_abort(a, fill_value, "Array.resize");
    bool can_resize = a->data_storage != XR_ARRAY_DATA_BORROWED;
    XrArrayCoreResizePlan plan =
        xr_array_core_resize_plan(a->length, a->capacity, XR_TO_INT(len_value), can_resize);
    if (plan.kind == XR_ARRAY_CORE_RESIZE_INVALID)
        xrt_throw_error(XR_ERR_OUT_OF_MEMORY, XR_ERROR_CORE_ARRAY_RESIZE_FAILED_MSG);
    if (plan.kind == XR_ARRAY_CORE_RESIZE_KEEP)
        return arr_value;
    if (plan.reserve_capacity > a->capacity)
        xrt_array_reserve_raw(a, plan.reserve_capacity);
    if (plan.kind == XR_ARRAY_CORE_RESIZE_GROW) {
        int64_t fill_end = plan.fill_start + plan.fill_count;
        if (!xrt_array_fill_typed_fast(a, fill_value, plan.fill_start, fill_end)) {
            for (int64_t i = plan.fill_start; i < fill_end; i++)
                xr_typed_set(a->data, (int32_t) i, fill_value, a->elem_type);
        }
    }
    a->length = plan.length;
    if (a->elem_type == XR_ELEM_ANY)
        XR_ARRAY_MARK_MUTATED(a);
    return arr_value;
}

static inline XrValue xrt_array_reserve_value(XrValue arr_value, XrValue cap_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr)
        return arr_value;
    if (!XR_IS_INT(cap_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_ARRAY_RESERVE_EXPECTS_MSG);
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    bool can_resize = a->data_storage != XR_ARRAY_DATA_BORROWED;
    XrArrayCoreCapacityPlan plan =
        xr_array_core_reserve_plan(a->capacity, XR_TO_INT(cap_value), can_resize);
    if (plan.kind == XR_ARRAY_CORE_CAPACITY_INVALID)
        xrt_throw_error(XR_ERR_OUT_OF_MEMORY, XR_ERROR_CORE_ARRAY_RESERVE_FAILED_MSG);
    if (plan.kind == XR_ARRAY_CORE_CAPACITY_GROW)
        xrt_array_reserve_raw(a, plan.target_capacity);
    return arr_value;
}

static inline xrt_array_t *xrt_array_reserve_trusted_raw(xrt_array_t *a, int64_t cap) {
    if (!a)
        return a;
    bool can_resize = a->data_storage != XR_ARRAY_DATA_BORROWED;
    XrArrayCoreCapacityPlan plan = xr_array_core_reserve_plan(a->capacity, cap, can_resize);
    if (plan.kind == XR_ARRAY_CORE_CAPACITY_INVALID)
        xrt_throw_error(XR_ERR_OUT_OF_MEMORY, XR_ERROR_CORE_ARRAY_RESERVE_FAILED_MSG);
    if (plan.kind == XR_ARRAY_CORE_CAPACITY_GROW)
        xrt_array_reserve_raw(a, plan.target_capacity);
    return a;
}

static inline XrValue xrt_array_new_filled_value(XrValue len_value, XrValue fill_value,
                                                 uint8_t etype) {
    if (!XR_IS_INT(len_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_ARRAY_CAPACITY_EXPECTS_MSG);
    int64_t len = XR_TO_INT(len_value);
    if (len < 0)
        len = 0;
    XrValue arr = xrt_array_new_typed_exact(len, etype);
    xrt_array_t *a = (xrt_array_t *) arr.ptr;
    xrt_array_check_store_or_abort(a, fill_value, "xrt_array_new_filled");
    if (!xrt_array_fill_value_is_zero_bits(fill_value, a->elem_type) &&
        !xrt_array_fill_typed_fast(a, fill_value, 0, len)) {
        for (int64_t i = 0; i < len; i++)
            xr_typed_set(a->data, (int32_t) i, fill_value, a->elem_type);
    }
    a->length = len;
    return arr;
}

static inline XrValue xrt_array_new_copy_value(XrValue src_value, uint8_t etype) {
    if (!XR_IS_ARRAY(src_value) || !src_value.ptr)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_CONSTRUCTOR_EXPECTS_MSG);
    xrt_array_t *src = (xrt_array_t *) src_value.ptr;
    int64_t len = src->length < 0 ? 0 : src->length;
    XrValue arr = xrt_array_new_typed_exact(len, etype);
    xrt_array_t *dst = (xrt_array_t *) arr.ptr;
    dst->length = len;
    if (len <= 0)
        return arr;
    if (src->elem_type == dst->elem_type && dst->elem_type != XR_ELEM_ANY) {
        memcpy(dst->data, src->data, (size_t) len * (size_t) dst->elem_size);
        return arr;
    }
    for (int64_t i = 0; i < len; i++) {
        XrValue item = xr_typed_get(src->data, (int32_t) i, src->elem_type);
        xr_typed_set(dst->data, (int32_t) i, item, dst->elem_type);
    }
    return arr;
}

static inline int xrt_byte_array_range_ok(xrt_array_t *a, int64_t offset, int64_t count) {
    return a && a->elem_type == XR_ELEM_U8 && offset >= 0 && count >= 0 && count <= a->length &&
           offset <= a->length - count;
}

static inline int64_t xrt_byte_array_load_u16_le_unchecked_raw(xrt_array_t *a, int64_t off) {
    const uint8_t *p = (const uint8_t *) a->data + off;
    return (int64_t) xr_raw_u16_from_le(xr_raw_load_u16_unaligned(p));
}

static inline uint16_t xrt_byte_array_load_u16_le_raw(xrt_array_t *a, int64_t off) {
    if (!xrt_byte_array_range_ok(a, off, 2))
        return 0;
    return (uint16_t) xrt_byte_array_load_u16_le_unchecked_raw(a, off);
}

static inline int64_t xrt_byte_array_load_u16_le_checked_raw(xrt_array_t *a, int64_t off) {
    if (!a || a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_LOAD_U16_RECEIVER_MSG);
    if (!xrt_byte_array_range_ok(a, off, 2))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_LOAD_U16_OOB_MSG);
    return (int64_t) xrt_byte_array_load_u16_le_raw(a, off);
}

static inline int64_t xrt_byte_array_load_u32_le_unchecked_raw(xrt_array_t *a, int64_t off) {
    const uint8_t *p = (const uint8_t *) a->data + off;
    return (int64_t) xr_raw_u32_from_le(xr_raw_load_u32_unaligned(p));
}

static inline uint32_t xrt_byte_array_load_u32_le_raw(xrt_array_t *a, int64_t off) {
    if (!xrt_byte_array_range_ok(a, off, 4))
        return 0;
    return (uint32_t) xrt_byte_array_load_u32_le_unchecked_raw(a, off);
}

static inline int64_t xrt_byte_array_load_u32_le_checked_raw(xrt_array_t *a, int64_t off) {
    if (!a || a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_LOAD_U32_RECEIVER_MSG);
    if (!xrt_byte_array_range_ok(a, off, 4))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_LOAD_U32_OOB_MSG);
    return (int64_t) xrt_byte_array_load_u32_le_raw(a, off);
}

static inline int64_t xrt_byte_array_load_u64_le_unchecked_raw(xrt_array_t *a, int64_t off) {
    const uint8_t *p = (const uint8_t *) a->data + off;
    return (int64_t) xr_raw_u64_from_le(xr_raw_load_u64_unaligned(p));
}

static inline uint64_t xrt_byte_array_load_u64_le_raw(xrt_array_t *a, int64_t off) {
    if (!xrt_byte_array_range_ok(a, off, 8))
        return 0;
    return (uint64_t) xrt_byte_array_load_u64_le_unchecked_raw(a, off);
}

static inline int64_t xrt_byte_array_load_u64_le_checked_raw(xrt_array_t *a, int64_t off) {
    if (!a || a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_LOAD_U64_RECEIVER_MSG);
    if (!xrt_byte_array_range_ok(a, off, 8))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_LOAD_U64_OOB_MSG);
    return (int64_t) xrt_byte_array_load_u64_le_raw(a, off);
}

static inline int xrt_byte_slice_range_ok(xr_span_t span, int64_t off, int64_t count) {
    return span.data && xr_array_core_bytes_range_ok(span.length, XR_ELEM_U8, off, count);
}

static inline int64_t xrt_endian_arg(XrValue value) {
    int64_t endian = XR_ENDIAN_NATIVE;
    if (XR_IS_INT(value)) {
        endian = XR_TO_INT(value);
    } else if (value.tag == XR_TAG_ENUM) {
        const XrAotEnumBox *ev = (const XrAotEnumBox *) value.ptr;
        if (ev)
            endian = (int64_t) ev->member_index;
    }
    if (endian < XR_ENDIAN_NATIVE || endian > XR_ENDIAN_BE)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, "invalid Endian value");
    if (endian == XR_ENDIAN_NATIVE)
        return XRT_TARGET_NATIVE_ENDIAN;
    return endian;
}

static inline int64_t xrt_byte_slice_load_u16_checked_raw(xr_span_t span, int64_t off,
                                                          int64_t endian) {
    bool ok = false;
    uint16_t value =
        xr_array_core_bytes_load_u16(span.data, span.length, XR_ELEM_U8, off, endian, &ok);
    if (!ok)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_LOAD_U16_OOB_MSG);
    return (int64_t) value;
}

static inline int64_t xrt_byte_slice_load_u32_checked_raw(xr_span_t span, int64_t off,
                                                          int64_t endian) {
    bool ok = false;
    uint32_t value =
        xr_array_core_bytes_load_u32(span.data, span.length, XR_ELEM_U8, off, endian, &ok);
    if (!ok)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_LOAD_U32_OOB_MSG);
    return (int64_t) value;
}

static inline int64_t xrt_byte_slice_load_u64_checked_raw(xr_span_t span, int64_t off,
                                                          int64_t endian) {
    bool ok = false;
    uint64_t value =
        xr_array_core_bytes_load_u64(span.data, span.length, XR_ELEM_U8, off, endian, &ok);
    if (!ok)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_LOAD_U64_OOB_MSG);
    return (int64_t) value;
}

static inline double xrt_byte_slice_load_f32_checked_raw(xr_span_t span, int64_t off,
                                                         int64_t endian) {
    bool ok = false;
    float value =
        xr_array_core_bytes_load_f32(span.data, span.length, XR_ELEM_U8, off, endian, &ok);
    if (!ok)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_LOAD_F32_OOB_MSG);
    return (double) value;
}

static inline double xrt_byte_slice_load_f64_checked_raw(xr_span_t span, int64_t off,
                                                         int64_t endian) {
    bool ok = false;
    double value =
        xr_array_core_bytes_load_f64(span.data, span.length, XR_ELEM_U8, off, endian, &ok);
    if (!ok)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_LOAD_F64_OOB_MSG);
    return value;
}

static inline void xrt_byte_slice_store_u16_checked_raw(xr_span_t span, int64_t off, int64_t value,
                                                        int64_t endian) {
    if (!xr_array_core_bytes_store_u16(span.data, span.length, XR_ELEM_U8, off, (uint16_t) value,
                                       endian))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_STORE_U16_OOB_MSG);
}

static inline void xrt_byte_slice_store_u32_checked_raw(xr_span_t span, int64_t off, int64_t value,
                                                        int64_t endian) {
    if (!xr_array_core_bytes_store_u32(span.data, span.length, XR_ELEM_U8, off, (uint32_t) value,
                                       endian))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_STORE_U32_OOB_MSG);
}

static inline void xrt_byte_slice_store_u64_checked_raw(xr_span_t span, int64_t off, int64_t value,
                                                        int64_t endian) {
    if (!xr_array_core_bytes_store_u64(span.data, span.length, XR_ELEM_U8, off, (uint64_t) value,
                                       endian))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_STORE_U64_OOB_MSG);
}

static inline void xrt_byte_slice_store_f32_checked_raw(xr_span_t span, int64_t off, double value,
                                                        int64_t endian) {
    if (!xr_array_core_bytes_store_f32(span.data, span.length, XR_ELEM_U8, off, (float) value,
                                       endian))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_STORE_F32_OOB_MSG);
}

static inline void xrt_byte_slice_store_f64_checked_raw(xr_span_t span, int64_t off, double value,
                                                        int64_t endian) {
    if (!xr_array_core_bytes_store_f64(span.data, span.length, XR_ELEM_U8, off, value, endian))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_STORE_F64_OOB_MSG);
}

static inline xr_span_t xrt_byte_slice_fill_checked_raw(xr_span_t span, int64_t value) {
    if (span.length > 0 && !span.data)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_FILL_OOB_MSG);
    if (span.length > 0)
        memset(span.data, (uint8_t) value, (size_t) span.length);
    return span;
}

static inline xr_span_t xrt_byte_slice_copy_checked_raw(xr_span_t dst, xr_span_t src) {
    if (!xr_array_core_bytes_copy_from(dst.data, dst.length, XR_ELEM_U8, src.data, src.length,
                                       XR_ELEM_U8, 0, 0, src.length, false))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_COPY_OOB_MSG);
    return dst;
}

static inline int64_t xrt_byte_slice_compare_checked_raw(xr_span_t left, xr_span_t right) {
    int64_t n = left.length < right.length ? left.length : right.length;
    int cmp = 0;
    if (n > 0) {
        if (!left.data || !right.data)
            xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_COMPARE_NO_DATA_MSG);
        cmp = memcmp(left.data, right.data, (size_t) n);
    }
    if (cmp < 0)
        return -1;
    if (cmp > 0)
        return 1;
    if (left.length < right.length)
        return -1;
    if (left.length > right.length)
        return 1;
    return 0;
}

static inline int64_t xrt_byte_slice_common_prefix_checked_raw(xr_span_t left, xr_span_t right) {
    bool ok = false;
    int64_t prefix = xr_array_core_bytes_common_prefix(left.data, left.length, XR_ELEM_U8,
                                                       right.data, right.length, XR_ELEM_U8, &ok);
    if (!ok)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_COMMON_PREFIX_NO_DATA_MSG);
    return prefix;
}

static inline xr_span_t xrt_byte_slice_repeat_from_checked_raw(xr_span_t span, int64_t dst_offset,
                                                               int64_t distance, int64_t count) {
    if (!xr_array_core_bytes_repeat_from(span.data, span.length, XR_ELEM_U8, dst_offset, distance,
                                         count))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_REPEAT_OOB_MSG);
    return span;
}

static inline xr_span_t xrt_byte_slice_from_value(XrValue recv, const char *message) {
    if (XR_IS_ARRAY(recv) && recv.ptr) {
        xrt_array_t *arr = (xrt_array_t *) recv.ptr;
        if (arr->elem_type != XR_ELEM_U8)
            xrt_throw_error(XR_ERR_TYPE_MISMATCH, message);
        return xrt_span_from_array_slice(recv, 0, arr->length);
    }
    if (recv.tag == XR_TAG_AGG_REF && recv.ptr && !XR_IS_ARRAY_REF(recv)) {
        return *(const xr_span_t *) recv.ptr;
    }
    xrt_throw_error(XR_ERR_TYPE_MISMATCH, message);
    return xrt_span_empty();
}

static inline XrValue xrt_byte_slice_load_u16_value(XrValue recv, XrValue off_value,
                                                    XrValue endian_value) {
    if (!XR_IS_INT(off_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_LOAD_OFFSET_EXPECTS_MSG);
    xr_span_t span =
        xrt_byte_slice_from_value(recv, XR_ERROR_CORE_BYTE_SLICE_LOAD_U16_RECEIVER_MSG);
    int64_t value = xrt_byte_slice_load_u16_checked_raw(span, XR_TO_INT(off_value),
                                                        xrt_endian_arg(endian_value));
    return XR_FROM_INT(value);
}

static inline XrValue xrt_byte_slice_load_u32_value(XrValue recv, XrValue off_value,
                                                    XrValue endian_value) {
    if (!XR_IS_INT(off_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_LOAD_OFFSET_EXPECTS_MSG);
    xr_span_t span =
        xrt_byte_slice_from_value(recv, XR_ERROR_CORE_BYTE_SLICE_LOAD_U32_RECEIVER_MSG);
    int64_t value = xrt_byte_slice_load_u32_checked_raw(span, XR_TO_INT(off_value),
                                                        xrt_endian_arg(endian_value));
    return XR_FROM_INT(value);
}

static inline XrValue xrt_byte_slice_load_u64_value(XrValue recv, XrValue off_value,
                                                    XrValue endian_value) {
    if (!XR_IS_INT(off_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_LOAD_OFFSET_EXPECTS_MSG);
    xr_span_t span =
        xrt_byte_slice_from_value(recv, XR_ERROR_CORE_BYTE_SLICE_LOAD_U64_RECEIVER_MSG);
    int64_t value = xrt_byte_slice_load_u64_checked_raw(span, XR_TO_INT(off_value),
                                                        xrt_endian_arg(endian_value));
    return XR_FROM_INT(value);
}

static inline XrValue xrt_byte_slice_load_f32_value(XrValue recv, XrValue off_value,
                                                    XrValue endian_value) {
    if (!XR_IS_INT(off_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_LOAD_OFFSET_EXPECTS_MSG);
    xr_span_t span =
        xrt_byte_slice_from_value(recv, XR_ERROR_CORE_BYTE_SLICE_LOAD_F32_RECEIVER_MSG);
    double value = xrt_byte_slice_load_f32_checked_raw(span, XR_TO_INT(off_value),
                                                       xrt_endian_arg(endian_value));
    return XR_FROM_FLOAT(value);
}

static inline XrValue xrt_byte_slice_load_f64_value(XrValue recv, XrValue off_value,
                                                    XrValue endian_value) {
    if (!XR_IS_INT(off_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_LOAD_OFFSET_EXPECTS_MSG);
    xr_span_t span =
        xrt_byte_slice_from_value(recv, XR_ERROR_CORE_BYTE_SLICE_LOAD_F64_RECEIVER_MSG);
    double value = xrt_byte_slice_load_f64_checked_raw(span, XR_TO_INT(off_value),
                                                       xrt_endian_arg(endian_value));
    return XR_FROM_FLOAT(value);
}

static inline void xrt_byte_slice_store_u16_value(XrValue recv, XrValue off_value, XrValue value,
                                                  XrValue endian_value) {
    if (!XR_IS_INT(off_value) || !XR_IS_INT(value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_STORE_VALUE_EXPECTS_MSG);
    xr_span_t span =
        xrt_byte_slice_from_value(recv, XR_ERROR_CORE_BYTE_SLICE_STORE_U16_RECEIVER_MSG);
    xrt_byte_slice_store_u16_checked_raw(span, XR_TO_INT(off_value), XR_TO_INT(value),
                                         xrt_endian_arg(endian_value));
}

static inline void xrt_byte_slice_store_u32_value(XrValue recv, XrValue off_value, XrValue value,
                                                  XrValue endian_value) {
    if (!XR_IS_INT(off_value) || !XR_IS_INT(value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_STORE_VALUE_EXPECTS_MSG);
    xr_span_t span =
        xrt_byte_slice_from_value(recv, XR_ERROR_CORE_BYTE_SLICE_STORE_U32_RECEIVER_MSG);
    xrt_byte_slice_store_u32_checked_raw(span, XR_TO_INT(off_value), XR_TO_INT(value),
                                         xrt_endian_arg(endian_value));
}

static inline void xrt_byte_slice_store_u64_value(XrValue recv, XrValue off_value, XrValue value,
                                                  XrValue endian_value) {
    if (!XR_IS_INT(off_value) || !XR_IS_INT(value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_STORE_VALUE_EXPECTS_MSG);
    xr_span_t span =
        xrt_byte_slice_from_value(recv, XR_ERROR_CORE_BYTE_SLICE_STORE_U64_RECEIVER_MSG);
    xrt_byte_slice_store_u64_checked_raw(span, XR_TO_INT(off_value), XR_TO_INT(value),
                                         xrt_endian_arg(endian_value));
}

static inline void xrt_byte_slice_store_f32_value(XrValue recv, XrValue off_value, XrValue value,
                                                  XrValue endian_value) {
    if (!XR_IS_INT(off_value) || !XR_IS_FLOAT(value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTE_SLICE_STORE_FLOAT_VALUE_EXPECTS_MSG);
    xr_span_t span =
        xrt_byte_slice_from_value(recv, XR_ERROR_CORE_BYTE_SLICE_STORE_F32_RECEIVER_MSG);
    xrt_byte_slice_store_f32_checked_raw(span, XR_TO_INT(off_value), XR_TO_FLOAT(value),
                                         xrt_endian_arg(endian_value));
}

static inline void xrt_byte_slice_store_f64_value(XrValue recv, XrValue off_value, XrValue value,
                                                  XrValue endian_value) {
    if (!XR_IS_INT(off_value) || !XR_IS_FLOAT(value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTE_SLICE_STORE_FLOAT_VALUE_EXPECTS_MSG);
    xr_span_t span =
        xrt_byte_slice_from_value(recv, XR_ERROR_CORE_BYTE_SLICE_STORE_F64_RECEIVER_MSG);
    xrt_byte_slice_store_f64_checked_raw(span, XR_TO_INT(off_value), XR_TO_FLOAT(value),
                                         xrt_endian_arg(endian_value));
}

static inline int64_t xrt_byte_slice_load_u16_le_unchecked_raw(xr_span_t span, int64_t off) {
    const uint8_t *ptr = (const uint8_t *) span.data + off;
    return (int64_t) xr_raw_u16_from_le(xr_raw_load_u16_unaligned(ptr));
}

static inline int64_t xrt_byte_slice_load_u16_le_checked_raw(xr_span_t span, int64_t off) {
    if (!xrt_byte_slice_range_ok(span, off, 2))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_LOAD_U16_OOB_MSG);
    return xrt_byte_slice_load_u16_le_unchecked_raw(span, off);
}

static inline int64_t xrt_byte_slice_load_u32_le_unchecked_raw(xr_span_t span, int64_t off) {
    const uint8_t *ptr = (const uint8_t *) span.data + off;
    return (int64_t) xr_raw_u32_from_le(xr_raw_load_u32_unaligned(ptr));
}

static inline int64_t xrt_byte_slice_load_u32_le_checked_raw(xr_span_t span, int64_t off) {
    if (!xrt_byte_slice_range_ok(span, off, 4))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_LOAD_U32_OOB_MSG);
    return xrt_byte_slice_load_u32_le_unchecked_raw(span, off);
}

static inline int64_t xrt_byte_slice_load_u64_le_unchecked_raw(xr_span_t span, int64_t off) {
    const uint8_t *ptr = (const uint8_t *) span.data + off;
    return (int64_t) xr_raw_u64_from_le(xr_raw_load_u64_unaligned(ptr));
}

static inline int64_t xrt_byte_slice_load_u64_le_checked_raw(xr_span_t span, int64_t off) {
    if (!xrt_byte_slice_range_ok(span, off, 8))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_LOAD_U64_OOB_MSG);
    return xrt_byte_slice_load_u64_le_unchecked_raw(span, off);
}

static inline xrt_array_t *xrt_byte_array_copy_within_raw(xrt_array_t *a, int64_t dst, int64_t src,
                                                          int64_t count) {
    if (xrt_byte_array_range_ok(a, dst, count) && xrt_byte_array_range_ok(a, src, count) &&
        count > 0)
        memmove((uint8_t *) a->data + dst, (uint8_t *) a->data + src, (size_t) count);
    return a;
}

static inline xrt_array_t *xrt_byte_array_copy_from_raw(xrt_array_t *dst, xrt_array_t *src,
                                                        int64_t src_offset, int64_t dst_offset,
                                                        int64_t count) {
    if (!xrt_byte_array_range_ok(src, src_offset, count) ||
        !xrt_byte_array_range_ok(dst, dst_offset, count) || count <= 0)
        return dst;
    uint8_t *dp = (uint8_t *) dst->data + dst_offset;
    uint8_t *sp = (uint8_t *) src->data + src_offset;
    xr_array_core_copy_or_move_bytes(dp, sp, count);
    return dst;
}

static inline xrt_array_t *xrt_byte_array_copy_within_checked_raw(xrt_array_t *a, int64_t dst,
                                                                  int64_t src, int64_t count) {
    if (!a || a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_COPY_WITHIN_RECEIVER_MSG);
    if (!xrt_byte_array_range_ok(a, dst, count) || !xrt_byte_array_range_ok(a, src, count))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_ARRAY_COPY_WITHIN_OOB_MSG);
    return xrt_byte_array_copy_within_raw(a, dst, src, count);
}

static inline xrt_array_t *xrt_byte_array_copy_from_checked_raw(xrt_array_t *dst, xrt_array_t *src,
                                                                int64_t src_offset,
                                                                int64_t dst_offset, int64_t count) {
    if (!dst || !src || dst->elem_type != XR_ELEM_U8 || src->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_COPY_FROM_OPERANDS_MSG);
    if (!xrt_byte_array_range_ok(src, src_offset, count) ||
        !xrt_byte_array_range_ok(dst, dst_offset, count))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_ARRAY_COPY_FROM_OOB_MSG);
    return xrt_byte_array_copy_from_raw(dst, src, src_offset, dst_offset, count);
}

static inline xrt_array_t *xrt_byte_array_repeat_from_raw(xrt_array_t *a, int64_t dst,
                                                          int64_t distance, int64_t count) {
    if (!a || a->elem_type != XR_ELEM_U8 || dst < 0 || distance <= 0 || count < 0 ||
        dst - distance < 0 || count > a->length || dst > a->length - count)
        return a;
    xr_array_core_bytes_repeat_copy(a->data, dst, distance, count);
    return a;
}

static inline xrt_array_t *xrt_byte_array_append_from_span_slow_raw(xrt_array_t *dst,
                                                                    xr_span_t src) {
    if (!dst || dst->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_APPEND_FROM_OPERANDS_MSG);
    if (dst->data_storage == XR_ARRAY_DATA_BORROWED || src.length < 0 ||
        (src.length > 0 && !src.data) || src.length > INT64_MAX - dst->length)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_ARRAY_APPEND_FROM_OOB_MSG);

    bool aliases_dst = false;
    int64_t src_offset = 0;
    if (src.length > 0 && dst->data) {
        const uint8_t *base = (const uint8_t *) dst->data;
        const uint8_t *sp = (const uint8_t *) src.data;
        if (sp >= base && sp <= base + dst->length) {
            src_offset = (int64_t) (sp - base);
            if (src.length > dst->length || src_offset > dst->length - src.length)
                xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS,
                                XR_ERROR_CORE_BYTE_ARRAY_APPEND_FROM_OOB_MSG);
            aliases_dst = true;
        }
    }

    int64_t old_length = dst->length;
    int64_t new_length = old_length + src.length;
    if (new_length > dst->capacity)
        xrt_array_reserve_trusted_raw(dst, new_length);
    if (new_length > dst->capacity || (new_length > 0 && !dst->data))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_ARRAY_APPEND_FROM_OOB_MSG);
    if (src.length > 0) {
        const uint8_t *sp =
            aliases_dst ? (const uint8_t *) dst->data + src_offset : (const uint8_t *) src.data;
        uint8_t *dp = (uint8_t *) dst->data + old_length;
        xr_array_core_copy_or_move_bytes(dp, sp, src.length);
    }
    dst->length = new_length;
    return dst;
}

static inline xrt_array_t *xrt_byte_array_append_from_span_raw(xrt_array_t *dst, xr_span_t src) {
    if (XR_LIKELY(dst && dst->elem_type == XR_ELEM_U8 &&
                  dst->data_storage != XR_ARRAY_DATA_BORROWED && src.length >= 0 &&
                  src.length <= INT64_MAX - dst->length)) {
        int64_t old_length = dst->length;
        int64_t new_length = old_length + src.length;
        if (XR_LIKELY(new_length <= dst->capacity && (new_length == 0 || dst->data) &&
                      (src.length == 0 || src.data))) {
            if (src.length > 0) {
                uint8_t *dp = (uint8_t *) dst->data + old_length;
                xr_array_core_copy_or_move_bytes(dp, src.data, src.length);
            }
            dst->length = new_length;
            return dst;
        }
    }
    return xrt_byte_array_append_from_span_slow_raw(dst, src);
}

static inline xrt_array_t *xrt_byte_array_repeat_from_tail_raw(xrt_array_t *a, int64_t distance,
                                                               int64_t count) {
    if (!a || a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_RECEIVER_MSG);
    if (a->data_storage == XR_ARRAY_DATA_BORROWED || distance <= 0 || count < 0 ||
        distance > a->length || count > INT64_MAX - a->length)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_OOB_MSG);
    int64_t dst = a->length;
    int64_t new_length = dst + count;
    if (new_length > a->capacity)
        xrt_array_reserve_trusted_raw(a, new_length);
    if (new_length > a->capacity || (new_length > 0 && !a->data))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_OOB_MSG);
    xr_array_core_bytes_repeat_copy(a->data, dst, distance, count);
    a->length = new_length;
    return a;
}

static inline XrValue xrt_byte_array_load_u32_le(XrValue arr_value, XrValue offset_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr || !XR_IS_INT(offset_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_LOAD_U32_EXPECTS_MSG);
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    if (a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_LOAD_U32_RECEIVER_MSG);
    int64_t off = XR_TO_INT(offset_value);
    if (!xrt_byte_array_range_ok(a, off, 4))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_LOAD_U32_OOB_MSG);
    return XR_FROM_INT((int64_t) xrt_byte_array_load_u32_le_raw(a, off));
}

static inline XrValue xrt_byte_array_load_u16_le(XrValue arr_value, XrValue offset_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr || !XR_IS_INT(offset_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_LOAD_U16_EXPECTS_MSG);
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    if (a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_LOAD_U16_RECEIVER_MSG);
    int64_t off = XR_TO_INT(offset_value);
    if (!xrt_byte_array_range_ok(a, off, 2))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_LOAD_U16_OOB_MSG);
    return XR_FROM_INT((int64_t) xrt_byte_array_load_u16_le_raw(a, off));
}

static inline XrValue xrt_byte_array_load_u64_le(XrValue arr_value, XrValue offset_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr || !XR_IS_INT(offset_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_LOAD_U64_EXPECTS_MSG);
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    if (a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_LOAD_U64_RECEIVER_MSG);
    int64_t off = XR_TO_INT(offset_value);
    if (!xrt_byte_array_range_ok(a, off, 8))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_LOAD_U64_OOB_MSG);
    return XR_FROM_INT((int64_t) xrt_byte_array_load_u64_le_raw(a, off));
}

static inline XrValue xrt_byte_array_copy_within_value(XrValue arr_value, XrValue dst_value,
                                                       XrValue src_value, XrValue count_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr || !XR_IS_INT(dst_value) ||
        !XR_IS_INT(src_value) || !XR_IS_INT(count_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_COPY_WITHIN_EXPECTS_MSG);
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    if (a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_COPY_WITHIN_RECEIVER_MSG);
    int64_t dst = XR_TO_INT(dst_value);
    int64_t src = XR_TO_INT(src_value);
    int64_t count = XR_TO_INT(count_value);
    xrt_byte_array_copy_within_checked_raw(a, dst, src, count);
    return arr_value;
}

static inline XrValue xrt_byte_array_copy_from_value(XrValue dst_value, XrValue src_value,
                                                     XrValue src_offset_value,
                                                     XrValue dst_offset_value,
                                                     XrValue count_value) {
    if (!XR_IS_ARRAY(dst_value) || !XR_IS_ARRAY(src_value) || !dst_value.ptr || !src_value.ptr ||
        !XR_IS_INT(src_offset_value) || !XR_IS_INT(dst_offset_value) || !XR_IS_INT(count_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_COPY_FROM_EXPECTS_MSG);
    int64_t src_offset = XR_TO_INT(src_offset_value);
    int64_t dst_offset = XR_TO_INT(dst_offset_value);
    int64_t count = XR_TO_INT(count_value);
    xrt_byte_array_copy_from_checked_raw((xrt_array_t *) dst_value.ptr,
                                         (xrt_array_t *) src_value.ptr, src_offset, dst_offset,
                                         count);
    return dst_value;
}

static inline XrValue xrt_byte_array_repeat_from_value(XrValue arr_value, XrValue dst_value,
                                                       XrValue distance_value,
                                                       XrValue count_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_RECEIVER_MSG);
    if (!XR_IS_INT(dst_value) || !XR_IS_INT(distance_value) || !XR_IS_INT(count_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_EXPECTS_MSG);
    int64_t dst = xr_value_to_int64_coerce(dst_value);
    int64_t distance = xr_value_to_int64_coerce(distance_value);
    int64_t count = xr_value_to_int64_coerce(count_value);
    xrt_byte_array_repeat_from_raw((xrt_array_t *) arr_value.ptr, dst, distance, count);
    return arr_value;
}

static inline XrValue xrt_byte_array_append_from_value(XrValue arr_value, XrValue src_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_APPEND_FROM_OPERANDS_MSG);
    xr_span_t src =
        xrt_byte_slice_from_value(src_value, XR_ERROR_CORE_BYTE_ARRAY_APPEND_FROM_EXPECTS_MSG);
    xrt_byte_array_append_from_span_raw((xrt_array_t *) arr_value.ptr, src);
    return arr_value;
}

static inline XrValue xrt_byte_array_repeat_from_tail_value(XrValue arr_value,
                                                            XrValue distance_value,
                                                            XrValue count_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr || !XR_IS_INT(distance_value) ||
        !XR_IS_INT(count_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_EXPECTS_MSG);
    xrt_byte_array_repeat_from_tail_raw((xrt_array_t *) arr_value.ptr, XR_TO_INT(distance_value),
                                        XR_TO_INT(count_value));
    return arr_value;
}

/* =========================================================================
 * Typed array algorithm fast paths — avoid the per-element xr_typed_get /
 * xr_typed_set switch in hot loops.  Semantics match the generic paths:
 * fill coerces once like xr_typed_set; indexOf only matches when the boxed
 * element tag would equal the needle tag (so an i64 needle never matches a
 * float buffer and vice versa).  Return 0 to fall back to the generic loop.
 * ========================================================================= */

#define XRT_FILL_LOOP(T, val)                                                                      \
    do {                                                                                           \
        T *d = (T *) a->data;                                                                      \
        T fv = (T) (val);                                                                          \
        for (int64_t i = start; i < end; i++)                                                      \
            d[i] = fv;                                                                             \
    } while (0)

static inline int xrt_array_fill_typed_fast(xrt_array_t *a, XrValue v, int64_t start, int64_t end) {
    size_t count = (size_t) (end - start);
    switch (a->elem_type) {
        case XR_ELEM_I8:
        case XR_ELEM_U8: {
            uint8_t b = (uint8_t) xr_value_to_int64_coerce(v);
            memset((uint8_t *) a->data + start, b, count);
            return 1;
        }
        case XR_ELEM_BOOL: {
            int falsy = XR_IS_FALSE(v) || XR_IS_NULL(v) || (XR_IS_INT(v) && v.i == 0) ||
                        (XR_IS_FLOAT(v) && v.f == 0.0);
            memset((uint8_t *) a->data + start, falsy ? 0 : 1, count);
            return 1;
        }
        case XR_ELEM_I16:
            XRT_FILL_LOOP(int16_t, xr_value_to_int64_coerce(v));
            return 1;
        case XR_ELEM_U16:
            XRT_FILL_LOOP(uint16_t, xr_value_to_int64_coerce(v));
            return 1;
        case XR_ELEM_I32:
            XRT_FILL_LOOP(int32_t, xr_value_to_int64_coerce(v));
            return 1;
        case XR_ELEM_U32:
            XRT_FILL_LOOP(uint32_t, xr_value_to_int64_coerce(v));
            return 1;
        case XR_ELEM_RUNE:
            if (!XR_IS_RUNE(v))
                return 1;
            XRT_FILL_LOOP(uint32_t, XR_TO_RUNE(v));
            return 1;
        case XR_ELEM_I64:
            XRT_FILL_LOOP(int64_t, xr_value_to_int64_coerce(v));
            return 1;
        case XR_ELEM_U64:
            XRT_FILL_LOOP(uint64_t, xr_value_to_int64_coerce(v));
            return 1;
        case XR_ELEM_F32:
            XRT_FILL_LOOP(float, xr_value_to_f64_coerce(v));
            return 1;
        case XR_ELEM_F64:
            XRT_FILL_LOOP(double, xr_value_to_f64_coerce(v));
            return 1;
        default:
            return 0;
    }
}

#undef XRT_FILL_LOOP

static inline XrValue xrt_array_fill_range_value(XrValue arr_value, XrValue fill_value,
                                                 int64_t start, int64_t end) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr)
        return arr_value;
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    XrArrayCoreRange range = xr_array_core_fill_range(a->length, start, end);
    if (range.count <= 0)
        return arr_value;
    xrt_array_check_store_or_abort(a, fill_value, "Array.fill");
    if (xrt_array_fill_typed_fast(a, fill_value, range.start, range.end)) {
        if (a->elem_type == XR_ELEM_ANY)
            XR_ARRAY_MARK_MUTATED(a);
        return arr_value;
    }
    for (int64_t i = range.start; i < range.end; i++)
        xr_typed_set(a->data, (int32_t) i, fill_value, a->elem_type);
    if (a->elem_type == XR_ELEM_ANY)
        XR_ARRAY_MARK_MUTATED(a);
    return arr_value;
}

static inline XrValue xrt_array_fill_value(XrValue arr_value, XrValue fill_value,
                                           XrValue start_value, XrValue end_value) {
    return xrt_array_fill_range_value(arr_value, fill_value, xr_value_to_int64_coerce(start_value),
                                      xr_value_to_int64_coerce(end_value));
}

#define XRT_INDEXOF_LOOP(T)                                                                        \
    do {                                                                                           \
        const T *d = (const T *) a->data;                                                          \
        for (int64_t i = 0; i < a->length; i++)                                                    \
            if ((int64_t) d[i] == needle)                                                          \
                return i;                                                                          \
        return -1;                                                                                 \
    } while (0)

/* Returns the match index, -1 for no match, or sets *handled = 0 when the
 * combination requires the generic boxed loop. */
static inline int64_t xrt_array_indexof_typed_fast(xrt_array_t *a, XrValue v, int *handled) {
    *handled = 1;
    switch (a->elem_type) {
        case XR_ELEM_I8:
        case XR_ELEM_U8: {
            if (v.tag != XR_TAG_I64)
                return -1; /* boxed elem is I64; other tags never match */
            int64_t needle = v.i;
            if (a->elem_type == XR_ELEM_U8) {
                if (needle < 0 || needle > 255)
                    return -1;
                const void *p = memchr(a->data, (int) needle, (size_t) a->length);
                return p ? (int64_t) ((const uint8_t *) p - (const uint8_t *) a->data) : -1;
            }
            XRT_INDEXOF_LOOP(int8_t);
        }
        case XR_ELEM_BOOL: {
            if (v.tag != XR_TAG_BOOL)
                return -1;
            const void *p = memchr(a->data, v.i ? 1 : 0, (size_t) a->length);
            return p ? (int64_t) ((const uint8_t *) p - (const uint8_t *) a->data) : -1;
        }
        case XR_ELEM_I16: {
            if (v.tag != XR_TAG_I64)
                return -1;
            int64_t needle = v.i;
            XRT_INDEXOF_LOOP(int16_t);
        }
        case XR_ELEM_U16: {
            if (v.tag != XR_TAG_I64)
                return -1;
            int64_t needle = v.i;
            XRT_INDEXOF_LOOP(uint16_t);
        }
        case XR_ELEM_I32: {
            if (v.tag != XR_TAG_I64)
                return -1;
            int64_t needle = v.i;
            XRT_INDEXOF_LOOP(int32_t);
        }
        case XR_ELEM_U32: {
            if (v.tag != XR_TAG_I64)
                return -1;
            int64_t needle = v.i;
            XRT_INDEXOF_LOOP(uint32_t);
        }
        case XR_ELEM_RUNE: {
            if (v.tag != XR_TAG_RUNE)
                return -1;
            int64_t needle = (int64_t) XR_TO_RUNE(v);
            XRT_INDEXOF_LOOP(uint32_t);
        }
        case XR_ELEM_I64: {
            if (v.tag != XR_TAG_I64)
                return -1;
            int64_t needle = v.i;
            XRT_INDEXOF_LOOP(int64_t);
        }
        case XR_ELEM_U64: {
            if (v.tag != XR_TAG_I64)
                return -1;
            int64_t needle = v.i;
            const uint64_t *d = (const uint64_t *) a->data;
            for (int64_t i = 0; i < a->length; i++)
                if ((int64_t) d[i] == needle)
                    return i;
            return -1;
        }
        case XR_ELEM_F32: {
            if (v.tag != XR_TAG_F64)
                return -1;
            const float *d = (const float *) a->data;
            for (int64_t i = 0; i < a->length; i++)
                if ((double) d[i] == v.f)
                    return i;
            return -1;
        }
        case XR_ELEM_F64: {
            if (v.tag != XR_TAG_F64)
                return -1;
            const double *d = (const double *) a->data;
            for (int64_t i = 0; i < a->length; i++)
                if (d[i] == v.f)
                    return i;
            return -1;
        }
        default:
            *handled = 0;
            return -1;
    }
}

#undef XRT_INDEXOF_LOOP
