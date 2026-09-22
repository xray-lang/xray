/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_execution_allocations.c - Lease and resource pin allocation atomicity
 *
 * KEY CONCEPT:
 *   Ticket allocation failure preserves existing authority and module cleanup.
 *   Successful growth always moves storage to expose retained interior pointers.
 */

#include "execution/xr_execution.h"
#include "program/xr_program_verify.h"
#include "../plan/target_profile_test_fixture.h"
#include "../program/xr_program_module_fixture.h"
#include "../program/xr_program_allocation_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static size_t attempt, fail_at, live_count;
static struct { void *pointer; size_t size; } live[8];

static void track(void *pointer, size_t size) {
    REQUIRE(pointer != NULL);
    for (size_t index = 0u; index < XR_COUNTOF(live); ++index) {
        if (live[index].pointer)
            continue;
        live[index].pointer = pointer;
        live[index].size = size;
        ++live_count;
        return;
    }
    abort();
}

void *xr_program_test_malloc(size_t size) {
    if (++attempt == fail_at)
        return NULL;
    void *pointer = xr_program_test_system_malloc(size);
    track(pointer, size);
    return pointer;
}

void *xr_program_test_calloc(size_t count, size_t size) {
    if (++attempt == fail_at)
        return NULL;
    REQUIRE(size == 0u || count <= SIZE_MAX / size);
    void *pointer = xr_program_test_system_calloc(count, size);
    track(pointer, count * size);
    return pointer;
}

void *xr_program_test_realloc(void *pointer, size_t size) {
    if (++attempt == fail_at)
        return NULL;
    void *moved = xr_program_test_system_malloc(size);
    REQUIRE(moved != NULL);
    if (!pointer) {
        track(moved, size);
        return moved;
    }
    for (size_t index = 0u; index < XR_COUNTOF(live); ++index) {
        if (live[index].pointer != pointer)
            continue;
        memcpy(moved, pointer, live[index].size < size ? live[index].size : size);
        xr_program_test_system_free(pointer);
        live[index].pointer = moved;
        live[index].size = size;
        return moved;
    }
    abort();
}

void xr_program_test_free(void *pointer) {
    if (!pointer)
        return;
    for (size_t index = 0u; index < XR_COUNTOF(live); ++index) {
        if (live[index].pointer != pointer)
            continue;
        live[index].pointer = NULL;
        live[index].size = 0u;
        --live_count;
        xr_program_test_system_free(pointer);
        return;
    }
    abort();
}

typedef struct StateProbe {
    XrExecutionResourcePin pin;
    unsigned destroyed;
} StateProbe;

static void destroy_state(void *opaque) {
    StateProbe *state = opaque;
    REQUIRE(state->destroyed == 0u);
    REQUIRE(xr_execution_resource_pin_release(&state->pin));
    ++state->destroyed;
}

static void fail_next_allocation(void) {
    attempt = 0u;
    fail_at = 1u;
}

static unsigned payload_destroys;

static void destroy_owned_payload(void *payload) {
    REQUIRE(*(int64_t *) payload == 42);
    ++payload_destroys;
    xr_program_test_system_free(payload);
}

static void test_payload_allocation_failures(const XrExecutionBindingInput *input) {
    for (size_t failure = 1u; failure <= 2u; ++failure) {
        fail_at = 0u;
        XrInstance *instance = NULL;
        REQUIRE(xr_execution_instance_create(input, &instance, NULL) == XR_EXECUTION_OK);
        XrExecutionLease lease = {0};
        REQUIRE(xr_execution_instance_acquire(instance, &lease) == XR_EXECUTION_OK);
        XrExecutionResourcePin pins[7] = {0};
        for (size_t index = 0u; index < XR_COUNTOF(pins); ++index)
            REQUIRE(xr_execution_resource_pin_acquire(&lease, &pins[index]));
        int64_t *payload = xr_program_test_system_malloc(sizeof(*payload));
        REQUIRE(payload != NULL);
        *payload = 42;
        XrExecutionResource *resource = NULL;
        attempt = 0u;
        fail_at = failure;
        unsigned destroyed_before = payload_destroys;
        REQUIRE(xr_execution_resource_adopt(&lease, 32u, (XrStableId) {{1u, 2u, 3u, 4u}},
                                             payload, destroy_owned_payload, &resource) == XR_EXECUTION_OUT_OF_MEMORY);
        REQUIRE(resource == NULL && attempt == failure && live_count == 2u);
        REQUIRE(payload_destroys == destroyed_before && *payload == 42);
        REQUIRE(xr_execution_lease_is_valid(&lease));
        REQUIRE(xr_execution_instance_lease_count(instance) == 1u);
        fail_at = 0u;
        REQUIRE(xr_execution_resource_adopt(&lease, 32u, (XrStableId) {{1u, 2u, 3u, 4u}},
                                             payload, destroy_owned_payload, &resource) == XR_EXECUTION_OK);
        REQUIRE(resource != NULL && live_count == 3u);
        REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_lease_release(&lease));
        xr_execution_resource_free(&resource);
        REQUIRE(resource == NULL && payload_destroys == destroyed_before + 1u && live_count == 2u);
        REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_GENERATION_REJECTED);
        for (size_t index = 0u; index < XR_COUNTOF(pins); ++index)
            REQUIRE(xr_execution_resource_pin_release(&pins[index]));
        REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(instance == NULL && live_count == 0u);
    }
}

