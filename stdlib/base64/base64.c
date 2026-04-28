/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * base64.c - Base64 encoding/decoding implementation
 *
 * KEY CONCEPT:
 *   Implements RFC 4648 Base64 encoding with lookup table for fast decoding.
 *   Supports both standard and URL-safe variants.
 */

#include "base64.h"
#include "../common.h"
#include "../../src/vm/xvm_internal.h"
#include <stdio.h>
#include <string.h>

/* ========== Base64 encoding tables ========== */

// Standard Base64 alphabet
static const char BASE64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// URL-safe Base64 alphabet (+ -> -, / -> _)
static const char BASE64_URL_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// Decode lookup table (64 = invalid character)
static const unsigned char BASE64_DECODE_TABLE[256] = {
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 62, 64, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64, 64, 0,  1,  2,  3,  4,  5,  6,
    7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 63,
    64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
    49, 50, 51, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
};

/* ========== Helper functions ========== */

/* ========== Encoding implementation ========== */

// Internal encoding function
static char *base64_encode_internal(const unsigned char *data, size_t len, const char *chars,
                                    bool padding, size_t *out_len) {
    // Calculate output length
    size_t encoded_len = ((len + 2) / 3) * 4;
    if (!padding) {
        // Without padding, reduce trailing '=' count
        size_t remainder = len % 3;
        if (remainder == 1)
            encoded_len -= 2;
        else if (remainder == 2)
            encoded_len -= 1;
    }

    char *output = (char *) xr_malloc(encoded_len + 1);
    if (!output)
        return NULL;

    size_t i = 0, j = 0;

    // Encode 3 bytes into 4 characters
    while (i + 2 < len) {
        unsigned int n =
            ((unsigned int) data[i] << 16) | ((unsigned int) data[i + 1] << 8) | data[i + 2];

        output[j++] = chars[(n >> 18) & 0x3F];
        output[j++] = chars[(n >> 12) & 0x3F];
        output[j++] = chars[(n >> 6) & 0x3F];
        output[j++] = chars[n & 0x3F];

        i += 3;
    }

    // Handle remaining bytes
    size_t remaining = len - i;
    if (remaining == 1) {
        unsigned int n = (unsigned int) data[i] << 16;
        output[j++] = chars[(n >> 18) & 0x3F];
        output[j++] = chars[(n >> 12) & 0x3F];
        if (padding) {
            output[j++] = '=';
            output[j++] = '=';
        }
    } else if (remaining == 2) {
        unsigned int n = ((unsigned int) data[i] << 16) | ((unsigned int) data[i + 1] << 8);
        output[j++] = chars[(n >> 18) & 0x3F];
        output[j++] = chars[(n >> 12) & 0x3F];
        output[j++] = chars[(n >> 6) & 0x3F];
        if (padding) {
            output[j++] = '=';
        }
    }

    output[j] = '\0';
    if (out_len)
        *out_len = j;
    return output;
}

// Count and validate trailing padding (max 2 '=' allowed)
static int count_padding(const char *data, size_t len) {
    int pad = 0;
    while (len > 0 && data[len - 1] == '=') {
        pad++;
        len--;
    }
    if (pad > 2)
        return -1;
    return pad;
}

