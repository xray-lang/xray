/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_path_core.h - Pure path string helpers shared by VM stdlib and AOT
 */

#ifndef XR_PATH_CORE_H
#define XR_PATH_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../base/xplatform.h"

static inline bool xr_path_core_is_sep(char c) {
#ifdef XR_OS_WINDOWS
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static inline const char *xr_path_core_sep_str(void) {
    return "/";
}

static inline const char *xr_path_core_delimiter_str(void) {
#ifdef XR_OS_WINDOWS
    return ";";
#else
    return ":";
#endif
}

static inline bool xr_path_core_is_absolute(const char *path, size_t len);

static inline bool xr_path_core_join_has_absolute(const char **parts, const size_t *lens,
                                                  size_t count) {
    for (size_t i = 0; i < count; i++) {
        const char *part = parts ? parts[i] : NULL;
        size_t len = lens ? lens[i] : 0;
        if (part && len > 0 && xr_path_core_is_absolute(part, len))
            return true;
    }
    return false;
}

static inline size_t xr_path_core_join_start_index(const char **parts, const size_t *lens,
                                                   size_t count) {
    size_t start = 0;
    for (size_t i = 0; i < count; i++) {
        const char *part = parts ? parts[i] : NULL;
        size_t len = lens ? lens[i] : 0;
        if (part && len > 0 && xr_path_core_is_absolute(part, len))
            start = i;
    }
    return start;
}

static inline bool xr_path_core_join_len(const char **parts, const size_t *lens, size_t count,
                                         size_t *out_len) {
    if (!out_len)
        return false;
    size_t pos = 0;
    char last = '\0';
    size_t start_index = xr_path_core_join_start_index(parts, lens, count);
    for (size_t i = start_index; i < count; i++) {
        const char *part = parts ? parts[i] : NULL;
        size_t len = lens ? lens[i] : 0;
        if (!part || len == 0)
            continue;

        if (pos > 0 && !xr_path_core_is_sep(last) && !xr_path_core_is_sep(part[0])) {
            pos++;
            last = '/';
        }

        size_t start = 0;
        if (pos > 0 && xr_path_core_is_sep(part[0]))
            start = 1;
        size_t copy_len = len - start;
        if (copy_len > 0) {
            pos += copy_len;
            last = part[len - 1];
        }
    }
    *out_len = pos;
    return true;
}

static inline void xr_path_core_join_write(const char **parts, const size_t *lens, size_t count,
                                           char *out) {
    size_t pos = 0;
    size_t start_index = xr_path_core_join_start_index(parts, lens, count);
    for (size_t i = start_index; i < count; i++) {
        const char *part = parts ? parts[i] : NULL;
        size_t len = lens ? lens[i] : 0;
        if (!part || len == 0)
            continue;

        if (pos > 0 && !xr_path_core_is_sep(out[pos - 1]) && !xr_path_core_is_sep(part[0]))
            out[pos++] = '/';

        size_t start = 0;
        if (pos > 0 && xr_path_core_is_sep(part[0]))
            start = 1;
        size_t copy_len = len - start;
        if (copy_len > 0) {
            memcpy(out + pos, part + start, copy_len);
            pos += copy_len;
        }
    }
    out[pos] = '\0';
}

static inline const char *xr_path_core_dirname(const char *path, size_t len, size_t *out_len) {
    if (!out_len)
        return ".";
    if (!path || len == 0) {
        *out_len = 1;
        return ".";
    }
    while (len > 0 && xr_path_core_is_sep(path[len - 1]))
        len--;
    while (len > 0 && !xr_path_core_is_sep(path[len - 1]))
        len--;
    while (len > 1 && xr_path_core_is_sep(path[len - 1]))
        len--;
    if (len == 0) {
        *out_len = 1;
        return xr_path_core_is_sep(path[0]) ? xr_path_core_sep_str() : ".";
    }
    *out_len = len;
    return path;
}

static inline const char *xr_path_core_basename(const char *path, size_t len, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!path || len == 0)
        return "";
    while (len > 0 && xr_path_core_is_sep(path[len - 1]))
        len--;
    if (len == 0)
        return "";
    size_t start = len;
    while (start > 0 && !xr_path_core_is_sep(path[start - 1]))
        start--;
    if (out_len)
        *out_len = len - start;
    return path + start;
}

