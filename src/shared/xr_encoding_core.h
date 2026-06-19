/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_encoding_core.h - Pure encoding predicates shared by VM stdlib and AOT
 */

#ifndef XR_ENCODING_CORE_H
#define XR_ENCODING_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_ENCODING_UNICODE_MAX 0x10FFFFu
#define XR_ENCODING_UNICODE_INVALID 0xFFFDu

static inline uint8_t xr_encoding_core_hex_value(unsigned char c) {
    if (c >= '0' && c <= '9')
        return (uint8_t) (c - '0');
    if (c >= 'a' && c <= 'f')
        return (uint8_t) (10 + c - 'a');
    if (c >= 'A' && c <= 'F')
        return (uint8_t) (10 + c - 'A');
    return 255;
}

static inline bool xr_encoding_core_hex_valid(const char *hex, size_t len) {
    if (!hex || (len % 2) != 0)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (xr_encoding_core_hex_value((unsigned char) hex[i]) == 255)
            return false;
    }
    return true;
}

static inline int xr_encoding_core_utf8_char_size(uint8_t first_byte) {
    if ((first_byte & 0x80) == 0x00)
        return 1;
    if ((first_byte & 0xE0) == 0xC0)
        return 2;
    if ((first_byte & 0xF0) == 0xE0)
        return 3;
    if ((first_byte & 0xF8) == 0xF0)
        return 4;
    return 1;
}

static inline bool xr_encoding_core_utf8_is_continuation(uint8_t byte) {
    return (byte & 0xC0) == 0x80;
}

static inline int xr_encoding_core_utf8_decode(const char *str, size_t len, uint32_t *out_cp) {
    if (!str || len == 0) {
        if (out_cp)
            *out_cp = 0;
        return 0;
    }

    uint8_t b0 = (uint8_t) str[0];
    uint32_t cp;
    int size;

    if ((b0 & 0x80) == 0) {
        cp = b0;
        size = 1;
    } else if ((b0 & 0xE0) == 0xC0) {
        if (len < 2)
            goto invalid;
        uint8_t b1 = (uint8_t) str[1];
        if (!xr_encoding_core_utf8_is_continuation(b1))
            goto invalid;
        cp = ((uint32_t) (b0 & 0x1F) << 6) | (uint32_t) (b1 & 0x3F);
        if (cp < 0x80)
            goto invalid;
        size = 2;
    } else if ((b0 & 0xF0) == 0xE0) {
        if (len < 3)
            goto invalid;
        uint8_t b1 = (uint8_t) str[1];
        uint8_t b2 = (uint8_t) str[2];
        if (!xr_encoding_core_utf8_is_continuation(b1) ||
            !xr_encoding_core_utf8_is_continuation(b2))
            goto invalid;
        cp =
            ((uint32_t) (b0 & 0x0F) << 12) | ((uint32_t) (b1 & 0x3F) << 6) | (uint32_t) (b2 & 0x3F);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))
            goto invalid;
        size = 3;
    } else if ((b0 & 0xF8) == 0xF0) {
        if (len < 4)
            goto invalid;
        uint8_t b1 = (uint8_t) str[1];
        uint8_t b2 = (uint8_t) str[2];
        uint8_t b3 = (uint8_t) str[3];
        if (!xr_encoding_core_utf8_is_continuation(b1) ||
            !xr_encoding_core_utf8_is_continuation(b2) ||
            !xr_encoding_core_utf8_is_continuation(b3))
            goto invalid;
        cp = ((uint32_t) (b0 & 0x07) << 18) | ((uint32_t) (b1 & 0x3F) << 12) |
             ((uint32_t) (b2 & 0x3F) << 6) | (uint32_t) (b3 & 0x3F);
        if (cp < 0x10000 || cp > XR_ENCODING_UNICODE_MAX)
            goto invalid;
        size = 4;
    } else {
        goto invalid;
    }

    if (out_cp)
        *out_cp = cp;
    return size;

invalid:
    if (out_cp)
        *out_cp = XR_ENCODING_UNICODE_INVALID;
    return 1;
}

static inline bool xr_encoding_core_utf8_valid(const char *str, size_t len) {
    if (!str)
        return false;
    size_t pos = 0;
    while (pos < len) {
        uint32_t cp;
        int size = xr_encoding_core_utf8_decode(str + pos, len - pos, &cp);
        if (size == 0 || cp == XR_ENCODING_UNICODE_INVALID)
            return false;
        pos += (size_t) size;
    }
    return true;
}

static inline size_t xr_encoding_core_utf8_count(const char *str, size_t len) {
    if (!str || len == 0)
        return 0;
    size_t count = 0;
    size_t pos = 0;
    while (pos < len) {
        int size = xr_encoding_core_utf8_char_size((uint8_t) str[pos]);
        if (pos + (size_t) size > len) {
            count++;
            break;
        }
        pos += (size_t) size;
        count++;
    }
    return count;
}

#endif  // XR_ENCODING_CORE_H
