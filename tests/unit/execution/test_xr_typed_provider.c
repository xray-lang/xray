/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_typed_provider.c - Real typed host resources through canonical VM
 */
#include "xr_typed_provider_fixture.h"
#include "base/xmalloc.h"
#include "vm/xr_program_vm.h"

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); abort(); } } while (0)

#include "xr_typed_provider_host_fixture.h"

static void check_direct_calls(XrInstance *instance, HostState *host) {
    XrExecutionLease lease = {0};
    REQUIRE(xr_execution_instance_acquire(instance, &lease) == XR_EXECUTION_OK);
    XrProviderValuePack arguments = {.count = 2u,
        .nodes = {{.token = XR_PROVIDER_TYPE_I64, .as.i64 = 42},
                  {.token = XR_PROVIDER_TYPE_BOOL, .as.boolean = false}}};
    XrProviderValuePack result = {0};
    REQUIRE(xr_execution_lease_provider_call_typed(&lease, 0u, 0u, &arguments, &result) == XR_EXECUTION_PROVIDER_CALL_OK);
    REQUIRE(result.count == 1u && result.nodes[0].token == XR_PROVIDER_TYPE_OPTIONAL && result.nodes[0].child_count == 0u);
    REQUIRE(host->made == 0u);
    unsigned calls = host->calls;
    REQUIRE(xr_execution_lease_provider_call_typed(&lease, 0u, 0u, &arguments, &result) == XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE);
    REQUIRE(host->calls == calls);
    xr_execution_provider_result_dispose(&result);
    arguments.nodes[0].token = XR_PROVIDER_TYPE_BOOL;
    REQUIRE(xr_execution_lease_provider_call_typed(&lease, 0u, 0u, &arguments, &result) == XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE);
    REQUIRE(host->calls == calls && result.count == 0u);
    arguments.nodes[0].token = XR_PROVIDER_TYPE_I64;
    arguments.nodes[1].as.boolean = true;
    REQUIRE(xr_execution_lease_provider_call_typed(&lease, 0u, 0u, &arguments, &result) == XR_EXECUTION_PROVIDER_CALL_OK);
    REQUIRE(result.count == 2u && result.nodes[1].as.resource.owner != NULL);
    REQUIRE(result.nodes[1].as.resource.payload == NULL && result.nodes[1].as.resource.destroy == NULL);
    XrProviderValuePack borrowed = {.count = 1u, .nodes = {result.nodes[1]}}, answer = {0};
    REQUIRE(xr_execution_lease_provider_call_typed(&lease, 0u, 1u, &borrowed, &answer) == XR_EXECUTION_PROVIDER_CALL_OK);
    REQUIRE(answer.count == 1u && answer.nodes[0].token == XR_PROVIDER_TYPE_I64 && answer.nodes[0].as.i64 == 42);
    xr_execution_provider_result_dispose(&answer);
    host->mode = 4u;
    REQUIRE(xr_execution_lease_provider_call_typed(&lease, 0u, 1u, &borrowed, &answer) == XR_EXECUTION_PROVIDER_CALL_FAILED);
    REQUIRE(answer.count == 0u && host->freed == 0u);
    host->mode = 0u;
    REQUIRE(xr_execution_lease_release(&lease));
    xr_execution_provider_result_dispose(&result);
    xr_execution_provider_result_dispose(&result);
    REQUIRE(host->made == 1u && host->freed == 1u);
}

#ifdef XR_TYPED_PROVIDER_ALLOCATION_TEST
#include "xr_typed_provider_allocations.inc.c"
#endif

static void check_complete_resource_programs(void) {
    for (unsigned scenario = 4u; scenario <= 5u; ++scenario) {
        XrValidatedProgram *program = typed_program_build(scenario);
        XrTargetProfile *profile = typed_profile_build(0u);
        REQUIRE(program && profile);
        XrVmCode *code = NULL;
        XrVmCodeOptions options = xr_vm_code_default_options();
        REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
        for (unsigned mode = 0u; mode <= 5u; ++mode) {
            HostState host = {.mode = mode};
            XrInstance *instance = typed_instance(program, profile, &host);
            for (unsigned run = 0u; run < 2u; ++run) {
                XrVmOutcome result = xr_vm_code_execute(code, instance, 0u, NULL, 0u);
                if (mode == 0u)
                    REQUIRE(result.kind == XR_VM_OUTCOME_RETURN && result.value.kind == XR_VM_VALUE_I64 && result.value.as.i64 == 42);
                else if (mode == 5u)
                    REQUIRE(result.kind == XR_VM_OUTCOME_RESOURCE_LIMIT);
                else
                    REQUIRE(result.kind == XR_VM_OUTCOME_TRAP && result.trap == XR_VM_TRAP_PROVIDER_CALL_FAILED);
                xr_vm_outcome_dispose(&result);
                REQUIRE(host.calls == (scenario == 5u ? 1u : run + 1u));
                REQUIRE(host.made == host.calls);
                REQUIRE(host.freed == (scenario == 5u && (mode == 0u || mode == 4u) ? 0u : host.made));
                REQUIRE(host.reads == (mode == 0u || mode == 4u ? run + 1u : 0u));
            }
            REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(host.freed == host.made);
            REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
        }
        xr_vm_code_free(code);
        xr_validated_program_free(program);
        xr_target_profile_free(profile);
    }
}

