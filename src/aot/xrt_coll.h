/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_coll.h - Collection runtime: Array, Map, StringBuilder, Closure, index ops
 */

#ifndef XRT_COLL_H
#define XRT_COLL_H

#include "xrt_value.h"
#include "xrt_arc.h"  // xrt_str_alloc used by xrt_strbuf_finish
#include "xrt_range.h"
#include "../shared/xr_elem_type.h"
#include "../shared/xr_typed_ops.h"

/* =========================================================================
 * Array runtime
 * ========================================================================= */

typedef struct {
    int64_t len;
    int64_t cap;
    void *data;        /* uint8_t[] / int64_t[] / XrValue[] — depends on elem_type */
    uint8_t elem_type; /* XR_ELEM_ANY / XR_ELEM_U8 / ... */
    uint8_t elem_size; /* cached bytes per element */
    uint8_t is_slice;  /* view over another array; cannot grow */
} xrt_array_t;

static inline XrValue xrt_array_new(int64_t cap) {
    if (cap < 4)
        cap = 4;
    xrt_array_t *a = (xrt_array_t *) XRT_MALLOC(sizeof(xrt_array_t));
    if (!a) {
        fprintf(stderr, "xrt_array_new: out of memory\n");
        abort();
    }
    a->len = 0;
    a->cap = cap;
    a->elem_type = XR_ELEM_ANY;
    a->elem_size = (uint8_t) sizeof(XrValue);
    a->is_slice = 0;
    a->data = XRT_CALLOC((size_t) cap, sizeof(XrValue));
    if (!a->data) {
        fprintf(stderr, "xrt_array_new: out of memory\n");
        abort();
    }
    return xr_mkptr(a, XR_TAG_ARRAY);
}

static inline xrt_array_t *xrt_array_new_typed_ptr(int64_t cap, uint8_t etype) {
    if (cap < 4)
        cap = 4;
    xrt_array_t *a = (xrt_array_t *) XRT_MALLOC(sizeof(xrt_array_t));
    if (!a) {
        fprintf(stderr, "xrt_array_new_typed: out of memory\n");
        abort();
    }
    a->len = 0;
    a->cap = cap;
    a->elem_type = etype;
    a->elem_size = XR_ELEM_SIZES[etype];
    a->is_slice = 0;
    a->data = XRT_CALLOC((size_t) cap, (size_t) a->elem_size);
    if (!a->data) {
        fprintf(stderr, "xrt_array_new_typed: out of memory\n");
        abort();
    }
    return a;
}

static inline XrValue xrt_array_new_typed(int64_t cap, uint8_t etype) {
    return xr_mkptr(xrt_array_new_typed_ptr(cap, etype), XR_TAG_ARRAY);
}

static inline xrt_array_t *xrt_array_new_typed_uninit_ptr(int64_t cap, uint8_t etype) {
    if (cap < 4)
        cap = 4;
    xrt_array_t *a = (xrt_array_t *) XRT_MALLOC(sizeof(xrt_array_t));
    if (!a) {
        fprintf(stderr, "xrt_array_new_typed_uninit: out of memory\n");
        abort();
    }
    a->len = 0;
    a->cap = cap;
    a->elem_type = etype;
    a->elem_size = XR_ELEM_SIZES[etype];
    a->is_slice = 0;
    a->data = XRT_MALLOC((size_t) cap * (size_t) a->elem_size);
    if (!a->data) {
        fprintf(stderr, "xrt_array_new_typed_uninit: out of memory\n");
        abort();
    }
    return a;
}

static inline XrValue xrt_array_new_typed_uninit(int64_t cap, uint8_t etype) {
    return xr_mkptr(xrt_array_new_typed_uninit_ptr(cap, etype), XR_TAG_ARRAY);
}
static inline XrValue xrt_bytes_new_len(int64_t len) {
    if (len < 0)
        len = 0;
    XrValue arr = xrt_array_new_typed(len, XR_ELEM_U8);
    ((xrt_array_t *) arr.ptr)->len = len;
    return arr;
}

static inline XrValue xrt_bytes_new_fill(XrValue len_value, XrValue fill_value) {
    int64_t len = xr_value_to_int64_coerce(len_value);
    XrValue arr = xrt_bytes_new_len(len);
    xrt_array_t *a = (xrt_array_t *) arr.ptr;
    uint8_t fill = (uint8_t) (xr_value_to_int64_coerce(fill_value) & 0xFF);
    if (a->len > 0)
        memset(a->data, fill, (size_t) a->len);
    return arr;
}

