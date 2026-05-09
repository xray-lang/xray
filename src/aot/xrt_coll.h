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
#include "../shared/xr_elem_type.h"
#include "../shared/xr_typed_ops.h"

/* =========================================================================
 * Array runtime
 * ========================================================================= */

typedef struct {
    int64_t  len;
    int64_t  cap;
    void    *data;        /* uint8_t[] / int64_t[] / XrValue[] — depends on elem_type */
    uint8_t  elem_type;   /* XR_ELEM_ANY / XR_ELEM_U8 / ... */
    uint8_t  elem_size;   /* cached bytes per element */
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
    a->elem_size = (uint8_t)sizeof(XrValue);
    a->data = XRT_CALLOC((size_t) cap, sizeof(XrValue));
    if (!a->data) {
        fprintf(stderr, "xrt_array_new: out of memory\n");
        abort();
    }
    return xr_mkptr(a, XR_TAG_ARRAY);
}

static inline XrValue xrt_array_new_typed(int64_t cap, uint8_t etype) {
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
    a->data = XRT_CALLOC((size_t) cap, (size_t) a->elem_size);
    if (!a->data) {
        fprintf(stderr, "xrt_array_new_typed: out of memory\n");
        abort();
    }
    return xr_mkptr(a, XR_TAG_ARRAY);
}

static inline void xrt_array_push(XrValue arr, XrValue val) {
    xrt_array_t *a = (xrt_array_t *) arr.ptr;
    if (a->len >= a->cap) {
        a->cap *= 2;
        void *tmp = XRT_REALLOC(a->data, (size_t) a->cap * (size_t) a->elem_size);
        if (!tmp) {
            fprintf(stderr, "xrt_array_push: out of memory\n");
            abort();
        }
        a->data = tmp;
    }
    xr_typed_set(a->data, (int32_t)a->len, val, a->elem_type);
    a->len++;
}

static inline int64_t xrt_array_len(XrValue arr) {
    return ((xrt_array_t *) arr.ptr)->len;
}


/* Stack-allocated array: header on stack, data buffer on stack via alloca.
 * Used for NO_ESCAPE arrays (escape analysis optimization).
 * The returned XrValue is valid only within the current function scope. */
#ifndef xrt_array_stack_new
#define xrt_array_stack_new(cap_expr) ({                                    \
    int64_t _cap = (cap_expr);                                             \
    if (_cap < 4) _cap = 4;                                                \
    xrt_array_t *_a = (xrt_array_t *) __builtin_alloca(                   \
        sizeof(xrt_array_t) + (size_t)_cap * sizeof(XrValue));            \
    _a->len = 0;                                                           \
    _a->cap = _cap;                                                        \
    _a->elem_type = XR_ELEM_ANY;                                          \
    _a->elem_size = (uint8_t)sizeof(XrValue);                             \
    _a->data = (void *)((char *)_a + sizeof(xrt_array_t));               \
    memset(_a->data, 0, (size_t)_cap * sizeof(XrValue));                  \
    xr_mkptr(_a, XR_TAG_ARRAY);                                           \
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
    if (a.tag == XR_TAG_STR)
        return strcmp((const char *) a.ptr, (const char *) b.ptr) == 0;
    return a.ptr == b.ptr;
}

static inline XrValue xrt_map_get(xrt_map_t *m, XrValue key) {
    for (int64_t i = 0; i < m->len; i++)
        if (xrt_key_eq(m->entries[i].key, key))
            return m->entries[i].val;
    return (XrValue){.i = 0, .tag = XR_TAG_NULL};
}

static inline void xrt_map_set(xrt_map_t *m, XrValue key, XrValue val) {
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
 * Json object runtime (flat field array, O(1) indexed access)
 * ========================================================================= */

typedef struct {
    int64_t field_count;
    XrValue fields[];       /* flexible array of field values */
} xrt_json_t;

static inline XrValue xrt_json_new(int64_t field_count) {
    xrt_json_t *j = (xrt_json_t *) XRT_MALLOC(
        sizeof(xrt_json_t) + (size_t)field_count * sizeof(XrValue));
    if (!j) {
        fprintf(stderr, "xrt_json_new: out of memory\n");
        abort();
    }
    j->field_count = field_count;
    for (int64_t i = 0; i < field_count; i++)
        j->fields[i] = (XrValue){.i = 0, .tag = XR_TAG_NULL};
    return xr_mkptr(j, XR_TAG_PTR);
}

static inline XrValue xrt_json_get_field(XrValue obj, int field_idx) {
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    if (field_idx >= 0 && field_idx < j->field_count)
        return j->fields[field_idx];
    return (XrValue){.i = 0, .tag = XR_TAG_NULL};
}

static inline void xrt_json_set_field(XrValue obj, int field_idx, XrValue val) {
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    if (field_idx >= 0 && field_idx < j->field_count)
        j->fields[field_idx] = val;
}

/* =========================================================================
 * Generic index get / set  (Array[i] and Map[key])
 * ========================================================================= */

static inline XrValue xrt_index_get(XrValue obj, XrValue key) {
    if (obj.tag == XR_TAG_ARRAY && key.tag == XR_TAG_I64) {
        xrt_array_t *a = (xrt_array_t *) obj.ptr;
        int64_t idx = key.i;
        if (idx < 0)
            idx += a->len;
        if (idx >= 0 && idx < a->len)
            return xr_typed_get(a->data, (int32_t)idx, a->elem_type);
    } else if (obj.tag == XR_TAG_MAP) {
        return xrt_map_get((xrt_map_t *) obj.ptr, key);
    }
    return (XrValue){.i = 0, .tag = XR_TAG_NULL};
}

static inline void xrt_index_set(XrValue obj, XrValue key, XrValue val) {
    if (obj.tag == XR_TAG_ARRAY && key.tag == XR_TAG_I64) {
        xrt_array_t *a = (xrt_array_t *) obj.ptr;
        int64_t idx = key.i;
        if (idx < 0)
            idx += a->len;
        if (idx >= 0 && idx < a->len) {
            xr_typed_set(a->data, (int32_t)idx, val, a->elem_type);
        } else if (idx >= 0) {
            while (a->len < idx)
                xrt_array_push(obj, XR_NULL_VAL);
            xrt_array_push(obj, val);
        }
    } else if (obj.tag == XR_TAG_MAP) {
        xrt_map_set((xrt_map_t *) obj.ptr, key, val);
    }
}

/* =========================================================================
 * Closure runtime
 * ========================================================================= */

typedef struct xrt_closure {
    void *fn;           // C function pointer
    int nupvals;        // number of captured upvalues
    XrValue upvals[];  // captured values (flexible array)
} xrt_closure_t;

static inline XrValue xrt_closure_new(void *fn, int nupvals) {
    xrt_closure_t *c =
        (xrt_closure_t *) XRT_MALLOC(sizeof(xrt_closure_t) + (size_t) nupvals * sizeof(XrValue));
    if (!c) {
        fprintf(stderr, "xrt_closure_new: out of memory\n");
        abort();
    }
    c->fn = fn;
    c->nupvals = nupvals;
    for (int i = 0; i < nupvals; i++)
        c->upvals[i] = (XrValue){.i = 0, .tag = XR_TAG_NULL};
    return xr_mkptr(c, XR_TAG_CLOSURE);
}

#endif  // XRT_COLL_H
