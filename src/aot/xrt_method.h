/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_method.h - AOT-side method dispatch, property access, toString.
 *
 * This header is included verbatim into generated C and stays independent
 * from the VM/runtime value headers. XRT_SYM_* values must match the VM
 * symbol table; xrt_symbol_check.c validates the shared numeric namespace.
 */

#ifndef XRT_METHOD_H
#define XRT_METHOD_H

#include "xrt_value.h"
#include "xrt_arc.h"   // xrt_str_concat, xrt_str_alloc
#include "xrt_coll.h"  // xrt_array_t, xrt_map_t, xrt_strbuf_finish, xrt_array_push
#include "xrt_array_hof.h"
#include "xrt_range.h"
#include "xrt_datetime.h"
#include "xrt_arith.h"  // xrt_value_to_string for container/tuple toString
#include "../shared/xr_int_arith.h"
#include "../shared/xr_range_core.h"
#include "../shared/xr_string_core.h"

/* Builtin method symbol IDs. */
#include "xrt_method_symbols.h"

#include "xrt_range_methods.inc.c"
#include "xrt_sort.inc.c"

static inline int xrt_weak_value_is_heap_object(XrValue v) {
    if (!v.ptr)
        return 0;
    switch (xrt_value_kind(v)) {
        case XR_TAG_PTR:
        case XR_TAG_STR:
        case XR_TAG_ARRAY:
        case XR_TAG_MAP:
        case XR_TAG_STRBUF:
        case XR_TAG_CLOSURE:
        case XR_TAG_STR_ARC:
        case XR_TAG_CELL:
        case XR_TAG_TUPLE:
        case XR_TAG_SET:
        case XR_TAG_STRUCT_REF:
        case XR_TAG_REGEX:
        case XR_TAG_SYS_MUTEX:
        case XR_TAG_SYS_RWLOCK:
        case XR_TAG_SYS_CONDVAR:
        case XR_TAG_SYS_BARRIER:
        case XR_TAG_SYS_ONCE:
        case XR_TAG_RANGE:
        case XR_TAG_ENUM:
        case XR_TAG_ITERATOR:
            return 1;
        default:
            return 0;
    }
}

/* toString helper. */

