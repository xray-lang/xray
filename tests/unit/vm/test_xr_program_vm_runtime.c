/* Task 299: an embedder that links only the canonical XrProgram VM runtime. */

#include "execution/xr_execution.h"
#include "program/xr_program_verify.h"
#include "vm/xr_program_vm.h"
#include "../plan/target_profile_test_fixture.h"
#include "xr_program_vm_embedded_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

typedef struct RuntimeBindings {
    XrProviderBinding providers[XR_RUNTIME_ABI_MAX_PROVIDERS];
    XrProviderOperationBinding operations[XR_RUNTIME_ABI_MAX_PROVIDERS]
                                         [XR_RUNTIME_ABI_MAX_PROVIDER_OPERATIONS];
    size_t count;
} RuntimeBindings;

static void provider_entry(void) {
}

static void build_bindings(const XrTargetProfile *profile, RuntimeBindings *bindings) {
    memset(bindings, 0, sizeof(*bindings));
    bindings->count = xr_target_profile_provider_count(profile);
    REQUIRE(bindings->count > 0u);
    for (size_t provider_index = 0; provider_index < bindings->count; ++provider_index) {
        const XrTargetProviderContract *contract =
            xr_target_profile_provider(profile, provider_index);
        REQUIRE(contract != NULL);
        XrProviderBinding *provider = &bindings->providers[provider_index];
        provider->contract_id = contract->contract_id;
        REQUIRE(xr_target_provider_contract_fingerprint(
                    contract, &provider->contract_fingerprint) == XR_RUNTIME_ABI_OK);
        provider->behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;
        provider->operations = bindings->operations[provider_index];
        provider->operation_count = contract->operation_count;
        for (uint16_t operation_index = 0; operation_index < contract->operation_count;
             ++operation_index) {
            XrProviderOperationBinding *operation =
                &bindings->operations[provider_index][operation_index];
            operation->operation_id = contract->operations[operation_index].stable_id;
            operation->entry = provider_entry;
        }
    }
}

int main(void) {
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic program_diagnostic;
    REQUIRE(xr_program_validate(xr_program_vm_embedded_fixture,
                                (size_t) xr_program_vm_embedded_fixture_size, NULL, &program,
                                &program_diagnostic) == XR_PROGRAM_VERIFY_OK);
    REQUIRE(program != NULL);

    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    RuntimeBindings bindings;
    build_bindings(profile, &bindings);
    XrExecutionBindingInput input = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program,
        .profile = profile,
        .providers = bindings.providers,
        .provider_count = bindings.count,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    REQUIRE(xr_execution_instance_create(&input, &instance, &execution_diagnostic) ==
            XR_EXECUTION_OK);

    XrVmCode *code = NULL;
    XrVmCodeDiagnostic code_diagnostic;
    REQUIRE(xr_vm_code_build(instance, NULL, &code, &code_diagnostic) == XR_VM_CODE_OK);
    XrVmOutcome result =
        xr_vm_code_execute(code, instance, xr_validated_program_entry_function(program), NULL, 0u);
    REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
    REQUIRE(result.value.kind == XR_VM_VALUE_I64);
    REQUIRE(result.value.as.i64 == 42);
    xr_vm_code_free(code);

    REQUIRE(xr_execution_instance_begin_drain(instance, &execution_diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_retire(instance, &execution_diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&instance, &execution_diagnostic) == XR_EXECUTION_OK);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    puts("task-299 runtime-only embedder passed");
    return 0;
}
