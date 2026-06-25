/* AOT Array capacity and Bytes helpers. */

static inline XrValue xrt_array_new_typed_exact(int64_t cap, uint8_t etype) {
    if (cap < 0)
        cap = 0;
    xrt_array_t *a = xrt_array_alloc_inline(cap, etype, 1, "xrt_array_new_typed_exact");
    return xr_mkptr(a, XR_TAG_ARRAY);
}

static inline bool xrt_array_reserve_raw(xrt_array_t *a, int64_t cap) {
    if (!a)
        return false;
    XrArrayCoreCapacityPlan plan =
        xr_array_core_reserve_plan(a->capacity, cap, a->data_storage != XR_ARRAY_DATA_BORROWED);
    if (plan.kind == XR_ARRAY_CORE_CAPACITY_INVALID)
        return false;
    if (plan.kind == XR_ARRAY_CORE_CAPACITY_KEEP)
        return true;
    size_t old_bytes = (size_t) a->capacity * (size_t) a->elem_size;
    xrt_array_data_grow(a, plan.target_capacity);
    size_t new_bytes = (size_t) plan.target_capacity * (size_t) a->elem_size;
    memset((uint8_t *) a->data + old_bytes, 0, new_bytes - old_bytes);
    return a->capacity >= plan.target_capacity;
}

static inline XrValue xrt_array_with_capacity_value(XrValue cap_value, uint8_t etype) {
    int64_t cap =
        xrt_array_required_int_arg_or_panic(cap_value, XR_ERROR_CORE_ARRAY_CAPACITY_EXPECTS_MSG);
    return xrt_array_new_typed_exact(cap, etype);
}

static inline XrValue xrt_array_resize_value(XrValue arr_value, XrValue len_value,
                                             XrValue fill_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr)
        return arr_value;
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    int64_t len =
        xrt_array_required_int_arg_or_panic(len_value, XR_ERROR_CORE_ARRAY_RESIZE_EXPECTS_MSG);
    XrArrayCoreResizePlan plan = xr_array_core_resize_plan(
        a->length, a->capacity, len, a->data_storage != XR_ARRAY_DATA_BORROWED);
    if (plan.kind == XR_ARRAY_CORE_RESIZE_INVALID)
        xrt_throw_error(XR_ERR_OUT_OF_MEMORY, XR_ERROR_CORE_ARRAY_RESIZE_FAILED_MSG);
    if (plan.kind == XR_ARRAY_CORE_RESIZE_KEEP)
        return arr_value;
    if (plan.kind == XR_ARRAY_CORE_RESIZE_SHRINK) {
        if (a->elem_type == XR_ELEM_ANY && a->data) {
            XrValue *items = (XrValue *) a->data;
            for (int64_t i = plan.length; i < a->length; i++) {
                xrt_release(items[i]);
                items[i] = XR_NULL_VAL;
            }
        }
        a->length = plan.length;
        return arr_value;
    }
    if (!xrt_array_reserve_raw(a, plan.reserve_capacity))
        xrt_throw_error(XR_ERR_OUT_OF_MEMORY, XR_ERROR_CORE_ARRAY_RESIZE_FAILED_MSG);
    if (!xr_array_core_fill_typed_storage(a->data, plan.fill_start, plan.fill_count, a->elem_type,
                                          fill_value)) {
        if (a->elem_type == XR_ELEM_ANY) {
            for (int64_t i = 0; i < plan.fill_count; i++)
                xrt_retain(fill_value);
        }
        for (int64_t i = plan.fill_start; i < plan.length; i++)
            xr_typed_set(a->data, (int32_t) i, fill_value, a->elem_type);
    }
    a->length = plan.length;
    return arr_value;
}

static inline XrValue xrt_array_reserve_value(XrValue arr_value, XrValue cap_value) {
    if (XR_IS_ARRAY(arr_value) && arr_value.ptr) {
        int64_t cap =
            xrt_array_required_int_arg_or_panic(cap_value, XR_ERROR_CORE_ARRAY_RESERVE_EXPECTS_MSG);
        if (!xrt_array_reserve_raw((xrt_array_t *) arr_value.ptr, cap))
            xrt_throw_error(XR_ERR_OUT_OF_MEMORY, XR_ERROR_CORE_ARRAY_RESERVE_FAILED_MSG);
    }
    return arr_value;
}

