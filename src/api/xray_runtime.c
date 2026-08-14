/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_runtime.c - Self-contained embedding runtime for AOT-compiled code
 *
 * KEY CONCEPT:
 *   Implements xrt_* functions declared in runtime.h.
 *   All functions are self-contained with no dependency on VM internals.
 *   Pure-compute AOT code doesn't link this file at all.
 */

#include "../../include/runtime.h"
#include "../base/xchecks.h"
#include "../shared/xr_int_arith_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "../base/xmalloc.h"

/* ========== Value → String Conversion ========== */

static const char *xrt_to_cstr(XrValue v, char *buf, size_t bufsz) {
    switch (v.tag) {
        case XR_TAG_STR:
            return (const char *) v.ptr;
        case XR_TAG_I64:
            snprintf(buf, bufsz, "%" PRId64, v.i);
            return buf;
        case XR_TAG_F64:
            snprintf(buf, bufsz, "%g", v.f);
            return buf;
        case XR_TAG_BOOL:
            return v.i ? "true" : "false";
        case XR_TAG_NULL:
            return "null";
        default:
            snprintf(buf, bufsz, "<object@%p>", v.ptr);
            return buf;
    }
}

static XrValue xrt_str_concat(const char *sa, const char *sb) {
    size_t la = strlen(sa), lb = strlen(sb);
    char *r = (char *) xr_malloc(la + lb + 1);
    if (!r)
        return XR_NULL_VAL;
    memcpy(r, sa, la);
    memcpy(r + la, sb, lb + 1);
    return xrt_box_str(r);
}

/* ========== Mixed-type Arithmetic ========== */

/* This library is linked into an embedder's process, where there is no Xray
 * frame to unwind and therefore no error channel: where the AOT value profile
 * in src/aot/xrt_arith.h raises E0404 / E0420 / E0421 through xrt_throw_exc,
 * the closest behaviour available here is the one the rest of this file
 * already uses for an operation it cannot honour -- report and abort, rather
 * than return a value computed from bits that do not mean what the arithmetic
 * would read them as. */
static void xrt_arith_fault(const char *diagnostic) {
    fprintf(stderr, "%s\n", diagnostic);
    abort();
}

/* Only I64 and F64 carry a number. Every other tag -- including a heap
 * pointer and a string -- has a payload whose bits are not a double, so it
 * must never reach the float lane. */
static bool xrt_is_tagged_number(XrValue v) {
    return v.tag == XR_TAG_I64 || v.tag == XR_TAG_F64;
}

static double xrt_tagged_number(XrValue v) {
    return v.tag == XR_TAG_I64 ? (double) v.i : v.f;
}

static const char *xrt_str_payload(XrValue v) {
    return v.ptr ? (const char *) v.ptr : "";
}

/* Integer wrap, division and modulo come from the shared owners in
 * src/shared/xr_int_arith_core.h, the same kernels the VM and the AOT value
 * profiles consume, so every tier agrees on INT64_MAX + 1, INT64_MIN / -1 and
 * a zero divisor. This value model has no BigInt tag, so an operand that the
 * language would promote arrives as a heap pointer and is rejected by the
 * numeric guard instead of being read as a double. */
#define XRT_API_INT_DIV_MOD(kind, lhs, rhs)                                                        \
    XR_INT_DIV_MOD_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_HI,                              \
                               XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_LO, XR_SEM_CONSUMER_AOT_HOSTED,  \
                               (kind), XR_INT_DIV_MOD_PROOF_NONE, (lhs), (rhs))

XrValue xrt_add(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return xrt_box_int(xr_i64_add_wrap(a.i, b.i));
    /* `+` joins two strings; a string paired with anything else has no result. */
    if (a.tag == XR_TAG_STR && b.tag == XR_TAG_STR)
        return xrt_str_concat(xrt_str_payload(a), xrt_str_payload(b));
    if (!xrt_is_tagged_number(a) || !xrt_is_tagged_number(b))
        xrt_arith_fault(
            "E0404: operator '+' requires both operands to be numeric or both string");
    return xrt_box_float(xrt_tagged_number(a) + xrt_tagged_number(b));
}

