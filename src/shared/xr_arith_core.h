/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_arith_core.h - Runtime-neutral signed-overflow predicates.
 *
 * KEY CONCEPT:
 *   Single semantic source for the int.addOverflows/subOverflows/mulOverflows
 *   methods (task 153). The VM method (xint_methods.h), the AOT dispatch
 *   (xrt_method.h) and the AOT cgen direct lowering all call these, so VM
 *   and AOT produce identical results by construction. All operate on the
 *   64-bit signed `int` domain.
 *
 *   Wrapping arithmetic lives in xr_int_arith.h (xr_i64_*_wrap) — the
 *   wrapping helpers that used to live here were duplicates and were
 *   deleted when mem.addWrapping/... moved to int methods.
 *
 *   Self-contained: depends only on <stdint.h>, so it stays includable from
 *   the freestanding AOT runtime.
 */

#ifndef XRAY_SHARED_XR_ARITH_CORE_H
#define XRAY_SHARED_XR_ARITH_CORE_H

#include <stdint.h>

/* Whether signed 64-bit `a + b` overflows [INT64_MIN, INT64_MAX]. */
static inline int xr_arith_core_add_overflows(int64_t a, int64_t b) {
#if defined(__GNUC__) || defined(__clang__)
    int64_t r;
    return __builtin_add_overflow(a, b, &r) ? 1 : 0;
#else
    int64_t r = (int64_t) ((uint64_t) a + (uint64_t) b);
    /* Overflow iff a and b share a sign that differs from the result. */
    return (((a ^ r) & (b ^ r)) < 0) ? 1 : 0;
#endif
}

/* Whether signed 64-bit `a - b` overflows [INT64_MIN, INT64_MAX]. */
static inline int xr_arith_core_sub_overflows(int64_t a, int64_t b) {
#if defined(__GNUC__) || defined(__clang__)
    int64_t r;
    return __builtin_sub_overflow(a, b, &r) ? 1 : 0;
#else
    int64_t r = (int64_t) ((uint64_t) a - (uint64_t) b);
    /* Overflow iff a and b differ in sign and the result's sign differs from a. */
    return (((a ^ b) & (a ^ r)) < 0) ? 1 : 0;
#endif
}

/* Whether signed 64-bit `a * b` overflows [INT64_MIN, INT64_MAX]. */
static inline int xr_arith_core_mul_overflows(int64_t a, int64_t b) {
#if defined(__GNUC__) || defined(__clang__)
    int64_t r;
    return __builtin_mul_overflow(a, b, &r) ? 1 : 0;
#else
    if (a == 0 || b == 0)
        return 0;
    /* INT64_MIN * -1 (and its commutation) overflows but survives r/a == b. */
    if ((a == INT64_MIN && b == -1) || (b == INT64_MIN && a == -1))
        return 1;
    int64_t r = (int64_t) ((uint64_t) a * (uint64_t) b);
    return (r / a != b) ? 1 : 0;
#endif
}

#endif  // XRAY_SHARED_XR_ARITH_CORE_H