static inline const char *xr_path_core_extname(const char *path, size_t plen, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!path || plen == 0)
        return "";
    size_t len = plen;
    while (len > 0 && xr_path_core_is_sep(path[len - 1]))
        len--;
    size_t start = len;
    while (start > 0 && !xr_path_core_is_sep(path[start - 1]))
        start--;

    const char *base = path + start;
    size_t base_len = len - start;
    const char *dot = NULL;
    for (size_t i = base_len; i > 0; i--) {
        if (base[i - 1] == '.') {
            if (i > 1)
                dot = base + i - 1;
            break;
        }
    }
    if (!dot)
        return "";
    if (out_len)
        *out_len = (size_t) ((path + len) - dot);
    return dot;
}

typedef struct XrPathCoreSlice {
    const char *data;
    size_t len;
} XrPathCoreSlice;

typedef struct XrPathCoreParsePlan {
    XrPathCoreSlice root;
    XrPathCoreSlice dir;
    XrPathCoreSlice base;
    XrPathCoreSlice name;
    XrPathCoreSlice ext;
} XrPathCoreParsePlan;

static inline XrPathCoreSlice xr_path_core_slice(const char *data, size_t len) {
    XrPathCoreSlice s;
    s.data = data;
    s.len = len;
    return s;
}

static inline bool xr_path_core_parse_plan(const char *path, size_t len,
                                           XrPathCoreParsePlan *plan) {
    if (!plan)
        return false;
    if (!path) {
        path = "";
        len = 0;
    }

    size_t dir_len = 0;
    size_t base_len = 0;
    size_t ext_len = 0;
    const char *dir = xr_path_core_dirname(path, len, &dir_len);
    const char *base = xr_path_core_basename(path, len, &base_len);
    const char *ext = xr_path_core_extname(base, base_len, &ext_len);

    size_t name_len = base_len;
    if (ext_len > 0)
        name_len = (size_t) (ext - base);

    plan->root = (len > 0 && xr_path_core_is_sep(path[0]))
                     ? xr_path_core_slice(xr_path_core_sep_str(), 1)
                     : xr_path_core_slice("", 0);
    plan->dir = xr_path_core_slice(dir, dir_len);
    plan->base = xr_path_core_slice(base, base_len);
    plan->name = xr_path_core_slice(base, name_len);
    plan->ext = xr_path_core_slice(ext, ext_len);
    return true;
}

typedef struct XrPathCoreFormatPlan {
    XrPathCoreSlice dir;
    XrPathCoreSlice base_a;
    XrPathCoreSlice base_b;
    bool need_sep;
    size_t out_len;
} XrPathCoreFormatPlan;

static inline XrPathCoreSlice xr_path_core_sanitize_slice(XrPathCoreSlice slice) {
    if (!slice.data)
        slice.len = 0;
    return slice;
}

static inline bool xr_path_core_format_plan(XrPathCoreSlice dir, XrPathCoreSlice base,
                                            XrPathCoreSlice name, XrPathCoreSlice ext,
                                            XrPathCoreFormatPlan *plan) {
    if (!plan)
        return false;
    dir = xr_path_core_sanitize_slice(dir);
    base = xr_path_core_sanitize_slice(base);
    name = xr_path_core_sanitize_slice(name);
    ext = xr_path_core_sanitize_slice(ext);

    XrPathCoreSlice base_a = base;
    XrPathCoreSlice base_b = xr_path_core_slice(NULL, 0);
    if (base.len == 0 && name.len > 0) {
        base_a = name;
        base_b = ext;
    }

    size_t base_len = base_a.len + base_b.len;
    bool need_sep = dir.len > 0 && !xr_path_core_is_sep(dir.data[dir.len - 1]);
    size_t out_len = base_len;
    if (dir.len > 0)
        out_len += dir.len + (need_sep ? 1 : 0);

    plan->dir = dir;
    plan->base_a = base_a;
    plan->base_b = base_b;
    plan->need_sep = need_sep;
    plan->out_len = out_len;
    return true;
}

