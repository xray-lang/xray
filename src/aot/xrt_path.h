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
#include "xrt_coll.h"
#include "../shared/xr_path_core.h"

#if defined(XR_OS_WINDOWS)
#include <direct.h>
#define xrt_path_getcwd _getcwd
#else
#include <limits.h>
#include <unistd.h>
#define xrt_path_getcwd getcwd
#endif

#ifndef XRT_PATH_MAX
#if defined(XR_OS_WINDOWS)
#define XRT_PATH_MAX 4096
#elif defined(PATH_MAX)
#define XRT_PATH_MAX PATH_MAX
#else
#define XRT_PATH_MAX 4096
#endif
#endif

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

static inline XrValue xrt_path_normalize_joined(const char *path, size_t path_len) {
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

static inline XrValue xrt_path_join_then_normalize(const char **parts, const size_t *lens,
                                                   size_t count) {
    size_t joined_len = 0;
    if (!xr_path_core_join_len(parts, lens, count, &joined_len))
        return XR_NULL_VAL;
    char *joined = (char *) XRT_MALLOC(joined_len + 1);
    if (!joined)
        return XR_NULL_VAL;
    xr_path_core_join_write(parts, lens, count, joined);
    XrValue result = xrt_path_normalize_joined(joined, joined_len);
    XRT_FREE(joined);
    return result;
}

static inline XrValue xrt_path_resolve(int64_t count_i, const char **parts, const size_t *lens) {
    size_t count = count_i < 0 ? 0 : (size_t) count_i;
    if (xr_path_core_join_has_absolute(parts, lens, count))
        return xrt_path_join_then_normalize(parts, lens, count);

    enum {
        XRT_PATH_RESOLVE_STACK_PARTS = 17
    };
    const char *stack_parts[XRT_PATH_RESOLVE_STACK_PARTS];
    size_t stack_lens[XRT_PATH_RESOLVE_STACK_PARTS];
    const char **all_parts = stack_parts;
    size_t *all_lens = stack_lens;
    void *heap_buf = NULL;
    size_t total = count + 1;
    if (total > XRT_PATH_RESOLVE_STACK_PARTS) {
        heap_buf = XRT_MALLOC(sizeof(char *) * total + sizeof(size_t) * total);
        if (!heap_buf)
            return XR_NULL_VAL;
        all_parts = (const char **) heap_buf;
        all_lens = (size_t *) (all_parts + total);
    }

    char cwd[XRT_PATH_MAX];
    if (!xrt_path_getcwd(cwd, sizeof(cwd))) {
        cwd[0] = '/';
        cwd[1] = '\0';
    }
    all_parts[0] = cwd;
    all_lens[0] = strlen(cwd);
    for (size_t i = 0; i < count; i++) {
        all_parts[i + 1] = parts ? parts[i] : NULL;
        all_lens[i + 1] = lens ? lens[i] : 0;
    }

    XrValue result = xrt_path_join_then_normalize(all_parts, all_lens, total);
    XRT_FREE(heap_buf);
    return result;
}

static inline XrValue xrt_path_string_from_slice(XrPathCoreSlice slice) {
    XrValue result = xrt_str_alloc(slice.len);
    if (slice.len > 0 && slice.data)
        memcpy(xr_str_buf(result), slice.data, slice.len);
    return result;
}

static inline XrValue xrt_path_parse(const char *path, int64_t len_i) {
    size_t len = (!path || len_i < 0) ? 0 : (size_t) len_i;
    XrPathCoreParsePlan plan;
    if (!xr_path_core_parse_plan(path, len, &plan))
        return XR_NULL_VAL;

    static const char *const fields[] = {"root", "dir", "base", "name", "ext"};
    XrValue obj = xrt_json_new_named(5, fields);
    xrt_json_set_field(obj, 0, xrt_path_string_from_slice(plan.root));
    xrt_json_set_field(obj, 1, xrt_path_string_from_slice(plan.dir));
    xrt_json_set_field(obj, 2, xrt_path_string_from_slice(plan.base));
    xrt_json_set_field(obj, 3, xrt_path_string_from_slice(plan.name));
    xrt_json_set_field(obj, 4, xrt_path_string_from_slice(plan.ext));
    return obj;
}

static inline XrPathCoreSlice xrt_path_string_view(XrValue value) {
    if (!XR_IS_STR(value) || !value.ptr)
        return xr_path_core_slice(NULL, 0);
    int64_t len = xr_str_len(value);
    if (len <= 0)
        return xr_path_core_slice(xr_str_data(value), 0);
    return xr_path_core_slice(xr_str_data(value), (size_t) len);
}

static inline void xrt_path_copy_slice(char *out, size_t *pos, XrPathCoreSlice slice) {
    if (slice.len > 0 && slice.data) {
        memcpy(out + *pos, slice.data, slice.len);
        *pos += slice.len;
    }
}

static inline XrValue xrt_path_format(XrValue obj) {
    if (obj.tag != XR_TAG_PTR || !obj.ptr)
        return xrt_str_alloc(0);

    XrPathCoreSlice dir = xrt_path_string_view(xrt_json_get_name(obj, "dir"));
    XrPathCoreSlice base = xrt_path_string_view(xrt_json_get_name(obj, "base"));
    XrPathCoreSlice name = xrt_path_string_view(xrt_json_get_name(obj, "name"));
    XrPathCoreSlice ext = xrt_path_string_view(xrt_json_get_name(obj, "ext"));

    XrPathCoreSlice base_a = base;
    XrPathCoreSlice base_b = xr_path_core_slice(NULL, 0);
    if (base.len == 0 && name.len > 0) {
        base_a = name;
        base_b = ext;
    }

    size_t base_len = base_a.len + base_b.len;
    if (dir.len > 0) {
        bool need_sep = !xr_path_core_is_sep(dir.data[dir.len - 1]);
        XrValue result = xrt_str_alloc(dir.len + (need_sep ? 1 : 0) + base_len);
        char *out = xr_str_buf(result);
        size_t pos = 0;
        xrt_path_copy_slice(out, &pos, dir);
        if (need_sep)
            out[pos++] = '/';
        xrt_path_copy_slice(out, &pos, base_a);
        xrt_path_copy_slice(out, &pos, base_b);
        out[pos] = '\0';
        return result;
    }

    XrValue result = xrt_str_alloc(base_len);
    char *out = xr_str_buf(result);
    size_t pos = 0;
    xrt_path_copy_slice(out, &pos, base_a);
    xrt_path_copy_slice(out, &pos, base_b);
    out[pos] = '\0';
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
    return xrt_path_normalize_joined(path, path_len);
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
