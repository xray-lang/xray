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

#include "../shared/xr_compress_core.h"
#include "xrt_value.h"

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
