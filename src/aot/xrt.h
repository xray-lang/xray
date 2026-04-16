/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt.h - Self-contained AOT runtime (no VM dependency for arithmetic/print)
 *
 * KEY CONCEPT:
 *   Single header providing all runtime primitives for AOT-generated C code:
 *   value representation, boxing/unboxing, inline arithmetic/comparison/print,
 *   collections (Array, Map, StringBuilder), closure runtime, method dispatch,
 *   ARC memory management, and string helpers.
 *
 *   All runtime primitives are fully self-contained (no extern VM dependency).
 *   Arithmetic, comparison, print, method dispatch, property access, and
 *   closure upvalue access are all inlined in this header.
 *
 * RELATED MODULES:
 *   - xcgen.c: generates C code that includes this header
 */

#ifndef XRT_H
#define XRT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdarg.h>
#include <math.h>

/* =========================================================================
 * Value representation
 * ========================================================================= */

typedef union XrtValue {
    struct { int64_t  i;   uint8_t _pi[7]; uint8_t tag; };
    struct { double   f;   uint8_t _pf[7]; uint8_t _tf; };
    struct { void    *ptr; uint32_t heap_type; uint8_t _pp[3]; uint8_t _tp; };
    uint8_t raw[16];
} XrtValue;

#define XRT_TAG_NULL    0
#define XRT_TAG_BOOL    1
// 2 reserved (was XRT_TAG_FALSE)
#define XRT_TAG_I64     6
#define XRT_TAG_F64     12
#define XRT_TAG_PTR     13
#define XRT_TAG_STR     14 // static / literal string (no ARC)
#define XRT_TAG_ARRAY   15
#define XRT_TAG_MAP     16
#define XRT_TAG_STRBUF  17
#define XRT_TAG_CLOSURE 18
#define XRT_TAG_STR_ARC 19 // heap string managed by ARC (xrt_arc_alloc)

// Treat both STR and STR_ARC as strings in generic operations
#define XRT_IS_STR(v)  ((v).tag == XRT_TAG_STR || (v).tag == XRT_TAG_STR_ARC)

/* =========================================================================
 * Boxing / unboxing
 * ========================================================================= */

static inline XrtValue xrt_box_int(int64_t v) {
    return (XrtValue){.i = v, .tag = XRT_TAG_I64};
}

static inline XrtValue xrt_box_float(double v) {
    return (XrtValue){.f = v, .tag = XRT_TAG_F64};
}

static inline XrtValue xrt_box_bool(int64_t v) {
    XrtValue r;
    r.tag = XRT_TAG_BOOL;
    r.i   = v ? 1 : 0;
    return r;
}

static inline XrtValue xrt_box_str(const char *s) {
    return (XrtValue){.ptr = (void *)s, .tag = XRT_TAG_STR};
}

static inline int64_t      xrt_unbox_int(XrtValue v)   { return v.i; }
static inline double       xrt_unbox_float(XrtValue v)  { return v.f; }
static inline const char  *xrt_unbox_str(XrtValue v)    { return (const char *)v.ptr; }

/* =========================================================================
 * String helpers
 * ========================================================================= */

static inline const char *xrt_to_cstr(XrtValue v, char *buf, size_t bufsz) {
    switch (v.tag) {
        case XRT_TAG_STR:
        case XRT_TAG_STR_ARC: return (const char *)v.ptr;
        case XRT_TAG_I64:   snprintf(buf, bufsz, "%lld", (long long)v.i); return buf;
        case XRT_TAG_F64:   snprintf(buf, bufsz, "%g",   v.f);            return buf;
        case XRT_TAG_BOOL:  return v.i ? "true" : "false";
        case XRT_TAG_NULL:  return "null";
        default:            snprintf(buf, bufsz, "<object@%p>", v.ptr);   return buf;
    }
}

// xrt_str_alloc/xrt_str_concat: defined after xrt_arc_alloc below
static inline XrtValue xrt_str_alloc(size_t len);
static inline XrtValue xrt_str_concat(const char *sa, const char *sb);

/* =========================================================================
 * Tagged arithmetic — all inline, no extern dependency
 * ========================================================================= */