static inline XrValue xrt_bytes_new_copy(XrValue src_value) {
    if (src_value.tag != XR_TAG_ARRAY || !src_value.ptr)
        return XR_NULL_VAL;
    xrt_array_t *src = (xrt_array_t *) src_value.ptr;
    XrValue arr = xrt_bytes_new_len(src->len);
    xrt_array_t *dst = (xrt_array_t *) arr.ptr;
    if (src->elem_type == XR_ELEM_U8) {
        memcpy(dst->data, src->data, (size_t) src->len);
        return arr;
    }
    for (int64_t i = 0; i < src->len; i++) {
        XrValue item = xr_typed_get(src->data, (int32_t) i, src->elem_type);
        xr_typed_set(dst->data, (int32_t) i, item, dst->elem_type);
    }
    return arr;
}

static inline XrValue xrt_bytes_new_1(XrValue arg) {
    if (arg.tag == XR_TAG_ARRAY)
        return xrt_bytes_new_copy(arg);
    return xrt_bytes_new_len(xr_value_to_int64_coerce(arg));
}

static inline void xrt_array_push(XrValue arr, XrValue val) {
    xrt_array_t *a = (xrt_array_t *) arr.ptr;
    if (a->is_slice) {
        fprintf(stderr, "xrt_array_push: cannot push to array slice\n");
        abort();
    }
    if (a->len >= a->cap) {
        a->cap = a->cap == 0 ? 4 : a->cap * 2;
        void *tmp = XRT_REALLOC(a->data, (size_t) a->cap * (size_t) a->elem_size);
        if (!tmp) {
            fprintf(stderr, "xrt_array_push: out of memory\n");
            abort();
        }
        a->data = tmp;
    }
    xr_typed_set(a->data, (int32_t) a->len, val, a->elem_type);
    a->len++;
}

static inline int64_t xrt_array_len(XrValue arr) {
    return ((xrt_array_t *) arr.ptr)->len;
}

static inline XrValue xrt_array_slice_view(XrValue arr, int64_t start, int64_t end) {
    if (arr.tag != XR_TAG_ARRAY || !arr.ptr)
        return XR_NULL_VAL;
    xrt_array_t *src = (xrt_array_t *) arr.ptr;
    if (start < 0)
        start = 0;
    if (end < 0 || end > src->len)
        end = src->len;
    if (start > src->len)
        start = src->len;
    if (start > end)
        start = end;

    xrt_array_t *slice = (xrt_array_t *) XRT_MALLOC(sizeof(xrt_array_t));
    if (!slice) {
        fprintf(stderr, "xrt_array_slice_view: out of memory\n");
        abort();
    }
    slice->len = end - start;
    slice->cap = 0;
    slice->elem_type = src->elem_type;
    slice->elem_size = src->elem_size;
    slice->is_slice = 1;
    slice->data = (uint8_t *) src->data + (size_t) start * (size_t) src->elem_size;
    return xr_mkptr(slice, XR_TAG_ARRAY);
}

#include "xrt_array_bytes.inc.c"

typedef struct {
    int64_t len;
    XrValue items[];
} xrt_tuple_t;

static inline XrValue xrt_tuple_new(int64_t len) {
    if (len < 0)
        len = 0;
    xrt_tuple_t *t =
        (xrt_tuple_t *) XRT_MALLOC(sizeof(xrt_tuple_t) + (size_t) len * sizeof(XrValue));
    if (!t) {
        fprintf(stderr, "xrt_tuple_new: out of memory\n");
        abort();
    }
    t->len = len;
    for (int64_t i = 0; i < len; i++)
        t->items[i] = XR_NULL_VAL;
    return xr_mkptr(t, XR_TAG_TUPLE);
}

static inline XrValue xrt_tuple_make(int64_t len, const XrValue *items) {
    XrValue tuple = xrt_tuple_new(len);
    xrt_tuple_t *t = (xrt_tuple_t *) tuple.ptr;
    for (int64_t i = 0; i < t->len; i++)
        t->items[i] = items ? items[i] : XR_NULL_VAL;
    return tuple;
}

