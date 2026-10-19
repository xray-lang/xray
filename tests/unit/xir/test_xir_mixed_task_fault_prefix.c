/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_fault_prefix.c - Retire real exception Tasks at every public poll boundary
 */
#define XR_TASK_FAULT_RESOURCE_MAIN
#include "test_xir_mixed_task_fault_lifetime.c"

typedef struct TaskFaultPrefixPair {
    TaskLateFixture fixture;
    XrXirInstance *instances[2];
    XrXirDomain *domains[2];
    XrXirValue tasks[2][2];
    TaskLateWitness witness[2];
    unsigned captured[2];
} TaskFaultPrefixPair;
typedef struct TaskFaultPrefixResult {
    unsigned polls, phase, captured, terminal, copies, cancelled;
} TaskFaultPrefixResult;

static void task_fault_prefix_cost(XrXirDomainBudgetStats before, XrXirDomainBudgetStats after) {
    CHECK(before.work == after.work && before.requested_bytes == after.requested_bytes &&
        before.requested_call_bytes == after.requested_call_bytes);
}
static XrXirCallStatus task_fault_prefix_tick(TaskFaultPrefixPair *pair, unsigned index) {
    XrXirInstance *peer = pair->instances[1-index]; XrXirDomainBudgetStats before = {0};
    if (peer) before = xr_xir_domain_budget_stats(peer->domain);
    XrXirInstanceResult event = xr_xir_instance_poll_bounded(pair->instances[index], 1);
    XrXirCallStatus status = event.outcome.status;
    CHECK(status == XR_XIR_CALL_READY || status == XR_XIR_CALL_RETURNED || status == XR_XIR_CALL_CANCELLED);
    if (status == XR_XIR_CALL_CANCELLED) CHECK(xr_xir_call_result_valid(&event.outcome) &&
        !event.outcome.value.type && !event.outcome.value.reserved && !event.outcome.value.payload &&
        !event.outcome.wake && xr_xir_panic_empty(&event.outcome.panic));
    task_late_observe(&pair->fixture, pair->instances[index], &pair->witness[index]);
    if (peer) task_fault_prefix_cost(before, xr_xir_domain_budget_stats(peer->domain));
    return status;
}
/* Only copy the canonical stopped wait value. Early prefixes with no await
 * value keep an empty slot rather than manufacturing a Task handle. */
