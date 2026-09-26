/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_float.c - Integer-only IEEE conversion and ordering
 *
 * KEY CONCEPT:
 *   A finite value is a signed integer significand times a power of two.
 */
#include "xxir_float.h"

typedef struct FloatFormat {
    unsigned fraction;
    int bias;
    uint64_t sign, infinity, nan;
} FloatFormat;
typedef struct FloatParts {
    uint64_t significand;
    int exponent;
    bool negative, infinite, nan;
} FloatParts;
static bool float_format(uint32_t bits, FloatFormat *format) {
    if (bits == 32) *format = (FloatFormat) {23, 127, UINT64_C(0x80000000),
        UINT64_C(0x7f800000), UINT64_C(0x7fc00000)};
    else if (bits == 64) *format = (FloatFormat) {52, 1023, UINT64_C(0x8000000000000000),
        UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff8000000000000)};
    else return false;
    return true;
}
static bool float_decode(uint32_t bits, uint64_t input, FloatParts *parts) {
    FloatFormat format;
    if (!float_format(bits, &format) || (bits == 32 && input > UINT32_MAX)) return false;
    uint64_t fraction = input & ((UINT64_C(1) << format.fraction) - 1);
    unsigned exponent = (unsigned) ((input & format.infinity) >> format.fraction);
    bool special = (input & format.infinity) == format.infinity;
    *parts = (FloatParts) {fraction, (exponent ? (int) exponent : 1) - format.bias - (int) format.fraction,
        (input & format.sign) != 0, special && !fraction, special && fraction != 0};
    if (exponent && !special) parts->significand |= UINT64_C(1) << format.fraction;
    return true;
}
static uint64_t float_round_right(uint64_t value, unsigned shift) {
    if (!shift) return value;
    if (shift > 64) return 0;
    if (shift == 64) return value > (UINT64_C(1) << 63);
    uint64_t whole = value >> shift;
    uint64_t tail = value & ((UINT64_C(1) << shift) - 1);
    uint64_t half = UINT64_C(1) << (shift - 1);
    return whole + (tail > half || (tail == half && (whole & 1)));
}
static uint64_t float_pack(const FloatFormat *format, FloatParts parts) {
    uint64_t sign = parts.negative ? format->sign : 0;
    if (parts.nan) return format->nan;
    if (parts.infinite) return sign | format->infinity;
    if (!parts.significand) return sign;
    int high = 0;
    for (uint64_t rest = parts.significand >> 1; rest; rest >>= 1) ++high;
    int exponent = parts.exponent + high;
    uint64_t rounded;
    if (exponent < 1 - format->bias) {
        int shift = 1 - format->bias - (int) format->fraction - parts.exponent;
        rounded = shift > 0 ? float_round_right(parts.significand, (unsigned) shift) :
            parts.significand << (unsigned) -shift;
        return sign | rounded;
    }
    int shift = high - (int) format->fraction;
    rounded = shift > 0 ? float_round_right(parts.significand, (unsigned) shift) :
        parts.significand << (unsigned) -shift;
    if (rounded == (UINT64_C(1) << (format->fraction + 1))) { rounded >>= 1; ++exponent; }
    if (exponent > format->bias) return sign | format->infinity;
    uint64_t fraction = rounded & ((UINT64_C(1) << format->fraction) - 1);
    return sign | ((uint64_t) (exponent + format->bias) << format->fraction) | fraction;
}
XrXirNumericStatus xr_xir_float_convert(uint32_t source_bits, uint32_t target_bits,
    uint64_t input, uint64_t *output) {
    if (!output) return XR_XIR_NUMERIC_BAD_ARGUMENT;
    *output = 0;
    FloatFormat target; FloatParts parts;
    if (!float_format(target_bits, &target) || !float_decode(source_bits, input, &parts)) return XR_XIR_NUMERIC_BAD_ARGUMENT;
    *output = float_pack(&target, parts); return XR_XIR_NUMERIC_OK;
}
XrXirNumericStatus xr_xir_integer_to_float(XrXirIntegerFormat source,
    uint32_t target_bits, int64_t input, uint64_t *output) {
    if (!output) return XR_XIR_NUMERIC_BAD_ARGUMENT;
    *output = 0;
    FloatFormat target; int64_t validated;
    if (!float_format(target_bits, &target) || xr_xir_integer_convert(source, source, input, &validated) != XR_XIR_RUN_OK)
        return XR_XIR_NUMERIC_BAD_ARGUMENT;
    bool negative = source.is_signed && validated < 0;
    uint64_t magnitude = negative ? UINT64_C(0) - (uint64_t) validated : (uint64_t) validated;
    *output = float_pack(&target, (FloatParts) {magnitude, 0, negative, false, false});
    return XR_XIR_NUMERIC_OK;
}
XrXirNumericStatus xr_xir_float_to_integer(uint32_t source_bits,
    XrXirIntegerFormat target, uint64_t input, int64_t *output) {
    if (!output) return XR_XIR_NUMERIC_BAD_ARGUMENT;
    *output = 0;
    FloatParts parts; int64_t validated;
    if (!float_decode(source_bits, input, &parts) || xr_xir_integer_convert(target, target, 0, &validated) != XR_XIR_RUN_OK)
        return XR_XIR_NUMERIC_BAD_ARGUMENT;
    if (parts.nan || parts.infinite) return XR_XIR_NUMERIC_RANGE;
    uint64_t magnitude;
    if (parts.exponent >= 0) {
        if (parts.exponent >= 64 || parts.significand > (UINT64_MAX >> (unsigned) parts.exponent)) return XR_XIR_NUMERIC_RANGE;
        magnitude = parts.significand << (unsigned) parts.exponent;
    } else magnitude = parts.exponent <= -64 ? 0 : parts.significand >> (unsigned) -parts.exponent;
    uint64_t limit = target.is_signed ? (UINT64_C(1) << (target.bits - 1)) - (parts.negative ? 0 : 1) :
        UINT64_MAX >> (64 - target.bits);
    if (magnitude > limit || (!target.is_signed && parts.negative && magnitude)) return XR_XIR_NUMERIC_RANGE;
    uint64_t bits = parts.negative ? UINT64_C(0) - magnitude : magnitude;
    *output = bits <= INT64_MAX ? (int64_t) bits : -1 - (int64_t) (UINT64_MAX - bits);
    return XR_XIR_NUMERIC_OK;
}
XrXirNumericStatus xr_xir_float_compare(uint32_t bits,
    uint64_t left, uint64_t right, XrXirFloatOrder *output) {
    if (!output) return XR_XIR_NUMERIC_BAD_ARGUMENT;
    *output = XR_XIR_FLOAT_EQUAL;
    FloatParts first, second; FloatFormat format;
    if (!float_decode(bits, left, &first) || !float_decode(bits, right, &second) || !float_format(bits, &format))
        return XR_XIR_NUMERIC_BAD_ARGUMENT;
    if (first.nan || second.nan) { *output = XR_XIR_FLOAT_UNORDERED; return XR_XIR_NUMERIC_OK; }
    uint64_t a = left & ~format.sign, b = right & ~format.sign;
    if (left == right || (!a && !b)) return XR_XIR_NUMERIC_OK;
    bool less = first.negative != second.negative ? first.negative : (first.negative ? a > b : a < b);
    *output = less ? XR_XIR_FLOAT_LESS : XR_XIR_FLOAT_GREATER; return XR_XIR_NUMERIC_OK;
}
XrXirNumericStatus xr_xir_float_negate(uint32_t bits, uint64_t input, uint64_t *output) {
    if (!output) return XR_XIR_NUMERIC_BAD_ARGUMENT;
    *output = 0;
    FloatParts parts; FloatFormat format;
    if (!float_decode(bits, input, &parts) || !float_format(bits, &format)) return XR_XIR_NUMERIC_BAD_ARGUMENT;
    *output = parts.nan ? format.nan : input ^ format.sign; return XR_XIR_NUMERIC_OK;
}

