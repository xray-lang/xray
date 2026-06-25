/* Fixed native arrays and dynamic index access helpers. */

/* Out-of-bounds array index throws E0430 (spec §3 expr-index-access), matching
 * the VM's XR_ERR_INDEX_OUT_OF_BOUNDS. An `int` index outside [0, length) —
 * including any negative index — is a fault: there is no from-end wraparound.
 * Defined in xrt_exception.h (pulled in ahead of this header via xrt_arith.h in
 * the full runtime); forward-declared here so core-library TUs can link against
 * the generated program's XRT_IMPL exception entry when a throwing path is
 * reachable. */
XRT_COLD _Noreturn void xrt_index_oob(int64_t idx, int64_t length);

static inline XrValue xrt_fixed_array_get(void *base, uint8_t native_type, int64_t idx) {
    uint8_t *p = (uint8_t *) base + (size_t) idx * xr_native_type_size(native_type);
    switch (native_type) {
        case XR_NATIVE_F32:
            return XR_FROM_FLOAT((double) *(float *) p);
        case XR_NATIVE_F64:
            return XR_FROM_FLOAT(*(double *) p);
        case XR_NATIVE_BOOL:
            return *(uint8_t *) p ? XR_TRUE_VAL : XR_FALSE_VAL;
        case XR_NATIVE_I8:
            return XR_FROM_INT((int64_t) *(int8_t *) p);
        case XR_NATIVE_I16:
            return XR_FROM_INT((int64_t) *(int16_t *) p);
        case XR_NATIVE_I32:
            return XR_FROM_INT((int64_t) *(int32_t *) p);
        case XR_NATIVE_U8:
            return XR_FROM_INT((int64_t) *(uint8_t *) p);
        case XR_NATIVE_U16:
            return XR_FROM_INT((int64_t) *(uint16_t *) p);
        case XR_NATIVE_U32:
            return XR_FROM_INT((int64_t) *(uint32_t *) p);
        case XR_NATIVE_U64:
            return XR_FROM_INT((int64_t) *(uint64_t *) p);
        default:
            return XR_FROM_INT(*(int64_t *) p);
    }
}

