/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_result_pending.c - Real pending compound Tasks survive owner cancellation
 */
#define XR_TASK_RESULT_RESOURCE_MAIN
#include "test_xir_mixed_task_result_lifetime.c"

typedef struct TaskPendingPair {
    TaskLateFixture fixture;
    XrXirInstance *instances[2];
    XrXirDomain *domains[2];
    XrXirValue tasks[2][2];
    TaskLateWitness witness[2];
} TaskPendingPair;

static void task_pending_equal_cost(XrXirDomainBudgetStats before, XrXirDomainBudgetStats after) {
    CHECK(before.work == after.work && before.requested_bytes == after.requested_bytes &&
        before.requested_call_bytes == after.requested_call_bytes);
}
static XrXirCallStatus task_pending_tick(TaskPendingPair *pair, unsigned index) {
    CHECK(pair->instances[index]);
    XrXirDomainBudgetStats peer = xr_xir_domain_budget_stats(pair->domains[1-index]);
    XrXirInstanceResult event = xr_xir_instance_poll_bounded(pair->instances[index], 1);
    XrXirCallStatus status = event.outcome.status;
    CHECK(status == XR_XIR_CALL_READY || status == XR_XIR_CALL_RETURNED || status == XR_XIR_CALL_CANCELLED);
    if (status == XR_XIR_CALL_CANCELLED) CHECK(!event.outcome.value.type && !event.outcome.value.reserved &&
        !event.outcome.value.payload && !event.outcome.wake && xr_xir_panic_empty(&event.outcome.panic));
    task_late_observe(&pair->fixture, pair->instances[index], &pair->witness[index]);
    task_pending_equal_cost(peer, xr_xir_domain_budget_stats(pair->domains[1-index])); return status;
}
static void task_pending_take(TaskPendingPair *pair, unsigned index, XrXirValue *value) {
    CHECK(xr_xir_instance_take_result(pair->instances[index], value) == XR_XIR_CALL_RETURNED);
    XrXirValue empty = {0};
    CHECK(xr_xir_instance_take_result(pair->instances[index], &empty) == XR_XIR_CALL_BAD_STATE && !empty.type && !empty.payload);
    const XirTask *task = (const XirTask *)(uintptr_t)value->payload;
    CHECK(xr_xir_task_storage_valid(task) && task->object.domain == pair->domains[index] && task->state == XIR_TASK_TERMINAL);
    CHECK(task->outcome.status == XR_XIR_CALL_RETURNED);
}
static void task_pending_warm(TaskPendingPair *pair) {
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_instance_start(pair->instances[i], p20_task_result_entries[0], NULL, 0) == XR_XIR_CALL_READY);
    bool done[2] = {0}; unsigned polls = 0;
    while (!done[0] || !done[1]) for (unsigned i = 0; i < 2; ++i) {
        if (done[i]) continue;
        XrXirCallStatus status = task_pending_tick(pair, i); CHECK(++polls < 8192);
        if (status == XR_XIR_CALL_READY) continue;
        CHECK(status == XR_XIR_CALL_RETURNED); task_pending_take(pair, i, &pair->tasks[0][i]); done[i] = true;
    }
}
/* The canonical Call already owns this pending Task. Copy its real value and
 * inspect the matching stopped wait token without changing entry or callback state. */
