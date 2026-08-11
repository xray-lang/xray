/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_bits_core.h - Runtime-neutral bit-manipulation intrinsics.
 *
 * KEY CONCEPT:
 *   Single semantic source for exact-width integer bit intrinsics. Xi stores
 *   the receiver's XrNativeType in aux_int; the VM and constant folders call
 *   this core, while AOT emits the equivalent compiler-recognizable native
 *   expression.  Values use an int64 payload, including uint64 bit patterns.
 *
 *   Self-contained: depends only on <stdint.h> plus <intrin.h> on MSVC,
 *   so it stays includable from the freestanding AOT runtime.
 */

#ifndef XRAY_SHARED_XR_BITS_CORE_H
#define XRAY_SHARED_XR_BITS_CORE_H

#include "xr_semantic_owner_ids_gen.h"
#include <stdint.h>
#include "xr_native_type_core.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

#ifndef XR_HAS_BUILTIN
#if defined(__has_builtin)
#define XR_HAS_BUILTIN(x) __has_builtin(x)
#else
#define XR_HAS_BUILTIN(x) 0
#endif
#endif

/* AOT emits these macro intrinsics for exact-width rotates. Clang's rotate
 * builtins preserve the scalar rotate operation through optimization; spelling
 * a rotate as shifts plus OR can trigger unprofitable SLP vectorization on
 * targets whose SIMD ISA has no rotate instruction. The fallback remains
 * defined for C toolchains without rotate builtins and masks both counts so
 * zero and negative counts never cause an invalid-width shift. */
static inline uint32_t xr_bits_aot_rotate_input_u32(uint32_t value) {
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    /* AArch64 NEON has no lane-wise rotate. Without this zero-instruction
     * scalar fence, Clang can SLP-pack four independent uint32 rotates into
     * shift/shift/or vectors that are substantially slower than scalar ror.
     * Keep other targets free to use their native vector rotate cost model. */
    __asm__("" : "+r"(value));
#endif
    return value;
}

static inline uint8_t xr_bits_aot_rotl8(uint8_t value, int64_t count) {
#if XR_HAS_BUILTIN(__builtin_rotateleft8)
    return __builtin_rotateleft8(value, (uint8_t) count);
#else
    const uint64_t n = (uint64_t) count;
    return (uint8_t) ((value << (n & 7u)) | (value >> ((-n) & 7u)));
#endif
}

static inline uint8_t xr_bits_aot_rotr8(uint8_t value, int64_t count) {
#if XR_HAS_BUILTIN(__builtin_rotateright8)
    return __builtin_rotateright8(value, (uint8_t) count);
#else
    const uint64_t n = (uint64_t) count;
    return (uint8_t) ((value >> (n & 7u)) | (value << ((-n) & 7u)));
#endif
}

static inline uint16_t xr_bits_aot_rotl16(uint16_t value, int64_t count) {
#if XR_HAS_BUILTIN(__builtin_rotateleft16)
    return __builtin_rotateleft16(value, (uint16_t) count);
#else
    const uint64_t n = (uint64_t) count;
    return (uint16_t) ((value << (n & 15u)) | (value >> ((-n) & 15u)));
#endif
}

static inline uint16_t xr_bits_aot_rotr16(uint16_t value, int64_t count) {
#if XR_HAS_BUILTIN(__builtin_rotateright16)
    return __builtin_rotateright16(value, (uint16_t) count);
#else
    const uint64_t n = (uint64_t) count;
    return (uint16_t) ((value >> (n & 15u)) | (value << ((-n) & 15u)));
#endif
}

static inline uint32_t xr_bits_aot_rotl32(uint32_t value, int64_t count) {
    value = xr_bits_aot_rotate_input_u32(value);
#if XR_HAS_BUILTIN(__builtin_rotateleft32)
    return __builtin_rotateleft32(value, (uint32_t) count);
#else
    const uint64_t n = (uint64_t) count;
    return (value << (n & 31u)) | (value >> ((-n) & 31u));
#endif
}

static inline uint32_t xr_bits_aot_rotr32(uint32_t value, int64_t count) {
    value = xr_bits_aot_rotate_input_u32(value);
#if XR_HAS_BUILTIN(__builtin_rotateright32)
    return __builtin_rotateright32(value, (uint32_t) count);
#else
    const uint64_t n = (uint64_t) count;
    return (value >> (n & 31u)) | (value << ((-n) & 31u));
#endif
}

