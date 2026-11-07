/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_source.h - Owned LIR from one synchronous source compilation
 *
 * KEY CONCEPT:
 *   Both execution backends receive the same complete source root. Temporary
 *   frontend and specialization owners retire before the result is published.
 */
#ifndef XLIR_SOURCE_H
#define XLIR_SOURCE_H

#include "xlir_lower.h"
#include "xir/xxir_source.h"

typedef enum XrLirSourceStatus {
    XR_LIR_SOURCE_OK,
    XR_LIR_SOURCE_BAD_ARGUMENT,
    XR_LIR_SOURCE_PREPARATION_FAILED,
    XR_LIR_SOURCE_CHECK_FAILED,
    XR_LIR_SOURCE_SPECIALIZATION_FAILED,
    XR_LIR_SOURCE_LOWER_FAILED,
    XR_LIR_SOURCE_OPTIMIZATION_FAILED
} XrLirSourceStatus;

typedef struct XrLirSourceRequest {
    XrCompileResources *resources;
    const char *entry_path;
    const char *stdlib_path;
    bool optimize;
} XrLirSourceRequest;

typedef struct XrLirSourceFailure {
    XrLirSourceStatus stage;
    XrXirSourceDiagnostic source;
    XrXirSourceFailure source_failure;
    XrXirStatus specialization_status;
    XrLirLowerFailure lower;
} XrLirSourceFailure;

/* Borrow the request and its strings synchronously. Output and failure must be
 * empty. Success owns its LIR independently of the request, parser, checked
 * artifact and caller's memory-owner reference. Failure preserves output and
 * owns any diagnostic path/report until failure_free. The explicit stdlib path
 * is forwarded unchanged to module resolution. No executor is selected here. */
XR_FUNC XrLirSourceStatus xr_lir_compile_source(const XrLirSourceRequest *request,
    XrLirLowered *output, XrLirSourceFailure *failure);
XR_FUNC void xr_lir_source_failure_free(XrLirSourceFailure *failure);

#endif // XLIR_SOURCE_H
