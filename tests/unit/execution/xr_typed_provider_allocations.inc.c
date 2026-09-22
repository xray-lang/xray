/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_provider_allocations.inc.c - Resource adoption and VM publication OOM
 */
#include "xr_typed_provider_allocation_probe.h"

static void check_typed_allocation_failures(XrValidatedProgram *program, XrTargetProfile *profile) {
    unsigned after_host_failures = 0u;
    for (size_t failure = 1u; failure < 128u; ++failure) {
        HostState host = {0};
        XrInstance *instance = typed_instance(program, profile, &host);
        XrVmCode *code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, NULL, &code, NULL) == XR_VM_CODE_OK);
        typed_attempt = 0u;
        typed_fail_at = failure;
        typed_failed = false;
        typed_armed = true;
        XrVmOutcome outcome = xr_vm_code_execute(code, instance,
            xr_validated_program_entry_function(program), NULL, 0u);
        typed_armed = false;
        if (typed_failed) {
            if (outcome.kind != XR_VM_OUTCOME_RESOURCE_LIMIT)
                fprintf(stderr, "typed allocation %zu returned outcome %d (host allocations %u)\n",
                        failure, outcome.kind, host.made);
            REQUIRE(outcome.kind == XR_VM_OUTCOME_RESOURCE_LIMIT);
            after_host_failures += host.made != 0u;
        } else {
            REQUIRE(outcome.kind == XR_VM_OUTCOME_RETURN && outcome.owns_dynamic_values);
        }
        xr_vm_outcome_dispose(&outcome);
        REQUIRE(host.made == host.freed);
        xr_vm_code_free(code);
        REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(typed_live_count == 0u);
        if (!typed_failed) {
            REQUIRE(typed_attempt + 1u == failure && after_host_failures >= 3u);
            printf("Typed resource execution allocation points: %zu; after-host failures: %u\n",
                   typed_attempt, after_host_failures);
            return;
        }
    }
    abort();
}
