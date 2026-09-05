/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_run.c - Canonical 'xray run' command implementation
 *
 * KEY CONCEPT:
 *   Source run has one route: source owner to validated XrProgram, exact
 *   target profile and instance, then a VM-private executable view.
 */

#include "xcli.h"
#include "xcli_canonical_source.h"
#include "xcli_spec.h"
#include "../toolchain/xtc_target_profile.h"
#include "../../api/xisolate_profile.h"
#include "../../execution/xr_execution.h"
#include "../../plan/target/xr_target_profile.h"
#include "../../vm/xr_program_vm.h"

#include "xray_vm.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static bool is_exact_source_path(const char *path) {
    size_t length = path ? strlen(path) : 0u;
    return length > 3u && strcmp(path + length - 3u, ".xr") == 0;
}

static XrVMRuntime *create_compiler_host(const char *source_path) {
    XrVMConfig config;
    xr_isolate_profile_params(XR_ISOLATE_PROFILE_RUN, &config);
    config.script_file = source_path;
    return xr_isolate_profile_create(&config);
}

static bool retire_instance(XrInstance **instance) {
    if (!instance || !*instance)
        return true;
    XrExecutionDiagnostic diagnostic;
    if (xr_execution_instance_state(*instance) == XR_INSTANCE_ACTIVE &&
        xr_execution_instance_begin_drain(*instance, &diagnostic) != XR_EXECUTION_OK)
        return false;
    if (xr_execution_instance_state(*instance) == XR_INSTANCE_DRAINING &&
        xr_execution_instance_retire(*instance, &diagnostic) != XR_EXECUTION_OK)
        return false;
    return xr_execution_instance_free(instance, &diagnostic) == XR_EXECUTION_OK;
}

static int report_source_failure(const XrCliCanonicalSourceDiagnostic *diagnostic) {
    fprintf(stderr, "XR_RUN_6001: canonical source build failed: %s",
            diagnostic && diagnostic->message[0] ? diagnostic->message : "unknown failure");
    if (diagnostic && diagnostic->status == XR_CLI_CANONICAL_SOURCE_BUILD_REJECTED)
        fprintf(stderr, " (stage=%u status=%s)", (unsigned) diagnostic->build.stage,
                xr_program_source_build_status_name(diagnostic->build.status));
    fputc('\n', stderr);
    return XR_CLI_EXIT_FAIL;
}

static int report_execution_outcome(XrVmOutcome outcome) {
    switch (outcome.kind) {
        case XR_VM_OUTCOME_RETURN:
            if (outcome.value.kind == XR_VM_VALUE_VOID)
                return XR_CLI_EXIT_OK;
            if (outcome.value.kind == XR_VM_VALUE_I64 && outcome.value.as.i64 == 0)
                return XR_CLI_EXIT_OK;
            if (outcome.value.kind == XR_VM_VALUE_I64 && outcome.value.as.i64 > 0 &&
                outcome.value.as.i64 <= 255) {
                fprintf(stderr, "XR_RUN_6012: main returned process status %" PRId64 "\n",
                        outcome.value.as.i64);
                return (int) outcome.value.as.i64;
            }
            fprintf(stderr,
                    "XR_RUN_6007: main must return void or an i64 process status in [0, 255]\n");
            return XR_CLI_EXIT_FAIL;
        case XR_VM_OUTCOME_TRAP:
            fprintf(stderr,
                    "XR_RUN_6004: canonical execution trapped (trap=%u steps=%" PRIu64 ")\n",
                    (unsigned) outcome.trap, outcome.steps);
            return XR_CLI_EXIT_FAIL;
        case XR_VM_OUTCOME_ERROR:
            fprintf(stderr, "XR_RUN_6005: main returned an uncaught typed error\n");
            return XR_CLI_EXIT_FAIL;
        case XR_VM_OUTCOME_PANIC:
            fprintf(stderr, "XR_RUN_6006: main published an uncaught panic\n");
            return XR_CLI_EXIT_FAIL;
        case XR_VM_OUTCOME_RESOURCE_LIMIT:
            fprintf(stderr, "XR_RUN_6003: canonical execution exceeded its resource budget\n");
            return XR_CLI_EXIT_FAIL;
        case XR_VM_OUTCOME_SUSPENDED:
        case XR_VM_OUTCOME_INVALID_INVOCATION:
        case XR_VM_OUTCOME_STALE_CODE:
            fprintf(stderr, "XR_RUN_6002: canonical execution was rejected (outcome=%u)\n",
                    (unsigned) outcome.kind);
            return XR_CLI_EXIT_FAIL;
    }
    fprintf(stderr, "XR_RUN_6002: canonical execution returned an unknown outcome\n");
    return XR_CLI_EXIT_INTERNAL;
}

