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
#include "xcompile_state.h"

/* ========== Source Cache Structures ========== */

typedef struct XrSourceFile {
    char *path;
    char *content;
    char **lines;
    int line_count;
} XrSourceFile;

typedef struct XrSourceCache {
    XrCompileState *state; /* retained; published storage survives the compiler producer */
    XrSourceFile *files;
    int file_count;
    int file_capacity;
} XrSourceCache;

/* ========== API ========== */

/* Compile producers require the caller's shared state. Output must be empty.
 * add publishes a complete file only after all storage and indexing succeed. */
XR_FUNC XrCompileResourceStatus xr_compile_source_cache_open(XrCompileState *state, XrSourceCache **output);
XR_FUNC XrCompileResourceStatus xr_compile_source_cache_add(XrSourceCache *cache, const char *path, const char *content);
XR_FUNC void xr_owned_source_cache_close(XrSourceCache *cache);

/* Runtime diagnostic reads use the same published storage without consuming
 * compiler work or consulting its sticky failure. They allocate nothing. */
XR_FUNC const char *xr_runtime_source_cache_get_line(const XrSourceCache *cache, const char *path, int line);
XR_FUNC int xr_runtime_source_cache_get_line_length(const XrSourceCache *cache, const char *path, int line);

#endif  // XSOURCE_CACHE_H
