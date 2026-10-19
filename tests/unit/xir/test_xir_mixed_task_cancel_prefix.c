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
    XrXirDomainBudgetStats cost;
    size_t sites;
    bool complete;
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
    XrXirValueStatus copied = xr_xir_value_copy(&call->waiting_task, &run->tasks[1]);
    if (!task_result_value_status(run, copied)) { CHECK(!run->tasks[1].type && !run->tasks[1].payload); return; }
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
    do { status = task_pending_resource_tick(run); CHECK(++polls < 4096); } while (status == XR_XIR_CALL_READY);
    if (!run->complete) return;
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
static void task_prefix_drive(TaskResultRun *run, unsigned prefix, unsigned mode, TaskPrefixResult *result) {
    XrXirCallStatus started = xr_xir_instance_start(run->instance, p20_task_result_entries[0], NULL, 0);
    if (started != XR_XIR_CALL_READY) { CHECK(started == run->fault); run->complete = false; return; }
    if (!mode) {
        XrXirCallStatus status;
        do {
            status = task_pending_resource_tick(run); CHECK(++result->polls < 4096);
            if (!run->complete) return;
            task_prefix_observe(run); if (!run->complete) return;
        } while (status == XR_XIR_CALL_READY);
        CHECK(status == XR_XIR_CALL_RETURNED);
        XrXirValue returned = {0}; CHECK(xr_xir_instance_take_result(run->instance, &returned) == XR_XIR_CALL_RETURNED);
        CHECK(run->tasks[1].type && returned.type == run->tasks[1].type && returned.payload == run->tasks[1].payload);
        xr_xir_value_drop(&returned);
    } else {
        for (unsigned step = 0; step < prefix; ++step) {
            XrXirCallStatus status = task_pending_resource_tick(run);
            if (!run->complete) return;
            CHECK(status == XR_XIR_CALL_READY); task_prefix_observe(run); ++result->polls;
            if (!run->complete) return;
        }
        task_prefix_observe(run); if (!run->complete) return;
        result->phase = task_prefix_phase(run);
        result->captured = run->tasks[1].type != 0; result->terminal = result->phase == 3;
        if (mode == 1) {
            XrXirCallStatus status = xr_xir_instance_cancel_current(run->instance);
            if (status != XR_XIR_CALL_CANCEL_REQUESTED) {
                CHECK(status == XR_XIR_CALL_BAD_STATE);
                task_prefix_finish(run, run->fault);
                CHECK(!run->complete && run->driven); return;
            }
        }
        if (mode == 2) {
            XrXirCallStatus status = xr_xir_instance_stop(run->instance);
            if (status != XR_XIR_CALL_READY) { CHECK(status == run->fault); run->complete = false; run->driven = true; return; }
        }
        if (mode < 3) task_prefix_finish(run, XR_XIR_CALL_CANCELLED);
    }
}
static TaskPrefixResult task_prefix_operation(unsigned backend, unsigned prefix, unsigned mode,
    size_t failure, const XrXirInstanceConfig *limits) {
    CHECK(mode < 4 && !runtime_live && !runtime_bytes); runtime_fail_at = SIZE_MAX;
    TaskResultRun run = {.fault=limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM,.complete=true};
    XrXirProgram *program = task_late_program(&run.fixture, backend);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    if (limits) config = *limits;
    runtime_attempts = 0; runtime_fail_at = failure;
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &run.instance);
    run.complete = status == XR_XIR_CALL_READY; CHECK(run.complete || (status == run.fault && !run.instance));
    if (run.instance) { run.domain = run.instance->domain; CHECK(xr_xir_domain_retain(run.domain)); }
    xr_xir_compile_program_drop(program); CHECK(run.instance ? !run.fixture.releases : run.fixture.releases == 1);
    TaskPrefixResult result = {0};
    if (run.complete) { task_result_round(&run, 0); task_result_cost(&run, false); }
    if (run.complete) { CHECK(run.instance->domain == run.domain); task_prefix_drive(&run, prefix, mode, &result); task_result_cost(&run, false); }
    if (run.instance) {
        XrXirCallStatus expected = run.driven || run.instance->budget.exhausted ? run.fault : XR_XIR_CALL_READY;
        status = xr_xir_instance_free(run.instance); run.instance = NULL;
        if (status == run.fault) run.complete = false;
        else {
            if (status != expected) fprintf(stderr, "Task prefix retirement backend=%u prefix=%u mode=%u site=%zu status=%u expected=%u\n",
                backend, prefix, mode, failure, status, expected);
            CHECK(status == expected);
        }
    }
    CHECK(run.fixture.releases == 1 && !run.fixture.vm && !run.fixture.entries);
    unsigned outcome_mode = result.terminal ? 0 : mode;
    const bool execution_complete = run.complete;
    for (unsigned round = 0; round < 2; ++round) task_pending_resource_copy(&run, round, outcome_mode, execution_complete);
    if (run.domain) {
        task_result_cost(&run, false); result.cost = xr_xir_domain_budget_stats(run.domain);
        CHECK(!result.cost.call_live && !result.cost.metadata_live && result.cost.call_allocations == result.cost.call_frees &&
            result.cost.metadata_allocations == result.cost.metadata_frees && xr_xir_domain_stats(run.domain).live_bytes == sizeof(*run.domain));
        CHECK(result.cost.work <= config.work_limit && result.cost.requested_bytes <= config.requested_value_limit &&
            result.cost.requested_call_bytes <= config.requested_call_limit);
        xr_xir_domain_drop(run.domain);
    }
    result.complete = run.complete; result.sites = runtime_attempts; runtime_fail_at = SIZE_MAX;
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes); return result;
}
#ifndef XR_TASK_CANCEL_PREFIX_RESOURCE_MAIN
int main(int argc, char **argv) {
    CHECK(argc == 2); (void)task_late_graph; (void)task_result_axes; (void)task_pending_resource_axes; (void)task_pending_resource_number;
    static const char *const names[] = {"0", "1", "vm", "native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], names[backend])) ++backend;
    CHECK(backend < 4); TaskPrefixResult baseline = task_prefix_operation(backend, 0, 0, SIZE_MAX, NULL);
    CHECK(baseline.complete && baseline.polls == 23 && baseline.polls < 4096);
    unsigned masks[3] = {0}, captured[3] = {0}, terminal[3] = {0};
    for (unsigned prefix = 0; prefix < baseline.polls; ++prefix) for (unsigned mode = 1; mode < 4; ++mode) {
        TaskPrefixResult result = task_prefix_operation(backend, prefix, mode, SIZE_MAX, NULL);
        CHECK(result.complete && result.polls == prefix && result.phase < 4); masks[mode-1] |= 1u << result.phase;
        captured[mode-1] += result.captured; terminal[mode-1] += result.terminal;
        printf("Task cancel prefix backend=%u prefix=%u mode=%u phase=%u captured=%u terminalBeforeControl=%u genuineWaitValue warmTaskPreserved codeLease1 lateArrayCOW physical0/0\n",
            backend, prefix, mode, result.phase, result.captured, result.terminal);
    }
    for (unsigned i = 0; i < 3; ++i) CHECK(masks[i] == 15 && captured[i] == 19 && terminal[i] == 14);
    printf("Task cancel prefix census backend=%u actualPolls=%u operations=%u phaseMasks=%u/%u/%u captured=%u/%u/%u terminal=%u/%u/%u sourceDelegatesUnchanged physical0/0\n",
        backend, baseline.polls, baseline.polls*3, masks[0], masks[1], masks[2], captured[0], captured[1], captured[2], terminal[0], terminal[1], terminal[2]);
    return 0;
}
#endif
