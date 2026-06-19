/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_base64_core.h - Pure Base64 predicates shared by VM stdlib and AOT
 */

#ifndef XR_BASE64_CORE_H
#define XR_BASE64_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline uint8_t xr_base64_core_decode_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z')
        return (uint8_t) (c - 'A');
    if (c >= 'a' && c <= 'z')
        return (uint8_t) (26 + c - 'a');
    if (c >= '0' && c <= '9')
        return (uint8_t) (52 + c - '0');
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return 64;
}

static inline int xr_base64_core_count_padding(const char *data, size_t len) {
    int pad = 0;
    while (len > 0 && data[len - 1] == '=') {
        pad++;
        len--;
    }
    if (pad > 2)
        return -1;
    return pad;
}

static inline bool xr_base64_core_is_valid(const char *data, size_t len) {
    if (!data)
        return false;

    int pad = xr_base64_core_count_padding(data, len);
    if (pad < 0)
        return false;
    len -= (size_t) pad;

    if ((len % 4) == 1)
        return false;

    for (size_t i = 0; i < len; i++) {
        if (xr_base64_core_decode_value((unsigned char) data[i]) == 64)
            return false;
    }
    return true;
}

#endif  // XR_BASE64_CORE_H
