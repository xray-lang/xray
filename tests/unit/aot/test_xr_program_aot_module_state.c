/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_program_aot_module_state.c - Native typed instance state and physical reclamation
 */

#include "execution/xr_native_execution.h"
#include "program/xr_program_verify.h"
#include "../plan/target_profile_test_fixture.h"
#include "../program/xr_program_module_fixture.h"

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

typedef struct StateTrace {
    char output[512];
    size_t size;
    uint32_t live, allocations, frees;
    uint32_t events, dropped, finalized, reclaimed;
    uint64_t reclaimed_ids[8];
    uint32_t attempted, fail_at, constructed, total_finalized, total_reclaimed;
} StateTrace;

static StateTrace *current_trace;
#if XR_MODULE_STATE_SCENARIO != 0 && XR_MODULE_STATE_SCENARIO != 2
static struct {
    void *pointer;
    StateTrace *owner;
} allocations[128];

static void *native_malloc(size_t size) {
    REQUIRE(current_trace);
    if (++current_trace->attempted == current_trace->fail_at)
        return NULL;
    void *pointer = malloc(size);
    REQUIRE(pointer && current_trace);
    for (size_t i = 0u; i < XR_COUNTOF(allocations); ++i) {
        if (allocations[i].pointer)
            continue;
        allocations[i].pointer = pointer;
        allocations[i].owner = current_trace;
        ++current_trace->live;
        ++current_trace->allocations;
        return pointer;
    }
    abort();
}

static void native_free(void *pointer) {
    if (!pointer)
        return;
    for (size_t i = 0u; i < XR_COUNTOF(allocations); ++i) {
        if (allocations[i].pointer != pointer)
            continue;
        --allocations[i].owner->live;
        ++allocations[i].owner->frees;
        allocations[i].pointer = NULL;
        free(pointer);
        return;
    }
    abort();
}

#define malloc native_malloc
#define free native_free
#endif
#define main xr_module_standalone_main
#include XR_MODULE_STATE_GENERATED_C
#undef main
#if XR_MODULE_STATE_SCENARIO != 0 && XR_MODULE_STATE_SCENARIO != 2
#undef malloc
#undef free
#endif

#if XR_MODULE_STATE_SCENARIO >= 5
static void observe_lifecycle(void *opaque, const XrAotLifecycleEvent *event) {
    StateTrace *trace = opaque;
    REQUIRE(trace && event);
    ++trace->events;
    if (event->kind == 1u)
        ++trace->constructed;
    if (event->kind == 8u)
        ++trace->total_finalized;
    if (event->kind == 9u)
        ++trace->total_reclaimed;
    if (event->origin != 4u)
        return;
    REQUIRE(event->kind == 7u || event->kind == 8u || event->kind == 9u);
    if (event->kind == 7u)
        ++trace->dropped;
    if (event->kind == 8u)
        ++trace->finalized;
    if (event->kind == 9u) {
        REQUIRE(trace->reclaimed < XR_COUNTOF(trace->reclaimed_ids));
        trace->reclaimed_ids[trace->reclaimed++] = event->identity;
    }
}
#endif

static void observe_frame(void *opaque, void *state, const XrBackendNativeHost *host) {
    xr_aot_entry_coroutine_descriptor.initialize(opaque, state, host);
#if XR_MODULE_STATE_SCENARIO >= 5
    XrAotEntryCoroutineFrame *frame = opaque;
    frame->context.lifecycle_context = current_trace;
    frame->context.lifecycle_event = observe_lifecycle;
    XrAotModules *modules = state;
    modules->storage.lifecycle_context = current_trace;
    modules->storage.lifecycle_event = observe_lifecycle;
#endif
}

static XrProviderCallStatus capture_output(void *opaque, const uint8_t *bytes, size_t size) {
    StateTrace *trace = opaque;
    REQUIRE(trace && bytes && size <= sizeof(trace->output) - trace->size);
    memcpy(trace->output + trace->size, bytes, size);
    trace->size += size;
    return XR_PROVIDER_CALL_OK;
}

static XrInstance *create_instance(XrValidatedProgram *program, XrTargetProfile *profile,
                                   StateTrace *trace, bool output) {
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program,
        .profile = profile,
        .generation = 1u,
    };
    XrProviderOperationBinding operation = {0};
    XrProviderBinding provider = {0};
    if (output) {
        const XrTargetProviderContract *contract = NULL;
        for (size_t i = 0u; i < xr_target_profile_provider_count(profile); ++i) {
            const XrTargetProviderContract *candidate = xr_target_profile_provider(profile, i);
            if (xr_test_target_profile_is_provider(candidate, XR_PROVIDER_IO_CONTRACT_KEY))
                contract = candidate;
        }
        REQUIRE(contract && contract->operation_count == 1u);
        operation.operation_id = contract->operations[0].stable_id;
        operation.trampoline_kind = XR_PROVIDER_TRAMPOLINE_OUTPUT_WRITE;
        operation.entry.output_write = capture_output;
        operation.context = trace;
        provider.contract_id = contract->contract_id;
        REQUIRE(xr_target_provider_contract_fingerprint(contract, &provider.contract_fingerprint) ==
                XR_RUNTIME_ABI_OK);
        provider.behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;
        provider.operations = &operation;
        provider.operation_count = 1u;
        binding.providers = &provider;
        binding.provider_count = 1u;
    }
    XrInstance *instance = NULL;
    REQUIRE(xr_execution_instance_create(&binding, &instance, NULL) == XR_EXECUTION_OK);
    return instance;
}

