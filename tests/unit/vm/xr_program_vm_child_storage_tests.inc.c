/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_vm_child_storage_tests.inc.c - Suspended child value lifetime tests
 */

#include "../program/xr_program_indirect_coroutine_fixture.h"

static XrValidatedProgram *child_string_program(bool parent_allocation) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256];
    XrProgramBuildStatus built = xr_program_indirect_coroutine_fixture_write(
        parent_allocation ? XR_PROGRAM_COROUTINE_SEALED_STRING_BUDGET
                          : XR_PROGRAM_COROUTINE_SEALED_STRING_RESULT,
        &artifact, diagnostic, sizeof(diagnostic));
    if (built != XR_PROGRAM_BUILD_OK)
        fprintf(stderr, "child string fixture rejected: %s\n", diagnostic);
    REQUIRE(built == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify;
    XrProgramVerifyStatus verified =
        xr_program_validate(artifact.bytes, artifact.size, NULL, &program, &verify);
    if (verified != XR_PROGRAM_VERIFY_OK)
        fprintf(stderr, "child string admission rejected: %s at %u/%u/%u\n",
                xr_program_diagnostic_kind_name(verify.kind), verify.location.function_id,
                verify.location.block_id, verify.location.instruction_id);
    REQUIRE(verified == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    return program;
}

static void require_child_string_result(XrVmOutcome result) {
    REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
    XrVmStringView view;
    REQUIRE(xr_vm_value_string_view(&result.value, &view));
    REQUIRE(view.size == 2u);
    REQUIRE(memcmp(view.bytes, "21", 2u) == 0);
}

static void test_child_string_execution(XrVmCode *code, XrInstance *instance,
                                         uint32_t entry, uint32_t finish, bool limited) {
    XrVmExecution *first = NULL;
    XrVmExecution *second = NULL;
    REQUIRE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &first));
    REQUIRE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &second));
    XrVmOutcome result = xr_vm_execution_step(first);
    XrVmOutcome other = xr_vm_execution_step(second);
    if (limited) {
        REQUIRE(result.kind == XR_VM_OUTCOME_RESOURCE_LIMIT);
        REQUIRE(other.kind == XR_VM_OUTCOME_RESOURCE_LIMIT);
    } else {
        REQUIRE(result.kind == XR_VM_OUTCOME_SUSPENDED);
        REQUIRE(other.kind == XR_VM_OUTCOME_SUSPENDED);
        if (finish == 0u)
            require_child_string_result(xr_vm_execution_step(first));
        else if (finish == 1u)
            REQUIRE(xr_vm_execution_cancel(first).kind == XR_VM_OUTCOME_CANCELLED);
    }
    xr_vm_execution_free(first);
    if (!limited)
        require_child_string_result(xr_vm_execution_step(second));
    xr_vm_execution_free(second);
    REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
}

static void test_coroutine_child_string_storage(void) {
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    for (uint32_t allocation = 0u; allocation < 2u; ++allocation) {
        XrValidatedProgram *program = child_string_program(allocation != 0u);
        {
            for (uint32_t budget = 5u; budget <= 6u; ++budget) {
                XrVmCodeOptions options = xr_vm_code_default_options();
                options.max_value_cells = budget;
                XrVmCode *code = NULL;
                REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
                XrInstance *instance = create_instance(program, profile, &bindings, 0u + 1u);
                for (uint32_t finish = 0u; finish < 3u; ++finish)
                    test_child_string_execution(code, instance,
                                                xr_validated_program_entry_function(program),
                                                finish, allocation != 0u && budget == 5u);
                xr_vm_code_free(code);
                retire_and_free(&instance);
            }
        }
        xr_validated_program_free(program);
    }
    xr_target_profile_free(profile);
}
