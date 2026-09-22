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

static void build_bindings(const XrTargetProfile *profile, RuntimeBindings *bindings) {
    (void) profile;
    memset(bindings, 0, sizeof(*bindings));
}

int main(void) {
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic program_diagnostic;
    unsigned char corrupted[sizeof(xr_program_vm_embedded_fixture)];
    memcpy(corrupted, xr_program_vm_embedded_fixture, sizeof(corrupted));
    corrupted[XR_PROGRAM_MAGIC_SIZE + 2u] = UINT8_C(3);
    REQUIRE(xr_program_validate(corrupted, sizeof(corrupted), NULL, &program,
                                &program_diagnostic) == XR_PROGRAM_VERIFY_STRUCTURAL_REJECTED);
    REQUIRE(program == NULL);
    corrupted[XR_PROGRAM_MAGIC_SIZE + 2u] = UINT8_C(4);
    REQUIRE(xr_program_validate(corrupted, sizeof(corrupted), NULL, &program,
                                &program_diagnostic) == XR_PROGRAM_VERIFY_STRUCTURAL_REJECTED);
    REQUIRE(program == NULL);
    memcpy(corrupted, xr_program_vm_embedded_fixture, sizeof(corrupted));
    corrupted[XR_PROGRAM_MAGIC_SIZE + 4u + 1u] ^= UINT8_C(1);
    REQUIRE(xr_program_validate(corrupted, sizeof(corrupted), NULL, &program,
                                &program_diagnostic) == XR_PROGRAM_VERIFY_SEMANTIC_REJECTED);
    REQUIRE(program == NULL && program_diagnostic.kind == XR_PROGRAM_DIAGNOSTIC_CORE_SPEC_IDENTITY);
    /* The exact pre-message contract is obsolete; no fallback decoder accepts it. */
    static const unsigned char old_core_spec[32] = {0x80,0xe7,0xef,0xb9,0x72,0xdd,0xfe,0xde,0x0f,0xf4,0x05,0xc0,0xce,0x17,0xf4,0x26,0x7a,0x6b,0x81,0x94,0x6c,0xac,0xde,0xad,0x75,0x22,0x04,0x25,0xe1,0xeb,0x1c,0x89};
    memcpy(corrupted, xr_program_vm_embedded_fixture, sizeof(corrupted));
    memcpy(corrupted + 13u, old_core_spec, sizeof(old_core_spec));
    REQUIRE(xr_program_validate(corrupted, sizeof(corrupted), NULL, &program,
                                &program_diagnostic) == XR_PROGRAM_VERIFY_SEMANTIC_REJECTED);
    REQUIRE(program == NULL && program_diagnostic.kind == XR_PROGRAM_DIAGNOSTIC_CORE_SPEC_IDENTITY);
    REQUIRE(xr_program_validate(xr_program_vm_embedded_fixture,
                                (size_t) xr_program_vm_embedded_fixture_size, NULL, &program,
                                &program_diagnostic) == XR_PROGRAM_VERIFY_OK);
    REQUIRE(program != NULL);

    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    RuntimeBindings bindings;
    build_bindings(profile, &bindings);
    XrVmCode *code = NULL;
    XrVmCodeDiagnostic code_diagnostic;
    REQUIRE(xr_vm_code_build(program, profile, NULL, &code, &code_diagnostic) == XR_VM_CODE_OK);
    uint32_t entry = xr_validated_program_entry_function(program);
    XrExecutionBindingInput input = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program,
        .profile = profile,
        .providers = bindings.count ? bindings.providers : NULL,
        .provider_count = bindings.count,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    REQUIRE(xr_execution_instance_create(&input, &instance, &execution_diagnostic) ==
            XR_EXECUTION_OK);

    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    XrVmOutcome result = xr_vm_code_execute(code, instance, entry, NULL, 0u);
    REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
    REQUIRE(result.value.kind == XR_VM_VALUE_I64);
    REQUIRE(result.value.as.i64 == 42);

    REQUIRE(xr_execution_instance_begin_drain(instance, &execution_diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_retire(instance, &execution_diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&instance, &execution_diagnostic) == XR_EXECUTION_OK);
    xr_vm_code_free(code);
    puts("task-299 runtime-only embedder passed");
    return 0;
}
