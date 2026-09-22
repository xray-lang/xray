/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_program_aot_module_instance.c - Native instance initialization authority
 */

#include "execution/xr_native_execution.h"
#include "os/os_thread.h"
#include "program/xr_program_verify.h"
#include "../plan/target_profile_test_fixture.h"
#include "../program/xr_program_module_fixture.h"

#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

extern const XrBackendNativeDescriptor xr_aot_entry_coroutine_descriptor;

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

typedef struct Capture {
    char bytes[32];
    size_t size;
    uint32_t calls;
    uint32_t refuse_at;
    bool refuse;
    XrBackendExecution *waiter;
} Capture;

static atomic_uint state_initializations, state_drops, state_aborts;

static void observe_initialize(void *state) {
    ++state_initializations;
    xr_aot_entry_coroutine_descriptor.state_initialize(state);
}

static void observe_drop(void *state) {
    ++state_drops;
    xr_aot_entry_coroutine_descriptor.state_drop(state);
}

static void observe_abort(void *state) {
    ++state_aborts;
    xr_aot_entry_coroutine_descriptor.initialization_abort(state);
}

static XrProviderCallStatus capture_write(void *opaque, const uint8_t *bytes, size_t size) {
    Capture *capture = opaque;
    REQUIRE(capture && bytes && size <= sizeof(capture->bytes) - capture->size);
    ++capture->calls;
    /* A reentrant entry sees the initializer's lease, without gaining publication authority. */
    if (capture->waiter)
        REQUIRE(xr_backend_execution_step(capture->waiter).kind ==
                XR_BACKEND_EXECUTION_INITIALIZING);
    if (capture->refuse || capture->calls == capture->refuse_at)
        return XR_PROVIDER_CALL_FAILED;
    memcpy(capture->bytes + capture->size, bytes, size);
    capture->size += size;
    return XR_PROVIDER_CALL_OK;
}

typedef struct NativeRace {
    XrInstance *instance;
    const XrBackendNativeDescriptor *descriptor;
    atomic_bool start;
    bool failure;
    uint32_t trap;
} NativeRace;

static void *native_initialization_worker(void *opaque) {
    NativeRace *race = opaque;
    while (!atomic_load_explicit(&race->start, memory_order_acquire))
        xr_thread_yield();
    XrBackendExecution *execution = NULL;
    REQUIRE(xr_backend_execution_create(race->instance, race->descriptor, &execution));
    XrBackendExecutionOutcome result;
    do {
        result = xr_backend_execution_step(execution);
        if (result.kind == XR_BACKEND_EXECUTION_INITIALIZING)
            REQUIRE(xr_backend_execution_cancel(execution).kind == XR_BACKEND_EXECUTION_INVALID);
        xr_thread_yield();
    } while (result.kind == XR_BACKEND_EXECUTION_INITIALIZING ||
             result.kind == XR_BACKEND_EXECUTION_SUSPENDED);
    REQUIRE(result.kind ==
            (race->failure ? XR_BACKEND_EXECUTION_TRAP : XR_BACKEND_EXECUTION_RETURN));
    REQUIRE(result.safepoint_id == race->trap);
    xr_backend_execution_free(execution);
    return NULL;
}