static inline uint64_t xr_bits_aot_rotl64(uint64_t value, int64_t count) {
#if XR_HAS_BUILTIN(__builtin_rotateleft64)
    return __builtin_rotateleft64(value, (uint64_t) count);
#else
    const uint64_t n = (uint64_t) count;
    return (value << (n & 63u)) | (value >> ((-n) & 63u));
#endif
}

static inline uint64_t xr_bits_aot_rotr64(uint64_t value, int64_t count) {
#if XR_HAS_BUILTIN(__builtin_rotateright64)
    return __builtin_rotateright64(value, (uint64_t) count);
#else
    const uint64_t n = (uint64_t) count;
    return (value >> (n & 63u)) | (value << ((-n) & 63u));
#endif
}

/* Keep generated C independent of compiler-specific builtin spellings while
 * evaluating both operands exactly once on every toolchain. */
#define XR_BITS_ROTL8(value, count) xr_bits_aot_rotl8((uint8_t) (value), (int64_t) (count))
#define XR_BITS_ROTR8(value, count) xr_bits_aot_rotr8((uint8_t) (value), (int64_t) (count))
#define XR_BITS_ROTL16(value, count) xr_bits_aot_rotl16((uint16_t) (value), (int64_t) (count))
#define XR_BITS_ROTR16(value, count) xr_bits_aot_rotr16((uint16_t) (value), (int64_t) (count))
#define XR_BITS_ROTL32(value, count) xr_bits_aot_rotl32((uint32_t) (value), (int64_t) (count))
#define XR_BITS_ROTR32(value, count) xr_bits_aot_rotr32((uint32_t) (value), (int64_t) (count))
#define XR_BITS_ROTL64(value, count) xr_bits_aot_rotl64((uint64_t) (value), (int64_t) (count))
#define XR_BITS_ROTR64(value, count) xr_bits_aot_rotr64((uint64_t) (value), (int64_t) (count))

static inline uint8_t xr_bits_exact_width(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
            return 8;
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
            return 16;
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
            return 32;
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
            return (uint8_t) (sizeof(void *) * 8u);
        case XR_NATIVE_I64:
        case XR_NATIVE_U64:
        default:
            return 64;
    }
}

static inline int xr_bits_exact_is_signed(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
        case XR_NATIVE_USIZE:
            return 0;
        default:
            return 1;
    }
}

static inline uint64_t xr_bits_exact_mask(uint8_t width) {
    return width >= 64 ? UINT64_MAX : (UINT64_C(1) << width) - UINT64_C(1);
}

static inline uint64_t xr_bits_exact_pattern(int64_t x, uint8_t native_type) {
    return (uint64_t) x & xr_bits_exact_mask(xr_bits_exact_width(native_type));
}

/* Restore the exact receiver type in the VM's int64 payload convention. */
static inline int64_t xr_bits_exact_restore(uint64_t bits, uint8_t native_type) {
    uint8_t width = xr_bits_exact_width(native_type);
    uint64_t mask = xr_bits_exact_mask(width);
    bits &= mask;
    if (xr_bits_exact_is_signed(native_type) && width < 64 &&
        (bits & (UINT64_C(1) << (width - 1u))) != 0)
        bits |= ~mask;
    return (int64_t) bits;
}

/* Canonical xi.bnot semantics are a signed 64-bit two's-complement complement.
 * Convert the complemented bit pattern without an implementation-defined
 * uint64_t-to-int64_t cast so hosted and freestanding C agree at both edges. */
static inline int64_t xr_bits_not_i64(int64_t value) {
    uint64_t bits = ~(uint64_t) value;
    if ((bits & (UINT64_C(1) << 63)) == 0)
        return (int64_t) bits;
    return INT64_MIN + (int64_t) (bits & (uint64_t) INT64_MAX);
}

typedef enum XrShiftKind {
    XR_SHIFT_LEFT = 0,
    XR_SHIFT_RIGHT_SIGNED = 1,
    XR_SHIFT_RIGHT_UNSIGNED = 2,
} XrShiftKind;

