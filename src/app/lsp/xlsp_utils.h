/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_utils.h - Common LSP utility functions
 *
 * KEY CONCEPT:
 *   Shared constants and helper functions used across LSP modules.
 */

#ifndef XLSP_UTILS_H
#define XLSP_UTILS_H

#include "../../runtime/value/xtype.h"
#include "../../shared/xr_param_mode.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Maximum path length for LSP operations
#define XLSP_MAX_PATH 1024

// Convert file:// URI to filesystem path (returns pointer into uri, no allocation)
static inline const char *xlsp_uri_to_path(const char *uri) {
    if (!uri)
        return NULL;
    if (strncmp(uri, "file://", 7) == 0) {
        const char *path = uri + 7;
#ifdef _WIN32
        /* A standard Windows file URI is file:///C:/path. Win32 path APIs
         * accept C:/path, not the URI's leading slash. */
        if (path[0] == '/' &&
            ((path[1] >= 'A' && path[1] <= 'Z') || (path[1] >= 'a' && path[1] <= 'z')) &&
            path[2] == ':') {
            path++;
        }
#endif
        return path;
    }
    return uri;
}

static inline XrParamMode xlsp_function_param_mode(XrType *function_type, int index) {
    if (!function_type || function_type->kind != XR_KIND_FUNCTION)
        return XR_PARAM_READ;
    return xr_type_function_param_mode(function_type, index);
}

static inline int xlsp_append_param_display(char *buf, size_t cap, int len, const char *name,
                                            XrType *type, XrParamMode mode) {
    if (!buf || cap == 0)
        return len;
    if (len < 0)
        len = 0;
    if ((size_t) len >= cap)
        return len;

    const char *pname = name ? name : "_";
    const char *ptype = type ? xr_type_to_string(type) : "<error>";
    if (mode != XR_PARAM_READ && xr_param_mode_is_valid(mode)) {
        return len + snprintf(buf + len, cap - (size_t) len, "%s: %s %s", pname,
                              xr_param_mode_label(mode), ptype);
    }
    return len + snprintf(buf + len, cap - (size_t) len, "%s: %s", pname, ptype);
}

#endif  // XLSP_UTILS_H