static inline XrValue xrt_array_new_filled_value(XrValue len_value, XrValue fill_value,
                                                 uint8_t etype) {
    int64_t len =
        xrt_array_required_int_arg_or_panic(len_value, XR_ERROR_CORE_ARRAY_CAPACITY_EXPECTS_MSG);
    if (len < 0)
        len = 0;
    XrValue arr = xrt_array_new_typed_exact(len, etype);
    xrt_array_t *a = (xrt_array_t *) arr.ptr;
    if (!xr_array_core_fill_typed_storage(a->data, 0, len, a->elem_type, fill_value)) {
        if (a->elem_type == XR_ELEM_ANY) {
            for (int64_t i = 0; i < len; i++)
                xrt_retain(fill_value);
        }
        for (int64_t i = 0; i < len; i++)
            xr_typed_set(a->data, (int32_t) i, fill_value, a->elem_type);
    }
    a->length = len;
    return arr;
}

static inline uint32_t xrt_bytes_load_u32_le_raw(xrt_array_t *a, int64_t off) {
    bool ok = false;
    uint32_t value = xr_array_core_bytes_load_u32_le(a ? a->data : NULL, a ? a->length : 0,
                                                     a ? a->elem_type : XR_ELEM_ANY, off, &ok);
    if (!ok)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTES_LOAD_U32_OOB_MSG);
    return value;
}

static inline uint64_t xrt_bytes_load_u64_le_raw(xrt_array_t *a, int64_t off) {
    bool ok = false;
    uint64_t value = xr_array_core_bytes_load_u64_le(a ? a->data : NULL, a ? a->length : 0,
                                                     a ? a->elem_type : XR_ELEM_ANY, off, &ok);
    if (!ok)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTES_LOAD_U64_OOB_MSG);
    return value;
}

static inline xrt_array_t *xrt_bytes_copy_within_raw(xrt_array_t *a, int64_t dst, int64_t src,
                                                     int64_t count) {
    if (!xr_array_core_bytes_copy_within(a ? a->data : NULL, a ? a->length : 0,
                                         a ? a->elem_type : XR_ELEM_ANY, dst, src, count))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTES_COPY_WITHIN_OOB_MSG);
    return a;
}

static inline xrt_array_t *xrt_bytes_copy_from_raw(xrt_array_t *dst, xrt_array_t *src,
                                                   int64_t src_offset, int64_t dst_offset,
                                                   int64_t count) {
    if (!xr_array_core_bytes_copy_from(dst ? dst->data : NULL, dst ? dst->length : 0,
                                       dst ? dst->elem_type : XR_ELEM_ANY, src ? src->data : NULL,
                                       src ? src->length : 0, src ? src->elem_type : XR_ELEM_ANY,
                                       src_offset, dst_offset, count, dst == src))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTES_COPY_FROM_OOB_MSG);
    return dst;
}

static inline xrt_array_t *xrt_bytes_repeat_from_raw(xrt_array_t *a, int64_t dst, int64_t distance,
                                                     int64_t count) {
    if (!xr_array_core_bytes_repeat_from(a ? a->data : NULL, a ? a->length : 0,
                                         a ? a->elem_type : XR_ELEM_ANY, dst, distance, count))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTES_REPEAT_FROM_OOB_MSG);
    return a;
}

static inline XrValue xrt_bytes_load_u32_le(XrValue arr_value, XrValue offset_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_LOAD_U32_RECEIVER_MSG);
    int64_t off = xrt_array_required_int_arg_or_panic(
        offset_value, "Bytes.loadU32LE(offset): offset must be integer");
    return XR_FROM_INT((int64_t) xrt_bytes_load_u32_le_raw((xrt_array_t *) arr_value.ptr, off));
}

static inline XrValue xrt_bytes_load_u64_le(XrValue arr_value, XrValue offset_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_LOAD_U64_RECEIVER_MSG);
    int64_t off = xrt_array_required_int_arg_or_panic(
        offset_value, "Bytes.loadU64LE(offset): offset must be integer");
    return XR_FROM_INT((int64_t) xrt_bytes_load_u64_le_raw((xrt_array_t *) arr_value.ptr, off));
}

