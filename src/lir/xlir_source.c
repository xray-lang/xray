/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_source.c - One owned source-to-LIR compilation boundary
 *
 * KEY CONCEPT:
 *   The complete root is lowered before either backend can consume it.
 *   Failure transfers diagnostic owners without allocating during cleanup.
 */
#include "xlir_source.h"
#include "xlir_builder.h"
#include "xlir_inline.h"
#include "xlir_sccp.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_generic.h"
#include <string.h>

static XrLirStatus source_optimize(XrLirLowered *lowered) {
    XrLirModule *optimized = NULL;
    XrLirStatus status = xr_lir_sccp(lowered->module, &optimized);
    if (status != XR_LIR_OK) return status;
    xr_lir_module_destroy(lowered->module);
    lowered->module = optimized;
    lowered->entry.module = optimized;
    optimized = NULL;
    status = xr_lir_inline(lowered->module, &optimized);
    if (status != XR_LIR_OK) return status;
    xr_lir_module_destroy(lowered->module);
    lowered->module = optimized;
    lowered->entry.module = optimized;
    return XR_LIR_OK;
}

XR_FUNC XrLirSourceStatus xr_lir_compile_source(const XrLirSourceRequest *request,
    XrLirLowered *output, XrLirSourceFailure *failure) {
    if (!request || !request->resources || !request->entry_path || !request->entry_path[0] ||
        !output || output->module || output->entry.module || output->entry.id || !failure ||
        failure->stage != XR_LIR_SOURCE_OK || failure->lower.owned_path ||
        !xr_xir_compile_source_failure_empty(&failure->source_failure))
        return XR_LIR_SOURCE_BAD_ARGUMENT;

    XrCompilerSession *session = NULL;
    XrModuleIdentityAuthority authority = {0};
    char *root = NULL;
    XrXirSourceResult source = {0};
    XrXirArtifact *closed = NULL;
    XrLirLowered lowered = {0};
    XrLirSourceStatus status = XR_LIR_SOURCE_PREPARATION_FAILED;
    if (xr_compile_session_new(request->resources, &session) != XR_COMPILE_RESOURCE_OK ||
        xr_compile_module_identity_script_authority_from_source(request->resources,
            request->entry_path, &authority, &root) != XR_MODULE_OK)
        goto cleanup;

    XrXirCompileContext context = {request->resources, xr_xir_compile_default_limits()};
    XrXirSourceRequest input = {.session = session, .entry_path = request->entry_path,
        .authority = &authority, .context = &context, .stdlib_path = request->stdlib_path,
        .linkage_kind = XR_XIR_PROGRAM};
    status = XR_LIR_SOURCE_CHECK_FAILED;
    if (xr_xir_compile_source_check(&input, &source, &failure->source,
            &failure->source_failure) != XR_XIR_OK)
        goto cleanup;

    status = XR_LIR_SOURCE_SPECIALIZATION_FAILED;
    XrXirDiagnostic specialization = {0};
    failure->specialization_status = xr_xir_compile_specialize(source.checked, &closed,
        &specialization, NULL);
    if (failure->specialization_status != XR_XIR_OK) goto cleanup;

    status = XR_LIR_SOURCE_LOWER_FAILED;
    XrLirLowerRequest lower_request = {closed, source.snapshot, false};
    if (xr_lir_lower_program_checked(&lower_request, &lowered, &failure->lower) != XR_LIR_OK)
        goto cleanup;

    status = XR_LIR_SOURCE_OK;

cleanup:
    xr_xir_compile_artifact_free(closed);
    xr_xir_compile_source_result_free(&source);
    xr_compile_session_free(session);
    xr_compile_resources_free(root);
    if (status == XR_LIR_SOURCE_OK && request->optimize &&
        source_optimize(&lowered) != XR_LIR_OK)
        status = XR_LIR_SOURCE_OPTIMIZATION_FAILED;
    if (status == XR_LIR_SOURCE_OK) {
        *output = lowered;
    } else {
        xr_lir_lowered_free(&lowered);
        failure->stage = status;
    }
    return status;
}

XR_FUNC void xr_lir_source_failure_free(XrLirSourceFailure *failure) {
    if (!failure) return;
    xr_xir_compile_source_failure_free(&failure->source_failure);
    xr_lir_lower_failure_free(&failure->lower);
    memset(failure, 0, sizeof(*failure));
}
