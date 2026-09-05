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
 *   It owns only the native frame, never Core, BackendIR, or VM execution state.
 */

#include "aot/program/xr_backend_ir.h"
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

int main(void) {
    XrValidatedProgram *program = build_coroutine_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrExecutionId execution_id = {0};
    REQUIRE(xr_execution_id_compute(program, profile, &execution_id));
    REQUIRE(xr_fingerprint_equal(xr_aot_entry_coroutine_descriptor.execution_id,
                                 execution_id));

    XrBackendNativeDescriptor mismatch = xr_aot_entry_coroutine_descriptor;
    mismatch.execution_id.bytes[0] ^= UINT8_C(1);
    XrBackendExecution *rejected = NULL;
    REQUIRE(!xr_backend_execution_create(execution_id, &mismatch, &rejected));
    REQUIRE(rejected == NULL);

    XrBackendNativeDescriptor wrong_schema = xr_aot_entry_coroutine_descriptor;
    wrong_schema.schema_version += 1u;
    REQUIRE(!xr_backend_execution_create(execution_id, &wrong_schema, &rejected));
    REQUIRE(rejected == NULL);

    XrBackendExecution *execution = NULL;
    REQUIRE(xr_backend_execution_create(execution_id, &xr_aot_entry_coroutine_descriptor,
                                        &execution));

    XrBackendExecutionOutcome yielded = xr_backend_execution_step(execution);
    REQUIRE(yielded.kind == XR_BACKEND_EXECUTION_SUSPENDED);
    REQUIRE(yielded.state_id == 1u && yielded.safepoint_id == 0u);

    XrBackendExecutionOutcome returned = xr_backend_execution_step(execution);
    REQUIRE(returned.kind == XR_BACKEND_EXECUTION_RETURN && returned.value == 42);
    REQUIRE(xr_backend_execution_step(execution).kind == XR_BACKEND_EXECUTION_INVALID);
    xr_backend_execution_free(execution);

    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    puts("generated native coroutine step lifecycle test passed");
    return 0;
}
