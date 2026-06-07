/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_set_direct.inc.c - Direct typed Set helpers for AOT class-native fields.
 */

#ifndef XRT_SET_DIRECT_INC
#define XRT_SET_DIRECT_INC

static inline void xrt_set_direct_type_mismatch(uint8_t expected, uint8_t actual, const char *who) {
    fprintf(stderr, "%s: set element type mismatch (%u != %u)\n", who ? who : "xrt_set_direct",
            (unsigned) expected, (unsigned) actual);
    abort();
}

static inline void xrt_set_direct_unsupported(uint8_t elem_type, const char *who) {
    fprintf(stderr, "%s: unsupported set element type %u\n", who ? who : "xrt_set_direct",
            (unsigned) elem_type);
    abort();
}

#define XRT_SET_I64_HAS_CASE(elem, ctype, expr)                                                    \
    case elem: {                                                                                   \
        ctype needle = (ctype) (expr);                                                             \
        ctype *items = (ctype *) s->items;                                                         \
        for (int64_t i = 0; i < s->len; i++) {                                                     \
            if (items[i] == needle)                                                                \
                return 1;                                                                          \
        }                                                                                          \
        return 0;                                                                                  \
    }

#define XRT_SET_I64_ADD_CASE(elem, ctype, expr)                                                    \
    case elem:                                                                                     \
        ((ctype *) s->items)[s->len++] = (ctype) (expr);                                           \
        return 1

#define XRT_SET_I64_DELETE_CASE(elem, ctype, expr)                                                 \
    case elem: {                                                                                   \
        ctype needle = (ctype) (expr);                                                             \
        ctype *items = (ctype *) s->items;                                                         \
        for (int64_t i = 0; i < s->len; i++) {                                                     \
            if (items[i] == needle) {                                                              \
                items[i] = items[--s->len];                                                        \
                return 1;                                                                          \
            }                                                                                      \
        }                                                                                          \
        return 0;                                                                                  \
    }

#define XRT_SET_F64_HAS_CASE(elem, ctype, expr)                                                    \
    case elem: {                                                                                   \
        ctype needle = (ctype) (expr);                                                             \
        ctype *items = (ctype *) s->items;                                                         \
        for (int64_t i = 0; i < s->len; i++) {                                                     \
            if (items[i] == needle)                                                                \
                return 1;                                                                          \
        }                                                                                          \
        return 0;                                                                                  \
    }

#define XRT_SET_F64_ADD_CASE(elem, ctype, expr)                                                    \
    case elem:                                                                                     \
        ((ctype *) s->items)[s->len++] = (ctype) (expr);                                           \
        return 1

#define XRT_SET_F64_DELETE_CASE(elem, ctype, expr)                                                 \
    case elem: {                                                                                   \
        ctype needle = (ctype) (expr);                                                             \
        ctype *items = (ctype *) s->items;                                                         \
        for (int64_t i = 0; i < s->len; i++) {                                                     \
            if (items[i] == needle) {                                                              \
                items[i] = items[--s->len];                                                        \
                return 1;                                                                          \
            }                                                                                      \
        }                                                                                          \
        return 0;                                                                                  \
    }

static inline int xrt_set_has_i64_typed(xrt_set_t *s, int64_t value, uint8_t elem_type) {
    if (s->elem_type != elem_type)
        xrt_set_direct_type_mismatch(elem_type, s->elem_type, "xrt_set_has_i64_typed");
    switch (elem_type) {
        XRT_SET_I64_HAS_CASE(XR_ELEM_I8, int8_t, value);
        XRT_SET_I64_HAS_CASE(XR_ELEM_U8, uint8_t, value);
        XRT_SET_I64_HAS_CASE(XR_ELEM_I16, int16_t, value);
        XRT_SET_I64_HAS_CASE(XR_ELEM_U16, uint16_t, value);
        XRT_SET_I64_HAS_CASE(XR_ELEM_I32, int32_t, value);
        XRT_SET_I64_HAS_CASE(XR_ELEM_U32, uint32_t, value);
        XRT_SET_I64_HAS_CASE(XR_ELEM_I64, int64_t, value);
        XRT_SET_I64_HAS_CASE(XR_ELEM_U64, uint64_t, value);
        XRT_SET_I64_HAS_CASE(XR_ELEM_BOOL, uint8_t, value != 0 ? 1 : 0);
        default:
            xrt_set_direct_unsupported(elem_type, "xrt_set_has_i64_typed");
    }
    return 0;
}

