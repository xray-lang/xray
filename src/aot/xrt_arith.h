/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_arith.h - Tagged arithmetic, comparison, and print
 */

#ifndef XRT_ARITH_H
#define XRT_ARITH_H

#include "xrt_value.h"
#include "xrt_arc.h"  // xrt_str_concat used by xrt_add
#include "xrt_range.h"
#include "xrt_coll.h"  // forward-declares xrt_throw_exc (div/mod by zero); the
                       // definition is provided by xrt_exception.h in the full
                       // xrt.h build and by the host TU in standalone unit tests
#include "../shared/xr_int_arith.h"

/* =========================================================================
 * Tagged arithmetic — all inline, no extern dependency
 * ========================================================================= */

/* int64 add/sub/mul with two's-complement wrap on overflow.
 * Signed overflow is UB in C, so compute on uint64_t and cast back. This is
 * the single source of truth for AOT integer arithmetic wrap and MUST match
 * the VM (uint64 wrap in xvm_dispatch_arith) and xi_opt constant folding so
 * INT64_MAX + 1, INT64_MIN - 1, etc. produce identical results across tiers. */
static inline int64_t xrt_i64_add(int64_t a, int64_t b) {
    return xr_i64_add_wrap(a, b);
}
static inline int64_t xrt_i64_sub(int64_t a, int64_t b) {
    return xr_i64_sub_wrap(a, b);
}
static inline int64_t xrt_i64_mul(int64_t a, int64_t b) {
    return xr_i64_mul_wrap(a, b);
}

static inline XrValue xrt_add(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(xrt_i64_add(a.i, b.i));
    if (XR_IS_STR(a) && XR_IS_STR(b))
        return xrt_str_concat_value(a, b); /* header lengths, no strlen */
    if (XR_IS_STR(a) || XR_IS_STR(b)) {
        char ba[64], bb[64];
        return xrt_str_concat(xr_to_cstr(a, ba, sizeof(ba)), xr_to_cstr(b, bb, sizeof(bb)));
    }
    double fa = a.tag == XR_TAG_I64 ? (double) a.i : a.f;
    double fb = b.tag == XR_TAG_I64 ? (double) b.i : b.f;
    return XR_FROM_FLOAT(fa + fb);
}

static inline XrValue xrt_sub(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(xrt_i64_sub(a.i, b.i));
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return XR_FROM_FLOAT(fa - fb);
}

static inline XrValue xrt_mul(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(xrt_i64_mul(a.i, b.i));
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return XR_FROM_FLOAT(fa * fb);
}

/* Integer div/mod with zero-check + wrap.
 * Single source of truth for every AOT integer divide path (typed scalar
 * codegen in xi_cgen and the tagged xrt_div / xrt_mod below).
 *   b == 0          → throw (matches VM E0420 / E0421)
 *   INT64_MIN / -1  → INT64_MIN (unsigned negate; matches xi_opt fold)
 *   INT64_MIN % -1  → 0 */
static inline int64_t xrt_int_div(int64_t a, int64_t b) {
    if (XR_UNLIKELY(b == 0))
        xrt_throw_exc(xr_box_str("E0420: division by zero"));
    return xr_i64_div_wrap(a, b);
}
static inline int64_t xrt_int_mod(int64_t a, int64_t b) {
    if (XR_UNLIKELY(b == 0))
        xrt_throw_exc(xr_box_str("E0421: modulo by zero"));
    return xr_i64_mod_wrap(a, b);
}

/* Shifts: the language defines the count as taken mod 64 (spec: "shift count
 * is taken modulo 64 — unlike C, xray shifts are always defined"). Matches
 * xi_opt constant folding and AOT hardware behavior (x64 SHL/SAR with CL,
 * ARM64 LSL/ASR, RISC-V SLL/SRA all mask to 6 bits). Left shift goes through
 * uint64_t because shifting into/past the sign bit is UB on signed in C. */
