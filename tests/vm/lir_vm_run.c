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
#include "lir/xlir_source.h"
#include "lir/xlir_test_plan.h"
#include "vm/register/xrbc_lir.h"
#include "lir_vm_result_report.h"
#include <stdio.h>
#include <string.h>

static XrRbcProgram *compile_file(const char *path, bool optimize, const char *stdlib,
    const char *test_name, bool test_plan) {
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    XrLirLowered lowered = {0};
    XrLirTestPlan *plan = NULL;
    XrLirSourceFailure failure = {0};
    XrRbcProgram *program = NULL;
    if (xr_compile_resources_new(&limits, &resources) != XR_COMPILE_RESOURCE_OK) {
        fprintf(stderr, "lir_vm_run: cannot prepare source input: %s\n", path);
        return NULL;
    }
    XrLirSourceRequest request = {resources, path, stdlib, optimize, NULL, NULL, XR_XIR_HOST_TARGET};
    XrLirSourceStatus status;
    if (test_plan)
        status = xr_lir_compile_test_plan(&request, &plan, &failure);
    else if (test_name)
        status = xr_lir_compile_test_source(&request, test_name, &lowered, &failure);
    else
        status = xr_lir_compile_source(&request, &lowered, &failure);
    if (status == XR_LIR_SOURCE_CHECK_FAILED) {
        fprintf(stderr, "lir_vm_run: source %s:%d:%d: %s\n",
            failure.source_failure.path ? failure.source_failure.path : path,
            failure.source.line, failure.source.column, failure.source.message);
    } else if (status == XR_LIR_SOURCE_TEST_SELECTION_FAILED) {
        fputs("no matching root-module @test declaration\n", stderr);
    } else if (status == XR_LIR_SOURCE_SPECIALIZATION_FAILED) {
        fprintf(stderr, "lir_vm_run: specialization failed (%u)\n",
            (unsigned)failure.specialization_status);
    } else if (status == XR_LIR_SOURCE_LOWER_FAILED) {
        fprintf(stderr, "lir_vm_run: lower %s:%u:%u: %s [function=%u block=%u instruction=%u]\n",
            failure.lower.owned_path ? failure.lower.owned_path : path,
            failure.lower.diagnostic.source.line, failure.lower.diagnostic.source.column,
            failure.lower.diagnostic.message, failure.lower.diagnostic.function,
            failure.lower.diagnostic.block, failure.lower.diagnostic.instruction);
    } else if (status != XR_LIR_SOURCE_OK) {
        fprintf(stderr, "lir_vm_run: cannot prepare source input: %s\n", path);
    } else {
        XrRbcCompileDiagnostic diagnostic = {0};
        XrLirFunction entry = test_plan ? xr_lir_test_plan_initializer(plan) : lowered.entry;
        if (xr_rbc_compile(entry.module, entry, &program, &diagnostic) !=
            XR_RBC_COMPILE_OK) {
            fprintf(stderr, "lir_vm_run: bytecode %s:%u:%u: %s\n",
                diagnostic.path[0] ? diagnostic.path : path,
                diagnostic.line, diagnostic.column, diagnostic.message);
        }
    }
    xr_lir_source_failure_free(&failure);
    xr_lir_lowered_free(&lowered);
    xr_lir_test_plan_free(plan);
    xr_compile_resources_release(resources);
    return program;
}

static bool report_test_panic(const XrRbcOutcome *outcome) {
    uint32_t code, source;
    const char *message;
    size_t length;
    const XrtLocationOwner *locations;
    if (!xr_rbc_outcome_message_panic(outcome, &code, &message, &length, &source, &locations))
        return false;
    if (!xrt_message_panic_report(code, message, length, xrt_locations_get(locations, source)))
        fputs("cannot render uncaught message panic\n", stderr);
    return true;
}

int main(int argc, char **argv) {
    bool twice = false, optimize = false, test_plan = false, valid = argc >= 2 && argc <= 8;
    const char *stdlib = NULL, *test_name = NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--twice") && !twice) twice = true;
        else if (!strcmp(argv[i], "--optimize") && !optimize) optimize = true;
        else if (!strcmp(argv[i], "--stdlib") && !stdlib && i + 1 < argc) stdlib = argv[++i];
        else if (!strcmp(argv[i], "--test") && !test_name && i + 1 < argc) test_name = argv[++i];
        else if (!strcmp(argv[i], "--test-plan") && !test_plan) test_plan = true;
        else valid = false;
    }
    if (!valid || (test_plan && test_name)) {
        fputs("usage: lir_vm_run <source.xr> [--twice] [--optimize] [--stdlib <directory>] "
            "[--test NAME | --test-plan]\n", stderr);
        return 2;
    }
    XrRbcProgram *program = compile_file(argv[1], optimize, stdlib, test_name, test_plan);
    if (!program) return 1;
    int result = 0;
    for (unsigned i = 0; i < (twice ? 2u : 1u) && !result; ++i) {
        XrRbcExecution *execution = NULL;
        XrRbcStatus status = xr_rbc_program_prepare(program, NULL, 0, &execution);
        XrRbcOutcome *outcome = status == XR_RBC_OK ? xr_rbc_execution_run(execution) : NULL;
        if (status == XR_RBC_OK) status = xr_rbc_outcome_status(outcome);
        xr_rbc_execution_free(&execution);
        if (i + 1 == (twice ? 2u : 1u)) {
            xr_rbc_program_destroy(program);
            program = NULL;
        }
        result = (test_name || test_plan) && report_test_panic(outcome) ? 1 : lir_vm_result_report(outcome, status);
        xr_rbc_outcome_free(&outcome);
    }
    xr_rbc_program_destroy(program);
    return result;
}
