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
        "regex", "math", "time",   "datetime", "path",     "io",   "os",
        "net",   "http", "crypto", "base64",   "encoding", "url",  "csv",
        "toml",  "yaml", "xml",    "compress", "log",      "text", "strconv",
    };
    for (uint32_t i = 0; i < (uint32_t) (sizeof(modules) / sizeof(modules[0])); i++) {
        if (strcmp(name, modules[i]) == 0)
            return true;
    }
    return false;
}

static inline uint32_t xr_stdlib_metadata_function_runtime_capabilities(const char *module,
                                                                        const char *name) {
    uint32_t capabilities = 0;
    if (!module || !name)
        return 0;
    for (uint32_t i = 0; i < XR_STDLIB_DEF_ENTRY_COUNT; i++) {
        const XrStdlibDefEntry *entry = &xr_stdlib_def_entries[i];
        if (entry->module && entry->name && strcmp(entry->module, module) == 0 &&
            strcmp(entry->name, name) == 0)
            capabilities |= entry->runtime_capabilities;
    }
    return capabilities;
}

static inline bool xr_stdlib_metadata_function_may_suspend(const char *module, const char *name) {
    if (!module || !name)
        return false;
    for (uint32_t i = 0; i < XR_STDLIB_DEF_ENTRY_COUNT; i++) {
        const XrStdlibDefEntry *entry = &xr_stdlib_def_entries[i];
        if (entry->module && entry->name && entry->vm_binding &&
            strcmp(entry->module, module) == 0 && strcmp(entry->name, name) == 0 &&
            strcmp(entry->vm_binding, "yieldable") == 0)
            return true;
    }
    return false;
}

#endif  // XSTDLIB_METADATA_H