typedef enum XrShiftStatus {
    XR_SHIFT_STATUS_OK = 0,
    XR_SHIFT_STATUS_COUNT_RANGE = 1,
    XR_SHIFT_STATUS_CAPACITY_OVERFLOW = 2,
} XrShiftStatus;

typedef struct XrBigIntShiftPlan {
    XrShiftStatus status;
    XrShiftKind kind;
    uint32_t limb_shift;
    uint32_t bit_shift;
    uint32_t capacity;
    uint8_t copy_input;
    uint8_t zero_result;
} XrBigIntShiftPlan;

static inline int64_t xr_shift_i64_from_bits(uint64_t bits) {
    if ((bits & (UINT64_C(1) << 63)) == 0)
        return (int64_t) bits;
    return INT64_MIN + (int64_t) (bits & (uint64_t) INT64_MAX);
}

/* Canonical scalar shift semantics. Counts are modulo 64, left shift wraps,
 * and signed right shift is explicitly sign-extending on every C compiler. */
static inline int64_t xr_shift_i64(XrShiftKind kind, int64_t value, int64_t count) {
    uint32_t n = (uint32_t) ((uint64_t) count & UINT64_C(63));
    uint64_t bits = (uint64_t) value;
    if (kind == XR_SHIFT_LEFT)
        return xr_shift_i64_from_bits(bits << n);
    if (kind == XR_SHIFT_RIGHT_UNSIGNED)
        return xr_shift_i64_from_bits(bits >> n);
    if (n == 0 || value >= 0)
        return xr_shift_i64_from_bits(bits >> n);
    return xr_shift_i64_from_bits((bits >> n) | (UINT64_MAX << (64u - n)));
}

/* Canonical BigInt planning. Unlike scalar shifts, BigInt counts are not
 * masked: they must fit uint32_t and must not be negative. */
static inline XrBigIntShiftPlan xr_shift_bigint_plan(XrShiftKind kind, uint32_t source_len,
                                                     int source_is_zero, int64_t count) {
    XrBigIntShiftPlan plan = {XR_SHIFT_STATUS_OK, kind, 0, 0, 1, 0, 0};
    if (kind != XR_SHIFT_LEFT && kind != XR_SHIFT_RIGHT_SIGNED &&
        kind != XR_SHIFT_RIGHT_UNSIGNED) {
        plan.status = XR_SHIFT_STATUS_COUNT_RANGE;
        return plan;
    }
    if (count < 0 || (uint64_t) count > UINT32_MAX) {
        plan.status = XR_SHIFT_STATUS_COUNT_RANGE;
        return plan;
    }
    if (source_len == 0)
        source_len = 1;
    if (source_is_zero || count == 0) {
        plan.capacity = source_len;
        plan.copy_input = 1;
        return plan;
    }
    plan.limb_shift = (uint32_t) count / 32u;
    plan.bit_shift = (uint32_t) count % 32u;
    if (kind == XR_SHIFT_LEFT) {
        if (source_len == UINT32_MAX ||
            plan.limb_shift > UINT32_MAX - source_len - 1u) {
            plan.status = XR_SHIFT_STATUS_CAPACITY_OVERFLOW;
            return plan;
        }
        plan.capacity = source_len + plan.limb_shift + 1u;
        return plan;
    }
    if (plan.limb_shift >= source_len) {
        plan.zero_result = 1;
        return plan;
    }
    plan.capacity = source_len - plan.limb_shift;
    return plan;
}

/* Apply a validated plan to little-endian base-2^32 magnitude limbs. The
 * returned length and sign are normalized; adapters only allocate storage. */
