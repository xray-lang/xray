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
#include "../../base/xplatform.h"
#include "../../execution/xr_execution.h"
#include "../../execution/xr_stdlib_provider_binding.h"
#include "../../os/os_time.h"
#include "../../plan/semantic/xr_semantic_ids.h"
#include "../../plan/target/xr_target_profile.h"
#include "../../runtime/abi/xr_builtin_provider_contract.h"
#include "../../vm/xr_program_vm.h"

#include "xray_vm.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
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

static bool stable_id_equal(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static bool builtin_provider_id(const char *key, XrStableId *out) {
    XrFingerprint digest;
    return xr_stable_id_from_key(key, out, &digest);
}

static const XrTargetProviderContract *find_profile_provider(const XrTargetProfile *profile,
                                                             XrStableId contract_id) {
    for (size_t index = 0; index < xr_target_profile_provider_count(profile); ++index) {
        const XrTargetProviderContract *candidate = xr_target_profile_provider(profile, index);
        if (candidate && stable_id_equal(candidate->contract_id, contract_id))
            return candidate;
    }
    return NULL;
}

static XrProviderCallStatus stdout_output_write(void *context, const uint8_t *bytes, size_t size) {
    FILE *stream = context;
    if (!stream || (!bytes && size != 0u))
        return XR_PROVIDER_CALL_FAILED;
#if XR_OS_WINDOWS
    if (_setmode(_fileno(stream), _O_BINARY) == -1)
        return XR_PROVIDER_CALL_FAILED;
#endif
    return fwrite(bytes, 1u, size, stream) == size ? XR_PROVIDER_CALL_OK : XR_PROVIDER_CALL_FAILED;
}

typedef struct XrRunProviderBindings {
    XrProviderBinding providers[XR_RUNTIME_ABI_MAX_PROVIDERS];
    XrProviderOperationBinding operations[XR_RUNTIME_ABI_MAX_PROVIDERS]
                                         [XR_RUNTIME_ABI_MAX_PROVIDER_OPERATIONS];
    uint32_t count;
} XrRunProviderBindings;

static bool build_run_provider_binding(const XrValidatedProgram *program,
                                       const XrTargetProfile *profile,
                                       XrRunProviderBindings *bindings) {
    if (!program || !profile || !bindings)
        return false;
    memset(bindings, 0, sizeof(*bindings));
    bindings->count = xr_validated_program_provider_requirement_count(program);
    if (bindings->count > XR_RUNTIME_ABI_MAX_PROVIDERS)
        return false;
    XrStableId output_contract = {{0}};
    XrStableId output_operation = {{0}};
    if (!builtin_provider_id(XR_PROVIDER_IO_CONTRACT_KEY, &output_contract) ||
        !builtin_provider_id(XR_PROVIDER_IO_OUTPUT_WRITE_OPERATION_KEY, &output_operation))
        return false;

    for (uint32_t provider_index = 0u; provider_index < bindings->count; ++provider_index) {
        XrProgramProviderRequirementView requirement = {0};
        if (!xr_validated_program_provider_requirement(program, provider_index, &requirement) ||
            requirement.operation_count == 0u ||
            requirement.operation_count > XR_RUNTIME_ABI_MAX_PROVIDER_OPERATIONS)
            return false;
        const XrTargetProviderContract *contract =
            find_profile_provider(profile, requirement.contract_id);
        if (!contract)
            return false;
        XrProviderBinding *provider = &bindings->providers[provider_index];
        provider->contract_id = requirement.contract_id;
        provider->behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;
        provider->operations = bindings->operations[provider_index];
        provider->operation_count = (uint16_t) requirement.operation_count;
        if (xr_target_provider_contract_fingerprint(contract, &provider->contract_fingerprint) !=
            XR_RUNTIME_ABI_OK)
            return false;
        for (uint16_t operation_index = 0u; operation_index < provider->operation_count;
             ++operation_index) {
            XrStableId required_operation = requirement.operations[operation_index].operation_id;
            const XrTargetProviderOperationContract *operation_contract = NULL;
            for (uint16_t candidate = 0u; candidate < contract->operation_count; ++candidate) {
                if (!stable_id_equal(contract->operations[candidate].stable_id, required_operation))
                    continue;
                if (operation_contract)
                    return false;
                operation_contract = &contract->operations[candidate];
            }
            if (!operation_contract)
                return false;
            XrProviderOperationBinding *operation =
                &bindings->operations[provider_index][operation_index];
            operation->operation_id = required_operation;
            if (stable_id_equal(requirement.contract_id, output_contract) &&
                stable_id_equal(required_operation, output_operation) &&
                contract->provider_role == XR_TARGET_PROVIDER_ROLE_OPERATIONS) {
                operation->trampoline_kind = XR_PROVIDER_TRAMPOLINE_OUTPUT_WRITE;
                operation->entry.output_write = stdout_output_write;
                operation->context = stdout;
                provider->behavior_flags &=
                    XR_PROVIDER_BEHAVIOR_THREAD_SAFE | XR_PROVIDER_BEHAVIOR_REENTRANT;
            } else {
                const XrStdlibProviderDescriptor *descriptor =
                    xr_stdlib_provider_find(requirement.contract_id, required_operation);
                uint32_t behavior = 0u;
                if (contract->provider_role != XR_TARGET_PROVIDER_ROLE_OPERATIONS ||
                    !xr_stdlib_provider_operation_binding(descriptor, operation, &behavior))
                    return false;
                provider->behavior_flags &= behavior;
            }
        }
    }
    return true;
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

static int report_execution_outcome(XrVmOutcome outcome) {
    switch (outcome.kind) {
        case XR_VM_OUTCOME_RETURN:
            if (outcome.value.kind == XR_VM_VALUE_VOID)
                return XR_CLI_EXIT_OK;
            fprintf(stderr, "XR_RUN_6007: module initializer must return void\n");
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
        case XR_VM_OUTCOME_CANCELLED:
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

static bool drive_vm_suspension(const XrVmOutcome *outcome) {
    if (!outcome || outcome->kind != XR_VM_OUTCOME_SUSPENDED)
        return false;
    switch (outcome->suspension.kind) {
        case XR_SUSPENSION_REQUEST_COOPERATIVE_YIELD:
            return outcome->suspension.operand_count == 0u;
        case XR_SUSPENSION_REQUEST_TIMER_AFTER_MS: {
            int64_t milliseconds = outcome->suspension.payload.timer_after_ms;
            if (outcome->suspension.operand_count != 1u ||
                milliseconds != xr_suspension_timer_normalize_ms(milliseconds))
                return false;
            xr_time_sleep_ms((uint64_t) milliseconds);
            return true;
        }
        case XR_SUSPENSION_REQUEST_NONE:
            return false;
    }
    return false;
}

static int execute_program(XrProgramSourceProduct *product, XrTargetProfile *profile) {
    XrRunProviderBindings bindings = {0};
    const XrProviderBinding *providers = NULL;
    uint32_t provider_count = xr_validated_program_provider_requirement_count(product->program);
    if (provider_count != 0u) {
        if (!build_run_provider_binding(product->program, profile, &bindings)) {
            fprintf(stderr, "XR_RUN_6008: canonical run cannot bind the exact program provider "
                            "requirements\n");
            return XR_CLI_EXIT_FAIL;
        }
        providers = bindings.providers;
    }
    XrVmCodeOptions options = xr_vm_code_default_options();
    XrVmCodeDiagnostic code_diagnostic;
    XrVmCode *code = NULL;
    XrVmCodeStatus code_status =
        xr_vm_code_build(product->program, profile, &options, &code, &code_diagnostic);
    if (code_status != XR_VM_CODE_OK) {
        fprintf(stderr, "XR_RUN_6002: VM-private code build failed: %s\n",
                xr_vm_code_status_name(code_status));
        return XR_CLI_EXIT_FAIL;
    }

    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = product->program,
        .profile = profile,
        .providers = providers,
        .provider_count = provider_count,
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
        xr_vm_code_free(code);
        return XR_CLI_EXIT_FAIL;
    }

    uint32_t entry = xr_validated_program_entry_function(product->program);
    XrVmOutcome outcome = xr_vm_code_execute(code, instance, entry, NULL, 0u);
    XrVmExecution *execution = NULL;
    if (outcome.kind == XR_VM_OUTCOME_INVALID_INVOCATION &&
        xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution)) {
        do {
            outcome = xr_vm_execution_step(execution);
        } while (outcome.kind == XR_VM_OUTCOME_SUSPENDED && drive_vm_suspension(&outcome));
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
    int result = status == XR_CLI_CANONICAL_SOURCE_OK ? execute_program(&product, profile)
                                                      : report_source_failure(&diagnostic);
    xr_program_source_product_free(&product);
    xray_vm_delete(compiler_host);
    xr_target_profile_free(profile);
    return result;
}
