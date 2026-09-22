/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_native_execution_allocations.c - Native frame/state allocation failure ownership
 */

#include "execution/xr_native_execution.h"
#include "program/xr_program_verify.h"
#include "../plan/target_profile_test_fixture.h"
#include "../program/xr_program_coroutine_fixture.h"
#include "../program/xr_program_allocation_probe.h"

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

static size_t attempt, fail_at, live_count;
static void *live[8];

void *xr_program_test_calloc(size_t count, size_t size) {
    if (++attempt == fail_at)
        return NULL;
    void *pointer = xr_program_test_system_calloc(count, size);
    REQUIRE(pointer);
    for (size_t i = 0u; i < XR_COUNTOF(live); ++i) {
        if (live[i])
            continue;
        live[i] = pointer;
        ++live_count;
        return pointer;
    }
    abort();
}

void xr_program_test_free(void *pointer) {
    if (!pointer)
        return;
    for (size_t i = 0u; i < XR_COUNTOF(live); ++i) {
        if (live[i] != pointer)
            continue;
        live[i] = NULL;
        --live_count;
        xr_program_test_system_free(pointer);
        return;
    }
    abort();
}

int main(void) {
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_coroutine_fixture_write(&artifact, NULL, 0u) == XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program,
        .profile = profile,
        .generation = 1u,
    };
    for (size_t failure = 1u; failure <= 4u; ++failure) {
        XrInstance *instance = NULL;
        REQUIRE(xr_execution_instance_create(&binding, &instance, NULL) == XR_EXECUTION_OK);
        fail_at = failure;
        attempt = 0u;
        XrBackendExecution *execution = NULL;
        REQUIRE(
            !xr_backend_execution_create(instance, &xr_aot_entry_coroutine_descriptor, &execution));
        REQUIRE(!execution && attempt == failure && live_count == 0u);
        REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
        XrExecutionLease lease = {0};
        REQUIRE(xr_execution_instance_acquire(instance, &lease) == XR_EXECUTION_OK);
        void *state = NULL;
        REQUIRE(xr_execution_lease_bind_state(&lease,
                                              xr_aot_entry_coroutine_descriptor.state_layout, NULL,
                                              NULL, &state) == XR_EXECUTION_STATE_EMPTY);
        REQUIRE(!state && xr_execution_lease_release(&lease));
        fail_at = 0u;
        attempt = 0u;
        REQUIRE(
            xr_backend_execution_create(instance, &xr_aot_entry_coroutine_descriptor, &execution));
        REQUIRE(attempt == 4u && live_count == 4u);
        REQUIRE(xr_backend_execution_step(execution).kind == XR_BACKEND_EXECUTION_SUSPENDED);
        for (size_t later_failure = 1u; later_failure <= 2u; ++later_failure) {
            attempt = 0u;
            fail_at = later_failure;
            XrBackendExecution *rejected = NULL;
            REQUIRE(!xr_backend_execution_create(instance, &xr_aot_entry_coroutine_descriptor,
                                                 &rejected));
            REQUIRE(!rejected && attempt == later_failure && live_count == 4u);
            REQUIRE(xr_execution_instance_lease_count(instance) == 1u);
        }
        fail_at = 0u;
        XrBackendExecutionOutcome returned = xr_backend_execution_step(execution);
        REQUIRE(returned.kind == XR_BACKEND_EXECUTION_RETURN && returned.value == 42);
        xr_backend_execution_free(execution);
        REQUIRE(live_count == 2u && xr_execution_instance_lease_count(instance) == 0u);
        REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(live_count == 0u);
        REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
    }
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    puts("native allocation failures: four initial and two existing-state sites passed");
    return 0;
}
