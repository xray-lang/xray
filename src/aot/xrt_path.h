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

static inline XrValue xrt_path_join(int64_t count_i, const char **parts, const size_t *lens) {
    size_t count = count_i < 0 ? 0 : (size_t) count_i;
    size_t out_len = 0;
    if (!xr_path_core_join_len(parts, lens, count, &out_len))
        return XR_NULL_VAL;
    XrValue result = xrt_str_alloc(out_len);
    xr_path_core_join_write(parts, lens, count, xr_str_buf(result));
    return result;
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

static inline bool xrt_path_normalize_temp(const char *path, size_t path_len, char **out,
                                           size_t *out_len) {
    if (!out || !out_len)
        return false;
    *out = NULL;
    *out_len = 0;
    size_t max_segs = xr_path_core_normalize_segment_cap(path_len);
    size_t *seg_buf = (size_t *) XRT_MALLOC(sizeof(size_t) * max_segs * 2);
    if (!seg_buf)
        return false;

    size_t *seg_starts = seg_buf;
    size_t *seg_lens = seg_buf + max_segs;
    size_t seg_count = 0;
    bool is_absolute = false;
    bool ok = xr_path_core_normalize_plan(path, path_len, seg_starts, seg_lens, max_segs,
                                          &seg_count, &is_absolute, out_len);
    if (!ok) {
        XRT_FREE(seg_buf);
        return false;
    }

    char *result = (char *) XRT_MALLOC(*out_len + 1);
    if (!result) {
        XRT_FREE(seg_buf);
        return false;
    }
    xr_path_core_normalize_write(path, seg_starts, seg_lens, seg_count, is_absolute, result);
    XRT_FREE(seg_buf);
    *out = result;
    return true;
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

static inline XrValue xrt_path_relative(const char *from, int64_t from_len_i, const char *to,
                                        int64_t to_len_i) {
    size_t from_len = (!from || from_len_i < 0) ? 0 : (size_t) from_len_i;
    size_t to_len = (!to || to_len_i < 0) ? 0 : (size_t) to_len_i;
    char *from_norm = NULL;
    char *to_norm = NULL;
    size_t from_norm_len = 0;
    size_t to_norm_len = 0;
    if (!xrt_path_normalize_temp(from, from_len, &from_norm, &from_norm_len))
        return XR_NULL_VAL;
    if (!xrt_path_normalize_temp(to, to_len, &to_norm, &to_norm_len)) {
        XRT_FREE(from_norm);
        return XR_NULL_VAL;
    }

    XrPathCoreRelativePlan plan;
    if (!xr_path_core_relative_plan(from_norm, from_norm_len, to_norm, to_norm_len, &plan)) {
        XRT_FREE(from_norm);
        XRT_FREE(to_norm);
        return XR_NULL_VAL;
    }

    XrValue result = xrt_str_alloc(plan.out_len);
    xr_path_core_relative_write(to_norm, &plan, xr_str_buf(result));
    XRT_FREE(from_norm);
    XRT_FREE(to_norm);
    return result;
}

#endif  // XRT_PATH_H