static inline int64_t xrt_i64_shl(int64_t a, int64_t b) {
    return xr_i64_shl_wrap(a, b);
}
static inline int64_t xrt_i64_shr(int64_t a, int64_t b) {
    return xr_i64_shr_wrap(a, b);
}

static inline XrValue xrt_div(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(xrt_int_div(a.i, b.i));
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return XR_FROM_FLOAT(fa / fb);
}

static inline XrValue xrt_mod(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(xrt_int_mod(a.i, b.i));
    xrt_throw_exc(xr_box_str("E0404: modulo requires integer types"));
}

static inline XrValue xrt_neg(XrValue a) {
    if (a.tag == XR_TAG_I64)
        return XR_FROM_INT(xr_i64_neg_wrap(a.i));
    if (a.tag == XR_TAG_F64)
        return XR_FROM_FLOAT(-a.f);
    return XR_FROM_INT(0);
}

/* =========================================================================
 * Inline tagged comparisons
 * ========================================================================= */

static inline int64_t xrt_lt(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return a.i < b.i;
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return fa < fb;
}

static inline int64_t xrt_le(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return a.i <= b.i;
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return fa <= fb;
}

/* =========================================================================
 * Inline print
 * ========================================================================= */

/* Recursion / element caps must match the VM formatter (xvalue_format.c:
 * XR_FORMAT_MAX_DEPTH / XR_FORMAT_MAX_ELEMENTS) so container printing is
 * byte-identical across backends. */
#define XRT_FORMAT_MAX_DEPTH 3
#define XRT_FORMAT_MAX_ELEMENTS 32

/* Format sink: a NULL strbuf writes to stdout, otherwise text is appended to the
 * builder. The print path and value.toString() share xrt_format_value so they
 * render scalars and containers identically (single source of truth). */
static inline void xrt_fmt_puts(xrt_strbuf_t *sb, const char *s, size_t n) {
    if (sb) {
        xrt_strbuf_grow(sb, (int64_t) n);
        memcpy(sb->buf + sb->len, s, n);
        sb->len += (int64_t) n;
        sb->buf[sb->len] = 0;
    } else if (n > 0) {
        fwrite(s, 1, n, stdout);
    }
}
static inline void xrt_fmt_cstr(xrt_strbuf_t *sb, const char *s) {
    xrt_fmt_puts(sb, s, strlen(s));
}
static inline void xrt_fmt_char(xrt_strbuf_t *sb, char c) {
    xrt_fmt_puts(sb, &c, 1);
}