static inline XrtValue xrt_add(XrtValue a, XrtValue b) {
    if (a.tag == XRT_TAG_I64 && b.tag == XRT_TAG_I64)
        return xrt_box_int(a.i + b.i);
    if (XRT_IS_STR(a) || XRT_IS_STR(b)) {
        char ba[64], bb[64];
        return xrt_str_concat(xrt_to_cstr(a, ba, sizeof(ba)),
                              xrt_to_cstr(b, bb, sizeof(bb)));
    }
    double fa = a.tag == XRT_TAG_I64 ? (double)a.i : a.f;
    double fb = b.tag == XRT_TAG_I64 ? (double)b.i : b.f;
    return xrt_box_float(fa + fb);
}

static inline int64_t xrt_eq(XrtValue a, XrtValue b) {
    // Normalize STR_ARC to STR for comparison
    uint32_t ta = (a.tag == XRT_TAG_STR_ARC) ? XRT_TAG_STR : a.tag;
    uint32_t tb = (b.tag == XRT_TAG_STR_ARC) ? XRT_TAG_STR : b.tag;
    if (ta != tb) return 0;
    if (ta == XRT_TAG_I64) return a.i == b.i;
    if (ta == XRT_TAG_F64) return a.f == b.f;
    if (ta == XRT_TAG_STR)
        return strcmp((const char *)a.ptr, (const char *)b.ptr) == 0;
    return a.ptr == b.ptr;
}

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

/* =========================================================================
 * Builtin method symbol IDs  (must match xsymbol_table.h)
 * ========================================================================= */

#define XRT_SYM_SIZE        1
#define XRT_SYM_LENGTH      2
#define XRT_SYM_IS_EMPTY    3
#define XRT_SYM_HAS         4
#define XRT_SYM_GET         5
#define XRT_SYM_SET         6
#define XRT_SYM_DELETE      7
#define XRT_SYM_CLEAR       8
#define XRT_SYM_KEYS        9
#define XRT_SYM_VALUES      10
#define XRT_SYM_PUSH        49
#define XRT_SYM_POP         50
#define XRT_SYM_JOIN        53
#define XRT_SYM_REVERSE     54
#define XRT_SYM_SLICE       17
#define XRT_SYM_INDEXOF     20
#define XRT_SYM_CONTAINS    21
#define XRT_SYM_STARTSWITH  22
#define XRT_SYM_ENDSWITH    23
#define XRT_SYM_TOLOWER     24
#define XRT_SYM_TOUPPER     25
#define XRT_SYM_TRIM        26
#define XRT_SYM_SPLIT       27
#define XRT_SYM_REPLACE     28
#define XRT_SYM_REPEAT      30
#define XRT_SYM_FLOOR       59
#define XRT_SYM_CEIL        60
#define XRT_SYM_ROUND       61
#define XRT_SYM_ABS         62
#define XRT_SYM_SQRT        63
#define XRT_SYM_POW         64
#define XRT_SYM_TOFIXED     65
#define XRT_SYM_TOSTRING    86

/* =========================================================================
 * toString helper
 * ========================================================================= */

static XrtValue xrt_tostring(XrtValue val, int slot_hint) {
    if (slot_hint == 1 || val.tag == XRT_TAG_I64) {
        char tmp[32]; int n = 0;
        int64_t v = val.i;
        int64_t t = v;
        if (t < 0) { tmp[n++] = '-'; t = -t; }
        if (t == 0) { tmp[n++] = '0'; } else {
            char rev[20]; int r = 0;
            while (t > 0) { rev[r++] = '0' + (char)(t % 10); t /= 10; }
            while (r > 0) tmp[n++] = rev[--r];
        }
        tmp[n] = 0;
        return xrt_str_concat(tmp, "");
    }
    if (slot_hint == 2 || val.tag == XRT_TAG_F64) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%g", val.f);
        return xrt_str_concat(tmp, "");
    }
    if (val.tag == XRT_TAG_STR || val.tag == XRT_TAG_STR_ARC) return val;
    if (val.tag == XRT_TAG_NULL)  return xrt_box_str("null");
    if (val.tag == XRT_TAG_BOOL)  return xrt_box_str(val.i ? "true" : "false");
    return xrt_box_str("[object]");
}

