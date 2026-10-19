/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_struct_control.c - Control every actual Struct Task poll prefix
 */
#define XR_TASK_STRUCT_CONTROL_MAIN
#include "test_xir_mixed_task_struct_lifetime.c"

typedef struct TaskStructControlResult { unsigned polls, phase, captured, terminal, returned, cancelled; } TaskStructControlResult;
typedef struct TaskStructPeer { XrXirTaskWaitToken token; XrXirDomainBudgetStats cost; uintptr_t slot; } TaskStructPeer;

static void task_struct_control_cost(XrXirDomainBudgetStats before, XrXirDomainBudgetStats after) {
    CHECK(before.requested_bytes == after.requested_bytes && before.requested_call_bytes == after.requested_call_bytes && before.work == after.work);
}
static XrXirCallStatus task_struct_control_tick(TaskStructGraph *graph, unsigned index) {
    XrXirDomainBudgetStats peer = xr_xir_domain_budget_stats(graph->domains[1-index]);
    XrXirInstanceResult event = xr_xir_instance_poll_bounded(graph->instances[index], 1);
    XrXirCallStatus status = event.outcome.status;
    CHECK(status == XR_XIR_CALL_READY || status == XR_XIR_CALL_RETURNED || status == XR_XIR_CALL_CANCELLED);
    if (status == XR_XIR_CALL_CANCELLED) CHECK(!event.outcome.value.type && !event.outcome.value.reserved &&
        !event.outcome.value.payload && !event.outcome.wake && xr_xir_panic_empty(&event.outcome.panic));
    task_late_observe(&graph->fixture, graph->instances[index], &graph->witness[index]);
    task_struct_control_cost(peer, xr_xir_domain_budget_stats(graph->domains[1-index])); return status;
}
/* Retain only the value already owned by the stopped Call's linked wait. */
static void task_struct_control_capture(TaskStructGraph *graph, unsigned index) {
    XrXirValue *value = &graph->tasks[2+index]; if (value->type) return;
    XrXirInstance *instance = graph->instances[index]; XrXirCall *call = instance->call;
    XrXirTaskWaitToken token = {0};
    if (!call || !call->wait_linked || !call->waiting_task.type || !xr_xir_call_task_wait_token(call, &token)) return;
    const XirTask *task = (const XirTask *)(uintptr_t)call->waiting_task.payload;
    CHECK(xr_xir_task_storage_valid(task) && task->object.domain == graph->domains[index]);
    CHECK(token.binding.owner == instance->executor && token.binding.activation == &instance->executor->root);
    CHECK(token.binding.generation == instance->executor->generation && token.binding.ticket == instance->executor->root.ticket);
    CHECK(token.request.kind == XR_XIR_WAIT_TASK && token.request.subject == task);
    CHECK(xr_xir_value_copy(&call->waiting_task, value) == XR_XIR_VALUE_OK);
    CHECK(value->payload == call->waiting_task.payload && value->payload != graph->tasks[index].payload);
    graph->task_addresses[2+index] = (uintptr_t)value->payload;
}
static unsigned task_struct_control_phase(const TaskStructGraph *graph, unsigned index) {
    const XrXirValue *value = &graph->tasks[2+index];
    if (!value->type) return graph->instances[index]->executor->active ? 1u : 0u;
    const XirTask *task = (const XirTask *)(uintptr_t)value->payload;
    CHECK(xr_xir_task_storage_valid(task) && task->object.domain == graph->domains[index]);
    if (task->state == XIR_TASK_TERMINAL) {
        CHECK(task->outcome.status == XR_XIR_CALL_RETURNED && !task->call && !task->executor); return 3;
    }
    CHECK(task->executor == graph->instances[index]->executor && task->call && task->activation.call == task->call);
    XrXirCallResult empty = {0}; XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(graph->domains[index]);
    size_t attempts = runtime_attempts;
    CHECK(xr_xir_task_copy_outcome(value, &empty) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&empty));
    task_struct_control_cost(before, xr_xir_domain_budget_stats(graph->domains[index])); CHECK(attempts == runtime_attempts);
    return 2;
}
static void task_struct_control_same_peer(TaskStructGraph *graph, const TaskStructPeer *before) {
    XrXirInstance *instance = graph->instances[1]; XrXirTaskWaitToken after = {0};
    CHECK(xr_xir_call_task_wait_token(instance->call, &after));
    CHECK(after.binding.owner == before->token.binding.owner && after.binding.activation == before->token.binding.activation &&
        after.binding.generation == before->token.binding.generation && after.binding.ticket == before->token.binding.ticket);
    CHECK(after.wake == before->token.wake && after.request.kind == before->token.request.kind &&
        after.request.reserved == before->token.request.reserved && after.request.after_ms == before->token.request.after_ms &&
        after.request.subject == before->token.request.subject && after.request.generation == before->token.request.generation &&
        after.request.ticket == before->token.request.ticket);
    CHECK(xr_xir_value_valid(&instance->slots[0]) && (uintptr_t)instance->slots[0].payload == before->slot);
    const XirCell *cell = (const XirCell *)object_pointer(&instance->slots[0]);
    CHECK(cell && cell->object.kind == XIR_OBJECT_CELL && cell->object.domain == graph->domains[1]);
    CHECK(cell->initialized && cell->module_storage && cell->module_owner == instance && !cell->module_slot);
    task_late_text(&cell->value, "changed"); CHECK(task_struct_control_phase(graph, 1) == 2);
    task_struct_control_cost(before->cost, xr_xir_domain_budget_stats(graph->domains[1]));
}
static void task_struct_control_pause_peer(TaskStructGraph *graph, TaskStructPeer *peer) {
    CHECK(xr_xir_instance_start(graph->instances[1], p20_task_result_entries[0], NULL, 0) == XR_XIR_CALL_READY);
    unsigned polls = 0;
    for (;;) {
        CHECK(task_struct_control_tick(graph, 1) == XR_XIR_CALL_READY && ++polls < 4096);
        task_struct_control_capture(graph, 1);
        if (!graph->tasks[3].type) continue;
        const XirTask *task = (const XirTask *)(uintptr_t)graph->tasks[3].payload;
        CHECK(task->state != XIR_TASK_TERMINAL && task->call);
        if (!task->call->next_wake) continue;
        CHECK(task->call->next_wake == 1 && graph->witness[1].yielded_count == 2); break;
    }
    CHECK(xr_xir_call_task_wait_token(graph->instances[1]->call, &peer->token));
    peer->slot = (uintptr_t)graph->instances[1]->slots[0].payload;
    peer->cost = xr_xir_domain_budget_stats(graph->domains[1]); task_struct_control_same_peer(graph, peer);
}
static void task_struct_control_take(TaskStructGraph *graph, unsigned index) {
    XrXirValue returned = {0};
    CHECK(xr_xir_instance_take_result(graph->instances[index], &returned) == XR_XIR_CALL_RETURNED);
    CHECK(graph->tasks[2+index].type && returned.type == graph->tasks[2+index].type &&
        returned.payload == graph->tasks[2+index].payload); xr_xir_value_drop(&returned);
    CHECK(xr_xir_instance_take_result(graph->instances[index], &returned) == XR_XIR_CALL_BAD_STATE &&
        !returned.type && !returned.reserved && !returned.payload);
}
static void task_struct_control_finish(TaskStructGraph *graph, unsigned index, XrXirCallStatus expected) {
    unsigned polls = 0; XrXirCallStatus status;
    do {
        status = task_struct_control_tick(graph, index); CHECK(++polls < 4096);
    } while (status == XR_XIR_CALL_READY);
    CHECK(status == expected);
    if (expected == XR_XIR_CALL_RETURNED) task_struct_control_take(graph, index);
}
static void task_struct_control_free(TaskStructGraph *graph, unsigned index) {
    CHECK(graph->instances[index] && xr_xir_instance_free(graph->instances[index]) == XR_XIR_CALL_READY);
    graph->instances[index] = NULL;
}
static void task_struct_control_cancelled(TaskStructGraph *graph, unsigned at) {
    const XirTask *task = (const XirTask *)(uintptr_t)graph->tasks[at].payload;
    CHECK(xr_xir_task_storage_valid(task) && task->state == XIR_TASK_TERMINAL && !task->call && !task->executor);
    CHECK(task->object.domain == graph->domains[at%2] && task->outcome.status == XR_XIR_CALL_CANCELLED);
    const XrXirTypes *types = xr_xir_compile_type_arena_types(task->object.arena);
    const XrXirTypeNode *node = xr_xir_type_node(types, (XrXirType)graph->tasks[at].type);
    CHECK(node && node->kind == XR_XIR_TYPE_TASK && !node->parameter_span && xr_xir_type_is_struct(types, node->element));
    XrXirCallResult occupied = {.status=XR_XIR_CALL_RETURNED,.value={XR_XIR_I64,0,193}}, saved;
    memcpy(&saved, &occupied, sizeof(saved));
    XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(task->object.domain); size_t attempts = runtime_attempts;
    CHECK(xr_xir_task_copy_outcome(&graph->tasks[at], &occupied) == XR_XIR_CALL_BAD_ARGUMENT &&
        !memcmp(&saved, &occupied, sizeof(saved)));
    task_struct_control_cost(before, xr_xir_domain_budget_stats(task->object.domain)); CHECK(attempts == runtime_attempts);
    for (unsigned copy = 0; copy < 2; ++copy) {
        XrXirCallResult *out = &graph->outcomes[at*2+copy];
        CHECK(xr_xir_task_copy_outcome(&graph->tasks[at], out) == XR_XIR_CALL_CANCELLED &&
            !out->value.type && !out->value.reserved && !out->value.payload && !out->wake && xr_xir_panic_empty(&out->panic));
    }
}
static bool task_struct_control_physical(uintptr_t address) {
    for (size_t i = 0; i < runtime_owned_capacity; ++i) if (runtime_owned[i].pointer) {
        uintptr_t base = (uintptr_t)runtime_owned[i].pointer;
        if (address >= base && address-base < runtime_owned[i].bytes) return true;
    }
    return false;
}
static void task_struct_control_late(TaskStructGraph *graph, XrXirCallStatus expected, TaskStructControlResult *result) {
    for (unsigned at = 0; at < 4; ++at) if (graph->tasks[at].type) {
        const XirTask *task = (const XirTask *)(uintptr_t)graph->tasks[at].payload;
        CHECK(graph->task_addresses[at] == (uintptr_t)task && task_struct_control_physical(graph->task_addresses[at]));
        CHECK(task->state == XIR_TASK_TERMINAL && task->outcome.status == (at == 2 ? expected : XR_XIR_CALL_RETURNED));
        if (at == 2 && expected == XR_XIR_CALL_CANCELLED) { task_struct_control_cancelled(graph, at); ++result->cancelled; }
        else { task_struct_copy(graph, at); ++result->returned; }
    }
    task_struct_dead_tasks(graph);
    for (unsigned at = 0; at < 4; ++at) if (graph->task_addresses[at]) CHECK(!task_struct_control_physical(graph->task_addresses[at]));
    for (unsigned i = 0; i < 4; ++i) if (graph->outcomes[i*2].status == XR_XIR_CALL_RETURNED) task_struct_late_pair(graph, i);
    for (unsigned i = 0; i < 4; ++i) if (graph->atomic_addresses[i]) for (unsigned j = 0; j < i; ++j)
        if (graph->atomic_addresses[j]) CHECK(graph->atomic_addresses[i] != graph->atomic_addresses[j]);
    for (unsigned i = 0; i < 8; ++i) { xr_xir_call_result_drop(&graph->outcomes[i]); CHECK(xr_xir_call_result_empty(&graph->outcomes[i])); }
    CHECK(!effects_compile_live && !effects_compile_bytes);
    unsigned strings = 0;
    for (unsigned i = 0; i < 4; ++i) if (graph->strings[i].type) {
        task_late_text(&graph->strings[i], "birch"); xr_xir_value_drop(&graph->strings[i]); ++strings;
    }
    CHECK(strings == result->returned && !runtime_live && !runtime_bytes);
}
/* The normal control measures actual public polls. Every intervention is made
 * before the corresponding next poll and fixes its own pre-control expectation. */
