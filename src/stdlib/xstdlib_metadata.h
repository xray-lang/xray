/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstdlib_metadata.h - helpers over generated stdlib metadata
 */

#ifndef XSTDLIB_METADATA_H
#define XSTDLIB_METADATA_H

#include "xstdlib_defs_generated.h"
#include <string.h>

static inline bool xr_stdlib_metadata_link_dependency_module_known(const char *name) {
    if (!name || !name[0] || name[0] == '.')
        return false;
    static const char *modules[] = {
        "regex", "math", "time",   "datetime", "path",     "io",  "os",
        "net",   "http", "crypto", "base64",   "encoding", "url", "csv",
        "toml",  "yaml", "xml",    "compress", "log",
    };
    for (uint32_t i = 0; i < (uint32_t) (sizeof(modules) / sizeof(modules[0])); i++) {
        if (strcmp(name, modules[i]) == 0)
            return true;
    }
    return false;
}

#endif  // XSTDLIB_METADATA_H
