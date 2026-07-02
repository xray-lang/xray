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
        case XR_ELEM_CHAR:
            return XR_IS_CHAR(value) && XR_TO_CHAR(value) == 0;
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

static inline int xrt_bytes_range_ok(xrt_array_t *a, int64_t offset, int64_t count) {
    return a && a->elem_type == XR_ELEM_U8 && offset >= 0 && count >= 0 && count <= a->length &&
           offset <= a->length - count;
}

static inline int64_t xrt_ptr_load_u16_le_unchecked_raw(const void *ptr) {
    uint16_t value;
    memcpy(&value, ptr, sizeof(value));
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) &&                                    \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap16(value);
#endif
    return (int64_t) value;
}

static inline int64_t xrt_ptr_load_u32_le_unchecked_raw(const void *ptr) {
    uint32_t value;
    memcpy(&value, ptr, sizeof(value));
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) &&                                    \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap32(value);
#endif
    return (int64_t) value;
}

static inline int64_t xrt_ptr_load_u64_le_unchecked_raw(const void *ptr) {
    uint64_t value;
    memcpy(&value, ptr, sizeof(value));
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) &&                                    \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap64(value);
#endif
    return (int64_t) value;
}

static inline int64_t xrt_bytes_load_u16_le_unchecked_raw(xrt_array_t *a, int64_t off) {
    const uint8_t *p = (const uint8_t *) a->data + off;
    uint16_t value;
    memcpy(&value, p, sizeof(value));
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) &&                                    \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap16(value);
#endif
    return (int64_t) value;
}

static inline uint16_t xrt_bytes_load_u16_le_raw(xrt_array_t *a, int64_t off) {
    if (!xrt_bytes_range_ok(a, off, 2))
        return 0;
    return (uint16_t) xrt_bytes_load_u16_le_unchecked_raw(a, off);
}

static inline int64_t xrt_bytes_load_u16_le_checked_raw(xrt_array_t *a, int64_t off) {
    if (!a || a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_LOAD_U16_RECEIVER_MSG);
    if (!xrt_bytes_range_ok(a, off, 2))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTES_LOAD_U16_OOB_MSG);
    return (int64_t) xrt_bytes_load_u16_le_raw(a, off);
}

static inline int64_t xrt_bytes_load_u32_le_unchecked_raw(xrt_array_t *a, int64_t off) {
    const uint8_t *p = (const uint8_t *) a->data + off;
    uint32_t value;
    memcpy(&value, p, sizeof(value));
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) &&                                    \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap32(value);
#endif
    return (int64_t) value;
}

static inline uint32_t xrt_bytes_load_u32_le_raw(xrt_array_t *a, int64_t off) {
    if (!xrt_bytes_range_ok(a, off, 4))
        return 0;
    return (uint32_t) xrt_bytes_load_u32_le_unchecked_raw(a, off);
}

static inline int64_t xrt_bytes_load_u32_le_checked_raw(xrt_array_t *a, int64_t off) {
    if (!a || a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_LOAD_U32_RECEIVER_MSG);
    if (!xrt_bytes_range_ok(a, off, 4))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTES_LOAD_U32_OOB_MSG);
    return (int64_t) xrt_bytes_load_u32_le_raw(a, off);
}

static inline int64_t xrt_bytes_load_u64_le_unchecked_raw(xrt_array_t *a, int64_t off) {
    const uint8_t *p = (const uint8_t *) a->data + off;
    uint64_t value;
    memcpy(&value, p, sizeof(value));
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) &&                                    \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap64(value);
#endif
    return (int64_t) value;
}

static inline uint64_t xrt_bytes_load_u64_le_raw(xrt_array_t *a, int64_t off) {
    if (!xrt_bytes_range_ok(a, off, 8))
        return 0;
    return (uint64_t) xrt_bytes_load_u64_le_unchecked_raw(a, off);
}

static inline int64_t xrt_bytes_load_u64_le_checked_raw(xrt_array_t *a, int64_t off) {
    if (!a || a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_LOAD_U64_RECEIVER_MSG);
    if (!xrt_bytes_range_ok(a, off, 8))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTES_LOAD_U64_OOB_MSG);
    return (int64_t) xrt_bytes_load_u64_le_raw(a, off);
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
    xr_array_core_copy_or_move_bytes(dp, sp, count);
    return dst;
}

