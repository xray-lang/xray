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
        xr_stdlib_metadata_hash_u64(&ctx, entry->target_leaf);
        xr_stdlib_metadata_hash_u64(&ctx, entry->aot_direct ? 1u : 0u);
    }
    xr_sha256_final(&ctx, out->bytes);
}

static inline const XrStdlibDefEntry *
xr_stdlib_metadata_unique_func_span(const char *module, size_t module_length, const char *name) {
    if (!module || module_length == 0 || module[0] == '.' || !name || !name[0])
        return NULL;
    const XrStdlibDefEntry *found = NULL;
    for (uint32_t i = 0; i < XR_STDLIB_DEF_ENTRY_COUNT; i++) {
        const XrStdlibDefEntry *entry = &xr_stdlib_def_entries[i];
        if (!entry->module || !entry->name || strlen(entry->module) != module_length ||
            memcmp(entry->module, module, module_length) != 0 ||
            strcmp(entry->name, name) != 0)
            continue;
        if (found)
            return NULL;
        found = entry;
    }
    return found;
}

static inline const XrStdlibDefEntry *
xr_stdlib_metadata_unique_func(const char *module, const char *name) {
    return module ? xr_stdlib_metadata_unique_func_span(module, strlen(module), name) : NULL;
}

/* A callsite identifies an overload by module, selector, and arity. Keep the
 * uniqueness check on that complete identity: rejecting at (module, selector)
 * would make every valid overload set unreachable, while accepting the first
 * matching arity would make a duplicate registry row order-dependent. */
static inline const XrStdlibDefEntry *
xr_stdlib_metadata_unique_func_arity_in_entries(const XrStdlibDefEntry *entries,
                                                uint32_t entry_count, const char *module,
                                                const char *name, uint16_t argument_count) {
    if (!entries || !module || !module[0] || module[0] == '.' || !name || !name[0])
        return NULL;
    const XrStdlibDefEntry *found = NULL;
    for (uint32_t i = 0; i < entry_count; i++) {
        const XrStdlibDefEntry *entry = &entries[i];
        if (!entry->module || !entry->name || entry->argc != argument_count ||
            strcmp(entry->module, module) != 0 || strcmp(entry->name, name) != 0)
            continue;
        if (found)
            return NULL;
        found = entry;
    }
    return found;
}

static inline const XrStdlibDefEntry *
xr_stdlib_metadata_unique_func_arity(const char *module, const char *name,
                                     uint16_t argument_count) {
    return xr_stdlib_metadata_unique_func_arity_in_entries(
        xr_stdlib_def_entries, XR_STDLIB_DEF_ENTRY_COUNT, module, name, argument_count);
}

static inline const XrStdlibDefEntry *
xr_stdlib_metadata_unique_func_arity_span(const char *module, size_t module_length,
                                          const char *name, uint16_t argument_count) {
    if (!module || module_length == 0 || module[0] == '.' || !name || !name[0])
        return NULL;
    const XrStdlibDefEntry *found = NULL;
    for (uint32_t i = 0; i < XR_STDLIB_DEF_ENTRY_COUNT; i++) {
        const XrStdlibDefEntry *entry = &xr_stdlib_def_entries[i];
        if (!entry->module || !entry->name || entry->argc != argument_count ||
            strlen(entry->module) != module_length ||
            memcmp(entry->module, module, module_length) != 0 || strcmp(entry->name, name) != 0)
            continue;
        if (found)
            return NULL;
        found = entry;
    }
    return found;
}

static inline bool xr_stdlib_metadata_module_known(const char *module) {
    if (!module || !module[0] || module[0] == '.')
        return false;
    for (uint32_t i = 0; i < XR_STDLIB_DEF_ENTRY_COUNT; i++) {
        const XrStdlibDefEntry *entry = &xr_stdlib_def_entries[i];
        if (entry->module && strcmp(entry->module, module) == 0)
            return true;
    }
    return false;
}