static inline int xrt_char_encode(uint32_t cp, char *tmp) {
    if (cp <= 0x7Fu) {
        tmp[0] = (char) cp;
        return 1;
    }
    if (cp <= 0x7FFu) {
        tmp[0] = (char) (0xC0u | (cp >> 6));
        tmp[1] = (char) (0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp <= 0xFFFFu) {
        if (cp >= 0xD800u && cp <= 0xDFFFu)
            return 0;
        tmp[0] = (char) (0xE0u | (cp >> 12));
        tmp[1] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
        tmp[2] = (char) (0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        tmp[0] = (char) (0xF0u | (cp >> 18));
        tmp[1] = (char) (0x80u | ((cp >> 12) & 0x3Fu));
        tmp[2] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
        tmp[3] = (char) (0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;
}

static inline XrValue xrt_char_to_string(uint32_t cp) {
    char tmp[4];
    int n = xrt_char_encode(cp, tmp);
    XrValue out = xrt_str_alloc((size_t) (n > 0 ? n : 0));
    if (n > 0)
        memcpy(xr_str_buf(out), tmp, (size_t) n);
    xr_str_buf(out)[n > 0 ? n : 0] = 0;
    return out;
}

static inline int xrt_char_is_letter(uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

static inline int xrt_char_is_number(uint32_t cp) {
    return cp >= '0' && cp <= '9';
}

static inline int xrt_char_is_alnum(uint32_t cp) {
    return xrt_char_is_letter(cp) || xrt_char_is_number(cp);
}

static inline int xrt_char_is_whitespace(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v';
}

static XrValue xrt_tostring(XrValue val, int slot_hint) {
    if (slot_hint == 3)
        return xrt_uint64_to_string((uint64_t) val.i);
    if (slot_hint == 1 || val.tag == XR_TAG_I64) {
        char tmp[32];
        int n = 0;
        int64_t v = val.i;
        uint64_t t = xr_i64_abs_magnitude(v);
        if (v < 0) {
            tmp[n++] = '-';
        }
        if (t == 0) {
            tmp[n++] = '0';
        } else {
            char rev[20];
            int r = 0;
            while (t > 0) {
                rev[r++] = '0' + (char) (t % 10u);
                t /= 10;
            }
            while (r > 0)
                tmp[n++] = rev[--r];
        }
        tmp[n] = 0;
        return xrt_str_from_cstr(tmp);
    }
    if (slot_hint == 2 || val.tag == XR_TAG_F64) {
        char tmp[64];
        xrt_format_float(tmp, sizeof(tmp), val.f);
        return xrt_str_from_cstr(tmp);
    }
    if (val.tag == XR_TAG_STR || val.tag == XR_TAG_STR_ARC)
        return val;
    if (val.tag == XR_TAG_RANGE)
        return xrt_range_to_string(val);
    if (val.tag == XR_TAG_NULL)
        return xr_box_str("null");
    if (val.tag == XR_TAG_BOOL)
        return xr_box_str(val.i ? "true" : "false");
    if (val.tag == XR_TAG_CHAR) {
        return xrt_char_to_string(XR_TO_CHAR(val));
    }
    if (val.tag == XR_TAG_ENUM) {
        char tmp[256];
        return xrt_str_from_cstr(xr_to_cstr(val, tmp, sizeof(tmp)));
    }
    return xr_box_str("[object]");
}

/* char(x): construct a Unicode scalar char (tagged XR_TAG_CHAR).
 * Validates range and excludes UTF-16 surrogates; invalid yields null. */
static XrValue xrt_to_char(XrValue val) {
    if (XR_IS_CHAR(val))
        return val;
    if (XR_IS_INT(val)) {
        int64_t cp = XR_TO_INT(val);
        if (cp >= 0 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF))
            return XR_FROM_CHAR((uint32_t) cp);
    }
    return XR_NULL_VAL;
}

static XrValue xrt_to_int(XrValue val) {
    if (XR_IS_INT(val))
        return val;
    if (XR_IS_CHAR(val))
        return XR_FROM_INT((int64_t) XR_TO_CHAR(val));
    if (XR_IS_FLOAT(val))
        return XR_FROM_INT((int64_t) XR_TO_FLOAT(val));
    if (XR_IS_STR(val)) {
        XrStringCoreParseIntResult parsed =
            xr_string_core_parse_int64(xr_str_data(val), (size_t) xr_str_len(val));
        return parsed.ok ? XR_FROM_INT(parsed.value) : XR_NULL_VAL;
    }
    if (XR_IS_BOOL(val))
        return XR_FROM_INT(val.i != 0 ? 1 : 0);
    return XR_NULL_VAL;
}

static XrValue xrt_to_float(XrValue val) {
    if (XR_IS_FLOAT(val))
        return val;
    if (XR_IS_INT(val))
        return XR_FROM_FLOAT((double) XR_TO_INT(val));
    if (XR_IS_STR(val)) {
        XrStringCoreParseFloatResult parsed =
            xr_string_core_parse_float64(xr_str_data(val), (size_t) xr_str_len(val));
        return parsed.ok ? XR_FROM_FLOAT(parsed.value) : XR_NULL_VAL;
    }
    if (XR_IS_BOOL(val))
        return XR_FROM_FLOAT(val.i != 0 ? 1.0 : 0.0);
    return XR_NULL_VAL;
}

static XrValue xrt_to_string(XrValue val) {
    return XR_IS_STR(val) ? val : xrt_tostring(val, 0);
}

static XrValue xrt_to_bool(XrValue val) {
    if (XR_IS_BOOL(val))
        return val;
    if (XR_IS_NULL(val))
        return XR_FALSE_VAL;
    if (XR_IS_INT(val))
        return XR_FROM_BOOL(XR_TO_INT(val) != 0);
    if (XR_IS_FLOAT(val))
        return XR_FROM_BOOL(XR_TO_FLOAT(val) != 0.0);
    if (XR_IS_STR(val))
        return XR_FROM_BOOL(xr_str_len(val) > 0);
    if (XR_IS_ARRAY(val))
        return XR_FROM_BOOL(((xrt_array_t *) val.ptr)->length > 0);
    if (XR_IS_MAP(val))
        return XR_FROM_BOOL(xrt_map_len((xrt_map_t *) val.ptr) > 0);
    if (XR_IS_SET(val))
        return XR_FROM_BOOL(xrt_set_len((xrt_set_t *) val.ptr) > 0);
    return XR_TRUE_VAL;
}

/* Fixed-arity method dispatch is intentionally inlineable by the C compiler. */

static inline int64_t xrt_utf8_scalar_count(const char *s, int64_t slen) {
    if (!s || slen <= 0)
        return 0;
    const unsigned char *p = (const unsigned char *) s;
    const unsigned char *end = p + slen;
    int64_t count = 0;
    while (p < end) {
        unsigned char b = *p;
        int size = 1;
        if ((b & 0x80u) == 0)
            size = 1;
        else if ((b & 0xE0u) == 0xC0u)
            size = 2;
        else if ((b & 0xF0u) == 0xE0u)
            size = 3;
        else if ((b & 0xF8u) == 0xF0u)
            size = 4;
        if (p + size > end)
            size = 1;
        p += size;
        count++;
    }
    return count;
}

static inline XrValue xrt_string_entries(XrValue recv) {
    int64_t n = xrt_utf8_scalar_count(xr_str_data(recv), xr_str_len(recv));
    XrValue arr = xrt_array_with_capacity(n);
    xrt_iterator_t iter = {
        .coll = recv,
        .cursor = 0,
        .index = 0,
        .kind = XRT_ITER_PAIRS,
    };
    while (xrt_iterator_has_next(&iter))
        xrt_array_push(arr, xrt_iterator_next(&iter));
    return arr;
}

static inline XrValue xrt_json_collect(XrValue recv, uint8_t kind) {
    xrt_json_t *j = (xrt_json_t *) recv.ptr;
    XrValue arr = xrt_array_with_capacity(xrt_json_iter_count(j));
    xrt_iterator_t iter = {
        .coll = recv,
        .cursor = 0,
        .index = 0,
        .kind = kind,
        .gen = NULL,
    };
    while (xrt_iterator_has_next(&iter))
        xrt_array_push(arr, xrt_iterator_next(&iter));
    return arr;
}

/* String 0-arg method dispatch. */
static inline XrValue xrt_str_method_0(const char *s, int64_t slen, XrValue recv, int sym) {
    if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
        return XR_FROM_INT(xrt_utf8_scalar_count(s, slen));
    if (sym == XRT_SYM_IS_EMPTY)
        return XR_FROM_BOOL(slen == 0);
    if (sym == XRT_SYM_TOSTRING)
        return recv;
    if (sym == XRT_SYM_ITERATOR)
        return xrt_iterator_new(recv, XRT_ITER_VALUES);
    if (sym == XRT_SYM_ENTRIES_ITERATOR)
        return xrt_iterator_new(recv, XRT_ITER_PAIRS);
    if (sym == XRT_SYM_ENTRIES)
        return xrt_string_entries(recv);
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
        memcpy(xr_str_buf(sv), start, (size_t) rlen);
        xr_str_buf(sv)[rlen] = 0;
        return sv;
    }
    if (sym == XRT_SYM_TOLOWER) {
        XrValue sv = xrt_str_alloc((size_t) slen);
        char *r = xr_str_buf(sv);
        for (int64_t i = 0; i < slen; i++)
            r[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char) (s[i] + 32) : s[i];
        r[slen] = 0;
        return sv;
    }
    if (sym == XRT_SYM_TOUPPER) {
        XrValue sv = xrt_str_alloc((size_t) slen);
        char *r = xr_str_buf(sv);
        for (int64_t i = 0; i < slen; i++)
            r[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char) (s[i] - 32) : s[i];
        r[slen] = 0;
        return sv;
    }
    if (sym == XRT_SYM_TOINT) {
        XrStringCoreParseIntResult parsed = xr_string_core_parse_int64(s, (size_t) slen);
        return parsed.ok ? XR_FROM_INT(parsed.value) : XR_NULL_VAL;
    }
    if (sym == XRT_SYM_TOFLOAT) {
        XrStringCoreParseFloatResult parsed = xr_string_core_parse_float64(s, (size_t) slen);
        return parsed.ok ? XR_FROM_FLOAT(parsed.value) : XR_NULL_VAL;
    }
    if (sym == XRT_SYM_ORD) {
        /* Mirror VM xr_string_ord: empty -> null; a 1-byte string is a raw
         * octet (byteAt() on binary buffers) -> unsigned byte; otherwise
         * decode the first UTF-8 scalar (invalid sequences -> U+FFFD). */
        if (slen == 0)
            return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
        const unsigned char *p = (const unsigned char *) s;
        if (slen == 1 || (p[0] & 0x80u) == 0)
            return XR_FROM_INT((int64_t) p[0]);
        uint32_t cp = 0xFFFD;
        if ((p[0] & 0xE0u) == 0xC0u && slen >= 2 && (p[1] & 0xC0u) == 0x80u) {
            cp = ((uint32_t) (p[0] & 0x1Fu) << 6) | (uint32_t) (p[1] & 0x3Fu);
        } else if ((p[0] & 0xF0u) == 0xE0u && slen >= 3 && (p[1] & 0xC0u) == 0x80u &&
                   (p[2] & 0xC0u) == 0x80u) {
            cp = ((uint32_t) (p[0] & 0x0Fu) << 12) | ((uint32_t) (p[1] & 0x3Fu) << 6) |
                 (uint32_t) (p[2] & 0x3Fu);
        } else if ((p[0] & 0xF8u) == 0xF0u && slen >= 4 && (p[1] & 0xC0u) == 0x80u &&
                   (p[2] & 0xC0u) == 0x80u && (p[3] & 0xC0u) == 0x80u) {
            cp = ((uint32_t) (p[0] & 0x07u) << 18) | ((uint32_t) (p[1] & 0x3Fu) << 12) |
                 ((uint32_t) (p[2] & 0x3Fu) << 6) | (uint32_t) (p[3] & 0x3Fu);
        }
        return XR_FROM_INT((int64_t) cp);
    }
    if (sym == XRT_SYM_REVERSE) {
        XrValue sv = xrt_str_alloc((size_t) slen);
        xr_string_core_reverse_utf8_write(xr_str_buf(sv), s, (size_t) slen);
        return sv;
    }
    return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
}

/* string.toBytes() -> Bytes (Array<uint8>): the UTF-8 bytes of the string.
 * Mirrors the VM m_to_bytes and round-trips with Bytes.toString(). */
static inline XrValue xrt_str_to_bytes(XrValue s) {
    int64_t len = (int64_t) xr_str_len(s);
    xrt_array_t *b = xrt_array_new_typed_ptr(len, XR_ELEM_U8);
    if (len > 0)
        memcpy(b->data, xr_str_data(s), (size_t) len);
    return xr_mkptr(b, XR_TAG_ARRAY);
}

static inline XrValue xrt_method_0(XrValue recv, int sym) {
    /* Container/tuple toString renders via the shared value formatter so AOT
     * matches the VM ("[1, 2, 3]", "#{...}", "#[...]"). Simple enums are
     * XR_TAG_ENUM and handled by their own toString case below. */
    if (sym == XRT_SYM_TOSTRING) {
        int rk = xrt_value_kind(recv);
        if (rk == XR_TAG_ARRAY) {
            /* Bytes (Array<uint8>) decodes as UTF-8 text — mirrors the VM
             * m_to_string and round-trips with string.toBytes(). */
            xrt_array_t *a = (xrt_array_t *) recv.ptr;
            if (a->elem_type == XR_ELEM_U8) {
                XrValue sv = xrt_str_alloc((size_t) a->length);
                if (a->length > 0)
                    memcpy(xr_str_buf(sv), a->data, (size_t) a->length);
                xr_str_buf(sv)[a->length] = 0;
                return sv;
            }
            return xrt_value_to_string(recv);
        }
        if (rk == XR_TAG_MAP || rk == XR_TAG_SET || rk == XR_TAG_TUPLE)
            return xrt_value_to_string(recv);
    }
    if (XR_IS_STR(recv)) {
        return xrt_str_method_0(xr_str_data(recv), xr_str_len(recv), recv, sym);
    }
    if (XR_IS_ARRAY(recv)) {
        xrt_array_t *a = (xrt_array_t *) recv.ptr;
        if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
            return XR_FROM_INT(a->length);
        if (sym == XRT_SYM_CAPACITY)
            return XR_FROM_INT(a->capacity);
        if (sym == XRT_SYM_IS_EMPTY)
            return XR_FROM_BOOL(a->length == 0);
        if (sym == XRT_SYM_CLEAR)
            return xrt_array_clear_value(recv);
        if (sym == XRT_SYM_POP && a->length > 0) {
            a->length--;
            return xr_typed_get(a->data, (int32_t) a->length, a->elem_type);
        }
        if (sym == XRT_SYM_SHIFT && a->length > 0) {
            XrValue first = xr_typed_get(a->data, 0, a->elem_type);
            for (int64_t i = 0; i < a->length - 1; i++) {
                XrValue next = xr_typed_get(a->data, (int32_t) (i + 1), a->elem_type);
                xr_typed_set(a->data, (int32_t) i, next, a->elem_type);
            }
            a->length--;
            return first;
        }
        if (sym == XRT_SYM_REVERSE) {
            for (int64_t i = 0, j = a->length - 1; i < j; i++, j--) {
                XrValue vi = xr_typed_get(a->data, (int32_t) i, a->elem_type);
                XrValue vj = xr_typed_get(a->data, (int32_t) j, a->elem_type);
                xr_typed_set(a->data, (int32_t) i, vj, a->elem_type);
                xr_typed_set(a->data, (int32_t) j, vi, a->elem_type);
            }
            return recv;
        }
        if (sym == XRT_SYM_SORT)
            return xrt_array_sort(recv, NULL);
    }
    if (XR_IS_MAP(recv)) {
        xrt_map_t *m = (xrt_map_t *) recv.ptr;
        if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
            return XR_FROM_INT(xrt_map_len(m));
        if (sym == XRT_SYM_IS_EMPTY)
            return XR_FROM_BOOL(xrt_map_len(m) == 0);
        if (!xrt_map_is_boolmap(m) && (m->flags & XR_MAP_FLAG_WEAK))
            return XR_NULL_VAL;
        if (sym == XRT_SYM_KEYS)
            return xrt_map_keys(m);
        if (sym == XRT_SYM_VALUES)
            return xrt_map_values(m);
        if (sym == XRT_SYM_ITERATOR)
            return xrt_iterator_new(recv, XRT_ITER_KEYS);
        if (sym == XRT_SYM_ENTRIES_ITERATOR)
            return xrt_iterator_new(recv, XRT_ITER_PAIRS);
    }
    if (xrt_is_json_object_value(recv)) {
        xrt_json_t *j = (xrt_json_t *) recv.ptr;
        if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
            return XR_FROM_INT(xrt_json_iter_count(j));
        if (sym == XRT_SYM_IS_EMPTY)
            return XR_FROM_BOOL(xrt_json_iter_count(j) == 0);
        if (sym == XRT_SYM_KEYS)
            return xrt_json_collect(recv, XRT_ITER_KEYS);
        if (sym == XRT_SYM_VALUES)
            return xrt_json_collect(recv, XRT_ITER_VALUES);
        if (sym == XRT_SYM_ENTRIES)
            return xrt_json_collect(recv, XRT_ITER_PAIRS);
        if (sym == XRT_SYM_ITERATOR)
            return xrt_iterator_new(recv, XRT_ITER_KEYS);
        if (sym == XRT_SYM_ENTRIES_ITERATOR)
            return xrt_iterator_new(recv, XRT_ITER_PAIRS);
    }
    if (XR_IS_SET(recv)) {
        xrt_set_t *s = (xrt_set_t *) recv.ptr;
        if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
            return XR_FROM_INT(xrt_set_len(s));
        if (sym == XRT_SYM_IS_EMPTY)
            return XR_FROM_BOOL(xrt_set_len(s) == 0);
        if (s->flags & XR_SET_FLAG_WEAK)
            return XR_NULL_VAL;
        if (sym == XRT_SYM_CLEAR) {
            xrt_set_clear(s);
            return XR_NULL_VAL;
        }
        if (sym == XRT_SYM_VALUES)
            return xrt_set_values(s);
        if (sym == XRT_SYM_ITERATOR)
            return xrt_iterator_new(recv, XRT_ITER_VALUES);
    }
    if (recv.tag == XR_TAG_ITERATOR) {
        xrt_iterator_t *it = (xrt_iterator_t *) recv.ptr;
        if (sym == XRT_SYM_ITERATOR)
            return recv;
        if (sym == XRT_SYM_HAS_NEXT)
            return XR_FROM_BOOL(xrt_iterator_has_next(it));
        if (sym == XRT_SYM_NEXT)
            return xrt_iterator_next(it);
    }
    if (recv.tag == XR_TAG_STRBUF) {
        xrt_strbuf_t *sb = (xrt_strbuf_t *) recv.ptr;
        if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
            return XR_FROM_INT(sb ? sb->len : 0);
        if (sym == XRT_SYM_CLEAR) {
            if (sb) {
                sb->len = 0;
                if (sb->buf)
                    sb->buf[0] = 0;
            }
            return recv;
        }
        if (sym == XRT_SYM_TOSTRING)
            return xrt_strbuf_finish(recv);
    }
    if (recv.tag == XR_TAG_RANGE)
        return xrt_range_method_0(recv, sym);
    if (recv.tag == XR_TAG_DATETIME)
        return xrt_datetime_method_0(recv, sym);
    if (recv.tag == XR_TAG_SYS_MUTEX)
        return xrt_sys_mutex_method_0(recv, sym);
    if (recv.tag == XR_TAG_SYS_RWLOCK)
        return xrt_sys_rwlock_method_0(recv, sym);
    if (recv.tag == XR_TAG_SYS_CONDVAR)
        return xrt_sys_condvar_method_0(recv, sym);
    if (recv.tag == XR_TAG_SYS_BARRIER)
        return xrt_sys_barrier_method_0(recv, sym);
    if (recv.tag == XR_TAG_I64) {
        if (sym == XRT_SYM_ABS)
            return XR_FROM_INT(xr_i64_abs_wrap(recv.i));
        if (sym == XRT_SYM_TOSTRING)
            return xrt_tostring(recv, 1);
        if (sym == XRT_SYM_TOHEX) {
            char buf[32];
            if (recv.i < 0)
                snprintf(buf, sizeof(buf), "-0x%" PRIX64, xr_i64_abs_magnitude(recv.i));
            else
                snprintf(buf, sizeof(buf), "0x%" PRIX64, (uint64_t) recv.i);
            return xrt_str_from_cstr(buf);
        }
    }
    if (recv.tag == XR_TAG_F64) {
        double v = recv.f;
        if (sym == XRT_SYM_TOSTRING)
            return xrt_tostring(recv, 2);
        /* floor/ceil/round return int (matching the VM float methods), not float. */
        if (sym == XRT_SYM_FLOOR)
            return XR_FROM_INT((int64_t) floor(v));
        if (sym == XRT_SYM_CEIL)
            return XR_FROM_INT((int64_t) ceil(v));
        if (sym == XRT_SYM_ROUND)
            return XR_FROM_INT((int64_t) round(v));
        if (sym == XRT_SYM_ABS)
            return XR_FROM_FLOAT(fabs(v));
        if (sym == XRT_SYM_SQRT)
            return XR_FROM_FLOAT(sqrt(v));
        if (sym == XRT_SYM_ISNAN)
            return XR_FROM_BOOL(isnan(v));
    }
    if (recv.tag == XR_TAG_BOOL && sym == XRT_SYM_TOSTRING)
        return xrt_tostring(recv, 0);
    if (recv.tag == XR_TAG_CHAR) {
        uint32_t cp = XR_TO_CHAR(recv);
        if (sym == XRT_SYM_TOSTRING)
            return xrt_char_to_string(cp);
        if (sym == XRT_SYM_ORD)
            return XR_FROM_INT((int64_t) cp);
        if (sym == XRT_SYM_IS_LETTER)
            return XR_FROM_BOOL(xrt_char_is_letter(cp));
        if (sym == XRT_SYM_IS_NUMBER)
            return XR_FROM_BOOL(xrt_char_is_number(cp));
        if (sym == XRT_SYM_IS_ALNUM)
            return XR_FROM_BOOL(xrt_char_is_alnum(cp));
        if (sym == XRT_SYM_IS_WHITESPACE)
            return XR_FROM_BOOL(xrt_char_is_whitespace(cp));
    }
    if (recv.tag == XR_TAG_ENUM && sym == XRT_SYM_TOSTRING)
        return xrt_tostring(recv, 0);
    return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
}

static inline XrValue xrt_str_from_core_slice(XrStringCoreSlice slice) {
    XrValue sv = xrt_str_alloc(slice.len);
    if (slice.len != 0)
        memcpy(xr_str_buf(sv), slice.data, slice.len);
    xr_str_buf(sv)[slice.len] = 0;
    return sv;
}

typedef struct XrtStringSplitCtx {
    XrValue array;
} XrtStringSplitCtx;

static inline bool xrt_str_split_emit(XrStringCoreSlice slice, void *user) {
    XrtStringSplitCtx *ctx = (XrtStringSplitCtx *) user;
    xrt_array_push(ctx->array, xrt_str_from_core_slice(slice));
    return true;
}

static inline XrValue xrt_str_split(const char *s, int64_t slen, const char *sep, size_t sep_len) {
    if (slen < 0)
        return XR_NULL_VAL;
    size_t len = (size_t) slen;
    XrStringCoreSplitPlan plan = xr_string_core_split_plan(s, len, sep, sep_len);
    if (plan.kind == XR_STRING_CORE_SPLIT_INVALID || plan.count > (size_t) INT64_MAX)
        return XR_NULL_VAL;

    XrValue arr = xrt_array_with_capacity((int64_t) plan.count);
    XrtStringSplitCtx ctx = {arr};
    size_t emitted = xr_string_core_split_each(s, len, sep, sep_len, xrt_str_split_emit, &ctx);
    return emitted == plan.count ? arr : XR_NULL_VAL;
}

/* String 1-arg method dispatch. */
static inline XrValue xrt_str_method_1(const char *s, int64_t slen, XrValue recv, int sym,
                                       XrValue arg0) {
    if ((sym == XRT_SYM_CONTAINS || sym == XRT_SYM_INCLUDES) && XR_IS_STR(arg0)) {
        return XR_FROM_BOOL(xr_string_core_contains(s, (size_t) slen, xr_str_data(arg0),
                                                    (size_t) xr_str_len(arg0)));
    }
    if (sym == XRT_SYM_INDEXOF && XR_IS_STR(arg0)) {
        return XR_FROM_INT((int64_t) xr_string_core_index_of(s, (size_t) slen, xr_str_data(arg0),
                                                             (size_t) xr_str_len(arg0)));
    }
    if (sym == XRT_SYM_SLICE && arg0.tag == XR_TAG_I64) {
        XrStringCoreSlice slice = xr_string_core_range_slice(s, (size_t) slen, arg0.i, slen);
        return xrt_str_from_core_slice(slice);
    }
    if (sym == XRT_SYM_STARTSWITH && XR_IS_STR(arg0)) {
        const char *p = xr_str_data(arg0);
        size_t plen = (size_t) xr_str_len(arg0);
        return XR_FROM_BOOL(xr_string_core_starts_with(s, (size_t) slen, p, plen));
    }
    if (sym == XRT_SYM_ENDSWITH && XR_IS_STR(arg0)) {
        const char *p = xr_str_data(arg0);
        size_t plen = (size_t) xr_str_len(arg0);
        return XR_FROM_BOOL(xr_string_core_ends_with(s, (size_t) slen, p, plen));
    }
    if (sym == XRT_SYM_CHARAT && arg0.tag == XR_TAG_I64) {
        XrStringCoreSlice slice = xr_string_core_utf8_char_slice_at(s, (size_t) slen, arg0.i);
        return slice.len == 0 ? XR_NULL_VAL : xrt_str_from_core_slice(slice);
    }
    if (sym == XRT_SYM_CONCAT && XR_IS_STR(arg0)) {
        const char *s2 = xr_str_data(arg0);
        size_t s2len = (size_t) xr_str_len(arg0);
        size_t rlen = (size_t) slen + s2len;
        XrValue sv = xrt_str_alloc(rlen);
        memcpy(xr_str_buf(sv), s, (size_t) slen);
        memcpy(xr_str_buf(sv) + slen, s2, s2len);
        xr_str_buf(sv)[rlen] = 0;
        return sv;
    }
    if (sym == XRT_SYM_LASTINDEXOF && XR_IS_STR(arg0)) {
        const char *needle = xr_str_data(arg0);
        size_t nlen = (size_t) xr_str_len(arg0);
        if (nlen == 0)
            return XR_FROM_INT(slen);
        for (int64_t i = slen - (int64_t) nlen; i >= 0; i--) {
            if (memcmp(s + i, needle, nlen) == 0)
                return XR_FROM_INT(i);
        }
        return XR_FROM_INT(-1);
    }
    if (sym == XRT_SYM_SPLIT && XR_IS_STR(arg0)) {
        const char *sep = xr_str_data(arg0);
        size_t seplen = (size_t) xr_str_len(arg0);
        return xrt_str_split(s, slen, sep, seplen);
    }
    if (sym == XRT_SYM_REPEAT && arg0.tag == XR_TAG_I64) {
        XrStringCoreRepeatPlan plan = xr_string_core_repeat_plan(s, (size_t) slen, arg0.i);
        if (plan.kind == XR_STRING_CORE_REPEAT_INVALID)
            return XR_NULL_VAL;
        if (plan.kind == XR_STRING_CORE_REPEAT_EMPTY)
            return xrt_str_alloc(0);
        if (plan.kind == XR_STRING_CORE_REPEAT_ORIGINAL)
            return recv;
        XrValue sv = xrt_str_alloc(plan.len);
        xr_string_core_repeat_write(xr_str_buf(sv), s, (size_t) slen, arg0.i);
        return sv;
    }
    if (sym == XRT_SYM_REPLACE && XR_IS_STR(arg0)) {
        /* replace(old) with empty string — 1-arg form */
        const char *old_s = xr_str_data(arg0);
        const char *found = strstr(s, old_s);
        if (!found)
            return recv;
        size_t olen = (size_t) xr_str_len(arg0);
        size_t rlen = (size_t) slen - olen;
        XrValue sv = xrt_str_alloc(rlen);
        char *r = xr_str_buf(sv);
        size_t pre = (size_t) (found - s);
        memcpy(r, s, pre);
        memcpy(r + pre, found + olen, (size_t) slen - pre - olen);
        r[rlen] = 0;
        return sv;
    }
    if ((sym == XRT_SYM_PAD_START || sym == XRT_SYM_PAD_END) && arg0.tag == XR_TAG_I64) {
        XrStringCorePadPlan plan = xr_string_core_pad_plan(s, (size_t) slen, arg0.i, NULL, 0);
        if (plan.kind == XR_STRING_CORE_PAD_INVALID || plan.kind == XR_STRING_CORE_PAD_ORIGINAL)
            return recv;
        XrValue sv = xrt_str_alloc(plan.len);
        xr_string_core_pad_write(xr_str_buf(sv), s, (size_t) slen, plan,
                                 sym == XRT_SYM_PAD_START ? XR_STRING_CORE_PAD_START
                                                          : XR_STRING_CORE_PAD_END);
        return sv;
    }
    if (sym == XRT_SYM_BYTE_AT && arg0.tag == XR_TAG_I64) {
        XrStringCoreSlice slice = xr_string_core_byte_slice_at(s, (size_t) slen, arg0.i);
        return slice.len == 0 ? XR_NULL_VAL : xrt_str_from_core_slice(slice);
    }
    if (sym == XRT_SYM_CODEPOINT_AT && arg0.tag == XR_TAG_I64) {
        int64_t target = arg0.i;
        if (target < 0)
            return XR_FROM_INT(-1);
        const unsigned char *p = (const unsigned char *) s;
        const unsigned char *end = p + slen;
        for (int64_t char_index = 0; p < end; char_index++) {
            int64_t remaining = (int64_t) (end - p);
            uint32_t cp = p[0];
            int advance = 1;
            if ((p[0] & 0x80u) == 0) {
                cp = p[0];
            } else if ((p[0] & 0xE0u) == 0xC0u && remaining >= 2 && (p[1] & 0xC0u) == 0x80u) {
                cp = ((uint32_t) (p[0] & 0x1Fu) << 6) | (uint32_t) (p[1] & 0x3Fu);
                advance = 2;
            } else if ((p[0] & 0xF0u) == 0xE0u && remaining >= 3 && (p[1] & 0xC0u) == 0x80u &&
                       (p[2] & 0xC0u) == 0x80u) {
                cp = ((uint32_t) (p[0] & 0x0Fu) << 12) | ((uint32_t) (p[1] & 0x3Fu) << 6) |
                     (uint32_t) (p[2] & 0x3Fu);
                advance = 3;
            } else if ((p[0] & 0xF8u) == 0xF0u && remaining >= 4 && (p[1] & 0xC0u) == 0x80u &&
                       (p[2] & 0xC0u) == 0x80u && (p[3] & 0xC0u) == 0x80u) {
                cp = ((uint32_t) (p[0] & 0x07u) << 18) | ((uint32_t) (p[1] & 0x3Fu) << 12) |
                     ((uint32_t) (p[2] & 0x3Fu) << 6) | (uint32_t) (p[3] & 0x3Fu);
                advance = 4;
            } else {
                cp = 0xFFFD;
            }
            if (char_index == target)
                return XR_FROM_INT(cp);
            p += advance;
        }
        return XR_FROM_INT(-1);
    }
    return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
}

static inline XrValue xrt_method_1(XrValue recv, int sym, XrValue arg0) {
    if (XR_IS_STR(recv)) {
        return xrt_str_method_1(xr_str_data(recv), xr_str_len(recv), recv, sym, arg0);
    }
    if (XR_IS_ARRAY(recv)) {
        xrt_array_t *a = (xrt_array_t *) recv.ptr;
        if (sym == XRT_SYM_PUSH) {
            xrt_array_push(recv, arg0);
            return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
        }
        if (sym == XRT_SYM_PUSH_UNCHECKED) {
            xrt_array_push_unchecked(recv, arg0);
            return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
        }
        if (sym == XRT_SYM_RESERVE)
            return xrt_array_reserve_value(recv, arg0);
        if (sym == XRT_SYM_SET_LENGTH_UNCHECKED)
            return xrt_bytes_set_length_unchecked_value(recv, arg0);
        if (sym == XRT_SYM_RESIZE)
            return xrt_array_resize_value(
                recv, arg0, a->elem_type == XR_ELEM_CHAR ? XR_FROM_CHAR(0) : XR_FROM_INT(0));
        if (sym == XRT_SYM_UNSHIFT) {
            xrt_array_check_store_or_abort(a, arg0, "Array.unshift");
            if (XR_UNLIKELY(a->data_storage == XR_ARRAY_DATA_BORROWED)) {
                fprintf(stderr, "xrt_array_unshift: cannot unshift array slice\n");
                abort();
            }
            if (XR_UNLIKELY(a->length >= a->capacity))
                xrt_array_data_grow(a, a->capacity == 0 ? 4 : a->capacity * 2);
            memmove((uint8_t *) a->data + a->elem_size, a->data,
                    (size_t) a->length * (size_t) a->elem_size);
            a->length++;
            xr_typed_set(a->data, 0, arg0, a->elem_type);
            XR_ARRAY_MARK_MUTATED(a);
            return XR_NULL_VAL;
        }
        if (sym == XRT_SYM_FILL) {
            return xrt_array_fill_value(recv, arg0, XR_FROM_INT(0), XR_FROM_INT(a->length));
        }
        if (sym == XRT_SYM_INDEXOF) {
            int handled;
            int64_t idx = xrt_array_indexof_typed_fast(a, arg0, &handled);
            if (handled)
                return XR_FROM_INT(idx);
            for (int64_t i = 0; i < a->length; i++) {
                XrValue elem = xr_typed_get(a->data, (int32_t) i, a->elem_type);
                if (xrt_eq(elem, arg0))
                    return XR_FROM_INT(i);
            }
            return XR_FROM_INT(-1);
        }
        if (sym == XRT_SYM_INCLUDES) {
            int handled;
            int64_t idx = xrt_array_indexof_typed_fast(a, arg0, &handled);
            if (handled)
                return XR_FROM_BOOL(idx >= 0);
            for (int64_t i = 0; i < a->length; i++) {
                XrValue elem = xr_typed_get(a->data, (int32_t) i, a->elem_type);
                if (xrt_eq(elem, arg0))
                    return XR_FROM_BOOL(1);
            }
            return XR_FROM_BOOL(0);
        }
        if (sym == XRT_SYM_JOIN && XR_IS_STR(arg0)) {
            const char *sep = xr_str_data(arg0);
            size_t seplen = (size_t) xr_str_len(arg0);
            size_t total = 0;
            for (int64_t i = 0; i < a->length; i++) {
                XrValue sv = xrt_tostring(xr_typed_get(a->data, (int32_t) i, a->elem_type), 0);
                total += (size_t) xr_str_len(sv);
                if (i < a->length - 1)
                    total += seplen;
            }
            XrValue result = xrt_str_alloc(total);
            char *r = xr_str_buf(result);
            size_t pos = 0;
            for (int64_t i = 0; i < a->length; i++) {
                XrValue sv = xrt_tostring(xr_typed_get(a->data, (int32_t) i, a->elem_type), 0);
                const char *p = xr_str_data(sv);
                size_t plen = (size_t) xr_str_len(sv);
                memcpy(r + pos, p, plen);
                pos += plen;
                if (i < a->length - 1) {
                    memcpy(r + pos, sep, seplen);
                    pos += seplen;
                }
            }
            r[total] = 0;
            return result;
        }
        /* Higher-order callbacks are AOT closures. */
        if (arg0.tag == XR_TAG_CLOSURE) {
            xrt_closure_t *cl = (xrt_closure_t *) arg0.ptr;
            typedef XrValue (*xrt_fn1_t)(xrt_closure_t *, XrValue);
            xrt_fn1_t fn = (xrt_fn1_t) cl->fn;
            if (sym == XRT_SYM_SORT)
                return xrt_array_sort(recv, cl);
            if (sym == XRT_SYM_MAP) {
                return xrt_array_map_typed(recv, arg0, XR_ELEM_ANY);
            }
            if (sym == XRT_SYM_FILTER) {
                return xrt_array_filter_typed(recv, arg0);
            }
            if (sym == XRT_SYM_FOREACH) {
                for (int64_t i = 0; i < a->length; i++)
                    fn(cl, xr_typed_get(a->data, (int32_t) i, a->elem_type));
                return XR_NULL_VAL;
            }
        }
    }
    if (XR_IS_MAP(recv)) {
        xrt_map_t *m = (xrt_map_t *) recv.ptr;
        if (sym == XRT_SYM_GET)
            return xrt_map_get_owned(m, arg0);
        if (sym == XRT_SYM_HAS)
            return XR_FROM_BOOL(xrt_map_has(m, arg0));
        if (sym == XRT_SYM_DELETE)
            return XR_FROM_BOOL(xrt_map_delete(m, arg0));
    }
    if (XR_IS_SET(recv)) {
        xrt_set_t *s = (xrt_set_t *) recv.ptr;
        if (sym == XRT_SYM_ADD) {
            if ((s->flags & XR_SET_FLAG_WEAK) && !xrt_weak_value_is_heap_object(arg0))
                return XR_NULL_VAL;
            (void) xrt_set_add(s, arg0);
            return XR_NULL_VAL;
        }
        if (sym == XRT_SYM_HAS)
            return XR_FROM_BOOL(xrt_set_has(s, arg0));
        if (sym == XRT_SYM_DELETE)
            return XR_FROM_BOOL(xrt_set_delete(s, arg0));
    }
    if (recv.tag == XR_TAG_RANGE)
        return xrt_range_method_1(recv, sym, arg0);
    if (recv.tag == XR_TAG_DATETIME)
        return xrt_datetime_method_1(recv, sym, arg0);
    if (recv.tag == XR_TAG_SYS_CONDVAR)
        return xrt_sys_condvar_method_1(recv, sym, arg0);
    if (recv.tag == XR_TAG_SYS_ONCE)
        return xrt_sys_once_method_1(recv, sym, arg0);
    if (recv.tag == XR_TAG_F64 && sym == XRT_SYM_POW) {
        double exp = (arg0.tag == XR_TAG_F64) ? arg0.f : (double) arg0.i;
        return XR_FROM_FLOAT(pow(recv.f, exp));
    }
    /* toFixed(digits): clamp decimals to [0, XR_TOFIXED_MAX_DECIMALS] via the
     * shared numeric core, matching the VM (negative -> 0, large -> capped). */
    if (recv.tag == XR_TAG_F64 && sym == XRT_SYM_TOFIXED && arg0.tag == XR_TAG_I64) {
        char buf[64];
        xr_numeric_core_format_fixed(buf, sizeof(buf), recv.f, arg0.i);
        return xrt_str_from_cstr(buf);
    }
    if (recv.tag == XR_TAG_I64 && arg0.tag == XR_TAG_I64) {
        int64_t out;
        if (sym == XRT_SYM_CHECKED_ADD)
            return xr_i64_checked_add(recv.i, arg0.i, &out) ? XR_FROM_INT(out) : XR_NULL_VAL;
        if (sym == XRT_SYM_CHECKED_SUB)
            return xr_i64_checked_sub(recv.i, arg0.i, &out) ? XR_FROM_INT(out) : XR_NULL_VAL;
        if (sym == XRT_SYM_CHECKED_MUL)
            return xr_i64_checked_mul(recv.i, arg0.i, &out) ? XR_FROM_INT(out) : XR_NULL_VAL;
        if (sym == XRT_SYM_SATURATING_ADD)
            return XR_FROM_INT(xr_i64_saturating_add(recv.i, arg0.i));
        if (sym == XRT_SYM_SATURATING_SUB)
            return XR_FROM_INT(xr_i64_saturating_sub(recv.i, arg0.i));
        if (sym == XRT_SYM_SATURATING_MUL)
            return XR_FROM_INT(xr_i64_saturating_mul(recv.i, arg0.i));
        if (sym == XRT_SYM_WRAPPING_ADD)
            return XR_FROM_INT(xr_i64_add_wrap(recv.i, arg0.i));
        if (sym == XRT_SYM_WRAPPING_SUB)
            return XR_FROM_INT(xr_i64_sub_wrap(recv.i, arg0.i));
        if (sym == XRT_SYM_WRAPPING_MUL)
            return XR_FROM_INT(xr_i64_mul_wrap(recv.i, arg0.i));
    }
    /* max/min accept int or float operands. */
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
    return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
}

static inline XrValue xrt_method_2(XrValue recv, int sym, XrValue arg0, XrValue arg1) {
    if (XR_IS_STR(recv) && (sym == XRT_SYM_SLICE || sym == XRT_SYM_SUBSTRING)) {
        const char *s = xr_str_data(recv);
        size_t slen = (size_t) xr_str_len(recv);
        int64_t start = (arg0.tag == XR_TAG_I64) ? arg0.i : 0;
        int64_t end = (arg1.tag == XR_TAG_I64) ? arg1.i : xr_string_core_len_i64(slen);
        XrStringCoreSlice slice = (sym == XRT_SYM_SUBSTRING)
                                      ? xr_string_core_substring_slice(s, slen, start, end)
                                      : xr_string_core_range_slice(s, slen, start, end);
        return xrt_str_from_core_slice(slice);
    }
    if (XR_IS_STR(recv) && sym == XRT_SYM_REPLACEALL && XR_IS_STR(arg0) && XR_IS_STR(arg1)) {
        const char *s = xr_str_data(recv);
        const char *old_s = xr_str_data(arg0);
        const char *new_s = xr_str_data(arg1);
        size_t slen = (size_t) xr_str_len(recv);
        size_t olen = (size_t) xr_str_len(arg0), nlen = (size_t) xr_str_len(arg1);
        XrStringCoreReplacePlan plan =
            xr_string_core_replace_plan(s, slen, old_s, olen, new_s, nlen, true);
        if (plan.kind == XR_STRING_CORE_REPLACE_INVALID)
            return XR_NULL_VAL;
        if (plan.kind == XR_STRING_CORE_REPLACE_ORIGINAL)
            return recv;
        XrValue sv = xrt_str_alloc(plan.len);
        xr_string_core_replace_write(xr_str_buf(sv), s, slen, old_s, olen, new_s, nlen, plan, true);
        return sv;
    }
    if (XR_IS_STR(recv) && (sym == XRT_SYM_PAD_START || sym == XRT_SYM_PAD_END) &&
        arg0.tag == XR_TAG_I64 && XR_IS_STR(arg1)) {
        const char *s = xr_str_data(recv);
        const char *pad = xr_str_data(arg1);
        XrStringCorePadPlan plan = xr_string_core_pad_plan(s, (size_t) xr_str_len(recv), arg0.i,
                                                           pad, (size_t) xr_str_len(arg1));
        if (plan.kind == XR_STRING_CORE_PAD_INVALID || plan.kind == XR_STRING_CORE_PAD_ORIGINAL)
            return recv;
        XrValue sv = xrt_str_alloc(plan.len);
        xr_string_core_pad_write(xr_str_buf(sv), s, (size_t) xr_str_len(recv), plan,
                                 sym == XRT_SYM_PAD_START ? XR_STRING_CORE_PAD_START
                                                          : XR_STRING_CORE_PAD_END);
        return sv;
    }
    if (XR_IS_STR(recv) && sym == XRT_SYM_REPLACE && XR_IS_STR(arg0) && XR_IS_STR(arg1)) {
        const char *s = xr_str_data(recv);
        const char *old_s = xr_str_data(arg0);
        const char *new_s = xr_str_data(arg1);
        size_t slen = (size_t) xr_str_len(recv);
        size_t olen = (size_t) xr_str_len(arg0), nlen = (size_t) xr_str_len(arg1);
        XrStringCoreReplacePlan plan =
            xr_string_core_replace_plan(s, slen, old_s, olen, new_s, nlen, false);
        if (plan.kind == XR_STRING_CORE_REPLACE_INVALID)
            return XR_NULL_VAL;
        if (plan.kind == XR_STRING_CORE_REPLACE_ORIGINAL)
            return recv;
        XrValue sv = xrt_str_alloc(plan.len);
        xr_string_core_replace_write(xr_str_buf(sv), s, slen, old_s, olen, new_s, nlen, plan,
                                     false);
        return sv;
    }
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_REDUCE && arg0.tag == XR_TAG_CLOSURE)
        return xrt_array_reduce_typed(recv, arg0, arg1);
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_RESIZE)
        return xrt_array_resize_value(recv, arg0, arg1);
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_REPEAT_FROM_UNCHECKED)
        return xrt_bytes_repeat_from_unchecked_value(recv, arg0, arg1);
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_FILL) {
        xrt_array_t *a = (xrt_array_t *) recv.ptr;
        return xrt_array_fill_value(recv, arg0, arg1, XR_FROM_INT(a->length));
    }
    if (recv.tag == XR_TAG_SYS_CONDVAR)
        return xrt_sys_condvar_method_2(recv, sym, arg0, arg1);
    if (XR_IS_MAP(recv) && sym == XRT_SYM_SET) {
        xrt_map_t *m = (xrt_map_t *) recv.ptr;
        if ((m->flags & XR_MAP_FLAG_WEAK) && !xrt_weak_value_is_heap_object(arg0))
            return XR_NULL_VAL;
        xrt_map_set(m, arg0, arg1);
        return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
    }
    if (recv.tag == XR_TAG_DATETIME)
        return xrt_datetime_method_2(recv, sym, arg0, arg1);
    return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
}