int main(void) {
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    XrCoreIrTypeInput resource_type = {
        .local_id = 32u, .kind = XR_CORE_IR_TYPE_PROVIDER_RESOURCE,
        .key.bytes = {1u}, .resource_id.bytes = {1u, 2u, 3u, 4u},
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_FORBIDDEN,
    };
    fixture.input.types = &resource_type;
    fixture.input.type_count = 1u;
    XrCoreIrProgram *core = NULL;
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_core_ir_program_build(&fixture.input, &core, NULL, 0u) == XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_write(core, &artifact, NULL, 0u) == XR_PROGRAM_BUILD_OK);
    xr_core_ir_program_free(core);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrExecutionBindingInput input = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program, .profile = profile, .generation = 1u,
    };
    XrInstance *instance = NULL;
    XrExecutionDiagnostic diagnostic;
    fail_next_allocation();
    REQUIRE(xr_execution_instance_create(&input, &instance, &diagnostic) == XR_EXECUTION_OUT_OF_MEMORY);
    REQUIRE(!instance && attempt == 1u && live_count == 0u);
    REQUIRE(diagnostic.kind == XR_EXECUTION_DIAGNOSTIC_OUT_OF_MEMORY);
    fail_at = 0u;
    REQUIRE(xr_execution_instance_create(&input, &instance, NULL) == XR_EXECUTION_OK);
    REQUIRE(live_count == 1u);
    XrExecutionLease lease = {0};
    fail_next_allocation();
    REQUIRE(xr_execution_instance_acquire(instance, &lease) == XR_EXECUTION_OUT_OF_MEMORY);
    REQUIRE(!lease.instance && !lease.ticket && attempt == 1u && live_count == 1u);
    REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
    fail_at = 0u;
    REQUIRE(xr_execution_instance_acquire(instance, &lease) == XR_EXECUTION_OK);
    REQUIRE(live_count == 2u);
    XrExecutionResourcePin pins[16] = {0};
    for (size_t index = 0u; index < 7u; ++index)
        REQUIRE(xr_execution_resource_pin_acquire(&lease, &pins[index]));
    fail_next_allocation();
    REQUIRE(!xr_execution_resource_pin_acquire(&lease, &pins[7]));
    REQUIRE(!pins[7].instance && !pins[7].ticket && attempt == 1u && live_count == 2u);
    REQUIRE(xr_execution_lease_is_valid(&lease));
    REQUIRE(xr_execution_instance_lease_count(instance) == 1u);
    fail_at = 0u;
    for (size_t index = 7u; index < 15u; ++index)
        REQUIRE(xr_execution_resource_pin_acquire(&lease, &pins[index]));
    StateProbe state = {.pin = pins[0]};
    static const char layout = 0;
    void *observed = NULL;
    REQUIRE(xr_execution_lease_bind_state(&lease, &layout, &state, destroy_state, &observed) ==
            XR_EXECUTION_STATE_ADOPTED);
    REQUIRE(observed == &state);
    REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
    fail_next_allocation();
    REQUIRE(!xr_execution_resource_pin_acquire(&lease, &pins[15]));
    REQUIRE(!pins[15].instance && !pins[15].ticket && attempt == 1u && live_count == 2u);
    REQUIRE(state.destroyed == 0u && xr_execution_lease_is_valid(&lease));
    fail_at = 0u;
    REQUIRE(xr_execution_resource_pin_acquire(&lease, &pins[15]));
    REQUIRE(xr_execution_lease_release(&lease));
    REQUIRE(state.destroyed == 1u && xr_execution_instance_lease_count(instance) == 0u);
    REQUIRE(!xr_execution_resource_pin_release(&pins[0]));
    REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_GENERATION_REJECTED);
    for (size_t index = 1u; index < XR_COUNTOF(pins); ++index) {
        REQUIRE(xr_execution_resource_pin_release(&pins[index]));
        if (index + 1u < XR_COUNTOF(pins))
            REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_GENERATION_REJECTED);
    }
    REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
    REQUIRE(instance == NULL && live_count == 0u);
    test_payload_allocation_failures(&input);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    puts("execution allocation failures: instance, tickets, active/draining growth and payload adoption passed");
    return 0;
}
