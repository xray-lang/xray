/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_int_arith_core.h - Defined int64 arithmetic helpers shared by VM/AOT/native methods.
 */

#ifndef XR_INT_ARITH_CORE_H
#define XR_INT_ARITH_CORE_H

#include "xr_semantic_owner_ids_gen.h"
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

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

/* Owner guards for xi.add / xi.sub / xi.mul.  The wrapping semantics are stated
 * once, here; every consumer reaches them through XR_INT_WRAP_OWNER_APPLY and
 * names both the owner ID and its own consumer bit, so a second implementation
 * or an undeclared consumer fails to compile rather than diverging quietly. */
#define XR_INT_WRAP_OWNER_GUARD(owner_hi, owner_lo)                                                \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_primitive_integer_wrapping                                   \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_PRIMITIVE_INTEGER_WRAPPING_HI &&          \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_PRIMITIVE_INTEGER_WRAPPING_LO)            \
                   ? 1                                                                             \
                   : -1);                                                                          \
    }))

#define XR_INT_WRAP_CONSUMER_GUARD(consumer_bit)                                                   \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_primitive_integer_wrapping                      \
            : (((uint32_t) (consumer_bit) != 0 &&                                                  \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&            \
                (XR_SEM_OWNER_ID_PRIMITIVE_INTEGER_WRAPPING_CONSUMERS &                            \
                 (uint32_t) (consumer_bit)) != 0)                                                  \
                   ? 1                                                                             \
                   : -1);                                                                          \
    }))

#define XR_INT_WRAP_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, kernel, lhs, rhs)                \
    (XR_INT_WRAP_OWNER_GUARD((owner_hi), (owner_lo)), XR_INT_WRAP_CONSUMER_GUARD((consumer_bit)),  \
     (kernel) ((int64_t) (lhs), (int64_t) (rhs)))

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

/* Both halves of a full-width unsigned 64 x 64 product.  Keeping the pair in
 * one expression lets native compilers select the target's single wide
 * multiply instruction when AOT CGen proves that low and high Xi operations
 * consume the same operands. */
static inline uint64_t xr_u64_mul_wide(uint64_t a, uint64_t b, uint64_t *high) {
#if defined(__SIZEOF_INT128__)
    __uint128_t product = (__uint128_t) a * (__uint128_t) b;
    *high = (uint64_t) (product >> 64);
    return (uint64_t) product;
#elif defined(_MSC_VER) && defined(_M_X64)
    return _umul128(a, b, high);
#else
    *high = xr_u64_mul_high(a, b);
    return a * b;
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

/* Canonical xi.div / xi.mod integer semantics.
 *
 * The signed kinds implement the language rule that division is total on the
 * two's-complement i64 domain once the divisor is nonzero:
 *   INT64_MIN / -1 = INT64_MIN  (unsigned negate; raw C signed division is UB
 *                                and traps IDIV on x86-64)
 *   INT64_MIN % -1 = 0
 * The unsigned kinds mirror OP_DIV_U / OP_MOD_U: the i64 value slot carries no
 * signedness tag, so u64/usize operands with the top bit set must divide as
 * unsigned. Narrower unsigned payloads are zero-extended, so both agree there.
 *
 * Divide-by-zero is reported, never raised: xr_int_div_mod_eval hands the
 * backend a flag and each consumer publishes it in its own domain (VM runtime
 * error, hosted throw, freestanding trap). The kernel owns the value rule; the
 * adapters own the error channel. */
typedef enum XrIntDivModKind {
    XR_INT_DIV_MOD_DIV = 0,
    XR_INT_DIV_MOD_MOD = 1,
    XR_INT_DIV_MOD_DIV_U = 2,
    XR_INT_DIV_MOD_MOD_U = 3,
} XrIntDivModKind;

/* What the caller has already proven about the divisor. A proof is a promise
 * the plan discharged, so it selects a strictly narrower rule:
 *   NONE      nothing proven; the zero probe belongs to the kernel
 *   NONZERO   divisor != 0; the signed wrap rule can still fire
 *   POSITIVE  divisor  > 0; the wrap rule is unreachable, leaving the plain
 *             machine divide with no residual compare */
typedef enum XrIntDivModProof {
    XR_INT_DIV_MOD_PROOF_NONE = 0,
    XR_INT_DIV_MOD_PROOF_NONZERO = 1,
    XR_INT_DIV_MOD_PROOF_POSITIVE = 2,
} XrIntDivModProof;

typedef struct XrIntDivModResult {
    int64_t value;
    bool divisor_is_zero;
} XrIntDivModResult;

/* Value rule for a divisor the caller proved nonzero. */
static inline int64_t xr_int_div_mod_apply(XrIntDivModKind kind, XrIntDivModProof proof, int64_t a,
                                           int64_t b) {
    switch (kind) {
        case XR_INT_DIV_MOD_DIV:
            if (proof != XR_INT_DIV_MOD_PROOF_POSITIVE && b == -1)
                return xr_i64_neg_wrap(a);
            return a / b;
        case XR_INT_DIV_MOD_MOD:
            if (proof != XR_INT_DIV_MOD_PROOF_POSITIVE && b == -1)
                return 0;
            return a % b;
        case XR_INT_DIV_MOD_DIV_U:
            return (int64_t) ((uint64_t) a / (uint64_t) b);
        case XR_INT_DIV_MOD_MOD_U:
            return (int64_t) ((uint64_t) a % (uint64_t) b);
    }
    return 0;
}

/* Full rule including the divide-by-zero probe the backend must publish. */
static inline XrIntDivModResult xr_int_div_mod_eval(XrIntDivModKind kind, XrIntDivModProof proof,
                                                    int64_t a, int64_t b) {
    XrIntDivModResult result;
    result.value = 0;
    result.divisor_is_zero = false;
    if (proof == XR_INT_DIV_MOD_PROOF_NONE && b == 0) {
        result.divisor_is_zero = true;
        return result;
    }
    result.value = xr_int_div_mod_apply(kind, proof, a, b);
    return result;
}

#define XR_INT_DIV_MOD_OWNER_GUARD(owner_hi, owner_lo)                                             \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_int_div_mod                                           \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_HI &&                  \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_LO)                    \
                   ? 1                                                                             \
                   : -1);                                                                          \
    }))

