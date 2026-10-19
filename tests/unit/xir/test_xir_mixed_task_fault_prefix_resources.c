/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_fault_prefix_resources.c - Bound every real exception Task cancellation prefix
 */
#define XR_TASK_FAULT_PREFIX_RESOURCE_MAIN
#include "test_xir_mixed_task_fault_resources.c"

typedef struct TaskFaultPrefixResourceResult {
    XrXirDomainBudgetStats cost;
    size_t sites;
    unsigned polls, phase, captured, terminal;
    bool complete;
} TaskFaultPrefixResourceResult;

static XrXirCallStatus task_fault_prefix_resource_tick(TaskFaultRun *run) {
    XrXirInstanceResult event = xr_xir_instance_poll_bounded(run->instance, 1);
    XrXirCallStatus status = event.outcome.status;
    if (status == run->fault) { run->complete = false; run->driven = true; }
    else CHECK(status == XR_XIR_CALL_READY || status == XR_XIR_CALL_RETURNED || status == XR_XIR_CALL_CANCELLED);
    if (status == run->fault || status == XR_XIR_CALL_CANCELLED) CHECK(!event.outcome.value.type &&
        !event.outcome.value.reserved && !event.outcome.value.payload && !event.outcome.wake && xr_xir_panic_empty(&event.outcome.panic));
    task_late_observe(&run->fixture, run->instance, &run->witness); return status;
}
static void task_fault_prefix_resource_capture(TaskFaultRun *run) {
    XrXirCall *call = run->instance->call; XrXirTaskWaitToken token = {0};
    if (!call || !call->wait_linked || !call->waiting_task.type || !xr_xir_call_task_wait_token(call, &token)) return;
    const XirTask *task = (const XirTask *)(uintptr_t)call->waiting_task.payload;
    CHECK(xr_xir_task_storage_valid(task) && task->object.domain == run->domain);
    CHECK(token.binding.owner == run->instance->executor && token.binding.activation == &run->instance->executor->root);
    CHECK(token.binding.generation == run->instance->executor->generation && token.binding.ticket == run->instance->executor->root.ticket);
    CHECK(token.request.kind == XR_XIR_WAIT_TASK && token.request.subject == task);
    for (unsigned kind = 0; kind < run->captured; ++kind) if (run->tasks[kind].payload == call->waiting_task.payload) return;
    if (task->state == XIR_TASK_TERMINAL) return;
    CHECK(run->captured < 2 && task->call && task->executor == run->instance->executor);
    XrXirValue *output = &run->tasks[run->captured];
    if (!task_fault_resource_value(run, xr_xir_value_copy(&call->waiting_task, output))) {
        CHECK(!output->type && !output->reserved && !output->payload); return;
    }
    ++run->captured; XrXirCallResult empty = {0}; size_t attempts = runtime_attempts;
    XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(run->domain);
    CHECK(xr_xir_task_copy_outcome(output, &empty) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&empty));
    XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(run->domain);
    CHECK(before.work == after.work && before.requested_bytes == after.requested_bytes &&
        before.requested_call_bytes == after.requested_call_bytes && attempts == runtime_attempts);
}
static void task_fault_prefix_resource_finish(TaskFaultRun *run, XrXirCallStatus expected, unsigned *polls) {
    XrXirCallStatus status; unsigned steps = 0;
    do {
        status = task_fault_prefix_resource_tick(run); CHECK(++steps < 4096);
        if (polls) ++*polls;
        if (!run->complete) return;
        if (status == XR_XIR_CALL_READY) { task_fault_prefix_resource_capture(run); if (!run->complete) return; }
    } while (status == XR_XIR_CALL_READY);
    CHECK(status == expected); XrXirValue returned = {0};
    if (expected == XR_XIR_CALL_RETURNED) {
        CHECK(run->captured == 2);
        CHECK(xr_xir_instance_take_result(run->instance, &returned) == XR_XIR_CALL_RETURNED);
        CHECK(returned.type == run->tasks[1].type && returned.payload == run->tasks[1].payload); xr_xir_value_drop(&returned);
    }
    CHECK(xr_xir_instance_take_result(run->instance, &returned) == XR_XIR_CALL_BAD_STATE &&
        !returned.type && !returned.reserved && !returned.payload);
}
static unsigned task_fault_prefix_resource_state(TaskFaultRun *run, XrXirCallStatus before[2]) {
    for (unsigned kind = 0; kind < run->captured; ++kind) {
        const XirTask *task = (const XirTask *)(uintptr_t)run->tasks[kind].payload;
        CHECK(xr_xir_task_storage_valid(task) && task->object.domain == run->domain);
        if (task->state != XIR_TASK_TERMINAL) continue;
        before[kind] = task->outcome.status;
        if (before[kind] == run->fault) run->complete = false;
        else CHECK(before[kind] == (kind ? XR_XIR_CALL_ASSERTION : XR_XIR_CALL_THROWN));
    }
    if (!run->captured) return run->instance->executor->active ? 1u : 0u;
    if (run->captured == 1) return before[0] == XR_XIR_CALL_READY ? 2u : 3u;
    CHECK(before[0] == XR_XIR_CALL_THROWN);
    return before[1] == XR_XIR_CALL_READY ? 4u : 5u;
}
static void task_fault_prefix_resource_drive(TaskFaultRun *run, unsigned prefix, unsigned mode,
    TaskFaultPrefixResourceResult *result, XrXirCallStatus before[2]) {
    XrXirCallStatus status = xr_xir_instance_start(run->instance, p20_task_result_entries[0], NULL, 0);
    if (status != XR_XIR_CALL_READY) { CHECK(status == run->fault); run->complete = false; return; }
    if (!mode) { task_fault_prefix_resource_finish(run, XR_XIR_CALL_RETURNED, &result->polls); return; }
    for (unsigned step = 0; step < prefix; ++step) {
        status = task_fault_prefix_resource_tick(run); if (!run->complete) return;
        CHECK(status == XR_XIR_CALL_READY); ++result->polls; task_fault_prefix_resource_capture(run);
        if (!run->complete) return;
    }
    task_fault_prefix_resource_capture(run); if (!run->complete) return;
    result->phase = task_fault_prefix_resource_state(run, before); result->captured = run->captured;
    for (unsigned kind = 0; kind < run->captured; ++kind) result->terminal += before[kind] != XR_XIR_CALL_READY;
    if (!run->complete) return;
    if (mode == 1) {
        status = xr_xir_instance_cancel_current(run->instance);
        if (status != XR_XIR_CALL_CANCEL_REQUESTED) {
            CHECK(status == XR_XIR_CALL_BAD_STATE); task_fault_prefix_resource_finish(run, run->fault, NULL);
            CHECK(!run->complete && run->driven); return;
        }
    }
    if (mode == 2) {
        status = xr_xir_instance_stop(run->instance);
        if (status != XR_XIR_CALL_READY) { CHECK(status == run->fault); run->complete = false; run->driven = true; return; }
    }
    if (mode < 3) task_fault_prefix_resource_finish(run, XR_XIR_CALL_CANCELLED, NULL);
}
/* Fix normal expectations before any late copy can fail. Earlier terminal
 * children also remain sticky when an execution fault forced retirement. */
