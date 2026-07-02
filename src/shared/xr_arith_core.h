/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_arith_core.h - Runtime-neutral wrapping / overflow-checked integer math.
 *
 * KEY CONCEPT:
 *   Single semantic source for the mem.* fixed-width arithmetic intrinsics.
 *   Both the VM binding (stdlib/mem/mem.c) and the AOT freestanding wrapper
 *   (src/aot/xrt_mem.h) call these, so VM and AOT produce identical results
 *   by construction. All operate on the 64-bit signed `int` domain.
 *
 *   Wrapping ops compute in the unsigned domain and reinterpret, giving
 *   two's-complement wraparound with no undefined behavior. Overflow
 *   predicates report whether the *signed* operation would step outside
 *   [INT64_MIN, INT64_MAX].
 *
 *   Self-contained: depends only on <stdint.h>, so it stays includable from
 *   the freestanding AOT runtime.
 */

#ifndef XRAY_SHARED_XR_ARITH_CORE_H
#define XRAY_SHARED_XR_ARITH_CORE_H

#include <stdint.h>

/* Two's-complement wrapping addition (wraps modulo 2^64). */
static inline int64_t xr_arith_core_add_wrapping(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a + (uint64_t) b);
}

/* Two's-complement wrapping subtraction (wraps modulo 2^64). */
static inline int64_t xr_arith_core_sub_wrapping(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a - (uint64_t) b);
}

/* Two's-complement wrapping multiplication (wraps modulo 2^64). */
static inline int64_t xr_arith_core_mul_wrapping(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a * (uint64_t) b);
}

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
