/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_float_arithmetic.inc.c - Integer-only correctly rounded arithmetic
 *
 * KEY CONCEPT:
 *   Keep discarded information below guard bits until one final rounding.
 */
static uint64_t float_shift_jam(uint64_t value, unsigned shift) {
    if (!shift) return value;
    if (shift >= 64) return value != 0;
    return (value >> shift) | ((value & ((UINT64_C(1) << shift) - 1)) != 0);
}
static void float_normalize(const FloatFormat *format, FloatParts *parts) {
    if (!parts->significand) return;
    while (!(parts->significand & (UINT64_C(1) << format->fraction))) {
        parts->significand <<= 1;
        --parts->exponent;
    }
}
static FloatParts float_sum(FloatParts a, FloatParts b) {
    if (!a.significand && !b.significand) {
        a.negative = a.negative && b.negative;
        return a;
    }
    if (!a.significand) return b;
    if (!b.significand) return a;
    if (a.exponent < b.exponent || (a.exponent == b.exponent && a.significand < b.significand)) {
        FloatParts swap = a; a = b; b = swap;
    }
    uint64_t first = a.significand << 4;
    uint64_t second = float_shift_jam(b.significand << 4, (unsigned) (a.exponent - b.exponent));
    a.significand = a.negative == b.negative ? first + second : first - second;
    a.exponent -= 4;
    if (!a.significand) a.negative = false;
    return a;
}
static FloatParts float_product(FloatParts a, FloatParts b) {
    uint64_t low_a = (uint32_t) a.significand, high_a = a.significand >> 32;
    uint64_t low_b = (uint32_t) b.significand, high_b = b.significand >> 32;
    uint64_t low = low_a * low_b;
    uint64_t middle = high_a * low_b + (low >> 32);
    uint64_t carry = middle >> 32;
    middle = (uint32_t) middle + low_a * high_b;
    uint64_t high = high_a * high_b + carry + (middle >> 32);
    low = (middle << 32) | (uint32_t) low;
    a.exponent += b.exponent;
    a.negative = a.negative != b.negative;
    if (high) {
        unsigned shift = 1;
        for (uint64_t rest = high; rest; rest >>= 1) ++shift;
        /* A 106-bit product retains 63 bits: at least ten guard bits for f64. */
        a.significand = (high << (64 - shift)) | (low >> shift) |
            ((low & ((UINT64_C(1) << shift) - 1)) != 0);
        a.exponent += (int) shift;
    } else a.significand = low;
    return a;
}
static FloatParts float_quotient(const FloatFormat *format, FloatParts a, FloatParts b) {
    uint64_t quotient = a.significand / b.significand;
    uint64_t remainder = a.significand % b.significand;
    unsigned scale = format->fraction + 4;
    for (unsigned i = 0; i < scale; ++i) {
        remainder <<= 1;
        quotient <<= 1;
        if (remainder >= b.significand) { remainder -= b.significand; quotient |= 1; }
    }
    a.significand = quotient | (remainder != 0);
    a.exponent -= b.exponent + (int) scale;
    a.negative = a.negative != b.negative;
    return a;
}
XR_FUNC XrXirNumericStatus xr_xir_float_arithmetic(uint32_t bits, XrXirFloatOperation operation,
    uint64_t left, uint64_t right, uint64_t *output) {
    if (!output) return XR_XIR_NUMERIC_BAD_ARGUMENT;
    *output = 0;
    FloatFormat format; FloatParts a, b;
    if (operation < XR_XIR_FLOAT_ADD || operation > XR_XIR_FLOAT_DIVIDE ||
        !float_format(bits, &format) || !float_decode(bits, left, &a) || !float_decode(bits, right, &b))
        return XR_XIR_NUMERIC_BAD_ARGUMENT;
    if (operation == XR_XIR_FLOAT_SUBTRACT) { b.negative = !b.negative; operation = XR_XIR_FLOAT_ADD; }
    bool negative = a.negative != b.negative;
    uint64_t sign = negative ? format.sign : 0;
    if (a.nan || b.nan) *output = format.nan;
    else if (operation == XR_XIR_FLOAT_ADD && (a.infinite || b.infinite)) {
        *output = a.infinite && b.infinite && negative ? format.nan :
            format.infinity | ((a.infinite ? a.negative : b.negative) ? format.sign : 0);
    } else if (operation == XR_XIR_FLOAT_MULTIPLY && (a.infinite || b.infinite)) {
        *output = (!a.infinite && !a.significand) || (!b.infinite && !b.significand) ?
            format.nan : sign | format.infinity;
    } else if (operation == XR_XIR_FLOAT_DIVIDE && (a.infinite || b.infinite || !b.significand)) {
        if ((a.infinite && b.infinite) || (!a.infinite && !a.significand && !b.infinite && !b.significand))
            *output = format.nan;
        else *output = b.infinite ? sign : sign | format.infinity;
    } else {
        float_normalize(&format, &a); float_normalize(&format, &b);
        FloatParts result = operation == XR_XIR_FLOAT_ADD ? float_sum(a, b) :
            operation == XR_XIR_FLOAT_MULTIPLY ? float_product(a, b) : float_quotient(&format, a, b);
        *output = float_pack(&format, result);
    }
    return XR_XIR_NUMERIC_OK;
}
XR_FUNC XrXirRunStatus xr_xir_float_binary(XrXirType type, XrXirFloatOperation operation,
    int64_t left, int64_t right, int64_t *output) {
    if (!output) return XR_XIR_RUN_BAD_ARGUMENT;
    *output = 0;
    if (!xr_xir_float_payload_valid(type, left) || !xr_xir_float_payload_valid(type, right))
        return XR_XIR_RUN_BAD_ARGUMENT;
    uint64_t bits = 0;
    XrXirNumericStatus status = xr_xir_float_arithmetic(xr_xir_float_bits(type), operation,
        (uint64_t) left, (uint64_t) right, &bits);
    if (status != XR_XIR_NUMERIC_OK) return XR_XIR_RUN_BAD_ARGUMENT;
    memcpy(output, &bits, sizeof(bits));
    return XR_XIR_RUN_OK;
}
