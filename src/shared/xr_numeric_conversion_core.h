/*
 * xray - Deterministic scalar numeric conversion primitives.
 *
 * These helpers define language semantics without relying on implementation-
 * defined signed casts or the process floating-point rounding mode.  VM and
 * generated AOT C both consume this file.
 */

#ifndef XR_NUMERIC_CONVERSION_CORE_H
#define XR_NUMERIC_CONVERSION_CORE_H

#include "xr_native_type_core.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_NUMERIC_CANONICAL_F32_NAN UINT32_C(0x7fc00000)

/* Copy object representations without requiring a hosted C library.  Character
 * accesses are alias-safe in both C and C++, which keeps this the single
 * bit-cast primitive shared by the VM and freestanding generated C. */
static inline void xr_numeric_bit_copy(void *destination, const void *source, size_t byte_count) {
    unsigned char *dst = (unsigned char *) destination;
    const unsigned char *src = (const unsigned char *) source;
    for (size_t index = 0; index < byte_count; index++)
        dst[index] = src[index];
}

static inline uint8_t xr_numeric_scalar_bit_width(uint8_t scalar_rep, uint8_t pointer_bits) {
    switch ((XrNativeType) scalar_rep) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
            return 8;
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
            return 16;
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
        case XR_NATIVE_F32:
            return 32;
        case XR_NATIVE_I64:
        case XR_NATIVE_U64:
        case XR_NATIVE_F64:
            return 64;
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
            return pointer_bits;
        default:
            return 0;
    }
}

static inline bool xr_numeric_scalar_is_signed_int(uint8_t scalar_rep) {
    return scalar_rep == XR_NATIVE_I8 || scalar_rep == XR_NATIVE_I16 ||
           scalar_rep == XR_NATIVE_I32 || scalar_rep == XR_NATIVE_I64 ||
           scalar_rep == XR_NATIVE_ISIZE;
}

static inline uint64_t xr_numeric_mask_for_bits(uint8_t bits) {
    return bits >= 64 ? UINT64_MAX : ((UINT64_C(1) << bits) - UINT64_C(1));
}

static inline int64_t xr_numeric_i64_from_bits(uint64_t bits) {
    int64_t value;
    xr_numeric_bit_copy(&value, &bits, sizeof(value));
    return value;
}

static inline uint64_t xr_numeric_i64_to_bits(int64_t value) {
    uint64_t bits;
    xr_numeric_bit_copy(&bits, &value, sizeof(bits));
    return bits;
}

/* Integer conversions are modulo 2^N followed by an explicit two's-complement
 * interpretation for signed targets.  The returned int64_t is only a VM/AOT
 * storage lane; unsigned u64 values retain their exact bits in that lane. */
static inline int64_t xr_numeric_int_convert_i64(int64_t raw, uint8_t source_rep,
                                                 uint8_t target_rep, uint8_t pointer_bits) {
    uint8_t source_bits = xr_numeric_scalar_bit_width(source_rep, pointer_bits);
    uint8_t target_bits = xr_numeric_scalar_bit_width(target_rep, pointer_bits);
    if (source_bits == 0 || target_bits == 0)
        return raw;

    uint64_t bits = xr_numeric_i64_to_bits(raw) & xr_numeric_mask_for_bits(source_bits);
    if (xr_numeric_scalar_is_signed_int(source_rep) && source_bits < 64 &&
        (bits & (UINT64_C(1) << (source_bits - 1))) != 0)
        bits |= ~xr_numeric_mask_for_bits(source_bits);

    bits &= xr_numeric_mask_for_bits(target_bits);
    if (xr_numeric_scalar_is_signed_int(target_rep) && target_bits < 64 &&
        (bits & (UINT64_C(1) << (target_bits - 1))) != 0)
        bits |= ~xr_numeric_mask_for_bits(target_bits);
    return xr_numeric_i64_from_bits(bits);
}

static inline int xr_numeric_u64_high_bit(uint64_t value) {
    int bit = -1;
    while (value != 0) {
        value >>= 1;
        bit++;
    }
    return bit;
}

