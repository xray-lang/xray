/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_map_typed.inc.c - Typed Map backing and direct AOT helpers.
 */

#ifndef XRT_MAP_TYPED_INC
#define XRT_MAP_TYPED_INC

static inline int xrt_map_is_typed(const xrt_map_t *m) {
    return m && m->key_type != XR_ELEM_ANY && m->value_type != XR_ELEM_ANY;
}

static inline void xrt_map_typed_abort(const char *who, const char *msg) {
    fprintf(stderr, "%s: %s\n", who ? who : "xrt_map_typed", msg ? msg : "invalid map state");
    abort();
}

static inline XrValue xrt_map_new_typed(int64_t cap, uint8_t key_type, uint8_t value_type) {
    if (key_type == XR_ELEM_ANY && value_type == XR_ELEM_ANY)
        return xrt_map_new(cap);
    if (key_type == XR_ELEM_ANY || value_type == XR_ELEM_ANY || key_type >= XR_ELEM_COUNT ||
        value_type >= XR_ELEM_COUNT)
        xrt_map_typed_abort("xrt_map_new_typed", "unsupported typed map layout");
    if (cap < 8)
        cap = 8;
    xrt_map_t *m = (xrt_map_t *) XRT_MALLOC(sizeof(xrt_map_t));
    if (!m)
        xrt_map_typed_abort("xrt_map_new_typed", "out of memory");
    m->len = 0;
    m->cap = cap;
    m->entries = NULL;
    m->key_type = key_type;
    m->value_type = value_type;
    m->key_size = XR_ELEM_SIZES[key_type];
    m->value_size = XR_ELEM_SIZES[value_type];
    m->last_lookup_index = -1;
    m->last_i64_key = 0;
    m->last_f32_key = 0.0f;
    m->last_f64_key = 0.0;
    m->keys = XRT_CALLOC((size_t) cap, (size_t) m->key_size);
    m->values = XRT_CALLOC((size_t) cap, (size_t) m->value_size);
    if (!m->keys || !m->values)
        xrt_map_typed_abort("xrt_map_new_typed", "out of memory");
    return xr_mkptr(m, XR_TAG_MAP);
}

static inline XrValue xrt_map_key_get(xrt_map_t *m, int64_t index) {
    return xrt_map_is_typed(m) ? xr_typed_get(m->keys, (int32_t) index, m->key_type)
                               : m->entries[index].key;
}

static inline XrValue xrt_map_value_get(xrt_map_t *m, int64_t index) {
    return xrt_map_is_typed(m) ? xr_typed_get(m->values, (int32_t) index, m->value_type)
                               : m->entries[index].val;
}

static inline int64_t xrt_map_find_typed(xrt_map_t *m, XrValue key) {
    for (int64_t i = 0; i < m->len; i++) {
        if (xrt_key_eq(xrt_map_key_get(m, i), key))
            return i;
    }
    return -1;
}

static inline void xrt_map_grow_typed(xrt_map_t *m, const char *who) {
    m->cap *= 2;
    void *new_keys = XRT_REALLOC(m->keys, (size_t) m->cap * (size_t) m->key_size);
    void *new_values = XRT_REALLOC(m->values, (size_t) m->cap * (size_t) m->value_size);
    if (!new_keys || !new_values)
        xrt_map_typed_abort(who, "out of memory");
    m->keys = new_keys;
    m->values = new_values;
}

static inline void xrt_map_copy_typed_entry(xrt_map_t *m, int64_t dst, int64_t src) {
    memcpy((uint8_t *) m->keys + (size_t) dst * (size_t) m->key_size,
           (uint8_t *) m->keys + (size_t) src * (size_t) m->key_size, (size_t) m->key_size);
    memcpy((uint8_t *) m->values + (size_t) dst * (size_t) m->value_size,
           (uint8_t *) m->values + (size_t) src * (size_t) m->value_size, (size_t) m->value_size);
}

static inline XrValue xrt_map_get_typed(xrt_map_t *m, XrValue key) {
    int64_t index = xrt_map_find_typed(m, key);
    return index >= 0 ? xrt_map_value_get(m, index) : XR_NULL_VAL;
}

static inline int xrt_map_has_typed(xrt_map_t *m, XrValue key) {
    return xrt_map_find_typed(m, key) >= 0;
}

static inline int xrt_map_delete_typed(xrt_map_t *m, XrValue key) {
    int64_t index = xrt_map_find_typed(m, key);
    if (index < 0)
        return 0;
    m->len--;
    if (index != m->len)
        xrt_map_copy_typed_entry(m, index, m->len);
    return 1;
}

