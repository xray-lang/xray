/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_c_run.c - Source-backed C emission for independent host compiler tests
 *
 * KEY CONCEPT:
 *   Destroy frontend inputs before C emission and LIR before writing the
 *   owned output. The host test runner compiles and executes that output.
 */
#include "lir/xlir_c.h"
#include "lir/xlir_lower.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_source.h"
#include "xir/xxir_generic.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fputs("usage: lir_c_run <absolute-source.xr> <output.c>\n", stderr);
        return 2;
    }
    int result = 1;
    XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    XrModuleIdentityAuthority authority = {0};
    char *authority_root = NULL;
    XrXirSourceResult source = {0};
    XrXirArtifact *closed = NULL;
    XrXirSourceDiagnostic source_diagnostic = {0};
    XrXirSourceFailure source_failure = {0};
    XrLirLowered lowered = {0};
    XrLirLowerFailure lower_failure = {0};
    XrLirCOutput output = {0};
    if (xr_compile_resources_new(&limits, &resources) != XR_COMPILE_RESOURCE_OK ||
        xr_compile_session_new(resources, &session) != XR_COMPILER_SESSION_OK ||
        xr_compile_module_identity_script_authority_from_source(resources, argv[1],
            &authority, &authority_root) != XR_MODULE_OK) {
        fputs("cannot prepare source compilation\n", stderr);
        goto done;
    }
    XrXirCompileContext context = {.resources = resources, .limits = xr_xir_compile_default_limits()};
    XrXirSourceRequest source_request = {.session = session, .entry_path = argv[1],
        .authority = &authority, .context = &context, .linkage_kind = XR_XIR_PROGRAM};
    if (xr_xir_compile_source_check(&source_request, &source, &source_diagnostic,
                                    &source_failure) != XR_XIR_OK) {
        fprintf(stderr, "source %d:%d: %s\n", source_diagnostic.line,
                source_diagnostic.column, source_diagnostic.message);
        goto done;
    }
    if (xr_xir_compile_specialize(source.checked, &closed, NULL) != XR_XIR_OK) {
        fputs("source specialization failed\n", stderr);
        goto done;
    }
    XrLirLowerRequest lower_request = {closed, source.snapshot, false};
    if (xr_lir_lower_program_checked(&lower_request, &lowered, &lower_failure) != XR_LIR_OK) {
        fprintf(stderr, "lowering: %s\n", lower_failure.diagnostic.message);
        goto done;
    }
    xr_xir_compile_artifact_free(closed);
    closed = NULL;
    xr_xir_compile_source_result_free(&source);
    xr_compile_session_free(session);
    session = NULL;
    xr_compile_resources_free(authority_root);
    authority_root = NULL;
    XrLirCRequest request = {lowered.module, lowered.entry, resources, argv[1]};
    XrLirCDiagnostic diagnostic;
    if (xr_lir_emit_c(&request, &output, &diagnostic) != XR_LIR_C_OK) {
        fprintf(stderr, "C emission: %s\n", diagnostic.message);
        goto done;
    }
    xr_lir_lowered_free(&lowered);
    FILE *file = fopen(argv[2], "wb");
    if (!file) {
        fputs("cannot open C output\n", stderr);
        goto done;
    }
    bool written = fwrite(output.bytes, 1, output.length, file) == output.length;
    bool closed_file = fclose(file) == 0;
    if (!written || !closed_file) {
        fputs("cannot write C output\n", stderr);
        goto done;
    }
    result = 0;
done:
    xr_lir_c_output_free(&output);
    xr_lir_lowered_free(&lowered);
    xr_lir_lower_failure_free(&lower_failure);
    xr_xir_compile_artifact_free(closed);
    xr_xir_compile_source_failure_free(&source_failure);
    xr_xir_compile_source_result_free(&source);
    xr_compile_session_free(session);
    xr_compile_resources_free(authority_root);
    xr_compile_resources_release(resources);
    return result;
}