/* =========================================================================
 * Fixed-arity method dispatch (no varargs, inlinable by C compiler)
 *
 * Replaces the old varargs xrt_invoke_method. Three fixed-arity versions
 * for 0, 1, and 2 arguments. The C compiler can inline these because
 * they have known signatures (unlike varargs which block inlining).
 * ========================================================================= */

static inline XrtValue xrt_method_0(XrtValue recv, int sym) {
    if (XRT_IS_STR(recv)) {
        const char *s = (const char *)recv.ptr;
        int64_t slen = (int64_t)strlen(s);
        if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE) return xrt_box_int(slen);
        if (sym == XRT_SYM_IS_EMPTY) return xrt_box_int(slen == 0);
        if (sym == XRT_SYM_TOSTRING) return recv;
    }
    if (recv.tag == XRT_TAG_ARRAY) {
        xrt_array_t *a = (xrt_array_t *)recv.ptr;
        if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE) return xrt_box_int(a->len);
        if (sym == XRT_SYM_IS_EMPTY) return xrt_box_int(a->len == 0);
        if (sym == XRT_SYM_POP && a->len > 0) return a->data[--a->len];
        if (sym == XRT_SYM_REVERSE) {
            for (int64_t i = 0, j = a->len - 1; i < j; i++, j--) {
                XrtValue tmp = a->data[i]; a->data[i] = a->data[j]; a->data[j] = tmp;
            }
            return recv;
        }
    }
    if (recv.tag == XRT_TAG_MAP) {
        xrt_map_t *m = (xrt_map_t *)recv.ptr;
        if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE) return xrt_box_int(m->len);
        if (sym == XRT_SYM_IS_EMPTY) return xrt_box_int(m->len == 0);
    }
    if (recv.tag == XRT_TAG_STRBUF) return xrt_strbuf_finish(recv);
    if (recv.tag == XRT_TAG_I64) {
        if (sym == XRT_SYM_ABS) return xrt_box_int(recv.i < 0 ? -recv.i : recv.i);
        if (sym == XRT_SYM_TOSTRING) return xrt_tostring(recv, 1);
    }
    if (recv.tag == XRT_TAG_F64) {
        double v = recv.f;
        if (sym == XRT_SYM_FLOOR)  return xrt_box_float(floor(v));
        if (sym == XRT_SYM_CEIL)   return xrt_box_float(ceil(v));
        if (sym == XRT_SYM_ROUND)  return xrt_box_float(round(v));
        if (sym == XRT_SYM_ABS)    return xrt_box_float(fabs(v));
        if (sym == XRT_SYM_SQRT)   return xrt_box_float(sqrt(v));
    }
    return (XrtValue){.i = 0, .tag = XRT_TAG_NULL};
}

static inline XrtValue xrt_method_1(XrtValue recv, int sym, XrtValue arg0) {
    if (XRT_IS_STR(recv)) {
        const char *s = (const char *)recv.ptr;
        int64_t slen = (int64_t)strlen(s);
        if (sym == XRT_SYM_CONTAINS && XRT_IS_STR(arg0))
            return xrt_box_int(strstr(s, (const char *)arg0.ptr) ? 1 : 0);
        if (sym == XRT_SYM_INDEXOF && XRT_IS_STR(arg0)) {
            const char *p = strstr(s, (const char *)arg0.ptr);
            return xrt_box_int(p ? (int64_t)(p - s) : -1);
        }
        if (sym == XRT_SYM_SLICE && arg0.tag == XRT_TAG_I64) {
            int64_t start = arg0.i;
            if (start < 0) start += slen;
            if (start < 0) start = 0;
            if (start >= slen) return xrt_box_str("");
            int64_t rlen = slen - start;
            XrtValue sv = xrt_str_alloc((size_t)rlen);
            memcpy((char *)sv.ptr, s + start, (size_t)rlen);
            ((char *)sv.ptr)[rlen] = 0;
            return sv;
        }
        if (sym == XRT_SYM_STARTSWITH && XRT_IS_STR(arg0)) {
            const char *p = (const char *)arg0.ptr;
            size_t plen = strlen(p);
            return xrt_box_int((size_t)slen >= plen && memcmp(s, p, plen) == 0);
        }
        if (sym == XRT_SYM_ENDSWITH && XRT_IS_STR(arg0)) {
            const char *p = (const char *)arg0.ptr;
            size_t plen = strlen(p);
            return xrt_box_int((size_t)slen >= plen && memcmp(s + slen - plen, p, plen) == 0);
        }
    }
    if (recv.tag == XRT_TAG_ARRAY) {
        if (sym == XRT_SYM_PUSH) {
            xrt_array_push(recv, arg0);
            return (XrtValue){.i = 0, .tag = XRT_TAG_NULL};
        }
    }
    if (recv.tag == XRT_TAG_MAP) {
        xrt_map_t *m = (xrt_map_t *)recv.ptr;
        if (sym == XRT_SYM_GET) return xrt_map_get(m, arg0);
        if (sym == XRT_SYM_HAS) {
            for (int64_t i = 0; i < m->len; i++)
                if (xrt_key_eq(m->entries[i].key, arg0)) return xrt_box_int(1);
            return xrt_box_int(0);
        }
        if (sym == XRT_SYM_DELETE) {
            for (int64_t i = 0; i < m->len; i++) {
                if (xrt_key_eq(m->entries[i].key, arg0)) {
                    m->entries[i] = m->entries[--m->len];
                    return xrt_box_int(1);
                }
            }
            return xrt_box_int(0);
        }
    }
    if (recv.tag == XRT_TAG_F64 && sym == XRT_SYM_POW) {
        double exp = (arg0.tag == XRT_TAG_F64) ? arg0.f : (double)arg0.i;
        return xrt_box_float(pow(recv.f, exp));
    }
    return (XrtValue){.i = 0, .tag = XRT_TAG_NULL};
}