int main(void) {
    check_complete_resource_programs();
    XrTargetProfile *profile = typed_profile_build(0u);
    REQUIRE(profile != NULL);
    XrValidatedProgram *program = typed_program_build(0u);
    REQUIRE(program != NULL);
    for (unsigned mutation = 1u; mutation <= 3u; ++mutation) {
        XrTargetProfile *wrong_abi = typed_profile_build(mutation);
        REQUIRE(wrong_abi != NULL);
        HostState uncalled = {0};
        REQUIRE(typed_instance_expect(program, wrong_abi, &uncalled, XR_EXECUTION_PROVIDER_REJECTED) == NULL);
        REQUIRE(uncalled.calls == 0u && uncalled.reads == 0u);
        xr_target_profile_free(wrong_abi);
    }
#ifdef XR_TYPED_PROVIDER_ALLOCATION_TEST
    check_typed_allocation_failures(program, profile);
#endif
    for (unsigned mutation = 1u; mutation <= 3u; ++mutation) {
        XrValidatedProgram *rejected = typed_program_build(mutation);
        REQUIRE(rejected == NULL);
    }
    HostState host = {0}, other = {0};
    XrInstance *instance = typed_instance(program, profile, &host);
    XrInstance *foreign = typed_instance(program, profile, &other);
    check_direct_calls(instance, &host);
    XrVmCode *code = NULL;
    XrVmCodeOptions options = xr_vm_code_default_options();
    REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
    uint32_t entry = xr_validated_program_entry_function(program);
    REQUIRE(entry == 0u && xr_validated_program_function_count(program) == 2u);
    XrVmOutcome made = xr_vm_code_execute(code, instance, entry, NULL, 0u);
    REQUIRE(made.kind == XR_VM_OUTCOME_RETURN && made.owns_dynamic_values);
    XrVmAggregateView view = {0};
    REQUIRE(xr_vm_value_aggregate_view(&made.value, &view));
    REQUIRE(view.type_id == 33u && view.variant_ordinal == 1u && view.field_count == 1u);
    REQUIRE(view.fields[0].kind == XR_VM_VALUE_RESOURCE);
    XrVmOutcome read = xr_vm_code_execute(code, instance, 1u, view.fields, 1u);
    REQUIRE(read.kind == XR_VM_OUTCOME_RETURN && read.value.kind == XR_VM_VALUE_I64 && read.value.as.i64 == 42);
    xr_vm_outcome_dispose(&read);
    read = xr_vm_code_execute(code, foreign, 1u, view.fields, 1u);
    REQUIRE(read.kind == XR_VM_OUTCOME_TRAP && read.trap == XR_VM_TRAP_PROVIDER_CALL_FAILED);
    REQUIRE(other.reads == 0u);
    xr_vm_outcome_dispose(&read);
    REQUIRE(host.made == 2u && host.freed == 1u);
    xr_vm_outcome_dispose(&made);
    REQUIRE(host.freed == 2u);
    for (host.mode = 1u; host.mode <= 3u; ++host.mode) {
        made = xr_vm_code_execute(code, instance, entry, NULL, 0u);
        REQUIRE(made.kind == XR_VM_OUTCOME_TRAP && made.trap == XR_VM_TRAP_PROVIDER_CALL_FAILED);
        xr_vm_outcome_dispose(&made);
        REQUIRE(host.made == host.freed);
    }
    host.mode = 0u;
    XrVmCode *limited = NULL;
    options.max_value_cells = 1u;
    REQUIRE(xr_vm_code_build(program, profile, &options, &limited, NULL) == XR_VM_CODE_OK);
    unsigned before_limit = host.made;
    made = xr_vm_code_execute(limited, instance, entry, NULL, 0u);
    REQUIRE(made.kind == XR_VM_OUTCOME_RESOURCE_LIMIT);
    REQUIRE(host.made == before_limit && host.made == host.freed);
    xr_vm_outcome_dispose(&made);
    xr_vm_code_free(limited);
    made = xr_vm_code_execute(code, instance, entry, NULL, 0u);
    REQUIRE(made.kind == XR_VM_OUTCOME_RETURN && host.made == host.freed + 1u);
    xr_vm_code_free(code);
    xr_validated_program_free(program);
    xr_target_profile_free(profile);
    REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_GENERATION_REJECTED);
    REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_GENERATION_REJECTED && instance != NULL);
    xr_vm_outcome_dispose(&made);
    REQUIRE(host.made == host.freed);
    REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK && instance == NULL);
    REQUIRE(xr_execution_instance_begin_drain(foreign, NULL) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_retire(foreign, NULL) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&foreign, NULL) == XR_EXECUTION_OK && foreign == NULL);
    puts("typed provider: optional resources, VM borrow, isolation, refusal cleanup and physical release passed");
    return 0;
}