static void task_fault_prefix_resource_late(TaskFaultRun *run, unsigned mode,
    const XrXirCallStatus control[2], const XrXirCallStatus retirement[2], bool execution_complete) {
    XrXirCallStatus observed[2] = {XR_XIR_CALL_READY, XR_XIR_CALL_READY};
    for (unsigned kind = 0; kind < run->captured; ++kind) {
        const XirTask *task = (const XirTask *)(uintptr_t)run->tasks[kind].payload;
        CHECK(xr_xir_task_storage_valid(task) && task->state == XIR_TASK_TERMINAL && !task->call && !task->executor);
        CHECK(task->object.domain == run->domain); observed[kind] = task->outcome.status;
        if (control[kind] != XR_XIR_CALL_READY) CHECK(observed[kind] == control[kind]);
        if (retirement[kind] != XR_XIR_CALL_READY) CHECK(observed[kind] == retirement[kind]);
        XrXirCallStatus expected = kind ? XR_XIR_CALL_ASSERTION : XR_XIR_CALL_THROWN;
        if (mode >= 2 && control[kind] == XR_XIR_CALL_READY) expected = XR_XIR_CALL_CANCELLED;
        if (execution_complete) CHECK(observed[kind] == expected);
        else CHECK(observed[kind] == (kind ? XR_XIR_CALL_ASSERTION : XR_XIR_CALL_THROWN) ||
            observed[kind] == XR_XIR_CALL_CANCELLED || observed[kind] == run->fault);
    }
    task_fault_resource_late(run, observed, execution_complete && mode < 2);
}
static TaskFaultPrefixResourceResult task_fault_prefix_resource_operation(unsigned backend, unsigned prefix,
    unsigned mode, size_t failure, const XrXirInstanceConfig *limits) {
    CHECK(mode < 4 && !runtime_live && !runtime_bytes); runtime_fail_at = SIZE_MAX;
    TaskFaultRun run = {.fault=limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM, .complete=true};
    XrXirProgram *program = task_late_program(&run.fixture, backend); task_fault_layout(program);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    if (limits) config = *limits;
    runtime_attempts = 0; runtime_fail_at = failure;
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &run.instance);
    run.complete = status == XR_XIR_CALL_READY; CHECK(run.complete || (status == run.fault && !run.instance));
    if (run.instance) { run.domain = run.instance->domain; CHECK(xr_xir_domain_retain(run.domain)); }
    xr_xir_compile_program_drop(program); CHECK(run.instance ? !run.fixture.releases : run.fixture.releases == 1);
    TaskFaultPrefixResourceResult result = {0}; XrXirCallStatus control[2] = {0}, retirement[2] = {0};
    if (run.complete) task_fault_resource_call(&run, 1, "cold");
    if (run.complete) task_fault_prefix_resource_drive(&run, prefix, mode, &result, control);
    for (unsigned kind = 0; kind < run.captured; ++kind) {
        const XirTask *task = (const XirTask *)(uintptr_t)run.tasks[kind].payload;
        if (task->state == XIR_TASK_TERMINAL) retirement[kind] = task->outcome.status;
    }
    if (run.instance) {
        bool failed = run.driven || run.instance->budget.exhausted;
        status = xr_xir_instance_free(run.instance); run.instance = NULL;
        if (failed) CHECK(status == run.fault); else CHECK(status == XR_XIR_CALL_READY || status == run.fault);
        if (status == run.fault) run.complete = false;
    }
    CHECK(run.fixture.releases == 1 && !run.fixture.vm && !run.fixture.entries);
    const bool execution_complete = run.complete;
    task_fault_prefix_resource_late(&run, mode, control, retirement, execution_complete);
    if (run.domain) {
        result.cost = xr_xir_domain_budget_stats(run.domain);
        CHECK(!result.cost.call_live && !result.cost.metadata_live && result.cost.call_allocations == result.cost.call_frees &&
            result.cost.metadata_allocations == result.cost.metadata_frees && xr_xir_domain_stats(run.domain).live_bytes == sizeof(*run.domain));
        CHECK(result.cost.requested_bytes <= config.requested_value_limit && result.cost.requested_call_bytes <= config.requested_call_limit && result.cost.work <= config.work_limit);
        xr_xir_domain_drop(run.domain);
    }
    result.sites = runtime_attempts; result.complete = run.complete; runtime_fail_at = SIZE_MAX;
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes); return result;
}
static size_t task_fault_prefix_resource_axes(unsigned backend, unsigned prefix, unsigned mode) {
    TaskFaultPrefixResourceResult normal = task_fault_prefix_resource_operation(backend, prefix, mode, SIZE_MAX, NULL);
    CHECK(normal.complete && normal.polls == prefix && normal.sites && normal.sites < 20000);
    unsigned phase = prefix < 4 ? 0u : prefix < 6 ? 1u : prefix < 12 ? 2u : prefix < 26 ? 3u : prefix < 33 ? 4u : 5u;
    CHECK(normal.phase == phase && normal.captured == (phase < 2 ? 0u : phase < 4 ? 1u : 2u) &&
        normal.terminal == (phase < 3 ? 0u : phase < 5 ? 1u : 2u));
    const uint64_t axes[] = {normal.cost.requested_bytes, normal.cost.requested_call_bytes, normal.cost.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY && axes[axis]);
        if (!axis) config.requested_value_limit = axes[axis]-minus;
        else if (axis == 1) config.requested_call_limit = axes[axis]-minus;
        else config.work_limit = axes[axis]-minus;
        TaskFaultPrefixResourceResult result = task_fault_prefix_resource_operation(backend, prefix, mode, SIZE_MAX, &config);
        if (result.complete != !minus) fprintf(stderr, "Task fault prefix axis mismatch backend=%u prefix=%u mode=%u axis=%u minus=%u complete=%u\n",
            backend, prefix, mode, axis, minus, (unsigned)result.complete);
        CHECK(result.complete == !minus);
        if (!minus) CHECK(result.phase == normal.phase && result.captured == normal.captured && result.terminal == normal.terminal);
        printf("Task fault prefix resource axis backend=%u prefix=%u mode=%u axis=%u minus=%u complete=%u value=%llu call=%llu work=%llu\n",
            backend, prefix, mode, axis, minus, (unsigned)result.complete,
            (unsigned long long)result.cost.requested_bytes, (unsigned long long)result.cost.requested_call_bytes, (unsigned long long)result.cost.work);
    }
    printf("Task fault prefix resources backend=%u prefix=%u mode=%u phase=%u captured=%u terminal=%u sites=%zu value=%llu call=%llu work=%llu sameOriginalDomain independentStickyOracle codeLease1 physical0/0\n",
        backend, prefix, mode, normal.phase, normal.captured, normal.terminal, normal.sites,
        (unsigned long long)axes[0], (unsigned long long)axes[1], (unsigned long long)axes[2]); return normal.sites;
}
static size_t task_fault_prefix_resource_number(const char *text) {
    char *end = NULL; unsigned long number = strtoul(text, &end, 10);
    CHECK(text[0] && end && !*end && number < 20000); return (size_t)number;
}
int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 6); (void)task_fault_resource_axes; (void)task_late_graph; (void)task_fault_graph;
    static const char *const names[] = {"0", "1", "vm", "native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], names[backend])) ++backend;
    CHECK(backend < 4); unsigned mode = (unsigned)task_fault_prefix_resource_number(argv[argc == 3 ? 2 : 3]);
    CHECK(mode >= 1 && mode <= 3);
    TaskFaultPrefixResourceResult baseline = task_fault_prefix_resource_operation(backend, 0, 0, SIZE_MAX, NULL);
    CHECK(baseline.complete && baseline.polls == 66);
    if (argc == 6) {
        CHECK(!strcmp(argv[2], "fi")); unsigned prefix = (unsigned)task_fault_prefix_resource_number(argv[4]);
        CHECK(prefix < baseline.polls); size_t sites = task_fault_prefix_resource_axes(backend, prefix, mode);
        CHECK(sites == task_fault_prefix_resource_number(argv[5]));
        for (size_t site = 0; site < sites; ++site) {
            TaskFaultPrefixResourceResult result = task_fault_prefix_resource_operation(backend, prefix, mode, site, NULL);
            CHECK(!result.complete && result.sites > site);
        }
        printf("Task fault prefix complete runtime FI backend=%u prefix=%u mode=%u sites=%zu everyOrdinalHit typedOOM sameOriginalDomain codeLease1 physical0/0\n",
            backend, prefix, mode, sites); return 0;
    }
    size_t sites = 0;
    for (unsigned prefix = 0; prefix < baseline.polls; ++prefix) sites += task_fault_prefix_resource_axes(backend, prefix, mode);
    printf("Task fault prefix resource census backend=%u mode=%u actualPrefixes=%u normalAllocationSum=%zu exactMinus1=%u physical0/0\n",
        backend, mode, baseline.polls, sites, baseline.polls*6); return 0;
}
