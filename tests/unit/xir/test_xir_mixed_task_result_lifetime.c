/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_result_lifetime.c - Compound outcomes outlive all code owners
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Task late %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
XR_DATA const XrXirProgramSpec p20_task_result_fixture_program;
XR_DATA const uint32_t p20_task_result_entries[3];
#ifndef XR_MIXED_TASK_RESULT_MODULES
#define XR_MIXED_TASK_RESULT_MODULES 2
#endif
typedef struct TaskLateFixture {
    XrCompileResources *resources;
    XrXirProgram *vm;
    XrXirCallEntry *entries;
    uint32_t count;
    bool root[64];
    unsigned backend, releases;
} TaskLateFixture;
#include "xir_mixed_task_result_observe.h"

static void task_late_code_free(void *pointer) {
    TaskLateFixture *fixture = pointer; CHECK(!fixture->releases++ && fixture->vm && fixture->entries);
    xr_xir_compile_program_drop(fixture->vm); fixture->vm = NULL;
    xr_compile_resources_free(fixture->entries); fixture->entries = NULL;
}
static XrXirProgram *task_late_program(TaskLateFixture *fixture, unsigned backend) {
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    XrCompileResourceLimits limits = {UINT64_C(64)*1024*1024, UINT64_C(8)*1024*1024, UINT64_C(128000000)};
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    fixture->resources = context.resources; fixture->backend = backend;
    const XrXirProgramSpec *native = &p20_task_result_fixture_program;
    void *packet = NULL; size_t length = native->proof.length;
    CHECK(xr_compile_resources_alloc(context.resources, length, &packet) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_work(context.resources, length) == XR_COMPILE_RESOURCE_OK);
    memcpy(packet, native->proof.bytes, length);
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet, length, &checked, NULL) == XR_XIR_OK);
    memset(packet, 0xA5, length); xr_compile_resources_free(packet);
    CHECK(xr_xir_compile_lower(checked, &native->target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count == native->entry_count && module->function_count < 64);
    fixture->count = module->function_count;
    CHECK(module->declarations->module_count == XR_MIXED_TASK_RESULT_MODULES && module->declarations->slot_count == 1);
    CHECK(xr_compile_resources_calloc(context.resources, fixture->count, sizeof(*fixture->entries),
        (void **)&fixture->entries) == XR_COMPILE_RESOURCE_OK);
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    CHECK(proof.length == native->proof.length && !memcmp(proof.bytes, native->proof.bytes, proof.length));
    CHECK(xr_xir_compile_vm_program_take(&lowered, &fixture->vm) == XR_XIR_OK && !lowered);
    unsigned vm = 0, c = 0;
    for (uint32_t f = 0; f < fixture->count; ++f) {
        fixture->root[f] = module->declarations->functions[f].module == module->declarations->root_module;
        bool use_native = task_late_native(backend, fixture->root[f]);
        CHECK(native->entries[f].resume != fixture->vm->entries[f].resume);
        fixture->entries[f] = use_native ? native->entries[f] : fixture->vm->entries[f];
        if (use_native) ++c; else ++vm;
    }
    if (backend < 2) CHECK(vm && c);
    else CHECK(backend == 2 ? vm == fixture->count && !c : c == fixture->count && !vm);
    CHECK(fixture->root[p20_task_result_entries[0]] && fixture->root[p20_task_result_entries[1]] &&
        !fixture->root[p20_task_result_entries[2]]);
    XrXirProgramSpec spec = *native;
    spec.entries = fixture->entries; spec.types = module->types; spec.declarations = module->declarations;
    spec.proof = proof; spec.code = (XrXirCodeLease){fixture, task_late_code_free};
    XrXirProgram *program = NULL; CHECK(xr_xir_compile_program_seal(&context, &spec, &program) == XR_XIR_OK);
    /* Only the caller reference dies here. The real arena pins this same finite
     * ledger until the final owned compound result releases its metadata. */
    xr_compile_resources_release(context.resources); return program;
}
static XrXirInstanceResult task_late_tick(const TaskLateFixture *fixture, XrXirInstance **instances,
    unsigned index, TaskLateWitness *witness) {
    XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(instances[1-index]->domain);
    XrXirInstanceResult event = xr_xir_instance_poll_bounded(instances[index], 1);
    CHECK(event.outcome.status == XR_XIR_CALL_READY || event.outcome.status == XR_XIR_CALL_RETURNED);
    task_late_observe(fixture, instances[index], witness);
    XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(instances[1-index]->domain);
    CHECK(before.work == after.work && before.requested_bytes == after.requested_bytes &&
        before.requested_call_bytes == after.requested_call_bytes);
    return event;
}
static void task_late_observation(const TaskLateFixture *fixture, XrXirInstance **instances,
    unsigned index, TaskLateWitness *witness, const char *expected) {
    CHECK(xr_xir_instance_start(instances[index], p20_task_result_entries[1], NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult event = {0}; unsigned polls = 0;
    do { event = task_late_tick(fixture, instances, index, witness); CHECK(++polls < 4096); }
    while (event.outcome.status == XR_XIR_CALL_READY);
    XrXirValue result = {0}; CHECK(xr_xir_instance_take_result(instances[index], &result) == XR_XIR_CALL_RETURNED);
    task_late_text(&result, expected); xr_xir_value_drop(&result);
}
static void task_late_graph(unsigned backend, unsigned order) {
    TaskLateFixture fixture = {0}; XrXirProgram *program = task_late_program(&fixture, backend);
    XrXirInstance *instances[2] = {0}; XrXirValue tasks[4] = {{0}}; TaskLateWitness witness[2] = {{0}};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    CHECK(instances[0]->domain != instances[1]->domain);
    xr_xir_compile_program_drop(program); CHECK(!fixture.releases);
    for (unsigned i = 0; i < 2; ++i) task_late_observation(&fixture, instances, i, &witness[i], "cold");
    for (unsigned round = 0; round < 2; ++round) {
        bool done[2] = {0}, started[2] = {true, round != 0}; unsigned polls = 0;
        CHECK(xr_xir_instance_start(instances[0], p20_task_result_entries[0], NULL, 0) == XR_XIR_CALL_READY);
        if (round) CHECK(xr_xir_instance_start(instances[1], p20_task_result_entries[0], NULL, 0) == XR_XIR_CALL_READY);
        while (!done[0] || !done[1]) for (unsigned i = 0; i < 2; ++i) {
            if (done[i]) continue;
            if (!started[i]) {
                if (!done[0]) continue;
                CHECK(!round && i == 1);
                task_late_observation(&fixture, instances, i, &witness[i], "cold");
                CHECK(xr_xir_instance_start(instances[i], p20_task_result_entries[0], NULL, 0) == XR_XIR_CALL_READY);
                started[i] = true;
            }
            XrXirInstanceResult event = task_late_tick(&fixture, instances, i, &witness[i]); CHECK(++polls < 4096);
            if (event.outcome.status != XR_XIR_CALL_RETURNED) continue;
            XrXirValue occupied = {XR_XIR_I64,0,811}, saved = occupied;
            CHECK(xr_xir_instance_take_result(instances[i], &occupied) == XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&occupied, &saved, sizeof(saved)));
            CHECK(xr_xir_instance_take_result(instances[i], &tasks[round*2+i]) == XR_XIR_CALL_RETURNED);
            XrXirValue empty = {0}; CHECK(xr_xir_instance_take_result(instances[i], &empty) == XR_XIR_CALL_BAD_STATE);
            CHECK(!empty.type && !empty.reserved && !empty.payload); done[i] = true;
        }
        for (unsigned i = 0; i < 2; ++i) task_late_observation(&fixture, instances, i, &witness[i], "changed");
    }
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(witness[i].root_providers == (task_late_native(backend, true) ? 2u : 1u));
        CHECK(witness[i].worker_providers == (task_late_native(backend, false) ? 2u : 1u) && witness[i].yielded_count == 2);
    }
    CHECK(xr_xir_instance_free(instances[order]) == XR_XIR_CALL_READY && !fixture.releases);
    CHECK(xr_xir_instance_free(instances[1-order]) == XR_XIR_CALL_READY && fixture.releases == 1);
    CHECK(!fixture.vm && !fixture.entries);
    XrCompileResourceStats cost = {0};
    CHECK(xr_compile_resources_stats(fixture.resources, &cost) == XR_COMPILE_RESOURCE_OK);
    for (unsigned i = 0; i < 4; ++i) task_late_outcomes(&fixture, &tasks[i]);
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    printf("P20 Task result backend=%u order=%u tasks4 lateCopies8 lateCOW4 peerColdAfterOriginChange metadataOnly2 rootMask=%u workerMask=%u yielded2/2 codeLease1 compilerAllocated=%llu peak=%llu work=%llu physical0/0\n",
        backend, order, witness[0].root_providers, witness[0].worker_providers,
        (unsigned long long)cost.allocated_bytes, (unsigned long long)cost.peak_bytes, (unsigned long long)cost.work);
}
#ifndef XR_TASK_RESULT_RESOURCE_MAIN
int main(int argc, char **argv) {
    CHECK(argc == 2); static const char *const names[] = {"0", "1", "vm", "native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], names[backend])) ++backend;
    CHECK(backend < 4);
    for (unsigned order = 0; order < 2; ++order) task_late_graph(backend, order);
    return 0;
}
#endif
