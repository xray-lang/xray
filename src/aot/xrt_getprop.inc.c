/* Inline property access for known AOT values. */

static inline XrValue xrt_getprop(XrValue obj, int64_t symbol_id) {
    if (obj.tag == XR_TAG_ARRAY) {
        xrt_array_t *a = (xrt_array_t *) obj.ptr;
        if (symbol_id == XRT_SYM_LENGTH || symbol_id == XRT_SYM_SIZE)
            return XR_FROM_INT(a->len);
        if (symbol_id == XRT_SYM_CAPACITY)
            return XR_FROM_INT(a->cap);
        if (symbol_id == XRT_SYM_IS_EMPTY)
            return XR_FROM_INT(a->len == 0);
    }
    if (obj.tag == XR_TAG_MAP) {
        xrt_map_t *m = (xrt_map_t *) obj.ptr;
        if (symbol_id == XRT_SYM_LENGTH || symbol_id == XRT_SYM_SIZE)
            return XR_FROM_INT(m->len);
        if (symbol_id == XRT_SYM_IS_EMPTY)
            return XR_FROM_INT(m->len == 0);
    }
    if (obj.tag == XR_TAG_SET) {
        xrt_set_t *s = (xrt_set_t *) obj.ptr;
        if (symbol_id == XRT_SYM_LENGTH || symbol_id == XRT_SYM_SIZE)
            return XR_FROM_INT(s->len);
        if (symbol_id == XRT_SYM_IS_EMPTY)
            return XR_FROM_INT(s->len == 0);
    }
    if (XR_IS_STR(obj)) {
        const char *s = (const char *) obj.ptr;
        if (symbol_id == XRT_SYM_LENGTH || symbol_id == XRT_SYM_SIZE)
            return XR_FROM_INT((int64_t) strlen(s));
        if (symbol_id == XRT_SYM_IS_EMPTY)
            return XR_FROM_INT(s[0] == '\0');
    }
    if (obj.tag == XR_TAG_STRBUF) {
        xrt_strbuf_t *sb = (xrt_strbuf_t *) obj.ptr;
        if (symbol_id == XRT_SYM_LENGTH || symbol_id == XRT_SYM_SIZE)
            return XR_FROM_INT(sb ? sb->len : 0);
        if (symbol_id == XRT_SYM_IS_EMPTY)
            return XR_FROM_INT(!sb || sb->len == 0);
    }
    return XR_NULL_VAL;
}