static inline xrt_array_t *xrt_bytes_repeat_from_raw(xrt_array_t *a, int64_t dst, int64_t distance,
                                                     int64_t count) {
    if (!a || a->elem_type != XR_ELEM_U8 || dst < 0 || distance <= 0 || count < 0 ||
        dst - distance < 0 || count > a->length || dst > a->length - count)
        return a;
    xr_array_core_bytes_repeat_copy(a->data, dst, distance, count);
    return a;
}

static inline xrt_array_t *xrt_bytes_append_from_unchecked_raw(xrt_array_t *dst, xrt_array_t *src,
                                                               int64_t src_offset, int64_t count) {
    if (!dst || !src || dst->elem_type != XR_ELEM_U8 || src->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTES_APPEND_FROM_UNCHECKED_OPERANDS_MSG);
    if (dst->data_storage == XR_ARRAY_DATA_BORROWED ||
        !xrt_bytes_range_ok(src, src_offset, count) || count < 0 ||
        count > dst->capacity - dst->length)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS,
                        XR_ERROR_CORE_BYTES_APPEND_FROM_UNCHECKED_OOB_MSG);
    if (count > 0) {
        uint8_t *dp = (uint8_t *) dst->data + dst->length;
        const uint8_t *sp = (const uint8_t *) src->data + src_offset;
        if (dst != src && dst->data_storage != XR_ARRAY_DATA_BORROWED &&
            src->data_storage != XR_ARRAY_DATA_BORROWED)
            xr_array_core_copy_nonoverlap_bytes(dp, sp, count);
        else
            xr_array_core_copy_or_move_bytes(dp, sp, count);
    }
    dst->length += count;
    return dst;
}

static inline xrt_array_t *xrt_bytes_append_from_unchecked_trusted_raw(xrt_array_t *dst,
                                                                       xrt_array_t *src,
                                                                       int64_t src_offset,
                                                                       int64_t count) {
    if (count > 0) {
        uint8_t *dp = (uint8_t *) dst->data + dst->length;
        const uint8_t *sp = (const uint8_t *) src->data + src_offset;
        if (dst != src && dst->data_storage != XR_ARRAY_DATA_BORROWED &&
            src->data_storage != XR_ARRAY_DATA_BORROWED)
            xr_array_core_copy_nonoverlap_bytes(dp, sp, count);
        else
            xr_array_core_copy_or_move_bytes(dp, sp, count);
    }
    dst->length += count;
    return dst;
}

static inline xrt_array_t *xrt_bytes_repeat_from_unchecked_raw(xrt_array_t *a, int64_t distance,
                                                               int64_t count) {
    if (!a || a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTES_REPEAT_FROM_UNCHECKED_RECEIVER_MSG);
    if (a->data_storage == XR_ARRAY_DATA_BORROWED || distance <= 0 || count < 0 ||
        distance > a->length || count > a->capacity - a->length)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS,
                        XR_ERROR_CORE_BYTES_REPEAT_FROM_UNCHECKED_OOB_MSG);
    int64_t dst = a->length;
    xr_array_core_bytes_repeat_copy(a->data, dst, distance, count);
    a->length += count;
    return a;
}

static inline xrt_array_t *
xrt_bytes_repeat_from_unchecked_trusted_raw(xrt_array_t *a, int64_t distance, int64_t count) {
    int64_t dst = a->length;
    xr_array_core_bytes_repeat_copy(a->data, dst, distance, count);
    a->length += count;
    return a;
}

static inline xrt_array_t *xrt_bytes_write_from_unchecked_raw(xrt_array_t *dst, int64_t dst_offset,
                                                              xrt_array_t *src, int64_t src_offset,
                                                              int64_t count) {
    if (!dst || !src || dst->elem_type != XR_ELEM_U8 || src->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTES_WRITE_FROM_UNCHECKED_OPERANDS_MSG);
    if (dst->data_storage == XR_ARRAY_DATA_BORROWED ||
        !xrt_bytes_range_ok(src, src_offset, count) || dst_offset < 0 || count < 0 ||
        count > dst->capacity - dst_offset)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS,
                        XR_ERROR_CORE_BYTES_WRITE_FROM_UNCHECKED_OOB_MSG);
    if (count > 0) {
        uint8_t *dp = (uint8_t *) dst->data + dst_offset;
        const uint8_t *sp = (const uint8_t *) src->data + src_offset;
        if (dst != src && dst->data_storage != XR_ARRAY_DATA_BORROWED &&
            src->data_storage != XR_ARRAY_DATA_BORROWED)
            xr_array_core_copy_nonoverlap_bytes(dp, sp, count);
        else
            xr_array_core_copy_or_move_bytes(dp, sp, count);
    }
    return dst;
}

