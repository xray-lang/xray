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
#include "xxir.h"
#include "../module/xmodule_identity.h"
struct XrCompilerSession;
typedef struct XrXirSourceRequest {
    struct XrCompilerSession *session;
    const char *entry_path;
    const XrModuleIdentityAuthority *authority;
    const XrXirBudget *budget;
} XrXirSourceRequest;
typedef struct XrXirSourceDiagnostic {
    XrXirStatus status;
    uint32_t module;
    int line, column;
    char message[192];
} XrXirSourceDiagnostic;
XR_FUNC XrXirStatus xr_xir_source_check(const XrXirSourceRequest *request,
    XrXirArtifact **output, XrXirSourceDiagnostic *diagnostic);
#endif // XXIR_SOURCE_H
