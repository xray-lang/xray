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
#include "../shared/xr_type_names_core.h"
#include "../shared/xr_value_format_core.h"

/* =========================================================================
 * Tagged arithmetic — all inline, no extern dependency
 * ========================================================================= */

/* int64 add/sub/mul with two's-complement wrap on overflow.
 * Signed overflow is UB in C, so these AOT adapters forward to shared numeric
 * core helpers used by VM dispatch and xi_opt constant folding. */
static inline int64_t xrt_i64_add(int64_t a, int64_t b) {
    return xr_numeric_core_i64_add_wrap(a, b);
}
static inline int64_t xrt_i64_sub(int64_t a, int64_t b) {
    return xr_numeric_core_i64_sub_wrap(a, b);
}
static inline int64_t xrt_i64_mul(int64_t a, int64_t b) {
    return xr_numeric_core_i64_mul_wrap(a, b);
}

static inline XrValue xrt_add(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(xrt_i64_add(a.i, b.i));
    if (XR_IS_STR(a) && XR_IS_STR(b))
        return xrt_str_concat_value(a, b); /* header lengths, no strlen */
    if (XR_IS_STR(a) || XR_IS_STR(b)) {
        if (a.tag == XR_TAG_RANGE)
            return xrt_str_concat_value(xrt_range_to_string(a), b);
        if (b.tag == XR_TAG_RANGE)
            return xrt_str_concat_value(a, xrt_range_to_string(b));
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
 * Typed scalar codegen in xi_cgen and tagged xrt_div / xrt_mod both pass
 * through this AOT exception adapter, then into shared numeric core.
 *   b == 0          → throw (matches VM XR_ERR_DIV_BY_ZERO / XR_ERR_MOD_BY_ZERO)
 *   INT64_MIN / -1  → INT64_MIN (unsigned negate; matches xi_opt fold)
 *   INT64_MIN % -1  → 0 */
static inline int64_t xrt_int_div(int64_t a, int64_t b) {
    if (XR_UNLIKELY(b == 0))
        xrt_throw_error(XR_ERR_DIV_BY_ZERO, XR_ERROR_CORE_DIVISION_BY_ZERO_MSG);
    return xr_numeric_core_i64_div_wrap(a, b);
}
static inline int64_t xrt_int_mod(int64_t a, int64_t b) {
    if (XR_UNLIKELY(b == 0))
        xrt_throw_error(XR_ERR_MOD_BY_ZERO, XR_ERROR_CORE_MODULO_BY_ZERO_MSG);
    return xr_numeric_core_i64_mod_wrap(a, b);
}

/* Shifts: the language defines the count as taken mod 64 (spec: "shift count
 * is taken modulo 64 — unlike C, xray shifts are always defined"). Matches
 * xi_opt constant folding and AOT hardware behavior (x64 SHL/SAR with CL,
 * ARM64 LSL/ASR, RISC-V SLL/SRA all mask to 6 bits). Left shift goes through
 * uint64_t because shifting into/past the sign bit is UB on signed in C. */
static inline int64_t xrt_i64_shl(int64_t a, int64_t b) {
    return xr_numeric_core_i64_shl_wrap(a, b);
}
static inline int64_t xrt_i64_shr(int64_t a, int64_t b) {
    return xr_numeric_core_i64_shr_wrap(a, b);
}

static inline XrValue xrt_div(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(xrt_int_div(a.i, b.i));
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    if (XR_UNLIKELY(fb == 0.0))
        xrt_throw_error(XR_ERR_DIV_BY_ZERO, XR_ERROR_CORE_DIVISION_BY_ZERO_MSG);
    return XR_FROM_FLOAT(fa / fb);
}

static inline XrValue xrt_mod(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(xrt_int_mod(a.i, b.i));
    xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_MODULO_REQUIRES_INTEGER_MSG);
}

static inline XrValue xrt_neg(XrValue a) {
    if (a.tag == XR_TAG_I64)
        return XR_FROM_INT(xr_numeric_core_i64_neg_wrap(a.i));
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
        case XR_TAG_I64: {
            char buf[32];
            (void) xr_numeric_core_format_i64(buf, sizeof(buf), v.i);
            fputs(buf, stdout);
        }
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

    if (depth > XR_VALUE_FORMAT_MAX_DEPTH) {
        fputs("...", stdout);
        return;
    }

    switch (xrt_value_kind(v)) {
        case XR_TAG_ARRAY: {
            xrt_array_t *a = (xrt_array_t *) v.ptr;
            if (a && a->adt_enum_name && a->adt_member_name) {
                printf("%s.%s", a->adt_enum_name, a->adt_member_name);
                if (a->length > 1) {
                    printf("(");
                    for (int64_t i = 1; i < a->length; i++) {
                        if (i > 1)
                            printf(", ");
                        xrt_print_value(xr_typed_get(a->data, (int32_t) i, a->elem_type),
                                        depth + 1);
                    }
                    printf(")");
                }
                return;
            }
            int64_t len = a ? a->length : 0;
            int64_t limit = len > XR_VALUE_FORMAT_MAX_ELEMENTS ? XR_VALUE_FORMAT_MAX_ELEMENTS : len;
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
            int64_t limit = len > XR_VALUE_FORMAT_MAX_ELEMENTS ? XR_VALUE_FORMAT_MAX_ELEMENTS : len;
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
        case XR_TAG_MAP: {
            xrt_map_t *m = (xrt_map_t *) v.ptr;
            int64_t total = m ? xrt_map_len(m) : 0;
            int64_t n_slots = !m ? 0 : (xrt_map_is_typed(m) ? m->order_len : (int64_t) m->nentries);
            int64_t count = 0;
            fputs("#{", stdout);
            for (int64_t i = 0; i < n_slots && count < XR_VALUE_FORMAT_MAX_ELEMENTS; i++) {
                int64_t slot = xrt_map_is_typed(m) ? m->order[i] : i;
                if (!xrt_map_slot_is_full(m, slot))
                    continue;
                if (count > 0)
                    fputs(", ", stdout);
                xrt_print_value(xrt_map_slot_key(m, slot), depth + 1);
                fputs(": ", stdout);
                xrt_print_value(xrt_map_slot_value(m, slot), depth + 1);
                count++;
            }
            if (total > count)
                printf(", ...(%lld more)", (long long) (total - count));
            putchar('}');
            return;
        }
        case XR_TAG_SET: {
            xrt_set_t *s = (xrt_set_t *) v.ptr;
            int64_t total = s ? xrt_set_len(s) : 0;
            int64_t n_slots = !s ? 0 : (xrt_set_is_typed(s) ? s->order_len : (int64_t) s->nentries);
            int64_t count = 0;
            fputs("#[", stdout);
            for (int64_t i = 0; i < n_slots && count < XR_VALUE_FORMAT_MAX_ELEMENTS; i++) {
                int64_t slot = xrt_set_is_typed(s) ? s->order[i] : i;
                if (!xrt_set_slot_is_full(s, slot))
                    continue;
                if (count > 0)
                    fputs(", ", stdout);
                xrt_print_value(xrt_set_slot_item(s, slot), depth + 1);
                count++;
            }
            if (total > count)
                printf(", ...(%lld more)", (long long) (total - count));
            putchar(']');
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

/* typeof(x) returns the shared public XrTypeId used by VM Type.xxx and AOT. */
static inline int64_t xrt_typeof_id(XrValue v) {
    switch (xrt_value_kind(v)) {
        case XR_TAG_I64:
            return XR_TID_INT;
        case XR_TAG_F64:
            return XR_TID_FLOAT;
        case XR_TAG_BOOL:
            return XR_TID_BOOL;
        case XR_TAG_NULL:
            return XR_TID_NULL;
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            return XR_TID_STRING;
        case XR_TAG_ARRAY:
            return XR_TID_ARRAY;
        case XR_TAG_SET:
            return XR_TID_SET;
        case XR_TAG_MAP:
            return XR_TID_MAP;
        case XR_TAG_CLOSURE:
            return XR_TID_FUNCTION;
        case XR_TAG_STRBUF:
            return XR_TID_STRINGBUILDER;
        case XR_TAG_RANGE:
            return XR_TID_RANGE;
        default:
            return XR_TID_INSTANCE;
    }
}

/* typename(x) — return type name as a static literal string value */
static inline XrValue xrt_typeof_str(XrValue v) {
    XRT_STR_LIT_DEF(xs_int, TYPE_NAME_INT);
    XRT_STR_LIT_DEF(xs_float, TYPE_NAME_FLOAT);
    XRT_STR_LIT_DEF(xs_bool, TYPE_NAME_BOOL);
    XRT_STR_LIT_DEF(xs_null, TYPE_NAME_NULL);
    XRT_STR_LIT_DEF(xs_string, TYPE_NAME_STRING);
    XRT_STR_LIT_DEF(xs_array, TYPE_NAME_ARRAY);
    XRT_STR_LIT_DEF(xs_set, TYPE_NAME_SET);
    XRT_STR_LIT_DEF(xs_map, TYPE_NAME_MAP);
    XRT_STR_LIT_DEF(xs_function, TYPE_NAME_FUNCTION);
    XRT_STR_LIT_DEF(xs_strbuf, TYPE_NAME_STRINGBUILDER);
    XRT_STR_LIT_DEF(xs_tuple, "tuple");
    XRT_STR_LIT_DEF(xs_range, TYPE_NAME_RANGE);
    XRT_STR_LIT_DEF(xs_object, TYPE_NAME_OBJECT);
    switch (xrt_value_kind(v)) {
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
