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
#include "../base/xsha256.h"
#include <string.h>

static inline void xr_stdlib_metadata_hash_u64(XrSHA256Context *ctx, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_sha256_update(ctx, bytes, sizeof(bytes));
}

static inline void xr_stdlib_metadata_hash_string(XrSHA256Context *ctx, const char *value) {
    size_t length = value ? strlen(value) : 0;
    xr_stdlib_metadata_hash_u64(ctx, (uint64_t) length);
    if (length)
        xr_sha256_update(ctx, (const uint8_t *) value, length);
}

static inline void xr_stdlib_metadata_registry_fingerprint(XrFingerprint *out) {
    static const uint8_t domain[] = "xray-stdlib-definition-registry-v1\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    xr_stdlib_metadata_hash_u64(&ctx, XR_STDLIB_DEF_ENTRY_COUNT);
    for (uint32_t i = 0; i < XR_STDLIB_DEF_ENTRY_COUNT; i++) {
        const XrStdlibDefEntry *entry = &xr_stdlib_def_entries[i];
        xr_stdlib_metadata_hash_string(&ctx, entry->module);
        xr_stdlib_metadata_hash_string(&ctx, entry->name);
        xr_stdlib_metadata_hash_string(&ctx, entry->signature);
        xr_stdlib_metadata_hash_string(&ctx, entry->vm);
        xr_stdlib_metadata_hash_string(&ctx, entry->vm_binding);
        xr_stdlib_metadata_hash_string(&ctx, entry->vm_ifdef);
        xr_stdlib_metadata_hash_string(&ctx, entry->aot);
        xr_stdlib_metadata_hash_string(&ctx, entry->arg_spec);
        xr_stdlib_metadata_hash_string(&ctx, entry->ret);
        xr_stdlib_metadata_hash_string(&ctx, entry->aot_enum);
        xr_stdlib_metadata_hash_string(&ctx, entry->link_object);
        xr_stdlib_metadata_hash_string(&ctx, entry->define);
        xr_stdlib_metadata_hash_string(&ctx, entry->layer);
        xr_stdlib_metadata_hash_string(&ctx, entry->aot_kind);
        xr_stdlib_metadata_hash_u64(&ctx, entry->runtime_capabilities);
        xr_stdlib_metadata_hash_u64(&ctx, entry->argc);
        xr_stdlib_metadata_hash_u64(&ctx, entry->aot_direct ? 1u : 0u);
    }
    xr_sha256_final(&ctx, out->bytes);
}

static inline const XrStdlibDefEntry *
xr_stdlib_metadata_unique_func(const char *module, const char *name) {
    if (!module || !module[0] || module[0] == '.' || !name || !name[0])
        return NULL;
    const XrStdlibDefEntry *found = NULL;
    for (uint32_t i = 0; i < XR_STDLIB_DEF_ENTRY_COUNT; i++) {
        const XrStdlibDefEntry *entry = &xr_stdlib_def_entries[i];
        if (!entry->module || !entry->name || strcmp(entry->module, module) != 0 ||
            strcmp(entry->name, name) != 0)
            continue;
        if (found)
            return NULL;
        found = entry;
    }
    return found;
}

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

static inline bool xr_stdlib_metadata_func_is_yieldable(const char *module, const char *name) {
    const XrStdlibDefEntry *entry = xr_stdlib_metadata_unique_func(module, name);
    return entry && entry->signature && entry->vm && entry->vm_binding &&
           strcmp(entry->vm_binding, "yieldable") == 0;
}

static inline bool xr_stdlib_metadata_func_resumes_by_netpoll_retry(const char *module,
                                                                    const char *name) {
    if (!module || !name || strcmp(module, "net") != 0 ||
        !xr_stdlib_metadata_func_is_yieldable(module, name))
        return false;
    return strcmp(name, "__accept") == 0 || strcmp(name, "__read") == 0 ||
           strcmp(name, "__readInto") == 0 || strcmp(name, "__readExactInto") == 0 ||
           strcmp(name, "__write") == 0 || strcmp(name, "__writeBytes") == 0 ||
           strcmp(name, "__copyBidirectional") == 0;
}

#endif  // XSTDLIB_METADATA_H
