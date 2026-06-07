/* AOT Array capacity and Bytes helpers. */

static inline XrValue xrt_array_new_typed_exact(int64_t cap, uint8_t etype) {
    if (cap < 0)
        cap = 0;
    xrt_array_t *a = (xrt_array_t *) XRT_MALLOC(sizeof(xrt_array_t));
    if (!a) {
        fprintf(stderr, "xrt_array_new_typed_exact: out of memory\n");
        abort();
    }
    a->len = 0;
    a->cap = cap;
    a->elem_type = etype;
    a->elem_size = XR_ELEM_SIZES[etype];
    a->is_slice = 0;
    a->data = cap > 0 ? XRT_CALLOC((size_t) cap, (size_t) a->elem_size) : NULL;
    if (cap > 0 && !a->data) {
        fprintf(stderr, "xrt_array_new_typed_exact: out of memory\n");
        abort();
    }
    return xr_mkptr(a, XR_TAG_ARRAY);
}

static inline void xrt_array_reserve_raw(xrt_array_t *a, int64_t cap) {
    if (!a || a->is_slice || cap <= a->cap)
        return;
    void *tmp = XRT_REALLOC(a->data, (size_t) cap * (size_t) a->elem_size);
    if (!tmp) {
        fprintf(stderr, "xrt_array_reserve: out of memory\n");
        abort();
    }
    if (cap > a->cap) {
        size_t old_bytes = (size_t) a->cap * (size_t) a->elem_size;
        size_t new_bytes = (size_t) cap * (size_t) a->elem_size;
        memset((uint8_t *) tmp + old_bytes, 0, new_bytes - old_bytes);
    }
    a->data = tmp;
    a->cap = cap;
}

static inline XrValue xrt_array_with_capacity_value(XrValue cap_value, uint8_t etype) {
    return xrt_array_new_typed_exact(xr_value_to_int64_coerce(cap_value), etype);
}

static inline XrValue xrt_array_resize_value(XrValue arr_value, XrValue len_value,
                                             XrValue fill_value) {
    if (arr_value.tag != XR_TAG_ARRAY || !arr_value.ptr)
        return arr_value;
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    int64_t len = xr_value_to_int64_coerce(len_value);
    if (len < 0)
        len = 0;
    if (len > a->cap)
        xrt_array_reserve_raw(a, len);
    for (int64_t i = a->len; i < len; i++)
        xr_typed_set(a->data, (int32_t) i, fill_value, a->elem_type);
    a->len = len;
    return arr_value;
}

static inline XrValue xrt_array_reserve_value(XrValue arr_value, XrValue cap_value) {
    if (arr_value.tag == XR_TAG_ARRAY && arr_value.ptr)
        xrt_array_reserve_raw((xrt_array_t *) arr_value.ptr, xr_value_to_int64_coerce(cap_value));
    return arr_value;
}

static inline XrValue xrt_array_new_filled_value(XrValue len_value, XrValue fill_value,
                                                 uint8_t etype) {
    int64_t len = xr_value_to_int64_coerce(len_value);
    if (len < 0)
        len = 0;
    XrValue arr = xrt_array_new_typed_exact(len, etype);
    xrt_array_t *a = (xrt_array_t *) arr.ptr;
    for (int64_t i = 0; i < len; i++)
        xr_typed_set(a->data, (int32_t) i, fill_value, a->elem_type);
    a->len = len;
    return arr;
}

static inline int xrt_bytes_range_ok(xrt_array_t *a, int64_t offset, int64_t count) {
    return a && a->elem_type == XR_ELEM_U8 && offset >= 0 && count >= 0 && offset + count <= a->len;
}

