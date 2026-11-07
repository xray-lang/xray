/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_compile.h - Shared source checking and owned diagnostics
 *
 * KEY CONCEPT:
 *   Source consumers share project authority, parsing and the caller ledger.
 */
#ifndef XSOURCE_COMPILE_H
#define XSOURCE_COMPILE_H
#include "../xir/xxir_source.h"
#include "../module/xnative_package.h"
#include "../toolchain/xcompiler_session.h"

typedef struct XrSourceCompileRequest {
    const XrXirCompileContext *context;
    const char *absolute_entry_path;
    const char *absolute_stdlib_path;
    const struct XrXirLibraryCatalog *libraries;
    XrTomlParseLimits manifest_limits;
    XrCompilerSession *session; /* NULL creates a temporary parser owner; otherwise borrows this exact ledger. */
} XrSourceCompileRequest;

typedef enum XrSourceCompileStatus {
    XR_SOURCE_COMPILE_OK,
    XR_SOURCE_COMPILE_BAD_ARGUMENT,
    XR_SOURCE_COMPILE_NOT_FOUND,
    XR_SOURCE_COMPILE_INVALID,
    XR_SOURCE_COMPILE_LIMIT,
    XR_SOURCE_COMPILE_BUDGET,
    XR_SOURCE_COMPILE_OUT_OF_MEMORY,
    XR_SOURCE_COMPILE_IO,
    XR_SOURCE_COMPILE_UNSUPPORTED,
    XR_SOURCE_COMPILE_REJECTED,
    XR_SOURCE_COMPILE_MEMORY_LIMIT,
    XR_SOURCE_COMPILE_OVERFLOW,
    XR_SOURCE_COMPILE_CANCELLED,
    XR_SOURCE_COMPILE_DEADLINE,
    XR_SOURCE_COMPILE_CLOCK_FAILURE
} XrSourceCompileStatus;

typedef enum XrSourceCompileStage {
    XR_SOURCE_COMPILE_INPUT,
    XR_SOURCE_COMPILE_PATH,
    XR_SOURCE_COMPILE_AUTHORITY,
    XR_SOURCE_COMPILE_SESSION,
    XR_SOURCE_COMPILE_CHECK,
    XR_SOURCE_COMPILE_LIBRARY
} XrSourceCompileStage;

typedef struct XrSourceCheckDiagnostic {
    XrXirStatus status;
    XrXirSourceDiagnostic diagnostic;
    XrXirSourceFailure failure;
} XrSourceCheckDiagnostic;

typedef struct XrSourceCompileDiagnostic {
    XrSourceCompileStatus status;
    XrSourceCompileStage stage;
    XrManifestStatus authority_status;
    XrCompileResourceStatus session_status;
    XrSourceCheckDiagnostic source;
} XrSourceCompileDiagnostic;

/* One outer operation creates a ledger from these defaults. Stage adapters
 * keep that ledger; they do not create a new allowance for each stage. */
XR_FUNC XrCompileResourceLimits xr_source_compile_default_resource_limits(void);
XR_FUNC XrTomlParseLimits xr_source_compile_default_manifest_limits(void);

/* Borrow inputs synchronously. Output must be empty. Success publishes owned
 * Checked XIR and source metadata independently of parser and authority owners.
 * Diagnostics retain their own reports until diagnostic_free. An explicit
 * Catalog remains borrowed; NULL compiles standard-library source directly. */
XR_FUNC XrSourceCompileStatus xr_source_compile_check(
    const XrSourceCompileRequest *request, XrXirSourceResult *output,
    XrSourceCompileDiagnostic *diagnostic);
XR_FUNC void xr_source_compile_diagnostic_free(XrSourceCompileDiagnostic *diagnostic);
XR_FUNC const char *xr_source_compile_status_name(XrSourceCompileStatus status);
/* Consumes the immediate manifest producer fact before its scratch expires. */
XR_FUNC XrCompileResourceStatus xr_source_compile_manifest_report(
    XrSourceCompileDiagnostic *,XrCompileResources *,const XrManifestDiagnostic *);
/* Produces all roots and original labels without a fixed message buffer.
 * Returned text is a controlled allocation, independent of the report. */
XR_FUNC XrSourceCompileStatus xr_source_compile_diagnostic_render(
    const XrSourceCompileDiagnostic *,XrCompileResources *,char **);
#endif // XSOURCE_COMPILE_H