static inline XrValue xrt_tuple_get(XrValue tuple, int64_t index) {
    if (tuple.tag != XR_TAG_TUPLE || !tuple.ptr)
        return XR_NULL_VAL;
    xrt_tuple_t *t = (xrt_tuple_t *) tuple.ptr;
    if (index < 0 || index >= t->len)
        return XR_NULL_VAL;
    return t->items[index];
}

static inline XrValue xrt_slice(XrValue source, XrValue start_value, XrValue end_value) {
    int64_t start = xr_value_to_int64_coerce(start_value);
    int64_t end = xr_value_to_int64_coerce(end_value);
    if (source.tag == XR_TAG_ARRAY)
        return xrt_array_slice_view(source, start, end);
    if (XR_IS_STR(source)) {
        const char *s = (const char *) source.ptr;
        int64_t len = (int64_t) strlen(s);
        if (start < 0) {
            start += len;
            if (start < 0)
                start = 0;
        }
        if (end < 0) {
            end += len;
            if (end < 0)
                end = 0;
        }
        if (start > len)
            start = len;
        if (end > len)
            end = len;
        if (start >= end)
            return xr_box_str("");
        int64_t rlen = end - start;
        XrValue sv = xrt_str_alloc((size_t) rlen);
        memcpy((char *) sv.ptr, s + start, (size_t) rlen);
        ((char *) sv.ptr)[rlen] = 0;
        return sv;
    }
    return XR_NULL_VAL;
}

/* Stack-allocated array: header on stack, data buffer on stack via alloca.
 * Used for NO_ESCAPE arrays (escape analysis optimization).
 * The returned XrValue is valid only within the current function scope. */
#ifndef xrt_array_stack_new
#define xrt_array_stack_new(cap_expr)                                                              \
    ({                                                                                             \
        int64_t _cap = (cap_expr);                                                                 \
        if (_cap < 4)                                                                              \
            _cap = 4;                                                                              \
        xrt_array_t *_a = (xrt_array_t *) __builtin_alloca(sizeof(xrt_array_t) +                   \
                                                           (size_t) _cap * sizeof(XrValue));       \
        _a->len = 0;                                                                               \
        _a->cap = _cap;                                                                            \
        _a->elem_type = XR_ELEM_ANY;                                                               \
        _a->elem_size = (uint8_t) sizeof(XrValue);                                                 \
        _a->data = (void *) ((char *) _a + sizeof(xrt_array_t));                                   \
        memset(_a->data, 0, (size_t) _cap * sizeof(XrValue));                                      \
        xr_mkptr(_a, XR_TAG_ARRAY);                                                                \
    })
#endif

/* =========================================================================
 * StringBuilder runtime
 * ========================================================================= */

typedef struct {
    char *buf;
    int64_t len;
    int64_t cap;
} xrt_strbuf_t;

static inline XrValue xrt_strbuf_new(void) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) XRT_MALLOC(sizeof(xrt_strbuf_t));
    if (!sb) {
        fprintf(stderr, "xrt_strbuf_new: out of memory\n");
        abort();
    }
    sb->cap = 64;
    sb->len = 0;
    sb->buf = (char *) XRT_MALLOC(64);
    if (!sb->buf) {
        fprintf(stderr, "xrt_strbuf_new: out of memory\n");
        abort();
    }
    sb->buf[0] = 0;
    return xr_mkptr(sb, XR_TAG_STRBUF);
}

static inline void xrt_strbuf_grow(xrt_strbuf_t *sb, int64_t need) {
    while (sb->len + need >= sb->cap)
        sb->cap *= 2;
    char *tmp = (char *) XRT_REALLOC(sb->buf, (size_t) sb->cap);
    if (!tmp) {
        fprintf(stderr, "xrt_strbuf_grow: out of memory\n");
        abort();
    }
    sb->buf = tmp;
}