// Internal decoding function
static unsigned char *base64_decode_internal(const char *data, size_t len, size_t *out_len) {
    // Validate and strip trailing padding
    int pad = count_padding(data, len);
    if (pad < 0)
        return NULL;
    len -= (size_t) pad;

    // After stripping padding, len%4 == 1 is invalid (6 bits, not enough for 1 byte)
    if (len % 4 == 1)
        return NULL;

    // Calculate output length
    size_t decoded_len = (len / 4) * 3;
    size_t tail = len % 4;
    if (tail == 2)
        decoded_len += 1;
    else if (tail == 3)
        decoded_len += 2;

    unsigned char *output = (unsigned char *) xr_malloc(decoded_len + 1);
    if (!output)
        return NULL;

    size_t i = 0, j = 0;

    while (i + 3 < len) {
        unsigned char a = BASE64_DECODE_TABLE[(unsigned char) data[i]];
        unsigned char b = BASE64_DECODE_TABLE[(unsigned char) data[i + 1]];
        unsigned char c = BASE64_DECODE_TABLE[(unsigned char) data[i + 2]];
        unsigned char d = BASE64_DECODE_TABLE[(unsigned char) data[i + 3]];

        if (a == 64 || b == 64 || c == 64 || d == 64) {
            xr_free(output);
            return NULL;
        }

        output[j++] = (a << 2) | (b >> 4);
        output[j++] = (b << 4) | (c >> 2);
        output[j++] = (c << 6) | d;

        i += 4;
    }

    // Handle remaining characters (2 or 3)
    size_t remaining = len - i;
    if (remaining >= 2) {
        unsigned char a = BASE64_DECODE_TABLE[(unsigned char) data[i]];
        unsigned char b = BASE64_DECODE_TABLE[(unsigned char) data[i + 1]];

        if (a == 64 || b == 64) {
            xr_free(output);
            return NULL;
        }

        output[j++] = (a << 2) | (b >> 4);

        if (remaining == 3) {
            unsigned char c = BASE64_DECODE_TABLE[(unsigned char) data[i + 2]];
            if (c == 64) {
                xr_free(output);
                return NULL;
            }
            output[j++] = (b << 4) | (c >> 2);
        }
    }

    output[j] = '\0';
    if (out_len)
        *out_len = j;
    return output;
}

// Internal validation function
static bool base64_is_valid_internal(const char *data, size_t len) {
    // Validate and strip trailing padding
    int pad = count_padding(data, len);
    if (pad < 0)
        return false;
    len -= (size_t) pad;

    // len%4 == 1 is structurally invalid
    if (len % 4 == 1)
        return false;

    // Check each character
    for (size_t i = 0; i < len; i++) {
        if (BASE64_DECODE_TABLE[(unsigned char) data[i]] == 64) {
            return false;
        }
    }

    return true;
}

/* ========== C-level API implementation ========== */

char *xr_base64_encode(const unsigned char *data, size_t len, size_t *out_len) {
    return base64_encode_internal(data, len, BASE64_CHARS, true, out_len);
}

char *xr_base64_encode_url(const unsigned char *data, size_t len, size_t *out_len) {
    return base64_encode_internal(data, len, BASE64_URL_CHARS, false, out_len);
}

unsigned char *xr_base64_decode(const char *data, size_t len, size_t *out_len) {
    return base64_decode_internal(data, len, out_len);
}

unsigned char *xr_base64_decode_url(const char *data, size_t len, size_t *out_len) {
    // Decode table already handles both standard (+/) and URL-safe (-_) characters
    return base64_decode_internal(data, len, out_len);
}

bool xr_base64_is_valid(const char *data, size_t len) {
    return base64_is_valid_internal(data, len);
}

/* ========== xray module exported functions ========== */

// encode(str)
static XrValue base64_encode(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();

    size_t len;
    const char *data = xrs_string_arg(args[0], &len);
    if (!data)
        return xr_null();

    size_t out_len;
    char *encoded =
        base64_encode_internal((const unsigned char *) data, len, BASE64_CHARS, true, &out_len);
    if (!encoded)
        return xr_null();

    XrValue result = xrs_string_value_n(X, encoded, out_len);
    xr_free(encoded);
    return result;
}

// decode(str)
static XrValue base64_decode(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();

    size_t len;
    const char *data = xrs_string_arg(args[0], &len);
    if (!data)
        return xr_null();

    size_t out_len;
    unsigned char *decoded = base64_decode_internal(data, len, &out_len);
    if (!decoded)
        return xr_null();

    XrValue result = xrs_string_value_n(X, (char *) decoded, out_len);
    xr_free(decoded);
    return result;
}

// encodeUrl(str)
static XrValue base64_encodeUrl(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();

    size_t len;
    const char *data = xrs_string_arg(args[0], &len);
    if (!data)
        return xr_null();

    size_t out_len;
    char *encoded = base64_encode_internal((const unsigned char *) data, len, BASE64_URL_CHARS,
                                           false, &out_len);
    if (!encoded)
        return xr_null();

    XrValue result = xrs_string_value_n(X, encoded, out_len);
    xr_free(encoded);
    return result;
}

// decodeUrl(str)
static XrValue base64_decodeUrl(XrayIsolate *X, XrValue *args, int argc) {
    return base64_decode(X, args, argc);
}

