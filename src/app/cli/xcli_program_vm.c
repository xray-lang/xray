/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_program_vm.c - Shared CLI Program execution owner
 *
 * KEY CONCEPT:
 *   Source run has one route: source owner to validated XrProgram, exact
 *   target profile and instance, then a VM-private executable view.
 */

#include "xcli_program_vm.h"
#include "xcli.h"
#include "xcli_output.h"
#include "../../execution/xr_stdlib_provider_binding.h"
#include "../../os/os_time.h"
#include "../../plan/semantic/xr_semantic_ids.h"
#include "../../plan/target/xr_target_profile.h"
#include <stdio.h>
#include <string.h>
#if XR_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

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

static bool build_provider_bindings(const XrValidatedProgram *program,
                                    const XrTargetProfile *profile,
                                    XrCliProviderBindings *bindings) {
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

bool xr_cli_program_vm_open(XrCliProgramVm *vm, XrValidatedProgram *program,
                            XrTargetProfile *profile, char *error, size_t error_size) {
    if (!vm || !program || !profile || !error || !error_size)
        return false;
    memset(vm, 0, sizeof(*vm));
    if (!build_provider_bindings(program, profile, &vm->bindings)) {
        snprintf(error, error_size, "cannot bind exact Program provider requirements");
        return false;
    }
    XrVmCodeOptions options = xr_vm_code_default_options();
    options.max_steps = UINT64_MAX;
    XrVmCodeDiagnostic code_diagnostic;
    XrVmCodeStatus status =
        xr_vm_code_build(program, profile, &options, &vm->code, &code_diagnostic);
    if (status != XR_VM_CODE_OK) {
        snprintf(error, error_size, "VM code build failed: %s", xr_vm_code_status_name(status));
        return false;
    }
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program,
        .profile = profile,
        .providers = vm->bindings.count ? vm->bindings.providers : NULL,
        .provider_count = vm->bindings.count,
        .generation = 1u,
    };
    XrExecutionDiagnostic diagnostic;
    XrExecutionStatus execution_status =
        xr_execution_instance_create(&binding, &vm->instance, &diagnostic);
    if (execution_status != XR_EXECUTION_OK) {
        snprintf(error, error_size, "execution binding failed: %s/%s",
                 xr_execution_status_name(execution_status),
                 xr_execution_diagnostic_kind_name(diagnostic.kind));
        xr_vm_code_free(vm->code);
        vm->code = NULL;
        return false;
    }
    return true;
}

bool xr_cli_program_vm_close(XrCliProgramVm *vm) {
    if (!vm)
        return true;
    if (!retire_instance(&vm->instance))
        return false;
    xr_vm_code_free(vm->code);
    vm->code = NULL;
    return true;
}

typedef struct XrCliVmDeadline {
    double at_ms;
    bool expired;
} XrCliVmDeadline;

static bool deadline_expired(void *context) {
    XrCliVmDeadline *deadline = context;
    if (deadline->at_ms > 0.0 && xr_cli_get_time_ms() >= deadline->at_ms)
        deadline->expired = true;
    return deadline->expired;
}

XrCliVmResult xr_cli_program_vm_invoke(XrCliProgramVm *vm, uint32_t function_id,
                                       double deadline_ms, const XrValueFormatSink *errors) {
    XrCliVmResult result = {.kind = XR_VM_OUTCOME_INVALID_INVOCATION};
    XrVmExecution *execution = NULL;
    if (!vm || !xr_vm_execution_create(vm->code, vm->instance, function_id, NULL, 0u, &execution))
        return result;
    XrCliVmDeadline deadline = {.at_ms = deadline_ms};
    if (deadline_ms > 0.0 &&
        !xr_vm_execution_set_interrupt(execution, &deadline, deadline_expired)) {
        xr_vm_execution_free(execution);
        return result;
    }
    XrVmOutcome outcome;
    for (;;) {
        outcome = xr_vm_execution_step(execution);
        if (outcome.kind != XR_VM_OUTCOME_SUSPENDED)
            break;
        if (outcome.suspension.kind == XR_SUSPENSION_REQUEST_COOPERATIVE_YIELD &&
            outcome.suspension.operand_count == 0u)
            continue;
        if (outcome.suspension.kind != XR_SUSPENSION_REQUEST_TIMER_AFTER_MS ||
            outcome.suspension.operand_count != 1u)
            break;
        int64_t milliseconds = outcome.suspension.payload.timer_after_ms;
        if (milliseconds != xr_suspension_timer_normalize_ms(milliseconds))
            break;
        double remaining = deadline_ms > 0.0 ? deadline_ms - xr_cli_get_time_ms() : 0.0;
        if (deadline_ms > 0.0 && remaining < (double) milliseconds) {
            if (remaining > 0.0)
                xr_time_sleep_ms((uint64_t) remaining);
            deadline.expired = true;
            /* Resume only into the interrupt check; do not run a user operation. */
            continue;
        }
        xr_time_sleep_ms((uint64_t) milliseconds);
    }
    result.kind = outcome.kind;
    result.trap = outcome.trap;
    result.steps = outcome.steps;
    result.value_kind = outcome.value.kind;
    if (outcome.panic_value.kind == XR_VM_VALUE_PANIC_INFO)
        result.panic_info = outcome.panic_value.as.panic_info;
    result.timed_out = deadline.expired;
    if (errors && outcome.kind == XR_VM_OUTCOME_ERROR)
        result.error_reported = xr_value_format_uncaught(xr_vm_value_format_reader(vm->code),
            (XrValueFormatNode) {&outcome.error_value, 0u}, *errors, 0) != 0;
    if (errors && outcome.kind == XR_VM_OUTCOME_PANIC) {
        XrVmStringView message = {0};
        bool has_message = xr_vm_panic_message_view(&result.panic_info, &message);
        result.panic_reported = xr_value_format_panic(*errors, result.panic_info.code,
            result.panic_info.has_bounds, result.panic_info.index, result.panic_info.length,
            has_message, message.bytes, message.size) != 0;
    }
    result.panic_info.message = NULL;
    xr_vm_execution_free(execution);
    return result;
}