static inline uint32_t xr_shift_bigint_apply(const XrBigIntShiftPlan *plan,
                                             const uint32_t *source, uint32_t source_len,
                                             int8_t source_sign, uint32_t *result,
                                             int8_t *result_sign) {
    uint32_t result_len = 1;
    if (!plan || plan->status != XR_SHIFT_STATUS_OK || !source || !result || !result_sign)
        return 0;
    for (uint32_t i = 0; i < plan->capacity; i++)
        result[i] = 0;
    if (plan->copy_input) {
        result_len = source_len ? source_len : 1;
        for (uint32_t i = 0; i < source_len; i++)
            result[i] = source[i];
    } else if (plan->zero_result) {
        result[0] = 0;
    } else if (plan->kind == XR_SHIFT_LEFT) {
        uint32_t carry = 0;
        for (uint32_t i = 0; i < source_len; i++) {
            uint64_t shifted = ((uint64_t) source[i] << plan->bit_shift) | carry;
            result[i + plan->limb_shift] = (uint32_t) shifted;
            carry = (uint32_t) (shifted >> 32);
        }
        result_len = source_len + plan->limb_shift;
        if (carry != 0)
            result[result_len++] = carry;
    } else {
        uint32_t carry = 0;
        uint32_t carry_mask = plan->bit_shift == 0
                                  ? 0
                                  : UINT32_MAX >> (32u - plan->bit_shift);
        result_len = source_len - plan->limb_shift;
        for (uint32_t i = result_len; i-- > 0;) {
            uint32_t limb = source[i + plan->limb_shift];
            uint64_t joined = ((uint64_t) carry << 32) | limb;
            result[i] = (uint32_t) (joined >> plan->bit_shift);
            carry = limb & carry_mask;
        }
    }
    while (result_len > 1 && result[result_len - 1] == 0)
        result_len--;
    *result_sign = (result_len == 1 && result[0] == 0) ? 1 : (source_sign < 0 ? -1 : 1);
    return result_len;
}

/* Number of set bits in the receiver's exact-width bit pattern. */
static inline int64_t xr_bits_exact_popcount(int64_t x, uint8_t native_type) {
    uint64_t u = xr_bits_exact_pattern(x, native_type);
#if defined(_MSC_VER) && !defined(__clang__)
    return (int64_t) __popcnt64((unsigned long long) u);
#else
    return (int64_t) __builtin_popcountll((unsigned long long) u);
#endif
}

/* Count leading zeros in the exact receiver width.  Zero returns that width. */
static inline int64_t xr_bits_exact_leading_zeros(int64_t x, uint8_t native_type) {
    uint8_t width = xr_bits_exact_width(native_type);
    uint64_t u = xr_bits_exact_pattern(x, native_type);
    if (u == 0)
        return (int64_t) width;
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long idx;
    _BitScanReverse64(&idx, u);
    return (int64_t) (width - 1u - (uint8_t) idx);
#else
    return (int64_t) (__builtin_clzll(u) - (64u - width));
#endif
}

/* Count trailing zeros in the exact receiver width.  Zero returns that width. */
static inline int64_t xr_bits_exact_trailing_zeros(int64_t x, uint8_t native_type) {
    uint8_t width = xr_bits_exact_width(native_type);
    uint64_t u = xr_bits_exact_pattern(x, native_type);
    if (u == 0)
        return (int64_t) width;
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long idx;
    _BitScanForward64(&idx, u);
    return (int64_t) idx;
#else
    return (int64_t) __builtin_ctzll(u);
#endif
}

/* Reverse byte order inside the exact receiver width. */
static inline int64_t xr_bits_exact_byteswap(int64_t x, uint8_t native_type) {
    uint8_t width = xr_bits_exact_width(native_type);
    uint64_t u = xr_bits_exact_pattern(x, native_type);
    uint64_t out = 0;
    for (uint8_t shift = 0; shift < width; shift = (uint8_t) (shift + 8u)) {
        out = (out << 8u) | ((u >> shift) & UINT64_C(0xff));
    }
    return xr_bits_exact_restore(out, native_type);
}

/* Rotate counts use Euclidean modulo W. W is always a power of two, so the
 * unsigned mask also gives the required result for negative counts. */
static inline int64_t xr_bits_exact_rotate_left(int64_t x, int64_t n, uint8_t native_type) {
    uint8_t width = xr_bits_exact_width(native_type);
    uint64_t mask = xr_bits_exact_mask(width);
    uint64_t u = (uint64_t) x & mask;
    unsigned r = (unsigned) ((uint64_t) n & (uint64_t) (width - 1u));
    uint64_t out = ((u << r) | (u >> ((width - r) & (width - 1u)))) & mask;
    return xr_bits_exact_restore(out, native_type);
}

