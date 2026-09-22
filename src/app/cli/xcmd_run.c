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
#include "xcli_program_vm.h"
#include "../toolchain/xtc_target_profile.h"
#include "../../api/xisolate_profile.h"
#include "../../plan/target/xr_target_profile.h"
#include "../../shared/xr_value_format_core.h"

#include "xray_vm.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#if XR_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

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

static int report_source_failure(const XrCliCanonicalSourceDiagnostic *diagnostic) {
    if (diagnostic && diagnostic->status == XR_CLI_CANONICAL_SOURCE_BUILD_REJECTED &&
        diagnostic->build.source_path[0] && diagnostic->build.source_line > 0u) {
        fprintf(stderr, "%s:%u:%u: error", diagnostic->build.source_path,
                diagnostic->build.source_line, diagnostic->build.source_column);
        if (diagnostic->build.underlying_status > 0u)
            fprintf(stderr, "[E%04u]", diagnostic->build.underlying_status);
        fprintf(stderr, ": %s\n",
                diagnostic->message[0] ? diagnostic->message : "source build failed");
        return XR_CLI_EXIT_FAIL;
    }
    fprintf(stderr, "XR_RUN_6001: canonical source build failed: %s",
            diagnostic && diagnostic->message[0] ? diagnostic->message : "unknown failure");
    if (diagnostic && diagnostic->status == XR_CLI_CANONICAL_SOURCE_BUILD_REJECTED)
        fprintf(stderr, " (stage=%u status=%s)", (unsigned) diagnostic->build.stage,
                xr_program_source_build_status_name(diagnostic->build.status));
    fputc('\n', stderr);
    return XR_CLI_EXIT_FAIL;
}

static int report_execution_outcome(XrCliVmResult outcome) {
    switch (outcome.kind) {
        case XR_VM_OUTCOME_RETURN:
            if (outcome.value_kind == XR_VM_VALUE_VOID)
                return XR_CLI_EXIT_OK;
            fprintf(stderr, "XR_RUN_6007: module initializer must return void\n");
            return XR_CLI_EXIT_FAIL;
        case XR_VM_OUTCOME_TRAP:
            fprintf(stderr,
                    "XR_RUN_6004: canonical execution trapped (trap=%u steps=%" PRIu64 ")\n",
                    (unsigned) outcome.trap, outcome.steps);
            return XR_CLI_EXIT_FAIL;
        case XR_VM_OUTCOME_ERROR:
            if (!outcome.error_reported)
                fprintf(stderr, "XR_RUN_6005: cannot render uncaught typed error\n");
            return XR_CLI_EXIT_FAIL;
        case XR_VM_OUTCOME_PANIC:
            if (!outcome.panic_reported)
                fprintf(stderr, "XR_RUN_6005: cannot render uncaught typed panic\n");
            return XR_CLI_EXIT_FAIL;
        case XR_VM_OUTCOME_RESOURCE_LIMIT:
            fprintf(stderr, "XR_RUN_6003: canonical execution exceeded its resource budget\n");
            return XR_CLI_EXIT_FAIL;
        case XR_VM_OUTCOME_CANCELLED:
        case XR_VM_OUTCOME_SUSPENDED:
        case XR_VM_OUTCOME_INVALID_INVOCATION:
        case XR_VM_OUTCOME_STALE_CODE:
        case XR_VM_OUTCOME_INITIALIZING:
            fprintf(stderr, "XR_RUN_6002: canonical execution was rejected (outcome=%u)\n",
                    (unsigned) outcome.kind);
            return XR_CLI_EXIT_FAIL;
    }
    fprintf(stderr, "XR_RUN_6002: canonical execution returned an unknown outcome\n");
    return XR_CLI_EXIT_INTERNAL;
}

static int execute_program(XrProgramSourceProduct *product, XrTargetProfile *profile) {
    XrCliProgramVm vm = {0};
    char error[512] = {0};
    if (!xr_cli_program_vm_open(&vm, product->program, profile, error, sizeof(error))) {
        fprintf(stderr, "XR_RUN_6002: %s\n", error);
        return XR_CLI_EXIT_FAIL;
    }
    XrValueFormatSink errors = {stderr, xr_value_format_file_write};
#if XR_OS_WINDOWS
    if (_setmode(_fileno(stderr), _O_BINARY) == -1) {
        (void) xr_cli_program_vm_close(&vm);
        return XR_CLI_EXIT_INTERNAL;
    }
#endif
    XrCliVmResult outcome = xr_cli_program_vm_invoke(
        &vm, xr_validated_program_entry_function(product->program), 0.0, &errors);
    int result = report_execution_outcome(outcome);
    if (!xr_cli_program_vm_close(&vm)) {
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
        fprintf(stderr, "XR_RUN_6010: a module initializer does not accept arguments\n");
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
        .entry_function = NULL,
        .entry_kind = XR_PROGRAM_SOURCE_ENTRY_MODULE_INITIALIZER,
        .source_profile = XR_PROGRAM_SOURCE_PROFILE_DEVELOPMENT,
        .semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile),
    };
    XrProgramSourceProduct product = {0};
    XrCliCanonicalSourceDiagnostic diagnostic;
    XrCliCanonicalSourceStatus status =
        xr_cli_canonical_source_build(&request, &product, &diagnostic);
    xray_vm_delete(compiler_host);
    int result = status == XR_CLI_CANONICAL_SOURCE_OK ? execute_program(&product, profile)
                                                      : report_source_failure(&diagnostic);
    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
    return result;
}
