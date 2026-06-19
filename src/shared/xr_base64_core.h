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

static const char XR_BASE64_CORE_STD_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char XR_BASE64_CORE_URL_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

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

static inline bool xr_base64_core_encoded_len(size_t len, bool padding, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (len > (SIZE_MAX / 4) * 3)
        return false;

    size_t encoded_len = ((len + 2) / 3) * 4;
    if (!padding) {
        size_t remainder = len % 3;
        if (remainder == 1)
            encoded_len -= 2;
        else if (remainder == 2)
            encoded_len -= 1;
    }
    if (out_len)
        *out_len = encoded_len;
    return true;
}

static inline bool xr_base64_core_encode(const unsigned char *data, size_t len, bool url_safe,
                                         bool padding, char *out, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if ((!data && len != 0) || !out)
        return false;

    size_t encoded_len = 0;
    if (!xr_base64_core_encoded_len(len, padding, &encoded_len))
        return false;

    const char *chars = url_safe ? XR_BASE64_CORE_URL_ALPHABET : XR_BASE64_CORE_STD_ALPHABET;
    size_t i = 0;
    size_t j = 0;
    while (i + 2 < len) {
        unsigned int n =
            ((unsigned int) data[i] << 16) | ((unsigned int) data[i + 1] << 8) | data[i + 2];
        out[j++] = chars[(n >> 18) & 0x3F];
        out[j++] = chars[(n >> 12) & 0x3F];
        out[j++] = chars[(n >> 6) & 0x3F];
        out[j++] = chars[n & 0x3F];
        i += 3;
    }

    size_t remaining = len - i;
    if (remaining == 1) {
        unsigned int n = (unsigned int) data[i] << 16;
        out[j++] = chars[(n >> 18) & 0x3F];
        out[j++] = chars[(n >> 12) & 0x3F];
        if (padding) {
            out[j++] = '=';
            out[j++] = '=';
        }
    } else if (remaining == 2) {
        unsigned int n = ((unsigned int) data[i] << 16) | ((unsigned int) data[i + 1] << 8);
        out[j++] = chars[(n >> 18) & 0x3F];
        out[j++] = chars[(n >> 12) & 0x3F];
        out[j++] = chars[(n >> 6) & 0x3F];
        if (padding)
            out[j++] = '=';
    }

    out[j] = '\0';
    if (out_len)
        *out_len = j;
    return j == encoded_len;
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

static inline bool xr_base64_core_decoded_len(const char *data, size_t len, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!data)
        return false;

    int pad = xr_base64_core_count_padding(data, len);
    if (pad < 0)
        return false;
    len -= (size_t) pad;

    if ((len % 4) == 1)
        return false;

    size_t decoded_len = (len / 4) * 3;
    size_t tail = len % 4;
    if (tail == 2)
        decoded_len += 1;
    else if (tail == 3)
        decoded_len += 2;
    if (out_len)
        *out_len = decoded_len;
    return true;
}

static inline bool xr_base64_core_decode(const char *data, size_t len, unsigned char *out,
                                         size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!data || !out)
        return false;

    size_t decoded_len = 0;
    if (!xr_base64_core_decoded_len(data, len, &decoded_len))
        return false;

    int pad = xr_base64_core_count_padding(data, len);
    len -= (size_t) pad;

    size_t i = 0;
    size_t j = 0;
    while (i + 3 < len) {
        uint8_t a = xr_base64_core_decode_value((unsigned char) data[i]);
        uint8_t b = xr_base64_core_decode_value((unsigned char) data[i + 1]);
        uint8_t c = xr_base64_core_decode_value((unsigned char) data[i + 2]);
        uint8_t d = xr_base64_core_decode_value((unsigned char) data[i + 3]);
        if (a == 64 || b == 64 || c == 64 || d == 64)
            return false;

        out[j++] = (unsigned char) ((a << 2) | (b >> 4));
        out[j++] = (unsigned char) ((b << 4) | (c >> 2));
        out[j++] = (unsigned char) ((c << 6) | d);
        i += 4;
    }

    size_t remaining = len - i;
    if (remaining >= 2) {
        uint8_t a = xr_base64_core_decode_value((unsigned char) data[i]);
        uint8_t b = xr_base64_core_decode_value((unsigned char) data[i + 1]);
        if (a == 64 || b == 64)
            return false;

        out[j++] = (unsigned char) ((a << 2) | (b >> 4));
        if (remaining == 3) {
            uint8_t c = xr_base64_core_decode_value((unsigned char) data[i + 2]);
            if (c == 64)
                return false;
            out[j++] = (unsigned char) ((b << 4) | (c >> 2));
        }
    }

    out[j] = '\0';
    if (out_len)
        *out_len = j;
    return j == decoded_len;
}

#endif  // XR_BASE64_CORE_H
