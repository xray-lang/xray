/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_compress.h - Freestanding AOT compression helpers
 */

#ifndef XRT_COMPRESS_H
#define XRT_COMPRESS_H

#include <string.h>

#include "../shared/xr_compress_core.h"
#include "xrt_arc.h"
#include "xrt_value.h"

static inline int xrt_compress_level_or_default(XrValue level) {
    bool has_level = XR_IS_INT(level);
    return xr_compress_core_level_or_default(has_level, has_level ? XR_TO_INT(level) : 0);
}

static inline void *xrt_compress_core_alloc(void *ctx, size_t size) {
    (void) ctx;
    return XRT_MALLOC(size);
}

static inline void xrt_compress_core_free(void *ctx, void *ptr) {
    (void) ctx;
    XRT_FREE(ptr);
}

static inline XrValue xrt_compress_finish_string(uint8_t *buf, size_t len) {
    if (!buf)
        return XR_NULL_VAL;
    XrValue out = xrt_str_alloc(len);
    memcpy(xr_str_buf(out), buf, len);
    xr_str_buf(out)[len] = '\0';
    return out;
}

static inline XrValue xrt_compress_gzip_with_level(const char *data, int64_t len, int level) {
    XrCompressCoreInputView input;
    if (!xr_compress_core_input_view(data, len, &input))
        return XR_NULL_VAL;

    size_t out_len = 0;
    uint8_t *buf =
        xr_compress_core_gzip_alloc(input.data, input.len, &out_len, level, xrt_compress_core_alloc,
                                    xrt_compress_core_free, NULL);
    if (!buf)
        return XR_NULL_VAL;

    XrValue out = xrt_compress_finish_string(buf, out_len);
    XRT_FREE(buf);
    return out;
}

static inline XrValue xrt_compress_gzip_default(const char *data, int64_t len) {
    return xrt_compress_gzip_with_level(data, len, XR_COMPRESS_DEFAULT_COMPRESSION);
}

static inline XrValue xrt_compress_gzip(const char *data, int64_t len, XrValue level) {
    return xrt_compress_gzip_with_level(data, len, xrt_compress_level_or_default(level));
}

static inline XrValue xrt_compress_gunzip(const char *data, int64_t len) {
    XrCompressCoreInputView input;
    if (!xr_compress_core_input_view(data, len, &input))
        return XR_NULL_VAL;

    size_t out_len = 0;
    uint8_t *buf = xr_compress_core_gunzip_alloc(
        input.data, input.len, xr_gzip_original_size(input.data, input.len), &out_len,
        xrt_compress_core_alloc, xrt_compress_core_free, NULL);
    if (!buf)
        return XR_NULL_VAL;

    XrValue out = xrt_compress_finish_string(buf, out_len);
    XRT_FREE(buf);
    return out;
}

static inline XrValue xrt_compress_deflate_with_level(const char *data, int64_t len, int level) {
    XrCompressCoreInputView input;
    if (!xr_compress_core_input_view(data, len, &input))
        return XR_NULL_VAL;

    size_t out_len = 0;
    uint8_t *buf =
        xr_compress_core_deflate_alloc(input.data, input.len, &out_len, level,
                                       xrt_compress_core_alloc, xrt_compress_core_free, NULL);
    if (!buf)
        return XR_NULL_VAL;

    XrValue out = xrt_compress_finish_string(buf, out_len);
    XRT_FREE(buf);
    return out;
}

static inline XrValue xrt_compress_deflate_default(const char *data, int64_t len) {
    return xrt_compress_deflate_with_level(data, len, XR_COMPRESS_DEFAULT_COMPRESSION);
}

static inline XrValue xrt_compress_deflate(const char *data, int64_t len, XrValue level) {
    return xrt_compress_deflate_with_level(data, len, xrt_compress_level_or_default(level));
}

static inline XrValue xrt_compress_inflate(const char *data, int64_t len) {
    XrCompressCoreInputView input;
    if (!xr_compress_core_input_view(data, len, &input))
        return XR_NULL_VAL;

    size_t out_len = 0;
    uint8_t *buf = xr_compress_core_inflate_alloc(
        input.data, input.len, &out_len, xrt_compress_core_alloc, xrt_compress_core_free, NULL);
    if (!buf)
        return XR_NULL_VAL;

    XrValue out = xrt_compress_finish_string(buf, out_len);
    XRT_FREE(buf);
    return out;
}

static inline XrValue xrt_compress_zlib_compress_with_level(const char *data, int64_t len,
                                                            int level) {
    XrCompressCoreInputView input;
    if (!xr_compress_core_input_view(data, len, &input))
        return XR_NULL_VAL;

    size_t out_len = 0;
    uint8_t *buf =
        xr_compress_core_zlib_compress_alloc(input.data, input.len, &out_len, level,
                                             xrt_compress_core_alloc, xrt_compress_core_free, NULL);
    if (!buf)
        return XR_NULL_VAL;

    XrValue out = xrt_compress_finish_string(buf, out_len);
    XRT_FREE(buf);
    return out;
}

static inline XrValue xrt_compress_zlib_compress_default(const char *data, int64_t len) {
    return xrt_compress_zlib_compress_with_level(data, len, XR_COMPRESS_DEFAULT_COMPRESSION);
}

static inline XrValue xrt_compress_zlib_compress(const char *data, int64_t len, XrValue level) {
    return xrt_compress_zlib_compress_with_level(data, len, xrt_compress_level_or_default(level));
}

static inline XrValue xrt_compress_zlib_decompress(const char *data, int64_t len) {
    XrCompressCoreInputView input;
    if (!xr_compress_core_input_view(data, len, &input))
        return XR_NULL_VAL;

    size_t out_len = 0;
    uint8_t *buf = xr_compress_core_zlib_decompress_alloc(
        input.data, input.len, &out_len, xrt_compress_core_alloc, xrt_compress_core_free, NULL);
    if (!buf)
        return XR_NULL_VAL;

    XrValue out = xrt_compress_finish_string(buf, out_len);
    XRT_FREE(buf);
    return out;
}

static inline XrValue xrt_compress_is_gzip(const char *data, int64_t len) {
    XrCompressCoreInputView input;
    if (!xr_compress_core_input_view(data, len, &input))
        return XR_FALSE_VAL;
    return XR_FROM_BOOL(xr_is_gzip(input.data, input.len));
}

static inline XrValue xrt_compress_is_zlib(const char *data, int64_t len) {
    XrCompressCoreInputView input;
    if (!xr_compress_core_input_view(data, len, &input))
        return XR_FALSE_VAL;
    return XR_FROM_BOOL(xr_is_zlib(input.data, input.len));
}

static inline XrValue xrt_compress_crc32(const char *data, int64_t len) {
    XrCompressCoreInputView input;
    if (!xr_compress_core_input_view(data, len, &input))
        return XR_FROM_INT(0);
    return XR_FROM_INT((int64_t) xr_compress_core_crc32(input.data, input.len));
}

static inline XrValue xrt_compress_adler32(const char *data, int64_t len) {
    XrCompressCoreInputView input;
    if (!xr_compress_core_input_view(data, len, &input))
        return XR_FROM_INT(1);
    return XR_FROM_INT((int64_t) xr_compress_core_adler32(input.data, input.len));
}

#endif  // XRT_COMPRESS_H
