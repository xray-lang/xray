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

/* IDs must match SYMBOL_* enum in xsymbol_table.h (auto-numbered from 1) */
#define XRT_SYM_LENGTH      1
#define XRT_SYM_SIZE        1   /* alias — compiler always emits LENGTH */
#define XRT_SYM_IS_EMPTY    2
#define XRT_SYM_HAS         3
#define XRT_SYM_GET         4
#define XRT_SYM_SET         5
#define XRT_SYM_DELETE      6
#define XRT_SYM_CLEAR       7
#define XRT_SYM_KEYS        8
#define XRT_SYM_VALUES      9
#define XRT_SYM_SLICE       16
#define XRT_SYM_INDEXOF     19
#define XRT_SYM_CONTAINS    20
#define XRT_SYM_STARTSWITH  21
#define XRT_SYM_ENDSWITH    22
#define XRT_SYM_TOLOWER     23
#define XRT_SYM_TOUPPER     24
#define XRT_SYM_TRIM        25
#define XRT_SYM_SPLIT       26
#define XRT_SYM_REPLACE     27
#define XRT_SYM_REPEAT      29
#define XRT_SYM_PUSH        48
#define XRT_SYM_POP         49
#define XRT_SYM_JOIN        52
#define XRT_SYM_REVERSE     53
#define XRT_SYM_SORT        169  /* SYMBOL_SORT */
#define XRT_SYM_INCLUDES    170  /* SYMBOL_INCLUDES */
#define XRT_SYM_FLOOR       58
#define XRT_SYM_CEIL        59
#define XRT_SYM_ROUND       60
#define XRT_SYM_ABS         61
#define XRT_SYM_SQRT        62
#define XRT_SYM_POW         63
#define XRT_SYM_TOFIXED     64
#define XRT_SYM_TOSTRING    85

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
        if (sym == XRT_SYM_IS_EMPTY) return xrt_box_bool(slen == 0);
        if (sym == XRT_SYM_TOSTRING) return recv;
        if (sym == XRT_SYM_TRIM) {
            const char *start = s, *end = s + slen;
            while (start < end && (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r')) start++;
            while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) end--;
            int64_t rlen = (int64_t)(end - start);
            XrtValue sv = xrt_str_alloc((size_t)rlen);
            memcpy((char *)sv.ptr, start, (size_t)rlen);
            ((char *)sv.ptr)[rlen] = 0;
            return sv;
        }
        if (sym == XRT_SYM_TOLOWER) {
            XrtValue sv = xrt_str_alloc((size_t)slen);
            char *r = (char *)sv.ptr;
            for (int64_t i = 0; i < slen; i++)
                r[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
            r[slen] = 0;
            return sv;
        }
        if (sym == XRT_SYM_TOUPPER) {
            XrtValue sv = xrt_str_alloc((size_t)slen);
            char *r = (char *)sv.ptr;
            for (int64_t i = 0; i < slen; i++)
                r[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
            r[slen] = 0;
            return sv;
        }
    }
    if (recv.tag == XRT_TAG_ARRAY) {
        xrt_array_t *a = (xrt_array_t *)recv.ptr;
        if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE) return xrt_box_int(a->len);
        if (sym == XRT_SYM_IS_EMPTY) return xrt_box_bool(a->len == 0);
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
        if (sym == XRT_SYM_IS_EMPTY) return xrt_box_bool(m->len == 0);
        if (sym == XRT_SYM_KEYS) {
            XrtValue arr = xrt_array_new(m->len);
            xrt_array_t *a = (xrt_array_t *)arr.ptr;
            for (int64_t i = 0; i < m->len; i++) a->data[a->len++] = m->entries[i].key;
            return arr;
        }
        if (sym == XRT_SYM_VALUES) {
            XrtValue arr = xrt_array_new(m->len);
            xrt_array_t *a = (xrt_array_t *)arr.ptr;
            for (int64_t i = 0; i < m->len; i++) a->data[a->len++] = m->entries[i].val;
            return arr;
        }
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
            return xrt_box_bool(strstr(s, (const char *)arg0.ptr) ? 1 : 0);
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
            return xrt_box_bool((size_t)slen >= plen && memcmp(s, p, plen) == 0);
        }
        if (sym == XRT_SYM_ENDSWITH && XRT_IS_STR(arg0)) {
            const char *p = (const char *)arg0.ptr;
            size_t plen = strlen(p);
            return xrt_box_bool((size_t)slen >= plen && memcmp(s + slen - plen, p, plen) == 0);
        }
        if (sym == XRT_SYM_SPLIT && XRT_IS_STR(arg0)) {
            const char *sep = (const char *)arg0.ptr;
            size_t seplen = strlen(sep);
            XrtValue arr = xrt_array_new(4);
            if (seplen == 0) {
                /* split by char */
                for (int64_t i = 0; i < slen; i++) {
                    XrtValue ch = xrt_str_alloc(1);
                    ((char *)ch.ptr)[0] = s[i]; ((char *)ch.ptr)[1] = 0;
                    xrt_array_push(arr, ch);
                }
            } else {
                const char *cur = s;
                while (1) {
                    const char *found = strstr(cur, sep);
                    if (!found) {
                        xrt_array_push(arr, xrt_str_concat(cur, ""));
                        break;
                    }
                    size_t part = (size_t)(found - cur);
                    XrtValue sv = xrt_str_alloc(part);
                    memcpy((char *)sv.ptr, cur, part); ((char *)sv.ptr)[part] = 0;
                    xrt_array_push(arr, sv);
                    cur = found + seplen;
                }
            }
            return arr;
        }
        if (sym == XRT_SYM_REPEAT && arg0.tag == XRT_TAG_I64) {
            int64_t n = arg0.i;
            if (n <= 0) return xrt_box_str("");
            size_t rlen = (size_t)(slen * n);
            XrtValue sv = xrt_str_alloc(rlen);
            char *r = (char *)sv.ptr;
            for (int64_t i = 0; i < n; i++) memcpy(r + i * slen, s, (size_t)slen);
            r[rlen] = 0;
            return sv;
        }
        if (sym == XRT_SYM_REPLACE && XRT_IS_STR(arg0)) {
            /* replace(old) with empty string — 1-arg form */
            const char *old_s = (const char *)arg0.ptr;
            const char *found = strstr(s, old_s);
            if (!found) return recv;
            size_t olen = strlen(old_s);
            size_t rlen = (size_t)slen - olen;
            XrtValue sv = xrt_str_alloc(rlen);
            char *r = (char *)sv.ptr;
            size_t pre = (size_t)(found - s);
            memcpy(r, s, pre);
            memcpy(r + pre, found + olen, (size_t)slen - pre - olen);
            r[rlen] = 0;
            return sv;
        }
    }
    if (recv.tag == XRT_TAG_ARRAY) {
        xrt_array_t *a = (xrt_array_t *)recv.ptr;
        if (sym == XRT_SYM_PUSH) {
            xrt_array_push(recv, arg0);
            return (XrtValue){.i = 0, .tag = XRT_TAG_NULL};
        }
        if (sym == XRT_SYM_INDEXOF) {
            for (int64_t i = 0; i < a->len; i++) {
                if (a->data[i].tag == arg0.tag && a->data[i].i == arg0.i) return xrt_box_int(i);
            }
            return xrt_box_int(-1);
        }
        if (sym == XRT_SYM_INCLUDES) {
            for (int64_t i = 0; i < a->len; i++) {
                if (a->data[i].tag == arg0.tag && a->data[i].i == arg0.i) return xrt_box_bool(1);
            }
            return xrt_box_bool(0);
        }
        if (sym == XRT_SYM_JOIN && XRT_IS_STR(arg0)) {
            const char *sep = (const char *)arg0.ptr;
            size_t seplen = strlen(sep);
            /* compute total length */
            size_t total = 0;
            for (int64_t i = 0; i < a->len; i++) {
                XrtValue sv = xrt_tostring(a->data[i], 0);
                total += strlen((const char *)sv.ptr);
                if (i < a->len - 1) total += seplen;
            }
            XrtValue result = xrt_str_alloc(total);
            char *r = (char *)result.ptr;
            size_t pos = 0;
            for (int64_t i = 0; i < a->len; i++) {
                XrtValue sv = xrt_tostring(a->data[i], 0);
                const char *p = (const char *)sv.ptr;
                size_t plen = strlen(p);
                memcpy(r + pos, p, plen); pos += plen;
                if (i < a->len - 1) { memcpy(r + pos, sep, seplen); pos += seplen; }
            }
            r[total] = 0;
            return result;
        }
        if (sym == XRT_SYM_SLICE && arg0.tag == XRT_TAG_I64) {
            int64_t start = arg0.i;
            if (start < 0) start += a->len;
            if (start < 0) start = 0;
            if (start >= a->len) return xrt_array_new(0);
            int64_t rlen = a->len - start;
            XrtValue arr = xrt_array_new(rlen);
            xrt_array_t *ra = (xrt_array_t *)arr.ptr;
            for (int64_t i = 0; i < rlen; i++) ra->data[ra->len++] = a->data[start + i];
            return arr;
        }
    }
    if (recv.tag == XRT_TAG_MAP) {
        xrt_map_t *m = (xrt_map_t *)recv.ptr;
        if (sym == XRT_SYM_GET) return xrt_map_get(m, arg0);
        if (sym == XRT_SYM_HAS) {
            for (int64_t i = 0; i < m->len; i++)
                if (xrt_key_eq(m->entries[i].key, arg0)) return xrt_box_bool(1);
            return xrt_box_bool(0);
        }
        if (sym == XRT_SYM_DELETE) {
            for (int64_t i = 0; i < m->len; i++) {
                if (xrt_key_eq(m->entries[i].key, arg0)) {
                    m->entries[i] = m->entries[--m->len];
                    return xrt_box_bool(1);
                }
            }
            return xrt_box_bool(0);
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
    if (XRT_IS_STR(recv) && sym == XRT_SYM_REPLACE && XRT_IS_STR(arg0) && XRT_IS_STR(arg1)) {
        const char *s = (const char *)recv.ptr;
        int64_t slen = (int64_t)strlen(s);
        const char *old_s = (const char *)arg0.ptr;
        const char *new_s = (const char *)arg1.ptr;
        const char *found = strstr(s, old_s);
        if (!found) return recv;
        size_t olen = strlen(old_s), nlen = strlen(new_s);
        size_t rlen = (size_t)slen - olen + nlen;
        XrtValue sv = xrt_str_alloc(rlen);
        char *r = (char *)sv.ptr;
        size_t pre = (size_t)(found - s);
        memcpy(r, s, pre);
        memcpy(r + pre, new_s, nlen);
        memcpy(r + pre + nlen, found + olen, (size_t)slen - pre - olen);
        r[rlen] = 0;
        return sv;
    }
    if (recv.tag == XRT_TAG_ARRAY && sym == XRT_SYM_SLICE) {
        xrt_array_t *a = (xrt_array_t *)recv.ptr;
        int64_t start = (arg0.tag == XRT_TAG_I64) ? arg0.i : 0;
        int64_t end   = (arg1.tag == XRT_TAG_I64) ? arg1.i : a->len;
        if (start < 0) start += a->len;
        if (end   < 0) end   += a->len;
        if (start < 0) start = 0;
        if (end > a->len) end = a->len;
        if (start >= end) return xrt_array_new(0);
        int64_t rlen = end - start;
        XrtValue arr = xrt_array_new(rlen);
        xrt_array_t *ra = (xrt_array_t *)arr.ptr;
        for (int64_t i = 0; i < rlen; i++) ra->data[ra->len++] = a->data[start + i];
        return arr;
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