static inline uint64_t xr_numeric_round_shift_even_u64(uint64_t value, unsigned shift) {
    if (shift == 0)
        return value;
    if (shift > 64)
        return 0;
    if (shift == 64) {
        uint64_t half = UINT64_C(1) << 63;
        return value > half ? 1 : 0;
    }
    uint64_t quotient = value >> shift;
    uint64_t mask = (UINT64_C(1) << shift) - UINT64_C(1);
    uint64_t remainder = value & mask;
    uint64_t half = UINT64_C(1) << (shift - 1);
    if (remainder > half || (remainder == half && (quotient & UINT64_C(1)) != 0))
        quotient++;
    return quotient;
}

static inline double xr_numeric_double_from_bits(uint64_t bits) {
    double value;
    xr_numeric_bit_copy(&value, &bits, sizeof(value));
    return value;
}

static inline float xr_numeric_float_from_bits(uint32_t bits) {
    float value;
    xr_numeric_bit_copy(&value, &bits, sizeof(value));
    return value;
}

static inline uint64_t xr_numeric_double_to_bits(double value) {
    uint64_t bits;
    xr_numeric_bit_copy(&bits, &value, sizeof(bits));
    return bits;
}

static inline double xr_numeric_unsigned_to_f64(uint64_t magnitude, bool negative) {
    if (magnitude == 0)
        return xr_numeric_double_from_bits(negative ? (UINT64_C(1) << 63) : 0);
    int high = xr_numeric_u64_high_bit(magnitude);
    unsigned shift = high > 52 ? (unsigned) (high - 52) : 0;
    uint64_t significand =
        shift ? xr_numeric_round_shift_even_u64(magnitude, shift) : (magnitude << (52 - high));
    if (shift && significand == (UINT64_C(1) << 53)) {
        significand >>= 1;
        high++;
    }
    if (shift)
        significand <<= (52 - high + (int) shift);
    uint64_t fraction = significand & ((UINT64_C(1) << 52) - UINT64_C(1));
    uint64_t bits = ((uint64_t) (high + 1023) << 52) | fraction;
    if (negative)
        bits |= UINT64_C(1) << 63;
    return xr_numeric_double_from_bits(bits);
}

static inline double xr_numeric_unsigned_to_f32(uint64_t magnitude, bool negative) {
    if (magnitude == 0) {
        uint32_t zero_bits = negative ? UINT32_C(0x80000000) : 0;
        return (double) xr_numeric_float_from_bits(zero_bits);
    }
    int high = xr_numeric_u64_high_bit(magnitude);
    unsigned shift = high > 23 ? (unsigned) (high - 23) : 0;
    uint64_t significand =
        shift ? xr_numeric_round_shift_even_u64(magnitude, shift) : (magnitude << (23 - high));
    if (shift && significand == (UINT64_C(1) << 24)) {
        significand >>= 1;
        high++;
    }
    if (shift)
        significand <<= (23 - high + (int) shift);
    uint32_t fraction = (uint32_t) significand & UINT32_C(0x007fffff);
    uint32_t bits = ((uint32_t) (high + 127) << 23) | fraction;
    if (negative)
        bits |= UINT32_C(0x80000000);
    return (double) xr_numeric_float_from_bits(bits);
}

static inline double xr_numeric_int_to_float(int64_t raw, uint8_t source_rep, uint8_t target_rep,
                                             uint8_t pointer_bits) {
    uint8_t source_bits = xr_numeric_scalar_bit_width(source_rep, pointer_bits);
    uint64_t mask = xr_numeric_mask_for_bits(source_bits);
    uint64_t bits = xr_numeric_i64_to_bits(raw) & mask;
    bool negative = xr_numeric_scalar_is_signed_int(source_rep) && source_bits != 0 &&
                    (bits & (UINT64_C(1) << (source_bits - 1))) != 0;
    uint64_t magnitude = negative ? ((~bits + UINT64_C(1)) & mask) : bits;
    return target_rep == XR_NATIVE_F32 ? xr_numeric_unsigned_to_f32(magnitude, negative)
                                       : xr_numeric_unsigned_to_f64(magnitude, negative);
}

/* Exact IEEE-754 binary64 -> binary32 round-to-nearest-ties-to-even with one
 * canonical quiet NaN payload.  Overflow produces signed infinity. */
