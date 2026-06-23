/* AOT Range method dispatch. */

static inline XrValue xrt_range_method_0(XrValue recv, int sym) {
    xrt_range_t *r = (xrt_range_t *) recv.ptr;
    if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
        return XR_FROM_INT(xrt_range_length_ptr(r));
    if (sym == XRT_SYM_IS_EMPTY)
        return XR_FROM_BOOL(xrt_range_length_ptr(r) == 0);
    if (sym == XRT_SYM_TOSTRING)
        return xrt_range_to_string(recv);
    if (sym == XRT_SYM_VALUES || sym == XRT_SYM_TO_ARRAY) {
        XrRangeCore core =
            r ? xr_range_core_make(r->start, r->end, r->step) : xr_range_core_make(0, 0, 1);
        XrRangeCoreMaterializePlan plan = xr_range_core_materialize_plan(core);
        if (plan.kind == XR_RANGE_CORE_MATERIALIZE_TOO_LARGE) {
            fprintf(stderr, "range_to_array: range too large\n");
            abort();
        }
        XrValue arr = xrt_array_new_typed_uninit(plan.length, XR_ELEM_I64);
        xrt_array_t *a = (xrt_array_t *) arr.ptr;
        a->length = plan.length;
        int64_t *data = (int64_t *) a->data;
        for (int64_t i = 0; i < plan.length; i++)
            data[i] = xr_range_core_value_at(core, i);
        return arr;
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_range_method_1(XrValue recv, int sym, XrValue arg0) {
    if ((sym == XRT_SYM_INCLUDES || sym == XRT_SYM_CONTAINS) && arg0.tag == XR_TAG_I64)
        return XR_FROM_BOOL(xrt_range_contains_ptr((xrt_range_t *) recv.ptr, arg0.i));
    return XR_NULL_VAL;
}
