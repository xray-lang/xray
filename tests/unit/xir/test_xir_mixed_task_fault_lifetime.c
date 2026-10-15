/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_fault_lifetime.c - Real thrown and panicked Task outcomes outlive code
 */
#define XR_TASK_RESULT_RESOURCE_MAIN
#include "test_xir_mixed_task_result_lifetime.c"

static void task_fault_capture(XrXirInstance *instance, XrXirValue tasks[2], unsigned *count) {
    XrXirCall *call = instance->call; XrXirTaskWaitToken token = {0};
    if (!call || !call->wait_linked || !call->waiting_task.type || !xr_xir_call_task_wait_token(call, &token)) return;
    const XirTask *task = (const XirTask *)(uintptr_t)call->waiting_task.payload;
    CHECK(xr_xir_task_storage_valid(task) && task->object.domain == instance->domain);
    CHECK(token.binding.owner == instance->executor && token.binding.activation == &instance->executor->root);
    CHECK(token.binding.generation == instance->executor->generation && token.binding.ticket == instance->executor->root.ticket);
    CHECK(token.request.kind == XR_XIR_WAIT_TASK && token.request.subject == task);
    for (unsigned i = 0; i < *count; ++i) if (tasks[i].payload == call->waiting_task.payload) return;
    if (task->state == XIR_TASK_TERMINAL || !task->call->next_wake) return;
    CHECK(*count < 2 && task->call->next_wake == 1 && task->executor == instance->executor);
    CHECK(xr_xir_value_copy(&call->waiting_task, &tasks[*count]) == XR_XIR_VALUE_OK); ++*count;
    XrXirCallResult empty = {0};
    CHECK(xr_xir_task_copy_outcome(&call->waiting_task, &empty) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&empty));
}
static void task_fault_run(const TaskLateFixture *fixture, XrXirInstance **instances, unsigned index,
    TaskLateWitness *witness, XrXirValue tasks[2]) {
    CHECK(xr_xir_instance_start(instances[index], p20_task_result_entries[0], NULL, 0) == XR_XIR_CALL_READY);
    unsigned polls = 0, captured = 0; XrXirInstanceResult event;
    do {
        event = task_late_tick(fixture, instances, index, witness); CHECK(++polls < 4096);
        task_fault_capture(instances[index], tasks, &captured);
    } while (event.outcome.status == XR_XIR_CALL_READY);
    CHECK(event.outcome.status == XR_XIR_CALL_RETURNED && captured == 2 && witness->yielded_count == 2);
    XrXirValue returned = {0}; CHECK(xr_xir_instance_take_result(instances[index], &returned) == XR_XIR_CALL_RETURNED);
    CHECK(returned.type == tasks[1].type && returned.payload == tasks[1].payload); xr_xir_value_drop(&returned);
    XrXirValue empty = {0};
    CHECK(xr_xir_instance_take_result(instances[index], &empty) == XR_XIR_CALL_BAD_STATE && !empty.type && !empty.payload);
    CHECK(witness->root_providers == (task_late_native(fixture->backend, true) ? 2u : 1u));
    CHECK(witness->worker_providers == (task_late_native(fixture->backend, false) ? 2u : 1u));
}
static void task_fault_text(const XrXirCallResult *outcome, unsigned kind, XrXirDomain *domain) {
    CHECK(!outcome->wake);
    if (kind) {
        CHECK(outcome->status == XR_XIR_CALL_ASSERTION && !outcome->value.type && !outcome->value.payload);
        CHECK(outcome->panic.detail.code == 445); task_late_text(&outcome->panic.message, "task-panic");
        return;
    }
    CHECK(outcome->status == XR_XIR_CALL_THROWN && xr_xir_panic_empty(&outcome->panic));
    const XrXirTypeArena *arena = xr_xir_value_arena(&outcome->value);
    const XrXirTypes *types = xr_xir_compile_type_arena_types(arena);
    const XrXirTypeNode *node = xr_xir_type_node(types, (XrXirType)outcome->value.type);
    CHECK(node && node->kind == XR_XIR_TYPE_NOMINAL);
    CHECK(types->nominals && types->nominals->identities && node->nominal.declaration < types->nominals->count);
    const XrXirNominalIdentity *nominal = &types->nominals->identities[node->nominal.declaration];
    CHECK(nominal->kind == XR_XIR_NOMINAL_ENUM && nominal->name.length == 9 &&
        !memcmp(nominal->name.bytes, "TaskFault", 9));
    uint32_t variant = UINT32_MAX; CHECK(xr_xir_enum_variant(&outcome->value, &variant) == XR_XIR_VALUE_OK && !variant);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 10000, 65536}; XrXirValue message = {0};
    CHECK(xr_xir_enum_get(&outcome->value, 0, 0, &admission, &message) == XR_XIR_VALUE_OK);
    task_late_text(&message, "task-throw"); xr_xir_value_drop(&message);
}
static void task_fault_layout(const XrXirProgram *program) {
    const XrXirDeclarations *d = program->declarations;
    static const char *const names[] = {"module-id-v1:kind=6:script:namespace=0::path=7:root.xr",
        "module-id-v1:kind=6:script:namespace=0::path=9:worker.xr", "memory-module-v1:id=23:xray-core-assertions-v1"};
    CHECK(d->module_count == 3 && !d->root_module && d->slot_count == 1 && !d->slots[0].module);
    for (unsigned i = 0; i < 3; ++i)
        CHECK(d->modules[i].name_length == strlen(names[i]) && !memcmp(d->modules[i].name, names[i], strlen(names[i])));
}
static void task_fault_tasks_drop(XrXirValue tasks[2][2]) {
    uintptr_t addresses[4]; size_t attempts = runtime_attempts;
    for (unsigned i = 0; i < 2; ++i) for (unsigned kind = 0; kind < 2; ++kind)
        addresses[i*2+kind] = (uintptr_t)tasks[i][kind].payload;
    for (unsigned i = 0; i < 2; ++i) for (unsigned kind = 0; kind < 2; ++kind) {
        xr_xir_value_drop(&tasks[i][kind]); CHECK(!tasks[i][kind].type && !tasks[i][kind].payload);
    }
    CHECK(runtime_attempts == attempts);
    for (size_t slot = 0; slot < runtime_owned_capacity; ++slot) if (runtime_owned[slot].pointer) {
        uintptr_t base = (uintptr_t)runtime_owned[slot].pointer;
        for (unsigned i = 0; i < 4; ++i)
            CHECK(addresses[i] < base || addresses[i]-base >= runtime_owned[slot].bytes);
    }
}
static void task_fault_graph(unsigned backend, unsigned order) {
    TaskLateFixture fixture = {0}; XrXirProgram *program = task_late_program(&fixture, backend);
    task_fault_layout(program);
    XrXirInstance *instances[2] = {0}; XrXirValue tasks[2][2] = {{{0}}}; TaskLateWitness witness[2] = {{0}};
    XrXirDomain *domains[2] = {0};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i) domains[i] = instances[i]->domain;
    CHECK(instances[0]->domain != instances[1]->domain);
    xr_xir_compile_program_drop(program); CHECK(!fixture.releases);
    for (unsigned i = 0; i < 2; ++i) task_late_observation(&fixture, instances, i, &witness[i], "cold");
    task_fault_run(&fixture, instances, 0, &witness[0], tasks[0]);
    task_late_observation(&fixture, instances, 0, &witness[0], "thrown:task-panic:inner:outer");
    task_late_observation(&fixture, instances, 1, &witness[1], "cold");
    task_fault_run(&fixture, instances, 1, &witness[1], tasks[1]);
    task_late_observation(&fixture, instances, 1, &witness[1], "thrown:task-panic:inner:outer");
    CHECK(xr_xir_instance_free(instances[order]) == XR_XIR_CALL_READY && !fixture.releases);
    CHECK(xr_xir_instance_free(instances[1-order]) == XR_XIR_CALL_READY && fixture.releases == 1);
    CHECK(!fixture.vm && !fixture.entries);
    XrXirCallResult copies[2][2][2] = {{{{0}}}}; XrXirValue infos[2] = {{0}};
    for (unsigned i = 0; i < 2; ++i) for (unsigned kind = 0; kind < 2; ++kind) {
        const XirTask *task = (const XirTask *)(uintptr_t)tasks[i][kind].payload;
        CHECK(xr_xir_task_storage_valid(task) && task->state == XIR_TASK_TERMINAL && !task->call && !task->executor);
        CHECK(task->object.domain == domains[i]);
        task_late_metadata_only(&fixture, xr_xir_value_arena(&tasks[i][kind]));
        for (unsigned n = 0; n < 2; ++n) {
            CHECK(xr_xir_task_copy_outcome(&tasks[i][kind], &copies[i][kind][n]) == (kind ? XR_XIR_CALL_ASSERTION : XR_XIR_CALL_THROWN));
            task_fault_text(&copies[i][kind][n], kind, domains[i]);
        }
        if (kind) CHECK(xr_xir_panic_info_new(task->object.domain, &copies[i][kind][0].panic, &infos[i]) == XR_XIR_VALUE_OK);
    }
    task_fault_tasks_drop(tasks);
    for (unsigned i = 0; i < 2; ++i) for (unsigned kind = 0; kind < 2; ++kind) for (unsigned n = 0; n < 2; ++n) {
        task_fault_text(&copies[i][kind][n], kind, domains[i]); xr_xir_call_result_drop(&copies[i][kind][n]);
        CHECK(xr_xir_call_result_empty(&copies[i][kind][n]));
    }
    CHECK(!effects_compile_live && !effects_compile_bytes);
    for (unsigned i = 0; i < 2; ++i) {
        XrXirFaultDetail detail = {0}; XrXirValue message = {0};
        CHECK(xr_xir_panic_info_detail(&infos[i], &detail) && detail.code == 445);
        CHECK(xr_xir_panic_info_message(&infos[i], &message) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&infos[i]); task_late_text(&message, "task-panic"); xr_xir_value_drop(&message);
    }
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    printf("Task fault lifetime backend=%u order=%u instances2 genuineYieldedTasks4 thrown2 assertion2 lateCopies8 panicInfos2 innerOuter peerCold allTasksPhysicallyDeadBeforeLateReads metadataDeadBeforeFinalMessages codeLease1 physical0/0\n",
        backend, order);
}
int main(int argc, char **argv) {
    CHECK(argc == 2); (void)task_late_graph;
    static const char *const names[] = {"0", "1", "vm", "native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], names[backend])) ++backend;
    CHECK(backend < 4);
    for (unsigned order = 0; order < 2; ++order) task_fault_graph(backend, order);
    return 0;
}