static TaskStructControlResult task_struct_control_graph(unsigned backend, unsigned prefix, unsigned mode, unsigned order) {
    TaskStructGraph graph = {0}; TaskStructControlResult result = {0};
    XrXirProgram *program = task_late_program(&graph.fixture, backend);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_new(program, &config, &graph.instances[i]) == XR_XIR_CALL_READY);
        graph.domains[i] = graph.instances[i]->domain;
    }
    CHECK(graph.domains[0] != graph.domains[1]); xr_xir_compile_program_drop(program); CHECK(!graph.fixture.releases);
    for (unsigned i = 0; i < 2; ++i) task_late_observation(&graph.fixture, graph.instances, i, &graph.witness[i], "cold");
    task_struct_round(&graph, 0); TaskStructPeer peer = {0}; task_struct_control_pause_peer(&graph, &peer);
    CHECK(xr_xir_instance_start(graph.instances[0], p20_task_result_entries[0], NULL, 0) == XR_XIR_CALL_READY);
    XrXirCallStatus status = XR_XIR_CALL_READY;
    if (!mode) {
        do {
            status = task_struct_control_tick(&graph, 0); CHECK(++result.polls < 4096);
            task_struct_control_capture(&graph, 0);
        } while (status == XR_XIR_CALL_READY);
        CHECK(status == XR_XIR_CALL_RETURNED); task_struct_control_take(&graph, 0);
    } else for (unsigned step = 0; step < prefix; ++step) {
        CHECK(task_struct_control_tick(&graph, 0) == XR_XIR_CALL_READY); ++result.polls;
        task_struct_control_capture(&graph, 0);
    }
    task_struct_control_capture(&graph, 0); result.phase = task_struct_control_phase(&graph, 0);
    result.captured = graph.tasks[2].type != 0; result.terminal = result.phase == 3;
    const XrXirCallStatus expected = result.terminal || mode < 2 ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_CANCELLED;
    task_struct_control_same_peer(&graph, &peer);
    if (mode == 1) CHECK(xr_xir_instance_cancel_current(graph.instances[0]) == XR_XIR_CALL_CANCEL_REQUESTED);
    if (mode == 2) CHECK(xr_xir_instance_stop(graph.instances[0]) == XR_XIR_CALL_READY);
    if (mode && mode < 3) task_struct_control_finish(&graph, 0, XR_XIR_CALL_CANCELLED);
    task_struct_control_same_peer(&graph, &peer);
    if (order) {
        task_struct_control_finish(&graph, 1, XR_XIR_CALL_RETURNED);
        task_struct_control_free(&graph, 1); CHECK(!graph.fixture.releases);
    }
    if (result.terminal) {
        const XirTask *task = (const XirTask *)(uintptr_t)graph.tasks[2].payload;
        CHECK(task->state == XIR_TASK_TERMINAL && task->outcome.status == XR_XIR_CALL_RETURNED);
    }
    task_struct_control_free(&graph, 0);
    if (!order) {
        CHECK(!graph.fixture.releases); task_struct_control_same_peer(&graph, &peer);
        task_struct_control_finish(&graph, 1, XR_XIR_CALL_RETURNED); task_struct_control_free(&graph, 1);
    }
    CHECK(graph.fixture.releases == 1 && !graph.fixture.vm && !graph.fixture.entries);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(graph.witness[i].root_providers == (task_late_native(backend, true) ? 2u : 1u));
        CHECK(graph.witness[i].worker_providers == (task_late_native(backend, false) ? 2u : 1u));
    }
    CHECK(graph.witness[1].yielded_count == 2 && graph.witness[0].yielded_count >= 1 && graph.witness[0].yielded_count <= 2);
    task_struct_control_late(&graph, expected, &result);
    if (mode) printf("Struct Task control backend=%u order=%u prefix=%u mode=%u phase=%u captured=%u terminalBeforeControl=%u returned=%u cancelled=%u genuineWait peerTokenRootFeesUnchanged allRetainedTasksDead lateStructArrayCOW realSharedAtomic metadataDeadBeforeStrings codeLease1 physical0/0\n",
        backend, order, prefix, mode, result.phase, result.captured, result.terminal, result.returned, result.cancelled);
    return result;
}
int main(int argc, char **argv) {
    CHECK(argc == 2); (void)task_struct_graph; (void)task_late_graph;
    static const char *const names[] = {"0", "1", "vm", "native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], names[backend])) ++backend;
    CHECK(backend < 4); TaskStructControlResult baseline = task_struct_control_graph(backend, 0, 0, 0);
    CHECK(baseline.polls == 30 && baseline.polls < 4096);
    unsigned phases[4] = {0}, captured = 0, terminal = 0, returned = 0, cancelled = 0;
    for (unsigned order = 0; order < 2; ++order) for (unsigned prefix = 0; prefix < baseline.polls; ++prefix)
        for (unsigned mode = 1; mode < 4; ++mode) {
            TaskStructControlResult result = task_struct_control_graph(backend, prefix, mode, order);
            CHECK(result.polls == prefix && result.phase < 4); ++phases[result.phase];
            CHECK(result.returned+result.cancelled == 3+result.captured);
            CHECK(result.cancelled == (result.captured && !result.terminal && mode >= 2 ? 1u : 0u));
            captured += result.captured; terminal += result.terminal; returned += result.returned; cancelled += result.cancelled;
        }
    CHECK(phases[0] == 12 && phases[1] == 12 && phases[2] == 60 && phases[3] == 96);
    CHECK(captured == 156 && terminal == 96 && returned == 656 && cancelled == 40);
    printf("Struct Task control census backend=%u actualPolls=%u operations=%u phases=%u/%u/%u/%u captured=%u terminal=%u returned=%u cancelled=%u originalSourceAndLimits physical0/0\n",
        backend, baseline.polls, baseline.polls*6, phases[0], phases[1], phases[2], phases[3], captured, terminal, returned, cancelled);
    return 0;
}
