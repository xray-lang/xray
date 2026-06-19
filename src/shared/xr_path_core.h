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
        *out_len = (size_t) ((path + plen) - dot);
    return dot;
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

#endif  // XR_PATH_CORE_H
