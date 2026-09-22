/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_typed_provider_native.c - Generated resource call and physical cleanup
 */
#include "../execution/xr_typed_provider_fixture.h"
#include "execution/xr_native_execution.h"
#define REQUIRE(c) do { if (!(c)) { fputs("native typed provider assertion failed\n", stderr); abort(); } } while (0)
#include "../execution/xr_typed_provider_host_fixture.h"

extern const XrBackendNativeDescriptor xr_aot_entry_coroutine_descriptor;

#ifdef XR_NATIVE_RESOURCE_ALLOCATION_TEST
#include "../execution/xr_typed_provider_allocation_probe.h"

static void check_native_allocation_failures(XrValidatedProgram *program, XrTargetProfile *profile) {
    unsigned after_host = 0u;
    for (size_t failure = 1u; failure < 64u; ++failure) {
        HostState host = {0};
        XrInstance *instance = typed_instance(program, profile, &host);
        typed_attempt = 0u; typed_fail_at = failure; typed_failed = false; typed_armed = true;
        XrBackendExecution *execution = NULL;
        bool created = xr_backend_execution_create(instance, &xr_aot_entry_coroutine_descriptor, &execution);
        if (created) {
            /* Fill the initial ticket allocation so resource adoption must grow
             * it while the native frame and existing pins remain live. */
            typed_armed = false;
            XrExecutionLease observer = {0};
            XrExecutionResourcePin pins[6] = {0};
            REQUIRE(xr_execution_instance_acquire(instance, &observer) == XR_EXECUTION_OK);
            for (unsigned i = 0u; i < 6u; ++i)
                REQUIRE(xr_execution_resource_pin_acquire(&observer, &pins[i]));
            typed_armed = true;
            XrBackendExecutionOutcome outcome = xr_backend_execution_step(execution);
            typed_armed = false;
            for (unsigned i = 0u; i < 6u; ++i)
                REQUIRE(xr_execution_resource_pin_release(&pins[i]));
            REQUIRE(xr_execution_lease_release(&observer));
            if (typed_failed) {
                (void)failure;
                (void)outcome;
                REQUIRE(outcome.kind == XR_BACKEND_EXECUTION_TRAP && outcome.safepoint_id == 4u);
                after_host += host.made != 0u;
            } else REQUIRE(outcome.kind == XR_BACKEND_EXECUTION_RETURN && outcome.value == 42);
        } else REQUIRE(typed_failed && !execution);
        typed_armed = false;
        xr_backend_execution_free(execution);
        REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
        REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(host.made == host.freed);
        REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(typed_live_count == 0u);
        if (!typed_failed) {
            REQUIRE(typed_attempt + 1u == failure && after_host == 2u);
            printf("Native resource allocation points: %zu; after-host: %u\n", typed_attempt, after_host);
            return;
        }
    }
    abort();
}
#endif

int main(void) {
    XrValidatedProgram *program = typed_program_build(XR_TYPED_PROVIDER_MODULE ? 5u : 4u);
    XrTargetProfile *profile = typed_profile_build(0u);
    REQUIRE(program && profile);
#ifdef XR_NATIVE_RESOURCE_ALLOCATION_TEST
    check_native_allocation_failures(program, profile);
#endif
    HostState hosts[6] = {0};
    XrInstance *instances[6] = {0};
    for (unsigned mode = 0u; mode <= 5u; ++mode) {
        hosts[mode].mode = mode;
        instances[mode] = typed_instance(program, profile, &hosts[mode]);
    }
    XrBackendNativeDescriptor old = xr_aot_entry_coroutine_descriptor;
    old.schema_version = 8u;
    XrBackendExecution *execution = NULL;
    REQUIRE(!xr_backend_execution_create(instances[0], &old, &execution) && !execution);
    REQUIRE(hosts[0].calls == 0u);
    xr_validated_program_free(program);
    xr_target_profile_free(profile);
    for (unsigned run = 0u; run < 2u; ++run) {
        for (unsigned mode = 0u; mode <= 5u; ++mode) {
            HostState *host = &hosts[mode];
            REQUIRE(xr_backend_execution_create(instances[mode], &xr_aot_entry_coroutine_descriptor, &execution));
            XrBackendExecutionOutcome result = xr_backend_execution_step(execution);
            if (mode == 0u)
                REQUIRE(result.kind == XR_BACKEND_EXECUTION_RETURN && result.value == 42);
            else if (mode == 5u)
                REQUIRE(result.kind == XR_BACKEND_EXECUTION_TRAP && result.safepoint_id == 4u);
            else
                REQUIRE(result.kind == XR_BACKEND_EXECUTION_TRAP && result.safepoint_id == 7u);
            REQUIRE(host->calls == (XR_TYPED_PROVIDER_MODULE ? 1u : run + 1u));
            REQUIRE(host->made == host->calls);
            REQUIRE(host->freed == (XR_TYPED_PROVIDER_MODULE && (mode == 0u || mode == 4u) ? 0u : host->made));
            REQUIRE(host->reads == (mode == 0u || mode == 4u ? run + 1u : 0u));
            xr_backend_execution_free(execution);
            execution = NULL;
        }
    }
    for (unsigned mode = 0u; mode <= 5u; ++mode) {
        REQUIRE(xr_execution_instance_begin_drain(instances[mode], NULL) == XR_EXECUTION_OK);
        REQUIRE(hosts[mode].made == hosts[mode].freed);
        REQUIRE(xr_execution_instance_retire(instances[mode], NULL) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_free(&instances[mode], NULL) == XR_EXECUTION_OK && !instances[mode]);
    }
    puts("native typed resource: value42, instance isolation, refusal cleanup and module release passed");
    return 0;
}
