/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_canonical_source.h - Shared CLI admission to an owned Source product
 *
 * KEY CONCEPT:
 *   Every projection receives the same source owner and caller ledger.
 */
#ifndef XCLI_CANONICAL_SOURCE_H
#define XCLI_CANONICAL_SOURCE_H
#include "../../program/xr_xir_source_product.h"
#include "../../module/xnative_package.h"
#include "../../toolchain/xcompiler_session.h"

typedef struct XrCliCompileSourceRequest {
    const XrXirCompileContext *context;
    const char *absolute_entry_path;
    const char *absolute_stdlib_path;
    const struct XrXirLibraryCatalog *libraries;
    XrTomlParseLimits manifest_limits;
    XrXirTarget target;
} XrCliCompileSourceRequest;

typedef enum XrCliCompileSourceStatus {
    XR_CLI_COMPILE_SOURCE_OK,
    XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT,
    XR_CLI_COMPILE_SOURCE_NOT_FOUND,
    XR_CLI_COMPILE_SOURCE_INVALID,
    XR_CLI_COMPILE_SOURCE_LIMIT,
    XR_CLI_COMPILE_SOURCE_BUDGET,
    XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY,
    XR_CLI_COMPILE_SOURCE_IO,
    XR_CLI_COMPILE_SOURCE_UNSUPPORTED,
    XR_CLI_COMPILE_SOURCE_REJECTED
} XrCliCompileSourceStatus;

typedef enum XrCliCompileSourceStage {
    XR_CLI_COMPILE_SOURCE_INPUT,
    XR_CLI_COMPILE_SOURCE_PATH,
    XR_CLI_COMPILE_SOURCE_AUTHORITY,
    XR_CLI_COMPILE_SOURCE_SESSION,
    XR_CLI_COMPILE_SOURCE_PRODUCT
} XrCliCompileSourceStage;

typedef struct XrCliCompileSourceDiagnostic {
    XrCliCompileSourceStatus status;
    XrCliCompileSourceStage stage;
    XrManifestStatus authority_status;
    XrManifestDiagnostic authority;
    XrCompilerSessionStatus session_status;
    XrXirSourceProductDiagnostic source;
} XrCliCompileSourceDiagnostic;

/* One outer operation creates a ledger from these defaults. Stage adapters
 * keep that ledger; they do not create a new allowance for each stage. */
XR_FUNC XrCompileResourceLimits xr_cli_compile_default_resource_limits(void);
XR_FUNC XrTomlParseLimits xr_cli_compile_default_manifest_limits(void);

/* Inputs are borrowed synchronously. NULL libraries means no published input.
 * Output must be empty and is preserved on failure. Initialize diagnostics to
 * zero; free their owned partial snapshot before reusing them. Neither products
 * nor diagnostics borrow this request, its Catalog, or the temporary Session.
 * The target selects the existing Lowered representation, not host capability. */
XR_FUNC XrCliCompileSourceStatus xr_cli_compile_source_build(
    const XrCliCompileSourceRequest *request, XrXirSourceProduct **output,
    XrCliCompileSourceDiagnostic *diagnostic);
XR_FUNC void xr_cli_compile_source_diagnostic_free(XrCliCompileSourceDiagnostic *diagnostic);
XR_FUNC const char *xr_cli_compile_source_status_name(XrCliCompileSourceStatus status);
/* Renders one failed build. A located semantic failure becomes
 * "path:line:column: error: message" and returns true; any other failure is
 * "source build failed (stage=N status=NAME)[: message]" and returns false.
 * The text is truncated to fit. SIZE must be nonzero. */
XR_FUNC bool xr_cli_compile_source_diagnostic_format(
    const XrCliCompileSourceDiagnostic *diagnostic, char *out, size_t size);
#endif // XCLI_CANONICAL_SOURCE_H