static inline void xrt_strbuf_append(XrValue sbv, XrValue val) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) sbv.ptr;
    if (val.tag == XR_TAG_STR || val.tag == XR_TAG_STR_ARC) {
        const char *s = (const char *) val.ptr;
        int64_t slen = (int64_t) strlen(s);
        xrt_strbuf_grow(sb, slen);
        memcpy(sb->buf + sb->len, s, (size_t) slen);
        sb->len += slen;
        sb->buf[sb->len] = 0;
    } else if (val.tag == XR_TAG_I64) {
        char tmp[24];
        int n = snprintf(tmp, sizeof(tmp), "%lld", (long long) val.i);
        xrt_strbuf_grow(sb, n);
        memcpy(sb->buf + sb->len, tmp, (size_t) n);
        sb->len += n;
        sb->buf[sb->len] = 0;
    } else if (val.tag == XR_TAG_F64) {
        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), "%g", val.f);
        xrt_strbuf_grow(sb, n);
        memcpy(sb->buf + sb->len, tmp, (size_t) n);
        sb->len += n;
        sb->buf[sb->len] = 0;
    } else if (val.tag == XR_TAG_BOOL) {
        const char *bs = val.i ? "true" : "false";
        int blen = val.i ? 4 : 5;
        xrt_strbuf_grow(sb, blen);
        memcpy(sb->buf + sb->len, bs, (size_t) blen);
        sb->len += blen;
        sb->buf[sb->len] = 0;
    } else if (val.tag == XR_TAG_NULL) {
        xrt_strbuf_grow(sb, 4);
        memcpy(sb->buf + sb->len, "null", 4);
        sb->len += 4;
        sb->buf[sb->len] = 0;
    }
}

static inline XrValue xrt_strbuf_finish(XrValue sbv) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) sbv.ptr;
    XrValue v = xrt_str_alloc((size_t) sb->len);
    memcpy((char *) v.ptr, sb->buf, (size_t) (sb->len + 1));
    return v;
}

/* =========================================================================
 * Map runtime  (linear-probing, O(n) — suitable for small maps)
 * ========================================================================= */

typedef struct {
    XrValue key;
    XrValue val;
} xrt_map_entry_t;
typedef struct {
    int64_t len;
    int64_t cap;
    xrt_map_entry_t *entries;
    void *keys;
    void *values;
    uint8_t key_type;
    uint8_t value_type;
    uint8_t key_size;
    uint8_t value_size;
    int64_t last_lookup_index;
    uint64_t last_i64_key;
    float last_f32_key;
    double last_f64_key;
} xrt_map_t;

static inline XrValue xrt_map_new(int64_t cap) {
    if (cap < 8)
        cap = 8;
    xrt_map_t *m = (xrt_map_t *) XRT_MALLOC(sizeof(xrt_map_t));
    if (!m) {
        fprintf(stderr, "xrt_map_new: out of memory\n");
        abort();
    }
    m->len = 0;
    m->cap = cap;
    m->keys = NULL;
    m->values = NULL;
    m->key_type = XR_ELEM_ANY;
    m->value_type = XR_ELEM_ANY;
    m->key_size = (uint8_t) sizeof(XrValue);
    m->value_size = (uint8_t) sizeof(XrValue);
    m->last_lookup_index = -1;
    m->last_i64_key = 0;
    m->last_f32_key = 0.0f;
    m->last_f64_key = 0.0;
    m->entries = (xrt_map_entry_t *) XRT_CALLOC((size_t) cap, sizeof(xrt_map_entry_t));
    if (!m->entries) {
        fprintf(stderr, "xrt_map_new: out of memory\n");
        abort();
    }
    return xr_mkptr(m, XR_TAG_MAP);
}

static inline int xrt_key_eq(XrValue a, XrValue b) {
    if (a.tag != b.tag)
        return 0;
    if (a.tag == XR_TAG_I64)
        return a.i == b.i;
    if (a.tag == XR_TAG_F64)
        return a.f == b.f;
    if (a.tag == XR_TAG_BOOL)
        return a.i == b.i;
    if (a.tag == XR_TAG_STR)
        return strcmp((const char *) a.ptr, (const char *) b.ptr) == 0;
    return a.ptr == b.ptr;
}

#include "xrt_map_typed.inc.c"

static inline XrValue xrt_map_get(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_typed(m))
        return xrt_map_get_typed(m, key);
    for (int64_t i = 0; i < m->len; i++)
        if (xrt_key_eq(m->entries[i].key, key))
            return m->entries[i].val;
    return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
}

static inline int xrt_map_has(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_typed(m))
        return xrt_map_has_typed(m, key);
    for (int64_t i = 0; i < m->len; i++)
        if (xrt_key_eq(m->entries[i].key, key))
            return 1;
    return 0;
}

