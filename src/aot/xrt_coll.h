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
#include "xrt_arc.h" // xrt_str_alloc used by xrt_strbuf_finish

/* =========================================================================
 * Array runtime
 * ========================================================================= */

typedef struct { int64_t len; int64_t cap; XrtValue *data; } xrt_array_t;

static inline XrtValue xrt_array_new(int64_t cap) {
    if (cap < 4) cap = 4;
    xrt_array_t *a = (xrt_array_t *)malloc(sizeof(xrt_array_t));
    a->len  = 0;
    a->cap  = cap;
    a->data = (XrtValue *)calloc((size_t)cap, sizeof(XrtValue));
    return (XrtValue){.ptr = a, .tag = XRT_TAG_ARRAY};
}

static inline void xrt_array_push(XrtValue arr, XrtValue val) {
    xrt_array_t *a = (xrt_array_t *)arr.ptr;
    if (a->len >= a->cap) {
        a->cap  *= 2;
        a->data  = (XrtValue *)realloc(a->data, (size_t)a->cap * sizeof(XrtValue));
    }
    a->data[a->len++] = val;
}

static inline void xrt_array_push_i(XrtValue arr, int64_t val) {
    xrt_array_t *a = (xrt_array_t *)arr.ptr;
    if (a->len >= a->cap) {
        a->cap  *= 2;
        a->data  = (XrtValue *)realloc(a->data, (size_t)a->cap * sizeof(XrtValue));
    }
    a->data[a->len++] = (XrtValue){.i = val, .tag = XRT_TAG_I64};
}

static inline void xrt_array_push_f(XrtValue arr, double val) {
    xrt_array_t *a = (xrt_array_t *)arr.ptr;
    if (a->len >= a->cap) {
        a->cap  *= 2;
        a->data  = (XrtValue *)realloc(a->data, (size_t)a->cap * sizeof(XrtValue));
    }
    a->data[a->len++] = (XrtValue){.f = val, .tag = XRT_TAG_F64};
}

static inline int64_t xrt_array_len(XrtValue arr) {
    return ((xrt_array_t *)arr.ptr)->len;
}

// Typed array access — raw payload, no tag check
static inline int64_t xrt_tarray_get(XrtValue arr, int64_t idx) {
    xrt_array_t *a = (xrt_array_t *)arr.ptr;
    if (idx < 0 || idx >= a->len) return 0;
    return a->data[idx].i;
}

static inline void xrt_tarray_set(XrtValue arr, int64_t idx, int64_t val) {
    xrt_array_t *a = (xrt_array_t *)arr.ptr;
    if (idx < 0 || idx >= a->len) return;
    a->data[idx] = (XrtValue){.i = val, .tag = XRT_TAG_I64};
}

static inline int64_t xrt_array_get_i(XrtValue arr, int64_t idx) {
    xrt_array_t *a = (xrt_array_t *)arr.ptr;
    if ((uint64_t)idx >= (uint64_t)a->len) return 0;
    return a->data[idx].i;
}

static inline double xrt_array_get_f(XrtValue arr, int64_t idx) {
    xrt_array_t *a = (xrt_array_t *)arr.ptr;
    if ((uint64_t)idx >= (uint64_t)a->len) return 0.0;
    return a->data[idx].f;
}

static inline void xrt_array_set_i(XrtValue arr, int64_t idx, int64_t val) {
    xrt_array_t *a = (xrt_array_t *)arr.ptr;
    if ((uint64_t)idx >= (uint64_t)a->len) return;
    a->data[idx] = (XrtValue){.i = val, .tag = XRT_TAG_I64};
}

static inline void xrt_array_set_f(XrtValue arr, int64_t idx, double val) {
    xrt_array_t *a = (xrt_array_t *)arr.ptr;
    if ((uint64_t)idx >= (uint64_t)a->len) return;
    a->data[idx] = (XrtValue){.f = val, .tag = XRT_TAG_F64};
}

/* =========================================================================
 * StringBuilder runtime
 * ========================================================================= */

typedef struct { char *buf; int64_t len; int64_t cap; } xrt_strbuf_t;

static inline XrtValue xrt_strbuf_new(void) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *)malloc(sizeof(xrt_strbuf_t));
    sb->cap = 64;
    sb->len = 0;
    sb->buf = (char *)malloc(64);
    sb->buf[0] = 0;
    return (XrtValue){.ptr = sb, .tag = XRT_TAG_STRBUF};
}

static inline void xrt_strbuf_grow(xrt_strbuf_t *sb, int64_t need) {
    while (sb->len + need >= sb->cap) sb->cap *= 2;
    sb->buf = (char *)realloc(sb->buf, (size_t)sb->cap);
}