static XrXirRunStatus floating_result(XrXirNumericStatus status) {
    return status == XR_XIR_NUMERIC_OK ? XR_XIR_RUN_OK :
        status == XR_XIR_NUMERIC_RANGE ? XR_XIR_RUN_NUMERIC_RANGE : XR_XIR_RUN_BAD_ARGUMENT;
}
XrXirRunStatus xr_xir_number_convert(XrXirType source, XrXirType target, int64_t input, int64_t *output) {
    if (!output) return XR_XIR_RUN_BAD_ARGUMENT;
    *output = 0;
    if (!xr_xir_type_is_number(source) || !xr_xir_type_is_number(target)) return XR_XIR_RUN_BAD_ARGUMENT;
    if (xr_xir_float_bits(source) && !xr_xir_float_payload_valid(source, input)) return XR_XIR_RUN_BAD_ARGUMENT;
    if (!xr_xir_float_bits(source) && !xr_xir_float_bits(target))
        return xr_xir_integer_convert(xr_xir_integer_format(source), xr_xir_integer_format(target), input, output);
    if (!xr_xir_float_bits(target)) return floating_result(xr_xir_float_to_integer(
        xr_xir_float_bits(source), xr_xir_integer_format(target), (uint64_t) input, output));
    uint64_t bits = 0;
    XrXirNumericStatus status = xr_xir_float_bits(source) ?
        xr_xir_float_convert(xr_xir_float_bits(source), xr_xir_float_bits(target), (uint64_t) input, &bits) :
        xr_xir_integer_to_float(xr_xir_integer_format(source), xr_xir_float_bits(target), input, &bits);
    memcpy(output, &bits, sizeof(bits));
    return floating_result(status);
}
XrXirRunStatus xr_xir_float_relation(XrXirType type, XrXirFloatRelation relation,
    int64_t left, int64_t right, int64_t *output) {
    if (!output) return XR_XIR_RUN_BAD_ARGUMENT;
    *output = 0;
    if (!xr_xir_float_payload_valid(type, left) || !xr_xir_float_payload_valid(type, right) ||
        relation < XR_XIR_FLOAT_EQ || relation > XR_XIR_FLOAT_GE) return XR_XIR_RUN_BAD_ARGUMENT;
    XrXirFloatOrder order;
    XrXirNumericStatus status = xr_xir_float_compare(xr_xir_float_bits(type), (uint64_t) left, (uint64_t) right, &order);
    if (status != XR_XIR_NUMERIC_OK) return floating_result(status);
    if (order == XR_XIR_FLOAT_UNORDERED) { *output = relation == XR_XIR_FLOAT_NE; return XR_XIR_RUN_OK; }
    switch (relation) {
    case XR_XIR_FLOAT_EQ: *output = order == 0; break;
    case XR_XIR_FLOAT_NE: *output = order != 0; break;
    case XR_XIR_FLOAT_LT: *output = order < 0; break;
    case XR_XIR_FLOAT_LE: *output = order <= 0; break;
    case XR_XIR_FLOAT_GT: *output = order > 0; break;
    case XR_XIR_FLOAT_GE: *output = order >= 0; break;
    }
    return XR_XIR_RUN_OK;
}
XrXirRunStatus xr_xir_float_negative(XrXirType type, int64_t input, int64_t *output) {
    if (!output) return XR_XIR_RUN_BAD_ARGUMENT;
    *output = 0;
    if (!xr_xir_float_payload_valid(type, input)) return XR_XIR_RUN_BAD_ARGUMENT;
    uint64_t bits = 0;
    XrXirNumericStatus status = xr_xir_float_negate(xr_xir_float_bits(type), (uint64_t) input, &bits);
    memcpy(output, &bits, sizeof(bits));
    return floating_result(status);
}
