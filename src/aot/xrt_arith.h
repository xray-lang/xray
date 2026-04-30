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

/* =========================================================================
 * Tagged arithmetic — all inline, no extern dependency
 * ========================================================================= */

static inline XrValue xrt_add(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(a.i + b.i);
    if (XR_IS_STR(a) || XR_IS_STR(b)) {
        char ba[64], bb[64];
        return xrt_str_concat(xr_to_cstr(a, ba, sizeof(ba)), xr_to_cstr(b, bb, sizeof(bb)));
    }
    double fa = a.tag == XR_TAG_I64 ? (double) a.i : a.f;
    double fb = b.tag == XR_TAG_I64 ? (double) b.i : b.f;
    return XR_FROM_FLOAT(fa + fb);
}

static inline int64_t xrt_eq(XrValue a, XrValue b) {
    // Normalize STR_ARC to STR for comparison
    uint32_t ta = (a.tag == XR_TAG_STR_ARC) ? XR_TAG_STR : a.tag;
    uint32_t tb = (b.tag == XR_TAG_STR_ARC) ? XR_TAG_STR : b.tag;
    if (ta != tb)
        return 0;
    if (ta == XR_TAG_I64)
        return a.i == b.i;
    if (ta == XR_TAG_F64)
        return a.f == b.f;
    if (ta == XR_TAG_STR)
        return strcmp((const char *) a.ptr, (const char *) b.ptr) == 0;
    return a.ptr == b.ptr;
}

static inline XrValue xrt_sub(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(a.i - b.i);
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return XR_FROM_FLOAT(fa - fb);
}

static inline XrValue xrt_mul(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return XR_FROM_INT(a.i * b.i);
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return XR_FROM_FLOAT(fa * fb);
}

static inline XrValue xrt_div(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return b.i ? XR_FROM_INT(a.i / b.i) : XR_FROM_INT(0);
    double fa = (a.tag == XR_TAG_I64) ? (double) a.i : a.f;
    double fb = (b.tag == XR_TAG_I64) ? (double) b.i : b.f;
    return XR_FROM_FLOAT(fa / fb);
}

static inline XrValue xrt_mod(XrValue a, XrValue b) {
    if (a.tag == XR_TAG_I64 && b.tag == XR_TAG_I64)
        return b.i ? XR_FROM_INT(a.i % b.i) : XR_FROM_INT(0);
    return XR_FROM_INT(0);
}

static inline XrValue xrt_neg(XrValue a) {
    if (a.tag == XR_TAG_I64)
        return XR_FROM_INT(-a.i);
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

static inline void xrt_print(XrValue v) {
    switch (v.tag) {
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
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

static inline void xrt_println(XrValue v) {
    xrt_print(v);
    printf("\n");
}

#endif  // XRT_ARITH_H