static inline int xrt_map_delete(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_typed(m))
        return xrt_map_delete_typed(m, key);
    for (int64_t i = 0; i < m->len; i++) {
        if (xrt_key_eq(m->entries[i].key, key)) {
            m->entries[i] = m->entries[--m->len];
            return 1;
        }
    }
    return 0;
}

static inline void xrt_map_set(xrt_map_t *m, XrValue key, XrValue val) {
    if (xrt_map_is_typed(m)) {
        xrt_map_set_typed(m, key, val);
        return;
    }
    for (int64_t i = 0; i < m->len; i++) {
        if (xrt_key_eq(m->entries[i].key, key)) {
            m->entries[i].val = val;
            return;
        }
    }
    if (m->len >= m->cap) {
        m->cap *= 2;
        xrt_map_entry_t *tmp =
            (xrt_map_entry_t *) XRT_REALLOC(m->entries, (size_t) m->cap * sizeof(xrt_map_entry_t));
        if (!tmp) {
            fprintf(stderr, "xrt_map_set: out of memory\n");
            abort();
        }
        m->entries = tmp;
    }
    m->entries[m->len].key = key;
    m->entries[m->len].val = val;
    m->len++;
}

/* =========================================================================
 * Set runtime  (linear set, O(n) — suitable for small sets)
 * ========================================================================= */

typedef struct {
    int64_t len;
    int64_t cap;
    void *items;
    uint8_t elem_type;
    uint8_t elem_size;
} xrt_set_t;

static inline XrValue xrt_set_new_typed(int64_t cap, uint8_t elem_type) {
    if (cap < 8)
        cap = 8;
    if (elem_type >= XR_ELEM_COUNT)
        elem_type = XR_ELEM_ANY;
    xrt_set_t *s = (xrt_set_t *) XRT_MALLOC(sizeof(xrt_set_t));
    if (!s) {
        fprintf(stderr, "xrt_set_new: out of memory\n");
        abort();
    }
    s->len = 0;
    s->cap = cap;
    s->elem_type = elem_type;
    s->elem_size = elem_type == XR_ELEM_ANY ? (uint8_t) sizeof(XrValue) : XR_ELEM_SIZES[elem_type];
    s->items = XRT_CALLOC((size_t) cap, (size_t) s->elem_size);
    if (!s->items) {
        fprintf(stderr, "xrt_set_new: out of memory\n");
        abort();
    }
    return xr_mkptr(s, XR_TAG_SET);
}

static inline XrValue xrt_set_new(int64_t cap) {
    return xrt_set_new_typed(cap, XR_ELEM_ANY);
}

static inline void xrt_set_grow(xrt_set_t *s, const char *who) {
    s->cap *= 2;
    void *tmp = XRT_REALLOC(s->items, (size_t) s->cap * (size_t) s->elem_size);
    if (!tmp) {
        fprintf(stderr, "%s: out of memory\n", who ? who : "xrt_set_grow");
        abort();
    }
    s->items = tmp;
}

static inline XrValue xrt_set_item_get(xrt_set_t *s, int64_t index) {
    return xr_typed_get(s->items, (int32_t) index, s->elem_type);
}

static inline void xrt_set_item_set(xrt_set_t *s, int64_t index, XrValue value) {
    (void) xr_typed_set(s->items, (int32_t) index, value, s->elem_type);
}

static inline void xrt_set_item_copy(xrt_set_t *s, int64_t dst, int64_t src) {
    memcpy((uint8_t *) s->items + (size_t) dst * (size_t) s->elem_size,
           (uint8_t *) s->items + (size_t) src * (size_t) s->elem_size, (size_t) s->elem_size);
}

static inline int xrt_set_has(xrt_set_t *s, XrValue value) {
    for (int64_t i = 0; i < s->len; i++)
        if (xrt_key_eq(xrt_set_item_get(s, i), value))
            return 1;
    return 0;
}

static inline int xrt_set_add(xrt_set_t *s, XrValue value) {
    if (xrt_set_has(s, value))
        return 0;
    if (s->len >= s->cap)
        xrt_set_grow(s, "xrt_set_add");
    xrt_set_item_set(s, s->len++, value);
    return 1;
}

static inline int xrt_set_delete(xrt_set_t *s, XrValue value) {
    for (int64_t i = 0; i < s->len; i++) {
        if (xrt_key_eq(xrt_set_item_get(s, i), value)) {
            s->len--;
            if (i != s->len)
                xrt_set_item_copy(s, i, s->len);
            return 1;
        }
    }
    return 0;
}

