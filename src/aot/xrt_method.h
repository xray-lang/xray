/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_method.h - Method dispatch, property access, toString
 */

#ifndef XRT_METHOD_H
#define XRT_METHOD_H

#include "xrt_value.h"
#include "xrt_arc.h"   // xrt_str_concat, xrt_str_alloc
#include "xrt_coll.h"  // xrt_array_t, xrt_map_t, xrt_strbuf_finish, xrt_array_push

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

#endif // XRT_METHOD_H
