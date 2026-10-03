/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source.h - Source declarations to owned Checked XIR
 *
 * KEY CONCEPT:
 *   Parsing and module resolution precede direct typed XIR construction.
 */
#ifndef XXIR_SOURCE_H
#define XXIR_SOURCE_H
#include "xxir_source_query.h"
#include "../module/xmodule_identity.h"
struct XrCompilerSession;
struct XrLockfile;
struct XrXirLibraryCatalog;
typedef struct XrXirSourceRequest {
    struct XrCompilerSession *session;
    const char *entry_path;
    const XrModuleIdentityAuthority *authority;
    const XrXirCompileContext *context;
    const char *stdlib_path;
    /* Borrowed for this synchronous check; package imports require exact entries. */
    struct XrLockfile *lockfile;
    XrXirLinkageKind linkage_kind;
    const struct XrXirLibraryCatalog *libraries;
} XrXirSourceRequest;
/* Failure preserves output. Optional failure_path must point to NULL. A
 * semantic failure with a known location transfers its already-owned UTF-8
 * module path without allocating or consuming work during failure cleanup.
 * Release that path with xr_compile_resources_free. Graph and parse failures
 * without an exact semantic location leave failure_path unchanged. */
XR_FUNC XrXirStatus xr_xir_compile_source_check(const XrXirSourceRequest *request,
    XrXirSourceResult *output, XrXirSourceDiagnostic *diagnostic, char **failure_path);
#endif // XXIR_SOURCE_H
