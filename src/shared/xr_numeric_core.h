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
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

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