static inline void xr_path_core_copy_slice(char *out, size_t *pos, XrPathCoreSlice slice) {
    if (slice.len > 0 && slice.data) {
        memcpy(out + *pos, slice.data, slice.len);
        *pos += slice.len;
    }
}

static inline void xr_path_core_format_write(const XrPathCoreFormatPlan *plan, char *out) {
    size_t pos = 0;
    if (plan->dir.len > 0) {
        xr_path_core_copy_slice(out, &pos, plan->dir);
        if (plan->need_sep)
            out[pos++] = '/';
    }
    xr_path_core_copy_slice(out, &pos, plan->base_a);
    xr_path_core_copy_slice(out, &pos, plan->base_b);
    out[pos] = '\0';
}

static inline bool xr_path_core_is_absolute(const char *path, size_t len) {
    if (!path || len == 0)
        return false;
#ifdef XR_OS_WINDOWS
    if (len >= 2 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':')
        return true;
    if (len >= 2 && path[0] == '\\' && path[1] == '\\')
        return true;
#endif
    return path[0] == '/';
}

static inline size_t xr_path_core_normalize_segment_cap(size_t len) {
    return len / 2 + 2;
}

static inline bool xr_path_core_segment_is_dotdot(const char *path, const size_t *seg_starts,
                                                  const size_t *seg_lens, size_t idx) {
    return seg_lens[idx] == 2 && path[seg_starts[idx]] == '.' && path[seg_starts[idx] + 1] == '.';
}

static inline bool xr_path_core_normalize_plan(const char *path, size_t len, size_t *seg_starts,
                                               size_t *seg_lens, size_t seg_cap,
                                               size_t *out_seg_count, bool *out_is_absolute,
                                               size_t *out_len) {
    if (!out_seg_count || !out_is_absolute || !out_len)
        return false;

    *out_seg_count = 0;
    *out_is_absolute = false;
    *out_len = 1;
    if (!path || len == 0)
        return true;

    bool is_absolute = xr_path_core_is_sep(path[0]);
    size_t seg_count = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && xr_path_core_is_sep(path[i]))
            i++;
        if (i >= len)
            break;

        size_t seg_start = i;
        while (i < len && !xr_path_core_is_sep(path[i]))
            i++;
        size_t seg_len = i - seg_start;

        if (seg_len == 1 && path[seg_start] == '.') {
            continue;
        }
        if (seg_len == 2 && path[seg_start] == '.' && path[seg_start + 1] == '.') {
            if (seg_count > 0 &&
                !xr_path_core_segment_is_dotdot(path, seg_starts, seg_lens, seg_count - 1)) {
                seg_count--;
            } else if (!is_absolute) {
                if (seg_count >= seg_cap)
                    return false;
                seg_starts[seg_count] = seg_start;
                seg_lens[seg_count] = seg_len;
                seg_count++;
            }
            continue;
        }

        if (seg_count >= seg_cap)
            return false;
        seg_starts[seg_count] = seg_start;
        seg_lens[seg_count] = seg_len;
        seg_count++;
    }

    size_t result_len = is_absolute ? 1 : 0;
    for (size_t s = 0; s < seg_count; s++) {
        if (s > 0)
            result_len++;
        result_len += seg_lens[s];
    }
    if (result_len == 0)
        result_len = 1;

    *out_seg_count = seg_count;
    *out_is_absolute = is_absolute;
    *out_len = result_len;
    return true;
}

static inline void xr_path_core_normalize_write(const char *path, const size_t *seg_starts,
                                                const size_t *seg_lens, size_t seg_count,
                                                bool is_absolute, char *out) {
    size_t pos = 0;
    if (is_absolute)
        out[pos++] = '/';

    for (size_t s = 0; s < seg_count; s++) {
        if (s > 0)
            out[pos++] = '/';
        memcpy(out + pos, path + seg_starts[s], seg_lens[s]);
        pos += seg_lens[s];
    }

    if (pos == 0)
        out[pos++] = '.';
    out[pos] = '\0';
}

typedef void *(*XrPathCoreAllocFn)(void *ctx, size_t size);
typedef void (*XrPathCoreFreeFn)(void *ctx, void *ptr);

