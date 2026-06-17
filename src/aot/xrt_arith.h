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
#include "xrt_arc.h"        // xrt_str_concat used by xrt_add
#include "xrt_exception.h"  // xrt_throw_exc for div/mod by zero
#include "xrt_range.h"
#include "xrt_coll.h"

/* =========================================================================
 * Tagged arithmetic — all inline, no extern dependency
 * ========================================================================= */

/* int64 add/sub/mul with two's-complement wrap on overflow.
 * Signed overflow is UB in C, so compute on uint64_t and cast back. This is
 * the single source of truth for AOT integer arithmetic wrap and MUST match
 * the VM (uint64 wrap in xvm_dispatch_arith) and xi_opt constant folding so
 * INT64_MAX + 1, INT64_MIN - 1, etc. produce identical results across tiers. */
static inline int64_t xrt_i64_add(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a + (uint64_t) b);
}
static inline int64_t xrt_i64_sub(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a - (uint64_t) b);
}
static inline int64_t xrt_i64_mul(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a * (uint64_t) b);
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
 *   b == 0          → throw (matches VM E0301 / E0302)
 *   INT64_MIN / -1  → INT64_MIN (unsigned negate; matches xi_opt fold)
 *   INT64_MIN % -1  → 0 */
static inline int64_t xrt_int_div(int64_t a, int64_t b) {
    if (XR_UNLIKELY(b == 0))
        xrt_throw_exc(xr_box_str("E0301: division by zero"));
    if (XR_UNLIKELY(b == -1))
        return (int64_t) (-(uint64_t) a);
    return a / b;
}
static inline int64_t xrt_int_mod(int64_t a, int64_t b) {
    if (XR_UNLIKELY(b == 0))
        xrt_throw_exc(xr_box_str("E0302: modulo by zero"));
    if (XR_UNLIKELY(b == -1))
        return 0;
    return a % b;
}

/* Shifts: the language defines the count as taken mod 64 (spec: "shift count
 * is taken modulo 64 — unlike C, xray shifts are always defined"). Matches
 * xi_opt constant folding and AOT hardware behavior (x64 SHL/SAR with CL,
 * ARM64 LSL/ASR, RISC-V SLL/SRA all mask to 6 bits). Left shift goes through
 * uint64_t because shifting into/past the sign bit is UB on signed in C. */
static inline int64_t xrt_i64_shl(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a << ((uint64_t) b & 63));
}
static inline int64_t xrt_i64_shr(int64_t a, int64_t b) {
    return a >> ((uint64_t) b & 63);
}

static inline XrValue xrt_div(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(xrt_int_div(a.i, b.i));
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    if (XR_UNLIKELY(fb == 0.0))
        xrt_throw_exc(xr_box_str("E0301: division by zero"));
    return XR_FROM_FLOAT(fa / fb);
}

static inline XrValue xrt_mod(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(xrt_int_mod(a.i, b.i));
    xrt_throw_exc(xr_box_str("E0010: modulo requires integer types"));
}