static void xrt_format_value(XrValue v, xrt_strbuf_t *sb, int depth) {
    switch (v.tag) {
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            /* Nested strings are quoted, matching the VM formatter. */
            if (depth > 0)
                xrt_fmt_char(sb, '"');
            xrt_fmt_puts(sb, xr_str_data(v), (size_t) xr_str_len(v));
            if (depth > 0)
                xrt_fmt_char(sb, '"');
            return;
        case XR_TAG_I64: {
            char buf[24];
            int n = snprintf(buf, sizeof(buf), "%lld", (long long) v.i);
            xrt_fmt_puts(sb, buf, (size_t) n);
            return;
        }
        case XR_TAG_F64: {
            char buf[64];
            xrt_format_float(buf, sizeof(buf), v.f);
            xrt_fmt_cstr(sb, buf);
            return;
        }
        case XR_TAG_BOOL:
            xrt_fmt_cstr(sb, v.i ? "true" : "false");
            return;
        case XR_TAG_CHAR: {
            char buf[4];
            int n = xrt_char_utf8_encode(XR_TO_CHAR(v), buf);
            if (n > 0)
                xrt_fmt_puts(sb, buf, (size_t) n);
            return;
        }
        case XR_TAG_NULL:
            xrt_fmt_cstr(sb, "null");
            return;
        case XR_TAG_ENUM: {
            char buf[256];
            xrt_fmt_cstr(sb, xr_to_cstr(v, buf, sizeof(buf)));
            return;
        }
        case XR_TAG_RANGE: {
            char buf[96];
            xrt_range_format_buf((const xrt_range_t *) v.ptr, buf, sizeof(buf));
            xrt_fmt_cstr(sb, buf);
            return;
        }
        default:
            break;
    }

    if (depth > XRT_FORMAT_MAX_DEPTH) {
        xrt_fmt_cstr(sb, "...");
        return;
    }

    switch (xrt_value_kind(v)) {
        case XR_TAG_ARRAY: {
            xrt_array_t *a = (xrt_array_t *) v.ptr;
            if (a && a->adt_enum_name && a->adt_member_name) {
                char hdr[256];
                int n = snprintf(hdr, sizeof(hdr), "%s.%s", a->adt_enum_name, a->adt_member_name);
                xrt_fmt_puts(sb, hdr, (size_t) n);
                if (a->length > 1) {
                    xrt_fmt_char(sb, '(');
                    for (int64_t i = 1; i < a->length; i++) {
                        if (i > 1)
                            xrt_fmt_cstr(sb, ", ");
                        xrt_format_value(xr_typed_get(a->data, (int32_t) i, a->elem_type), sb,
                                         depth + 1);
                    }
                    xrt_fmt_char(sb, ')');
                }
                return;
            }
            int64_t len = a ? a->length : 0;
            int64_t limit = len > XRT_FORMAT_MAX_ELEMENTS ? XRT_FORMAT_MAX_ELEMENTS : len;
            xrt_fmt_char(sb, '[');
            for (int64_t i = 0; i < limit; i++) {
                if (i > 0)
                    xrt_fmt_cstr(sb, ", ");
                xrt_format_value(xr_typed_get(a->data, (int32_t) i, a->elem_type), sb, depth + 1);
            }
            if (len > limit) {
                char more[48];
                int n = snprintf(more, sizeof(more), ", ...(%lld more)", (long long) (len - limit));
                xrt_fmt_puts(sb, more, (size_t) n);
            }
            xrt_fmt_char(sb, ']');
            return;
        }
        case XR_TAG_TUPLE: {
            xrt_tuple_t *t = (xrt_tuple_t *) v.ptr;
            int64_t len = t ? t->len : 0;
            int64_t limit = len > XRT_FORMAT_MAX_ELEMENTS ? XRT_FORMAT_MAX_ELEMENTS : len;
            xrt_fmt_char(sb, '(');
            for (int64_t i = 0; i < limit; i++) {
                if (i > 0)
                    xrt_fmt_cstr(sb, ", ");
                xrt_format_value(t->items[i], sb, depth + 1);
            }
            if (len > limit) {
                char more[48];
                int n = snprintf(more, sizeof(more), ", ...(%lld more)", (long long) (len - limit));
                xrt_fmt_puts(sb, more, (size_t) n);
            }
            if (len == 1)
                xrt_fmt_char(sb, ',');
            xrt_fmt_char(sb, ')');
            return;
        }
        case XR_TAG_MAP: {
            xrt_map_t *m = (xrt_map_t *) v.ptr;
            int64_t total = m ? xrt_map_len(m) : 0;
            int64_t n_slots = !m ? 0 : (xrt_map_is_typed(m) ? m->order_len : (int64_t) m->nentries);
            int64_t count = 0;
            xrt_fmt_cstr(sb, "#{");
            for (int64_t i = 0; i < n_slots && count < XRT_FORMAT_MAX_ELEMENTS; i++) {
                int64_t slot = xrt_map_is_typed(m) ? m->order[i] : i;
                if (!xrt_map_slot_is_full(m, slot))
                    continue;
                if (count > 0)
                    xrt_fmt_cstr(sb, ", ");
                xrt_format_value(xrt_map_slot_key(m, slot), sb, depth + 1);
                xrt_fmt_cstr(sb, ": ");
                xrt_format_value(xrt_map_slot_value(m, slot), sb, depth + 1);
                count++;
            }
            if (total > count) {
                char more[48];
                int n =
                    snprintf(more, sizeof(more), ", ...(%lld more)", (long long) (total - count));
                xrt_fmt_puts(sb, more, (size_t) n);
            }
            xrt_fmt_char(sb, '}');
            return;
        }
        case XR_TAG_SET: {
            xrt_set_t *s = (xrt_set_t *) v.ptr;
            int64_t total = s ? xrt_set_len(s) : 0;
            int64_t n_slots = !s ? 0 : (xrt_set_is_typed(s) ? s->order_len : (int64_t) s->nentries);
            int64_t count = 0;
            xrt_fmt_cstr(sb, "#[");
            for (int64_t i = 0; i < n_slots && count < XRT_FORMAT_MAX_ELEMENTS; i++) {
                int64_t slot = xrt_set_is_typed(s) ? s->order[i] : i;
                if (!xrt_set_slot_is_full(s, slot))
                    continue;
                if (count > 0)
                    xrt_fmt_cstr(sb, ", ");
                xrt_format_value(xrt_set_slot_item(s, slot), sb, depth + 1);
                count++;
            }
            if (total > count) {
                char more[48];
                int n =
                    snprintf(more, sizeof(more), ", ...(%lld more)", (long long) (total - count));
                xrt_fmt_puts(sb, more, (size_t) n);
            }
            xrt_fmt_char(sb, ']');
            return;
        }
        default: {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "<object@%p>", v.ptr);
            xrt_fmt_puts(sb, buf, (size_t) n);
            return;
        }
    }
}

