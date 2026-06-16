/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_link.h - AOT link manifest
 */

#ifndef XAOT_LINK_H
#define XAOT_LINK_H

#include "../base/xdefs.h"

typedef struct XaotTarget {
    char *name;
    char *arch;
    char *os;
    char *abi;
    char *object_format;
    char *triple;
} XaotTarget;

typedef enum XaotLinkEntryKind {
    XAOT_LINK_GENERATED_C_FILE,
    XAOT_LINK_RUNTIME_OBJECT,
    XAOT_LINK_STDLIB_OBJECT,
    XAOT_LINK_STDLIB_SYMBOL,
    XAOT_LINK_SYSTEM_LIB,
    XAOT_LINK_DEFINE,
    XAOT_LINK_CC_FLAG,
    XAOT_LINK_LD_FLAG
} XaotLinkEntryKind;

typedef struct XaotLinkManifest {
    XaotTarget target;

    char **generated_c_files;
    uint32_t n_generated_c_files;

    char **runtime_objects;
    uint32_t n_runtime_objects;

    char **stdlib_objects;
    uint32_t n_stdlib_objects;

    char **stdlib_symbols;
    uint32_t n_stdlib_symbols;

    char **system_libs;
    uint32_t n_system_libs;

    char **defines;
    uint32_t n_defines;

    char **cc_flags;
    uint32_t n_cc_flags;

    char **ld_flags;
    uint32_t n_ld_flags;
} XaotLinkManifest;

XR_FUNC bool xaot_target_init(XaotTarget *target, const char *name);
XR_FUNC bool xaot_target_init_ex(XaotTarget *target, const char *name, const char *arch,
                                 const char *os, const char *abi, const char *object_format,
                                 const char *triple);
XR_FUNC void xaot_target_free(XaotTarget *target);

XR_FUNC bool xaot_link_manifest_init(XaotLinkManifest *manifest, const XaotTarget *target);
XR_FUNC void xaot_link_manifest_free(XaotLinkManifest *manifest);
XR_FUNC bool xaot_link_manifest_set_target(XaotLinkManifest *manifest, const XaotTarget *target);
XR_FUNC bool xaot_link_manifest_add(XaotLinkManifest *manifest, XaotLinkEntryKind kind,
                                    const char *value);
XR_FUNC bool xaot_link_manifest_add_unique(XaotLinkManifest *manifest, XaotLinkEntryKind kind,
                                           const char *value);
XR_FUNC bool xaot_link_manifest_contains(const XaotLinkManifest *manifest, XaotLinkEntryKind kind,
                                         const char *value);
XR_FUNC bool xaot_link_manifest_needs_runtime(const XaotLinkManifest *manifest);

/* Returns an xr_malloc-owned JSON string. Caller releases it with xr_free(). */
XR_FUNC char *xaot_link_manifest_dump_json(const XaotLinkManifest *manifest);

#endif /* XAOT_LINK_H */