int main(void) {
    uint32_t scenario = XR_MODULE_STATE_SCENARIO;
    bool output = scenario == 1u || scenario == 3u || scenario == 4u;
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_module_native_fixture_write(scenario, &artifact, NULL, 0u) ==
            XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile =
        output ? xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED)
               : xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    XrBackendNativeDescriptor descriptor = xr_aot_entry_coroutine_descriptor;
    descriptor.initialize = observe_frame;
    for (uint32_t mode = 0u; mode < (scenario == 7u ? 3u : 1u); ++mode) {
        StateTrace traces[2] = {0};
        XrInstance *instances[2] = {0};
        XrExecutionLease held[2] = {0};
        const void *errors[2] = {0};
        bool error = scenario == 9u || scenario == 11u;
        bool message = scenario == 13u;
        bool panic = scenario == 10u || scenario == 12u || message;
        uint32_t trap = scenario == 2u                                                     ? 8u
                        : scenario == 3u                                                   ? 9u
                        : scenario == 4u || scenario == 6u || mode != 0u ? 4u
                                                                                           : 0u;
        uint32_t objects = scenario == 5u || scenario == 8u ? 4u : scenario >= 6u ? 3u : 0u;
        for (uint32_t repeat = 0u; repeat < 24u; ++repeat) {
            for (uint32_t index = 0u; index < 2u; ++index) {
                current_trace = &traces[index];
                StateTrace *trace = current_trace;
                if (repeat == 0u) {
                    instances[index] = create_instance(program, profile, trace, output);
                    REQUIRE(xr_execution_instance_acquire(instances[index], &held[index]) == XR_EXECUTION_OK);
                }
                XrBackendExecution *execution = NULL;
                if (repeat == 0u) {
                    for (uint32_t version = 4u; version <= 6u; ++version) {
                        XrBackendNativeDescriptor stale = descriptor;
                        stale.schema_version = version;
                        REQUIRE(!xr_backend_execution_create(instances[index], &stale, &execution));
                        REQUIRE(execution == NULL);
                    }
                }
                REQUIRE(xr_backend_execution_create(instances[index], &descriptor, &execution));
                size_t before = trace->size;
                XrBackendExecutionOutcome result = xr_backend_execution_step(execution);
                if ((scenario == 11u || scenario == 12u) && repeat == 0u) {
                    REQUIRE(result.kind == XR_BACKEND_EXECUTION_SUSPENDED);
                    REQUIRE(trace->live == 0u && trace->events == 0u);
                    result = xr_backend_execution_step(execution);
                }
                if (scenario == 7u && repeat == 0u) {
                    REQUIRE(result.kind == XR_BACKEND_EXECUTION_SUSPENDED);
                    REQUIRE(trace->live == 6u && trace->events == 15u);
                    if (mode == 0u)
                        result = xr_backend_execution_step(execution);
                    else if (mode == 1u) {
                        result = xr_backend_execution_cancel(execution);
                        REQUIRE(result.kind == XR_BACKEND_EXECUTION_CANCELLED);
                    } else {
                        xr_backend_execution_free(execution);
                        execution = NULL;
                    }
                    if (mode != 0u) {
                        REQUIRE(trace->live == 0u && trace->reclaimed == 3u);
                        xr_backend_execution_free(execution);
                        execution = NULL;
                        REQUIRE(
                            xr_backend_execution_create(instances[index], &descriptor, &execution));
                        result = xr_backend_execution_step(execution);
                    }
                }
                REQUIRE(result.kind ==
                        (error ? XR_BACKEND_EXECUTION_ERROR : panic ? XR_BACKEND_EXECUTION_PANIC
                         : trap ? XR_BACKEND_EXECUTION_TRAP : XR_BACKEND_EXECUTION_RETURN));
                REQUIRE(result.safepoint_id == trap);
                REQUIRE(result.panic_present == (panic ? 1u : 0u));
                if (result.panic_present)
                    REQUIRE(result.panic_code == 1u);
                REQUIRE(result.panic_has_message == (message ? 1u : 0u));
                if (message) {
                    REQUIRE(result.panic_message && result.panic_message_size == 1u);
                    REQUIRE(result.panic_message[0] == 'x');
                    if (repeat == 0u) errors[index] = result.panic_message;
                    REQUIRE(errors[index] == result.panic_message);
                }
#if XR_MODULE_STATE_SCENARIO == 9 || XR_MODULE_STATE_SCENARIO == 11
                REQUIRE(result.error_type_id == 32u && result.error_value);
                if (repeat == 0u)
                    errors[index] = result.error_value;
                REQUIRE(result.error_value == errors[index]);
                XrAotType32 cause = *(const XrAotType32 *)result.error_value;
                REQUIRE(cause && cause->identity == 3u && cause->owners == 1u);
                REQUIRE(cause->f0 && cause->f0->size == 1u && cause->f0->bytes[0] == 'x');
                REQUIRE(trace->finalized == 2u && trace->reclaimed == 2u);
#else
                (void)errors;
                REQUIRE(result.error_type_id == 0u && result.error_value == NULL);
#endif
                if (scenario == 0u)
                    REQUIRE(result.value == (int64_t) (41u + repeat));
                if (scenario == 1u) {
                    const char *expected = repeat ? "changed\n" : "module-owned-text\n";
                    REQUIRE(trace->size - before == strlen(expected));
                    REQUIRE(memcmp(trace->output + before, expected, strlen(expected)) == 0);
                }
                xr_backend_execution_free(execution);
                REQUIRE(trace->live == (error ? 2u : message ? 1u : trap || panic ? 0u
                                                     : scenario == 1u ? 1u : 2u * objects));
                if (scenario >= 5u)
                    REQUIRE(trace->events == (error ? 21u : (trap || panic ? 8u : 5u) * objects));
            }
        }
        for (uint32_t index = 0u; index < 2u; ++index) {
            StateTrace *trace = &traces[index];
            current_trace = trace;
            XrBackendExecution *observer = NULL;
            if (error || message) {
                REQUIRE(errors[0] != errors[1]);
                REQUIRE(xr_backend_execution_create(instances[index], &descriptor, &observer));
                XrBackendExecutionOutcome observed = xr_backend_execution_step(observer);
                REQUIRE(observed.kind == (message ? XR_BACKEND_EXECUTION_PANIC : XR_BACKEND_EXECUTION_ERROR));
                REQUIRE((message ? (const void *)observed.panic_message : observed.error_value) == errors[index]);
            }
            REQUIRE(xr_execution_instance_begin_drain(instances[index], NULL) == XR_EXECUTION_OK);
            REQUIRE(trace->live == (error ? 2u : message ? 1u : trap || panic ? 0u
                                                 : scenario == 1u ? 1u : 2u * objects));
            REQUIRE(xr_execution_instance_retire(instances[index], NULL) ==
                    XR_EXECUTION_GENERATION_REJECTED);
            REQUIRE(xr_execution_lease_release(&held[index]));
            if (observer) {
                REQUIRE(xr_execution_instance_lease_count(instances[index]) == 1u);
                REQUIRE(xr_execution_instance_retire(instances[index], NULL) ==
                        XR_EXECUTION_GENERATION_REJECTED);
#if XR_MODULE_STATE_SCENARIO == 9 || XR_MODULE_STATE_SCENARIO == 11
                XrAotType32 cause = *(const XrAotType32 *)errors[index];
                REQUIRE(cause->identity == 3u && cause->owners == 1u);
                REQUIRE(cause->f0->size == 1u && cause->f0->bytes[0] == 'x');
                REQUIRE(trace->live == 2u && trace->reclaimed == 2u);
#endif
                if (message) {
                    REQUIRE(((const uint8_t *)errors[index])[0] == 'x');
                    REQUIRE(trace->live == 1u && trace->reclaimed == 3u);
                }
                xr_backend_execution_free(observer);
            }
            REQUIRE(trace->live == 0u && trace->allocations == trace->frees);
            REQUIRE(trace->dropped == objects + (scenario == 9u || scenario == 11u ? 1u : 0u) &&
                    trace->finalized == objects && trace->reclaimed == objects);
            for (uint32_t object = 0u; object < objects; ++object) {
                uint64_t expected = error ? (object == 2u ? 3u : 2u - object) : objects - object;
                REQUIRE(trace->reclaimed_ids[object] == expected);
            }
            REQUIRE(xr_execution_instance_retire(instances[index], NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_free(&instances[index], NULL) == XR_EXECUTION_OK);
        }
    }
    if (scenario >= 5u) {
        uint32_t attempts = scenario == 13u ? 11u : scenario == 8u ? 16u : scenario == 5u ? 12u : 9u;
        for (uint32_t failure = 1u; failure <= attempts; ++failure) {
            StateTrace trace = {.fail_at = failure};
            current_trace = &trace;
            XrInstance *instance = create_instance(program, profile, &trace, output);
            XrBackendExecution *execution = NULL;
            REQUIRE(xr_backend_execution_create(instance, &descriptor, &execution));
            XrBackendExecutionOutcome result = xr_backend_execution_step(execution);
            if (scenario == 11u || scenario == 12u) {
                REQUIRE(result.kind == XR_BACKEND_EXECUTION_SUSPENDED);
                REQUIRE(trace.live == 0u && trace.events == 0u);
                result = xr_backend_execution_step(execution);
            }
            REQUIRE(result.kind == XR_BACKEND_EXECUTION_TRAP && result.safepoint_id == 4u);
            REQUIRE(result.panic_present == 0u);
            REQUIRE(trace.live == 0u);
            xr_backend_execution_free(execution);
            REQUIRE(trace.attempted == failure && trace.live == 0u);
            REQUIRE(trace.allocations == trace.frees);
            REQUIRE(trace.constructed == (scenario == 8u ? (failure + 1u) / 4u : failure / 3u));
            REQUIRE(trace.constructed == trace.total_finalized);
            REQUIRE(trace.constructed == trace.total_reclaimed);
            REQUIRE(xr_backend_execution_create(instance, &descriptor, &execution));
            result = xr_backend_execution_step(execution);
            REQUIRE(result.kind == XR_BACKEND_EXECUTION_TRAP && result.safepoint_id == 4u);
            REQUIRE(result.panic_present == 0u);
            xr_backend_execution_free(execution);
            REQUIRE(trace.attempted == failure && trace.live == 0u);
            REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
        }
    }
    if (scenario == 1u) {
        for (uint32_t failure = 1u; failure <= 4u; ++failure) {
            StateTrace trace = {.fail_at = failure};
            current_trace = &trace;
            XrInstance *instance = create_instance(program, profile, &trace, true);
            XrBackendExecution *execution = NULL;
            REQUIRE(xr_backend_execution_create(instance, &descriptor, &execution));
            XrBackendExecutionOutcome result = xr_backend_execution_step(execution);
            REQUIRE(result.kind == XR_BACKEND_EXECUTION_TRAP && result.safepoint_id == 4u);
            REQUIRE(result.panic_present == 0u);
            REQUIRE(trace.live == (failure == 1u ? 0u : 1u) && trace.size == 0u);
            xr_backend_execution_free(execution);
            REQUIRE(trace.attempted == failure);
            REQUIRE(xr_backend_execution_create(instance, &descriptor, &execution));
            result = xr_backend_execution_step(execution);
            if (failure == 1u) {
                REQUIRE(result.kind == XR_BACKEND_EXECUTION_TRAP && result.safepoint_id == 4u);
                REQUIRE(result.panic_present == 0u);
                REQUIRE(trace.attempted == 1u && trace.live == 0u);
            } else {
                REQUIRE(result.kind == XR_BACKEND_EXECUTION_RETURN);
                const char *expected = failure == 4u ? "changed\n" : "module-owned-text\n";
                REQUIRE(trace.size == strlen(expected));
                REQUIRE(memcmp(trace.output, expected, trace.size) == 0);
                REQUIRE(trace.live == 1u);
            }
            xr_backend_execution_free(execution);
            REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(trace.live == 0u && trace.allocations == trace.frees);
        }
    }
#if XR_MODULE_STATE_SCENARIO == 10 || XR_MODULE_STATE_SCENARIO == 12
    for (uint32_t index = 0u; index < 2u; ++index) {
        StateTrace trace = {0};
        current_trace = &trace;
        XrAotModules modules = {0};
        modules.storage.modules = &modules;
        modules.storage.lifecycle_context = &trace;
        modules.storage.lifecycle_event = observe_lifecycle;
        XrAotContext context = {0};
        context.modules = &modules;
        context.lifecycle_context = &trace;
        context.lifecycle_event = observe_lifecycle;
        XrAotOutcome cause = {0};
        uint32_t attempts = 0u;
        for (uint32_t repeat = 0u; repeat < 3u; ++repeat) {
            cause = xr_aot_initialize_modules(&context);
            REQUIRE(cause.kind == 3u && cause.trap == 0u);
            REQUIRE(cause.panic_present == 1u && cause.panic_info.code == 1u);
            if (repeat == 0u)
                attempts = trace.attempted;
            REQUIRE(trace.attempted == attempts);
        }
        xr_aot_context_destroy(&context);
        xr_aot_modules_clear(&modules);
        REQUIRE(trace.live == 0u && trace.allocations == trace.frees);
        REQUIRE(trace.total_finalized == 3u && trace.total_reclaimed == 3u);
        REQUIRE(cause.panic_present == 1u && cause.panic_info.code == 1u);
    }
#endif
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    printf("native module slots passed scenario=%u\n", scenario);
    return 0;
}
