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

/* High half of a full-width unsigned 64 x 64 product.  This is the semantic
 * truth shared by VM and AOT for uint64.mulHigh().  Native AOT compilers lower
 * the int128 expression to UMULH/MULX (or the equivalent target instruction);
 * the four-part fallback keeps the language portable without a public uint128
 * type or architecture-specific API. */
static inline uint64_t xr_u64_mul_high(uint64_t a, uint64_t b) {
#if defined(__SIZEOF_INT128__)
    return (uint64_t) (((__uint128_t) a * (__uint128_t) b) >> 64);
#else
    uint64_t a_lo = (uint32_t) a;
    uint64_t a_hi = a >> 32;
    uint64_t b_lo = (uint32_t) b;
    uint64_t b_hi = b >> 32;
    uint64_t lo_lo = a_lo * b_lo;
    uint64_t hi_lo = a_hi * b_lo;
    uint64_t lo_hi = a_lo * b_hi;
    uint64_t hi_hi = a_hi * b_hi;
    uint64_t cross = (lo_lo >> 32) + (uint32_t) hi_lo + lo_hi;
    return hi_hi + (hi_lo >> 32) + (cross >> 32);
#endif
}

static inline uint64_t xr_uint_mul_high_bits(uint64_t a, uint64_t b, unsigned bits) {
    switch (bits) {
        case 8:
            return ((uint16_t) (uint8_t) a * (uint16_t) (uint8_t) b) >> 8;
        case 16:
            return ((uint32_t) (uint16_t) a * (uint32_t) (uint16_t) b) >> 16;
        case 32:
            return ((uint64_t) (uint32_t) a * (uint64_t) (uint32_t) b) >> 32;
        case 64:
            return xr_u64_mul_high(a, b);
        default:
            return 0;
    }
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

/* Unsigned division / modulo for statically-unsigned operands (mirrors
 * OP_DIV_U / OP_MOD_U in the VM and the uint64_t-typed division the AOT
 * backend emits). No signed-overflow edge cases: unsigned division only
 * traps on divide-by-zero, which the caller must reject first. Keyed on the
 * static type because the i64 value model carries no signedness tag — the
 * only widths where this differs from the signed path are u64/usize (the
 * narrower unsigned payloads are zero-extended, so both agree). */
static inline int64_t xr_i64_div_u_wrap(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a / (uint64_t) b);
}

static inline int64_t xr_i64_mod_u_wrap(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a % (uint64_t) b);
}

static inline int64_t xr_i64_shl_wrap(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a << ((uint64_t) b & 63));
}

static inline int64_t xr_i64_shr_wrap(int64_t a, int64_t b) {
    return a >> ((uint64_t) b & 63);
}

/* Logical (zero-extending) right shift for statically-unsigned lhs (uint64
 * is the only width where it differs from the arithmetic shift above: the
 * narrower unsigned payloads are already zero-extended in the i64 value
 * model). Shift count taken mod 64, same as the arithmetic variant. */
static inline int64_t xr_i64_shr_u_wrap(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a >> ((uint64_t) b & 63));
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