static inline XrtValue xrt_method_2(XrtValue recv, int sym, XrtValue arg0, XrtValue arg1) {
    if (XRT_IS_STR(recv) && sym == XRT_SYM_SLICE) {
        const char *s = (const char *)recv.ptr;
        int64_t slen = (int64_t)strlen(s);
        int64_t start = (arg0.tag == XRT_TAG_I64) ? arg0.i : 0;
        int64_t end   = (arg1.tag == XRT_TAG_I64) ? arg1.i : slen;
        if (start < 0) start += slen;
        if (end   < 0) end   += slen;
        if (start < 0) start = 0;
        if (end > slen) end = slen;
        if (start >= end) return xrt_box_str("");
        int64_t rlen = end - start;
        XrtValue sv = xrt_str_alloc((size_t)rlen);
        memcpy((char *)sv.ptr, s + start, (size_t)rlen);
        ((char *)sv.ptr)[rlen] = 0;
        return sv;
    }
    if (recv.tag == XRT_TAG_MAP && sym == XRT_SYM_SET) {
        xrt_map_set((xrt_map_t *)recv.ptr, arg0, arg1);
        return (XrtValue){.i = 0, .tag = XRT_TAG_NULL};
    }
    return (XrtValue){.i = 0, .tag = XRT_TAG_NULL};
}

/* =========================================================================
 * Inline property access (replaces extern xrt_vm_getprop for known types)
 *
 * Handles .length and .isEmpty for Array, Map, String.
 * Returns XRT_TAG_NULL for unknown properties.
 * ========================================================================= */

static inline XrtValue xrt_getprop(XrtValue obj, int64_t symbol_id) {
    if (obj.tag == XRT_TAG_ARRAY) {
        xrt_array_t *a = (xrt_array_t *)obj.ptr;
        if (symbol_id == XRT_SYM_LENGTH || symbol_id == XRT_SYM_SIZE)
            return xrt_box_int(a->len);
        if (symbol_id == XRT_SYM_IS_EMPTY)
            return xrt_box_int(a->len == 0);
    }
    if (obj.tag == XRT_TAG_MAP) {
        xrt_map_t *m = (xrt_map_t *)obj.ptr;
        if (symbol_id == XRT_SYM_LENGTH || symbol_id == XRT_SYM_SIZE)
            return xrt_box_int(m->len);
        if (symbol_id == XRT_SYM_IS_EMPTY)
            return xrt_box_int(m->len == 0);
    }
    if (XRT_IS_STR(obj)) {
        const char *s = (const char *)obj.ptr;
        if (symbol_id == XRT_SYM_LENGTH || symbol_id == XRT_SYM_SIZE)
            return xrt_box_int((int64_t)strlen(s));
        if (symbol_id == XRT_SYM_IS_EMPTY)
            return xrt_box_int(s[0] == '\0');
    }
    return (XrtValue){.i = 0, .tag = XRT_TAG_NULL};
}

