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
typedef struct XrXirSourcePromise {
    XrXirLiteral module;
    XrXirLiteral function;
    uint32_t promises;
    /* Zero selects the function; otherwise one plus the explicit parameter index, excluding receiver. */
    uint32_t parameter;
    /* Empty for top-level functions; otherwise the declaring nominal name. */
    XrXirLiteral owner;
} XrXirSourcePromise;
typedef struct XrXirSourcePromises {
    const XrXirSourcePromise *items;
    uint32_t count;
} XrXirSourcePromises;
typedef struct XrXirSourceRequest {
    struct XrCompilerSession *session;
    const char *entry_path;
    const XrModuleIdentityAuthority *authority;
    const XrXirBudget *budget;
    const char *stdlib_path;
    const XrXirSourcePromises *declarations;
} XrXirSourceRequest;
XR_FUNC XrXirStatus xr_xir_source_check(const XrXirSourceRequest *request,
    XrXirSourceResult *output, XrXirSourceDiagnostic *diagnostic);
#endif // XXIR_SOURCE_H