static int execute_program(XrProgramSourceProduct *product, XrTargetProfile *profile) {
    if (xr_validated_program_provider_requirement_count(product->program) != 0u) {
        fprintf(
            stderr,
            "XR_RUN_6008: canonical run has no bindings for the program provider requirements\n");
        return XR_CLI_EXIT_FAIL;
    }
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = product->program,
        .profile = profile,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    XrExecutionStatus execution_status =
        xr_execution_instance_create(&binding, &instance, &execution_diagnostic);
    if (execution_status != XR_EXECUTION_OK) {
        fprintf(stderr, "XR_RUN_6002: execution binding failed: %s/%s\n",
                xr_execution_status_name(execution_status),
                xr_execution_diagnostic_kind_name(execution_diagnostic.kind));
        return XR_CLI_EXIT_FAIL;
    }

    XrVmCodeOptions options = xr_vm_code_default_options();
    XrVmCodeDiagnostic code_diagnostic;
    XrVmCode *code = NULL;
    XrVmCodeStatus code_status = xr_vm_code_build(instance, &options, &code, &code_diagnostic);
    if (code_status != XR_VM_CODE_OK) {
        fprintf(stderr, "XR_RUN_6002: VM-private code build failed: %s\n",
                xr_vm_code_status_name(code_status));
        (void) retire_instance(&instance);
        return XR_CLI_EXIT_FAIL;
    }

    uint32_t entry = xr_validated_program_entry_function(product->program);
    XrVmOutcome outcome = xr_vm_code_execute(code, instance, entry, NULL, 0u);
    XrVmExecution *execution = NULL;
    if (outcome.kind == XR_VM_OUTCOME_INVALID_INVOCATION &&
        xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution)) {
        do {
            outcome = xr_vm_execution_step(execution);
        } while (outcome.kind == XR_VM_OUTCOME_SUSPENDED);
    }
    int result = report_execution_outcome(outcome);
    xr_vm_execution_free(execution);
    xr_vm_code_free(code);
    if (!retire_instance(&instance)) {
        fprintf(stderr, "XR_RUN_6009: execution instance did not retire cleanly\n");
        return XR_CLI_EXIT_INTERNAL;
    }
    return result;
}

XR_FUNC int cmd_run(const XrCliInvocation *inv) {
    if (!inv)
        return XR_CLI_EXIT_INTERNAL;
    if (!xr_cli_apply_xi_opt(inv, "run"))
        return XR_CLI_EXIT_USAGE;
    if (inv->positional_count != 1) {
        xr_cli_error("run", "exactly one source file is required");
        return XR_CLI_EXIT_USAGE;
    }
    if (inv->passthrough_argc != 0) {
        fprintf(stderr, "XR_RUN_6010: canonical main arguments are not implemented; no legacy "
                        "argument path exists\n");
        return XR_CLI_EXIT_FAIL;
    }
    const char *source_path = inv->positionals[0];
    if (!source_path || source_path[0] == '\0' ||
        (source_path[0] == '-' && source_path[1] == '\0')) {
        fprintf(stderr, "XR_RUN_6011: canonical run requires a file-backed source authority\n");
        return XR_CLI_EXIT_FAIL;
    }
    if (!is_exact_source_path(source_path)) {
        fprintf(stderr, "XR_RUN_6013: canonical run accepts only an exact '.xr' source path\n");
        return XR_CLI_EXIT_FAIL;
    }

    XrTargetCodegenFacts codegen = {0};
    XrTargetProfile *profile = NULL;
    char profile_error[512] = {0};
    if (!xtc_target_profile_build_current_native_hosted(&codegen, &profile, profile_error,
                                                        sizeof(profile_error))) {
        fprintf(stderr, "XR_RUN_6002: exact native target profile failed: %s\n",
                profile_error[0] ? profile_error : "unknown profile failure");
        return XR_CLI_EXIT_FAIL;
    }

    XrVMRuntime *compiler_host = create_compiler_host(source_path);
    if (!compiler_host) {
        xr_target_profile_free(profile);
        fprintf(stderr, "XR_RUN_6002: compiler host creation failed\n");
        return XR_CLI_EXIT_INTERNAL;
    }
    XrCliCanonicalSourceRequest request = {
        .schema_version = XR_CLI_CANONICAL_SOURCE_SCHEMA_VERSION,
        .compiler_host = compiler_host,
        .entry_source_path = source_path,
        .entry_function = "main",
        .source_profile = XR_PROGRAM_SOURCE_PROFILE_DEVELOPMENT,
        .semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile),
    };
    XrProgramSourceProduct product = {0};
    XrCliCanonicalSourceDiagnostic diagnostic;
    XrCliCanonicalSourceStatus status =
        xr_cli_canonical_source_build(&request, &product, &diagnostic);
    int result = status == XR_CLI_CANONICAL_SOURCE_OK ? execute_program(&product, profile)
                                                      : report_source_failure(&diagnostic);
    xr_program_source_product_free(&product);
    xray_vm_delete(compiler_host);
    xr_target_profile_free(profile);
    return result;
}