/* =========================================================================
 * ARC (Automatic Reference Counting)
 *
 * Object layout (bytes preceding user data):
 *   [XrtArcHdr][  user data  ]
 *    ^--- hdr pointer (via XRT_ARC_HDR macro)
 *
 * rc = 0 means freed. XRT_ARC_IMMORTAL prevents retain/release.
 * xrt_arc_deinit is called once when rc drops to 0 and HAS_DEINIT is set.
 * ========================================================================= */

typedef struct {
    uint32_t rc; // reference count (non-atomic, single-coroutine)
    uint16_t flags; // XRT_ARC_* flags
    uint16_t type; // object type tag for deinit dispatch
} XrtArcHdr;

#define XRT_ARC_HDR(p)       ((XrtArcHdr *)((char *)(p) - sizeof(XrtArcHdr)))
#define XRT_ARC_IMMORTAL     (1u << 0)
#define XRT_ARC_HAS_DEINIT   (1u << 1)

/* =========================================================================
 * Bump allocator for ARC objects
 *
 * Replaces calloc in xrt_arc_alloc for allocation-heavy workloads.
 * Objects are never individually freed — the entire arena is released
 * at program exit via xrt_bump_destroy().
 *
 * This is always compiled in; xrt_arc_alloc picks the bump path when
 * xrt_bump_enabled is set (default: on). Individual objects still carry
 * an XrtArcHdr so retain/release/deinit semantics are preserved;
 * the actual free() in xrt_arc_release becomes a no-op for bump objects.
 * ========================================================================= */

#define XRT_BUMP_BLOCK_SIZE  (2u * 1024u * 1024u) // 2 MB per block
#define XRT_ARC_BUMP         (1u << 2) // object was bump-allocated

typedef struct XrtBumpBlock {
    struct XrtBumpBlock *next;
    char data[];
} XrtBumpBlock;

static char         *xrt_bump_cursor;
static char         *xrt_bump_end;
static XrtBumpBlock *xrt_bump_blocks;
static int           xrt_bump_enabled = 0; // 0 = calloc (safe default); 1 = bump (fast, no per-object free)

static void xrt_bump_new_block(size_t min_size) {
    size_t bsize = XRT_BUMP_BLOCK_SIZE;
    if (min_size > bsize) bsize = min_size;
    XrtBumpBlock *b = (XrtBumpBlock *)malloc(sizeof(XrtBumpBlock) + bsize);
    if (!b) { fprintf(stderr, "xrt_bump: out of memory\n"); abort(); }
    b->next = xrt_bump_blocks;
    xrt_bump_blocks = b;
    xrt_bump_cursor = b->data;
    xrt_bump_end    = b->data + bsize;
}

static inline void *xrt_bump_alloc(size_t size) {
    if (__builtin_expect(xrt_bump_cursor + size <= xrt_bump_end, 1)) {
        void *p = xrt_bump_cursor;
        xrt_bump_cursor += size;
        return p;
    }
    xrt_bump_new_block(size);
    void *p = xrt_bump_cursor;
    xrt_bump_cursor += size;
    return p;
}

static void xrt_bump_destroy(void) {
    XrtBumpBlock *b = xrt_bump_blocks;
    while (b) {
        XrtBumpBlock *next = b->next;
        free(b);
        b = next;
    }
    xrt_bump_blocks  = NULL;
    xrt_bump_cursor  = NULL;
    xrt_bump_end     = NULL;
}

static inline void *xrt_arc_alloc(size_t obj_size) {
    obj_size = (obj_size + 7u) & ~(size_t)7u;
    size_t total = sizeof(XrtArcHdr) + obj_size;
    XrtArcHdr *hdr;
    if (__builtin_expect(xrt_bump_enabled, 1)) {
        hdr = (XrtArcHdr *)xrt_bump_alloc(total);
        memset(hdr, 0, total);
        hdr->flags = XRT_ARC_BUMP; // mark as bump-allocated
    } else {
        hdr = (XrtArcHdr *)calloc(1, total);
        if (!hdr) { fprintf(stderr, "xrt_arc_alloc: out of memory\n"); abort(); }
    }
    hdr->rc = 1;
    return (char *)hdr + sizeof(XrtArcHdr);
}

