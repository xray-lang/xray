/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_base64.h - Freestanding AOT Base64 helpers
 */

#ifndef XRT_BASE64_H
#define XRT_BASE64_H

#include "../shared/xr_base64_core.h"
#include "xrt_coll.h"
#include <stdint.h>
#include <string.h>

static inline XrValue xrt_base64_is_valid(const char *data, int64_t len) {
    return XR_FROM_BOOL(xr_base64_core_is_valid(data, len < 0 ? 0 : (size_t) len));
}

static inline XrValue xrt_base64_encode_impl(const char *data, int64_t len, bool url_safe) {
    if (!data && len != 0)
        return XR_NULL_VAL;
    size_t n = len < 0 ? 0 : (size_t) len;
    size_t out_len = 0;
    if (!xr_base64_core_encoded_len(n, !url_safe, &out_len))
        return XR_NULL_VAL;
    XrValue result = xrt_str_alloc(out_len);
    if (!xr_base64_core_encode((const unsigned char *) data, n, url_safe, !url_safe,
                               xr_str_buf(result), &out_len))
        return XR_NULL_VAL;
    return result;
}

static inline XrValue xrt_base64_encode(const char *data, int64_t len) {
    return xrt_base64_encode_impl(data, len, false);
}

static inline XrValue xrt_base64_encode_url(const char *data, int64_t len) {
    return xrt_base64_encode_impl(data, len, true);
}

static inline XrValue xrt_base64_decode(const char *data, int64_t len) {
    if (!data)
        return XR_NULL_VAL;
    size_t n = len < 0 ? 0 : (size_t) len;
    size_t out_len = 0;
    if (!xr_base64_core_decoded_len(data, n, &out_len))
        return XR_NULL_VAL;
    XrValue result = xrt_str_alloc(out_len);
    if (!xr_base64_core_decode(data, n, (unsigned char *) xr_str_buf(result), &out_len))
        return XR_NULL_VAL;
    return result;
}

static inline XrValue xrt_base64_decode_url(const char *data, int64_t len) {
    return xrt_base64_decode(data, len);
}

static inline XrValue xrt_base64_encode_bytes(XrValue bytes) {
    if (!XR_IS_ARRAY(bytes) || !bytes.ptr)
        return XR_NULL_VAL;

    xrt_array_t *arr = (xrt_array_t *) bytes.ptr;
    if (arr->length <= 0)
        return xrt_base64_encode_impl(NULL, 0, false);

    size_t len = (size_t) arr->length;
    if (arr->elem_type == XR_ELEM_U8)
        return xrt_base64_encode_impl((const char *) arr->data, (int64_t) len, false);

    if (arr->length > INT32_MAX)
        return XR_NULL_VAL;
    unsigned char *tmp = (unsigned char *) XRT_MALLOC(len);
    if (!tmp)
        return XR_NULL_VAL;
    for (int64_t i = 0; i < arr->length; i++) {
        XrValue item = xr_typed_get(arr->data, (int32_t) i, arr->elem_type);
        tmp[i] = (unsigned char) xr_value_to_int64_coerce(item);
    }
    XrValue result = xrt_base64_encode_impl((const char *) tmp, (int64_t) len, false);
    XRT_FREE(tmp);
    return result;
}

static inline XrValue xrt_base64_decode_to_bytes(const char *data, int64_t len) {
    if (!data)
        return XR_NULL_VAL;
    size_t n = len < 0 ? 0 : (size_t) len;
    if (!xr_base64_core_is_valid(data, n))
        return XR_NULL_VAL;

    size_t out_len = 0;
    if (!xr_base64_core_decoded_len(data, n, &out_len) || out_len > (size_t) INT64_MAX)
        return XR_NULL_VAL;

    XrValue result = xrt_array_new_typed_uninit((int64_t) out_len, XR_ELEM_U8);
    xrt_array_t *arr = (xrt_array_t *) result.ptr;
    arr->length = (int64_t) out_len;
    if (!xr_base64_core_decode(data, n, (unsigned char *) arr->data, &out_len)) {
        xrt_release(result);
        return XR_NULL_VAL;
    }
    arr->length = (int64_t) out_len;
    return result;
}

#endif  // XRT_BASE64_H
