/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_decimal_float.h - Exact decimal to IEEE binary conversion
 *
 * KEY CONCEPT:
 *   A bounded integer ratio is rounded once at the requested binary precision.
 */
#ifndef XR_DECIMAL_FLOAT_H
#define XR_DECIMAL_FLOAT_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>

#define XR_DECIMAL_LIMBS 192u
#define XR_DECIMAL_DIGITS 1152u
typedef struct XrDecimalInteger { uint32_t words[XR_DECIMAL_LIMBS]; unsigned count; } XrDecimalInteger;
typedef struct XrDecimalInput {
    XrDecimalInteger coefficient;
    int64_t point;
    unsigned digits;
    bool negative, sticky;
} XrDecimalInput;

static inline void xr_decimal_trim(XrDecimalInteger *n) {
    while (n->count && !n->words[n->count - 1]) --n->count;
}
static inline bool xr_decimal_multiply(XrDecimalInteger *n, uint32_t factor, uint32_t add) {
    uint64_t carry = add;
    for (unsigned i = 0; i < n->count; ++i) {
        carry += (uint64_t) n->words[i] * factor;
        n->words[i] = (uint32_t) carry; carry >>= 32;
    }
    if (carry) {
        if (n->count == XR_DECIMAL_LIMBS) return false;
        n->words[n->count++] = (uint32_t) carry;
    }
    return true;
}
static inline unsigned xr_decimal_bits(const XrDecimalInteger *n) {
    if (!n->count) return 0;
    unsigned bits = (n->count - 1) * 32;
    for (uint32_t high = n->words[n->count - 1]; high; high >>= 1) ++bits;
    return bits;
}
static inline int xr_decimal_compare(const XrDecimalInteger *a, const XrDecimalInteger *b) {
    if (a->count != b->count) return a->count < b->count ? -1 : 1;
    for (unsigned i = a->count; i > 0; --i)
        if (a->words[i - 1] != b->words[i - 1]) return a->words[i - 1] < b->words[i - 1] ? -1 : 1;
    return 0;
}
static inline bool xr_decimal_shift(XrDecimalInteger *n, unsigned shift) {
    if (!n->count || !shift) return true;
    unsigned whole = shift / 32, part = shift % 32;
    if (whole >= XR_DECIMAL_LIMBS || n->count > XR_DECIMAL_LIMBS - whole - (part != 0)) return false;
    unsigned old = n->count;
    if (part) {
        uint32_t carry = 0;
        for (unsigned i = 0; i < old; ++i) {
            uint64_t value = ((uint64_t) n->words[i] << part) | carry;
            n->words[i] = (uint32_t) value; carry = (uint32_t) (value >> 32);
        }
        if (carry) n->words[n->count++] = carry;
    }
    for (unsigned i = n->count; i > 0; --i) n->words[i - 1 + whole] = n->words[i - 1];
    memset(n->words, 0, whole * sizeof(n->words[0])); n->count += whole;
    return true;
}
static inline void xr_decimal_subtract(XrDecimalInteger *a, const XrDecimalInteger *b) {
    uint64_t borrow = 0;
    for (unsigned i = 0; i < a->count; ++i) {
        uint64_t sub = (i < b->count ? b->words[i] : 0) + borrow;
        uint64_t current = a->words[i];
        a->words[i] = (uint32_t) (current - sub); borrow = current < sub;
    }
    xr_decimal_trim(a);
}
static inline bool xr_decimal_digit(char c) { return c >= '0' && c <= '9'; }

/* Every binary64 rounding boundary has fewer than 770 significant decimal
 * digits: its denominator divides 2^1075 and its significand has at most 54
 * bits. Keeping 1152 digits therefore loses only a tie-breaking sticky tail.
 * All remaining characters are still scanned and validated. */
