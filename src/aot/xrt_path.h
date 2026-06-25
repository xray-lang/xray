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
#include <unistd.h>
#define xrt_path_getcwd getcwd
#endif

static inline void *xrt_path_core_alloc(void *ctx, size_t size) {
    (void) ctx;
    return XRT_MALLOC(size);
}

static inline void xrt_path_core_free(void *ctx, void *ptr) {
    (void) ctx;
    XRT_FREE(ptr);
}

static inline XrValue xrt_path_sep(void) {
    return xrt_str_from_cstr(xr_path_core_sep_str());
}

static inline XrValue xrt_path_delimiter(void) {
    return xrt_str_from_cstr(xr_path_core_delimiter_str());
}

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
    size_t out_len = 0;
    char *normalized = NULL;
    if (!xr_path_core_normalize_alloc(path, path_len, xrt_path_core_alloc, xrt_path_core_free, NULL,
                                      &normalized, &out_len))
        return XR_NULL_VAL;

    XrValue result = xrt_str_alloc(out_len);
    memcpy(xr_str_buf(result), normalized, out_len);
    XRT_FREE(normalized);
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

    char cwd[XR_PATH_CORE_MAX_PATH];
    size_t cwd_len = 0;
    if (xr_path_core_resolve_needs_cwd(parts, lens, count)) {
        if (!xrt_path_getcwd(cwd, sizeof(cwd)))
            xr_path_core_resolve_fallback_cwd(cwd, sizeof(cwd));
        cwd_len = strlen(cwd);
    }
    for (size_t i = 0; i < count; i++) {
        all_parts[i + 1] = parts ? parts[i] : NULL;
        all_lens[i + 1] = lens ? lens[i] : 0;
    }

    size_t resolve_count = 0;
    if (!xr_path_core_resolve_parts(all_parts + 1, all_lens + 1, count, cwd, cwd_len, all_parts,
                                    all_lens, total, &resolve_count)) {
        XRT_FREE(heap_buf);
        return XR_NULL_VAL;
    }
    XrValue result = xrt_path_join_then_normalize(all_parts, all_lens, resolve_count);
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

    XrValue obj =
        xrt_json_new_named(XR_PATH_CORE_PARSE_FIELD_COUNT, xr_path_core_parse_field_names());
    for (int i = 0; i < XR_PATH_CORE_PARSE_FIELD_COUNT; i++) {
        XrPathCoreSlice field = xr_path_core_parse_plan_field(&plan, (XrPathCoreParseField) i);
        xrt_json_set_field(obj, i, xrt_path_string_from_slice(field));
    }
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

static inline XrValue xrt_path_format(XrValue obj) {
    if (obj.tag != XR_TAG_PTR || !obj.ptr)
        return xrt_str_alloc(0);

    XrPathCoreSlice dir = xrt_path_string_view(xrt_json_get_name(obj, "dir"));
    XrPathCoreSlice base = xrt_path_string_view(xrt_json_get_name(obj, "base"));
    XrPathCoreSlice name = xrt_path_string_view(xrt_json_get_name(obj, "name"));
    XrPathCoreSlice ext = xrt_path_string_view(xrt_json_get_name(obj, "ext"));

    XrPathCoreFormatPlan plan;
    if (!xr_path_core_format_plan(dir, base, name, ext, &plan))
        return XR_NULL_VAL;
    XrValue result = xrt_str_alloc(plan.out_len);
    xr_path_core_format_write(&plan, xr_str_buf(result));
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
    return xr_path_core_normalize_alloc(path, path_len, xrt_path_core_alloc, xrt_path_core_free,
                                        NULL, out, out_len);
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