static inline int64_t xr_bits_exact_rotate_right(int64_t x, int64_t n, uint8_t native_type) {
    uint8_t width = xr_bits_exact_width(native_type);
    uint64_t mask = xr_bits_exact_mask(width);
    uint64_t u = (uint64_t) x & mask;
    unsigned r = (unsigned) ((uint64_t) n & (uint64_t) (width - 1u));
    uint64_t out = ((u >> r) | (u << ((width - r) & (width - 1u)))) & mask;
    return xr_bits_exact_restore(out, native_type);
}

/* High 64 bits of the unsigned 128-bit product. Prefers the compiler's native
 * wide multiply so AArch64 emits umulh and x86-64 emits mulx/mul high; the
 * portable 32x32 fallback keeps the header freestanding on toolchains without
 * __int128 or the MSVC intrinsic. */
static inline uint64_t xr_bits_umulh64(uint64_t a, uint64_t b) {
#if defined(__SIZEOF_INT128__)
    return (uint64_t) (((unsigned __int128) a * (unsigned __int128) b) >> 64);
#elif defined(_MSC_VER) && !defined(__clang__)
    return __umulh(a, b);
#else
    uint64_t al = a & UINT64_C(0xffffffff), ah = a >> 32;
    uint64_t bl = b & UINT64_C(0xffffffff), bh = b >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint64_t cross = (ll >> 32) + (lh & UINT64_C(0xffffffff)) + (hl & UINT64_C(0xffffffff));
    return hh + (lh >> 32) + (hl >> 32) + (cross >> 32);
#endif
}

/* High 64 bits of the signed 128-bit product (AArch64 smulh / x86-64 imul high). */
static inline int64_t xr_bits_smulh64(int64_t a, int64_t b) {
#if defined(__SIZEOF_INT128__)
    return (int64_t) (uint64_t) (((__int128) a * (__int128) b) >> 64);
#elif defined(_MSC_VER) && !defined(__clang__)
    return __mulh(a, b);
#else
    uint64_t hi = xr_bits_umulh64((uint64_t) a, (uint64_t) b);
    if (a < 0)
        hi -= (uint64_t) b;
    if (b < 0)
        hi -= (uint64_t) a;
    return (int64_t) hi;
#endif
}

/* High half of the full 2W-bit product of two exact-width receivers. Unsigned
 * receivers take the unsigned product, signed receivers the signed product; the
 * result follows the exact-width int64 payload convention. */
static inline int64_t xr_bits_exact_mul_high(int64_t a, int64_t b, uint8_t native_type) {
    uint8_t width = xr_bits_exact_width(native_type);
    int is_signed = xr_bits_exact_is_signed(native_type);
    if (width >= 64)
        return is_signed ? xr_bits_smulh64(a, b)
                         : (int64_t) xr_bits_umulh64((uint64_t) a, (uint64_t) b);
    if (is_signed) {
        int64_t sa = xr_bits_exact_restore(xr_bits_exact_pattern(a, native_type), native_type);
        int64_t sb = xr_bits_exact_restore(xr_bits_exact_pattern(b, native_type), native_type);
        return xr_bits_exact_restore((uint64_t) ((sa * sb) >> width), native_type);
    }
    uint64_t ua = xr_bits_exact_pattern(a, native_type);
    uint64_t ub = xr_bits_exact_pattern(b, native_type);
    return xr_bits_exact_restore((ua * ub) >> width, native_type);
}

/* Canonical entry points keep operation selection in the shared owner while
 * allowing VM and AOT adapters to marshal their own representations. */
static inline int64_t xr_bits_exact_kernel_rotl(int64_t lhs, int64_t rhs, uint8_t native_type) {
    return xr_bits_exact_rotate_left(lhs, rhs, native_type);
}

static inline int64_t xr_bits_exact_kernel_rotr(int64_t lhs, int64_t rhs, uint8_t native_type) {
    return xr_bits_exact_rotate_right(lhs, rhs, native_type);
}

static inline int64_t xr_bits_exact_kernel_bswap(int64_t lhs, int64_t rhs, uint8_t native_type) {
    (void) rhs;
    return xr_bits_exact_byteswap(lhs, native_type);
}

static inline int64_t xr_bits_exact_kernel_popcount(int64_t lhs, int64_t rhs,
                                                    uint8_t native_type) {
    (void) rhs;
    return xr_bits_exact_popcount(lhs, native_type);
}