static inline XrValue xrt_bytes_copy_within_value(XrValue arr_value, XrValue dst_value,
                                                  XrValue src_value, XrValue count_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr)
        return arr_value;
    int64_t dst = xrt_array_required_int_arg_or_panic(
        dst_value, "Bytes.copyWithin(dst, src, count): dst must be integer");
    int64_t src = xrt_array_required_int_arg_or_panic(
        src_value, "Bytes.copyWithin(dst, src, count): src must be integer");
    int64_t count = xrt_array_required_int_arg_or_panic(
        count_value, "Bytes.copyWithin(dst, src, count): count must be integer");
    xrt_bytes_copy_within_raw((xrt_array_t *) arr_value.ptr, dst, src, count);
    return arr_value;
}

static inline XrValue xrt_bytes_copy_from_value(XrValue dst_value, XrValue src_value,
                                                XrValue src_offset_value, XrValue dst_offset_value,
                                                XrValue count_value) {
    if (!XR_IS_ARRAY(dst_value) || !XR_IS_ARRAY(src_value) || !dst_value.ptr || !src_value.ptr)
        return dst_value;
    int64_t src_offset = xrt_array_required_int_arg_or_panic(
        src_offset_value,
        "Bytes.copyFrom(src, srcOffset, dstOffset, count): srcOffset must be integer");
    int64_t dst_offset = xrt_array_required_int_arg_or_panic(
        dst_offset_value,
        "Bytes.copyFrom(src, srcOffset, dstOffset, count): dstOffset must be integer");
    int64_t count = xrt_array_required_int_arg_or_panic(
        count_value, "Bytes.copyFrom(src, srcOffset, dstOffset, count): count must be integer");
    xrt_bytes_copy_from_raw((xrt_array_t *) dst_value.ptr, (xrt_array_t *) src_value.ptr,
                            src_offset, dst_offset, count);
    return dst_value;
}

static inline XrValue xrt_bytes_repeat_from_value(XrValue arr_value, XrValue dst_value,
                                                  XrValue distance_value, XrValue count_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr)
        return arr_value;
    int64_t dst = xrt_array_required_int_arg_or_panic(
        dst_value, "Bytes.repeatFrom(dst, distance, count): dst must be integer");
    int64_t distance = xrt_array_required_int_arg_or_panic(
        distance_value, "Bytes.repeatFrom(dst, distance, count): distance must be integer");
    int64_t count = xrt_array_required_int_arg_or_panic(
        count_value, "Bytes.repeatFrom(dst, distance, count): count must be integer");
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

static inline int xrt_array_fill_typed_fast(xrt_array_t *a, XrValue v, XrArrayCoreRange range) {
    return xr_array_core_fill_typed_storage(a ? a->data : NULL, range.start, range.count,
                                            a ? a->elem_type : XR_ELEM_ANY, v)
               ? 1
               : 0;
}

static inline XrValue xrt_array_fill_range_value(XrValue arr_value, XrValue fill_value,
                                                 int64_t start, int64_t end) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr)
        return arr_value;
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    XrArrayCoreRange range = xr_array_core_fill_range(a->length, start, end);
    if (xrt_array_fill_typed_fast(a, fill_value, range))
        return arr_value;
    for (int64_t i = range.start; i < range.end; i++)
        xr_typed_set(a->data, (int32_t) i, fill_value, a->elem_type);
    return arr_value;
}

static inline XrArrayCoreNeedle xrt_array_core_needle_from_value(XrValue v) {
    if (XR_IS_INT(v))
        return xr_array_core_needle_int(v.i);
    if (XR_IS_FLOAT(v))
        return xr_array_core_needle_float(v.f);
    if (XR_IS_BOOL(v))
        return xr_array_core_needle_bool(v.i != 0);
    return xr_array_core_needle_other();
}

/* Returns the match index, -1 for no match, or sets *handled = 0 when the
 * combination requires the generic boxed loop. */
static inline int64_t xrt_array_indexof_typed_fast(xrt_array_t *a, XrValue v, int *handled) {
    return xr_array_core_typed_index_of(a ? a->data : NULL, a ? a->length : 0,
                                        a ? a->elem_type : XR_ELEM_ANY,
                                        xrt_array_core_needle_from_value(v), handled);
}
