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
#include "xrt_arc.h"
#include <string.h>

static inline XrValue xrt_base64_is_valid(const char *data, int64_t len) {
    return XR_FROM_BOOL(xr_base64_core_is_valid(data, len < 0 ? 0 : (size_t) len));
}

static inline XrValue xrt_base64_encode_impl(const char *data, int64_t len, bool url_safe) {
    if (!data)
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

#endif  // XRT_BASE64_H