static inline void xrt_fixed_array_set(void *base, uint8_t native_type, int64_t idx,
                                       XrValue value) {
    uint8_t *p = (uint8_t *) base + (size_t) idx * xr_native_type_size(native_type);
    switch (native_type) {
        case XR_NATIVE_F32:
            *(float *) p = (float) xr_value_to_f64_coerce(value);
            break;
        case XR_NATIVE_F64:
            *(double *) p = xr_value_to_f64_coerce(value);
            break;
        case XR_NATIVE_BOOL:
            *(uint8_t *) p = (uint8_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_I8:
            *(int8_t *) p = (int8_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_I16:
            *(int16_t *) p = (int16_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_I32:
            *(int32_t *) p = (int32_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_U8:
            *(uint8_t *) p = (uint8_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_U16:
            *(uint16_t *) p = (uint16_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_U32:
            *(uint32_t *) p = (uint32_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_U64:
            *(uint64_t *) p = (uint64_t) xr_value_to_int64_coerce(value);
            break;
        default:
            *(int64_t *) p = xr_value_to_int64_coerce(value);
            break;
    }
}

static inline void xrt_fixed_array_copy(void *dst, XrValue src, uint8_t native_type,
                                        uint16_t elem_count) {
    size_t elem_size = xr_native_type_size(native_type);
    int64_t count = 0;
    if (XR_IS_ARRAY_REF(src)) {
        uint16_t src_count = XR_ARRAY_REF_ELEM_COUNT(src);
        count = src_count < elem_count ? src_count : elem_count;
        if (XR_ARRAY_REF_ELEM_TYPE(src) == native_type) {
            memcpy(dst, src.ptr, (size_t) count * elem_size);
        } else {
            for (int64_t i = 0; i < count; i++)
                xrt_fixed_array_set(dst, native_type, i,
                                    xrt_fixed_array_get(src.ptr, XR_ARRAY_REF_ELEM_TYPE(src), i));
        }
    } else if (XR_IS_ARRAY(src) && src.ptr) {
        xrt_array_t *arr = (xrt_array_t *) src.ptr;
        count = arr->length < elem_count ? arr->length : elem_count;
        for (int64_t i = 0; i < count; i++)
            xrt_fixed_array_set(dst, native_type, i,
                                xr_typed_get(arr->data, (int32_t) i, arr->elem_type));
    }
    if (count < elem_count)
        memset((uint8_t *) dst + (size_t) count * elem_size, 0,
               (size_t) (elem_count - count) * elem_size);
}

static inline XrValue xrt_string_index_get(XrValue obj, int64_t target) {
    if (!XR_IS_STR(obj))
        return XR_NULL_VAL;
    XrStringCoreSlice slice =
        xr_string_core_utf8_index_slice_at(xr_str_data(obj), (size_t) xr_str_len(obj), target);
    if (slice.len == 0)
        return XR_NULL_VAL;
    XrValue sv = xrt_str_alloc(slice.len);
    memcpy(xr_str_buf(sv), slice.data, slice.len);
    return sv;
}

static inline XrValue xrt_index_get(XrValue obj, XrValue key) {
    if (XR_IS_ARRAY_REF(obj) && key.tag == XR_TAG_I64) {
        int64_t idx = key.i;
        uint16_t count = XR_ARRAY_REF_ELEM_COUNT(obj);
        if (XR_LIKELY(idx >= 0 && idx < count))
            return xrt_fixed_array_get(obj.ptr, XR_ARRAY_REF_ELEM_TYPE(obj), idx);
        xrt_index_oob(idx, count);
    }
    if (XR_IS_ARRAY(obj) && key.tag == XR_TAG_I64) {
        xrt_array_t *a = (xrt_array_t *) obj.ptr;
        int64_t idx = key.i;
        if (XR_LIKELY(idx >= 0 && idx < a->length))
            return xr_typed_get(a->data, (int32_t) idx, a->elem_type);
        xrt_index_oob(idx, a->length);
    } else if (XR_IS_STR(obj) && key.tag == XR_TAG_I64) {
        return xrt_string_index_get(obj, key.i);
    } else if (obj.tag == XR_TAG_RANGE && key.tag == XR_TAG_I64) {
        bool ok = false;
        int64_t value = xrt_range_index_ptr((const xrt_range_t *) obj.ptr, key.i, &ok);
        return ok ? XR_FROM_INT(value) : XR_NULL_VAL;
    } else if (XR_IS_MAP(obj)) {
        return xrt_map_get((xrt_map_t *) obj.ptr, key);
    } else if (XR_IS_SET(obj) && key.tag == XR_TAG_I64) {
        // Positional access into the set's insertion order (used by for-in).
        xrt_set_t *s = (xrt_set_t *) obj.ptr;
        return xrt_set_value_at(s, key.i);
    }
    return XR_NULL_VAL;
}

static inline void xrt_index_set(XrValue obj, XrValue key, XrValue val) {
    if (XR_IS_ARRAY_REF(obj) && key.tag == XR_TAG_I64) {
        int64_t idx = key.i;
        uint16_t count = XR_ARRAY_REF_ELEM_COUNT(obj);
        if (XR_LIKELY(idx >= 0 && idx < count)) {
            xrt_fixed_array_set(obj.ptr, XR_ARRAY_REF_ELEM_TYPE(obj), idx, val);
            return;
        }
        fprintf(stderr, "fixed array index out of range: %lld (length %u)\n", (long long) idx,
                (unsigned) count);
        abort();
    }
    if (XR_IS_ARRAY(obj) && key.tag == XR_TAG_I64) {
        xrt_array_t *a = (xrt_array_t *) obj.ptr;
        XrArrayCoreIndexSetPlan plan = xr_array_core_index_set_plan(a->length, key.i);
        if (XR_LIKELY(plan.kind == XR_ARRAY_CORE_INDEX_SET_WRITE)) {
            xr_typed_set(a->data, (int32_t) plan.index, val, a->elem_type);
        } else {
            xrt_index_oob(key.i, a->length);
        }
    } else if (XR_IS_MAP(obj)) {
        xrt_map_set((xrt_map_t *) obj.ptr, key, val);
    }
}