static void task_pending_capture(TaskPendingPair *pair, unsigned index) {
    XrXirInstance *instance = pair->instances[index]; unsigned polls = 0;
    for (;;) {
        CHECK(task_pending_tick(pair, index) == XR_XIR_CALL_READY && ++polls < 4096);
        XrXirCall *call = instance->call; XrXirTaskWaitToken token = {0};
        if (!call || !call->wait_linked || !call->waiting_task.type ||
            !xr_xir_call_task_wait_token(call, &token)) continue;
        const XirTask *task = (const XirTask *)(uintptr_t)call->waiting_task.payload;
        CHECK(xr_xir_task_storage_valid(task) && task->state != XIR_TASK_TERMINAL);
        CHECK(task->executor == instance->executor && task->object.domain == pair->domains[index]);
        CHECK(task->call && task->activation.call == task->call && task->activation.task == task);
        if (!task->call->next_wake) continue;
        CHECK(task->call->next_wake == 1 && pair->witness[index].yielded_count == 2);
        CHECK(token.binding.owner == instance->executor && token.binding.activation == &instance->executor->root);
        CHECK(token.binding.generation == instance->executor->generation && token.binding.ticket == instance->executor->root.ticket);
        CHECK(token.request.kind == XR_XIR_WAIT_TASK && token.request.subject == task);
        CHECK(xr_xir_value_copy(&call->waiting_task, &pair->tasks[1][index]) == XR_XIR_VALUE_OK);
        CHECK(pair->tasks[1][index].payload == call->waiting_task.payload &&
            pair->tasks[1][index].payload != pair->tasks[0][index].payload);
        XrXirCallResult empty = {0}; XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(pair->domains[index]);
        size_t attempts = runtime_attempts;
        CHECK(xr_xir_task_copy_outcome(&pair->tasks[1][index], &empty) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&empty));
        task_pending_equal_cost(before, xr_xir_domain_budget_stats(pair->domains[index])); CHECK(attempts == runtime_attempts);
        return;
    }
}
static void task_pending_finish(TaskPendingPair *pair, unsigned index, XrXirCallStatus expected) {
    XrXirCallStatus status; unsigned polls = 0;
    do { status = task_pending_tick(pair, index); CHECK(++polls < 4096); } while (status == XR_XIR_CALL_READY);
    CHECK(status == expected);
    if (status == XR_XIR_CALL_RETURNED) {
        XrXirValue returned = {0}; task_pending_take(pair, index, &returned);
        CHECK(returned.type == pair->tasks[1][index].type && returned.payload == pair->tasks[1][index].payload);
        xr_xir_value_drop(&returned);
    }
}
static bool task_pending_physical(uintptr_t address) {
    for (size_t i = 0; i < runtime_owned_capacity; ++i) if (runtime_owned[i].pointer) {
        uintptr_t base = (uintptr_t)runtime_owned[i].pointer;
        if (address >= base && address-base < runtime_owned[i].bytes) return true;
    }
    return false;
}
static unsigned task_pending_outcomes(TaskPendingPair *pair, unsigned mode, unsigned victim) {
    XrXirCallResult outcomes[8] = {{0}}; uintptr_t addresses[4] = {0};
    const XrXirTypeArena *arena = xr_xir_value_arena(&pair->tasks[0][0]);
    unsigned returned = 0;
    for (unsigned round = 0; round < 2; ++round) for (unsigned i = 0; i < 2; ++i) {
        XrXirValue *value = &pair->tasks[round][i]; unsigned at = round*2+i;
        const XirTask *task = (const XirTask *)(uintptr_t)value->payload; addresses[at] = (uintptr_t)task;
        CHECK(task_pending_physical(addresses[at]) && xr_xir_task_storage_valid(task) &&
            task->state == XIR_TASK_TERMINAL && !task->call && !task->executor);
        XrXirCallStatus expected = round && i == victim && mode >= 2 ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED;
        CHECK(task->outcome.status == expected && task->object.arena == arena);
        task_late_metadata_only(&pair->fixture, arena);
        XrXirCallResult occupied = {.status=XR_XIR_CALL_RETURNED,.value={XR_XIR_I64,0,193}}, saved;
        memcpy(&saved, &occupied, sizeof(saved));
        XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(task->object.domain); size_t attempts = runtime_attempts;
        CHECK(xr_xir_task_copy_outcome(value, &occupied) == XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&saved, &occupied, sizeof(saved)));
        task_pending_equal_cost(before, xr_xir_domain_budget_stats(task->object.domain)); CHECK(attempts == runtime_attempts);
        for (unsigned copy = 0; copy < 2; ++copy) {
            XrXirCallResult *outcome = &outcomes[at*2+copy];
            CHECK(xr_xir_task_copy_outcome(value, outcome) == expected && !outcome->wake && xr_xir_panic_empty(&outcome->panic));
            if (expected == XR_XIR_CALL_CANCELLED) CHECK(!outcome->value.type && !outcome->value.reserved && !outcome->value.payload);
        }
        if (expected == XR_XIR_CALL_RETURNED) ++returned;
    }
    size_t before_drop = runtime_attempts;
    for (unsigned round = 0; round < 2; ++round) for (unsigned i = 0; i < 2; ++i) {
        xr_xir_value_drop(&pair->tasks[round][i]);
        CHECK(!pair->tasks[round][i].type && !pair->tasks[round][i].reserved && !pair->tasks[round][i].payload);
    }
    CHECK(runtime_attempts == before_drop);
    for (unsigned i = 0; i < 4; ++i) CHECK(!task_pending_physical(addresses[i]));
    task_late_metadata_only(&pair->fixture, arena);
    XrXirDomain *receiver = NULL; CHECK(xr_xir_domain_new(65536, &receiver) == XR_XIR_VALUE_OK);
    XrXirValue replacement = {0}; CHECK(xr_xir_string_new(receiver, "elm", 3, &replacement) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena, receiver, NULL, NULL, 10000, 65536};
    for (unsigned i = 0; i < 4; ++i) if (outcomes[i*2].status == XR_XIR_CALL_RETURNED) {
        task_late_array(&outcomes[i*2].value, "cedar"); task_late_array(&outcomes[i*2+1].value, "cedar");
        XrXirValuePlace place = {(XrXirType)outcomes[i*2].value.type, &outcomes[i*2].value.payload}; XrXirFaultDetail fault = {0};
        CHECK(xr_xir_array_set(&place, 0, &replacement, &admission, &fault) == XR_XIR_VALUE_OK);
    }
    xr_xir_domain_drop(receiver); xr_xir_value_drop(&replacement);
    for (unsigned i = 0; i < 4; ++i) if (outcomes[i*2].status == XR_XIR_CALL_RETURNED) {
        task_late_array(&outcomes[i*2].value, "elm"); task_late_array(&outcomes[i*2+1].value, "cedar");
    }
    task_late_metadata_only(&pair->fixture, arena);
    for (unsigned i = 0; i < 8; ++i) { xr_xir_call_result_drop(&outcomes[i]); CHECK(xr_xir_call_result_empty(&outcomes[i])); }
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes); return returned;
}
static void task_pending_graph(unsigned backend, unsigned mode, unsigned victim) {
    TaskPendingPair pair = {0}; XrXirProgram *program = task_late_program(&pair.fixture, backend);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_new(program, &config, &pair.instances[i]) == XR_XIR_CALL_READY);
        pair.domains[i] = pair.instances[i]->domain; CHECK(xr_xir_domain_retain(pair.domains[i]));
    }
    CHECK(pair.domains[0] != pair.domains[1]); xr_xir_compile_program_drop(program); CHECK(!pair.fixture.releases);
    task_pending_warm(&pair);
    XrXirDomainBudgetStats warm[2];
    for (unsigned i = 0; i < 2; ++i) {
        warm[i] = xr_xir_domain_budget_stats(pair.domains[i]);
        CHECK(xr_xir_instance_start(pair.instances[i], p20_task_result_entries[0], NULL, 0) == XR_XIR_CALL_READY);
    }
    for (unsigned i = 0; i < 2; ++i) task_pending_capture(&pair, i);
    unsigned peer = 1-victim;
    XrXirDomainBudgetStats peer_cost = xr_xir_domain_budget_stats(pair.domains[peer]);
    if (mode == 1) CHECK(xr_xir_instance_cancel_current(pair.instances[victim]) == XR_XIR_CALL_CANCEL_REQUESTED);
    if (mode == 2) CHECK(xr_xir_instance_stop(pair.instances[victim]) == XR_XIR_CALL_READY);
    if (mode < 3) task_pending_finish(&pair, victim, mode ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(pair.instances[victim]) == XR_XIR_CALL_READY); pair.instances[victim] = NULL;
    CHECK(!pair.fixture.releases);
    task_pending_equal_cost(peer_cost, xr_xir_domain_budget_stats(pair.domains[peer]));
    XrXirCallResult pending = {0}; size_t attempts = runtime_attempts;
    CHECK(xr_xir_task_copy_outcome(&pair.tasks[1][peer], &pending) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&pending));
    task_pending_equal_cost(peer_cost, xr_xir_domain_budget_stats(pair.domains[peer])); CHECK(attempts == runtime_attempts);
    task_pending_finish(&pair, peer, XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(pair.instances[peer]) == XR_XIR_CALL_READY); pair.instances[peer] = NULL;
    CHECK(pair.fixture.releases == 1 && !pair.fixture.vm && !pair.fixture.entries);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(pair.witness[i].root_providers == (task_late_native(backend, true) ? 2u : 1u));
        CHECK(pair.witness[i].worker_providers == (task_late_native(backend, false) ? 2u : 1u) && pair.witness[i].yielded_count == 2);
        XrXirDomainBudgetStats cost = xr_xir_domain_budget_stats(pair.domains[i]);
        CHECK(cost.work > warm[i].work && cost.requested_bytes > warm[i].requested_bytes && cost.requested_call_bytes > warm[i].requested_call_bytes);
        CHECK(!cost.call_live && !cost.metadata_live && cost.call_allocations == cost.call_frees && cost.metadata_allocations == cost.metadata_frees);
        xr_xir_domain_drop(pair.domains[i]); pair.domains[i] = NULL;
    }
    unsigned returned = task_pending_outcomes(&pair, mode, victim);
    CHECK(returned == (mode < 2 ? 4u : 3u));
    printf("Task compound pending backend=%u mode=%u firstDrop=%u tasks4 warm2 originalDomains2 actualWaitValues2 childYields2/2 peerStillPending codeLease1 allTasksPhysicallyDead lateArrays=%u lateCancelled=%u lateCOW=%u metadataOnly2 physical0/0\n",
        backend, mode, victim, returned*2, 8-returned*2, returned);
}
int main(int argc, char **argv) {
    CHECK(argc == 2); (void)task_late_graph;
    static const char *const names[] = {"0", "1", "vm", "native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], names[backend])) ++backend;
    CHECK(backend < 4);
    for (unsigned mode = 0; mode < 4; ++mode) for (unsigned victim = 0; victim < 2; ++victim)
        task_pending_graph(backend, mode, victim);
    return 0;
}
