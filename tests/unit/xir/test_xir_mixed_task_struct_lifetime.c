/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_struct_lifetime.c - Real Struct outcomes outlive Tasks and code
 */
#define XR_TASK_RESULT_RESOURCE_MAIN
#include "test_xir_mixed_task_result_lifetime.c"
#include "xir/xxir_struct.h"
#include "xir/xxir_atomic.h"

typedef struct TaskStructGraph {
    TaskLateFixture fixture;
    XrXirInstance *instances[2];
    XrXirDomain *domains[2];
    TaskLateWitness witness[2];
    XrXirValue tasks[4], strings[4];
    XrXirCallResult outcomes[8];
    uintptr_t task_addresses[4], atomic_addresses[4];
} TaskStructGraph;

static XrXirValue task_struct_field(const XrXirValue *value, unsigned field) {
    XrXirValue result = {0};
    const XirObject *object = (const XirObject *)(uintptr_t)value->payload;
    XrXirValueAdmission admission = {object->arena, object->domain, NULL, NULL, 10000, 65536};
    XrXirValueStatus status = xr_xir_struct_get(value, field, &admission, &result);
    if (status != XR_XIR_VALUE_OK) fprintf(stderr, "Struct late field=%u status=%u\n", field, status);
    CHECK(status == XR_XIR_VALUE_OK);
    return result;
}
static void task_struct_atomic(const XrXirValue *value, bool add, int64_t expected) {
    const XirObject *object = (const XirObject *)(uintptr_t)value->payload;
    XrXirValue operand = {XR_XIR_I64, 0, 7}, result = {0};
    XrXirValueAdmission admission = {object->arena, object->domain, NULL, NULL, 10000, 65536};
    XrXirAtomicRequest request = {value, add ? &operand : NULL, NULL, add ? 1u : 0u,
        add ? XR_XIR_ATOMIC_OPERATION_FETCH_ADD : XR_XIR_ATOMIC_OPERATION_LOAD, XR_XIR_I64};
    XrXirAtomicProgress progress = {0};
    XrXirAtomicOutcome outcome = xr_xir_atomic_start(&request, &admission, NULL, &progress, &result);
    CHECK(outcome.status == XR_XIR_RUN_OK && outcome.permission == XR_XIR_CALL_READY && !outcome.continuing);
    CHECK(result.type == XR_XIR_I64 && !result.reserved && result.payload == expected);
    xr_xir_atomic_progress_clear(&progress); xr_xir_value_drop(&result);
}
static void task_struct_round(TaskStructGraph *graph, unsigned round) {
    bool done[2] = {0}, started[2] = {true, round != 0}; unsigned polls = 0;
    CHECK(xr_xir_instance_start(graph->instances[0], p20_task_result_entries[0], NULL, 0) == XR_XIR_CALL_READY);
    if (round) CHECK(xr_xir_instance_start(graph->instances[1], p20_task_result_entries[0], NULL, 0) == XR_XIR_CALL_READY);
    while (!done[0] || !done[1]) for (unsigned i = 0; i < 2; ++i) {
        if (done[i]) continue;
        if (!started[i]) {
            if (!done[0]) continue;
            CHECK(!round && i == 1);
            task_late_observation(&graph->fixture, graph->instances, i, &graph->witness[i], "cold");
            CHECK(xr_xir_instance_start(graph->instances[i], p20_task_result_entries[0], NULL, 0) == XR_XIR_CALL_READY);
            started[i] = true;
        }
        XrXirInstanceResult event = task_late_tick(&graph->fixture, graph->instances, i, &graph->witness[i]);
        CHECK(++polls < 4096);
        if (event.outcome.status != XR_XIR_CALL_RETURNED) continue;
        XrXirValue occupied = {XR_XIR_I64, 0, 811}, saved = occupied;
        CHECK(xr_xir_instance_take_result(graph->instances[i], &occupied) == XR_XIR_CALL_BAD_ARGUMENT &&
            !memcmp(&occupied, &saved, sizeof(saved)));
        unsigned at = round*2+i;
        CHECK(xr_xir_instance_take_result(graph->instances[i], &graph->tasks[at]) == XR_XIR_CALL_RETURNED);
        graph->task_addresses[at] = (uintptr_t)graph->tasks[at].payload;
        XrXirValue empty = {0};
        CHECK(xr_xir_instance_take_result(graph->instances[i], &empty) == XR_XIR_CALL_BAD_STATE);
        CHECK(!empty.type && !empty.reserved && !empty.payload); done[i] = true;
    }
    for (unsigned i = 0; i < 2; ++i)
        task_late_observation(&graph->fixture, graph->instances, i, &graph->witness[i], "changed");
}
static void task_struct_copy(TaskStructGraph *graph, unsigned at) {
    const XrXirValue *value = &graph->tasks[at];
    const XirTask *task = (const XirTask *)(uintptr_t)value->payload;
    CHECK(xr_xir_task_storage_valid(task) && task->state == XIR_TASK_TERMINAL && !task->call && !task->executor);
    CHECK(task->object.domain == graph->domains[at%2]);
    const XrXirTypeArena *arena = xr_xir_value_arena(value);
    task_late_metadata_only(&graph->fixture, arena);
    const XrXirTypes *types = xr_xir_compile_type_arena_types(arena);
    const XrXirTypeNode *node = xr_xir_type_node(types, (XrXirType)value->type);
    CHECK(node && node->kind == XR_XIR_TYPE_TASK && !node->parameter_span);
    CHECK(xr_xir_type_is_struct(types, node->element));
    const XrXirType result_type = node->element;
    node = xr_xir_type_node(types, node->element);
    CHECK(node && !node->parameter_span && node->nominal.field_count == 2);
    XrXirCallResult occupied = {.status=XR_XIR_CALL_RETURNED,.value={XR_XIR_I64,0,917}}, saved;
    memcpy(&saved, &occupied, sizeof(saved));
    XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(task->object.domain);
    size_t attempts = runtime_attempts, live = runtime_live;
    CHECK(xr_xir_task_copy_outcome(value, &occupied) == XR_XIR_CALL_BAD_ARGUMENT &&
        !memcmp(&saved, &occupied, sizeof(saved)));
    XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(task->object.domain);
    CHECK(before.work == after.work && before.requested_bytes == after.requested_bytes &&
        before.requested_call_bytes == after.requested_call_bytes && attempts == runtime_attempts && live == runtime_live);
    for (unsigned copy = 0; copy < 2; ++copy) {
        XrXirCallResult *out = &graph->outcomes[at*2+copy];
        CHECK(xr_xir_task_copy_outcome(value, out) == XR_XIR_CALL_RETURNED && !out->wake && xr_xir_panic_empty(&out->panic));
        CHECK(out->value.type == (uint32_t)result_type && xr_xir_value_valid(&out->value));
    }
}
static void task_struct_dead_tasks(TaskStructGraph *graph) {
    size_t attempts = runtime_attempts;
    for (unsigned i = 0; i < 4; ++i) {
        xr_xir_value_drop(&graph->tasks[i]);
        CHECK(!graph->tasks[i].type && !graph->tasks[i].reserved && !graph->tasks[i].payload);
    }
    CHECK(attempts == runtime_attempts);
    for (size_t slot = 0; slot < runtime_owned_capacity; ++slot) if (runtime_owned[slot].pointer) {
        uintptr_t base = (uintptr_t)runtime_owned[slot].pointer;
        for (unsigned i = 0; i < 4; ++i)
            CHECK(graph->task_addresses[i] < base || graph->task_addresses[i]-base >= runtime_owned[slot].bytes);
    }
}
static void task_struct_late_pair(TaskStructGraph *graph, unsigned at) {
    XrXirValue *first = &graph->outcomes[at*2].value, *second = &graph->outcomes[at*2+1].value;
    const XrXirTypeArena *arena = xr_xir_value_arena(first);
    task_late_metadata_only(&graph->fixture, arena);
    XrXirValue array = task_struct_field(first, 0), other = task_struct_field(second, 0);
    task_late_array(&array, "cedar"); task_late_array(&other, "cedar"); xr_xir_value_drop(&other);
    XrXirValue atomic = task_struct_field(first, 1), alias = task_struct_field(second, 1);
    CHECK(atomic.type == alias.type && atomic.payload == alias.payload);
    const XirObject *cell = (const XirObject *)(uintptr_t)atomic.payload;
    CHECK(cell->domain == graph->domains[at%2]); graph->atomic_addresses[at] = (uintptr_t)atomic.payload;
    task_struct_atomic(&atomic, false, 42); task_struct_atomic(&atomic, true, 42);
    task_struct_atomic(&atomic, false, 49); task_struct_atomic(&alias, false, 49);
    xr_xir_value_drop(&atomic); xr_xir_value_drop(&alias);
    XrXirDomain *receiver = NULL; CHECK(xr_xir_domain_new(65536, &receiver) == XR_XIR_VALUE_OK);
    XrXirValue replacement = {0}; CHECK(xr_xir_string_new(receiver, "elm", 3, &replacement) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena, receiver, NULL, NULL, 10000, 65536};
    XrXirValuePlace element = {(XrXirType)array.type, &array.payload}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_set(&element, 0, &replacement, &admission, &fault) == XR_XIR_VALUE_OK);
    uintptr_t old_record = (uintptr_t)first->payload;
    XrXirValuePlace field = {(XrXirType)first->type, &first->payload};
    CHECK(xr_xir_struct_set(&field, 0, &array, &admission) == XR_XIR_VALUE_OK);
    CHECK(first->payload != second->payload && (uintptr_t)second->payload == old_record);
    xr_xir_value_drop(&array); xr_xir_value_drop(&replacement); xr_xir_domain_drop(receiver);
    array = task_struct_field(first, 0); other = task_struct_field(second, 0);
    task_late_array(&array, "elm"); task_late_array(&other, "cedar");
    CHECK(xr_xir_array_get(&other, 1, &admission, &graph->strings[at], &fault) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&array); xr_xir_value_drop(&other);
    atomic = task_struct_field(first, 1); alias = task_struct_field(second, 1);
    CHECK((uintptr_t)atomic.payload == graph->atomic_addresses[at] && alias.payload == atomic.payload);
    task_struct_atomic(&atomic, false, 49); task_struct_atomic(&alias, false, 49);
    xr_xir_value_drop(&atomic); xr_xir_value_drop(&alias); task_late_metadata_only(&graph->fixture, arena);
}
static void task_struct_graph(unsigned backend, unsigned order) {
    TaskStructGraph graph = {0}; XrXirProgram *program = task_late_program(&graph.fixture, backend);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_new(program, &config, &graph.instances[i]) == XR_XIR_CALL_READY);
        graph.domains[i] = graph.instances[i]->domain;
    }
    CHECK(graph.domains[0] != graph.domains[1]); xr_xir_compile_program_drop(program); CHECK(!graph.fixture.releases);
    for (unsigned i = 0; i < 2; ++i)
        task_late_observation(&graph.fixture, graph.instances, i, &graph.witness[i], "cold");
    for (unsigned round = 0; round < 2; ++round) task_struct_round(&graph, round);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(graph.witness[i].root_providers == (task_late_native(backend, true) ? 2u : 1u));
        CHECK(graph.witness[i].worker_providers == (task_late_native(backend, false) ? 2u : 1u));
        CHECK(graph.witness[i].yielded_count == 2 && graph.witness[i].yielded[0] != graph.witness[i].yielded[1]);
    }
    CHECK(xr_xir_instance_free(graph.instances[order]) == XR_XIR_CALL_READY && !graph.fixture.releases);
    CHECK(xr_xir_instance_free(graph.instances[1-order]) == XR_XIR_CALL_READY && graph.fixture.releases == 1);
    CHECK(!graph.fixture.vm && !graph.fixture.entries);
    XrCompileResourceStats cost = {0}; CHECK(xr_compile_resources_stats(graph.fixture.resources, &cost) == XR_COMPILE_RESOURCE_OK);
    for (unsigned i = 0; i < 4; ++i) task_struct_copy(&graph, i);
    task_struct_dead_tasks(&graph);
    for (unsigned i = 0; i < 4; ++i) task_struct_late_pair(&graph, i);
    for (unsigned i = 0; i < 4; ++i) for (unsigned j = 0; j < i; ++j)
        CHECK(graph.atomic_addresses[i] != graph.atomic_addresses[j]);
    for (unsigned i = 0; i < 8; ++i) {
        xr_xir_call_result_drop(&graph.outcomes[i]); CHECK(xr_xir_call_result_empty(&graph.outcomes[i]));
    }
    CHECK(!effects_compile_live && !effects_compile_bytes);
    for (unsigned i = 0; i < 4; ++i) { task_late_text(&graph.strings[i], "birch"); xr_xir_value_drop(&graph.strings[i]); }
    CHECK(!runtime_live && !runtime_bytes);
    printf("P20 Struct Task backend=%u order=%u tasks4 stickyStructCopies8 allTaskSpansDead lateStructArrayCOW4 sharedAtomic42to49 distinctAtoms4 metadataDeadBeforeStrings4 rootMask=%u workerMask=%u yielded2/2 codeLease1 compilerAllocated=%llu peak=%llu work=%llu physical0/0\n",
        backend, order, graph.witness[0].root_providers, graph.witness[0].worker_providers,
        (unsigned long long)cost.allocated_bytes, (unsigned long long)cost.peak_bytes, (unsigned long long)cost.work);
}
#ifndef XR_TASK_STRUCT_CONTROL_MAIN
int main(int argc, char **argv) {
    CHECK(argc == 2); (void)task_late_graph;
    static const char *const names[] = {"0", "1", "vm", "native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], names[backend])) ++backend;
    CHECK(backend < 4);
    for (unsigned order = 0; order < 2; ++order) task_struct_graph(backend, order);
    return 0;
}
#endif