static inline xrt_array_t *
xrt_bytes_write_from_unchecked_trusted_raw(xrt_array_t *dst, int64_t dst_offset, xrt_array_t *src,
                                           int64_t src_offset, int64_t count) {
    if (count > 0) {
        uint8_t *dp = (uint8_t *) dst->data + dst_offset;
        const uint8_t *sp = (const uint8_t *) src->data + src_offset;
        if (dst != src && dst->data_storage != XR_ARRAY_DATA_BORROWED &&
            src->data_storage != XR_ARRAY_DATA_BORROWED)
            xr_array_core_copy_nonoverlap_bytes(dp, sp, count);
        else
            xr_array_core_copy_or_move_bytes(dp, sp, count);
    }
    return dst;
}

static inline xrt_array_t *xrt_bytes_repeat_at_unchecked_raw(xrt_array_t *a, int64_t dst_offset,
                                                             int64_t distance, int64_t count) {
    if (!a || a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_REPEAT_AT_UNCHECKED_RECEIVER_MSG);
    if (a->data_storage == XR_ARRAY_DATA_BORROWED || dst_offset < 0 || distance <= 0 || count < 0 ||
        dst_offset - distance < 0 || count > a->capacity - dst_offset)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS,
                        XR_ERROR_CORE_BYTES_REPEAT_AT_UNCHECKED_OOB_MSG);
    xr_array_core_bytes_repeat_copy(a->data, dst_offset, distance, count);
    return a;
}

static inline xrt_array_t *xrt_bytes_repeat_at_unchecked_trusted_raw(xrt_array_t *a,
                                                                     int64_t dst_offset,
                                                                     int64_t distance,
                                                                     int64_t count) {
    xr_array_core_bytes_repeat_copy(a->data, dst_offset, distance, count);
    return a;
}

static inline xrt_array_t *xrt_bytes_wild_copy_from_nonoverlapping_unchecked_raw(
    xrt_array_t *dst, int64_t dst_offset, xrt_array_t *src, int64_t src_offset, int64_t count) {
    if (!dst || !src || dst->elem_type != XR_ELEM_U8 || src->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTES_WILD_COPY_FROM_NONOVERLAPPING_UNCHECKED_OPERANDS_MSG);
    int64_t src_limit = src->data_storage == XR_ARRAY_DATA_BORROWED ? src->length : src->capacity;
    if (dst->data_storage == XR_ARRAY_DATA_BORROWED || dst_offset < 0 || src_offset < 0 ||
        count < 0 || count > dst->capacity - dst_offset || count > src_limit - src_offset)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS,
                        XR_ERROR_CORE_BYTES_WILD_COPY_FROM_NONOVERLAPPING_UNCHECKED_OOB_MSG);
    if (count > 0) {
        uint8_t *dp = (uint8_t *) dst->data + dst_offset;
        const uint8_t *sp = (const uint8_t *) src->data + src_offset;
        xr_array_core_copy_nonoverlap_bytes(dp, sp, count);
    }
    return dst;
}

static inline xrt_array_t *xrt_bytes_wild_copy_from_nonoverlapping_unchecked_trusted_raw(
    xrt_array_t *dst, int64_t dst_offset, xrt_array_t *src, int64_t src_offset, int64_t count) {
    if (count > 0) {
        uint8_t *dp = (uint8_t *) dst->data + dst_offset;
        const uint8_t *sp = (const uint8_t *) src->data + src_offset;
        memcpy(dp, sp, (size_t) count);
    }
    return dst;
}

static inline xrt_array_t *xrt_bytes_wild_copy_16_nonoverlap_trusted_raw(xrt_array_t *dst,
                                                                         int64_t dst_offset,
                                                                         xrt_array_t *src,
                                                                         int64_t src_offset) {
    uint8_t *dp = (uint8_t *) dst->data + dst_offset;
    const uint8_t *sp = (const uint8_t *) src->data + src_offset;
    memcpy(dp, sp, (size_t) 16);
    return dst;
}