static inline int xrt_set_has_i64(xrt_set_t *s, int64_t value) {
    if (s->elem_type != XR_ELEM_I64)
        return xrt_set_has(s, XR_FROM_INT(value));
    int64_t *items = (int64_t *) s->items;
    for (int64_t i = 0; i < s->len; i++)
        if (items[i] == value)
            return 1;
    return 0;
}

static inline int xrt_set_add_i64(xrt_set_t *s, int64_t value) {
    if (s->elem_type != XR_ELEM_I64)
        return xrt_set_add(s, XR_FROM_INT(value));
    if (xrt_set_has_i64(s, value))
        return 0;
    if (s->len >= s->cap)
        xrt_set_grow(s, "xrt_set_add_i64");
    ((int64_t *) s->items)[s->len++] = value;
    return 1;
}

static inline int xrt_set_delete_i64(xrt_set_t *s, int64_t value) {
    if (s->elem_type != XR_ELEM_I64)
        return xrt_set_delete(s, XR_FROM_INT(value));
    int64_t *items = (int64_t *) s->items;
    for (int64_t i = 0; i < s->len; i++) {
        if (items[i] == value) {
            items[i] = items[--s->len];
            return 1;
        }
    }
    return 0;
}

#include "xrt_set_direct.inc.c"

static inline void xrt_set_clear(xrt_set_t *s) {
    s->len = 0;
}

static inline XrValue xrt_set_values(xrt_set_t *s) {
    XrValue arr = s->elem_type == XR_ELEM_ANY ? xrt_array_new(s->len)
                                              : xrt_array_new_typed(s->len, s->elem_type);
    for (int64_t i = 0; i < s->len; i++)
        xrt_array_push(arr, xrt_set_item_get(s, i));
    return arr;
}

static inline XrValue xrt_process_new(const char *file, int argc, char **argv, const char *dir) {
    if (argc < 0)
        argc = 0;
    XrValue args = xrt_array_new(argc);
    for (int i = 0; argv && i < argc; i++)
        xrt_array_push(args, xr_box_str(argv[i] ? argv[i] : ""));

    XrValue process = xrt_map_new(4);
    xrt_map_t *m = (xrt_map_t *) process.ptr;
    xrt_map_set(m, xr_box_str("file"), file ? xr_box_str(file) : XR_NULL_VAL);
    xrt_map_set(m, xr_box_str("args"), args);
    xrt_map_set(m, xr_box_str("dir"), dir ? xr_box_str(dir) : XR_NULL_VAL);
    return process;
}

/* =========================================================================
 * Json object runtime (flat field array, O(1) indexed access)
 * ========================================================================= */

typedef struct {
    int64_t field_count;
    XrValue fields[]; /* flexible array of field values */
} xrt_json_t;

static inline XrValue xrt_json_new(int64_t field_count) {
    xrt_json_t *j =
        (xrt_json_t *) XRT_MALLOC(sizeof(xrt_json_t) + (size_t) field_count * sizeof(XrValue));
    if (!j) {
        fprintf(stderr, "xrt_json_new: out of memory\n");
        abort();
    }
    j->field_count = field_count;
    for (int64_t i = 0; i < field_count; i++)
        j->fields[i] = (XrValue) {.i = 0, .tag = XR_TAG_NULL};
    return xr_mkptr(j, XR_TAG_PTR);
}

static inline XrValue xrt_json_get_field(XrValue obj, int field_idx) {
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    if (field_idx >= 0 && field_idx < j->field_count)
        return j->fields[field_idx];
    return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
}

static inline void xrt_json_set_field(XrValue obj, int field_idx, XrValue val) {
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    if (field_idx >= 0 && field_idx < j->field_count)
        j->fields[field_idx] = val;
}

#include "xrt_index_helpers.inc.c"

/* =========================================================================
 * Closure runtime
 * ========================================================================= */

typedef struct xrt_closure {
    void *fn;          // C function pointer
    int nupvals;       // number of captured upvalues
    XrValue upvals[];  // captured values (flexible array)
} xrt_closure_t;