static inline void xrt_map_set_typed(xrt_map_t *m, XrValue key, XrValue val) {
    int64_t index = xrt_map_find_typed(m, key);
    if (index >= 0) {
        (void) xr_typed_set(m->values, (int32_t) index, val, m->value_type);
        return;
    }
    if (m->len >= m->cap)
        xrt_map_grow_typed(m, "xrt_map_set_typed");
    (void) xr_typed_set(m->keys, (int32_t) m->len, key, m->key_type);
    (void) xr_typed_set(m->values, (int32_t) m->len, val, m->value_type);
    m->len++;
}

static inline void xrt_map_direct_type_mismatch(uint8_t expected_key, uint8_t actual_key,
                                                uint8_t expected_value, uint8_t actual_value,
                                                const char *who) {
    fprintf(stderr, "%s: map layout mismatch (key %u != %u, value %u != %u)\n",
            who ? who : "xrt_map_direct", (unsigned) expected_key, (unsigned) actual_key,
            (unsigned) expected_value, (unsigned) actual_value);
    abort();
}

static inline void xrt_map_direct_unsupported(uint8_t elem_type, const char *role,
                                              const char *who) {
    fprintf(stderr, "%s: unsupported %s map element type %u\n", who ? who : "xrt_map_direct",
            role ? role : "typed", (unsigned) elem_type);
    abort();
}

static inline void xrt_map_direct_require(xrt_map_t *m, uint8_t key_type, uint8_t value_type,
                                          const char *who) {
    if (!m || m->key_type != key_type || m->value_type != value_type) {
        uint8_t actual_key = m ? m->key_type : XR_ELEM_ANY;
        uint8_t actual_value = m ? m->value_type : XR_ELEM_ANY;
        xrt_map_direct_type_mismatch(key_type, actual_key, value_type, actual_value, who);
    }
}

#define XRT_MAP_I64_FIND_CASE(elem, ctype, expr)                                                   \
    case elem: {                                                                                   \
        ctype needle = (ctype) (expr);                                                             \
        ctype *keys = (ctype *) m->keys;                                                           \
        uint64_t cache_key = (uint64_t) needle;                                                    \
        int64_t cached = m->last_lookup_index;                                                     \
        if (cached >= 0 && cached < m->len && m->last_i64_key == cache_key &&                      \
            keys[cached] == needle)                                                                \
            return cached;                                                                         \
        for (int64_t i = 0; i < m->len; i++) {                                                     \
            if (keys[i] == needle) {                                                               \
                m->last_lookup_index = i;                                                          \
                m->last_i64_key = cache_key;                                                       \
                return i;                                                                          \
            }                                                                                      \
        }                                                                                          \
        m->last_lookup_index = -1;                                                                 \
        m->last_i64_key = cache_key;                                                               \
        return -1;                                                                                 \
    }

#define XRT_MAP_F64_FIND_CASE(elem, ctype, expr)                                                   \
    case elem: {                                                                                   \
        ctype needle = (ctype) (expr);                                                             \
        ctype *keys = (ctype *) m->keys;                                                           \
        double cache_key = (double) needle;                                                        \
        int64_t cached = m->last_lookup_index;                                                     \
        if (cached >= 0 && cached < m->len && m->last_f64_key == cache_key &&                      \
            keys[cached] == needle)                                                                \
            return cached;                                                                         \
        for (int64_t i = 0; i < m->len; i++) {                                                     \
            if (keys[i] == needle) {                                                               \
                m->last_lookup_index = i;                                                          \
                m->last_f64_key = cache_key;                                                       \
                return i;                                                                          \
            }                                                                                      \
        }                                                                                          \
        m->last_lookup_index = -1;                                                                 \
        m->last_f64_key = cache_key;                                                               \
        return -1;                                                                                 \
    }

#define XRT_MAP_I64_STORE_CASE(elem, ctype, expr, field)                                           \
    case elem:                                                                                     \
        ((ctype *) field)[index] = (ctype) (expr);                                                 \
        return

#define XRT_MAP_F64_STORE_CASE(elem, ctype, expr, field)                                           \
    case elem:                                                                                     \
        ((ctype *) field)[index] = (ctype) (expr);                                                 \
        return

#define XRT_MAP_I64_GET_CASE(elem, ctype)                                                          \
    case elem:                                                                                     \
        return (int64_t) ((ctype *) m->values)[index]

