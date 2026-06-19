/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_encoding.h - Freestanding AOT encoding helpers
 */

#ifndef XRT_ENCODING_H
#define XRT_ENCODING_H

#include "../shared/xr_encoding_core.h"
#include "xrt_coll.h"
#include <stdint.h>

static inline XrValue xrt_encoding_hex_encode(const char *data, int64_t len) {
    if (!data && len != 0)
        return XR_NULL_VAL;
    size_t n = len < 0 ? 0 : (size_t) len;
    size_t out_len = 0;
    if (!xr_encoding_core_hex_encoded_len(n, &out_len) || out_len > (size_t) INT64_MAX)
        return XR_NULL_VAL;
    XrValue result = xrt_str_alloc(out_len);
    if (!xr_encoding_core_hex_encode((const uint8_t *) data, n, xr_str_buf(result))) {
        xrt_release(result);
        return XR_NULL_VAL;
    }
    return result;
}

static inline XrValue xrt_encoding_hex_decode(const char *data, int64_t len) {
    if (!data)
        return XR_NULL_VAL;
    size_t n = len < 0 ? 0 : (size_t) len;
    size_t out_len = 0;
    if (!xr_encoding_core_hex_decoded_len(data, n, &out_len) || out_len > (size_t) INT64_MAX)
        return XR_NULL_VAL;
    XrValue result = xrt_array_new_typed_uninit((int64_t) out_len, XR_ELEM_U8);
    xrt_array_t *arr = (xrt_array_t *) result.ptr;
    arr->length = (int64_t) out_len;
    if (!xr_encoding_core_hex_decode(data, n, (uint8_t *) arr->data, &out_len)) {
        xrt_release(result);
        return XR_NULL_VAL;
    }
    arr->length = (int64_t) out_len;
    return result;
}

static inline XrValue xrt_encoding_hex_decode_string(const char *data, int64_t len) {
    if (!data)
        return XR_NULL_VAL;
    size_t n = len < 0 ? 0 : (size_t) len;
    size_t out_len = 0;
    if (!xr_encoding_core_hex_decoded_len(data, n, &out_len) || out_len > (size_t) INT64_MAX)
        return XR_NULL_VAL;
    XrValue result = xrt_str_alloc(out_len);
    if (!xr_encoding_core_hex_decode(data, n, (uint8_t *) xr_str_buf(result), &out_len)) {
        xrt_release(result);
        return XR_NULL_VAL;
    }
    xr_str_buf(result)[out_len] = '\0';
    return result;
}

static inline XrValue xrt_encoding_hex_valid(const char *data, int64_t len) {
    return XR_FROM_BOOL(xr_encoding_core_hex_valid(data, len < 0 ? 0 : (size_t) len));
}

static inline XrValue xrt_encoding_utf8_valid(const char *data, int64_t len) {
    return XR_FROM_BOOL(xr_encoding_core_utf8_valid(data, len < 0 ? 0 : (size_t) len));
}

static inline XrValue xrt_encoding_utf8_count(const char *data, int64_t len) {
    return XR_FROM_INT((int64_t) xr_encoding_core_utf8_count(data, len < 0 ? 0 : (size_t) len));
}

static inline XrValue xrt_encoding_utf8_byte_length(const char *data, int64_t len) {
    return XR_FROM_INT(len < 0 ? 0 : len);
}

#endif  // XRT_ENCODING_H