static inline XrValue xrt_method_3(XrValue recv, int sym, XrValue arg0, XrValue arg1,
                                   XrValue arg2) {
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_APPEND_FROM_UNCHECKED)
        return xrt_bytes_append_from_unchecked_value(recv, arg0, arg1, arg2);
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_REPEAT_AT_UNCHECKED)
        return xrt_bytes_repeat_at_unchecked_value(recv, arg0, arg1, arg2);
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_WILD_REPEAT_AT_UNCHECKED)
        return xrt_bytes_wild_repeat_at_unchecked_value(recv, arg0, arg1, arg2);
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_COMMON_PREFIX_UNCHECKED)
        return xrt_bytes_common_prefix_unchecked_value(recv, arg0, arg1, arg2);
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_FILL)
        return xrt_array_fill_value(recv, arg0, arg1, arg2);
    return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
}

static inline XrValue xrt_method_4(XrValue recv, int sym, XrValue arg0, XrValue arg1, XrValue arg2,
                                   XrValue arg3) {
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_WRITE_FROM_UNCHECKED)
        return xrt_bytes_write_from_unchecked_value(recv, arg0, arg1, arg2, arg3);
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_WILD_COPY_FROM_NONOVERLAPPING_UNCHECKED)
        return xrt_bytes_wild_copy_from_nonoverlapping_unchecked_value(recv, arg0, arg1, arg2,
                                                                       arg3);
    return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
}

#include "xrt_getprop.inc.c"

#endif  // XRT_METHOD_H
