/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_identity.h - Relocatable source-module identity authority
 */

#ifndef XMODULE_IDENTITY_H
#define XMODULE_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>

#include "../base/xdefs.h"
#include "../base/xfileio.h"
#include "../base/xcompile_resources.h"

/* Shared failure vocabulary for identity construction, resolution and graph build. */
typedef enum XrModuleStatus {
    XR_MODULE_OK = 0,
    XR_MODULE_INVALID = -1,
    XR_MODULE_OUT_OF_MEMORY = -2,
    XR_MODULE_BUDGET = -3,
    XR_MODULE_IO = -4,
    XR_MODULE_NOT_FOUND = -5
} XrModuleStatus;

static inline XrModuleStatus xr_module_status_from_path(XrPathStatus status) {
    switch (status) {
        case XR_PATH_OK: return XR_MODULE_OK;
        case XR_PATH_OUT_OF_MEMORY: return XR_MODULE_OUT_OF_MEMORY;
        case XR_PATH_BUDGET: return XR_MODULE_BUDGET;
        case XR_PATH_IO: return XR_MODULE_IO;
        case XR_PATH_NOT_FOUND: return XR_MODULE_NOT_FOUND;
        default: return XR_MODULE_INVALID;
    }
}

typedef enum XrModuleIdentityKind {
    XR_MODULE_IDENTITY_PROJECT = 1,
    XR_MODULE_IDENTITY_SCRIPT,
    XR_MODULE_IDENTITY_PACKAGE,
    XR_MODULE_IDENTITY_STDLIB,
    XR_MODULE_IDENTITY_MEMORY,
} XrModuleIdentityKind;

/* Physical roots are I/O authorities only. They never enter the identity bytes. */
typedef struct XrModuleIdentityAuthority {
    XrModuleIdentityKind kind;
    const char *namespace_id; /* project name or owner/name@locked-version */
    const char *physical_root;
} XrModuleIdentityAuthority;

/* Validate the logical namespace grammar without consulting the filesystem. */
XR_FUNC bool xr_module_identity_authority_valid(const XrModuleIdentityAuthority *authority);

/* Derive a durable identity from an already-authoritative logical path.
 * Memory authorities require an explicit namespace id and an empty path.
 * The output pins resources until xr_compile_resources_free; failure preserves it. */
XR_FUNC XrModuleStatus xr_compile_module_identity_from_logical(XrCompileResources *resources, const XrModuleIdentityAuthority *authority,
                                             const char *logical_path, char **identity_out);

/* Derive one root-relative logical path and its length-framed durable identity.
 * Both outputs pin the supplied ledger and use xr_compile_resources_free.
 * Sources must be absolute and stay inside the authority root. Failure preserves all outputs. */
XR_FUNC XrModuleStatus xr_compile_module_identity_from_source(XrCompileResources *resources, const XrModuleIdentityAuthority *authority,
                                            const char *source_path, char **identity_out,
                                            char **logical_path_out);

/* Build an explicit script authority rooted at the source file's directory.
 * The returned root pins the ledger and backs authority->physical_root.
 * Release it with xr_compile_resources_free; failure preserves both outputs. */
XR_FUNC XrModuleStatus xr_compile_module_identity_script_authority_from_source(
    XrCompileResources *resources, const char *source_path, XrModuleIdentityAuthority *authority, char **root_out);

/* Validate the exact typed identity grammar and optionally return its kind. */
XR_FUNC bool xr_module_identity_valid(const char *identity, XrModuleIdentityKind *kind_out);

/* Return the framed stdlib namespace as a non-owning slice.  Consumers that
 * compare frozen identities with registries must not parse the canonical key
 * independently or assume that the slice is NUL-terminated. */
XR_FUNC bool xr_module_identity_stdlib_namespace(const char *identity,
                                                 const char **namespace_out,
                                                 size_t *namespace_length_out);

#endif /* XMODULE_IDENTITY_H */
