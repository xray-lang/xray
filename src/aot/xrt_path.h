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

#include "xrt_arc.h"
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

static inline XrValue xrt_path_normalize(const char *path, int64_t len) {
    size_t path_len = (!path || len < 0) ? 0 : (size_t) len;
    size_t max_segs = xr_path_core_normalize_segment_cap(path_len);
    size_t *seg_buf = (size_t *) XRT_MALLOC(sizeof(size_t) * max_segs * 2);
    if (!seg_buf)
        return XR_NULL_VAL;

    size_t *seg_starts = seg_buf;
    size_t *seg_lens = seg_buf + max_segs;
    size_t seg_count = 0;
    bool is_absolute = false;
    size_t out_len = 0;
    bool ok = xr_path_core_normalize_plan(path, path_len, seg_starts, seg_lens, max_segs,
                                          &seg_count, &is_absolute, &out_len);
    if (!ok) {
        XRT_FREE(seg_buf);
        return XR_NULL_VAL;
    }

    XrValue result = xrt_str_alloc(out_len);
    xr_path_core_normalize_write(path, seg_starts, seg_lens, seg_count, is_absolute,
                                 xr_str_buf(result));
    XRT_FREE(seg_buf);
    return result;
}

#endif  // XRT_PATH_H