static inline bool xr_decimal_scan(const char *text, size_t length, XrDecimalInput *out) {
    if (!text || !length || length > INT32_MAX) return false;
    *out = (XrDecimalInput) {0};
    size_t at = 0; bool point = false, seen = false, nonzero = false;
    int64_t before = 0, leading = 0;
    if (text[at] == '+' || text[at] == '-') out->negative = text[at++] == '-';
    for (; at < length && text[at] != 'e' && text[at] != 'E'; ++at) {
        char c = text[at];
        if (c == '.') { if (point) return false; point = true; continue; }
        if (c == '_') {
            if (!at || at + 1 == length || !xr_decimal_digit(text[at - 1]) || !xr_decimal_digit(text[at + 1])) return false;
            continue;
        }
        if (!xr_decimal_digit(c)) return false;
        seen = true; if (!point) ++before;
        if (!nonzero && c == '0') { ++leading; continue; }
        nonzero = true;
        if (out->digits < XR_DECIMAL_DIGITS) {
            if (!xr_decimal_multiply(&out->coefficient, 10, (uint32_t) (c - '0'))) return false;
            ++out->digits;
        } else if (c != '0') out->sticky = true;
    }
    if (!seen) return false;
    int64_t exponent = 0; bool negative = false;
    if (at < length) {
        ++at;
        if (at < length && (text[at] == '+' || text[at] == '-')) negative = text[at++] == '-';
        if (at == length || !xr_decimal_digit(text[at])) return false;
        for (; at < length; ++at) {
            char c = text[at];
            if (c == '_') {
                if (at + 1 == length || !xr_decimal_digit(text[at - 1]) || !xr_decimal_digit(text[at + 1])) return false;
                continue;
            }
            if (!xr_decimal_digit(c)) return false;
            if (exponent < INT64_C(1099511627776)) exponent = exponent * 10 + (c - '0');
        }
    }
    out->point = before - leading + (negative ? -exponent : exponent);
    return true;
}
static inline bool xr_decimal_quotient(XrDecimalInteger *numerator, const XrDecimalInteger *denominator,
    bool sticky, uint64_t *rounded) {
    int shift = (int) xr_decimal_bits(numerator) - (int) xr_decimal_bits(denominator);
    if (shift > 63) return false;
    uint64_t value = 0;
    for (; shift >= 0; --shift) {
        XrDecimalInteger term = *denominator;
        if (!xr_decimal_shift(&term, (unsigned) shift)) return false;
        if (xr_decimal_compare(numerator, &term) >= 0) {
            xr_decimal_subtract(numerator, &term); value |= UINT64_C(1) << (unsigned) shift;
        }
    }
    if (!xr_decimal_shift(numerator, 1)) return false;
    int relation = xr_decimal_compare(numerator, denominator);
    *rounded = value + (relation > 0 || (!relation && (sticky || (value & 1))));
    return true;
}
static inline bool xr_decimal_encode(XrDecimalInput *input, uint32_t width, uint64_t *output) {
    unsigned fraction = width == 32 ? 23 : 52;
    int bias = width == 32 ? 127 : 1023, minimum = 1 - bias;
    uint64_t sign = input->negative ? UINT64_C(1) << (width - 1) : 0;
    uint64_t infinity = width == 32 ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    if (!input->digits || input->point < -325) { *output = sign; return true; }
    if (input->point > 310) { *output = sign | infinity; return true; }
    XrDecimalInteger numerator = input->coefficient, denominator = {{1}, 1};
    int exponent10 = (int) input->point - (int) input->digits;
    XrDecimalInteger *scaled = exponent10 < 0 ? &denominator : &numerator;
    unsigned power = (unsigned) (exponent10 < 0 ? -exponent10 : exponent10);
    for (unsigned i = 0; i < power; ++i) if (!xr_decimal_multiply(scaled, 10, 0)) return false;
    int exponent = (int) xr_decimal_bits(&numerator) - (int) xr_decimal_bits(&denominator);
    XrDecimalInteger trial = exponent < 0 ? numerator : denominator;
    if (!xr_decimal_shift(&trial, (unsigned) (exponent < 0 ? -exponent : exponent))) return false;
    if ((exponent < 0 ? xr_decimal_compare(&trial, &denominator) : xr_decimal_compare(&numerator, &trial)) < 0) --exponent;
    if (exponent > bias) { *output = sign | infinity; return true; }
    int unit = (exponent < minimum ? minimum : exponent) - (int) fraction;
    if (!xr_decimal_shift(unit < 0 ? &numerator : &denominator, (unsigned) (unit < 0 ? -unit : unit))) return false;
    uint64_t rounded;
    if (!xr_decimal_quotient(&numerator, &denominator, input->sticky, &rounded)) return false;
    if (exponent < minimum && rounded < (UINT64_C(1) << fraction)) { *output = sign | rounded; return true; }
    if (exponent < minimum) exponent = minimum;
    if (rounded >= (UINT64_C(1) << (fraction + 1))) { rounded >>= 1; ++exponent; }
    if (exponent > bias) { *output = sign | infinity; return true; }
    *output = sign | ((uint64_t) (exponent + bias) << fraction) | (rounded & ((UINT64_C(1) << fraction) - 1));
    return true;
}
/* Exact-length ASCII decimal, optional sign, decimal point, e/E exponent and
 * separators strictly between digits. Failure clears every nonnull output.
 * No allocation, locale, host FPU operation or floating exception is involved. */
static inline bool xr_decimal_float_parse(const char *text, size_t length, uint32_t width, uint64_t *output) {
    if (!output) return false;
    *output = 0;
    if (width != 32 && width != 64) return false;
    XrDecimalInput input;
    return xr_decimal_scan(text, length, &input) && xr_decimal_encode(&input, width, output);
}
#endif // XR_DECIMAL_FLOAT_H
