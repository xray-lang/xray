/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_compile.h - Shared source admission to an owned Source product
 *
 * KEY CONCEPT:
 *   Every projection receives the same source owner and caller ledger.
 */
#ifndef XSOURCE_COMPILE_H
#define XSOURCE_COMPILE_H
#include "../program/xr_xir_source_product.h"
#include "../module/xnative_package.h"
#include "../toolchain/xcompiler_session.h"

typedef struct XrSourceCompileRequest {
    const XrXirCompileContext *context;
    const char *absolute_entry_path;
    const char *absolute_stdlib_path;
    const struct XrXirLibraryCatalog *libraries;
    XrTomlParseLimits manifest_limits;
    XrXirTarget target;
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
    XR_SOURCE_COMPILE_REJECTED
} XrSourceCompileStatus;

typedef enum XrSourceCompileStage {
    XR_SOURCE_COMPILE_INPUT,
    XR_SOURCE_COMPILE_PATH,
    XR_SOURCE_COMPILE_AUTHORITY,
    XR_SOURCE_COMPILE_SESSION,
    XR_SOURCE_COMPILE_PRODUCT,
    XR_SOURCE_COMPILE_LIBRARY
} XrSourceCompileStage;

typedef struct XrSourceCompileDiagnostic {
    XrSourceCompileStatus status;
    XrSourceCompileStage stage;
    XrManifestStatus authority_status;
    XrManifestDiagnostic authority;
    XrCompilerSessionStatus session_status;
    XrXirSourceProductDiagnostic source;
} XrSourceCompileDiagnostic;

/* One outer operation creates a ledger from these defaults. Stage adapters
 * keep that ledger; they do not create a new allowance for each stage. */
XR_FUNC XrCompileResourceLimits xr_source_compile_default_resource_limits(void);
XR_FUNC XrTomlParseLimits xr_source_compile_default_manifest_limits(void);

/* Inputs are borrowed synchronously. NULL libraries selects the trusted
 * same-build stdlib Catalog; an explicit Catalog remains a borrowed input.
 * Output must be empty and is preserved on failure. Initialize diagnostics to
 * zero; free their owned partial snapshot before reusing them. Neither products
 * nor diagnostics borrow this request, its Catalog, or the temporary Session.
 * The target selects the existing Lowered representation, not host capability. */
XR_FUNC XrSourceCompileStatus xr_source_compile_build(
    const XrSourceCompileRequest *request, XrXirSourceProduct **output,
    XrSourceCompileDiagnostic *diagnostic);
XR_FUNC void xr_source_compile_diagnostic_free(XrSourceCompileDiagnostic *diagnostic);
XR_FUNC const char *xr_source_compile_status_name(XrSourceCompileStatus status);
/* Renders one failed build. A located semantic failure becomes
 * "path:line:column: error: message" and returns true; any other failure is
 * "source build failed (stage=N status=NAME)[: message]" and returns false.
 * The text is truncated to fit. SIZE must be nonzero. */
XR_FUNC bool xr_source_compile_diagnostic_format(
    const XrSourceCompileDiagnostic *diagnostic, char *out, size_t size);
#endif // XSOURCE_COMPILE_H
