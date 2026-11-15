/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_ieee_core.h - Pure binary format descriptions and exact decoded parts
 *
 * KEY CONCEPT:
 *   The compiler and runtime decode the same scalar bits without value objects.
 */
#ifndef XR_IEEE_CORE_H
#define XR_IEEE_CORE_H
#include <stdbool.h>
#include <stdint.h>

typedef struct XrIeeeFormat {
    unsigned fraction;
    int bias;
    uint64_t sign, infinity, nan;
} XrIeeeFormat;
typedef struct XrIeeeParts {
    uint64_t significand;
    int exponent;
    bool negative, infinite, nan;
} XrIeeeParts;
static inline bool xr_ieee_format(uint32_t bits, XrIeeeFormat *format) {
    if (bits == 32) *format = (XrIeeeFormat) {23, 127, UINT64_C(0x80000000),
        UINT64_C(0x7f800000), UINT64_C(0x7fc00000)};
    else if (bits == 64) *format = (XrIeeeFormat) {52, 1023, UINT64_C(0x8000000000000000),
        UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff8000000000000)};
    else return false;
    return true;
}
static inline bool xr_ieee_decode(uint32_t bits, uint64_t input, XrIeeeParts *parts) {
    XrIeeeFormat format;
    if (!xr_ieee_format(bits, &format) || (bits == 32 && input > UINT32_MAX)) return false;
    uint64_t fraction = input & ((UINT64_C(1) << format.fraction) - 1);
    unsigned exponent = (unsigned) ((input & format.infinity) >> format.fraction);
    bool special = (input & format.infinity) == format.infinity;
    *parts = (XrIeeeParts) {fraction, (exponent ? (int) exponent : 1) - format.bias - (int) format.fraction,
        (input & format.sign) != 0, special && !fraction, special && fraction != 0};
    if (exponent && !special) parts->significand |= UINT64_C(1) << format.fraction;
    return true;
}
#endif // XR_IEEE_CORE_H
