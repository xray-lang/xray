/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_pending_resources.c - Pending compound Task faults keep one finite Domain
 */
#define XR_TASK_PENDING_RESOURCE_MAIN
#include "test_xir_mixed_task_result_resources.c"

static XrXirCallStatus task_pending_resource_tick(TaskResultRun *run) {
    XrXirInstanceResult event = xr_xir_instance_poll_bounded(run->instance, 1);
    XrXirCallStatus status = event.outcome.status;
    if (status == run->fault) { run->complete = false; run->driven = true; return status; }
    CHECK(status == XR_XIR_CALL_READY || status == XR_XIR_CALL_RETURNED || status == XR_XIR_CALL_CANCELLED);
    if (status == XR_XIR_CALL_CANCELLED) CHECK(!event.outcome.value.type && !event.outcome.value.reserved &&
        !event.outcome.value.payload && !event.outcome.wake && xr_xir_panic_empty(&event.outcome.panic));
    task_late_observe(&run->fixture, run->instance, &run->witness); return status;
}
static bool task_pending_resource_capture(TaskResultRun *run) {
    unsigned polls = 0;
    for (;;) {
        XrXirCallStatus status = task_pending_resource_tick(run); CHECK(++polls < 4096);
        if (!run->complete) return false;
        CHECK(status == XR_XIR_CALL_READY);
        XrXirCall *call = run->instance->call; XrXirTaskWaitToken token = {0};
        if (!call || !call->wait_linked || !call->waiting_task.type || !xr_xir_call_task_wait_token(call, &token)) continue;
        const XirTask *task = (const XirTask *)(uintptr_t)call->waiting_task.payload;
        CHECK(xr_xir_task_storage_valid(task) && task->state != XIR_TASK_TERMINAL && task->executor == run->instance->executor);
        CHECK(task->object.domain == run->domain && task->call && task->activation.call == task->call && task->activation.task == task);
        if (!task->call->next_wake) continue;
        CHECK(task->call->next_wake == 1 && run->witness.yielded_count == 2);
        CHECK(token.binding.owner == run->instance->executor && token.binding.activation == &run->instance->executor->root);
        CHECK(token.binding.generation == run->instance->executor->generation && token.binding.ticket == run->instance->executor->root.ticket);
        CHECK(token.request.kind == XR_XIR_WAIT_TASK && token.request.subject == task);
        XrXirValueStatus copied = xr_xir_value_copy(&call->waiting_task, &run->tasks[1]);
        if (!task_result_value_status(run, copied)) { CHECK(!run->tasks[1].type && !run->tasks[1].payload); return false; }
        CHECK(run->tasks[1].payload == call->waiting_task.payload && run->tasks[1].payload != run->tasks[0].payload);
        XrXirCallResult empty = {0}; XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(run->domain);
        size_t attempts = runtime_attempts;
        CHECK(xr_xir_task_copy_outcome(&run->tasks[1], &empty) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&empty));
        XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(run->domain);
        CHECK(before.work == after.work && before.requested_bytes == after.requested_bytes &&
            before.requested_call_bytes == after.requested_call_bytes && attempts == runtime_attempts);
        return true;
    }
}
static void task_pending_resource_control(TaskResultRun *run, unsigned mode) {
    XrXirCallStatus status = xr_xir_instance_start(run->instance, p20_task_result_entries[0], NULL, 0);
    if (status != XR_XIR_CALL_READY) { CHECK(status == run->fault); run->complete = false; return; }
    if (!task_pending_resource_capture(run)) return;
    if (mode == 1) CHECK(xr_xir_instance_cancel_current(run->instance) == XR_XIR_CALL_CANCEL_REQUESTED);
    if (mode == 2 || mode == 3) {
        status = mode == 2 ? xr_xir_instance_stop(run->instance) : xr_xir_instance_free(run->instance);
        if (mode == 3) run->instance = NULL;
        CHECK(status == XR_XIR_CALL_READY || status == run->fault);
        if (status == run->fault) { run->complete = false; run->driven = true; return; }
    }
    if (mode == 3) return;
    unsigned polls = 0;
    do { status = task_pending_resource_tick(run); CHECK(++polls < 4096); } while (status == XR_XIR_CALL_READY);
    if (!run->complete) return;
    CHECK(status == (mode ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED));
    if (status == XR_XIR_CALL_RETURNED) {
        XrXirValue returned = {0}; CHECK(xr_xir_instance_take_result(run->instance, &returned) == XR_XIR_CALL_RETURNED);
        CHECK(returned.type == run->tasks[1].type && returned.payload == run->tasks[1].payload); xr_xir_value_drop(&returned);
    }
}
static void task_pending_resource_copy(TaskResultRun *run, unsigned round, unsigned mode, bool execution_complete) {
    XrXirValue *value = &run->tasks[round]; if (!value->type) return;
    const XirTask *task = (const XirTask *)(uintptr_t)value->payload;
    CHECK(xr_xir_task_storage_valid(task) && task->state == XIR_TASK_TERMINAL && !task->executor && !task->call);
    CHECK(task->object.domain == run->domain);
    XrXirCallStatus stored = task->outcome.status;
    if (execution_complete) CHECK(stored == (round && mode >= 2 ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED));
    else CHECK(stored == XR_XIR_CALL_RETURNED || stored == XR_XIR_CALL_CANCELLED || stored == run->fault);
    task_late_metadata_only(&run->fixture, xr_xir_value_arena(value));
    XrXirCallResult copies[2] = {{0}}; bool copied[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirCallStatus status = xr_xir_task_copy_outcome(value, &copies[i]);
        copied[i] = !xr_xir_call_result_empty(&copies[i]);
        if (!copied[i]) {
            CHECK(status == run->fault && xr_xir_value_valid(value)); run->complete = false;
        } else {
            CHECK(status == stored && !copies[i].wake && xr_xir_panic_empty(&copies[i].panic));
            if (status != XR_XIR_CALL_RETURNED) CHECK(!copies[i].value.type && !copies[i].value.reserved && !copies[i].value.payload);
        }
    }
    xr_xir_value_drop(value); CHECK(!value->type && !value->reserved && !value->payload);
    if (stored == XR_XIR_CALL_RETURNED) {
        if (copied[0] && copied[1]) task_result_late_cow(run, &copies[0].value, &copies[1].value);
        else for (unsigned i = 0; i < 2; ++i) if (copied[i]) (void)task_result_read(run, &copies[i].value, "cedar");
    }
    for (unsigned i = 0; i < 2; ++i) { xr_xir_call_result_drop(&copies[i]); CHECK(xr_xir_call_result_empty(&copies[i])); }
}
static bool task_pending_resource_operation(unsigned backend, unsigned mode, size_t failure,
    const XrXirInstanceConfig *limits, XrXirDomainBudgetStats *cost, size_t *attempts) {
    CHECK(mode < 4); runtime_fail_at = SIZE_MAX;
    TaskResultRun run = {.fault=limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM, .complete=true};
    XrXirProgram *program = task_late_program(&run.fixture, backend);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    if (limits) config = *limits;
    runtime_attempts = 0; runtime_fail_at = failure;
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &run.instance);
    run.complete = status == XR_XIR_CALL_READY; CHECK(run.complete || (status == run.fault && !run.instance));
    if (run.instance) { run.domain = run.instance->domain; CHECK(xr_xir_domain_retain(run.domain)); }
    xr_xir_compile_program_drop(program); CHECK(run.instance ? !run.fixture.releases : run.fixture.releases == 1);
    if (run.complete) { task_result_round(&run, 0); task_result_cost(&run, false); }
    if (run.complete) {
        CHECK(run.instance->domain == run.domain); task_pending_resource_control(&run, mode); task_result_cost(&run, true);
    }
    if (run.complete) {
        CHECK(run.witness.root_providers == (task_late_native(backend, true) ? 2u : 1u));
        CHECK(run.witness.worker_providers == (task_late_native(backend, false) ? 2u : 1u) && run.witness.yielded_count == 2);
    }
    if (run.instance) {
        XrXirCallStatus expected = run.driven || run.instance->budget.exhausted ? run.fault : XR_XIR_CALL_READY;
        status = xr_xir_instance_free(run.instance); run.instance = NULL;
        if (status != expected) fprintf(stderr, "Pending resource retirement backend=%u mode=%u site=%zu status=%u expected=%u\n",
            backend, mode, failure, status, expected);
        CHECK(status == expected);
    }
    CHECK(run.fixture.releases == 1 && !run.fixture.vm && !run.fixture.entries);
    const bool execution_complete = run.complete;
    for (unsigned round = 0; round < 2; ++round) task_pending_resource_copy(&run, round, mode, execution_complete);
    if (run.domain) {
        task_result_cost(&run, false); XrXirDomainBudgetStats final = xr_xir_domain_budget_stats(run.domain);
        CHECK(!final.call_live && !final.metadata_live && final.call_allocations == final.call_frees &&
            final.metadata_allocations == final.metadata_frees && xr_xir_domain_stats(run.domain).live_bytes == sizeof(*run.domain));
        CHECK(final.requested_bytes <= config.requested_value_limit && final.requested_call_bytes <= config.requested_call_limit && final.work <= config.work_limit);
        if (cost) *cost = final;
        xr_xir_domain_drop(run.domain);
    }
    if (attempts) *attempts = runtime_attempts;
    runtime_fail_at = SIZE_MAX;
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes); return run.complete;
}
static size_t task_pending_resource_axes(unsigned backend, unsigned mode) {
    XrXirDomainBudgetStats cost = {0}; size_t sites = 0;
    CHECK(task_pending_resource_operation(backend, mode, SIZE_MAX, NULL, &cost, &sites) && sites && sites < 20000);
    const uint64_t axes[] = {cost.requested_bytes, cost.requested_call_bytes, cost.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY && axes[axis]);
        if (!axis) config.requested_value_limit = axes[axis]-minus;
        else if (axis == 1) config.requested_call_limit = axes[axis]-minus;
        else config.work_limit = axes[axis]-minus;
        XrXirDomainBudgetStats spent = {0}; bool complete = task_pending_resource_operation(backend, mode, SIZE_MAX, &config, &spent, NULL);
        CHECK(complete == !minus);
        printf("Task pending resource axis backend=%u mode=%u axis=%u minus=%u complete=%u value=%llu call=%llu work=%llu\n",
            backend, mode, axis, minus, (unsigned)complete, (unsigned long long)spent.requested_bytes,
            (unsigned long long)spent.requested_call_bytes, (unsigned long long)spent.work);
    }
    printf("Task pending resources backend=%u mode=%u sites=%zu value=%llu call=%llu work=%llu actualWaitValue sameOriginalInstanceDomainTwoRounds codeDeadBeforeLateCopyCOW physical0/0\n",
        backend, mode, sites, (unsigned long long)axes[0], (unsigned long long)axes[1], (unsigned long long)axes[2]); return sites;
}
static size_t task_pending_resource_number(const char *text) {
    char *end = NULL; unsigned long number = strtoul(text, &end, 10);
    CHECK(text[0] && end && !*end && number < 20000); return (size_t)number;
}
#ifndef XR_TASK_CANCEL_PREFIX_MAIN
int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 5); (void)task_late_graph; (void)task_result_axes;
    static const char *const names[] = {"0", "1", "vm", "native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], names[backend])) ++backend;
    CHECK(backend < 4);
    if (!strcmp(argv[2], "axes")) {
        CHECK(argc == 3);
        for (unsigned mode = 0; mode < 4; ++mode) (void)task_pending_resource_axes(backend, mode);
        return 0;
    }
    CHECK(argc == 5 && !strcmp(argv[2], "fi"));
    unsigned mode = (unsigned)task_pending_resource_number(argv[3]);
    size_t expected = task_pending_resource_number(argv[4]); CHECK(mode < 4);
    size_t sites = task_pending_resource_axes(backend, mode); CHECK(sites == expected);
    for (size_t site = 0; site < sites; ++site) {
        size_t attempts = 0;
        CHECK(!task_pending_resource_operation(backend, mode, site, NULL, NULL, &attempts) && attempts > site);
    }
    printf("Task pending complete runtime FI backend=%u mode=%u sites=%zu everyOrdinalHit typedOOM sameOriginalDomain finalCodeDead physical0/0\n",
        backend, mode, sites);
    return 0;
}
#endif
