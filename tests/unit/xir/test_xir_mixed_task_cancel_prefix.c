/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_cancel_prefix.c - Cancel every actual compound Task execution prefix
 */
#define XR_TASK_CANCEL_PREFIX_MAIN
#include "test_xir_mixed_task_pending_resources.c"

typedef struct TaskPrefixResult {
    unsigned polls, phase, captured, terminal;
} TaskPrefixResult;

static void task_prefix_observe(TaskResultRun *run) {
    if (run->tasks[1].type) return;
    XrXirCall *call = run->instance->call; XrXirTaskWaitToken token = {0};
    if (!call || !call->wait_linked || !call->waiting_task.type || !xr_xir_call_task_wait_token(call, &token)) return;
    const XirTask *task = (const XirTask *)(uintptr_t)call->waiting_task.payload;
    CHECK(xr_xir_task_storage_valid(task) && task->object.domain == run->domain);
    CHECK(token.binding.owner == run->instance->executor && token.binding.activation == &run->instance->executor->root);
    CHECK(token.binding.generation == run->instance->executor->generation && token.binding.ticket == run->instance->executor->root.ticket);
    CHECK(token.request.kind == XR_XIR_WAIT_TASK && token.request.subject == task);
    CHECK(xr_xir_value_copy(&call->waiting_task, &run->tasks[1]) == XR_XIR_VALUE_OK);
    CHECK(run->tasks[1].payload == call->waiting_task.payload && run->tasks[1].payload != run->tasks[0].payload);
}
static unsigned task_prefix_phase(TaskResultRun *run) {
    if (!run->tasks[1].type) return run->instance->executor->active ? 1u : 0u;
    const XirTask *task = (const XirTask *)(uintptr_t)run->tasks[1].payload;
    CHECK(xr_xir_task_storage_valid(task) && task->object.domain == run->domain);
    if (task->state != XIR_TASK_TERMINAL) {
        CHECK(task->executor == run->instance->executor && task->call && task->activation.call == task->call);
        XrXirCallResult empty = {0};
        CHECK(xr_xir_task_copy_outcome(&run->tasks[1], &empty) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&empty));
        return 2;
    }
    CHECK(task->outcome.status == XR_XIR_CALL_RETURNED && !task->call && !task->executor); return 3;
}
static void task_prefix_finish(TaskResultRun *run, XrXirCallStatus expected) {
    unsigned polls = 0; XrXirCallStatus status;
    do { status = task_pending_resource_tick(run); CHECK(++polls < 4096 && run->complete); } while (status == XR_XIR_CALL_READY);
    CHECK(status == expected);
    if (status == XR_XIR_CALL_RETURNED) {
        XrXirValue returned = {0}; CHECK(xr_xir_instance_take_result(run->instance, &returned) == XR_XIR_CALL_RETURNED);
        if (run->tasks[1].type) {
            CHECK(returned.type == run->tasks[1].type && returned.payload == run->tasks[1].payload); xr_xir_value_drop(&returned);
        } else run->tasks[1] = returned;
    }
}
/* Prefix zero means immediately after the actual second start. Every later
 * prefix is measured in completed public quantum-one polls, never fabricated actions. */
