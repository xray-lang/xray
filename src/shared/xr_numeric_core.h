/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_numeric_core.h - Runtime-neutral scalar numeric method helpers.
 */

#ifndef XR_NUMERIC_CORE_H
#define XR_NUMERIC_CORE_H

#include "../base/xconstants.h"
#include "xr_semantic_owner_ids_gen.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef enum XrNumericNegKind {
    XR_NUMERIC_NEG_I64 = 0,
    XR_NUMERIC_NEG_F64 = 1,
} XrNumericNegKind;

typedef struct XrNumericNegResult {
    int64_t i64;
    double f64;
} XrNumericNegResult;

typedef struct XrNumericNegBigIntPlan {
    uint32_t length;
    int8_t result_sign;
    uint8_t valid;
} XrNumericNegBigIntPlan;

/* Canonical xi.neg scalar semantics. Integer negation wraps in the two's-
 * complement i64 domain. Floating negation toggles the IEEE-754 sign bit, so
 * signed zero, infinities, and NaN payloads are preserved exactly. */
static inline XrNumericNegResult xr_numeric_neg_eval(XrNumericNegKind kind, int64_t i64,
                                                     double f64) {
    XrNumericNegResult result = {0, 0.0};
    if (kind == XR_NUMERIC_NEG_I64) {
        result.i64 = (int64_t) (UINT64_C(0) - (uint64_t) i64);
        return result;
    }
    if (kind == XR_NUMERIC_NEG_F64) {
        uint64_t bits = 0;
        memcpy(&bits, &f64, sizeof(bits));
        bits ^= UINT64_C(1) << 63;
        memcpy(&result.f64, &bits, sizeof(result.f64));
    }
    return result;
}

/* BigInt adapters own allocation and copying only. This plan is the unique
 * rule for canonical zero and sign inversion over normalized magnitude limbs. */
static inline XrNumericNegBigIntPlan xr_numeric_neg_bigint_plan(const uint32_t *limbs,
                                                                uint32_t length, int8_t sign) {
    XrNumericNegBigIntPlan plan = {length, 1, 0};
    if (!limbs || length == 0 || (sign != 1 && sign != -1))
        return plan;
    if (length > 1 && limbs[length - 1] == 0)
        return plan;
    bool zero = true;
    for (uint32_t i = 0; i < length; i++) {
        if (limbs[i] != 0) {
            zero = false;
            break;
        }
    }
    plan.result_sign = zero ? 1 : (int8_t) -sign;
    plan.valid = 1;
    return plan;
}

#define XR_NUMERIC_NEG_OWNER_GUARD(owner_hi, owner_lo)                                            \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_numeric_neg                                          \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_HI &&                 \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_LO)                   \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_NUMERIC_NEG_CONSUMER_GUARD(consumer_bit)                                               \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_numeric_neg                             \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&           \
                (XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_CONSUMERS & (uint32_t) (consumer_bit)) != 0) \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_NUMERIC_NEG_KIND_GUARD(kind)                                                           \
    ((void) sizeof(struct {                                                                        \
        unsigned int kind_must_be_numeric_neg_i64_or_f64                                         \
            : (((kind) == XR_NUMERIC_NEG_I64 || (kind) == XR_NUMERIC_NEG_F64) ? 1 : -1);         \
    }))

#define XR_NUMERIC_NEG_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, kind, i64, f64)              \
    (XR_NUMERIC_NEG_OWNER_GUARD((owner_hi), (owner_lo)),                                           \
     XR_NUMERIC_NEG_CONSUMER_GUARD((consumer_bit)),                                                \
     XR_NUMERIC_NEG_KIND_GUARD((kind)),                                                            \
     xr_numeric_neg_eval((XrNumericNegKind) (kind), (int64_t) (i64), (double) (f64)))

#define XR_NUMERIC_NEG_BIGINT_OWNER_PLAN(owner_hi, owner_lo, consumer_bit, limbs, length, sign)   \
    (XR_NUMERIC_NEG_OWNER_GUARD((owner_hi), (owner_lo)),                                           \
     XR_NUMERIC_NEG_CONSUMER_GUARD((consumer_bit)),                                                \
     xr_numeric_neg_bigint_plan((limbs), (uint32_t) (length), (int8_t) (sign)))

static inline uint64_t xr_numeric_core_i64_abs_magnitude(int64_t value) {
    if (value >= 0)
        return (uint64_t) value;
    return (uint64_t) (-(value + 1)) + 1u;
}

static inline int64_t xr_numeric_core_i64_abs_wrap(int64_t value) {
    if (value >= 0)
        return value;
    return (int64_t) (-(uint64_t) value);
}

static inline int64_t xr_numeric_core_i64_add_wrap(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a + (uint64_t) b);
}

static inline int64_t xr_numeric_core_i64_sub_wrap(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a - (uint64_t) b);
}

static inline int64_t xr_numeric_core_i64_mul_wrap(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a * (uint64_t) b);
}

static inline int64_t xr_numeric_core_i64_neg_wrap(int64_t value) {
    return xr_numeric_neg_eval(XR_NUMERIC_NEG_I64, value, 0.0).i64;
}

/* Integer division and modulo belong to the shared.int-div-mod owner in
 * xr_int_arith_core.h; restating the wrap rule here would be a second
 * semantic source. */