static inline XrValue xrt_bytes_load_u32_le(XrValue arr_value, XrValue offset_value) {
    if (arr_value.tag != XR_TAG_ARRAY || !arr_value.ptr)
        return XR_FROM_INT(0);
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    int64_t off = xr_value_to_int64_coerce(offset_value);
    if (!xrt_bytes_range_ok(a, off, 4))
        return XR_FROM_INT(0);
    const uint8_t *p = (const uint8_t *) a->data + off;
    uint32_t v = (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
                 ((uint32_t) p[3] << 24);
    return XR_FROM_INT((int64_t) v);
}

static inline XrValue xrt_bytes_load_u64_le(XrValue arr_value, XrValue offset_value) {
    if (arr_value.tag != XR_TAG_ARRAY || !arr_value.ptr)
        return XR_FROM_INT(0);
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    int64_t off = xr_value_to_int64_coerce(offset_value);
    if (!xrt_bytes_range_ok(a, off, 8))
        return XR_FROM_INT(0);
    const uint8_t *p = (const uint8_t *) a->data + off;
    uint64_t v = (uint64_t) p[0] | ((uint64_t) p[1] << 8) | ((uint64_t) p[2] << 16) |
                 ((uint64_t) p[3] << 24) | ((uint64_t) p[4] << 32) | ((uint64_t) p[5] << 40) |
                 ((uint64_t) p[6] << 48) | ((uint64_t) p[7] << 56);
    return XR_FROM_INT((int64_t) v);
}

static inline XrValue xrt_bytes_copy_within_value(XrValue arr_value, XrValue dst_value,
                                                  XrValue src_value, XrValue count_value) {
    if (arr_value.tag != XR_TAG_ARRAY || !arr_value.ptr)
        return arr_value;
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    int64_t dst = xr_value_to_int64_coerce(dst_value);
    int64_t src = xr_value_to_int64_coerce(src_value);
    int64_t count = xr_value_to_int64_coerce(count_value);
    if (xrt_bytes_range_ok(a, dst, count) && xrt_bytes_range_ok(a, src, count) && count > 0)
        memmove((uint8_t *) a->data + dst, (uint8_t *) a->data + src, (size_t) count);
    return arr_value;
}

static inline XrValue xrt_bytes_copy_from_value(XrValue dst_value, XrValue src_value,
                                                XrValue src_offset_value, XrValue dst_offset_value,
                                                XrValue count_value) {
    if (dst_value.tag != XR_TAG_ARRAY || src_value.tag != XR_TAG_ARRAY || !dst_value.ptr ||
        !src_value.ptr)
        return dst_value;
    xrt_array_t *dst = (xrt_array_t *) dst_value.ptr;
    xrt_array_t *src = (xrt_array_t *) src_value.ptr;
    int64_t src_offset = xr_value_to_int64_coerce(src_offset_value);
    int64_t dst_offset = xr_value_to_int64_coerce(dst_offset_value);
    int64_t count = xr_value_to_int64_coerce(count_value);
    if (!xrt_bytes_range_ok(src, src_offset, count) ||
        !xrt_bytes_range_ok(dst, dst_offset, count) || count <= 0)
        return dst_value;
    uint8_t *dp = (uint8_t *) dst->data + dst_offset;
    uint8_t *sp = (uint8_t *) src->data + src_offset;
    if (dst == src)
        memmove(dp, sp, (size_t) count);
    else
        memcpy(dp, sp, (size_t) count);
    return dst_value;
}

static inline XrValue xrt_bytes_repeat_from_value(XrValue arr_value, XrValue dst_value,
                                                  XrValue distance_value, XrValue count_value) {
    if (arr_value.tag != XR_TAG_ARRAY || !arr_value.ptr)
        return arr_value;
    xrt_array_t *a = (xrt_array_t *) arr_value.ptr;
    int64_t dst = xr_value_to_int64_coerce(dst_value);
    int64_t distance = xr_value_to_int64_coerce(distance_value);
    int64_t count = xr_value_to_int64_coerce(count_value);
    if (a->elem_type != XR_ELEM_U8 || dst < 0 || distance <= 0 || count < 0 || dst - distance < 0 ||
        dst + count > a->len)
        return arr_value;
    uint8_t *data = (uint8_t *) a->data;
    for (int64_t i = 0; i < count; i++)
        data[dst + i] = data[dst - distance + i];
    return arr_value;
}