static inline double xr_numeric_f64_to_f32(double source) {
    uint64_t bits = xr_numeric_double_to_bits(source);
    uint32_t sign = (uint32_t) (bits >> 32) & UINT32_C(0x80000000);
    uint32_t exponent = (uint32_t) ((bits >> 52) & UINT64_C(0x7ff));
    uint64_t fraction = bits & ((UINT64_C(1) << 52) - UINT64_C(1));
    if (exponent == UINT32_C(0x7ff)) {
        if (fraction != 0)
            return (double) xr_numeric_float_from_bits(XR_NUMERIC_CANONICAL_F32_NAN);
        return (double) xr_numeric_float_from_bits(sign | UINT32_C(0x7f800000));
    }
    if (exponent == 0 && fraction == 0)
        return (double) xr_numeric_float_from_bits(sign);

    uint64_t mantissa = exponent == 0 ? fraction : ((UINT64_C(1) << 52) | fraction);
    int binary_exponent = exponent == 0 ? -1074 : (int) exponent - 1023 - 52;
    int high = xr_numeric_u64_high_bit(mantissa);
    int f32_exponent = high + binary_exponent;
    if (f32_exponent > 127)
        return (double) xr_numeric_float_from_bits(sign | UINT32_C(0x7f800000));

    uint32_t result_bits;
    if (f32_exponent >= -126) {
        int shift = high - 23;
        uint64_t significand = shift > 0
                                   ? xr_numeric_round_shift_even_u64(mantissa, (unsigned) shift)
                                   : mantissa << (unsigned) (-shift);
        if (significand == (UINT64_C(1) << 24)) {
            significand >>= 1;
            f32_exponent++;
            if (f32_exponent > 127)
                return (double) xr_numeric_float_from_bits(sign | UINT32_C(0x7f800000));
        }
        result_bits = sign | ((uint32_t) (f32_exponent + 127) << 23) |
                      ((uint32_t) significand & UINT32_C(0x007fffff));
    } else {
        int shift = -(binary_exponent + 149);
        uint64_t subnormal = shift > 0 ? xr_numeric_round_shift_even_u64(mantissa, (unsigned) shift)
                                       : mantissa << (unsigned) (-shift);
        if (subnormal >= (UINT64_C(1) << 23))
            result_bits = sign | UINT32_C(0x00800000);
        else
            result_bits = sign | (uint32_t) subnormal;
    }
    return (double) xr_numeric_float_from_bits(result_bits);
}

static inline double xr_numeric_float_convert(double source, uint8_t target_rep) {
    return target_rep == XR_NATIVE_F32 ? xr_numeric_f64_to_f32(source) : source;
}

static inline double xr_numeric_power_of_two(unsigned exponent) {
    return xr_numeric_double_from_bits((uint64_t) (exponent + 1023u) << 52);
}

/* Returns false for NaN, infinity, or a toward-zero result outside the target
 * integer domain.  No floating-to-integer cast executes before the range proof. */
static inline bool xr_numeric_float_to_int(double source, uint8_t target_rep, uint8_t pointer_bits,
                                           int64_t *out) {
    uint64_t source_bits = xr_numeric_double_to_bits(source);
    if (((source_bits >> 52) & UINT64_C(0x7ff)) == UINT64_C(0x7ff) || !out)
        return false;
    uint8_t bits = xr_numeric_scalar_bit_width(target_rep, pointer_bits);
    if (bits == 0)
        return false;

    uint64_t raw;
    if (xr_numeric_scalar_is_signed_int(target_rep)) {
        double limit = xr_numeric_power_of_two((unsigned) bits - 1u);
        double lower = bits == 64 ? -limit : -limit - 1.0;
        if (!(source > lower && source < limit) && !(bits == 64 && source == -limit))
            return false;
        int64_t signed_value = (int64_t) source;
        raw = xr_numeric_i64_to_bits(signed_value);
    } else {
        double limit = xr_numeric_power_of_two(bits);
        if (!(source > -1.0 && source < limit))
            return false;
        raw = source < 0.0 ? 0 : (uint64_t) source;
    }
    *out = xr_numeric_int_convert_i64(xr_numeric_i64_from_bits(raw), target_rep, target_rep,
                                      pointer_bits);
    return true;
}

#endif /* XR_NUMERIC_CONVERSION_CORE_H */