static void xrt_print_value(XrValue v, int depth) {
    xrt_format_value(v, NULL, depth);
}

/* value.toString() for containers and other non-scalars: render via the shared
 * formatter into a string (matches the VM's xr_value_to_string output). */
static inline XrValue xrt_value_to_string(XrValue v) {
    if (XR_IS_STR(v))
        return v;
    XrValue sbv = xrt_strbuf_new();
    xrt_format_value(v, (xrt_strbuf_t *) sbv.ptr, 0);
    return xrt_strbuf_finish(sbv);
}

static inline void xrt_print(XrValue v) {
    xrt_print_value(v, 0);
}

static inline void xrt_println(XrValue v) {
    xrt_print(v);
    printf("\n");
}

/* typeof(x) — return integer type ID matching VM XrTypeId.
 * XR_TID_INT=8, XR_TID_FLOAT=11, XR_TID_BOOL=1, XR_TID_NULL=0,
 * XR_TID_STRING=12, XR_TID_FUNCTION=13, XR_TID_ARRAY=14, XR_TID_SET=15,
 * XR_TID_MAP=16. */
static inline int64_t xrt_typeof_id(XrValue v) {
    switch (xrt_value_kind(v)) {
        case XR_TAG_I64:
            return 8; /* XR_TID_INT */
        case XR_TAG_F64:
            return 11; /* XR_TID_FLOAT */
        case XR_TAG_BOOL:
            return 1; /* XR_TID_BOOL */
        case XR_TAG_CHAR:
            return 41; /* XR_TID_CHAR */
        case XR_TAG_NULL:
            return 0; /* XR_TID_NULL */
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            return 12; /* XR_TID_STRING */
        case XR_TAG_ARRAY: {
            const xrt_array_t *arr = (const xrt_array_t *) v.ptr;
            if (arr && arr->adt_enum_name)
                return 25; /* XR_TID_ENUM_VALUE */
            return 14;     /* XR_TID_ARRAY */
        }
        case XR_TAG_SET:
            return 15; /* XR_TID_SET */
        case XR_TAG_MAP:
            return 16; /* XR_TID_MAP */
        case XR_TAG_PTR:
            if (v.ptr && v.heap_type == 0) {
                const xrt_json_t *obj = (const xrt_json_t *) v.ptr;
                return obj->object_kind == XRT_OBJECT_RECORD ? 41 : 18;
            }
            return 17; /* XR_TID_INSTANCE */
        case XR_TAG_CLOSURE:
            return 13; /* XR_TID_FUNCTION */
        case XR_TAG_STRBUF:
            return 20; /* XR_TID_STRINGBUILDER */
        case XR_TAG_RANGE:
            return 31; /* XR_TID_RANGE */
        case XR_TAG_ENUM:
            return 25; /* XR_TID_ENUM_VALUE */
        default:
            return 17; /* XR_TID_INSTANCE */
    }
}

