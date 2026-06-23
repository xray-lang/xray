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
        case XR_TAG_DATETIME:
            return 1;
        default:
            return 0;
    }
}

/* toString helper. */

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
    if (val.tag == XR_TAG_DATETIME)
        return xrt_datetime_method_0(val, XRT_SYM_TOSTRING);
    if (val.tag == XR_TAG_NULL)
        return xr_box_str("null");
    if (val.tag == XR_TAG_BOOL)
        return xr_box_str(val.i ? "true" : "false");
    if (val.tag == XR_TAG_ENUM) {
        char tmp[256];
        return xrt_str_from_cstr(xr_to_cstr(val, tmp, sizeof(tmp)));
    }
    return xr_box_str("[object]");
}

static XrValue xrt_to_int(XrValue val) {
    if (XR_IS_INT(val))
        return val;
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
        return XR_FROM_BOOL((xr_str_data(val))[0] != '\0');
    if (XR_IS_ARRAY(val))
        return XR_FROM_BOOL(((xrt_array_t *) val.ptr)->length > 0);
    if (XR_IS_MAP(val))
        return XR_FROM_BOOL(xrt_map_len((xrt_map_t *) val.ptr) > 0);
    if (XR_IS_SET(val))
        return XR_FROM_BOOL(((xrt_set_t *) val.ptr)->len > 0);
    return XR_TRUE_VAL;
}

/* Fixed-arity method dispatch is intentionally inlineable by the C compiler. */