// encodeBytes(bytes: Array<uint8>)
static XrValue base64_encodeBytes(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_ARRAY(args[0]))
        return xr_null();

    XrArray *arr = XR_TO_ARRAY(args[0]);
    if (arr->length == 0)
        return xrs_string_value_c(X, "");

    const unsigned char *data;
    size_t len = (size_t) arr->length;

    // Fast path: typed uint8 array has contiguous data
    if (arr->elem_type == XR_ELEM_U8) {
        data = (const unsigned char *) arr->data;
    } else {
        // Slow path: copy elements to temp buffer
        unsigned char *buf = (unsigned char *) xr_malloc(len);
        if (!buf)
            return xr_null();
        for (size_t i = 0; i < len; i++) {
            XrValue v = xr_array_get_element(arr, (int32_t) i);
            buf[i] = (unsigned char) (XR_IS_INT(v) ? XR_TO_INT(v) : 0);
        }
        size_t out_len;
        char *encoded = base64_encode_internal(buf, len, BASE64_CHARS, true, &out_len);
        xr_free(buf);
        if (!encoded)
            return xr_null();
        XrValue result = xrs_string_value_n(X, encoded, out_len);
        xr_free(encoded);
        return result;
    }

    size_t out_len;
    char *encoded = base64_encode_internal(data, len, BASE64_CHARS, true, &out_len);
    if (!encoded)
        return xr_null();

    XrValue result = xrs_string_value_n(X, encoded, out_len);
    xr_free(encoded);
    return result;
}

// decodeToBytes(str)
static XrValue base64_decodeToBytes(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();

    size_t len;
    const char *data = xrs_string_arg(args[0], &len);
    if (!data)
        return xr_null();

    size_t out_len;
    unsigned char *decoded = base64_decode_internal(data, len, &out_len);
    if (!decoded)
        return xr_null();

    XrArray *arr = xr_array_bytes_new(xr_current_coro(X), (int32_t) out_len);
    if (!arr) {
        xr_free(decoded);
        return xr_null();
    }

    memcpy(arr->data, decoded, out_len);
    arr->length = (int32_t) out_len;
    xr_free(decoded);

    return xr_value_from_array(arr);
}

// isValid(str)
static XrValue base64_isValid(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);

    size_t len;
    const char *data = xrs_string_arg(args[0], &len);
    if (!data)
        return xr_bool(false);

    return xr_bool(base64_is_valid_internal(data, len));
}

/* ========== Module loading ========== */

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module base64

XR_DEFINE_BUILTIN(base64_encode, "encode", "(data: string): string", "Base64 encode")
XR_DEFINE_BUILTIN(base64_decode, "decode", "(data: string): string?", "Base64 decode")
XR_DEFINE_BUILTIN(base64_encodeUrl, "encodeUrl", "(data: string): string", "URL-safe base64 encode")
XR_DEFINE_BUILTIN(base64_decodeUrl, "decodeUrl", "(data: string): string?",
                  "URL-safe base64 decode")
XR_DEFINE_BUILTIN(base64_encodeBytes, "encodeBytes", "(data: Array<uint8>): string",
                  "Encode byte array to Base64")
XR_DEFINE_BUILTIN(base64_decodeToBytes, "decodeToBytes", "(data: string): Array<uint8>?",
                  "Decode Base64 to byte array")
XR_DEFINE_BUILTIN(base64_isValid, "isValid", "(data: string): bool", "Check if valid base64")

XR_FUNC XrModule *xr_load_module_base64(XrayIsolate *isolate) {
    // Create native module
    XrModule *mod = xr_module_create_native(isolate, "base64");
    if (!mod)
        return NULL;

    XRS_EXPORT(mod, isolate, "encode", base64_encode);
    XRS_EXPORT(mod, isolate, "decode", base64_decode);
    XRS_EXPORT(mod, isolate, "encodeUrl", base64_encodeUrl);
    XRS_EXPORT(mod, isolate, "decodeUrl", base64_decodeUrl);
    XRS_EXPORT(mod, isolate, "encodeBytes", base64_encodeBytes);
    XRS_EXPORT(mod, isolate, "decodeToBytes", base64_decodeToBytes);
    XRS_EXPORT(mod, isolate, "isValid", base64_isValid);

    // Mark as loaded
    mod->loaded = true;
    return mod;
}
