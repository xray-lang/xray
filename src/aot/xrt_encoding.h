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

static inline int xrt_encoding_utf16_endian_from_value(XrValue value) {
    return XR_IS_INT(value) && XR_TO_INT(value) == XR_ENCODING_UTF16_BE ? XR_ENCODING_UTF16_BE
                                                                        : XR_ENCODING_UTF16_LE;
}

static inline XrValue xrt_encoding_utf16_encode_impl(const char *data, int64_t len, int endian) {
    if (!data && len != 0)
        return XR_NULL_VAL;
    size_t n = len < 0 ? 0 : (size_t) len;
    size_t out_len = 0;
    if (!xr_encoding_core_utf16_encoded_len(data, n, &out_len) || out_len > (size_t) INT64_MAX)
        return XR_NULL_VAL;
    XrValue result = xrt_array_new_typed_uninit((int64_t) out_len, XR_ELEM_U8);
    xrt_array_t *arr = (xrt_array_t *) result.ptr;
    arr->length = (int64_t) out_len;
    if (!xr_encoding_core_utf16_encode(data, n, (uint8_t *) arr->data, (size_t) arr->capacity,
                                       endian, &out_len)) {
        xrt_release(result);
        return XR_NULL_VAL;
    }
    arr->length = (int64_t) out_len;
    return result;
}

static inline XrValue xrt_encoding_utf16_encode(const char *data, int64_t len) {
    return xrt_encoding_utf16_encode_impl(data, len, XR_ENCODING_UTF16_LE);
}

static inline XrValue xrt_encoding_utf16_encode_endian(const char *data, int64_t len,
                                                       XrValue endian) {
    return xrt_encoding_utf16_encode_impl(data, len, xrt_encoding_utf16_endian_from_value(endian));
}

static inline int xrt_encoding_value_bytes_view(XrValue value, const uint8_t **out_data,
                                                size_t *out_len) {
    if (XR_IS_STR(value)) {
        *out_data = (const uint8_t *) xr_str_data(value);
        *out_len = (size_t) xr_str_len(value);
        return 1;
    }
    if (XR_IS_ARRAY(value) && value.ptr) {
        xrt_array_t *arr = (xrt_array_t *) value.ptr;
        if (arr->elem_type != XR_ELEM_U8)
            return 0;
        *out_data = (const uint8_t *) arr->data;
        *out_len = arr->length < 0 ? 0 : (size_t) arr->length;
        return 1;
    }
    return 0;
}

static inline XrValue xrt_encoding_utf16_decode_impl(XrValue input, int endian, int endian_explicit,
                                                     int strip_bom) {
    const uint8_t *data = NULL;
    size_t len = 0;
    if (!xrt_encoding_value_bytes_view(input, &data, &len))
        return XR_NULL_VAL;

    if (strip_bom && len >= 2) {
        if (data[0] == 0xFF && data[1] == 0xFE) {
            if (!endian_explicit)
                endian = XR_ENCODING_UTF16_LE;
            data += 2;
            len -= 2;
        } else if (data[0] == 0xFE && data[1] == 0xFF) {
            if (!endian_explicit)
                endian = XR_ENCODING_UTF16_BE;
            data += 2;
            len -= 2;
        }
    }

    size_t out_len = 0;
    if (!xr_encoding_core_utf16_to_utf8_len(data, len, endian, &out_len) ||
        out_len > (size_t) INT64_MAX)
        return XR_NULL_VAL;
    XrValue result = xrt_str_alloc(out_len);
    if (!xr_encoding_core_utf16_decode(data, len, xr_str_buf(result), out_len, endian, &out_len)) {
        xrt_release(result);
        return XR_NULL_VAL;
    }
    xr_str_buf(result)[out_len] = '\0';
    return result;
}

static inline XrValue xrt_encoding_utf16_decode(XrValue input) {
    return xrt_encoding_utf16_decode_impl(input, XR_ENCODING_UTF16_LE, 0, 1);
}

static inline XrValue xrt_encoding_utf16_decode_endian(XrValue input, XrValue endian) {
    return xrt_encoding_utf16_decode_impl(input, xrt_encoding_utf16_endian_from_value(endian), 1,
                                          1);
}

static inline XrValue xrt_encoding_utf16_decode_endian_strip(XrValue input, XrValue endian,
                                                             XrValue strip_bom) {
    int strip = !XR_IS_BOOL(strip_bom) || strip_bom.i != 0;
    return xrt_encoding_utf16_decode_impl(input, xrt_encoding_utf16_endian_from_value(endian), 1,
                                          strip);
}

#endif  // XRT_ENCODING_H
