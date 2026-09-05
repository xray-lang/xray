/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_program_aot_coroutine_native_step.c - Generated coroutine step lifecycle test
 *
 * KEY CONCEPT:
 *   The lifecycle adapter calls a separately compiled generated-C step function.
 *   It owns only the native frame and generation lease, never Core or BackendIR.
 */

#include "aot/program/xr_backend_ir.h"
#include "execution/xr_execution.h"
#include "program/xr_program.h"
#include "program/xr_program_verify.h"
#include "../plan/target_profile_test_fixture.h"
#include "../program/xr_program_coroutine_fixture.h"

#include <stdio.h>
#include <stdlib.h>

extern const XrBackendNativeDescriptor xr_aot_entry_coroutine_descriptor;

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static XrValidatedProgram *build_coroutine_program(void) {
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    char diagnostic[256] = {0};
    REQUIRE(xr_program_coroutine_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    return program;
}

static XrInstance *create_instance(XrValidatedProgram *program, XrTargetProfile *profile) {
    XrExecutionBindingInput input = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program,
        .profile = profile,
        .generation = UINT64_C(1),
    };
    XrExecutionDiagnostic diagnostic;
    XrInstance *instance = NULL;
    REQUIRE(xr_execution_instance_create(&input, &instance, &diagnostic) == XR_EXECUTION_OK);
    return instance;
}

int main(void) {
    XrValidatedProgram *program = build_coroutine_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrInstance *instance = create_instance(program, profile);
    REQUIRE(xr_fingerprint_equal(xr_aot_entry_coroutine_descriptor.execution_id,
                                 xr_execution_instance_id(instance)));

    XrBackendNativeDescriptor mismatch = xr_aot_entry_coroutine_descriptor;
    mismatch.execution_id.bytes[0] ^= UINT8_C(1);
    XrBackendExecution *rejected = NULL;
    REQUIRE(!xr_backend_execution_create(instance, &mismatch, &rejected));
    REQUIRE(rejected == NULL);
    REQUIRE(xr_execution_instance_lease_count(instance) == 0u);

    XrBackendNativeDescriptor wrong_schema = xr_aot_entry_coroutine_descriptor;
    wrong_schema.schema_version += 1u;
    REQUIRE(!xr_backend_execution_create(instance, &wrong_schema, &rejected));
    REQUIRE(rejected == NULL);

    XrBackendExecution *execution = NULL;
    REQUIRE(xr_backend_execution_create(instance, &xr_aot_entry_coroutine_descriptor,
                                        &execution));
    REQUIRE(xr_execution_instance_lease_count(instance) == 1u);

    XrBackendExecutionOutcome yielded = xr_backend_execution_step(execution);
    REQUIRE(yielded.kind == XR_BACKEND_EXECUTION_SUSPENDED);
    REQUIRE(yielded.state_id == 1u && yielded.safepoint_id == 0u);

    XrExecutionDiagnostic execution_diagnostic;
    REQUIRE(xr_execution_instance_begin_drain(instance, &execution_diagnostic) ==
            XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_retire(instance, &execution_diagnostic) ==
            XR_EXECUTION_GENERATION_REJECTED);
    REQUIRE(execution_diagnostic.kind == XR_EXECUTION_DIAGNOSTIC_GENERATION_BUSY);

    REQUIRE(!xr_backend_execution_create(instance, &xr_aot_entry_coroutine_descriptor,
                                         &rejected));
    REQUIRE(rejected == NULL);

    XrBackendExecutionOutcome returned = xr_backend_execution_step(execution);
    REQUIRE(returned.kind == XR_BACKEND_EXECUTION_RETURN && returned.value == 42);
    REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
    REQUIRE(xr_execution_instance_retire(instance, &execution_diagnostic) == XR_EXECUTION_OK);
    xr_backend_execution_free(execution);
    REQUIRE(xr_execution_instance_free(&instance, &execution_diagnostic) == XR_EXECUTION_OK);

    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    puts("generated native coroutine step lifecycle test passed");
    return 0;
}
