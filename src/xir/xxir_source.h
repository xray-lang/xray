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
typedef struct XrXirSourceRequest {
    struct XrCompilerSession *session;
    const char *entry_path;
    const XrModuleIdentityAuthority *authority;
    const XrXirBudget *budget;
    const char *stdlib_path;
    /* Borrowed for this synchronous check; package imports require exact entries. */
    struct XrLockfile *lockfile;
} XrXirSourceRequest;
XR_FUNC XrXirStatus xr_xir_source_check(const XrXirSourceRequest *request,
    XrXirSourceResult *output, XrXirSourceDiagnostic *diagnostic);
#endif // XXIR_SOURCE_H