#define XRT_MAP_F64_GET_CASE(elem, ctype)                                                          \
    case elem:                                                                                     \
        return (double) ((ctype *) m->values)[index]

static inline int64_t xrt_map_find_i64_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                             uint8_t value_type) {
    (void) value_type;
    switch (key_type) {
        XRT_MAP_I64_FIND_CASE(XR_ELEM_I8, int8_t, key);
        XRT_MAP_I64_FIND_CASE(XR_ELEM_U8, uint8_t, key);
        XRT_MAP_I64_FIND_CASE(XR_ELEM_I16, int16_t, key);
        XRT_MAP_I64_FIND_CASE(XR_ELEM_U16, uint16_t, key);
        XRT_MAP_I64_FIND_CASE(XR_ELEM_I32, int32_t, key);
        XRT_MAP_I64_FIND_CASE(XR_ELEM_U32, uint32_t, key);
        XRT_MAP_I64_FIND_CASE(XR_ELEM_I64, int64_t, key);
        XRT_MAP_I64_FIND_CASE(XR_ELEM_U64, uint64_t, key);
        XRT_MAP_I64_FIND_CASE(XR_ELEM_BOOL, uint8_t, key != 0 ? 1 : 0);
        default:
            xrt_map_direct_unsupported(key_type, "key", "xrt_map_find_i64_typed");
    }
    return -1;
}

static inline int64_t xrt_map_find_f64_typed(xrt_map_t *m, double key, uint8_t key_type,
                                             uint8_t value_type) {
    (void) value_type;
    switch (key_type) {
        XRT_MAP_F64_FIND_CASE(XR_ELEM_F32, float, key);
        XRT_MAP_F64_FIND_CASE(XR_ELEM_F64, double, key);
        default:
            xrt_map_direct_unsupported(key_type, "key", "xrt_map_find_f64_typed");
    }
    return -1;
}

static inline void xrt_map_store_i64_key_typed(xrt_map_t *m, int64_t index, int64_t key,
                                               uint8_t key_type) {
    switch (key_type) {
        XRT_MAP_I64_STORE_CASE(XR_ELEM_I8, int8_t, key, m->keys);
        XRT_MAP_I64_STORE_CASE(XR_ELEM_U8, uint8_t, key, m->keys);
        XRT_MAP_I64_STORE_CASE(XR_ELEM_I16, int16_t, key, m->keys);
        XRT_MAP_I64_STORE_CASE(XR_ELEM_U16, uint16_t, key, m->keys);
        XRT_MAP_I64_STORE_CASE(XR_ELEM_I32, int32_t, key, m->keys);
        XRT_MAP_I64_STORE_CASE(XR_ELEM_U32, uint32_t, key, m->keys);
        XRT_MAP_I64_STORE_CASE(XR_ELEM_I64, int64_t, key, m->keys);
        XRT_MAP_I64_STORE_CASE(XR_ELEM_U64, uint64_t, key, m->keys);
        XRT_MAP_I64_STORE_CASE(XR_ELEM_BOOL, uint8_t, key != 0 ? 1 : 0, m->keys);
        default:
            xrt_map_direct_unsupported(key_type, "key", "xrt_map_store_i64_key_typed");
    }
}

static inline void xrt_map_store_f64_key_typed(xrt_map_t *m, int64_t index, double key,
                                               uint8_t key_type) {
    switch (key_type) {
        XRT_MAP_F64_STORE_CASE(XR_ELEM_F32, float, key, m->keys);
        XRT_MAP_F64_STORE_CASE(XR_ELEM_F64, double, key, m->keys);
        default:
            xrt_map_direct_unsupported(key_type, "key", "xrt_map_store_f64_key_typed");
    }
}

static inline int64_t xrt_map_get_i64_value_typed(xrt_map_t *m, int64_t index, uint8_t value_type) {
    switch (value_type) {
        XRT_MAP_I64_GET_CASE(XR_ELEM_I8, int8_t);
        XRT_MAP_I64_GET_CASE(XR_ELEM_U8, uint8_t);
        XRT_MAP_I64_GET_CASE(XR_ELEM_I16, int16_t);
        XRT_MAP_I64_GET_CASE(XR_ELEM_U16, uint16_t);
        XRT_MAP_I64_GET_CASE(XR_ELEM_I32, int32_t);
        XRT_MAP_I64_GET_CASE(XR_ELEM_U32, uint32_t);
        XRT_MAP_I64_GET_CASE(XR_ELEM_I64, int64_t);
        XRT_MAP_I64_GET_CASE(XR_ELEM_U64, uint64_t);
        XRT_MAP_I64_GET_CASE(XR_ELEM_BOOL, uint8_t);
        default:
            xrt_map_direct_unsupported(value_type, "value", "xrt_map_get_i64_value_typed");
    }
    return 0;
}

