/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_vm_run.c - Source-backed register-bytecode test driver
 *
 * KEY CONCEPT:
 *   Execute only the entry returned by lowering, after all borrowed compiler
 *   inputs have been retired. Unsupported closures never select an executor.
 */
#include "lir/xlir_lower.h"
#include "toolchain/xcompiler_session.h"
#include "vm/register/xrbc_lir.h"
#include "lir_vm_result_report.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_source.h"
#include <stdio.h>

static XrRbcProgram *compile_file(const char *path) {
    XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
    XrXirCompileContext context = {0};
    XrCompilerSession *session = NULL;
    XrModuleIdentityAuthority authority = {0};
    char *root = NULL;
    XrXirSourceResult source = {0};
    XrXirSourceDiagnostic source_diagnostic = {0};
    XrXirSourceFailure source_failure = {0};
    XrXirArtifact *closed = NULL;
    XrLirLowered lowered = {0};
    XrLirLowerFailure lower_failure = {0};
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic bytecode_diagnostic = {0};
    if (xr_compile_resources_new(&limits, &context.resources) != XR_COMPILE_RESOURCE_OK ||
        xr_compile_session_new(context.resources, &session) != XR_COMPILER_SESSION_OK ||
        xr_compile_module_identity_script_authority_from_source(context.resources, path,
            &authority, &root) != XR_MODULE_OK) {
        fprintf(stderr, "lir_vm_run: cannot prepare source input: %s\n", path);
        goto done;
    }
    context.limits = xr_xir_compile_default_limits();
    XrXirSourceRequest request = {.session = session, .entry_path = path,
        .authority = &authority, .context = &context, .linkage_kind = XR_XIR_PROGRAM};
    if (xr_xir_compile_source_check(&request, &source, &source_diagnostic,
            &source_failure) != XR_XIR_OK) {
        fprintf(stderr, "lir_vm_run: source %s:%d:%d: %s\n",
            source_failure.path ? source_failure.path : path,
            source_diagnostic.line, source_diagnostic.column, source_diagnostic.message);
        goto done;
    }
    XrXirDiagnostic specialization = {0};
    if (xr_xir_compile_specialize(source.checked, &closed, &specialization) != XR_XIR_OK) {
        fprintf(stderr, "lir_vm_run: specialization failed (%u)\n",
            (unsigned)specialization.status);
        goto done;
    }
    XrLirLowerRequest lower_request = {closed, source.snapshot, false};
    if (xr_lir_lower_program_checked(&lower_request, &lowered, &lower_failure) != XR_LIR_OK) {
        fprintf(stderr, "lir_vm_run: lower %s:%u:%u: %s\n",
            lower_failure.owned_path ? lower_failure.owned_path : path,
            lower_failure.diagnostic.source.line, lower_failure.diagnostic.source.column,
            lower_failure.diagnostic.message);
        goto done;
    }
    // Lowering owns its output, including metadata, independently of Source.
    xr_xir_compile_artifact_free(closed);
    closed = NULL;
    xr_xir_compile_source_result_free(&source);
    xr_compile_session_free(session);
    session = NULL;
    xr_compile_resources_free(root);
    root = NULL;
    xr_compile_resources_release(context.resources);
    context.resources = NULL;
    if (xr_rbc_compile(lowered.module, lowered.entry, &program,
            &bytecode_diagnostic) != XR_RBC_COMPILE_OK) {
        fprintf(stderr, "lir_vm_run: bytecode %s:%u:%u: %s\n",
            bytecode_diagnostic.path[0] ? bytecode_diagnostic.path : path,
            bytecode_diagnostic.line, bytecode_diagnostic.column, bytecode_diagnostic.message);
    }
done:
    xr_lir_lowered_free(&lowered);
    xr_lir_lower_failure_free(&lower_failure);
    xr_xir_compile_artifact_free(closed);
    xr_xir_compile_source_result_free(&source);
    xr_xir_compile_source_failure_free(&source_failure);
    xr_compile_session_free(session);
    xr_compile_resources_free(root);
    xr_compile_resources_release(context.resources);
    return program;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fputs("usage: lir_vm_run <source.xr>\n", stderr);
        return 2;
    }
    XrRbcProgram *program = compile_file(argv[1]);
    if (!program) return 1;
    XrRbcExecution *execution = NULL;
    XrRbcStatus status = xr_rbc_program_prepare(program, NULL, 0, &execution);
    XrRbcOutcome *outcome = status == XR_RBC_OK ? xr_rbc_execution_run(execution) : NULL;
    if (status == XR_RBC_OK) status = xr_rbc_outcome_status(outcome);
    xr_rbc_execution_free(&execution);
    xr_rbc_program_destroy(program);
    int result = lir_vm_result_report(outcome, status);
    xr_rbc_outcome_free(&outcome);
    return result;
}