static inline XrValue xrt_closure_new(void *fn, int nupvals) {
    xrt_closure_t *c =
        (xrt_closure_t *) xrt_arc_alloc(sizeof(xrt_closure_t) + (size_t) nupvals * sizeof(XrValue));
    if (!c) {
        fprintf(stderr, "xrt_closure_new: out of memory\n");
        abort();
    }
    c->fn = fn;
    c->nupvals = nupvals;
    for (int i = 0; i < nupvals; i++)
        c->upvals[i] = (XrValue) {.i = 0, .tag = XR_TAG_NULL};
    return xr_mkptr(c, XR_TAG_CLOSURE);
}

typedef struct xrt_cell {
    XrValue value;
} xrt_cell_t;

static inline XrValue xrt_cell_new(XrValue value) {
    xrt_cell_t *cell = (xrt_cell_t *) xrt_arc_alloc(sizeof(xrt_cell_t));
    if (!cell) {
        fprintf(stderr, "xrt_cell_new: out of memory\n");
        abort();
    }
    cell->value = value;
    return xr_mkptr(cell, XR_TAG_CELL);
}

static inline XrValue xrt_cell_get(XrValue cell_value) {
    if (cell_value.tag != XR_TAG_CELL || !cell_value.ptr)
        return cell_value;
    return ((xrt_cell_t *) cell_value.ptr)->value;
}

static inline void xrt_cell_set(XrValue cell_value, XrValue value) {
    if (cell_value.tag != XR_TAG_CELL || !cell_value.ptr)
        return;
    ((xrt_cell_t *) cell_value.ptr)->value = value;
}

static inline XrValue xrt_value_clone_for_coro(XrValue val) {
    switch (val.tag) {
        case XR_TAG_ARRAY: {
            xrt_array_t *src = (xrt_array_t *) val.ptr;
            if (!src)
                return val;
            XrValue dstv = xrt_array_new_typed(src->cap, src->elem_type);
            xrt_array_t *dst = (xrt_array_t *) dstv.ptr;
            dst->len = src->len;
            if (src->elem_type == XR_ELEM_ANY) {
                for (int64_t i = 0; i < src->len; i++) {
                    XrValue item = xr_typed_get(src->data, (int32_t) i, src->elem_type);
                    xr_typed_set(dst->data, (int32_t) i, xrt_value_clone_for_coro(item),
                                 dst->elem_type);
                }
            } else {
                memcpy(dst->data, src->data, (size_t) src->len * (size_t) src->elem_size);
            }
            return dstv;
        }
        case XR_TAG_MAP: {
            xrt_map_t *src = (xrt_map_t *) val.ptr;
            if (!src)
                return val;
            XrValue dstv = xrt_map_is_typed(src)
                               ? xrt_map_new_typed(src->cap, src->key_type, src->value_type)
                               : xrt_map_new(src->cap);
            xrt_map_t *dst = (xrt_map_t *) dstv.ptr;
            if (xrt_map_is_typed(src)) {
                dst->len = src->len;
                memcpy(dst->keys, src->keys, (size_t) src->len * (size_t) src->key_size);
                memcpy(dst->values, src->values, (size_t) src->len * (size_t) src->value_size);
            } else {
                for (int64_t i = 0; i < src->len; i++) {
                    XrValue cloned_key = xrt_value_clone_for_coro(src->entries[i].key);
                    XrValue cloned_val = xrt_value_clone_for_coro(src->entries[i].val);
                    xrt_map_set(dst, cloned_key, cloned_val);
                }
            }
            return dstv;
        }
        case XR_TAG_STRBUF: {
            xrt_strbuf_t *src = (xrt_strbuf_t *) val.ptr;
            if (!src)
                return val;
            XrValue dstv = xrt_strbuf_new();
            xrt_strbuf_t *dst = (xrt_strbuf_t *) dstv.ptr;
            xrt_strbuf_grow(dst, src->len);
            memcpy(dst->buf, src->buf, (size_t) src->len + 1u);
            dst->len = src->len;
            return dstv;
        }
        case XR_TAG_STRUCT_REF: {
            if (!val.ptr)
                return val;
            uint32_t size = *(uint32_t *) val.ptr;
            if (size == 0 || size > (16u * 1024u * 1024u))
                return val;
            void *dst = xrt_arc_alloc(size);
            memcpy(dst, val.ptr, size);
            return xr_mkptr(dst, XR_TAG_STRUCT_REF);
        }
        default:
            return val;
    }
}

#endif  // XRT_COLL_H
