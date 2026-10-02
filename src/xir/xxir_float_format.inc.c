/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_float_format.inc.c - Shortest decimal text from exact rounding intervals
 *
 * KEY CONCEPT:
 *   Integer margins preserve asymmetric spacing at normal powers of two.
 */
#include "../shared/xr_decimal_float.h"

static XrDecimalInteger float_decimal_integer(uint64_t value) {
    XrDecimalInteger result = {{0}, 0};
    if (value) { result.words[0] = (uint32_t) value; result.count = 1; }
    if (value >> 32) { result.words[1] = (uint32_t) (value >> 32); result.count = 2; }
    return result;
}
static bool float_decimal_add(XrDecimalInteger *a, const XrDecimalInteger *b) {
    unsigned count = a->count > b->count ? a->count : b->count;
    uint64_t carry = 0;
    for (unsigned i = 0; i < count; ++i) {
        carry += (uint64_t) (i < a->count ? a->words[i] : 0) + (i < b->count ? b->words[i] : 0);
        a->words[i] = (uint32_t) carry; carry >>= 32;
    }
    a->count = count;
    if (carry) {
        if (count == XR_DECIMAL_LIMBS) return false;
        a->words[a->count++] = (uint32_t) carry;
    }
    return true;
}
static void float_decimal_carry(char *digits, unsigned *count, int *exponent) {
    unsigned index = *count;
    while (index && digits[index - 1] == '9') digits[--index] = '0';
    if (index) ++digits[index - 1];
    else { digits[0] = '1'; *count = 1; ++*exponent; }
}
static bool float_decimal_digits(const FloatFormat *format, FloatParts parts,
    char *digits, unsigned *count, int *exponent) {
    XrDecimalWork work = xr_decimal_runtime_work();
    bool inclusive = !(parts.significand & 1);
    bool asymmetric = parts.significand == (UINT64_C(1) << format->fraction) &&
        parts.exponent > 1 - format->bias - (int) format->fraction;
    XrDecimalInteger numerator = float_decimal_integer(parts.significand << 2);
    XrDecimalInteger denominator = float_decimal_integer(4);
    XrDecimalInteger lower = float_decimal_integer(asymmetric ? 1 : 2);
    XrDecimalInteger upper = float_decimal_integer(2);
    if (parts.exponent < 0) {
        if (!xr_decimal_shift(&work, &denominator, (unsigned) -parts.exponent)) return false;
    } else if (!xr_decimal_shift(&work, &numerator, (unsigned) parts.exponent) ||
        !xr_decimal_shift(&work, &lower, (unsigned) parts.exponent) ||
        !xr_decimal_shift(&work, &upper, (unsigned) parts.exponent)) return false;
    *exponent = 0; *count = 0;
    while (xr_decimal_compare(&work, &numerator, &denominator) < 0) {
        if (--*exponent < -324 || !xr_decimal_multiply(&work, &numerator, 10, 0) ||
            !xr_decimal_multiply(&work, &lower, 10, 0) || !xr_decimal_multiply(&work, &upper, 10, 0)) return false;
    }
    for (;;) {
        XrDecimalInteger trial = denominator;
        if (!xr_decimal_multiply(&work, &trial, 10, 0)) return false;
        if (xr_decimal_compare(&work, &numerator, &trial) < 0) break;
        if (++*exponent > 308) return false;
        denominator = trial;
    }
    /* Binary64 needs at most 17 digits. Even a conservative margin bound
     * occupies fewer than 1200 bits, within the shared 6144-bit workspace. */
    unsigned limit = format->fraction == 23 ? 9 : 17;
    while (*count < limit) {
        unsigned digit = 0;
        while (xr_decimal_compare(&work, &numerator, &denominator) >= 0) {
            xr_decimal_subtract(&work, &numerator, &denominator);
            if (++digit > 9) return false;
        }
        digits[(*count)++] = (char) ('0' + digit);
        int below = xr_decimal_compare(&work, &numerator, &lower);
        XrDecimalInteger above = numerator;
        if (!float_decimal_add(&above, &upper)) return false;
        int beyond = xr_decimal_compare(&work, &above, &denominator);
        bool down = below < 0 || (inclusive && !below);
        bool up = beyond > 0 || (inclusive && !beyond);
        if (down || up) {
            XrDecimalInteger twice = numerator;
            if (!xr_decimal_shift(&work, &twice, 1)) return false;
            int distance_order = xr_decimal_compare(&work, &twice, &denominator);
            if (up && (!down || distance_order > 0 || (!distance_order && (digit & 1))))
                float_decimal_carry(digits, count, exponent);
            while (*count > 1 && digits[*count - 1] == '0') --*count;
            return true;
        }
        if (!xr_decimal_multiply(&work, &numerator, 10, 0) || !xr_decimal_multiply(&work, &lower, 10, 0) ||
            !xr_decimal_multiply(&work, &upper, 10, 0)) return false;
    }
    return false;
}
static size_t float_decimal_text(char *text, const char *digits, unsigned count, int exponent) {
    size_t used = 0;
    if (exponent >= -4 && exponent < 16) {
        int point = exponent + 1;
        if (point <= 0) {
            text[used++] = '0'; text[used++] = '.';
            for (int i = 0; i < -point; ++i) text[used++] = '0';
            for (unsigned i = 0; i < count; ++i) text[used++] = digits[i];
        } else {
            for (int i = 0; i < point; ++i) text[used++] = (unsigned) i < count ? digits[i] : '0';
            text[used++] = '.';
            if ((unsigned) point >= count) text[used++] = '0';
            else for (unsigned i = (unsigned) point; i < count; ++i) text[used++] = digits[i];
        }
    } else {
        text[used++] = digits[0];
        if (count > 1) {
            text[used++] = '.';
            for (unsigned i = 1; i < count; ++i) text[used++] = digits[i];
        }
        text[used++] = 'e'; text[used++] = exponent < 0 ? '-' : '+';
        unsigned magnitude = (unsigned) (exponent < 0 ? -exponent : exponent);
        char reversed[3]; unsigned size = 0;
        do { reversed[size++] = (char) ('0' + magnitude % 10); magnitude /= 10; } while (magnitude);
        while (size) text[used++] = reversed[--size];
    }
    return used;
}
XR_FUNC bool xr_xir_float_format(uint32_t bits, uint64_t input,
    char *bytes, size_t capacity, size_t *length) {
    if (length) *length = 0;
    if (bytes && capacity) bytes[0] = 0;
    if (!bytes || !length) return false;
    FloatFormat format; FloatParts parts;
    if (!float_format(bits, &format) || !float_decode(bits, input, &parts)) return false;
    char text[32]; size_t used = 0;
    if (parts.nan) { memcpy(text, "nan", 3); used = 3; }
    else {
        if (parts.negative) text[used++] = '-';
        if (parts.infinite) { memcpy(text + used, "inf", 3); used += 3; }
        else if (!parts.significand) { memcpy(text + used, "0.0", 3); used += 3; }
        else {
            char digits[17]; unsigned count = 0; int exponent = 0;
            if (!float_decimal_digits(&format, parts, digits, &count, &exponent)) return false;
            used += float_decimal_text(text + used, digits, count, exponent);
        }
    }
    if (used >= capacity) return false;
    memcpy(bytes, text, used); bytes[used] = 0; *length = used;
    return true;
}