static inline XrValue xrt_neg(XrValue a) {
    if (a.tag == XR_TAG_I64)
        return XR_FROM_INT((int64_t) (-(uint64_t) a.i));
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

static void xrt_print_value(XrValue v, int depth) {
    switch (v.tag) {
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            /* Nested strings are quoted, matching the VM formatter. */
            if (depth > 0)
                putchar('"');
            fwrite(xr_str_data(v), 1, (size_t) xr_str_len(v), stdout);
            if (depth > 0)
                putchar('"');
            return;
        case XR_TAG_I64:
            printf("%lld", (long long) v.i);
            return;
        case XR_TAG_F64: {
            char buf[64];
            xrt_format_float(buf, sizeof(buf), v.f);
            fputs(buf, stdout);
            return;
        }
        case XR_TAG_BOOL:
            printf("%s", v.i ? "true" : "false");
            return;
        case XR_TAG_NULL:
            printf("null");
            return;
        case XR_TAG_ENUM: {
            char buf[256];
            fputs(xr_to_cstr(v, buf, sizeof(buf)), stdout);
            return;
        }
        case XR_TAG_RANGE: {
            char buf[96];
            xrt_range_format_buf((const xrt_range_t *) v.ptr, buf, sizeof(buf));
            fputs(buf, stdout);
            return;
        }
        default:
            break;
    }

    if (depth > XRT_FORMAT_MAX_DEPTH) {
        fputs("...", stdout);
        return;
    }

    switch (v.tag) {
        case XR_TAG_ARRAY: {
            xrt_array_t *a = (xrt_array_t *) v.ptr;
            if (a && a->adt_enum_name && a->adt_member_name) {
                printf("%s.%s", a->adt_enum_name, a->adt_member_name);
                if (a->len > 1) {
                    printf("(");
                    for (int64_t i = 1; i < a->len; i++) {
                        if (i > 1)
                            printf(", ");
                        xrt_print_value(xr_typed_get(a->data, (int32_t) i, a->elem_type),
                                        depth + 1);
                    }
                    printf(")");
                }
                return;
            }
            int64_t len = a ? a->len : 0;
            int64_t limit = len > XRT_FORMAT_MAX_ELEMENTS ? XRT_FORMAT_MAX_ELEMENTS : len;
            putchar('[');
            for (int64_t i = 0; i < limit; i++) {
                if (i > 0)
                    fputs(", ", stdout);
                xrt_print_value(xr_typed_get(a->data, (int32_t) i, a->elem_type), depth + 1);
            }
            if (len > limit)
                printf(", ...(%lld more)", (long long) (len - limit));
            putchar(']');
            return;
        }
        case XR_TAG_TUPLE: {
            xrt_tuple_t *t = (xrt_tuple_t *) v.ptr;
            int64_t len = t ? t->len : 0;
            int64_t limit = len > XRT_FORMAT_MAX_ELEMENTS ? XRT_FORMAT_MAX_ELEMENTS : len;
            putchar('(');
            for (int64_t i = 0; i < limit; i++) {
                if (i > 0)
                    fputs(", ", stdout);
                xrt_print_value(t->items[i], depth + 1);
            }
            if (len > limit)
                printf(", ...(%lld more)", (long long) (len - limit));
            if (len == 1)
                putchar(',');
            putchar(')');
            return;
        }
        default:
            printf("<object@%p>", v.ptr);
            return;
    }
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
    switch (v.tag) {
        case XR_TAG_I64:
            return 8; /* XR_TID_INT */
        case XR_TAG_F64:
            return 11; /* XR_TID_FLOAT */
        case XR_TAG_BOOL:
            return 1; /* XR_TID_BOOL */
        case XR_TAG_NULL:
            return 0; /* XR_TID_NULL */
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            return 12; /* XR_TID_STRING */
        case XR_TAG_ARRAY:
            return 14; /* XR_TID_ARRAY */
        case XR_TAG_SET:
            return 15; /* XR_TID_SET */
        case XR_TAG_MAP:
            return 16; /* XR_TID_MAP */
        case XR_TAG_CLOSURE:
            return 13; /* XR_TID_FUNCTION */
        case XR_TAG_STRBUF:
            return 20; /* XR_TID_STRINGBUILDER */
        case XR_TAG_RANGE:
            return 31; /* XR_TID_RANGE */
        default:
            return 17; /* XR_TID_INSTANCE */
    }
}

/* typename(x) — return type name as a static literal string value */
static inline XrValue xrt_typeof_str(XrValue v) {
    XRT_STR_LIT_DEF(xs_int, "int");
    XRT_STR_LIT_DEF(xs_float, "float");
    XRT_STR_LIT_DEF(xs_bool, "bool");
    XRT_STR_LIT_DEF(xs_null, "null");
    XRT_STR_LIT_DEF(xs_string, "string");
    XRT_STR_LIT_DEF(xs_array, "Array");
    XRT_STR_LIT_DEF(xs_set, "Set");
    XRT_STR_LIT_DEF(xs_map, "Map");
    XRT_STR_LIT_DEF(xs_function, "function");
    XRT_STR_LIT_DEF(xs_strbuf, "StringBuilder");
    XRT_STR_LIT_DEF(xs_tuple, "tuple");
    XRT_STR_LIT_DEF(xs_range, "Range");
    XRT_STR_LIT_DEF(xs_object, "object");
    switch (v.tag) {
        case XR_TAG_I64:
            return xr_str_lit(&xs_int);
        case XR_TAG_F64:
            return xr_str_lit(&xs_float);
        case XR_TAG_BOOL:
            return xr_str_lit(&xs_bool);
        case XR_TAG_NULL:
            return xr_str_lit(&xs_null);
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            return xr_str_lit(&xs_string);
        case XR_TAG_ARRAY:
            return xr_str_lit(&xs_array);
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
        default:
            return xr_str_lit(&xs_object);
    }
}

#endif  // XRT_ARITH_H