static inline xrt_array_t *xrt_bytes_wild_copy_96_nonoverlap_trusted_raw(xrt_array_t *dst,
                                                                         int64_t dst_offset,
                                                                         xrt_array_t *src,
                                                                         int64_t src_offset) {
    uint8_t *dp = (uint8_t *) dst->data + dst_offset;
    const uint8_t *sp = (const uint8_t *) src->data + src_offset;
    memcpy(dp, sp, (size_t) 64);
    memcpy(dp + 64, sp + 64, (size_t) 32);
    return dst;
}

static inline xrt_array_t *xrt_bytes_wild_copy_104_nonoverlap_trusted_raw(xrt_array_t *dst,
                                                                          int64_t dst_offset,
                                                                          xrt_array_t *src,
                                                                          int64_t src_offset) {
    uint8_t *dp = (uint8_t *) dst->data + dst_offset;
    const uint8_t *sp = (const uint8_t *) src->data + src_offset;
    memcpy(dp, sp, (size_t) 64);
    memcpy(dp + 64, sp + 64, (size_t) 32);
    memcpy(dp + 96, sp + 96, (size_t) 8);
    return dst;
}

static inline xrt_array_t *xrt_bytes_wild_copy_112_nonoverlap_trusted_raw(xrt_array_t *dst,
                                                                          int64_t dst_offset,
                                                                          xrt_array_t *src,
                                                                          int64_t src_offset) {
    uint8_t *dp = (uint8_t *) dst->data + dst_offset;
    const uint8_t *sp = (const uint8_t *) src->data + src_offset;
    memcpy(dp, sp, (size_t) 64);
    memcpy(dp + 64, sp + 64, (size_t) 32);
    memcpy(dp + 96, sp + 96, (size_t) 16);
    return dst;
}

static inline xrt_array_t *xrt_bytes_wild_repeat_at_unchecked_raw(xrt_array_t *a,
                                                                  int64_t dst_offset,
                                                                  int64_t distance, int64_t count) {
    if (!a || a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTES_WILD_REPEAT_AT_UNCHECKED_RECEIVER_MSG);
    if (a->data_storage == XR_ARRAY_DATA_BORROWED || dst_offset < 0 || distance <= 0 || count < 0 ||
        dst_offset - distance < 0 || count > a->capacity - dst_offset)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS,
                        XR_ERROR_CORE_BYTES_WILD_REPEAT_AT_UNCHECKED_OOB_MSG);
    xr_array_core_bytes_repeat_copy(a->data, dst_offset, distance, count);
    return a;
}

static inline xrt_array_t *xrt_bytes_wild_repeat_at_unchecked_trusted_raw(xrt_array_t *a,
                                                                          int64_t dst_offset,
                                                                          int64_t distance,
                                                                          int64_t count) {
    xr_array_core_bytes_repeat_copy(a->data, dst_offset, distance, count);
    return a;
}

static inline xrt_array_t *xrt_bytes_wild_repeat18_trusted_raw(xrt_array_t *a, int64_t dst_offset,
                                                               int64_t distance) {
    uint8_t *dp = (uint8_t *) a->data + dst_offset;
    if (distance >= 8) {
        const uint8_t *sp = dp - distance;
        memcpy(dp, sp, (size_t) 8);
        memcpy(dp + 8, sp + 8, (size_t) 8);
        memcpy(dp + 16, sp + 16, (size_t) 2);
    } else {
        xr_array_core_bytes_repeat_copy(a->data, dst_offset, distance, 18);
    }
    return a;
}

static inline xrt_array_t *
xrt_bytes_wild_repeat18_ge8_trusted_raw(xrt_array_t *a, int64_t dst_offset, int64_t distance) {
    uint8_t *dp = (uint8_t *) a->data + dst_offset;
    const uint8_t *sp = dp - distance;
    memcpy(dp, sp, (size_t) 8);
    memcpy(dp + 8, sp + 8, (size_t) 8);
    memcpy(dp + 16, sp + 16, (size_t) 2);
    return a;
}

static inline xrt_array_t *xrt_bytes_wild_repeat4_96_trusted_raw(xrt_array_t *a,
                                                                 int64_t dst_offset) {
    uint8_t *dp = (uint8_t *) a->data + dst_offset;
    const uint8_t *sp = dp - 4;
    memcpy(dp, sp, (size_t) 4);
    memcpy(dp + 4, sp, (size_t) 4);
    memcpy(dp + 8, dp, (size_t) 8);
    memcpy(dp + 16, dp, (size_t) 16);
    memcpy(dp + 32, dp, (size_t) 32);
    memcpy(dp + 64, dp, (size_t) 32);
    return a;
}