XrValue xrt_sub(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return xrt_box_int(xr_i64_sub_wrap(a.i, b.i));
    if (!xrt_is_tagged_number(a) || !xrt_is_tagged_number(b))
        xrt_arith_fault("E0404: subtraction requires numeric types");
    return xrt_box_float(xrt_tagged_number(a) - xrt_tagged_number(b));
}

XrValue xrt_mul(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return xrt_box_int(xr_i64_mul_wrap(a.i, b.i));
    if (!xrt_is_tagged_number(a) || !xrt_is_tagged_number(b))
        xrt_arith_fault("E0404: multiplication requires numeric types");
    return xrt_box_float(xrt_tagged_number(a) * xrt_tagged_number(b));
}

XrValue xrt_div(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64) {
        XrIntDivModResult quotient = XRT_API_INT_DIV_MOD(XR_INT_DIV_MOD_DIV, a.i, b.i);
        if (quotient.divisor_is_zero)
            xrt_arith_fault("E0420: division by zero");
        return xrt_box_int(quotient.value);
    }
    if (!xrt_is_tagged_number(a) || !xrt_is_tagged_number(b))
        xrt_arith_fault("E0404: division requires numeric types");
    return xrt_box_float(xrt_tagged_number(a) / xrt_tagged_number(b));
}

XrValue xrt_mod(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64) {
        XrIntDivModResult remainder = XRT_API_INT_DIV_MOD(XR_INT_DIV_MOD_MOD, a.i, b.i);
        if (remainder.divisor_is_zero)
            xrt_arith_fault("E0421: modulo by zero");
        return xrt_box_int(remainder.value);
    }
    xrt_arith_fault("E0404: modulo requires integer types");
    return XR_NULL_VAL;
}

XrValue xrt_neg(XrValue a) {
    if (a.tag == XR_TAG_I64)
        return xrt_box_int(xr_i64_neg_wrap(a.i));
    if (a.tag == XR_TAG_F64)
        return xrt_box_float(-a.f);
    xrt_arith_fault("E0404: operand must be numeric");
    return XR_NULL_VAL;
}

/* ========== Mixed-type Comparison ========== */

int64_t xrt_lt(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return a.i < b.i;
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return fa < fb;
}

int64_t xrt_le(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return a.i <= b.i;
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return fa <= fb;
}

int64_t xrt_eq(XrValue a, XrValue b) {
    if (a.tag == b.tag) {
        if (a.tag == XR_TAG_I64 || a.tag == XR_TAG_BOOL)
            return a.i == b.i;
        if (a.tag == XR_TAG_F64)
            return a.f == b.f;
        if (a.tag == XR_TAG_NULL)
            return 1;
        return a.ptr == b.ptr;
    }
    if ((a.tag == XR_TAG_I64 || a.tag == XR_TAG_F64) &&
        (b.tag == XR_TAG_I64 || b.tag == XR_TAG_F64)) {
        double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
        double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
        return fa == fb;
    }
    return 0;
}

/* ========== Print ========== */

void xrt_print(XrValue v) {
    switch (v.tag) {
        case XR_TAG_STR:
            printf("%s", (const char *) v.ptr);
            break;
        case XR_TAG_I64:
            printf("%lld", (long long) v.i);
            break;
        case XR_TAG_F64:
            printf("%g", v.f);
            break;
        case XR_TAG_BOOL:
            printf("%s", v.i ? "true" : "false");
            break;
        case XR_TAG_NULL:
            printf("null");
            break;
        default:
            printf("<object@%p>", v.ptr);
            break;
    }
}

void xrt_println(XrValue v) {
    xrt_print(v);
    printf("\n");
}

/* ========== String Operations ========== */