#define XR_INT_DIV_MOD_CONSUMER_GUARD(consumer_bit)                                                \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_int_div_mod                              \
            : (((uint32_t) (consumer_bit) != 0 &&                                                  \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&            \
                (XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_CONSUMERS & (uint32_t) (consumer_bit)) != 0)   \
                   ? 1                                                                             \
                   : -1);                                                                          \
    }))

#define XR_INT_DIV_MOD_KIND_GUARD(kind)                                                            \
    ((void) sizeof(struct {                                                                        \
        unsigned int kind_must_be_a_declared_int_div_mod_kind                                      \
            : (((kind) == XR_INT_DIV_MOD_DIV || (kind) == XR_INT_DIV_MOD_MOD ||                    \
                (kind) == XR_INT_DIV_MOD_DIV_U || (kind) == XR_INT_DIV_MOD_MOD_U)                  \
                   ? 1                                                                             \
                   : -1);                                                                          \
    }))

#define XR_INT_DIV_MOD_PROOF_GUARD(proof)                                                          \
    ((void) sizeof(struct {                                                                        \
        unsigned int proof_must_be_a_declared_int_div_mod_proof                                    \
            : (((proof) == XR_INT_DIV_MOD_PROOF_NONE || (proof) == XR_INT_DIV_MOD_PROOF_NONZERO || \
                (proof) == XR_INT_DIV_MOD_PROOF_POSITIVE)                                          \
                   ? 1                                                                             \
                   : -1);                                                                          \
    }))

/* A proven divisor skips the kernel's zero probe, so the proof itself must be
 * one the plan discharged. Emitting a "proven" division without a proof is a
 * compile-time failure rather than a silent unchecked divide. */
#define XR_INT_DIV_MOD_PROVEN_GUARD(proof)                                                         \
    ((void) sizeof(struct {                                                                        \
        unsigned int divisor_proof_must_be_discharged                                              \
            : (((proof) == XR_INT_DIV_MOD_PROOF_NONZERO ||                                         \
                (proof) == XR_INT_DIV_MOD_PROOF_POSITIVE)                                          \
                   ? 1                                                                             \
                   : -1);                                                                          \
    }))

#define XR_INT_DIV_MOD_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, kind, proof, a, b)            \
    (XR_INT_DIV_MOD_OWNER_GUARD((owner_hi), (owner_lo)),                                           \
     XR_INT_DIV_MOD_CONSUMER_GUARD((consumer_bit)), XR_INT_DIV_MOD_KIND_GUARD((kind)),             \
     XR_INT_DIV_MOD_PROOF_GUARD((proof)),                                                          \
     xr_int_div_mod_eval((XrIntDivModKind) (kind), (XrIntDivModProof) (proof), (int64_t) (a),      \
                         (int64_t) (b)))

#define XR_INT_DIV_MOD_OWNER_APPLY_PROVEN(owner_hi, owner_lo, consumer_bit, kind, proof, a, b)     \
    (XR_INT_DIV_MOD_OWNER_GUARD((owner_hi), (owner_lo)),                                           \
     XR_INT_DIV_MOD_CONSUMER_GUARD((consumer_bit)), XR_INT_DIV_MOD_KIND_GUARD((kind)),             \
     XR_INT_DIV_MOD_PROVEN_GUARD((proof)),                                                         \
     xr_int_div_mod_apply((XrIntDivModKind) (kind), (XrIntDivModProof) (proof), (int64_t) (a),     \
                          (int64_t) (b)))

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

#endif /* XR_INT_ARITH_CORE_H */