static inline double xrt_map_get_f64_value_typed(xrt_map_t *m, int64_t index, uint8_t value_type) {
    switch (value_type) {
        XRT_MAP_F64_GET_CASE(XR_ELEM_F32, float);
        XRT_MAP_F64_GET_CASE(XR_ELEM_F64, double);
        default:
            xrt_map_direct_unsupported(value_type, "value", "xrt_map_get_f64_value_typed");
    }
    return 0.0;
}

static inline void xrt_map_store_i64_value_typed(xrt_map_t *m, int64_t index, int64_t value,
                                                 uint8_t value_type) {
    switch (value_type) {
        XRT_MAP_I64_STORE_CASE(XR_ELEM_I8, int8_t, value, m->values);
        XRT_MAP_I64_STORE_CASE(XR_ELEM_U8, uint8_t, value, m->values);
        XRT_MAP_I64_STORE_CASE(XR_ELEM_I16, int16_t, value, m->values);
        XRT_MAP_I64_STORE_CASE(XR_ELEM_U16, uint16_t, value, m->values);
        XRT_MAP_I64_STORE_CASE(XR_ELEM_I32, int32_t, value, m->values);
        XRT_MAP_I64_STORE_CASE(XR_ELEM_U32, uint32_t, value, m->values);
        XRT_MAP_I64_STORE_CASE(XR_ELEM_I64, int64_t, value, m->values);
        XRT_MAP_I64_STORE_CASE(XR_ELEM_U64, uint64_t, value, m->values);
        XRT_MAP_I64_STORE_CASE(XR_ELEM_BOOL, uint8_t, value != 0 ? 1 : 0, m->values);
        default:
            xrt_map_direct_unsupported(value_type, "value", "xrt_map_store_i64_value_typed");
    }
}

static inline void xrt_map_store_f64_value_typed(xrt_map_t *m, int64_t index, double value,
                                                 uint8_t value_type) {
    switch (value_type) {
        XRT_MAP_F64_STORE_CASE(XR_ELEM_F32, float, value, m->values);
        XRT_MAP_F64_STORE_CASE(XR_ELEM_F64, double, value, m->values);
        default:
            xrt_map_direct_unsupported(value_type, "value", "xrt_map_store_f64_value_typed");
    }
}

static inline int64_t xrt_map_get_i64_i64_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                                uint8_t value_type) {
    int64_t index = xrt_map_find_i64_typed(m, key, key_type, value_type);
    return index >= 0 ? xrt_map_get_i64_value_typed(m, index, value_type) : 0;
}

static inline double xrt_map_get_i64_f64_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                               uint8_t value_type) {
    int64_t index = xrt_map_find_i64_typed(m, key, key_type, value_type);
    return index >= 0 ? xrt_map_get_f64_value_typed(m, index, value_type) : 0.0;
}

static inline int64_t xrt_map_get_f64_i64_typed(xrt_map_t *m, double key, uint8_t key_type,
                                                uint8_t value_type) {
    int64_t index = xrt_map_find_f64_typed(m, key, key_type, value_type);
    return index >= 0 ? xrt_map_get_i64_value_typed(m, index, value_type) : 0;
}

static inline double xrt_map_get_f64_f64_typed(xrt_map_t *m, double key, uint8_t key_type,
                                               uint8_t value_type) {
    int64_t index = xrt_map_find_f64_typed(m, key, key_type, value_type);
    return index >= 0 ? xrt_map_get_f64_value_typed(m, index, value_type) : 0.0;
}

static inline int xrt_map_has_i64_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                        uint8_t value_type) {
    return xrt_map_find_i64_typed(m, key, key_type, value_type) >= 0;
}

static inline int xrt_map_has_f64_typed(xrt_map_t *m, double key, uint8_t key_type,
                                        uint8_t value_type) {
    return xrt_map_find_f64_typed(m, key, key_type, value_type) >= 0;
}

static inline int xrt_map_delete_i64_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                           uint8_t value_type) {
    int64_t index = xrt_map_find_i64_typed(m, key, key_type, value_type);
    if (index < 0)
        return 0;
    m->len--;
    if (index != m->len)
        xrt_map_copy_typed_entry(m, index, m->len);
    return 1;
}