XrValue xrt_string_concat(XrValue a, XrValue b) {
    char ba[64], bb[64];
    return xrt_str_concat(xrt_to_cstr(a, ba, sizeof(ba)), xrt_to_cstr(b, bb, sizeof(bb)));
}

XrValue xrt_string_len(XrValue s) {
    XR_DCHECK(s.tag == XR_TAG_STR, "xrt_string_len: expected string");
    if (s.tag != XR_TAG_STR || !s.ptr)
        return xrt_box_int(0);
    return xrt_box_int((int64_t) strlen((const char *) s.ptr));
}

XrValue xrt_string_slice(XrValue s, int64_t start, int64_t end) {
    XR_DCHECK(s.tag == XR_TAG_STR, "xrt_string_slice: expected string");
    if (s.tag != XR_TAG_STR || !s.ptr)
        return XR_NULL_VAL;
    const char *str = (const char *) s.ptr;
    int64_t len = (int64_t) strlen(str);
    if (start < 0)
        start += len;
    if (end < 0)
        end += len;
    if (start < 0)
        start = 0;
    if (end > len)
        end = len;
    if (start >= end)
        return xrt_box_str("");
    int64_t slen = end - start;
    char *r = (char *) xr_malloc((size_t) slen + 1);
    if (!r)
        return XR_NULL_VAL;
    memcpy(r, str + start, (size_t) slen);
    r[slen] = '\0';
    return xrt_box_str(r);
}

int64_t xrt_string_eq(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_STR && b.tag == XR_TAG_STR) {
        if (a.ptr == b.ptr)
            return 1;
        if (!a.ptr || !b.ptr)
            return 0;
        return strcmp((const char *) a.ptr, (const char *) b.ptr) == 0;
    }
    return 0;
}

/* Stub functions: AOT array/map/field operations not yet implemented.
 * These abort with a clear message rather than silently returning wrong results. */

XrValue xrt_array_new(int64_t cap) {
    (void) cap;
    fprintf(stderr, "xrt_array_new: not implemented\n");
    abort();
}
XrValue xrt_array_get(XrValue a, int64_t i) {
    (void) a;
    (void) i;
    fprintf(stderr, "xrt_array_get: not implemented\n");
    abort();
}
void xrt_array_set(XrValue a, int64_t i, XrValue v) {
    (void) a;
    (void) i;
    (void) v;
    fprintf(stderr, "xrt_array_set: not implemented\n");
    abort();
}
int64_t xrt_array_len(XrValue a) {
    (void) a;
    fprintf(stderr, "xrt_array_len: not implemented\n");
    abort();
}
void xrt_array_push(XrValue a, XrValue v) {
    (void) a;
    (void) v;
    fprintf(stderr, "xrt_array_push: not implemented\n");
    abort();
}

XrValue xrt_map_new(void) {
    fprintf(stderr, "xrt_map_new: not implemented\n");
    abort();
}
XrValue xrt_map_get(XrValue m, XrValue k) {
    (void) m;
    (void) k;
    fprintf(stderr, "xrt_map_get: not implemented\n");
    abort();
}
void xrt_map_set(XrValue m, XrValue k, XrValue v) {
    (void) m;
    (void) k;
    (void) v;
    fprintf(stderr, "xrt_map_set: not implemented\n");
    abort();
}
int64_t xrt_map_len(XrValue m) {
    (void) m;
    fprintf(stderr, "xrt_map_len: not implemented\n");
    abort();
}

XrValue xrt_field_get(XrValue o, const char *n) {
    (void) o;
    (void) n;
    fprintf(stderr, "xrt_field_get: not implemented\n");
    abort();
}
void xrt_field_set(XrValue o, const char *n, XrValue v) {
    (void) o;
    (void) n;
    (void) v;
    fprintf(stderr, "xrt_field_set: not implemented\n");
    abort();
}

void xrt_safepoint(void) {
}
void *xrt_alloc(size_t size) {
    (void) size;
    fprintf(stderr, "xrt_alloc: not implemented\n");
    abort();
}