static void task_fault_prefix_capture(TaskFaultPrefixPair *pair, unsigned index) {
    XrXirInstance *instance = pair->instances[index]; XrXirCall *call = instance->call;
    XrXirTaskWaitToken token = {0};
    if (!call || !call->wait_linked || !call->waiting_task.type || !xr_xir_call_task_wait_token(call, &token)) return;
    const XirTask *task = (const XirTask *)(uintptr_t)call->waiting_task.payload;
    CHECK(xr_xir_task_storage_valid(task) && task->object.domain == pair->domains[index]);
    CHECK(token.binding.owner == instance->executor && token.binding.activation == &instance->executor->root);
    CHECK(token.binding.generation == instance->executor->generation && token.binding.ticket == instance->executor->root.ticket);
    CHECK(token.request.kind == XR_XIR_WAIT_TASK && token.request.subject == task);
    for (unsigned kind = 0; kind < pair->captured[index]; ++kind)
        if (pair->tasks[index][kind].payload == call->waiting_task.payload) return;
    if (task->state == XIR_TASK_TERMINAL) return;
    CHECK(task->executor == instance->executor && task->call && task->activation.call == task->call);
    CHECK(pair->captured[index] < 2);
    size_t attempts = runtime_attempts; XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(instance->domain);
    XrXirValue *output = &pair->tasks[index][pair->captured[index]++];
    CHECK(xr_xir_value_copy(&call->waiting_task, output) == XR_XIR_VALUE_OK && output->payload == call->waiting_task.payload);
    XrXirDomainBudgetStats copied = xr_xir_domain_budget_stats(instance->domain);
    CHECK(copied.work == before.work+1 && copied.requested_bytes == before.requested_bytes &&
        copied.requested_call_bytes == before.requested_call_bytes);
    XrXirCallResult empty = {0};
    CHECK(xr_xir_task_copy_outcome(output, &empty) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&empty));
    task_fault_prefix_cost(copied, xr_xir_domain_budget_stats(instance->domain)); CHECK(attempts == runtime_attempts);
}
static void task_fault_prefix_observation(TaskFaultPrefixPair *pair, unsigned index, const char *text) {
    CHECK(xr_xir_instance_start(pair->instances[index], p20_task_result_entries[1], NULL, 0) == XR_XIR_CALL_READY);
    unsigned polls = 0; XrXirCallStatus status;
    do { status = task_fault_prefix_tick(pair, index); CHECK(++polls < 4096); } while (status == XR_XIR_CALL_READY);
    CHECK(status == XR_XIR_CALL_RETURNED); XrXirValue result = {0};
    CHECK(xr_xir_instance_take_result(pair->instances[index], &result) == XR_XIR_CALL_RETURNED);
    task_late_text(&result, text); xr_xir_value_drop(&result);
}
static unsigned task_fault_prefix_finish(TaskFaultPrefixPair *pair, unsigned index, XrXirCallStatus expected) {
    unsigned polls = 0; XrXirCallStatus status;
    do {
        status = task_fault_prefix_tick(pair, index); CHECK(++polls < 4096);
        if (status == XR_XIR_CALL_READY) task_fault_prefix_capture(pair, index);
    } while (status == XR_XIR_CALL_READY);
    CHECK(status == expected); XrXirValue returned = {0};
    if (expected == XR_XIR_CALL_RETURNED) {
        CHECK(pair->captured[index] == 2);
        CHECK(xr_xir_instance_take_result(pair->instances[index], &returned) == XR_XIR_CALL_RETURNED);
        CHECK(returned.type == pair->tasks[index][1].type && returned.payload == pair->tasks[index][1].payload);
        xr_xir_value_drop(&returned);
    }
    CHECK(xr_xir_instance_take_result(pair->instances[index], &returned) == XR_XIR_CALL_BAD_STATE &&
        !returned.type && !returned.reserved && !returned.payload);
    return polls;
}
static void task_fault_prefix_peer(TaskFaultPrefixPair *pair, unsigned index) {
    CHECK(xr_xir_instance_start(pair->instances[index], p20_task_result_entries[0], NULL, 0) == XR_XIR_CALL_READY);
    unsigned polls = 0;
    do {
        CHECK(task_fault_prefix_tick(pair, index) == XR_XIR_CALL_READY && ++polls < 4096);
        task_fault_prefix_capture(pair, index);
    } while (!pair->captured[index] || !pair->witness[index].yielded_count);
    const XirTask *task = (const XirTask *)(uintptr_t)pair->tasks[index][0].payload;
    CHECK(pair->captured[index] == 1 && task->state != XIR_TASK_TERMINAL && task->call->next_wake == 1);
}
static void task_fault_prefix_same_wait(XrXirInstance *instance, const XrXirTaskWaitToken *before) {
    XrXirTaskWaitToken after = {0}; CHECK(xr_xir_call_task_wait_token(instance->call, &after));
    CHECK(after.binding.owner == before->binding.owner && after.binding.activation == before->binding.activation &&
        after.binding.generation == before->binding.generation && after.binding.ticket == before->binding.ticket);
    CHECK(after.wake == before->wake && after.request.kind == before->request.kind && after.request.reserved == before->request.reserved);
    CHECK(after.request.after_ms == before->request.after_ms && after.request.subject == before->request.subject &&
        after.request.generation == before->request.generation && after.request.ticket == before->request.ticket);
    CHECK((const void *)(uintptr_t)instance->call->waiting_task.payload == before->request.subject);
}
static unsigned task_fault_prefix_state(TaskFaultPrefixPair *pair, unsigned index, XrXirCallStatus before[2]) {
    unsigned count = pair->captured[index];
    for (unsigned kind = 0; kind < count; ++kind) {
        const XirTask *task = (const XirTask *)(uintptr_t)pair->tasks[index][kind].payload;
        CHECK(xr_xir_task_storage_valid(task) && task->object.domain == pair->domains[index]);
        if (task->state != XIR_TASK_TERMINAL) continue;
        CHECK(!task->call && !task->executor && task->outcome.status == (kind ? XR_XIR_CALL_ASSERTION : XR_XIR_CALL_THROWN));
        before[kind] = task->outcome.status;
    }
    if (!count) return pair->instances[index]->executor->active ? 1u : 0u;
    if (count == 1) return before[0] == XR_XIR_CALL_READY ? 2u : 3u;
    CHECK(before[0] == XR_XIR_CALL_THROWN); return before[1] == XR_XIR_CALL_READY ? 4u : 5u;
}
static void task_fault_prefix_read(const XrXirCallResult *outcome, unsigned kind, XrXirDomain *domain) {
    if (outcome->status != XR_XIR_CALL_CANCELLED) { task_fault_text(outcome, kind, domain); return; }
    CHECK(xr_xir_call_result_valid(outcome) && !outcome->wake && !outcome->value.type &&
        !outcome->value.reserved && !outcome->value.payload && xr_xir_panic_empty(&outcome->panic));
}
static void task_fault_prefix_late(TaskFaultPrefixPair *pair, const XrXirCallStatus before[2],
    unsigned victim, unsigned mode, TaskFaultPrefixResult *result) {
    XrXirCallResult copies[2][2][2] = {{{{0}}}}; XrXirValue infos[2] = {{0}};
    for (unsigned i = 0; i < 2; ++i) for (unsigned kind = 0; kind < pair->captured[i]; ++kind) {
        const XirTask *task = (const XirTask *)(uintptr_t)pair->tasks[i][kind].payload;
        CHECK(xr_xir_task_storage_valid(task) && task->state == XIR_TASK_TERMINAL && !task->call && !task->executor);
        CHECK(task->object.domain == pair->domains[i]);
        XrXirCallStatus expected = kind ? XR_XIR_CALL_ASSERTION : XR_XIR_CALL_THROWN;
        if (i == victim && mode >= 2 && before[kind] == XR_XIR_CALL_READY) expected = XR_XIR_CALL_CANCELLED;
        if (i == victim && before[kind] != XR_XIR_CALL_READY) CHECK(expected == before[kind]);
        CHECK(task->outcome.status == expected);
        task_late_metadata_only(&pair->fixture, xr_xir_value_arena(&pair->tasks[i][kind]));
        for (unsigned n = 0; n < 2; ++n) {
            CHECK(xr_xir_task_copy_outcome(&pair->tasks[i][kind], &copies[i][kind][n]) == expected);
            task_fault_prefix_read(&copies[i][kind][n], kind, pair->domains[i]); ++result->copies;
            result->cancelled += expected == XR_XIR_CALL_CANCELLED;
        }
        if (expected == XR_XIR_CALL_ASSERTION)
            CHECK(xr_xir_panic_info_new(pair->domains[i], &copies[i][kind][0].panic, &infos[i]) == XR_XIR_VALUE_OK);
    }
    task_fault_tasks_drop(pair->tasks);
    task_late_metadata_only(&pair->fixture, xr_xir_value_arena(&copies[1-victim][0][0].value));
    for (unsigned i = 0; i < 2; ++i) for (unsigned kind = 0; kind < pair->captured[i]; ++kind) for (unsigned n = 0; n < 2; ++n) {
        task_fault_prefix_read(&copies[i][kind][n], kind, pair->domains[i]);
        xr_xir_call_result_drop(&copies[i][kind][n]); CHECK(xr_xir_call_result_empty(&copies[i][kind][n]));
    }
    CHECK(!effects_compile_live && !effects_compile_bytes);
    for (unsigned i = 0; i < 2; ++i) if (infos[i].type) {
        XrXirFaultDetail detail = {0}; XrXirValue message = {0};
        CHECK(xr_xir_panic_info_detail(&infos[i], &detail) && detail.code == 445);
        CHECK(xr_xir_panic_info_message(&infos[i], &message) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&infos[i]); task_late_text(&message, "task-panic"); xr_xir_value_drop(&message);
    }
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
}
static TaskFaultPrefixResult task_fault_prefix_graph(unsigned backend, unsigned prefix, unsigned mode, unsigned victim) {
    CHECK(mode < 4 && victim < 2); TaskFaultPrefixPair pair = {0}; TaskFaultPrefixResult result = {0};
    XrXirProgram *program = task_late_program(&pair.fixture, backend); task_fault_layout(program);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_new(program, &config, &pair.instances[i]) == XR_XIR_CALL_READY);
        pair.domains[i] = pair.instances[i]->domain;
    }
    CHECK(pair.domains[0] != pair.domains[1]); xr_xir_compile_program_drop(program); CHECK(!pair.fixture.releases);
    for (unsigned i = 0; i < 2; ++i) task_fault_prefix_observation(&pair, i, "cold");
    unsigned peer = 1-victim; task_fault_prefix_peer(&pair, peer);
    XrXirDomainBudgetStats peer_cost = xr_xir_domain_budget_stats(pair.domains[peer]);
    XrXirTaskWaitToken peer_wait = {0}; CHECK(xr_xir_call_task_wait_token(pair.instances[peer]->call, &peer_wait));
    CHECK(xr_xir_instance_start(pair.instances[victim], p20_task_result_entries[0], NULL, 0) == XR_XIR_CALL_READY);
    XrXirCallStatus before[2] = {XR_XIR_CALL_READY, XR_XIR_CALL_READY};
    if (!mode) {
        result.polls = task_fault_prefix_finish(&pair, victim, XR_XIR_CALL_RETURNED);
        task_fault_prefix_observation(&pair, victim, "thrown:task-panic:inner:outer");
    } else {
        for (unsigned step = 0; step < prefix; ++step) {
            CHECK(task_fault_prefix_tick(&pair, victim) == XR_XIR_CALL_READY);
            task_fault_prefix_capture(&pair, victim); ++result.polls;
        }
        task_fault_prefix_capture(&pair, victim);
        result.phase = task_fault_prefix_state(&pair, victim, before); result.captured = pair.captured[victim];
        for (unsigned kind = 0; kind < result.captured; ++kind) result.terminal += before[kind] != XR_XIR_CALL_READY;
        if (mode == 1) CHECK(xr_xir_instance_cancel_current(pair.instances[victim]) == XR_XIR_CALL_CANCEL_REQUESTED);
        if (mode == 2) CHECK(xr_xir_instance_stop(pair.instances[victim]) == XR_XIR_CALL_READY);
        if (mode < 3) (void)task_fault_prefix_finish(&pair, victim, XR_XIR_CALL_CANCELLED);
    }
    CHECK(xr_xir_instance_free(pair.instances[victim]) == XR_XIR_CALL_READY); pair.instances[victim] = NULL;
    CHECK(!pair.fixture.releases); task_fault_prefix_cost(peer_cost, xr_xir_domain_budget_stats(pair.domains[peer]));
    task_fault_prefix_same_wait(pair.instances[peer], &peer_wait);
    XrXirCallResult empty = {0}; size_t attempts = runtime_attempts;
    CHECK(xr_xir_task_copy_outcome(&pair.tasks[peer][0], &empty) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&empty));
    task_fault_prefix_cost(peer_cost, xr_xir_domain_budget_stats(pair.domains[peer])); CHECK(attempts == runtime_attempts);
    (void)task_fault_prefix_finish(&pair, peer, XR_XIR_CALL_RETURNED);
    task_fault_prefix_observation(&pair, peer, "thrown:task-panic:inner:outer");
    CHECK(pair.witness[peer].yielded_count == 2 && pair.witness[peer].root_providers == (task_late_native(backend, true) ? 2u : 1u));
    CHECK(pair.witness[peer].worker_providers == (task_late_native(backend, false) ? 2u : 1u));
    CHECK(xr_xir_instance_free(pair.instances[peer]) == XR_XIR_CALL_READY);
    CHECK(pair.fixture.releases == 1 && !pair.fixture.vm && !pair.fixture.entries);
    task_fault_prefix_late(&pair, before, victim, mode, &result); return result;
}
int main(int argc, char **argv) {
    CHECK(argc == 2); (void)task_late_graph; (void)task_fault_graph;
    static const char *const names[] = {"0", "1", "vm", "native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], names[backend])) ++backend;
    CHECK(backend < 4);
    for (unsigned victim = 0; victim < 2; ++victim) {
        TaskFaultPrefixResult baseline = task_fault_prefix_graph(backend, 0, 0, victim);
        CHECK(baseline.polls == 66 && baseline.polls < 4096); unsigned phases[3][6] = {{0}}, captured[3] = {0}, terminal[3] = {0};
        for (unsigned prefix = 0; prefix < baseline.polls; ++prefix) for (unsigned mode = 1; mode < 4; ++mode) {
            TaskFaultPrefixResult result = task_fault_prefix_graph(backend, prefix, mode, victim);
            CHECK(result.polls == prefix && result.phase < 6); ++phases[mode-1][result.phase];
            captured[mode-1] += result.captured; terminal[mode-1] += result.terminal;
            printf("Task fault prefix backend=%u firstDrop=%u prefix=%u mode=%u phase=%u captured=%u terminalBefore=%u lateCopies=%u cancelled=%u peerPending codeLease1 allTasksDead physical0/0\n",
                backend, victim, prefix, mode, result.phase, result.captured, result.terminal, result.copies, result.cancelled);
        }
        static const unsigned expected[] = {4, 2, 6, 14, 7, 33};
        for (unsigned i = 0; i < 3; ++i) {
            CHECK(captured[i] == 100 && terminal[i] == 87);
            for (unsigned phase = 0; phase < 6; ++phase) CHECK(phases[i][phase] == expected[phase]);
            printf("Task fault prefix census backend=%u firstDrop=%u mode=%u actualPolls=%u phases=%u/%u/%u/%u/%u/%u captured=%u terminal=%u\n",
                backend, victim, i+1, baseline.polls, phases[i][0], phases[i][1], phases[i][2], phases[i][3], phases[i][4], phases[i][5], captured[i], terminal[i]);
        }
    }
    return 0;
}
