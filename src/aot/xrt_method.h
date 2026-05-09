/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_method.h - AOT-side method dispatch, property access, toString.
 *
 * KEY CONCEPT:
 *   This header lives inside the AOT runtime umbrella (xrt.h) and is
 *   #include'd verbatim into AOT-generated C files. AOT-generated code
 *   must compile and run without any runtime header, so this file has
 *   to be self-contained: it cannot reach into runtime/value/ even
 *   though those tables are the source of truth at the language level.
 *
 *   Concretely the AOT-side methods operate on XrValue (a small
 *   tagged union, see xrt_value.h) and call AOT's bump-allocator
 *   helpers; the runtime side operates on runtime/value/xvalue.h's
 *   XrValue and has access to XrayIsolate / GC. The layouts and tag
 *   numbers are intentionally distinct: the AOT side keeps a
 *   self-contained standalone representation and only reuses selected
 *   source-level names.
 *
 *   The remaining shared invariant is the SYMBOL ID number space:
 *   every XRT_SYM_X below must equal the matching SYMBOL_X in
 *   src/runtime/symbol/xsymbol_table.h, otherwise OP_INVOKE_BUILTIN
 *   in the VM and the equivalent xrt_method_N call in AOT-generated C
 *   would route the same method name to different slots. The runtime-
 *   only translation unit src/aot/xrt_symbol_check.c links into
 *   xray_core (never into AOT output) and pairs every XRT_SYM_X with
 *   its SYMBOL_X via _Static_assert; drift fails the build before any
 *   miscompiled AOT binary can ship.
 */

#ifndef XRT_METHOD_H
#define XRT_METHOD_H

#include "xrt_value.h"
#include "xrt_arc.h"   // xrt_str_concat, xrt_str_alloc
#include "xrt_coll.h"  // xrt_array_t, xrt_map_t, xrt_strbuf_finish, xrt_array_push

/* =========================================================================
 * Builtin method symbol IDs.
 *
 * The numeric constants live in xrt_method_symbols.h so that xray_core
 * TUs can consume them without dragging xrt_arc.h into link. They are
 * paired with SYMBOL_X via _Static_assert in xrt_symbol_check.c.
 * ========================================================================= */
#include "xrt_method_symbols.h"

/* =========================================================================
 * toString helper
 * ========================================================================= */

static XrValue xrt_tostring(XrValue val, int slot_hint) {
    if (slot_hint == 1 || val.tag == XR_TAG_I64) {
        char tmp[32];
        int n = 0;
        int64_t v = val.i;
        int64_t t = v;
        if (t < 0) {
            tmp[n++] = '-';
            t = -t;
        }
        if (t == 0) {
            tmp[n++] = '0';
        } else {
            char rev[20];
            int r = 0;
            while (t > 0) {
                rev[r++] = '0' + (char) (t % 10);
                t /= 10;
            }
            while (r > 0)
                tmp[n++] = rev[--r];
        }
        tmp[n] = 0;
        return xrt_str_concat(tmp, "");
    }
    if (slot_hint == 2 || val.tag == XR_TAG_F64) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%g", val.f);
        return xrt_str_concat(tmp, "");
    }
    if (val.tag == XR_TAG_STR || val.tag == XR_TAG_STR_ARC)
        return val;
    if (val.tag == XR_TAG_NULL)
        return xr_box_str("null");
    if (val.tag == XR_TAG_BOOL)
        return xr_box_str(val.i ? "true" : "false");
    return xr_box_str("[object]");
}

/* =========================================================================
 * Fixed-arity method dispatch (no varargs, inlinable by C compiler)
 *
 * Replaces the old varargs xrt_invoke_method. Three fixed-arity versions
 * for 0, 1, and 2 arguments. The C compiler can inline these because
 * they have known signatures (unlike varargs which block inlining).
 * ========================================================================= */