static inline xrt_array_t *xrt_bytes_set_length_unchecked_raw(xrt_array_t *a, int64_t length) {
    if (!a || a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTES_SET_LENGTH_UNCHECKED_RECEIVER_MSG);
    if (a->data_storage == XR_ARRAY_DATA_BORROWED || length < 0 || length > a->capacity)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS,
                        XR_ERROR_CORE_BYTES_SET_LENGTH_UNCHECKED_OOB_MSG);
    a->length = length;
    return a;
}

static inline xrt_array_t *xrt_bytes_set_length_unchecked_trusted_raw(xrt_array_t *a,
                                                                      int64_t length) {
    a->length = length;
    return a;
}

static inline int xrt_common_prefix_diff_byte64(uint64_t diff) {
#if defined(__GNUC__) || defined(__clang__)
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) &&                                    \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_clzll(diff) >> 3;
#else
    return __builtin_ctzll(diff) >> 3;
#endif
#else
    int n = 0;
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) &&                                    \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    while (((diff >> ((7 - n) * 8)) & UINT64_C(0xFF)) == 0)
        n++;
#else
    while (((diff >> (n * 8)) & UINT64_C(0xFF)) == 0)
        n++;
#endif
    return n;
#endif
}

static inline int64_t xrt_bytes_common_prefix_unchecked_raw(xrt_array_t *a, int64_t left,
                                                            int64_t right, int64_t max_count) {
    if (!a || a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        "ByteSpan.commonPrefixUnchecked receiver must be ByteSpan");
    if (left < 0 || right < 0 || max_count < 0 || max_count > a->length - left ||
        max_count > a->length - right)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS,
                        "ByteSpan.commonPrefixUnchecked range out of bounds");

    const uint8_t *lp = (const uint8_t *) a->data + left;
    const uint8_t *rp = (const uint8_t *) a->data + right;
    int64_t i = 0;
    while (i + 8 <= max_count) {
        uint64_t lv;
        uint64_t rv;
        memcpy(&lv, lp + i, sizeof(lv));
        memcpy(&rv, rp + i, sizeof(rv));
        uint64_t diff = lv ^ rv;
        if (diff)
            return i + xrt_common_prefix_diff_byte64(diff);
        i += 8;
    }
    while (i < max_count && lp[i] == rp[i])
        i++;
    return i;
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

static inline XrValue xrt_bytes_load_u16_le(XrValue arr_value, XrValue offset_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr || !XR_IS_INT(offset_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_LOAD_U16_EXPECTS_MSG);
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    if (a->elem_type != XR_ELEM_U8)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_LOAD_U16_RECEIVER_MSG);
    int64_t off = XR_TO_INT(offset_value);
    if (!xrt_bytes_range_ok(a, off, 2))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTES_LOAD_U16_OOB_MSG);
    return XR_FROM_INT((int64_t) xrt_bytes_load_u16_le_raw(a, off));
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