static TaskPrefixResult task_prefix_operation(unsigned backend, unsigned prefix, unsigned mode) {
    CHECK(mode < 4 && !runtime_live && !runtime_bytes); runtime_fail_at = SIZE_MAX;
    TaskResultRun run = {.fault=XR_XIR_CALL_OOM,.complete=true};
    XrXirProgram *program = task_late_program(&run.fixture, backend);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program, &config, &run.instance) == XR_XIR_CALL_READY);
    run.domain = run.instance->domain; CHECK(xr_xir_domain_retain(run.domain));
    xr_xir_compile_program_drop(program); CHECK(!run.fixture.releases);
    task_result_round(&run, 0); CHECK(run.complete); task_result_cost(&run, false);
    CHECK(xr_xir_instance_start(run.instance, p20_task_result_entries[0], NULL, 0) == XR_XIR_CALL_READY);
    TaskPrefixResult result = {0};
    if (!mode) {
        XrXirCallStatus status;
        do {
            status = task_pending_resource_tick(&run); CHECK(++result.polls < 4096 && run.complete);
            task_prefix_observe(&run);
        } while (status == XR_XIR_CALL_READY);
        CHECK(status == XR_XIR_CALL_RETURNED);
        XrXirValue returned = {0}; CHECK(xr_xir_instance_take_result(run.instance, &returned) == XR_XIR_CALL_RETURNED);
        CHECK(run.tasks[1].type && returned.type == run.tasks[1].type && returned.payload == run.tasks[1].payload);
        xr_xir_value_drop(&returned);
    } else {
        for (unsigned step = 0; step < prefix; ++step) {
            CHECK(task_pending_resource_tick(&run) == XR_XIR_CALL_READY && run.complete); task_prefix_observe(&run); ++result.polls;
        }
        task_prefix_observe(&run); result.phase = task_prefix_phase(&run);
        result.captured = run.tasks[1].type != 0; result.terminal = result.phase == 3;
        if (mode == 1) CHECK(xr_xir_instance_cancel_current(run.instance) == XR_XIR_CALL_CANCEL_REQUESTED);
        if (mode == 2) CHECK(xr_xir_instance_stop(run.instance) == XR_XIR_CALL_READY);
        if (mode < 3) task_prefix_finish(&run, XR_XIR_CALL_CANCELLED);
    }
    task_result_cost(&run, false);
    CHECK(xr_xir_instance_free(run.instance) == XR_XIR_CALL_READY); run.instance = NULL;
    CHECK(run.fixture.releases == 1 && !run.fixture.vm && !run.fixture.entries);
    unsigned outcome_mode = result.terminal ? 0 : mode;
    for (unsigned round = 0; round < 2; ++round) task_pending_resource_copy(&run, round, outcome_mode, true);
    CHECK(run.complete);
    XrXirDomainBudgetStats final = xr_xir_domain_budget_stats(run.domain);
    CHECK(final.work >= run.previous.work && final.requested_bytes >= run.previous.requested_bytes &&
        final.requested_call_bytes >= run.previous.requested_call_bytes);
    CHECK(!final.call_live && !final.metadata_live && final.call_allocations == final.call_frees &&
        final.metadata_allocations == final.metadata_frees && xr_xir_domain_stats(run.domain).live_bytes == sizeof(*run.domain));
    xr_xir_domain_drop(run.domain);
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes); return result;
}
int main(int argc, char **argv) {
    CHECK(argc == 2); (void)task_late_graph; (void)task_result_axes; (void)task_pending_resource_axes; (void)task_pending_resource_number;
    static const char *const names[] = {"0", "1", "vm", "native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], names[backend])) ++backend;
    CHECK(backend < 4); TaskPrefixResult baseline = task_prefix_operation(backend, 0, 0);
    CHECK(baseline.polls == 23 && baseline.polls < 4096);
    unsigned masks[3] = {0}, captured[3] = {0}, terminal[3] = {0};
    for (unsigned prefix = 0; prefix < baseline.polls; ++prefix) for (unsigned mode = 1; mode < 4; ++mode) {
        TaskPrefixResult result = task_prefix_operation(backend, prefix, mode);
        CHECK(result.polls == prefix && result.phase < 4); masks[mode-1] |= 1u << result.phase;
        captured[mode-1] += result.captured; terminal[mode-1] += result.terminal;
        printf("Task cancel prefix backend=%u prefix=%u mode=%u phase=%u captured=%u terminalBeforeControl=%u genuineWaitValue warmTaskPreserved codeLease1 lateArrayCOW physical0/0\n",
            backend, prefix, mode, result.phase, result.captured, result.terminal);
    }
    for (unsigned i = 0; i < 3; ++i) CHECK(masks[i] == 15 && captured[i] == 19 && terminal[i] == 14);
    printf("Task cancel prefix census backend=%u actualPolls=%u operations=%u phaseMasks=%u/%u/%u captured=%u/%u/%u terminal=%u/%u/%u sourceDelegatesUnchanged physical0/0\n",
        backend, baseline.polls, baseline.polls*3, masks[0], masks[1], masks[2], captured[0], captured[1], captured[2], terminal[0], terminal[1], terminal[2]);
    return 0;
}
