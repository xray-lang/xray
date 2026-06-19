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

#include "xrt_value.h"
#include "../shared/xr_encoding_core.h"

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
