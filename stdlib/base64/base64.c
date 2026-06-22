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
#include "../../src/coro/xcoroutine.h"
#include "../../src/shared/xr_base64_core.h"
#include <string.h>

/* ========== Encoding implementation ========== */

// Internal encoding function
static char *base64_encode_internal(const unsigned char *data, size_t len, bool url_safe,
                                    bool padding, size_t *out_len) {
    if (out_len)
        *out_len = 0;

    size_t encoded_len = 0;
    if (!xr_base64_core_encoded_len(len, padding, &encoded_len))
        return NULL;

    char *output = (char *) xr_malloc(encoded_len + 1);
    if (!output)
        return NULL;

    if (!xr_base64_core_encode(data, len, url_safe, padding, output, out_len)) {
        xr_free(output);
        return NULL;
    }
    return output;
}

// Internal decoding function
static unsigned char *base64_decode_internal(const char *data, size_t len, size_t *out_len) {
    if (out_len)
        *out_len = 0;

    size_t decoded_len = 0;
    if (!xr_base64_core_decoded_len(data, len, &decoded_len))
        return NULL;

    unsigned char *output = (unsigned char *) xr_malloc(decoded_len + 1);
    if (!output)
        return NULL;

    if (!xr_base64_core_decode(data, len, output, out_len)) {
        xr_free(output);
        return NULL;
    }
    return output;
}

// Internal validation function
static bool base64_is_valid_internal(const char *data, size_t len) {
    return xr_base64_core_is_valid(data, len);
}

/* ========== C-level API implementation ========== */

XR_FUNC char *xr_base64_encode(const unsigned char *data, size_t len, size_t *out_len) {
    return base64_encode_internal(data, len, false, true, out_len);
}

XR_FUNC char *xr_base64_encode_url(const unsigned char *data, size_t len, size_t *out_len) {
    return base64_encode_internal(data, len, true, false, out_len);
}

XR_FUNC unsigned char *xr_base64_decode(const char *data, size_t len, size_t *out_len) {
    return base64_decode_internal(data, len, out_len);
}

XR_FUNC unsigned char *xr_base64_decode_url(const char *data, size_t len, size_t *out_len) {
    // Decode table already handles both standard (+/) and URL-safe (-_) characters
    return base64_decode_internal(data, len, out_len);
}

XR_FUNC bool xr_base64_is_valid(const char *data, size_t len) {
    return base64_is_valid_internal(data, len);
}

/* ========== xray module exported functions ========== */

// encode(str)
static XrValue base64_encode(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();

    size_t len;
    const char *data = xrs_string_arg(args[0], &len);
    if (!data)
        return xr_null();

    size_t out_len;
    char *encoded =
        base64_encode_internal((const unsigned char *) data, len, false, true, &out_len);
    if (!encoded)
        return xr_null();

    XrValue result = xrs_string_value_n(X, encoded, out_len);
    xr_free(encoded);
    return result;
}

// decode(str)
static XrValue base64_decode(XrVMRuntime *X, XrValue *args, int argc) {
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
static XrValue base64_encodeUrl(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();

    size_t len;
    const char *data = xrs_string_arg(args[0], &len);
    if (!data)
        return xr_null();

    size_t out_len;
    char *encoded =
        base64_encode_internal((const unsigned char *) data, len, true, false, &out_len);
    if (!encoded)
        return xr_null();

    XrValue result = xrs_string_value_n(X, encoded, out_len);
    xr_free(encoded);
    return result;
}

// decodeUrl(str)
static XrValue base64_decodeUrl(XrVMRuntime *X, XrValue *args, int argc) {
    return base64_decode(X, args, argc);
}

// encodeBytes(bytes: Array<uint8>)
static XrValue base64_encodeBytes(XrVMRuntime *X, XrValue *args, int argc) {
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
        char *encoded = base64_encode_internal(buf, len, false, true, &out_len);
        xr_free(buf);
        if (!encoded)
            return xr_null();
        XrValue result = xrs_string_value_n(X, encoded, out_len);
        xr_free(encoded);
        return result;
    }

    size_t out_len;
    char *encoded = base64_encode_internal(data, len, false, true, &out_len);
    if (!encoded)
        return xr_null();

    XrValue result = xrs_string_value_n(X, encoded, out_len);
    xr_free(encoded);
    return result;
}

// decodeToBytes(str)
static XrValue base64_decodeToBytes(XrVMRuntime *X, XrValue *args, int argc) {
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

    if (out_len > INT32_MAX) {
        xr_free(decoded);
        return xr_null();
    }

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
static XrValue base64_isValid(XrVMRuntime *X, XrValue *args, int argc) {
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

XR_FUNC XrModule *xr_load_module_base64(XrVMRuntime *isolate) {
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