static inline bool xr_stdlib_metadata_link_dependency_module_known(const char *name) {
    if (!name || !name[0] || name[0] == '.')
        return false;
    static const char *modules[] = {
        "regex", "math", "time",   "datetime", "path",     "io",   "os",
        "net",   "http", "crypto", "base64",   "encoding", "url",  "csv",
        "toml",  "yaml", "xml",    "compress", "log",      "text",
    };
    for (uint32_t i = 0; i < (uint32_t) (sizeof(modules) / sizeof(modules[0])); i++) {
        if (strcmp(name, modules[i]) == 0)
            return true;
    }
    return false;
}

/* The frozen definition registry is the only authority over which native
 * stdlib member a callsite may dispatch to without a backend lookup. A member
 * qualifies when the registry names exactly one entry for the module, member,
 * and callsite arity, every argument crosses the boundary as one plain tagged
 * value, the member returns a single value through the generated direct shim,
 * and no conditional compilation or result enum qualifies the row. Runtime
 * capabilities remain part of the exact direct-call identity; callers that
 * need the narrower capability-free namespace-scalar family use the member
 * predicate below.
 *
 * The `builtin` AOT form is refused on purpose and is not a module exclusion:
 * the native backend rewrites those callsites into a different operation after
 * the SemanticPlan is already frozen, so no frozen row can describe the shape
 * the backend actually emits. A `yieldable` binding is refused because it
 * suspends, which is a coroutine-state fact this row does not state. */
static inline const XrStdlibDefEntry *
xr_stdlib_metadata_exact_native_direct_call_span(const char *module, size_t module_length,
                                                 const char *name, uint16_t argument_count) {
    const XrStdlibDefEntry *entry =
        xr_stdlib_metadata_unique_func_arity_span(module, module_length, name, argument_count);
    if (!entry || !entry->aot_direct || !entry->aot || !entry->aot_kind || !entry->ret ||
        !entry->vm || !entry->vm_binding || !entry->arg_spec || !entry->aot_enum ||
        !entry->vm_ifdef || !entry->define || !entry->signature)
        return NULL;
    if (entry->aot[0] == '\0' || entry->vm[0] == '\0' || strcmp(entry->aot_kind, "method") != 0 ||
        strcmp(entry->ret, "value") != 0 || strcmp(entry->vm_binding, "normal") != 0 ||
        entry->aot_enum[0] != '\0' || entry->vm_ifdef[0] != '\0' || entry->define[0] != '\0')
        return NULL;
    uint32_t spec = 0;
    for (; entry->arg_spec[spec] != '\0'; spec++) {
        if (entry->arg_spec[spec] != 'v')
            return NULL;
    }
    return spec == (uint32_t) argument_count ? entry : NULL;
}

static inline const XrStdlibDefEntry *
xr_stdlib_metadata_exact_native_direct_call(const char *module, const char *name,
                                            uint16_t argument_count) {
    return module ? xr_stdlib_metadata_exact_native_direct_call_span(module, strlen(module), name,
                                                                     argument_count)
                  : NULL;
}

static inline const XrStdlibDefEntry *
xr_stdlib_metadata_exact_native_direct_member_span(const char *module, size_t module_length,
                                                   const char *name, uint16_t argument_count) {
    const XrStdlibDefEntry *entry = xr_stdlib_metadata_exact_native_direct_call_span(
        module, module_length, name, argument_count);
    return entry && entry->runtime_capabilities == 0u ? entry : NULL;
}

static inline const XrStdlibDefEntry *
xr_stdlib_metadata_exact_native_direct_member(const char *module, const char *name,
                                              uint16_t argument_count) {
    return module ? xr_stdlib_metadata_exact_native_direct_member_span(module, strlen(module), name,
                                                                       argument_count)
                  : NULL;
}

static inline const XrStdlibDefEntry *
xr_stdlib_metadata_exact_native_target_leaf(const char *module, const char *name,
                                            uint16_t argument_count) {
    const XrStdlibDefEntry *entry =
        xr_stdlib_metadata_exact_native_direct_member(module, name, argument_count);
    return entry && entry->target_leaf > XR_STDLIB_TARGET_LEAF_NONE &&
                   entry->target_leaf < XR_STDLIB_TARGET_LEAF_COUNT
               ? entry
               : NULL;
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
           strcmp(name, "__write") == 0 || strcmp(name, "__writeBytes") == 0;
}

#endif  // XSTDLIB_METADATA_H