static inline int xrt_map_delete_f64_typed(xrt_map_t *m, double key, uint8_t key_type,
                                           uint8_t value_type) {
    int64_t index = xrt_map_find_f64_typed(m, key, key_type, value_type);
    if (index < 0)
        return 0;
    m->len--;
    if (index != m->len)
        xrt_map_copy_typed_entry(m, index, m->len);
    return 1;
}

static inline void xrt_map_prepare_new_typed_entry(xrt_map_t *m, int64_t *index, const char *who) {
    if (m->len >= m->cap)
        xrt_map_grow_typed(m, who);
    *index = m->len;
    m->len++;
}

static inline void xrt_map_set_i64_i64_typed(xrt_map_t *m, int64_t key, int64_t value,
                                             uint8_t key_type, uint8_t value_type) {
    int64_t index = xrt_map_find_i64_typed(m, key, key_type, value_type);
    if (index < 0) {
        xrt_map_prepare_new_typed_entry(m, &index, "xrt_map_set_i64_i64_typed");
        xrt_map_store_i64_key_typed(m, index, key, key_type);
    }
    xrt_map_store_i64_value_typed(m, index, value, value_type);
}

static inline void xrt_map_set_i64_f64_typed(xrt_map_t *m, int64_t key, double value,
                                             uint8_t key_type, uint8_t value_type) {
    int64_t index = xrt_map_find_i64_typed(m, key, key_type, value_type);
    if (index < 0) {
        xrt_map_prepare_new_typed_entry(m, &index, "xrt_map_set_i64_f64_typed");
        xrt_map_store_i64_key_typed(m, index, key, key_type);
    }
    xrt_map_store_f64_value_typed(m, index, value, value_type);
}

static inline void xrt_map_set_f64_i64_typed(xrt_map_t *m, double key, int64_t value,
                                             uint8_t key_type, uint8_t value_type) {
    int64_t index = xrt_map_find_f64_typed(m, key, key_type, value_type);
    if (index < 0) {
        xrt_map_prepare_new_typed_entry(m, &index, "xrt_map_set_f64_i64_typed");
        xrt_map_store_f64_key_typed(m, index, key, key_type);
    }
    xrt_map_store_i64_value_typed(m, index, value, value_type);
}

static inline void xrt_map_set_f64_f64_typed(xrt_map_t *m, double key, double value,
                                             uint8_t key_type, uint8_t value_type) {
    int64_t index = xrt_map_find_f64_typed(m, key, key_type, value_type);
    if (index < 0) {
        xrt_map_prepare_new_typed_entry(m, &index, "xrt_map_set_f64_f64_typed");
        xrt_map_store_f64_key_typed(m, index, key, key_type);
    }
    xrt_map_store_f64_value_typed(m, index, value, value_type);
}

static inline int64_t xrt_map_find_bool_key_typed(xrt_map_t *m, int64_t key) {
    uint8_t needle = key != 0 ? 1 : 0;
    uint8_t *keys = (uint8_t *) m->keys;
    if (m->len > 0 && keys[0] == needle)
        return 0;
    if (m->len > 1 && keys[1] == needle)
        return 1;
    return -1;
}

static inline int64_t xrt_map_get_bool_i64_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                                 uint8_t value_type) {
    (void) key_type;
    (void) value_type;
    int64_t index = xrt_map_find_bool_key_typed(m, key);
    return index >= 0 ? ((int64_t *) m->values)[index] : 0;
}

static inline int xrt_map_has_bool_i64_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                             uint8_t value_type) {
    (void) key_type;
    (void) value_type;
    return xrt_map_find_bool_key_typed(m, key) >= 0;
}

static inline int xrt_map_delete_bool_i64_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                                uint8_t value_type) {
    (void) key_type;
    (void) value_type;
    int64_t index = xrt_map_find_bool_key_typed(m, key);
    if (index < 0)
        return 0;
    m->len--;
    if (index != m->len) {
        ((uint8_t *) m->keys)[index] = ((uint8_t *) m->keys)[m->len];
        ((int64_t *) m->values)[index] = ((int64_t *) m->values)[m->len];
    }
    return 1;
}

