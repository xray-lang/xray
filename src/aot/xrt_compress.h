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
    int out = XR_COMPRESS_DEFAULT_COMPRESSION;
    if (XR_IS_INT(level)) {
        int64_t raw = XR_TO_INT(level);
        if (raw < 0)
            out = 0;
        else if (raw > 9)
            out = 9;
        else
            out = (int) raw;
    }
    return out;
}

static inline const uint8_t *xrt_compress_input(const char *data, int64_t len) {
    if (!data && len != 0)
        return NULL;
    if (len < 0)
        return NULL;
    return (const uint8_t *) (data ? data : "");
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
    const uint8_t *input = xrt_compress_input(data, len);
    if (!input)
        return XR_NULL_VAL;

    size_t bound = xr_deflate_bound((size_t) len) + 18;
    uint8_t *buf = (uint8_t *) XRT_MALLOC(bound);
    if (!buf)
        return XR_NULL_VAL;

    size_t out_len = 0;
    XrCompressError err = xr_gzip(input, (size_t) len, buf, bound, &out_len, level);
    if (err != XR_COMPRESS_OK) {
        XRT_FREE(buf);
        return XR_NULL_VAL;
    }

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
    const uint8_t *input = xrt_compress_input(data, len);
    if (!input)
        return XR_NULL_VAL;

    uint32_t orig_size = xr_gzip_original_size(input, (size_t) len);
    if (orig_size == 0)
        orig_size = (uint32_t) ((size_t) len * 4);

    size_t cap = (size_t) orig_size + 256;
    for (int tries = 0; tries < 4; tries++) {
        uint8_t *buf = (uint8_t *) XRT_MALLOC(cap);
        if (!buf)
            return XR_NULL_VAL;

        size_t out_len = 0;
        XrCompressError err = xr_gunzip(input, (size_t) len, buf, cap, &out_len);
        if (err == XR_COMPRESS_OK) {
            XrValue out = xrt_compress_finish_string(buf, out_len);
            XRT_FREE(buf);
            return out;
        }
        XRT_FREE(buf);
        if (err != XR_COMPRESS_ERR_BUFFER)
            return XR_NULL_VAL;
        cap *= 2;
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_compress_deflate_with_level(const char *data, int64_t len, int level) {
    const uint8_t *input = xrt_compress_input(data, len);
    if (!input)
        return XR_NULL_VAL;

    size_t bound = xr_deflate_bound((size_t) len);
    uint8_t *buf = (uint8_t *) XRT_MALLOC(bound);
    if (!buf)
        return XR_NULL_VAL;

    size_t out_len = 0;
    XrCompressError err = xr_deflate(input, (size_t) len, buf, bound, &out_len, level);
    if (err != XR_COMPRESS_OK) {
        XRT_FREE(buf);
        return XR_NULL_VAL;
    }

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
    const uint8_t *input = xrt_compress_input(data, len);
    if (!input)
        return XR_NULL_VAL;

    size_t cap = (size_t) len * 8 + 1024;
    for (int tries = 0; tries < 8; tries++) {
        uint8_t *buf = (uint8_t *) XRT_MALLOC(cap);
        if (!buf)
            return XR_NULL_VAL;

        size_t out_len = 0;
        XrCompressError err = xr_inflate(input, (size_t) len, buf, cap, &out_len);
        if (err == XR_COMPRESS_OK) {
            XrValue out = xrt_compress_finish_string(buf, out_len);
            XRT_FREE(buf);
            return out;
        }
        XRT_FREE(buf);
        if (err != XR_COMPRESS_ERR_BUFFER)
            return XR_NULL_VAL;
        cap *= 2;
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_compress_zlib_compress_with_level(const char *data, int64_t len,
                                                            int level) {
    const uint8_t *input = xrt_compress_input(data, len);
    if (!input)
        return XR_NULL_VAL;

    size_t bound = xr_deflate_bound((size_t) len) + 6;
    uint8_t *buf = (uint8_t *) XRT_MALLOC(bound);
    if (!buf)
        return XR_NULL_VAL;

    size_t out_len = 0;
    XrCompressError err = xr_zlib_compress(input, (size_t) len, buf, bound, &out_len, level);
    if (err != XR_COMPRESS_OK) {
        XRT_FREE(buf);
        return XR_NULL_VAL;
    }

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
    const uint8_t *input = xrt_compress_input(data, len);
    if (!input)
        return XR_NULL_VAL;

    size_t cap = (size_t) len * 8 + 1024;
    for (int tries = 0; tries < 8; tries++) {
        uint8_t *buf = (uint8_t *) XRT_MALLOC(cap);
        if (!buf)
            return XR_NULL_VAL;

        size_t out_len = 0;
        XrCompressError err = xr_zlib_decompress(input, (size_t) len, buf, cap, &out_len);
        if (err == XR_COMPRESS_OK) {
            XrValue out = xrt_compress_finish_string(buf, out_len);
            XRT_FREE(buf);
            return out;
        }
        XRT_FREE(buf);
        if (err != XR_COMPRESS_ERR_BUFFER)
            return XR_NULL_VAL;
        cap *= 2;
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_compress_is_gzip(const char *data, int64_t len) {
    const uint8_t *input = xrt_compress_input(data, len);
    if (!input)
        return XR_FALSE_VAL;
    return XR_FROM_BOOL(xr_is_gzip(input, (size_t) len));
}

static inline XrValue xrt_compress_is_zlib(const char *data, int64_t len) {
    const uint8_t *input = xrt_compress_input(data, len);
    if (!input)
        return XR_FALSE_VAL;
    return XR_FROM_BOOL(xr_is_zlib(input, (size_t) len));
}

static inline XrValue xrt_compress_crc32(const char *data, int64_t len) {
    if (!data && len != 0)
        return XR_FROM_INT(0);
    size_t n = len < 0 ? 0 : (size_t) len;
    return XR_FROM_INT((int64_t) xr_compress_core_crc32((const uint8_t *) data, n));
}

static inline XrValue xrt_compress_adler32(const char *data, int64_t len) {
    if (!data && len != 0)
        return XR_FROM_INT(1);
    size_t n = len < 0 ? 0 : (size_t) len;
    return XR_FROM_INT((int64_t) xr_compress_core_adler32((const uint8_t *) data, n));
}

#endif  // XRT_COMPRESS_H