static inline int64_t xr_bits_exact_kernel_clz(int64_t lhs, int64_t rhs, uint8_t native_type) {
    (void) rhs;
    return xr_bits_exact_leading_zeros(lhs, native_type);
}

static inline int64_t xr_bits_exact_kernel_ctz(int64_t lhs, int64_t rhs, uint8_t native_type) {
    (void) rhs;
    return xr_bits_exact_trailing_zeros(lhs, native_type);
}

static inline int64_t xr_bits_exact_kernel_mul_high(int64_t lhs, int64_t rhs,
                                                    uint8_t native_type) {
    return xr_bits_exact_mul_high(lhs, rhs, native_type);
}

#define XR_BITS_EXACT_OWNER_GUARD(owner_hi, owner_lo)                                             \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_bits                                                 \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_BITS_HI &&                        \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_BITS_LO)                          \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_BITS_EXACT_CONSUMER_GUARD(consumer_bit)                                                \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_bits                                    \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&           \
                (XR_SEM_OWNER_ID_SHARED_BITS_CONSUMERS & (uint32_t) (consumer_bit)) != 0)         \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_BITS_EXACT_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, kernel, lhs, rhs, native_type) \
    (XR_BITS_EXACT_OWNER_GUARD((owner_hi), (owner_lo)),                                            \
     XR_BITS_EXACT_CONSUMER_GUARD((consumer_bit)),                                                 \
     (kernel)((int64_t) (lhs), (int64_t) (rhs), (uint8_t) (native_type)))

#define XR_BITS_NOT_OWNER_GUARD(owner_hi, owner_lo)                                                \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_bits_not                                             \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_BITS_NOT_HI &&                    \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_BITS_NOT_LO)                      \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_BITS_NOT_CONSUMER_GUARD(consumer_bit)                                                  \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_bits_not                                \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&           \
                (XR_SEM_OWNER_ID_SHARED_BITS_NOT_CONSUMERS & (uint32_t) (consumer_bit)) != 0)     \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_BITS_NOT_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, value)                           \
    (XR_BITS_NOT_OWNER_GUARD((owner_hi), (owner_lo)),                                              \
     XR_BITS_NOT_CONSUMER_GUARD((consumer_bit)), xr_bits_not_i64((int64_t) (value)))

#define XR_SHIFT_OWNER_GUARD(owner_hi, owner_lo)                                                   \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_shift                                                \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_SHIFT_HI &&                       \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_SHIFT_LO)                         \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_SHIFT_CONSUMER_GUARD(consumer_bit)                                                     \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_shift                                   \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&           \
                (XR_SEM_OWNER_ID_SHARED_SHIFT_CONSUMERS & (uint32_t) (consumer_bit)) != 0)        \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_SHIFT_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, kind, value, count)                 \
    (XR_SHIFT_OWNER_GUARD((owner_hi), (owner_lo)),                                                 \
     XR_SHIFT_CONSUMER_GUARD((consumer_bit)),                                                      \
     xr_shift_i64((XrShiftKind) (kind), (int64_t) (value), (int64_t) (count)))

#define XR_SHIFT_BIGINT_OWNER_PLAN(owner_hi, owner_lo, consumer_bit, kind, source_len,             \
                                   source_is_zero, count)                                          \
    (XR_SHIFT_OWNER_GUARD((owner_hi), (owner_lo)),                                                 \
     XR_SHIFT_CONSUMER_GUARD((consumer_bit)),                                                      \
     xr_shift_bigint_plan((XrShiftKind) (kind), (uint32_t) (source_len),                           \
                          (source_is_zero), (int64_t) (count)))

#define XR_SHIFT_BIGINT_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, plan, source, source_len,    \
                                    source_sign, result, result_sign)                              \
    (XR_SHIFT_OWNER_GUARD((owner_hi), (owner_lo)),                                                 \
     XR_SHIFT_CONSUMER_GUARD((consumer_bit)),                                                      \
     xr_shift_bigint_apply((plan), (source), (uint32_t) (source_len), (int8_t) (source_sign),      \
                           (result), (result_sign)))

#endif  // XRAY_SHARED_XR_BITS_CORE_H
