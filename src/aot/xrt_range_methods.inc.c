/* AOT Range method dispatch. */

static inline XrValue xrt_range_method_0(XrValue recv, int sym) {
    xrt_range_t *r = (xrt_range_t *) recv.ptr;
    if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
        return XR_FROM_INT(xrt_range_length_ptr(r));
    if (sym == XRT_SYM_IS_EMPTY)
        return XR_FROM_BOOL(xrt_range_length_ptr(r) == 0);
    if (sym == XRT_SYM_TOSTRING)
        return xrt_range_to_string(recv);
    if (sym == XRT_SYM_VALUES) {
        int64_t len = xrt_range_length_ptr(r);
        XrValue arr = xrt_array_new_typed(len, XR_ELEM_I64);
        xrt_array_t *a = (xrt_array_t *) arr.ptr;
        a->len = len;
        int64_t value = r ? r->start : 0;
        for (int64_t i = 0; i < len; i++) {
            xr_typed_set(a->data, (int32_t) i, XR_FROM_INT(value), a->elem_type);
            value += r->step;
        }
        return arr;
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_range_method_1(XrValue recv, int sym, XrValue arg0) {
    if ((sym == XRT_SYM_INCLUDES || sym == XRT_SYM_CONTAINS) && arg0.tag == XR_TAG_I64)
        return XR_FROM_BOOL(xrt_range_contains_ptr((xrt_range_t *) recv.ptr, arg0.i));
    return XR_NULL_VAL;
}
