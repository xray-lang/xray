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

#include "../base/xdefs.h"

typedef enum XrModuleIdentityKind {
    XR_MODULE_IDENTITY_PROJECT = 1,
    XR_MODULE_IDENTITY_SCRIPT,
    XR_MODULE_IDENTITY_PACKAGE,
} XrModuleIdentityKind;

/* Physical roots are I/O authorities only. They never enter the identity bytes. */
typedef struct XrModuleIdentityAuthority {
    XrModuleIdentityKind kind;
    const char *namespace_id; /* project name or owner/name@locked-version */
    const char *physical_root;
} XrModuleIdentityAuthority;

/* Validate the logical namespace grammar without consulting the filesystem. */
XR_FUNC bool xr_module_identity_authority_valid(const XrModuleIdentityAuthority *authority);

/* Derive one root-relative logical path and its length-framed durable identity.
 * Both outputs are xr_malloc-owned. Absolute or escaping paths fail closed. */
XR_FUNC bool xr_module_identity_from_source(const XrModuleIdentityAuthority *authority,
                                            const char *source_path, char **identity_out,
                                            char **logical_path_out);

#endif /* XMODULE_IDENTITY_H */