static inline XrValue xrt_bytes_append_from_unchecked_value(XrValue dst_value, XrValue src_value,
                                                            XrValue src_offset_value,
                                                            XrValue count_value) {
    if (!XR_IS_ARRAY(dst_value) || !XR_IS_ARRAY(src_value) || !dst_value.ptr || !src_value.ptr ||
        !XR_IS_INT(src_offset_value) || !XR_IS_INT(count_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTES_APPEND_FROM_UNCHECKED_EXPECTS_MSG);
    xrt_bytes_append_from_unchecked_raw((xrt_array_t *) dst_value.ptr,
                                        (xrt_array_t *) src_value.ptr, XR_TO_INT(src_offset_value),
                                        XR_TO_INT(count_value));
    return dst_value;
}

static inline XrValue xrt_bytes_repeat_from_unchecked_value(XrValue arr_value,
                                                            XrValue distance_value,
                                                            XrValue count_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr || !XR_IS_INT(distance_value) ||
        !XR_IS_INT(count_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTES_REPEAT_FROM_UNCHECKED_EXPECTS_MSG);
    xrt_bytes_repeat_from_unchecked_raw((xrt_array_t *) arr_value.ptr, XR_TO_INT(distance_value),
                                        XR_TO_INT(count_value));
    return arr_value;
}

static inline XrValue
xrt_bytes_write_from_unchecked_value(XrValue dst_value, XrValue dst_offset_value, XrValue src_value,
                                     XrValue src_offset_value, XrValue count_value) {
    if (!XR_IS_ARRAY(dst_value) || !XR_IS_ARRAY(src_value) || !dst_value.ptr || !src_value.ptr ||
        !XR_IS_INT(dst_offset_value) || !XR_IS_INT(src_offset_value) || !XR_IS_INT(count_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_WRITE_FROM_UNCHECKED_EXPECTS_MSG);
    xrt_bytes_write_from_unchecked_raw((xrt_array_t *) dst_value.ptr, XR_TO_INT(dst_offset_value),
                                       (xrt_array_t *) src_value.ptr, XR_TO_INT(src_offset_value),
                                       XR_TO_INT(count_value));
    return dst_value;
}

static inline XrValue xrt_bytes_repeat_at_unchecked_value(XrValue arr_value,
                                                          XrValue dst_offset_value,
                                                          XrValue distance_value,
                                                          XrValue count_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr || !XR_IS_INT(dst_offset_value) ||
        !XR_IS_INT(distance_value) || !XR_IS_INT(count_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_REPEAT_AT_UNCHECKED_EXPECTS_MSG);
    xrt_bytes_repeat_at_unchecked_raw((xrt_array_t *) arr_value.ptr, XR_TO_INT(dst_offset_value),
                                      XR_TO_INT(distance_value), XR_TO_INT(count_value));
    return arr_value;
}

static inline XrValue
xrt_bytes_wild_copy_from_nonoverlapping_unchecked_value(XrValue dst_value, XrValue dst_offset_value,
                                                        XrValue src_value, XrValue src_offset_value,
                                                        XrValue count_value) {
    if (!XR_IS_ARRAY(dst_value) || !XR_IS_ARRAY(src_value) || !dst_value.ptr || !src_value.ptr ||
        !XR_IS_INT(dst_offset_value) || !XR_IS_INT(src_offset_value) || !XR_IS_INT(count_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTES_WILD_COPY_FROM_NONOVERLAPPING_UNCHECKED_EXPECTS_MSG);
    xrt_bytes_wild_copy_from_nonoverlapping_unchecked_raw(
        (xrt_array_t *) dst_value.ptr, XR_TO_INT(dst_offset_value), (xrt_array_t *) src_value.ptr,
        XR_TO_INT(src_offset_value), XR_TO_INT(count_value));
    return dst_value;
}

static inline XrValue xrt_bytes_wild_repeat_at_unchecked_value(XrValue arr_value,
                                                               XrValue dst_offset_value,
                                                               XrValue distance_value,
                                                               XrValue count_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr || !XR_IS_INT(dst_offset_value) ||
        !XR_IS_INT(distance_value) || !XR_IS_INT(count_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTES_WILD_REPEAT_AT_UNCHECKED_EXPECTS_MSG);
    xrt_bytes_wild_repeat_at_unchecked_raw((xrt_array_t *) arr_value.ptr,
                                           XR_TO_INT(dst_offset_value), XR_TO_INT(distance_value),
                                           XR_TO_INT(count_value));
    return arr_value;
}

static inline XrValue xrt_bytes_set_length_unchecked_value(XrValue arr_value,
                                                           XrValue length_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr || !XR_IS_INT(length_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_SET_LENGTH_UNCHECKED_EXPECTS_MSG);
    xrt_bytes_set_length_unchecked_raw((xrt_array_t *) arr_value.ptr, XR_TO_INT(length_value));
    return arr_value;
}

static inline XrValue xrt_bytes_common_prefix_unchecked_value(XrValue arr_value, XrValue left_value,
                                                              XrValue right_value,
                                                              XrValue max_count_value) {
    if (!XR_IS_ARRAY(arr_value) || !arr_value.ptr || !XR_IS_INT(left_value) ||
        !XR_IS_INT(right_value) || !XR_IS_INT(max_count_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        "ByteSpan.commonPrefixUnchecked expects integer ranges");
    return XR_FROM_INT(
        xrt_bytes_common_prefix_unchecked_raw((xrt_array_t *) arr_value.ptr, XR_TO_INT(left_value),
                                              XR_TO_INT(right_value), XR_TO_INT(max_count_value)));
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