static inline void *xrt_arc_retain(void *p) {
    if (!p) return NULL;
    XrtArcHdr *h = XRT_ARC_HDR(p);
    if (__builtin_expect(!!(h->flags & XRT_ARC_IMMORTAL), 0)) return p;
    h->rc++;
    return p;
}

// Forward declaration — definition generated per-module by xcgen_emit_struct_deinits()
static void xrt_arc_deinit(void *p, uint16_t type);

static inline void xrt_arc_release(void *p) {
    if (!p) return;
    XrtArcHdr *h = XRT_ARC_HDR(p);
    if (__builtin_expect(!!(h->flags & XRT_ARC_IMMORTAL), 0)) return;
    if (--h->rc == 0) {
        if (h->flags & XRT_ARC_HAS_DEINIT)
            xrt_arc_deinit((char *)h + sizeof(XrtArcHdr), h->type);
        if (!(h->flags & XRT_ARC_BUMP))
            free(h); // bump-allocated objects are freed in bulk by xrt_bump_destroy
    }
}

static inline XrtValue xrt_arc_retain_val(XrtValue v) {
    if ((v.tag == XRT_TAG_PTR || v.tag == XRT_TAG_STR_ARC) && v.ptr)
        xrt_arc_retain(v.ptr);
    return v;
}

static inline void xrt_arc_release_val(XrtValue v) {
    if ((v.tag == XRT_TAG_PTR || v.tag == XRT_TAG_STR_ARC) && v.ptr)
        xrt_arc_release(v.ptr);
}

static inline void xrt_arc_init(void) {
    if (xrt_bump_enabled) xrt_bump_new_block(0);
}

// Allocate a heap string via ARC; xrt_arc_release will free it
static inline XrtValue xrt_str_alloc(size_t len) {
    char *p = (char *)xrt_arc_alloc(len + 1);
    return (XrtValue){.ptr = p, .tag = XRT_TAG_STR_ARC};
}

static inline XrtValue xrt_str_concat(const char *sa, const char *sb) {
    size_t la = strlen(sa), lb = strlen(sb);
    XrtValue v = xrt_str_alloc(la + lb);
    char *r = (char *)v.ptr;
    memcpy(r, sa, la);
    memcpy(r + la, sb, lb + 1);
    return v;
}

/* =========================================================================
 * VM-native type aliases
 *
 * XrtValue and XrValue are the same struct (same ABI).
 * These aliases let AOT code and VM code share pointers without casting.
 * ========================================================================= */

typedef XrtValue XrValue;

// VM-native tag constants (must stay in sync with xvalue.h)
#define XR_TAG_NULL     XRT_TAG_NULL
#define XR_TAG_BOOL     XRT_TAG_BOOL
#define XR_TAG_I64      XRT_TAG_I64
#define XR_TAG_F64      XRT_TAG_F64
#define XR_TAG_PTR      XRT_TAG_PTR

// VM-native type checks
#define XR_IS_NULL(v)   ((v).tag == XR_TAG_NULL)
#define XR_IS_INT(v)    ((v).tag == XR_TAG_I64)
#define XR_IS_FLOAT(v)  ((v).tag == XR_TAG_F64)
#define XR_IS_NUM(v)    (XR_IS_INT(v) || XR_IS_FLOAT(v))

// VM-native value creation macros
#define XR_FROM_INT(x)   ((XrValue){.i = (int64_t)(x),  .tag = XR_TAG_I64})
#define XR_FROM_FLOAT(x) ((XrValue){.f = (double)(x),   .tag = XR_TAG_F64})
#define XR_FROM_BOOL(x)  ((XrValue){.i = (x) ? 1 : 0,   .tag = XR_TAG_BOOL})
#define XR_NULL_VAL      ((XrValue){.ptr = 0, .tag = XR_TAG_NULL})
#define XR_TRUE_VAL      ((XrValue){.i = 1,  .tag = XR_TAG_BOOL})
#define XR_FALSE_VAL     ((XrValue){.i = 0,  .tag = XR_TAG_BOOL})