static inline void xrt_strbuf_append(XrtValue sbv, XrtValue val) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *)sbv.ptr;
    if (val.tag == XRT_TAG_STR || val.tag == XRT_TAG_STR_ARC) {
        const char *s = (const char *)val.ptr;
        int64_t slen  = (int64_t)strlen(s);
        xrt_strbuf_grow(sb, slen);
        memcpy(sb->buf + sb->len, s, (size_t)slen);
        sb->len += slen;
        sb->buf[sb->len] = 0;
    } else if (val.tag == XRT_TAG_I64) {
        char tmp[24];
        int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)val.i);
        xrt_strbuf_grow(sb, n);
        memcpy(sb->buf + sb->len, tmp, (size_t)n);
        sb->len += n;
        sb->buf[sb->len] = 0;
    } else if (val.tag == XRT_TAG_F64) {
        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), "%g", val.f);
        xrt_strbuf_grow(sb, n);
        memcpy(sb->buf + sb->len, tmp, (size_t)n);
        sb->len += n;
        sb->buf[sb->len] = 0;
    } else if (val.tag == XRT_TAG_BOOL) {
        const char *bs = val.i ? "true" : "false";
        int blen = val.i ? 4 : 5;
        xrt_strbuf_grow(sb, blen);
        memcpy(sb->buf + sb->len, bs, (size_t)blen);
        sb->len += blen;
        sb->buf[sb->len] = 0;
    } else if (val.tag == XRT_TAG_NULL) {
        xrt_strbuf_grow(sb, 4);
        memcpy(sb->buf + sb->len, "null", 4);
        sb->len += 4;
        sb->buf[sb->len] = 0;
    }
}

static inline XrtValue xrt_strbuf_finish(XrtValue sbv) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *)sbv.ptr;
    XrtValue v = xrt_str_alloc((size_t)sb->len);
    memcpy((char *)v.ptr, sb->buf, (size_t)(sb->len + 1));
    return v;
}

/* =========================================================================
 * Map runtime  (linear-probing, O(n) — suitable for small maps)
 * ========================================================================= */

typedef struct { XrtValue key; XrtValue val; } xrt_map_entry_t;
typedef struct { int64_t len; int64_t cap; xrt_map_entry_t *entries; } xrt_map_t;

static inline XrtValue xrt_map_new(int64_t cap) {
    if (cap < 8) cap = 8;
    xrt_map_t *m  = (xrt_map_t *)malloc(sizeof(xrt_map_t));
    m->len     = 0;
    m->cap     = cap;
    m->entries = (xrt_map_entry_t *)calloc((size_t)cap, sizeof(xrt_map_entry_t));
    return (XrtValue){.ptr = m, .tag = XRT_TAG_MAP};
}

static inline int xrt_key_eq(XrtValue a, XrtValue b) {
    if (a.tag != b.tag) return 0;
    if (a.tag == XRT_TAG_I64) return a.i == b.i;
    if (a.tag == XRT_TAG_STR)
        return strcmp((const char *)a.ptr, (const char *)b.ptr) == 0;
    return a.ptr == b.ptr;
}

static inline XrtValue xrt_map_get(xrt_map_t *m, XrtValue key) {
    for (int64_t i = 0; i < m->len; i++)
        if (xrt_key_eq(m->entries[i].key, key)) return m->entries[i].val;
    return (XrtValue){.i = 0, .tag = XRT_TAG_NULL};
}

static inline void xrt_map_set(xrt_map_t *m, XrtValue key, XrtValue val) {
    for (int64_t i = 0; i < m->len; i++) {
        if (xrt_key_eq(m->entries[i].key, key)) {
            m->entries[i].val = val;
            return;
        }
    }
    if (m->len >= m->cap) {
        m->cap    *= 2;
        m->entries = (xrt_map_entry_t *)realloc(
            m->entries, (size_t)m->cap * sizeof(xrt_map_entry_t));
    }
    m->entries[m->len].key = key;
    m->entries[m->len].val = val;
    m->len++;
}

/* =========================================================================
 * Generic index get / set  (Array[i] and Map[key])
 * ========================================================================= */

static inline XrtValue xrt_index_get(XrtValue obj, XrtValue key) {
    if (obj.tag == XRT_TAG_ARRAY && key.tag == XRT_TAG_I64) {
        xrt_array_t *a = (xrt_array_t *)obj.ptr;
        int64_t idx    = key.i;
        if (idx < 0) idx += a->len;
        if (idx >= 0 && idx < a->len) return a->data[idx];
    } else if (obj.tag == XRT_TAG_MAP) {
        return xrt_map_get((xrt_map_t *)obj.ptr, key);
    }
    return (XrtValue){.i = 0, .tag = XRT_TAG_NULL};
}

static inline void xrt_index_set(XrtValue obj, XrtValue key, XrtValue val) {
    if (obj.tag == XRT_TAG_ARRAY && key.tag == XRT_TAG_I64) {
        xrt_array_t *a = (xrt_array_t *)obj.ptr;
        int64_t idx    = key.i;
        if (idx < 0) idx += a->len;
        if (idx >= 0 && idx < a->len) a->data[idx] = val;
    } else if (obj.tag == XRT_TAG_MAP) {
        xrt_map_set((xrt_map_t *)obj.ptr, key, val);
    }
}

/* =========================================================================
 * Closure runtime
 * ========================================================================= */

typedef struct xrt_closure {
    void     *fn; // C function pointer
    int       nupvals; // number of captured upvalues
    XrtValue  upvals[]; // captured values (flexible array)
} xrt_closure_t;

static inline XrtValue xrt_closure_new(void *fn, int nupvals) {
    xrt_closure_t *c = (xrt_closure_t *)malloc(
        sizeof(xrt_closure_t) + (size_t)nupvals * sizeof(XrtValue));
    c->fn      = fn;
    c->nupvals = nupvals;
    for (int i = 0; i < nupvals; i++)
        c->upvals[i] = (XrtValue){.i = 0, .tag = XRT_TAG_NULL};
    return (XrtValue){.ptr = c, .tag = XRT_TAG_CLOSURE};
}

#endif // XRT_COLL_H