static inline bool xr_path_core_normalize_alloc(const char *path, size_t len,
                                                XrPathCoreAllocFn alloc_fn,
                                                XrPathCoreFreeFn free_fn, void *ctx, char **out,
                                                size_t *out_len) {
    if (!out || !out_len || !alloc_fn || !free_fn)
        return false;
    *out = NULL;
    *out_len = 0;
    if (!path)
        len = 0;

    size_t max_segs = xr_path_core_normalize_segment_cap(len);
    if (max_segs > (SIZE_MAX / sizeof(size_t)) / 2)
        return false;
    size_t seg_bytes = sizeof(size_t) * max_segs * 2;
    size_t *seg_buf = (size_t *) alloc_fn(ctx, seg_bytes);
    if (!seg_buf)
        return false;

    size_t *seg_starts = seg_buf;
    size_t *seg_lens = seg_buf + max_segs;
    size_t seg_count = 0;
    bool is_absolute = false;
    bool ok = xr_path_core_normalize_plan(path, len, seg_starts, seg_lens, max_segs, &seg_count,
                                          &is_absolute, out_len);
    if (!ok || *out_len == SIZE_MAX) {
        free_fn(ctx, seg_buf);
        return false;
    }

    char *result = (char *) alloc_fn(ctx, *out_len + 1);
    if (!result) {
        free_fn(ctx, seg_buf);
        return false;
    }
    xr_path_core_normalize_write(path, seg_starts, seg_lens, seg_count, is_absolute, result);
    free_fn(ctx, seg_buf);
    *out = result;
    return true;
}

typedef struct XrPathCoreRelativePlan {
    size_t up_count;
    size_t to_rest_start;
    size_t to_rest_len;
    size_t out_len;
} XrPathCoreRelativePlan;

static inline bool xr_path_core_relative_plan(const char *from, size_t from_len, const char *to,
                                              size_t to_len, XrPathCoreRelativePlan *plan) {
    if (!plan)
        return false;
    if (!from) {
        from = "";
        from_len = 0;
    }
    if (!to) {
        to = "";
        to_len = 0;
    }

    size_t common = 0;
    size_t last_sep = 0;
    while (common < from_len && common < to_len && from[common] == to[common]) {
        if (xr_path_core_is_sep(from[common]))
            last_sep = common;
        common++;
    }

    bool at_boundary = (common == from_len && common == to_len) ||
                       (common == from_len && common < to_len && xr_path_core_is_sep(to[common])) ||
                       (common == to_len && common < from_len && xr_path_core_is_sep(from[common]));
    if (!at_boundary)
        common = last_sep;

    size_t from_pos = common;
    while (from_pos < from_len && xr_path_core_is_sep(from[from_pos]))
        from_pos++;

    size_t up_count = 0;
    if (from_pos < from_len) {
        up_count = 1;
        for (size_t i = from_pos; i < from_len; i++) {
            if (xr_path_core_is_sep(from[i]))
                up_count++;
        }
    }

    size_t to_rest_start = common;
    while (to_rest_start < to_len && xr_path_core_is_sep(to[to_rest_start]))
        to_rest_start++;
    size_t to_rest_len = to_len - to_rest_start;

    size_t out_len = 0;
    if (up_count > 0)
        out_len += up_count * 2 + (up_count - 1);
    if (to_rest_len > 0) {
        if (out_len > 0)
            out_len++;
        out_len += to_rest_len;
    }
    if (out_len == 0)
        out_len = 1;

    plan->up_count = up_count;
    plan->to_rest_start = to_rest_start;
    plan->to_rest_len = to_rest_len;
    plan->out_len = out_len;
    return true;
}

static inline void xr_path_core_relative_write(const char *to, const XrPathCoreRelativePlan *plan,
                                               char *out) {
    size_t pos = 0;
    for (size_t i = 0; i < plan->up_count; i++) {
        if (pos > 0)
            out[pos++] = '/';
        out[pos++] = '.';
        out[pos++] = '.';
    }

    if (plan->to_rest_len > 0) {
        if (pos > 0)
            out[pos++] = '/';
        memcpy(out + pos, to + plan->to_rest_start, plan->to_rest_len);
        pos += plan->to_rest_len;
    }

    if (pos == 0)
        out[pos++] = '.';
    out[pos] = '\0';
}

#endif  // XR_PATH_CORE_H