/* typename(x) — return type name as a static literal string value */
static inline XrValue xrt_typeof_str(XrValue v) {
    XRT_STR_LIT_DEF(xs_int, "int");
    XRT_STR_LIT_DEF(xs_float, "float");
    XRT_STR_LIT_DEF(xs_bool, "bool");
    XRT_STR_LIT_DEF(xs_char, "char");
    XRT_STR_LIT_DEF(xs_null, "null");
    XRT_STR_LIT_DEF(xs_string, "string");
    XRT_STR_LIT_DEF(xs_array, "Array");
    XRT_STR_LIT_DEF(xs_set, "Set");
    XRT_STR_LIT_DEF(xs_map, "Map");
    XRT_STR_LIT_DEF(xs_function, "function");
    XRT_STR_LIT_DEF(xs_strbuf, "StringBuilder");
    XRT_STR_LIT_DEF(xs_tuple, "tuple");
    XRT_STR_LIT_DEF(xs_range, "Range");
    XRT_STR_LIT_DEF(xs_json, "Json");
    XRT_STR_LIT_DEF(xs_record, "Record");
    XRT_STR_LIT_DEF(xs_object, "object");
    switch (xrt_value_kind(v)) {
        case XR_TAG_I64:
            return xr_str_lit(&xs_int);
        case XR_TAG_F64:
            return xr_str_lit(&xs_float);
        case XR_TAG_BOOL:
            return xr_str_lit(&xs_bool);
        case XR_TAG_CHAR:
            return xr_str_lit(&xs_char);
        case XR_TAG_NULL:
            return xr_str_lit(&xs_null);
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            return xr_str_lit(&xs_string);
        case XR_TAG_ARRAY: {
            const xrt_array_t *arr = (const xrt_array_t *) v.ptr;
            if (arr && arr->adt_enum_name)
                return xr_box_str(arr->adt_enum_name);
            return xr_str_lit(&xs_array);
        }
        case XR_TAG_SET:
            return xr_str_lit(&xs_set);
        case XR_TAG_MAP:
            return xr_str_lit(&xs_map);
        case XR_TAG_CLOSURE:
            return xr_str_lit(&xs_function);
        case XR_TAG_STRBUF:
            return xr_str_lit(&xs_strbuf);
        case XR_TAG_TUPLE:
            return xr_str_lit(&xs_tuple);
        case XR_TAG_RANGE:
            return xr_str_lit(&xs_range);
        case XR_TAG_ENUM: {
            const XrAotEnumValueView *ev = xrt_enum_value_view(v);
            return ev && ev->enum_name ? xr_box_str(ev->enum_name) : xr_str_lit(&xs_object);
        }
        case XR_TAG_PTR:
            if (v.ptr && v.heap_type == 0) {
                const xrt_json_t *obj = (const xrt_json_t *) v.ptr;
                return obj->object_kind == XRT_OBJECT_RECORD ? xr_str_lit(&xs_record)
                                                             : xr_str_lit(&xs_json);
            }
            return xr_str_lit(&xs_object);
        default:
            return xr_str_lit(&xs_object);
    }
}

#endif  // XRT_ARITH_H