static inline void xrt_map_set_bool_i64_typed(xrt_map_t *m, int64_t key, int64_t value,
                                              uint8_t key_type, uint8_t value_type) {
    (void) key_type;
    (void) value_type;
    int64_t index = xrt_map_find_bool_key_typed(m, key);
    if (index < 0) {
        xrt_map_prepare_new_typed_entry(m, &index, "xrt_map_set_bool_i64_typed");
        ((uint8_t *) m->keys)[index] = key != 0 ? 1 : 0;
    }
    ((int64_t *) m->values)[index] = value;
}

static inline double xrt_map_get_bool_f32_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                                uint8_t value_type) {
    (void) key_type;
    (void) value_type;
    int64_t index = xrt_map_find_bool_key_typed(m, key);
    return index >= 0 ? (double) ((float *) m->values)[index] : 0.0;
}

static inline int xrt_map_has_bool_f32_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                             uint8_t value_type) {
    (void) key_type;
    (void) value_type;
    return xrt_map_find_bool_key_typed(m, key) >= 0;
}

static inline int xrt_map_delete_bool_f32_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                                uint8_t value_type) {
    (void) key_type;
    (void) value_type;
    int64_t index = xrt_map_find_bool_key_typed(m, key);
    if (index < 0)
        return 0;
    m->len--;
    if (index != m->len) {
        ((uint8_t *) m->keys)[index] = ((uint8_t *) m->keys)[m->len];
        ((float *) m->values)[index] = ((float *) m->values)[m->len];
    }
    return 1;
}

static inline void xrt_map_set_bool_f32_typed(xrt_map_t *m, int64_t key, double value,
                                              uint8_t key_type, uint8_t value_type) {
    (void) key_type;
    (void) value_type;
    int64_t index = xrt_map_find_bool_key_typed(m, key);
    if (index < 0) {
        xrt_map_prepare_new_typed_entry(m, &index, "xrt_map_set_bool_f32_typed");
        ((uint8_t *) m->keys)[index] = key != 0 ? 1 : 0;
    }
    ((float *) m->values)[index] = (float) value;
}

static inline int64_t xrt_map_find_f32_f32_typed(xrt_map_t *m, double key, uint8_t key_type,
                                                 uint8_t value_type) {
    (void) key_type;
    (void) value_type;
    float needle = (float) key;
    float *keys = (float *) m->keys;
    int64_t cached = m->last_lookup_index;
    if (cached >= 0 && cached < m->len && m->last_f32_key == needle && keys[cached] == needle)
        return cached;
    for (int64_t i = 0; i < m->len; i++) {
        if (keys[i] == needle) {
            m->last_lookup_index = i;
            m->last_f32_key = needle;
            return i;
        }
    }
    m->last_lookup_index = -1;
    m->last_f32_key = needle;
    return -1;
}

static inline double xrt_map_get_f32_f32_typed(xrt_map_t *m, double key, uint8_t key_type,
                                               uint8_t value_type) {
    int64_t index = xrt_map_find_f32_f32_typed(m, key, key_type, value_type);
    return index >= 0 ? (double) ((float *) m->values)[index] : 0.0;
}

static inline int xrt_map_has_f32_f32_typed(xrt_map_t *m, double key, uint8_t key_type,
                                            uint8_t value_type) {
    return xrt_map_find_f32_f32_typed(m, key, key_type, value_type) >= 0;
}

static inline int xrt_map_delete_f32_f32_typed(xrt_map_t *m, double key, uint8_t key_type,
                                               uint8_t value_type) {
    int64_t index = xrt_map_find_f32_f32_typed(m, key, key_type, value_type);
    if (index < 0)
        return 0;
    m->len--;
    if (index != m->len) {
        ((float *) m->keys)[index] = ((float *) m->keys)[m->len];
        ((float *) m->values)[index] = ((float *) m->values)[m->len];
    }
    return 1;
}

static inline void xrt_map_set_f32_f32_typed(xrt_map_t *m, double key, double value,
                                             uint8_t key_type, uint8_t value_type) {
    int64_t index = xrt_map_find_f32_f32_typed(m, key, key_type, value_type);
    if (index < 0) {
        xrt_map_prepare_new_typed_entry(m, &index, "xrt_map_set_f32_f32_typed");
        ((float *) m->keys)[index] = (float) key;
    }
    ((float *) m->values)[index] = (float) value;
}

#undef XRT_MAP_I64_FIND_CASE
#undef XRT_MAP_F64_FIND_CASE
#undef XRT_MAP_I64_STORE_CASE
#undef XRT_MAP_F64_STORE_CASE
#undef XRT_MAP_I64_GET_CASE
#undef XRT_MAP_F64_GET_CASE

#endif /* XRT_MAP_TYPED_INC */
