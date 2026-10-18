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
 *   It owns the native frame and one instance lease through frame destruction.
 *   It never interprets Core, BackendIR, or VM execution state.
 */

#include "execution/xr_native_execution.h"
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

static XrInstance *drop_instance;
static uint32_t drop_calls;

static void observe_native_frame_drop(void *frame) {
    REQUIRE(xr_execution_instance_lease_count(drop_instance) == 1u);
    REQUIRE(xr_execution_instance_retire(drop_instance, NULL) == XR_EXECUTION_GENERATION_REJECTED);
    xr_aot_entry_coroutine_descriptor.drop(frame);
    ++drop_calls;
}

int main(void) {
    XrValidatedProgram *program = build_coroutine_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrExecutionId execution_id = {0};
    REQUIRE(xr_execution_id_compute(program, profile, &execution_id));
    REQUIRE(xr_fingerprint_equal(xr_aot_entry_coroutine_descriptor.execution_id,
                                 execution_id));
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program,
        .profile = profile,
        .generation = 1u,
    };
    XrInstance *instance = NULL;
    REQUIRE(xr_execution_instance_create(&binding, &instance, NULL) == XR_EXECUTION_OK);

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
    REQUIRE(xr_backend_execution_create(instance, &xr_aot_entry_coroutine_descriptor, &execution));
    REQUIRE(xr_execution_instance_lease_count(instance) == 1u);

    XrBackendExecutionOutcome yielded = xr_backend_execution_step(execution);
    REQUIRE(yielded.kind == XR_BACKEND_EXECUTION_SUSPENDED);
    REQUIRE(yielded.state_id == 1u && yielded.safepoint_id == 0u);

    XrBackendExecutionOutcome returned = xr_backend_execution_step(execution);
    REQUIRE(returned.kind == XR_BACKEND_EXECUTION_RETURN && returned.value == 42);
    REQUIRE(xr_backend_execution_step(execution).kind == XR_BACKEND_EXECUTION_INVALID);
    xr_backend_execution_free(execution);
    REQUIRE(xr_execution_instance_lease_count(instance) == 0u);

    XrBackendExecution *cancel_execution = NULL;
    XrBackendNativeDescriptor observed = xr_aot_entry_coroutine_descriptor;
    observed.drop = observe_native_frame_drop;
    drop_instance = instance;
    REQUIRE(xr_backend_execution_create(instance, &observed, &cancel_execution));
    REQUIRE(xr_backend_execution_cancel(cancel_execution).kind == XR_BACKEND_EXECUTION_INVALID);
    XrBackendExecutionOutcome cancel_yield = xr_backend_execution_step(cancel_execution);
    REQUIRE(cancel_yield.kind == XR_BACKEND_EXECUTION_SUSPENDED);
    REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_GENERATION_REJECTED);
    REQUIRE(!xr_backend_execution_create(instance, &xr_aot_entry_coroutine_descriptor, &rejected));
    REQUIRE(rejected == NULL);
    XrBackendExecutionOutcome cancelled = xr_backend_execution_cancel(cancel_execution);
    REQUIRE(cancelled.kind == XR_BACKEND_EXECUTION_CANCELLED);
    REQUIRE(cancelled.state_id == cancel_yield.state_id);
    REQUIRE(xr_backend_execution_step(cancel_execution).kind == XR_BACKEND_EXECUTION_INVALID);
    REQUIRE(xr_backend_execution_cancel(cancel_execution).kind == XR_BACKEND_EXECUTION_INVALID);
    xr_backend_execution_free(cancel_execution);
    REQUIRE(drop_calls == 1u);
    REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
    REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);

    XrInstance *successor = NULL;
    REQUIRE(xr_execution_instance_create_successor(instance, NULL, 0u, &successor, NULL) ==
            XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_generation(successor) == 2u);
    REQUIRE(xr_backend_execution_create(successor, &xr_aot_entry_coroutine_descriptor, &execution));
    REQUIRE(xr_backend_execution_step(execution).kind == XR_BACKEND_EXECUTION_SUSPENDED);
    returned = xr_backend_execution_step(execution);
    REQUIRE(returned.kind == XR_BACKEND_EXECUTION_RETURN && returned.value == 42);
    REQUIRE(xr_execution_instance_begin_drain(successor, NULL) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_retire(successor, NULL) == XR_EXECUTION_GENERATION_REJECTED);
    xr_backend_execution_free(execution);
    REQUIRE(xr_execution_instance_retire(successor, NULL) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&successor, NULL) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);

    REQUIRE(xr_execution_instance_create(&binding, &instance, NULL) == XR_EXECUTION_OK);
    drop_instance = instance;
    REQUIRE(xr_backend_execution_create(instance, &observed, &execution));
    REQUIRE(xr_backend_execution_step(execution).kind == XR_BACKEND_EXECUTION_SUSPENDED);
    REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
    xr_backend_execution_free(execution);
    REQUIRE(drop_calls == 2u);
    REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
    REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);

    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    puts("generated native coroutine step lifecycle test passed");
    return 0;
}
