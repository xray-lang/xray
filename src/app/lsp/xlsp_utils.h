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

#include <string.h>

// Maximum path length for LSP operations
#define XLSP_MAX_PATH 1024

// Convert file:// URI to filesystem path (returns pointer into uri, no allocation)
static inline const char *xlsp_uri_to_path(const char *uri) {
    if (!uri)
        return NULL;
    if (strncmp(uri, "file://", 7) == 0)
        return uri + 7;
    return uri;
}

#endif  // XLSP_UTILS_H
