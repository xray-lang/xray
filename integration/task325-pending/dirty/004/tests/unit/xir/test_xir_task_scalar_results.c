/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_scalar_results.c - Typed sticky Task results across providers
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Task scalar FAIL %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir_task_scalar_pipeline.h"
#include "task_scalar_generated.h"

enum { SCALAR_FUNCTIONS = 128 };
typedef struct ScalarFixture {
    XrXirProgram *program;
    XrXirArtifact *lowered;
    XrXirCallEntry entries[SCALAR_FUNCTIONS];
    XrXirVmBinding bindings[SCALAR_FUNCTIONS];
    uint32_t entry[TASK_SCALAR_CASES][2];
    unsigned code_releases;
} ScalarFixture;
static void scalar_code_drop(void *owner) {
    ScalarFixture *fixture = owner;
    CHECK(fixture->lowered && !fixture->code_releases++);
    xr_xir_compile_artifact_free(fixture->lowered); fixture->lowered = NULL;
}
static void scalar_build(ScalarFixture *fixture, const char *backend) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    fixture->lowered = task_scalar_lower(context, fixture->entry, false);
    const XrXirModule *module = xr_xir_compile_artifact_module(fixture->lowered);
    const XrXirProgramSpec *native = &task_scalar_native_program;
    CHECK(module->function_count <= SCALAR_FUNCTIONS && module->function_count == native->entry_count);
    XrXirProgramProof proof = xr_xir_compile_program_proof(fixture->lowered);
    CHECK(proof.length == native->proof.length && !memcmp(proof.bytes, native->proof.bytes, proof.length));
    bool vm = !strcmp(backend, "vm"), all_native = !strcmp(backend, "native");
    bool root_native = !strcmp(backend, "native-vm");
    CHECK(vm || all_native || root_native || !strcmp(backend, "vm-native"));
    for (uint32_t f = 0; f < module->function_count; ++f) {
        bool worker = module->functions[f].name_length >= 7 && !memcmp(module->functions[f].name, "worker_", 7);
        bool use_native = all_native || (!vm && (worker != root_native));
        XrXirCallEntry entry = {0};
        CHECK(xr_xir_compile_vm_bind(fixture->lowered, f, &fixture->bindings[f], &entry) == XR_XIR_OK);
        fixture->entries[f] = use_native ? native->entries[f] : entry;
    }
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, native->target, fixture->entries, module->function_count,
        module->declarations, {fixture, scalar_code_drop}, module->types, proof};
    CHECK(xr_xir_compile_program_seal(context, &spec, &fixture->program) == XR_XIR_OK);
    for (unsigned n = 0; n < TASK_SCALAR_CASES; ++n) for (unsigned mode = 0; mode < 2; ++mode)
        CHECK(fixture->entry[n][mode] == task_scalar_entries[n][mode]);
}
static void scalar_value(const XrXirValue *value, unsigned family) {
    const TaskScalarOracle *oracle = &task_scalar_oracles[family];
    CHECK(value->type == (uint32_t)oracle->type && !value->reserved && xr_xir_value_valid(value));
    if (oracle->type == XR_XIR_STRING) {
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(value, &bytes, &length) && length == 5 && !memcmp(bytes, "owned", 5));
    } else CHECK(value->payload == oracle->payload);
}
static bool scalar_operation(ScalarFixture *fixture, unsigned family, bool consume_program,
    const XrXirInstanceConfig *limits, XrXirDomainBudgetStats *cost) {
    XrXirInstance *instances[2] = {0}; XrXirDomain *domains[2] = {0}; XrXirValue tasks[2] = {{0}};
    bool completed = true;
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        if (limits) config = *limits;
        XrXirCallStatus status = xr_xir_instance_new(fixture->program, &config, &instances[i]);
        if (status != XR_XIR_CALL_READY) { CHECK(status == (limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM)); completed = false; }
        else { domains[i] = instances[i]->domain; CHECK(xr_xir_domain_retain(domains[i])); }
    }
    if (consume_program) { xr_xir_compile_program_drop(fixture->program); fixture->program = NULL; }
    for (unsigned i = 0; i < 2; ++i) if (instances[i]) {
        for (unsigned mode = 0; mode < 2; ++mode) {
            XrXirCallStatus status = xr_xir_instance_start(instances[i], fixture->entry[family][mode], NULL, 0);
            if (status == XR_XIR_CALL_READY) {
                XrXirInstanceResult result = {0}; unsigned polls = 0;
                do { result = xr_xir_instance_poll_bounded(instances[i], 1); CHECK(++polls < 4096); }
                while (result.outcome.status == XR_XIR_CALL_READY);
                if (result.outcome.status == XR_XIR_CALL_RETURNED) {
                    XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(instances[i], &value) == XR_XIR_CALL_RETURNED);
                    if (mode) tasks[i] = value;
                    else { scalar_value(&value, family); xr_xir_value_drop(&value); }
                } else { CHECK(result.outcome.status == (limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM)); completed = false; break; }
            } else { CHECK(status == (limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM)); completed = false; break; }
        }
        XrXirCallStatus freed = xr_xir_instance_free(instances[i]);
        CHECK(freed == XR_XIR_CALL_READY || freed == (limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM));
        if (freed != XR_XIR_CALL_READY) completed = false;
    }
    if (consume_program) CHECK(fixture->code_releases == 1 && !fixture->lowered);
    for (unsigned i = 0; i < 2; ++i) if (tasks[i].type) {
        const XirTask *task = (const XirTask *)(uintptr_t)tasks[i].payload;
        CHECK(xr_xir_task_storage_valid(task) && task->state == XIR_TASK_TERMINAL && !task->executor && !task->call);
        XrXirCallResult first = {0}, second = {0};
        XrXirCallStatus status = xr_xir_task_copy_outcome(&tasks[i], &first);
        if (status == XR_XIR_CALL_RETURNED) {
            XrXirCallStatus copied = xr_xir_task_copy_outcome(&tasks[i], &second);
            scalar_value(&first.value, family);
            if (copied == XR_XIR_CALL_RETURNED) scalar_value(&second.value, family);
            else { CHECK(limits && copied == XR_XIR_CALL_LIMIT && xr_xir_call_result_empty(&second)); completed = false; }
            CHECK(!first.wake && !second.wake && xr_xir_panic_empty(&first.panic) && xr_xir_panic_empty(&second.panic));
            xr_xir_call_result_drop(&first); xr_xir_call_result_drop(&second);
        } else { CHECK(limits && status == XR_XIR_CALL_LIMIT && xr_xir_call_result_empty(&first)); completed = false; }
        CHECK(task->outcome.status == XR_XIR_CALL_RETURNED);
        scalar_value(&task->outcome.value, family);
        xr_xir_value_drop(&tasks[i]);
    }
    for (unsigned i = 0; i < 2; ++i) if (domains[i]) {
        XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(domains[i]);
        CHECK(!spent.call_live && !spent.metadata_live && spent.call_allocations == spent.call_frees &&
            spent.metadata_allocations == spent.metadata_frees && xr_xir_domain_stats(domains[i]).live_bytes == sizeof(*domains[i]));
        if (cost) { if (!i) *cost = spent; else CHECK(spent.work == cost->work &&
            spent.requested_bytes == cost->requested_bytes && spent.requested_call_bytes == cost->requested_call_bytes); }
        xr_xir_domain_drop(domains[i]);
    }
    CHECK(!runtime_live && !runtime_bytes); return completed;
}
static void scalar_resource_axes(ScalarFixture *fixture, unsigned family) {
    XrXirDomainBudgetStats cost = {0}; CHECK(scalar_operation(fixture, family, false, NULL, &cost));
    const uint64_t exact[] = {cost.requested_bytes, cost.requested_call_bytes, cost.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        CHECK(exact[axis]);
        if (!axis) config.requested_value_limit = exact[axis] - minus;
        else if (axis == 1) config.requested_call_limit = exact[axis] - minus;
        else config.work_limit = exact[axis] - minus;
        CHECK(scalar_operation(fixture, family, false, &config, NULL) == !minus);
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 4); unsigned family = TASK_SCALAR_CASES;
    for (unsigned i = 0; i < TASK_SCALAR_CASES; ++i) if (!strcmp(argv[1], task_scalar_oracles[i].name)) family = i;
    CHECK(family < TASK_SCALAR_CASES); bool fi = !strcmp(argv[3], "fi"); CHECK(fi || !strcmp(argv[3], "normal"));
    ScalarFixture fixture = {0}; scalar_build(&fixture, argv[2]); scalar_resource_axes(&fixture, family);
    runtime_attempts = 0; runtime_fail_at = SIZE_MAX;
    CHECK(scalar_operation(&fixture, family, !fi, NULL, NULL)); size_t sites = runtime_attempts;
    if (fi) for (size_t site = 0; site < sites; ++site) {
        runtime_attempts = 0; runtime_fail_at = site;
        CHECK(!scalar_operation(&fixture, family, false, NULL, NULL) && runtime_attempts > site);
    }
    runtime_fail_at = SIZE_MAX; if (fi) xr_xir_compile_program_drop(fixture.program);
    CHECK(fixture.code_releases == 1 && !fixture.lowered && !runtime_live && !runtime_bytes);
    effects_source_owners_free(); CHECK(!effects_compile_live && !effects_compile_bytes);
    printf("TASK_SCALAR %s %s %s sites=%zu typedBits stickyCopies2 twoInstances postInstance program=%s codeReleases=1 physical=0/0\n",
        argv[1], argv[2], argv[3], sites, fi ? "guard" : "lastLease"); return 0;
}
