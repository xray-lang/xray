/* AOT Array capacity and Bytes helpers. */

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
        for (int64_t i = plan.fill_start; i < fill_end; i++)
            xr_typed_set(a->data, (int32_t) i, fill_value, a->elem_type);
    }
    a->length = plan.length;
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
    for (int64_t i = 0; i < len; i++)
        xr_typed_set(a->data, (int32_t) i, fill_value, a->elem_type);
    a->length = len;
    return arr;
}

static inline int xrt_bytes_range_ok(xrt_array_t *a, int64_t offset, int64_t count) {
    return a && a->elem_type == XR_ELEM_U8 && offset >= 0 && count >= 0 && count <= a->length &&
           offset <= a->length - count;
}

static inline uint32_t xrt_bytes_load_u32_le_raw(xrt_array_t *a, int64_t off) {
    if (!xrt_bytes_range_ok(a, off, 4))
        return 0;
    const uint8_t *p = (const uint8_t *) a->data + off;
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
}

static inline uint64_t xrt_bytes_load_u64_le_raw(xrt_array_t *a, int64_t off) {
    if (!xrt_bytes_range_ok(a, off, 8))
        return 0;
    const uint8_t *p = (const uint8_t *) a->data + off;
    return (uint64_t) p[0] | ((uint64_t) p[1] << 8) | ((uint64_t) p[2] << 16) |
           ((uint64_t) p[3] << 24) | ((uint64_t) p[4] << 32) | ((uint64_t) p[5] << 40) |
           ((uint64_t) p[6] << 48) | ((uint64_t) p[7] << 56);
}

static inline xrt_array_t *xrt_bytes_copy_within_raw(xrt_array_t *a, int64_t dst, int64_t src,
                                                     int64_t count) {
    if (xrt_bytes_range_ok(a, dst, count) && xrt_bytes_range_ok(a, src, count) && count > 0)
        memmove((uint8_t *) a->data + dst, (uint8_t *) a->data + src, (size_t) count);
    return a;
}

static inline xrt_array_t *xrt_bytes_copy_from_raw(xrt_array_t *dst, xrt_array_t *src,
                                                   int64_t src_offset, int64_t dst_offset,
                                                   int64_t count) {
    if (!xrt_bytes_range_ok(src, src_offset, count) ||
        !xrt_bytes_range_ok(dst, dst_offset, count) || count <= 0)
        return dst;
    uint8_t *dp = (uint8_t *) dst->data + dst_offset;
    uint8_t *sp = (uint8_t *) src->data + src_offset;
    if (dst == src)
        memmove(dp, sp, (size_t) count);
    else
        memcpy(dp, sp, (size_t) count);
    return dst;
}

static inline xrt_array_t *xrt_bytes_repeat_from_raw(xrt_array_t *a, int64_t dst, int64_t distance,
                                                     int64_t count) {
    if (!a || a->elem_type != XR_ELEM_U8 || dst < 0 || distance <= 0 || count < 0 ||
        dst - distance < 0 || count > a->length || dst > a->length - count)
        return a;
    uint8_t *data = (uint8_t *) a->data;
    for (int64_t i = 0; i < count; i++)
        data[dst + i] = data[dst - distance + i];
    return a;
}

static inline XrValue xrt_bytes_load_u32_le(XrValue arr_value, XrValue offset_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr || !XR_IS_INT(offset_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_LOAD_U32_EXPECTS_MSG);
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    if (a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_LOAD_U32_RECEIVER_MSG);
    int64_t off = XR_TO_INT(offset_value);
    if (!xrt_bytes_range_ok(a, off, 4))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTES_LOAD_U32_OOB_MSG);
    return XR_FROM_INT((int64_t) xrt_bytes_load_u32_le_raw(a, off));
}

static inline XrValue xrt_bytes_load_u64_le(XrValue arr_value, XrValue offset_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr || !XR_IS_INT(offset_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_LOAD_U64_EXPECTS_MSG);
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    if (a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_LOAD_U64_RECEIVER_MSG);
    int64_t off = XR_TO_INT(offset_value);
    if (!xrt_bytes_range_ok(a, off, 8))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTES_LOAD_U64_OOB_MSG);
    return XR_FROM_INT((int64_t) xrt_bytes_load_u64_le_raw(a, off));
}

static inline XrValue xrt_bytes_copy_within_value(XrValue arr_value, XrValue dst_value,
                                                  XrValue src_value, XrValue count_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr || !XR_IS_INT(dst_value) ||
        !XR_IS_INT(src_value) || !XR_IS_INT(count_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_COPY_WITHIN_EXPECTS_MSG);
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    if (a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_COPY_WITHIN_RECEIVER_MSG);
    int64_t dst = XR_TO_INT(dst_value);
    int64_t src = XR_TO_INT(src_value);
    int64_t count = XR_TO_INT(count_value);
    if (!xrt_bytes_range_ok(a, dst, count) || !xrt_bytes_range_ok(a, src, count))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTES_COPY_WITHIN_OOB_MSG);
    xrt_bytes_copy_within_raw(a, dst, src, count);
    return arr_value;
}

static inline XrValue xrt_bytes_copy_from_value(XrValue dst_value, XrValue src_value,
                                                XrValue src_offset_value, XrValue dst_offset_value,
                                                XrValue count_value) {
    if (!XR_IS_ARRAY(dst_value) || !XR_IS_ARRAY(src_value) || !dst_value.ptr || !src_value.ptr)
        return dst_value;
    int64_t src_offset = xr_value_to_int64_coerce(src_offset_value);
    int64_t dst_offset = xr_value_to_int64_coerce(dst_offset_value);
    int64_t count = xr_value_to_int64_coerce(count_value);
    xrt_bytes_copy_from_raw((xrt_array_t *) dst_value.ptr, (xrt_array_t *) src_value.ptr,
                            src_offset, dst_offset, count);
    return dst_value;
}

static inline XrValue xrt_bytes_repeat_from_value(XrValue arr_value, XrValue dst_value,
                                                  XrValue distance_value, XrValue count_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr)
        return arr_value;
    int64_t dst = xr_value_to_int64_coerce(dst_value);
    int64_t distance = xr_value_to_int64_coerce(distance_value);
    int64_t count = xr_value_to_int64_coerce(count_value);
    xrt_bytes_repeat_from_raw((xrt_array_t *) arr_value.ptr, dst, distance, count);
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
        case XR_ELEM_CHAR:
            if (!XR_IS_CHAR(v))
                return 1;
            XRT_FILL_LOOP(uint32_t, XR_TO_CHAR(v));
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
    if (xrt_array_fill_typed_fast(a, fill_value, range.start, range.end))
        return arr_value;
    for (int64_t i = range.start; i < range.end; i++)
        xr_typed_set(a->data, (int32_t) i, fill_value, a->elem_type);
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
        case XR_ELEM_CHAR: {
            if (v.tag != XR_TAG_CHAR)
                return -1;
            int64_t needle = (int64_t) XR_TO_CHAR(v);
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
