/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_path.h - Freestanding AOT path helpers
 */

#ifndef XRT_PATH_H
#define XRT_PATH_H

#include "xrt_value.h"
#include "../shared/xr_path_core.h"

static inline XrValue xrt_path_is_absolute(const char *path, int64_t len) {
    return XR_FROM_BOOL(xr_path_core_is_absolute(path, len < 0 ? 0 : (size_t) len));
}

static inline const char *xrt_path_dirname(const char *path, int64_t len, int64_t *out_len) {
    size_t rl = 0;
    const char *r = xr_path_core_dirname(path, len < 0 ? 0 : (size_t) len, &rl);
    if (out_len)
        *out_len = (int64_t) rl;
    return r;
}

static inline const char *xrt_path_basename(const char *path, int64_t len, int64_t *out_len) {
    size_t rl = 0;
    const char *r = xr_path_core_basename(path, len < 0 ? 0 : (size_t) len, &rl);
    if (out_len)
        *out_len = (int64_t) rl;
    return r;
}

static inline const char *xrt_path_extname(const char *path, int64_t len, int64_t *out_len) {
    size_t rl = 0;
    const char *r = xr_path_core_extname(path, len < 0 ? 0 : (size_t) len, &rl);
    if (out_len)
        *out_len = (int64_t) rl;
    return r;
}

#endif  // XRT_PATH_H