int main(void) {
    bool failure_fixture = XR_NATIVE_MODULE_FAILURE != 0;
    bool suspend_fixture = XR_NATIVE_MODULE_SUSPEND != 0;
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_module_output_fixture_write_with_suspension(
                failure_fixture ? 1u : UINT32_MAX, suspend_fixture ? 1u : UINT32_MAX, &artifact,
                NULL, 0u) == XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    const XrTargetProviderContract *contract = NULL;
    for (size_t i = 0u; i < xr_target_profile_provider_count(profile); ++i) {
        const XrTargetProviderContract *candidate = xr_target_profile_provider(profile, i);
        if (xr_test_target_profile_is_provider(candidate, XR_PROVIDER_IO_CONTRACT_KEY))
            contract = candidate;
    }
    REQUIRE(contract && contract->operation_count == 1u);
    XrBackendNativeDescriptor descriptor = xr_aot_entry_coroutine_descriptor;
    descriptor.state_initialize = observe_initialize;
    descriptor.state_drop = observe_drop;
    descriptor.initialization_abort = observe_abort;
    uint32_t expected_aborts = 0u;
    for (uint32_t mode = 0u; mode < (suspend_fixture ? 8u : 4u); ++mode) {
        Capture capture = {.refuse = mode == 2u || mode == 3u, .refuse_at = mode >= 6u ? 3u : 0u};
        XrProviderOperationBinding operation = {
            .operation_id = contract->operations[0].stable_id,
            .trampoline_kind = XR_PROVIDER_TRAMPOLINE_OUTPUT_WRITE,
            .entry.output_write = capture_write,
            .context = &capture,
        };
        XrProviderBinding provider = {
            .contract_id = contract->contract_id,
            .behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL,
            .operations = &operation,
            .operation_count = 1u,
        };
        REQUIRE(xr_target_provider_contract_fingerprint(contract, &provider.contract_fingerprint) ==
                XR_RUNTIME_ABI_OK);
        XrExecutionBindingInput binding = {
            .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
            .program = program,
            .profile = profile,
            .providers = &provider,
            .provider_count = 1u,
            .generation = 1u,
        };
        XrInstance *instance = NULL;
        REQUIRE(xr_execution_instance_create(&binding, &instance, NULL) == XR_EXECUTION_OK);
        XrBackendExecution *first = NULL;
        REQUIRE(xr_backend_execution_create(instance, &descriptor, &first));
        REQUIRE(xr_backend_execution_create(instance, &descriptor, &capture.waiter));
        REQUIRE(state_initializations == mode + 1u);
        static const uint8_t wrong_layout = 0;
        XrBackendNativeDescriptor incompatible = descriptor;
        incompatible.state_layout = &wrong_layout;
        XrBackendExecution *rejected = NULL;
        REQUIRE(!xr_backend_execution_create(instance, &incompatible, &rejected));
        REQUIRE(!rejected && xr_execution_instance_lease_count(instance) == 2u);
        XrBackendExecutionOutcome result = xr_backend_execution_step(first);
        if (suspend_fixture && !capture.refuse) {
            REQUIRE(result.kind == XR_BACKEND_EXECUTION_SUSPENDED);
            REQUIRE(result.state_id == 1u &&
                    result.suspension.kind == XR_SUSPENSION_REQUEST_COOPERATIVE_YIELD);
            REQUIRE(capture.size == 4u && memcmp(capture.bytes, "1\n2\n", 4u) == 0);
            REQUIRE(xr_backend_execution_step(capture.waiter).kind ==
                    XR_BACKEND_EXECUTION_INITIALIZING);
            if (mode >= 4u) {
                if (mode >= 6u)
                    REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
                if ((mode & 1u) == 0u) {
                    result = xr_backend_execution_cancel(first);
                    REQUIRE(result.kind == (mode >= 6u ? XR_BACKEND_EXECUTION_TRAP
                                                       : XR_BACKEND_EXECUTION_CANCELLED));
                    REQUIRE(result.state_id == 1u && result.safepoint_id == (mode >= 6u ? 7u : 0u));
                    REQUIRE(xr_backend_execution_step(first).kind == XR_BACKEND_EXECUTION_INVALID);
                }
                xr_backend_execution_free(first);
                first = NULL;
                result = xr_backend_execution_step(capture.waiter);
                REQUIRE(result.kind == XR_BACKEND_EXECUTION_TRAP &&
                        result.safepoint_id == (mode >= 6u ? 7u : 4u));
                ++expected_aborts;
                REQUIRE(state_aborts == expected_aborts);
                const char *cancel_expected = mode >= 6u ? "1\n2\n" : "1\n2\n2\n";
                REQUIRE(capture.calls == 3u && capture.size == strlen(cancel_expected));
                REQUIRE(memcmp(capture.bytes, cancel_expected, capture.size) == 0);
                REQUIRE(state_drops == mode);
                if (mode < 6u)
                    REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
                REQUIRE(xr_execution_instance_retire(instance, NULL) ==
                        XR_EXECUTION_GENERATION_REJECTED);
                xr_backend_execution_free(capture.waiter);
                REQUIRE(state_drops == mode + 1u);
                REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
                REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
                continue;
            }
            result = xr_backend_execution_step(first);
        }
        bool failed = failure_fixture || capture.refuse;
        REQUIRE(result.kind == (failed ? XR_BACKEND_EXECUTION_TRAP : XR_BACKEND_EXECUTION_RETURN));
        uint32_t trap = capture.refuse ? 7u : failure_fixture ? 4u : 0u;
        REQUIRE(result.safepoint_id == trap);
        REQUIRE(capture.calls == (capture.refuse ? 1u : failure_fixture ? 2u : 4u));
        const char *expected = capture.refuse ? "" : failure_fixture ? "1\n2\n" : "1\n2\n3\n4\n";
        REQUIRE(capture.size == strlen(expected) &&
                memcmp(capture.bytes, expected, capture.size) == 0);
        for (uint32_t repeat = 0u; repeat < 3u; ++repeat) {
            XrBackendExecution *next = NULL;
            REQUIRE(xr_backend_execution_create(instance, &descriptor, &next));
            XrBackendExecutionOutcome again = xr_backend_execution_step(next);
            REQUIRE(again.kind == result.kind && again.safepoint_id == trap);
            REQUIRE(capture.size == strlen(expected));
            xr_backend_execution_free(next);
        }
        XrBackendExecutionOutcome waiter = xr_backend_execution_step(capture.waiter);
        REQUIRE(waiter.kind == result.kind && waiter.safepoint_id == trap);
        REQUIRE(state_drops == mode);
        expected_aborts += failed ? 1u : 0u;
        REQUIRE(state_aborts == expected_aborts);
        xr_backend_execution_free(first);
        REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(state_drops == mode);
        REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_GENERATION_REJECTED);
        xr_backend_execution_free(capture.waiter);
        REQUIRE(state_drops == mode + 1u);
        REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
    }
    for (uint32_t refusal = 0u; refusal < 2u; ++refusal) {
        Capture capture = {.refuse = refusal != 0u};
        XrProviderOperationBinding operation = {
            .operation_id = contract->operations[0].stable_id,
            .trampoline_kind = XR_PROVIDER_TRAMPOLINE_OUTPUT_WRITE,
            .entry.output_write = capture_write,
            .context = &capture,
        };
        XrProviderBinding provider = {
            .contract_id = contract->contract_id,
            .behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL,
            .operations = &operation,
            .operation_count = 1u,
        };
        REQUIRE(xr_target_provider_contract_fingerprint(contract, &provider.contract_fingerprint) ==
                XR_RUNTIME_ABI_OK);
        XrExecutionBindingInput binding = {
            .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
            .program = program,
            .profile = profile,
            .providers = &provider,
            .provider_count = 1u,
            .generation = 1u,
        };
        NativeRace race = {
            .descriptor = &descriptor,
            .failure = refusal || failure_fixture,
            .trap = refusal           ? 7u
                    : failure_fixture ? 4u
                                      : 0u,
        };
        atomic_init(&race.start, false);
        REQUIRE(xr_execution_instance_create(&binding, &race.instance, NULL) == XR_EXECUTION_OK);
        uint32_t before = state_initializations;
        xr_thread_t threads[4];
        for (uint32_t i = 0u; i < 4u; ++i)
            REQUIRE(xr_thread_create(&threads[i], native_initialization_worker, &race));
        atomic_store_explicit(&race.start, true, memory_order_release);
        for (uint32_t i = 0u; i < 4u; ++i)
            REQUIRE(xr_thread_join(threads[i], NULL) == 0);
        REQUIRE(state_initializations > before);
        REQUIRE(state_initializations == state_drops + 1u);
        REQUIRE(capture.calls == (refusal ? 1u : failure_fixture ? 2u : 4u));
        const char *expected = refusal ? "" : failure_fixture ? "1\n2\n" : "1\n2\n3\n4\n";
        REQUIRE(capture.size == strlen(expected) &&
                memcmp(capture.bytes, expected, capture.size) == 0);
        expected_aborts += race.failure ? 1u : 0u;
        REQUIRE(state_aborts == expected_aborts);
        REQUIRE(xr_execution_instance_begin_drain(race.instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(state_initializations == state_drops);
        REQUIRE(xr_execution_instance_retire(race.instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_free(&race.instance, NULL) == XR_EXECUTION_OK);
    }
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    puts("native module instance initialization passed");
    return 0;
}