/* String 0-arg method dispatch (extracted to keep xrt_method_0 under 150 lines) */
static inline XrValue xrt_str_method_0(const char *s, int64_t slen, XrValue recv, int sym) {
    if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
        return XR_FROM_INT(slen);
    if (sym == XRT_SYM_IS_EMPTY)
        return XR_FROM_BOOL(slen == 0);
    if (sym == XRT_SYM_TOSTRING)
        return recv;
    if (sym == XRT_SYM_TRIM || sym == XRT_SYM_TRIM_START || sym == XRT_SYM_TRIM_END) {
        const char *start = s, *end = s + slen;
        if (sym != XRT_SYM_TRIM_END)
            while (start < end &&
                   (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r'))
                start++;
        if (sym != XRT_SYM_TRIM_START)
            while (end > start &&
                   (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
                end--;
        int64_t rlen = (int64_t) (end - start);
        XrValue sv = xrt_str_alloc((size_t) rlen);
        memcpy((char *) sv.ptr, start, (size_t) rlen);
        ((char *) sv.ptr)[rlen] = 0;
        return sv;
    }
    if (sym == XRT_SYM_TOLOWER) {
        XrValue sv = xrt_str_alloc((size_t) slen);
        char *r = (char *) sv.ptr;
        for (int64_t i = 0; i < slen; i++)
            r[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char) (s[i] + 32) : s[i];
        r[slen] = 0;
        return sv;
    }
    if (sym == XRT_SYM_TOUPPER) {
        XrValue sv = xrt_str_alloc((size_t) slen);
        char *r = (char *) sv.ptr;
        for (int64_t i = 0; i < slen; i++)
            r[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char) (s[i] - 32) : s[i];
        r[slen] = 0;
        return sv;
    }
    if (sym == XRT_SYM_TOINT) {
        int64_t v = 0;
        int neg = 0;
        const char *p = s;
        while (*p == ' ')
            p++;
        if (*p == '-') {
            neg = 1;
            p++;
        } else if (*p == '+')
            p++;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            p++;
        }
        return XR_FROM_INT(neg ? -v : v);
    }
    if (sym == XRT_SYM_TOFLOAT)
        return XR_FROM_FLOAT(atof(s));
    if (sym == XRT_SYM_ORD)
        return XR_FROM_INT(slen > 0 ? (int64_t) (unsigned char) s[0] : 0);
    if (sym == XRT_SYM_REVERSE) {
        XrValue sv = xrt_str_alloc((size_t) slen);
        char *r = (char *) sv.ptr;
        for (int64_t i = 0; i < slen; i++)
            r[i] = s[slen - 1 - i];
        r[slen] = 0;
        return sv;
    }
    return (XrValue){.i = 0, .tag = XR_TAG_NULL};
}

static inline XrValue xrt_method_0(XrValue recv, int sym) {
    if (XR_IS_STR(recv)) {
        return xrt_str_method_0((const char *) recv.ptr, (int64_t) strlen((const char *) recv.ptr),
                                recv, sym);
    }
    if (recv.tag == XR_TAG_ARRAY) {
        xrt_array_t *a = (xrt_array_t *) recv.ptr;
        if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
            return XR_FROM_INT(a->len);
        if (sym == XRT_SYM_IS_EMPTY)
            return XR_FROM_BOOL(a->len == 0);
        if (sym == XRT_SYM_POP && a->len > 0)
            return XRT_ARRAY_ELEMS(a)[--a->len];
        if (sym == XRT_SYM_SHIFT && a->len > 0) {
            XrValue first = XRT_ARRAY_ELEMS(a)[0];
            for (int64_t i = 0; i < a->len - 1; i++)
                XRT_ARRAY_ELEMS(a)[i] = XRT_ARRAY_ELEMS(a)[i + 1];
            a->len--;
            return first;
        }
        if (sym == XRT_SYM_REVERSE) {
            for (int64_t i = 0, j = a->len - 1; i < j; i++, j--) {
                XrValue tmp = XRT_ARRAY_ELEMS(a)[i];
                XRT_ARRAY_ELEMS(a)[i] = XRT_ARRAY_ELEMS(a)[j];
                XRT_ARRAY_ELEMS(a)[j] = tmp;
            }
            return recv;
        }
        if (sym == XRT_SYM_SORT && a->len > 1) {
            XrValue *elems = XRT_ARRAY_ELEMS(a);
            for (int64_t gap = a->len / 2; gap > 0; gap /= 2) {
                for (int64_t i = gap; i < a->len; i++) {
                    XrValue key = elems[i];
                    int64_t j = i;
                    while (j >= gap) {
                        XrValue *b = &elems[j - gap];
                        int cmp = 0;
                        if (b->tag == XR_TAG_I64 && key.tag == XR_TAG_I64)
                            cmp = (b->i > key.i) - (b->i < key.i);
                        else if (b->tag == XR_TAG_F64 && key.tag == XR_TAG_F64)
                            cmp = (b->f > key.f) - (b->f < key.f);
                        else if (XR_IS_STR(*b) && XR_IS_STR(key))
                            cmp = strcmp((const char *) b->ptr, (const char *) key.ptr);
                        else
                            cmp = (int) b->tag - (int) key.tag;
                        if (cmp <= 0)
                            break;
                        elems[j] = *b;
                        j -= gap;
                    }
                    elems[j] = key;
                }
            }
            return recv;
        }
    }
    if (recv.tag == XR_TAG_MAP) {
        xrt_map_t *m = (xrt_map_t *) recv.ptr;
        if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
            return XR_FROM_INT(m->len);
        if (sym == XRT_SYM_IS_EMPTY)
            return XR_FROM_BOOL(m->len == 0);
        if (sym == XRT_SYM_KEYS) {
            XrValue arr = xrt_array_new(m->len);
            xrt_array_t *ka = (xrt_array_t *) arr.ptr;
            for (int64_t i = 0; i < m->len; i++)
                XRT_ARRAY_ELEMS(ka)[ka->len++] = m->entries[i].key;
            return arr;
        }
        if (sym == XRT_SYM_VALUES) {
            XrValue arr = xrt_array_new(m->len);
            xrt_array_t *va = (xrt_array_t *) arr.ptr;
            for (int64_t i = 0; i < m->len; i++)
                XRT_ARRAY_ELEMS(va)[va->len++] = m->entries[i].val;
            return arr;
        }
    }
    if (recv.tag == XR_TAG_STRBUF)
        return xrt_strbuf_finish(recv);
    if (recv.tag == XR_TAG_I64) {
        if (sym == XRT_SYM_ABS)
            return XR_FROM_INT(recv.i < 0 ? -recv.i : recv.i);
        if (sym == XRT_SYM_TOSTRING)
            return xrt_tostring(recv, 1);
        if (sym == XRT_SYM_TOHEX) {
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%02" PRIX64, (uint64_t) recv.i);
            return xrt_str_concat(buf, "");
        }
    }
    if (recv.tag == XR_TAG_F64) {
        double v = recv.f;
        if (sym == XRT_SYM_FLOOR)
            return XR_FROM_FLOAT(floor(v));
        if (sym == XRT_SYM_CEIL)
            return XR_FROM_FLOAT(ceil(v));
        if (sym == XRT_SYM_ROUND)
            return XR_FROM_FLOAT(round(v));
        if (sym == XRT_SYM_ABS)
            return XR_FROM_FLOAT(fabs(v));
        if (sym == XRT_SYM_SQRT)
            return XR_FROM_FLOAT(sqrt(v));
    }
    return (XrValue){.i = 0, .tag = XR_TAG_NULL};
}

/* String 1-arg method dispatch (extracted to keep xrt_method_1 under 150 lines) */
static inline XrValue xrt_str_method_1(const char *s, int64_t slen, XrValue recv, int sym,
                                        XrValue arg0) {
    if (sym == XRT_SYM_CONTAINS && XR_IS_STR(arg0))
        return XR_FROM_BOOL(strstr(s, (const char *) arg0.ptr) ? 1 : 0);
    if (sym == XRT_SYM_INDEXOF && XR_IS_STR(arg0)) {
        const char *p = strstr(s, (const char *) arg0.ptr);
        return XR_FROM_INT(p ? (int64_t) (p - s) : -1);
    }
    if (sym == XRT_SYM_SLICE && arg0.tag == XR_TAG_I64) {
        int64_t start = arg0.i;
        if (start < 0)
            start += slen;
        if (start < 0)
            start = 0;
        if (start >= slen)
            return xr_box_str("");
        int64_t rlen = slen - start;
        XrValue sv = xrt_str_alloc((size_t) rlen);
        memcpy((char *) sv.ptr, s + start, (size_t) rlen);
        ((char *) sv.ptr)[rlen] = 0;
        return sv;
    }
    if (sym == XRT_SYM_STARTSWITH && XR_IS_STR(arg0)) {
        const char *p = (const char *) arg0.ptr;
        size_t plen = strlen(p);
        return XR_FROM_BOOL((size_t) slen >= plen && memcmp(s, p, plen) == 0);
    }
    if (sym == XRT_SYM_ENDSWITH && XR_IS_STR(arg0)) {
        const char *p = (const char *) arg0.ptr;
        size_t plen = strlen(p);
        return XR_FROM_BOOL((size_t) slen >= plen && memcmp(s + slen - plen, p, plen) == 0);
    }
    if (sym == XRT_SYM_CHARAT && arg0.tag == XR_TAG_I64) {
        int64_t idx = arg0.i;
        if (idx < 0 || idx >= slen)
            return xr_box_str("");
        XrValue sv = xrt_str_alloc(1);
        ((char *) sv.ptr)[0] = s[idx];
        ((char *) sv.ptr)[1] = 0;
        return sv;
    }
    if (sym == XRT_SYM_CONCAT && XR_IS_STR(arg0)) {
        const char *s2 = (const char *) arg0.ptr;
        size_t s2len = strlen(s2);
        size_t rlen = (size_t) slen + s2len;
        XrValue sv = xrt_str_alloc(rlen);
        memcpy((char *) sv.ptr, s, (size_t) slen);
        memcpy((char *) sv.ptr + slen, s2, s2len);
        ((char *) sv.ptr)[rlen] = 0;
        return sv;
    }
    if (sym == XRT_SYM_LASTINDEXOF && XR_IS_STR(arg0)) {
        const char *needle = (const char *) arg0.ptr;
        size_t nlen = strlen(needle);
        if (nlen == 0)
            return XR_FROM_INT(slen);
        for (int64_t i = slen - (int64_t) nlen; i >= 0; i--) {
            if (memcmp(s + i, needle, nlen) == 0)
                return XR_FROM_INT(i);
        }
        return XR_FROM_INT(-1);
    }
    if (sym == XRT_SYM_SPLIT && XR_IS_STR(arg0)) {
        const char *sep = (const char *) arg0.ptr;
        size_t seplen = strlen(sep);
        XrValue arr = xrt_array_new(4);
        if (seplen == 0) {
            /* split by char */
            for (int64_t i = 0; i < slen; i++) {
                XrValue ch = xrt_str_alloc(1);
                ((char *) ch.ptr)[0] = s[i];
                ((char *) ch.ptr)[1] = 0;
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
                size_t part = (size_t) (found - cur);
                XrValue sv = xrt_str_alloc(part);
                memcpy((char *) sv.ptr, cur, part);
                ((char *) sv.ptr)[part] = 0;
                xrt_array_push(arr, sv);
                cur = found + seplen;
            }
        }
        return arr;
    }
    if (sym == XRT_SYM_REPEAT && arg0.tag == XR_TAG_I64) {
        int64_t n = arg0.i;
        if (n <= 0)
            return xr_box_str("");
        size_t rlen = (size_t) (slen * n);
        XrValue sv = xrt_str_alloc(rlen);
        char *r = (char *) sv.ptr;
        for (int64_t i = 0; i < n; i++)
            memcpy(r + i * slen, s, (size_t) slen);
        r[rlen] = 0;
        return sv;
    }
    if (sym == XRT_SYM_REPLACE && XR_IS_STR(arg0)) {
        /* replace(old) with empty string — 1-arg form */
        const char *old_s = (const char *) arg0.ptr;
        const char *found = strstr(s, old_s);
        if (!found)
            return recv;
        size_t olen = strlen(old_s);
        size_t rlen = (size_t) slen - olen;
        XrValue sv = xrt_str_alloc(rlen);
        char *r = (char *) sv.ptr;
        size_t pre = (size_t) (found - s);
        memcpy(r, s, pre);
        memcpy(r + pre, found + olen, (size_t) slen - pre - olen);
        r[rlen] = 0;
        return sv;
    }
    if (sym == XRT_SYM_BYTE_AT && arg0.tag == XR_TAG_I64) {
        int64_t idx = arg0.i;
        if (idx < 0 || idx >= slen)
            return xr_box_str("");
        XrValue sv = xrt_str_alloc(1);
        ((char *) sv.ptr)[0] = s[idx];
        ((char *) sv.ptr)[1] = 0;
        return sv;
    }
    return (XrValue){.i = 0, .tag = XR_TAG_NULL};
}

static inline XrValue xrt_method_1(XrValue recv, int sym, XrValue arg0) {
    if (XR_IS_STR(recv)) {
        return xrt_str_method_1((const char *) recv.ptr, (int64_t) strlen((const char *) recv.ptr),
                                recv, sym, arg0);
    }
    if (recv.tag == XR_TAG_ARRAY) {
        xrt_array_t *a = (xrt_array_t *) recv.ptr;
        if (sym == XRT_SYM_PUSH) {
            xrt_array_push(recv, arg0);
            return (XrValue){.i = 0, .tag = XR_TAG_NULL};
        }
        if (sym == XRT_SYM_UNSHIFT) {
            xrt_array_push(recv, XR_NULL_VAL);
            a = (xrt_array_t *) recv.ptr;
            for (int64_t i = a->len - 1; i > 0; i--)
                XRT_ARRAY_ELEMS(a)[i] = XRT_ARRAY_ELEMS(a)[i - 1];
            XRT_ARRAY_ELEMS(a)[0] = arg0;
            return XR_NULL_VAL;
        }
        if (sym == XRT_SYM_FILL) {
            for (int64_t i = 0; i < a->len; i++)
                XRT_ARRAY_ELEMS(a)[i] = arg0;
            return recv;
        }
        if (sym == XRT_SYM_INDEXOF) {
            for (int64_t i = 0; i < a->len; i++) {
                if (XRT_ARRAY_ELEMS(a)[i].tag == arg0.tag &&
                    XRT_ARRAY_ELEMS(a)[i].i == arg0.i)
                    return XR_FROM_INT(i);
            }
            return XR_FROM_INT(-1);
        }
        if (sym == XRT_SYM_INCLUDES) {
            for (int64_t i = 0; i < a->len; i++) {
                if (XRT_ARRAY_ELEMS(a)[i].tag == arg0.tag &&
                    XRT_ARRAY_ELEMS(a)[i].i == arg0.i)
                    return XR_FROM_BOOL(1);
            }
            return XR_FROM_BOOL(0);
        }
        if (sym == XRT_SYM_JOIN && XR_IS_STR(arg0)) {
            const char *sep = (const char *) arg0.ptr;
            size_t seplen = strlen(sep);
            size_t total = 0;
            for (int64_t i = 0; i < a->len; i++) {
                XrValue sv = xrt_tostring(XRT_ARRAY_ELEMS(a)[i], 0);
                total += strlen((const char *) sv.ptr);
                if (i < a->len - 1)
                    total += seplen;
            }
            XrValue result = xrt_str_alloc(total);
            char *r = (char *) result.ptr;
            size_t pos = 0;
            for (int64_t i = 0; i < a->len; i++) {
                XrValue sv = xrt_tostring(XRT_ARRAY_ELEMS(a)[i], 0);
                const char *p = (const char *) sv.ptr;
                size_t plen = strlen(p);
                memcpy(r + pos, p, plen);
                pos += plen;
                if (i < a->len - 1) {
                    memcpy(r + pos, sep, seplen);
                    pos += seplen;
                }
            }
            r[total] = 0;
            return result;
        }
        if (sym == XRT_SYM_SLICE && arg0.tag == XR_TAG_I64) {
            int64_t start = arg0.i;
            if (start < 0)
                start += a->len;
            if (start < 0)
                start = 0;
            if (start >= a->len)
                return xrt_array_new(0);
            int64_t rlen = a->len - start;
            XrValue arr = xrt_array_new(rlen);
            xrt_array_t *ra = (xrt_array_t *) arr.ptr;
            for (int64_t i = 0; i < rlen; i++)
                XRT_ARRAY_ELEMS(ra)[ra->len++] = XRT_ARRAY_ELEMS(a)[start + i];
            return arr;
        }
        /* Higher-order methods: callback is an AOT closure (XR_TAG_CLOSURE).
         * The closure's fn pointer has signature:
         *   XrValue (*)(xrt_closure_t *_cl, XrValue elem)
         * where _cl points to the closure itself (for upvalue access). */
        if (arg0.tag == XR_TAG_CLOSURE) {
            xrt_closure_t *cl = (xrt_closure_t *) arg0.ptr;
            typedef XrValue (*xrt_fn1_t)(xrt_closure_t *, XrValue);
            xrt_fn1_t fn = (xrt_fn1_t) cl->fn;
            if (sym == XRT_SYM_MAP) {
                XrValue arr = xrt_array_new(a->len);
                for (int64_t i = 0; i < a->len; i++)
                    xrt_array_push(arr, fn(cl, XRT_ARRAY_ELEMS(a)[i]));
                return arr;
            }
            if (sym == XRT_SYM_FILTER) {
                XrValue arr = xrt_array_new(a->len);
                for (int64_t i = 0; i < a->len; i++) {
                    XrValue r = fn(cl, XRT_ARRAY_ELEMS(a)[i]);
                    if (xr_truthy(r))
                        xrt_array_push(arr, XRT_ARRAY_ELEMS(a)[i]);
                }
                return arr;
            }
            if (sym == XRT_SYM_FOREACH) {
                for (int64_t i = 0; i < a->len; i++)
                    fn(cl, XRT_ARRAY_ELEMS(a)[i]);
                return XR_NULL_VAL;
            }
        }
    }
    if (recv.tag == XR_TAG_MAP) {
        xrt_map_t *m = (xrt_map_t *) recv.ptr;
        if (sym == XRT_SYM_GET)
            return xrt_map_get(m, arg0);
        if (sym == XRT_SYM_HAS) {
            for (int64_t i = 0; i < m->len; i++)
                if (xrt_key_eq(m->entries[i].key, arg0))
                    return XR_FROM_BOOL(1);
            return XR_FROM_BOOL(0);
        }
        if (sym == XRT_SYM_DELETE) {
            for (int64_t i = 0; i < m->len; i++) {
                if (xrt_key_eq(m->entries[i].key, arg0)) {
                    m->entries[i] = m->entries[--m->len];
                    return XR_FROM_BOOL(1);
                }
            }
            return XR_FROM_BOOL(0);
        }
    }
    if (recv.tag == XR_TAG_F64 && sym == XRT_SYM_POW) {
        double exp = (arg0.tag == XR_TAG_F64) ? arg0.f : (double) arg0.i;
        return XR_FROM_FLOAT(pow(recv.f, exp));
    }
    /* toFixed(digits): float receiver, int arg */
    if (recv.tag == XR_TAG_F64 && sym == XRT_SYM_TOFIXED && arg0.tag == XR_TAG_I64) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.*f", (int) arg0.i, recv.f);
        return xrt_str_concat(buf, "");
    }
    /* max/min: polymorphic (int or float receiver+arg) */
    if (sym == XRT_SYM_MAX) {
        if (recv.tag == XR_TAG_I64 && arg0.tag == XR_TAG_I64)
            return XR_FROM_INT(recv.i > arg0.i ? recv.i : arg0.i);
        double a = (recv.tag == XR_TAG_F64) ? recv.f : (double) recv.i;
        double b = (arg0.tag == XR_TAG_F64) ? arg0.f : (double) arg0.i;
        return XR_FROM_FLOAT(a > b ? a : b);
    }
    if (sym == XRT_SYM_MIN) {
        if (recv.tag == XR_TAG_I64 && arg0.tag == XR_TAG_I64)
            return XR_FROM_INT(recv.i < arg0.i ? recv.i : arg0.i);
        double a = (recv.tag == XR_TAG_F64) ? recv.f : (double) recv.i;
        double b = (arg0.tag == XR_TAG_F64) ? arg0.f : (double) arg0.i;
        return XR_FROM_FLOAT(a < b ? a : b);
    }
    return (XrValue){.i = 0, .tag = XR_TAG_NULL};
}

static inline XrValue xrt_method_2(XrValue recv, int sym, XrValue arg0, XrValue arg1) {
    if (XR_IS_STR(recv) && (sym == XRT_SYM_SLICE || sym == XRT_SYM_SUBSTRING)) {
        const char *s = (const char *) recv.ptr;
        int64_t slen = (int64_t) strlen(s);
        int64_t start = (arg0.tag == XR_TAG_I64) ? arg0.i : 0;
        int64_t end = (arg1.tag == XR_TAG_I64) ? arg1.i : slen;
        if (start < 0)
            start += slen;
        if (end < 0)
            end += slen;
        if (start < 0)
            start = 0;
        if (end > slen)
            end = slen;
        if (start >= end)
            return xr_box_str("");
        int64_t rlen = end - start;
        XrValue sv = xrt_str_alloc((size_t) rlen);
        memcpy((char *) sv.ptr, s + start, (size_t) rlen);
        ((char *) sv.ptr)[rlen] = 0;
        return sv;
    }
    if (XR_IS_STR(recv) && sym == XRT_SYM_REPLACEALL && XR_IS_STR(arg0) && XR_IS_STR(arg1)) {
        const char *s = (const char *) recv.ptr;
        int64_t slen = (int64_t) strlen(s);
        const char *old_s = (const char *) arg0.ptr;
        const char *new_s = (const char *) arg1.ptr;
        size_t olen = strlen(old_s), nlen = strlen(new_s);
        if (olen == 0)
            return recv;
        /* Count occurrences first */
        int count = 0;
        const char *p = s;
        while ((p = strstr(p, old_s)) != NULL) {
            count++;
            p += olen;
        }
        if (count == 0)
            return recv;
        size_t rlen = (size_t) slen + count * (nlen - olen);
        XrValue sv = xrt_str_alloc(rlen);
        char *r = (char *) sv.ptr;
        const char *cur = s;
        size_t pos = 0;
        while ((p = strstr(cur, old_s)) != NULL) {
            size_t pre = (size_t) (p - cur);
            memcpy(r + pos, cur, pre);
            pos += pre;
            memcpy(r + pos, new_s, nlen);
            pos += nlen;
            cur = p + olen;
        }
        memcpy(r + pos, cur, strlen(cur));
        r[rlen] = 0;
        return sv;
    }
    if (XR_IS_STR(recv) && (sym == XRT_SYM_PAD_START || sym == XRT_SYM_PAD_END) &&
        arg0.tag == XR_TAG_I64 && XR_IS_STR(arg1)) {
        const char *s = (const char *) recv.ptr;
        int64_t slen = (int64_t) strlen(s);
        int64_t target = arg0.i;
        if (target <= slen)
            return recv;
        const char *pad = (const char *) arg1.ptr;
        size_t plen = strlen(pad);
        if (plen == 0)
            return recv;
        int64_t fill = target - slen;
        XrValue sv = xrt_str_alloc((size_t) target);
        char *r = (char *) sv.ptr;
        if (sym == XRT_SYM_PAD_START) {
            for (int64_t i = 0; i < fill; i++)
                r[i] = pad[i % plen];
            memcpy(r + fill, s, (size_t) slen);
        } else {
            memcpy(r, s, (size_t) slen);
            for (int64_t i = 0; i < fill; i++)
                r[slen + i] = pad[i % plen];
        }
        r[target] = 0;
        return sv;
    }
    if (XR_IS_STR(recv) && sym == XRT_SYM_REPLACE && XR_IS_STR(arg0) && XR_IS_STR(arg1)) {
        const char *s = (const char *) recv.ptr;
        int64_t slen = (int64_t) strlen(s);
        const char *old_s = (const char *) arg0.ptr;
        const char *new_s = (const char *) arg1.ptr;
        const char *found = strstr(s, old_s);
        if (!found)
            return recv;
        size_t olen = strlen(old_s), nlen = strlen(new_s);
        size_t rlen = (size_t) slen - olen + nlen;
        XrValue sv = xrt_str_alloc(rlen);
        char *r = (char *) sv.ptr;
        size_t pre = (size_t) (found - s);
        memcpy(r, s, pre);
        memcpy(r + pre, new_s, nlen);
        memcpy(r + pre + nlen, found + olen, (size_t) slen - pre - olen);
        r[rlen] = 0;
        return sv;
    }
    if (recv.tag == XR_TAG_ARRAY && sym == XRT_SYM_SLICE) {
        xrt_array_t *a = (xrt_array_t *) recv.ptr;
        int64_t start = (arg0.tag == XR_TAG_I64) ? arg0.i : 0;
        int64_t end = (arg1.tag == XR_TAG_I64) ? arg1.i : a->len;
        if (start < 0)
            start += a->len;
        if (end < 0)
            end += a->len;
        if (start < 0)
            start = 0;
        if (end > a->len)
            end = a->len;
        if (start >= end)
            return xrt_array_new(0);
        int64_t rlen = end - start;
        XrValue arr = xrt_array_new(rlen);
        xrt_array_t *ra = (xrt_array_t *) arr.ptr;
        for (int64_t i = 0; i < rlen; i++)
            XRT_ARRAY_ELEMS(ra)[ra->len++] = XRT_ARRAY_ELEMS(a)[start + i];
        return arr;
    }
    if (recv.tag == XR_TAG_MAP && sym == XRT_SYM_SET) {
        xrt_map_set((xrt_map_t *) recv.ptr, arg0, arg1);
        return (XrValue){.i = 0, .tag = XR_TAG_NULL};
    }
    return (XrValue){.i = 0, .tag = XR_TAG_NULL};
}

/* =========================================================================
 * Inline property access (replaces extern xrt_vm_getprop for known types)
 *
 * Handles .length and .isEmpty for Array, Map, String.
 * Returns XR_TAG_NULL for unknown properties.
 * ========================================================================= */

static inline XrValue xrt_getprop(XrValue obj, int64_t symbol_id) {
    if (obj.tag == XR_TAG_ARRAY) {
        xrt_array_t *a = (xrt_array_t *) obj.ptr;
        if (symbol_id == XRT_SYM_LENGTH || symbol_id == XRT_SYM_SIZE)
            return XR_FROM_INT(a->len);
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
    if (XR_IS_STR(obj)) {
        const char *s = (const char *) obj.ptr;
        if (symbol_id == XRT_SYM_LENGTH || symbol_id == XRT_SYM_SIZE)
            return XR_FROM_INT((int64_t) strlen(s));
        if (symbol_id == XRT_SYM_IS_EMPTY)
            return XR_FROM_INT(s[0] == '\0');
    }
    return (XrValue){.i = 0, .tag = XR_TAG_NULL};
}

#endif  // XRT_METHOD_H