// VM-native value extraction
#define XR_TO_INT(v)    ((v).i)
#define XR_TO_FLOAT(v)  ((v).f)

/* =========================================================================
 * Runtime context — opaque handle passed to all AOT functions.
 * Points to XrCoroutine* internally; AOT code never dereferences it directly.
 * ========================================================================= */

typedef void *XrtContext;

/* =========================================================================
 * Inline tagged arithmetic (B-class: no extern, no VM dependency)
 * ========================================================================= */

static inline XrtValue xrt_sub(XrtValue a, XrtValue b) {
    if (a.tag == XRT_TAG_I64 && b.tag == XRT_TAG_I64)
        return xrt_box_int(a.i - b.i);
    double fa = (a.tag == XRT_TAG_I64) ? (double)a.i : a.f;
    double fb = (b.tag == XRT_TAG_I64) ? (double)b.i : b.f;
    return xrt_box_float(fa - fb);
}

static inline XrtValue xrt_mul(XrtValue a, XrtValue b) {
    if (a.tag == XRT_TAG_I64 && b.tag == XRT_TAG_I64)
        return xrt_box_int(a.i * b.i);
    double fa = (a.tag == XRT_TAG_I64) ? (double)a.i : a.f;
    double fb = (b.tag == XRT_TAG_I64) ? (double)b.i : b.f;
    return xrt_box_float(fa * fb);
}

static inline XrtValue xrt_div(XrtValue a, XrtValue b) {
    if (a.tag == XRT_TAG_I64 && b.tag == XRT_TAG_I64)
        return b.i ? xrt_box_int(a.i / b.i) : xrt_box_int(0);
    double fa = (a.tag == XRT_TAG_I64) ? (double)a.i : a.f;
    double fb = (b.tag == XRT_TAG_I64) ? (double)b.i : b.f;
    return xrt_box_float(fa / fb);
}

static inline XrtValue xrt_mod(XrtValue a, XrtValue b) {
    if (a.tag == XRT_TAG_I64 && b.tag == XRT_TAG_I64)
        return b.i ? xrt_box_int(a.i % b.i) : xrt_box_int(0);
    return xrt_box_int(0);
}

static inline XrtValue xrt_neg(XrtValue a) {
    if (a.tag == XRT_TAG_I64) return xrt_box_int(-a.i);
    if (a.tag == XRT_TAG_F64) return xrt_box_float(-a.f);
    return xrt_box_int(0);
}

/* =========================================================================
 * Inline tagged comparisons (B-class: no extern, no VM dependency)
 * ========================================================================= */

static inline int64_t xrt_lt(XrtValue a, XrtValue b) {
    if (a.tag == XRT_TAG_I64 && b.tag == XRT_TAG_I64) return a.i < b.i;
    double fa = (a.tag == XRT_TAG_I64) ? (double)a.i : a.f;
    double fb = (b.tag == XRT_TAG_I64) ? (double)b.i : b.f;
    return fa < fb;
}

static inline int64_t xrt_le(XrtValue a, XrtValue b) {
    if (a.tag == XRT_TAG_I64 && b.tag == XRT_TAG_I64) return a.i <= b.i;
    double fa = (a.tag == XRT_TAG_I64) ? (double)a.i : a.f;
    double fb = (b.tag == XRT_TAG_I64) ? (double)b.i : b.f;
    return fa <= fb;
}

/* =========================================================================
 * Inline print (B-class: no extern, no VM dependency)
 * ========================================================================= */

static inline void xrt_print(XrtValue v) {
    switch (v.tag) {
        case XRT_TAG_STR:
        case XRT_TAG_STR_ARC: printf("%s", (const char *)v.ptr); break;
        case XRT_TAG_I64:     printf("%lld", (long long)v.i);     break;
        case XRT_TAG_F64:     printf("%g",   v.f);                break;
        case XRT_TAG_BOOL:    printf("%s", v.i ? "true" : "false"); break;
        case XRT_TAG_NULL:    printf("null");                      break;
        default:              printf("<object@%p>", v.ptr);        break;
    }
}

static inline void xrt_println(XrtValue v) { xrt_print(v); printf("\n"); }

#endif // XRT_H
