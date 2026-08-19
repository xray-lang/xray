/* Inline property access for known AOT values. */

static inline XrValue xrt_getprop(XrValue obj, int64_t symbol_id) {
    if (XR_IS_ARRAY(obj)) {
        xrt_array_t *a = (xrt_array_t *) obj.ptr;
        if (symbol_id == XRT_SYM_LENGTH || symbol_id == XRT_SYM_SIZE)
            return XR_FROM_INT(a->length);
        if (symbol_id == XRT_SYM_CAPACITY)
            return XR_FROM_INT(a->capacity);
        if (symbol_id == XRT_SYM_IS_EMPTY)
            return XR_FROM_INT(a->length == 0);
    }
    if (XR_IS_MAP(obj)) {
        xrt_map_t *m = (xrt_map_t *) obj.ptr;
        if (symbol_id == XRT_SYM_LENGTH || symbol_id == XRT_SYM_SIZE)
            return XR_FROM_INT(xrt_map_len(m));
        if (symbol_id == XRT_SYM_IS_EMPTY)
            return XR_FROM_INT(xrt_map_len(m) == 0);
    }
    if (XR_IS_SET(obj)) {
        xrt_set_t *s = (xrt_set_t *) obj.ptr;
        if (symbol_id == XRT_SYM_LENGTH || symbol_id == XRT_SYM_SIZE)
            return XR_FROM_INT(xrt_set_len(s));
        if (symbol_id == XRT_SYM_IS_EMPTY)
            return XR_FROM_INT(xrt_set_len(s) == 0);
    }
    if (XR_IS_STR(obj)) {
        if (symbol_id == XRT_SYM_LENGTH || symbol_id == XRT_SYM_SIZE)
            /* Code points, not the byte length sitting right there -- the
             * kernel names which count this is, and the VM reads the same
             * answer from the same place. */
            return XR_FROM_INT(xr_length_source_counts_runes_core(XR_LENGTH_SOURCE_STRING_RUNES)
                                   ? xrt_utf8_scalar_count(xr_str_data(obj), xr_str_len(obj))
                                   : (int64_t) xr_str_len(obj));
        if (symbol_id == XRT_SYM_IS_EMPTY)
            return XR_FROM_INT(xr_str_len(obj) == 0);
    }
    if (obj.tag == XR_TAG_STRBUF) {
        xrt_strbuf_t *sb = (xrt_strbuf_t *) obj.ptr;
        if (symbol_id == XRT_SYM_LENGTH || symbol_id == XRT_SYM_SIZE)
            return XR_FROM_INT(sb ? sb->len : 0);
        if (symbol_id == XRT_SYM_IS_EMPTY)
            return XR_FROM_INT(!sb || sb->len == 0);
    }
    if (obj.tag == XR_TAG_RANGE) {
        xrt_range_t *r = (xrt_range_t *) obj.ptr;
        if (symbol_id == XRT_SYM_LENGTH || symbol_id == XRT_SYM_SIZE)
            return XR_FROM_INT(xrt_range_length_ptr(r));
        if (symbol_id == XRT_SYM_IS_EMPTY)
            return XR_FROM_BOOL(xrt_range_length_ptr(r) == 0);
    }
    if (obj.tag == XR_TAG_BUFFER)
        return xrt_buffer_method_0(obj, (int) symbol_id);
    return XR_NULL_VAL;
}
