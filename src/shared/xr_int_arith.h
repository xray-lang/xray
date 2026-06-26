/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_int_arith.h - Defined int64 arithmetic helpers shared by VM/AOT/native methods.
 */

#ifndef XR_INT_ARITH_H
#define XR_INT_ARITH_H

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

#ifndef XR_HAS_BUILTIN
#if defined(__has_builtin)
#define XR_HAS_BUILTIN(x) __has_builtin(x)
#else
#define XR_HAS_BUILTIN(x) 0
#endif
#endif

static inline int64_t xr_i64_add_wrap(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a + (uint64_t) b);
}

static inline int64_t xr_i64_sub_wrap(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a - (uint64_t) b);
}

static inline int64_t xr_i64_mul_wrap(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a * (uint64_t) b);
}

static inline int64_t xr_i64_neg_wrap(int64_t v) {
    return (int64_t) (-(uint64_t) v);
}

static inline int64_t xr_i64_abs_wrap(int64_t v) {
    return v >= 0 ? v : xr_i64_neg_wrap(v);
}

static inline uint64_t xr_i64_abs_magnitude(int64_t v) {
    return v >= 0 ? (uint64_t) v : (uint64_t) 0 - (uint64_t) v;
}

static inline int64_t xr_i64_div_wrap(int64_t a, int64_t b) {
    if (b == -1)
        return xr_i64_neg_wrap(a);
    return a / b;
}

static inline int64_t xr_i64_mod_wrap(int64_t a, int64_t b) {
    if (b == -1)
        return 0;
    return a % b;
}

static inline int64_t xr_i64_shl_wrap(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a << ((uint64_t) b & 63));
}

static inline int64_t xr_i64_shr_wrap(int64_t a, int64_t b) {
    return a >> ((uint64_t) b & 63);
}

static inline bool xr_i64_checked_add(int64_t a, int64_t b, int64_t *out) {
#if XR_HAS_BUILTIN(__builtin_add_overflow) || defined(__GNUC__) || defined(__clang__)
    return !__builtin_add_overflow(a, b, out);
#else
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        return false;
    *out = a + b;
    return true;
#endif
}

static inline bool xr_i64_checked_sub(int64_t a, int64_t b, int64_t *out) {
#if XR_HAS_BUILTIN(__builtin_sub_overflow) || defined(__GNUC__) || defined(__clang__)
    return !__builtin_sub_overflow(a, b, out);
#else
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))
        return false;
    *out = a - b;
    return true;
#endif
}

static inline bool xr_i64_checked_mul(int64_t a, int64_t b, int64_t *out) {
#if XR_HAS_BUILTIN(__builtin_mul_overflow) || defined(__GNUC__) || defined(__clang__)
    return !__builtin_mul_overflow(a, b, out);
#else
    if (a == 0 || b == 0) {
        *out = 0;
        return true;
    }
    if (a == -1) {
        if (b == INT64_MIN)
            return false;
        *out = -b;
        return true;
    }
    if (b == -1) {
        if (a == INT64_MIN)
            return false;
        *out = -a;
        return true;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b)
                return false;
        } else if (b < INT64_MIN / a) {
            return false;
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b)
                return false;
        } else if (a != 0 && b < INT64_MAX / a) {
            return false;
        }
    }
    *out = a * b;
    return true;
#endif
}

static inline int64_t xr_i64_saturating_add(int64_t a, int64_t b) {
    int64_t out;
    if (xr_i64_checked_add(a, b, &out))
        return out;
    return b >= 0 ? INT64_MAX : INT64_MIN;
}

static inline int64_t xr_i64_saturating_sub(int64_t a, int64_t b) {
    int64_t out;
    if (xr_i64_checked_sub(a, b, &out))
        return out;
    return b < 0 ? INT64_MAX : INT64_MIN;
}

static inline int64_t xr_i64_saturating_mul(int64_t a, int64_t b) {
    int64_t out;
    if (xr_i64_checked_mul(a, b, &out))
        return out;
    return ((a < 0) ^ (b < 0)) ? INT64_MIN : INT64_MAX;
}

#endif /* XR_INT_ARITH_H */
