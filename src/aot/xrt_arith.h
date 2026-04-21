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
#include "xrt_arc.h" // xrt_str_concat used by xrt_add

/* =========================================================================
 * Tagged arithmetic — all inline, no extern dependency
 * ========================================================================= */

static inline XrtValue xrt_add(XrtValue a, XrtValue b) {
    if (a.tag == XRT_TAG_I64 && b.tag == XRT_TAG_I64)
        return xrt_box_int(a.i + b.i);
    if (XRT_IS_STR(a) || XRT_IS_STR(b)) {
        char ba[64], bb[64];
        return xrt_str_concat(xrt_to_cstr(a, ba, sizeof(ba)),
                              xrt_to_cstr(b, bb, sizeof(bb)));
    }
    double fa = a.tag == XRT_TAG_I64 ? (double)a.i : a.f;
    double fb = b.tag == XRT_TAG_I64 ? (double)b.i : b.f;
    return xrt_box_float(fa + fb);
}

static inline int64_t xrt_eq(XrtValue a, XrtValue b) {
    // Normalize STR_ARC to STR for comparison
    uint32_t ta = (a.tag == XRT_TAG_STR_ARC) ? XRT_TAG_STR : a.tag;
    uint32_t tb = (b.tag == XRT_TAG_STR_ARC) ? XRT_TAG_STR : b.tag;
    if (ta != tb) return 0;
    if (ta == XRT_TAG_I64) return a.i == b.i;
    if (ta == XRT_TAG_F64) return a.f == b.f;
    if (ta == XRT_TAG_STR)
        return strcmp((const char *)a.ptr, (const char *)b.ptr) == 0;
    return a.ptr == b.ptr;
}

static inline XrtValue xrt_sub(XrtValue a, XrtValue b) {
    if (a.tag == XRT_TAG_I64 && b.tag == XRT_TAG_I64)
        return xrt_box_int(a.i - b.i);
    double fa = (a.tag == XRT_TAG_I64) ? (double)a.i : a.f;
    double fb = (b.tag == XRT_TAG_I64) ? (double)b.i : b.f;
    return xrt_box_float(fa - fb);
}

static inline XrtValue xrt_mul(XrtValue a, XrtValue b) {
    if (a.tag == XRT_TAG_I64 && b.tag == XRT_TAG_I64)
        return xrt_box_int(a.i * b.i);
    double fa = (a.tag == XRT_TAG_I64) ? (double)a.i : a.f;
    double fb = (b.tag == XRT_TAG_I64) ? (double)b.i : b.f;
    return xrt_box_float(fa * fb);
}

static inline XrtValue xrt_div(XrtValue a, XrtValue b) {
    if (a.tag == XRT_TAG_I64 && b.tag == XRT_TAG_I64)
        return b.i ? xrt_box_int(a.i / b.i) : xrt_box_int(0);
    double fa = (a.tag == XRT_TAG_I64) ? (double)a.i : a.f;
    double fb = (b.tag == XRT_TAG_I64) ? (double)b.i : b.f;
    return xrt_box_float(fa / fb);
}

static inline XrtValue xrt_mod(XrtValue a, XrtValue b) {
    if (a.tag == XRT_TAG_I64 && b.tag == XRT_TAG_I64)
        return b.i ? xrt_box_int(a.i % b.i) : xrt_box_int(0);
    return xrt_box_int(0);
}

static inline XrtValue xrt_neg(XrtValue a) {
    if (a.tag == XRT_TAG_I64) return xrt_box_int(-a.i);
    if (a.tag == XRT_TAG_F64) return xrt_box_float(-a.f);
    return xrt_box_int(0);
}

/* =========================================================================
 * Inline tagged comparisons
 * ========================================================================= */

static inline int64_t xrt_lt(XrtValue a, XrtValue b) {
    if (a.tag == XRT_TAG_I64 && b.tag == XRT_TAG_I64) return a.i < b.i;
    double fa = (a.tag == XRT_TAG_I64) ? (double)a.i : a.f;
    double fb = (b.tag == XRT_TAG_I64) ? (double)b.i : b.f;
    return fa < fb;
}

static inline int64_t xrt_le(XrtValue a, XrtValue b) {
    if (a.tag == XRT_TAG_I64 && b.tag == XRT_TAG_I64) return a.i <= b.i;
    double fa = (a.tag == XRT_TAG_I64) ? (double)a.i : a.f;
    double fb = (b.tag == XRT_TAG_I64) ? (double)b.i : b.f;
    return fa <= fb;
}

/* =========================================================================
 * Inline print
 * ========================================================================= */

static inline void xrt_print(XrtValue v) {
    switch (v.tag) {
        case XRT_TAG_STR:
        case XRT_TAG_STR_ARC: printf("%s", (const char *)v.ptr); break;
        case XRT_TAG_I64:     printf("%lld", (long long)v.i);     break;
        case XRT_TAG_F64:     printf("%g",   v.f);                break;
        case XRT_TAG_BOOL:    printf("%s", v.i ? "true" : "false"); break;
        case XRT_TAG_NULL:    printf("null");                      break;
        default:              printf("<object@%p>", v.ptr);        break;
    }
}

static inline void xrt_println(XrtValue v) { xrt_print(v); printf("\n"); }

#endif // XRT_ARITH_H