/* String 0-arg method dispatch. */
static inline XrValue xrt_str_method_0(const char *s, int64_t slen, XrValue recv, int sym) {
    if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
        return XR_FROM_INT(slen);
    if (sym == XRT_SYM_IS_EMPTY)
        return XR_FROM_BOOL(slen == 0);
    if (sym == XRT_SYM_TOSTRING)
        return recv;
    if (sym == XRT_SYM_TRIM || sym == XRT_SYM_TRIM_START || sym == XRT_SYM_TRIM_END) {
        XrStringCoreTrimMode mode = XR_STRING_CORE_TRIM_BOTH;
        if (sym == XRT_SYM_TRIM_START)
            mode = XR_STRING_CORE_TRIM_START;
        else if (sym == XRT_SYM_TRIM_END)
            mode = XR_STRING_CORE_TRIM_END;
        XrStringCoreSlice slice = xr_string_core_trim_slice(s, (size_t) slen, mode);
        XrValue sv = xrt_str_alloc(slice.len);
        memcpy(xr_str_buf(sv), slice.data, slice.len);
        xr_str_buf(sv)[slice.len] = 0;
        return sv;
    }
    if (sym == XRT_SYM_TOLOWER) {
        XrValue sv = xrt_str_alloc((size_t) slen);
        char *r = xr_str_buf(sv);
        xr_string_core_ascii_lower_write(r, s, (size_t) slen);
        return sv;
    }
    if (sym == XRT_SYM_TOUPPER) {
        XrValue sv = xrt_str_alloc((size_t) slen);
        char *r = xr_str_buf(sv);
        xr_string_core_ascii_upper_write(r, s, (size_t) slen);
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
    if (sym == XRT_SYM_ORD)
        return XR_FROM_INT(slen > 0 ? (int64_t) (unsigned char) s[0] : 0);
    if (sym == XRT_SYM_REVERSE) {
        XrValue sv = xrt_str_alloc((size_t) slen);
        char *r = xr_str_buf(sv);
        xr_string_core_reverse_utf8_write(r, s, (size_t) slen);
        return sv;
    }
    return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
}

static inline XrValue xrt_method_0(XrValue recv, int sym) {
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
    if (recv.tag == XR_TAG_I64) {
        if (sym == XRT_SYM_ABS)
            return XR_FROM_INT(recv.i < 0 ? -recv.i : recv.i);
        if (sym == XRT_SYM_TOSTRING)
            return xrt_tostring(recv, 1);
        if (sym == XRT_SYM_TOHEX) {
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%02" PRIX64, (uint64_t) recv.i);
            return xrt_str_from_cstr(buf);
        }
    }
    if (recv.tag == XR_TAG_F64) {
        double v = recv.f;
        if (sym == XRT_SYM_TOSTRING)
            return xrt_tostring(recv, 2);
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
    if (recv.tag == XR_TAG_BOOL && sym == XRT_SYM_TOSTRING)
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

    XrValue arr = xrt_array_new((int64_t) plan.count);
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
        return XR_FROM_INT((int64_t) xr_string_core_last_index_of(
            s, (size_t) slen, xr_str_data(arg0), (size_t) xr_str_len(arg0)));
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
        uint32_t cp = 0;
        return xr_string_core_codepoint_at(s, (size_t) slen, arg0.i, &cp) ? XR_FROM_INT(cp)
                                                                          : XR_FROM_INT(-1);
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
        if (sym == XRT_SYM_RESERVE)
            return xrt_array_reserve_value(recv, arg0);
        if (sym == XRT_SYM_RESIZE)
            return xrt_array_resize_value(recv, arg0, XR_FROM_INT(0));
        if (sym == XRT_SYM_LOADU32LE)
            return xrt_bytes_load_u32_le(recv, arg0);
        if (sym == XRT_SYM_LOADU64LE)
            return xrt_bytes_load_u64_le(recv, arg0);
        if (sym == XRT_SYM_UNSHIFT) {
            xrt_array_push(recv, XR_NULL_VAL);
            a = (xrt_array_t *) recv.ptr;
            for (int64_t i = a->length - 1; i > 0; i--) {
                XrValue prev = xr_typed_get(a->data, (int32_t) (i - 1), a->elem_type);
                xr_typed_set(a->data, (int32_t) i, prev, a->elem_type);
            }
            xr_typed_set(a->data, 0, arg0, a->elem_type);
            return XR_NULL_VAL;
        }
        if (sym == XRT_SYM_FILL) {
            if (xrt_array_fill_typed_fast(a, arg0))
                return recv;
            for (int64_t i = 0; i < a->length; i++)
                xr_typed_set(a->data, (int32_t) i, arg0, a->elem_type);
            return recv;
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
        if (sym == XRT_SYM_SLICE && arg0.tag == XR_TAG_I64) {
            return xrt_array_slice_view(recv, arg0.i, a->length);
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
            return xrt_map_get(m, arg0);
        if (sym == XRT_SYM_HAS)
            return XR_FROM_BOOL(xrt_map_has(m, arg0));
        if (sym == XRT_SYM_DELETE)
            return XR_FROM_BOOL(xrt_map_delete(m, arg0));
    }
    if (XR_IS_SET(recv)) {
        xrt_set_t *s = (xrt_set_t *) recv.ptr;
        if (sym == XRT_SYM_ADD || sym == XRT_SYM_SET) {
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
    if (recv.tag == XR_TAG_F64 && sym == XRT_SYM_POW) {
        double exp = (arg0.tag == XR_TAG_F64) ? arg0.f : (double) arg0.i;
        return XR_FROM_FLOAT(pow(recv.f, exp));
    }
    /* toFixed(digits). */
    if (recv.tag == XR_TAG_F64 && sym == XRT_SYM_TOFIXED && arg0.tag == XR_TAG_I64) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.*f", (int) arg0.i, recv.f);
        return xrt_str_from_cstr(buf);
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
    if (recv.tag == XR_TAG_DATETIME)
        return xrt_datetime_method_2(recv, sym, arg0, arg1);
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
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_SLICE) {
        xrt_array_t *a = (xrt_array_t *) recv.ptr;
        int64_t start = (arg0.tag == XR_TAG_I64) ? arg0.i : 0;
        int64_t end = (arg1.tag == XR_TAG_I64) ? arg1.i : a->length;
        return xrt_array_slice_view(recv, start, end);
    }
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_REDUCE && arg0.tag == XR_TAG_CLOSURE)
        return xrt_array_reduce_typed(recv, arg0, arg1);
    if (XR_IS_ARRAY(recv) && sym == XRT_SYM_RESIZE)
        return xrt_array_resize_value(recv, arg0, arg1);
    if (XR_IS_MAP(recv) && sym == XRT_SYM_SET) {
        xrt_map_t *m = (xrt_map_t *) recv.ptr;
        if ((m->flags & XR_MAP_FLAG_WEAK) && !xrt_weak_value_is_heap_object(arg0))
            return XR_NULL_VAL;
        xrt_map_set(m, arg0, arg1);
        return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
    }
    return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
}

#include "xrt_getprop.inc.c"

#endif  // XRT_METHOD_H