/* ==========================================================================
 * BigInt binary dispatch
 *
 * Whether an operand pair evaluates in the BigInt domain is a language
 * question, not a per-backend one. The VM read it as "either side is a
 * BigInt" while the hosted AOT profile read it as "both sides are BigInt", so
 * `100n + 5` produced a BigInt under the interpreter and, once compiled, a
 * double read off the BigInt pointer. Both dispatchers now ask here.
 * ========================================================================== */

typedef enum XrBigIntOperandKind {
    XR_BIGINT_OPERAND_INT = 0,    /* int64 payload: promotes to a BigInt exactly */
    XR_BIGINT_OPERAND_BIGINT = 1, /* already a BigInt */
    XR_BIGINT_OPERAND_OTHER = 2   /* float, string, object, null: no promotion */
} XrBigIntOperandKind;

typedef enum XrBigIntBinaryDispatch {
    XR_BIGINT_BINARY_NONE = 0,     /* no BigInt operand: the numeric lanes decide */
    XR_BIGINT_BINARY_EVALUATE = 1, /* evaluate as BigInt, promoting an int operand */
    XR_BIGINT_BINARY_INVALID = 2   /* BigInt paired with an operand it has no rule for */
} XrBigIntBinaryDispatch;

/* A BigInt on either side pulls the whole operation into the BigInt domain,
 * and the other side may only be an int. Anything else has no rule, so it
 * fails closed instead of reinterpreting that operand's payload. */
static inline XrBigIntBinaryDispatch xr_bigint_binary_dispatch(XrBigIntOperandKind left,
                                                               XrBigIntOperandKind right) {
    if (left != XR_BIGINT_OPERAND_BIGINT && right != XR_BIGINT_OPERAND_BIGINT)
        return XR_BIGINT_BINARY_NONE;
    if (left == XR_BIGINT_OPERAND_OTHER || right == XR_BIGINT_OPERAND_OTHER)
        return XR_BIGINT_BINARY_INVALID;
    return XR_BIGINT_BINARY_EVALUATE;
}

/* Sign-magnitude limb form of an int64, the representation a BigInt stores.
 * The VM allocator and the AOT promotion both encode through this, so a
 * promoted operand carries identical limbs whichever backend built it. */
typedef struct XrBigIntI64Limbs {
    uint32_t limbs[2];
    uint32_t len;
    int8_t sign;
} XrBigIntI64Limbs;

static inline XrBigIntI64Limbs xr_bigint_limbs_from_i64(int64_t value) {
    uint64_t magnitude = xr_numeric_core_i64_abs_magnitude(value);
    XrBigIntI64Limbs out;
    out.limbs[0] = (uint32_t) (magnitude & 0xFFFFFFFFu);
    out.limbs[1] = (uint32_t) (magnitude >> 32);
    out.len = out.limbs[1] != 0u ? 2u : 1u;
    out.sign = value < 0 ? (int8_t) -1 : (int8_t) 1;
    return out;
}

static inline int xr_numeric_core_format_i64(char *buf, size_t bufsz, int64_t value) {
    if (!buf || bufsz == 0)
        return -1;

    char digits[20];
    int n = 0;
    uint64_t mag = xr_numeric_core_i64_abs_magnitude(value);
    do {
        digits[n++] = (char) ('0' + (mag % 10u));
        mag /= 10u;
    } while (mag != 0);

    bool neg = value < 0;
    size_t len = (size_t) n + (neg ? 1u : 0u);
    if (len + 1u > bufsz)
        return -1;

    char *p = buf;
    if (neg)
        *p++ = '-';
    while (n > 0)
        *p++ = digits[--n];
    *p = '\0';
    return (int) len;
}

static inline int xr_numeric_core_format_i64_hex(char *buf, size_t bufsz, int64_t value) {
    if (!buf || bufsz == 0)
        return -1;

    static const char kHex[] = "0123456789ABCDEF";
    char digits[16];
    int n = 0;
    uint64_t mag = xr_numeric_core_i64_abs_magnitude(value);
    do {
        digits[n++] = kHex[mag & 0xFu];
        mag >>= 4u;
    } while (mag != 0);

    bool neg = value < 0;
    size_t prefix_len = neg ? 3u : 2u;
    size_t len = prefix_len + (size_t) n;
    if (len + 1u > bufsz)
        return -1;

    char *p = buf;
    if (neg)
        *p++ = '-';
    *p++ = '0';
    *p++ = 'x';
    while (n > 0)
        *p++ = digits[--n];
    *p = '\0';
    return (int) len;
}

static inline int xr_numeric_core_to_fixed_decimals(int64_t decimals) {
    if (decimals < 0)
        return 0;
    if (decimals > XR_TOFIXED_MAX_DECIMALS)
        return XR_TOFIXED_MAX_DECIMALS;
    return (int) decimals;
}

static inline int xr_numeric_core_format_fixed(char *buf, size_t bufsz, double value,
                                               int64_t decimals) {
    if (!buf || bufsz == 0)
        return -1;
    return snprintf(buf, bufsz, "%.*f", xr_numeric_core_to_fixed_decimals(decimals), value);
}

#endif /* XR_NUMERIC_CORE_H */
