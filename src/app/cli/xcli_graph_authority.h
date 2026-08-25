/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_graph_authority.h - Exact source graph authority for CLI commands
 *
 * KEY CONCEPT:
 *   A source command owns one project, lockfile, and resolver context for its
 *   complete module graph. Resolver caches therefore cannot outlive or obscure
 *   the exact authority that governed their entries.
 */

#ifndef XCLI_GRAPH_AUTHORITY_H
#define XCLI_GRAPH_AUTHORITY_H

#include "../../module/xlockfile.h"
#include "../../module/xmodule.h"
#include "../../module/xmodule_identity.h"
#include "../../module/xmodule_resolver.h"
#include "../../module/xproject.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct XrCliGraphAuthority {
    XrProject *project;
    XrLockfile *lockfile;
    XrModuleResolver *resolver;
    XrModuleIdentityAuthority entry_authority;
    char *authority_namespace;
    char *authority_root;
} XrCliGraphAuthority;

/* Open an exact authority context for one source graph. The resolver borrows
 * registry factories and stdlib paths while this context owns its lockfile. */
XR_FUNC bool xr_cli_graph_authority_open(XrCliGraphAuthority *context, XrModuleRegistry *registry,
                                         const char *entry_path, char *error, size_t error_size);

XR_FUNC void xr_cli_graph_authority_close(XrCliGraphAuthority *context);

#endif /* XCLI_GRAPH_AUTHORITY_H */
