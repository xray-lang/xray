/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_cache.h - Source code cache for error display
 *
 * KEY CONCEPT:
 *   Caches compiled source files for runtime error display.
 *   Supports line-by-line access for error context.
 */

#ifndef XSOURCE_CACHE_H
#define XSOURCE_CACHE_H

#include <stdbool.h>
#include "xdefs.h"

/* ========== Source Cache Structures ========== */

typedef struct XrSourceFile {
    char *path;
    char *content;
    char **lines;
    int line_count;
} XrSourceFile;

typedef struct XrSourceCache {
    XrSourceFile *files;
    int file_count;
    int file_capacity;
} XrSourceCache;

/* ========== API ========== */

XR_FUNC XrSourceCache* xr_source_cache_new(void);
XR_FUNC void xr_source_cache_free(XrSourceCache *cache);
XR_FUNC bool xr_source_cache_add(XrSourceCache *cache, const char *path, const char *content);
XR_FUNC const char* xr_source_cache_get_line(XrSourceCache *cache, const char *path, int line);
XR_FUNC int xr_source_cache_get_line_length(XrSourceCache *cache, const char *path, int line);

#endif // XSOURCE_CACHE_H