static inline int xrt_set_add_i64_typed(xrt_set_t *s, int64_t value, uint8_t elem_type) {
    if (s->elem_type != elem_type)
        xrt_set_direct_type_mismatch(elem_type, s->elem_type, "xrt_set_add_i64_typed");
    if (xrt_set_has_i64_typed(s, value, elem_type))
        return 0;
    if (s->len >= s->cap)
        xrt_set_grow(s, "xrt_set_add_i64_typed");
    switch (elem_type) {
        XRT_SET_I64_ADD_CASE(XR_ELEM_I8, int8_t, value);
        XRT_SET_I64_ADD_CASE(XR_ELEM_U8, uint8_t, value);
        XRT_SET_I64_ADD_CASE(XR_ELEM_I16, int16_t, value);
        XRT_SET_I64_ADD_CASE(XR_ELEM_U16, uint16_t, value);
        XRT_SET_I64_ADD_CASE(XR_ELEM_I32, int32_t, value);
        XRT_SET_I64_ADD_CASE(XR_ELEM_U32, uint32_t, value);
        XRT_SET_I64_ADD_CASE(XR_ELEM_I64, int64_t, value);
        XRT_SET_I64_ADD_CASE(XR_ELEM_U64, uint64_t, value);
        XRT_SET_I64_ADD_CASE(XR_ELEM_BOOL, uint8_t, value != 0 ? 1 : 0);
        default:
            xrt_set_direct_unsupported(elem_type, "xrt_set_add_i64_typed");
    }
    return 0;
}

static inline int xrt_set_delete_i64_typed(xrt_set_t *s, int64_t value, uint8_t elem_type) {
    if (s->elem_type != elem_type)
        xrt_set_direct_type_mismatch(elem_type, s->elem_type, "xrt_set_delete_i64_typed");
    switch (elem_type) {
        XRT_SET_I64_DELETE_CASE(XR_ELEM_I8, int8_t, value);
        XRT_SET_I64_DELETE_CASE(XR_ELEM_U8, uint8_t, value);
        XRT_SET_I64_DELETE_CASE(XR_ELEM_I16, int16_t, value);
        XRT_SET_I64_DELETE_CASE(XR_ELEM_U16, uint16_t, value);
        XRT_SET_I64_DELETE_CASE(XR_ELEM_I32, int32_t, value);
        XRT_SET_I64_DELETE_CASE(XR_ELEM_U32, uint32_t, value);
        XRT_SET_I64_DELETE_CASE(XR_ELEM_I64, int64_t, value);
        XRT_SET_I64_DELETE_CASE(XR_ELEM_U64, uint64_t, value);
        XRT_SET_I64_DELETE_CASE(XR_ELEM_BOOL, uint8_t, value != 0 ? 1 : 0);
        default:
            xrt_set_direct_unsupported(elem_type, "xrt_set_delete_i64_typed");
    }
    return 0;
}

static inline int xrt_set_has_f64_typed(xrt_set_t *s, double value, uint8_t elem_type) {
    if (s->elem_type != elem_type)
        xrt_set_direct_type_mismatch(elem_type, s->elem_type, "xrt_set_has_f64_typed");
    switch (elem_type) {
        XRT_SET_F64_HAS_CASE(XR_ELEM_F32, float, value);
        XRT_SET_F64_HAS_CASE(XR_ELEM_F64, double, value);
        default:
            xrt_set_direct_unsupported(elem_type, "xrt_set_has_f64_typed");
    }
    return 0;
}

static inline int xrt_set_add_f64_typed(xrt_set_t *s, double value, uint8_t elem_type) {
    if (s->elem_type != elem_type)
        xrt_set_direct_type_mismatch(elem_type, s->elem_type, "xrt_set_add_f64_typed");
    if (xrt_set_has_f64_typed(s, value, elem_type))
        return 0;
    if (s->len >= s->cap)
        xrt_set_grow(s, "xrt_set_add_f64_typed");
    switch (elem_type) {
        XRT_SET_F64_ADD_CASE(XR_ELEM_F32, float, value);
        XRT_SET_F64_ADD_CASE(XR_ELEM_F64, double, value);
        default:
            xrt_set_direct_unsupported(elem_type, "xrt_set_add_f64_typed");
    }
    return 0;
}

static inline int xrt_set_delete_f64_typed(xrt_set_t *s, double value, uint8_t elem_type) {
    if (s->elem_type != elem_type)
        xrt_set_direct_type_mismatch(elem_type, s->elem_type, "xrt_set_delete_f64_typed");
    switch (elem_type) {
        XRT_SET_F64_DELETE_CASE(XR_ELEM_F32, float, value);
        XRT_SET_F64_DELETE_CASE(XR_ELEM_F64, double, value);
        default:
            xrt_set_direct_unsupported(elem_type, "xrt_set_delete_f64_typed");
    }
    return 0;
}

#undef XRT_SET_I64_HAS_CASE
#undef XRT_SET_I64_ADD_CASE
#undef XRT_SET_I64_DELETE_CASE
#undef XRT_SET_F64_HAS_CASE
#undef XRT_SET_F64_ADD_CASE
#undef XRT_SET_F64_DELETE_CASE

#endif /* XRT_SET_DIRECT_INC */
