/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_worker_retained.c - Real child Task outcomes outlive executable owners
 */
#include "xir/xxir_call.h"
static void worker_retained_observe(XrXirCallView *view, const XrXirAction *action);
#define XR_WORKER_CONTROL_ACTION_OBSERVER worker_retained_observe
#define XR_WORKER_CONTROL_RESOURCE_MAIN
#include "test_xir_mixed_worker_controls.c"
static XrXirInstance *worker_retained_instance;
static XrXirValue *worker_retained_target;
static unsigned worker_retained_pending;
static void worker_retained_observe(XrXirCallView *view, const XrXirAction *action) {
    if (view->instance != worker_retained_instance || !worker_retained_target || worker_retained_target->type ||
        action->kind != XR_XIR_ACTION_AWAIT_TASK) return;
    CHECK(xr_xir_value_valid(&action->value));
    CHECK(xr_xir_value_copy(&action->value, worker_retained_target) == XR_XIR_VALUE_OK);
}
static void worker_retained_expect_pending(const XrXirValue *task) {
    XrXirCallResult outcome = {0}; XrXirCallStatus status = xr_xir_task_copy_outcome(task, &outcome);
    CHECK(status == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&outcome));
    ++worker_retained_pending;
}
static void worker_retained_finish(XrXirInstance *instance, XrXirCallStatus expected) {
    unsigned ticks = 0; XrXirInstanceResult event = {0};
    do { event = xr_xir_instance_poll_bounded(instance, 1); CHECK(++ticks < 32768); }
    while (event.outcome.status == XR_XIR_CALL_READY);
    CHECK(event.outcome.status == expected);
    if (expected == XR_XIR_CALL_RETURNED) {
        XrXirValue result = {0}; CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
        CHECK(result.type == XR_XIR_I64 && result.payload == 7); xr_xir_value_drop(&result);
    } else CHECK(!event.outcome.value.type && !event.outcome.value.payload);
}
static XrXirCallStatus worker_retained_operation(XrXirProgram *program, const WorkerControlCase *oracle,
    unsigned mode, XrXirValue *retained) {
    CHECK(!worker_retained_instance && !worker_retained_target && mode < 4);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instance = NULL; CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirDomain *domain = instance->domain; CHECK(xr_xir_domain_retain(domain));
    worker_retained_instance = instance; worker_retained_target = retained;
    CHECK(xr_xir_instance_start(instance, *oracle->entry, NULL, 0) == XR_XIR_CALL_READY);
    unsigned ticks = 0;
    while (!retained->type) {
        XrXirInstanceResult event = xr_xir_instance_poll_bounded(instance, 1); CHECK(++ticks < 32768);
        CHECK(event.outcome.status == XR_XIR_CALL_READY || event.outcome.status == XR_XIR_CALL_RETURNED);
        if (event.outcome.status == XR_XIR_CALL_RETURNED) CHECK(retained->type);
    }
    worker_retained_instance = NULL; worker_retained_target = NULL;
    worker_retained_expect_pending(retained);
    XrXirCallStatus expected = mode < 2 ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_CANCELLED;
    if (mode == 1) CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_CANCEL_REQUESTED);
    if (mode == 2) CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
    if (mode < 3) worker_retained_finish(instance, mode ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && !worker_code_releases);
    CHECK(xr_xir_value_valid(retained));
    const XirTask *task = (const XirTask *)(uintptr_t)retained->payload;
    CHECK(!task->executor && !task->call && atomic_load_explicit(&task->state, memory_order_acquire) == XIR_TASK_TERMINAL);
    XrXirDomainBudgetStats cost = xr_xir_domain_budget_stats(domain);
    CHECK(!cost.call_live && !cost.metadata_live && cost.call_allocations == cost.call_frees &&
        cost.metadata_allocations == cost.metadata_frees);
    xr_xir_domain_drop(domain); return expected;
}
static void worker_retained_outcome(const XrXirValue *task, XrXirCallStatus expected, XrXirCallResult *owned) {
    XrXirCallResult occupied = {.status=XR_XIR_CALL_RETURNED, .value={XR_XIR_I64,0,73}}, before = occupied;
    CHECK(xr_xir_task_copy_outcome(task, &occupied) == XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&occupied, &before, sizeof(before)));
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        XrXirCallResult outcome = {0}; CHECK(xr_xir_task_copy_outcome(task, &outcome) == expected);
        CHECK(xr_xir_panic_empty(&outcome.panic));
        if (expected == XR_XIR_CALL_RETURNED) CHECK(outcome.value.type == XR_XIR_I64 && outcome.value.payload == 7);
        else CHECK(expected == XR_XIR_CALL_CANCELLED && !outcome.value.type && !outcome.value.payload);
        CHECK(xr_xir_call_result_copy(&outcome, &owned[repeat]) == XR_XIR_VALUE_OK);
        xr_xir_call_result_drop(&outcome);
    }
}
static int worker_retained_main(int argc, char **argv) {
    CHECK(argc == 3); (void)worker_control_main;
    const WorkerControlCase *oracle = NULL; unsigned backend = 0;
    for (unsigned i = 0; i < 7; ++i) if (!strcmp(argv[1], worker_control_cases[i].name)) oracle = &worker_control_cases[i];
    while (backend < 4 && strcmp(argv[2], worker_backends[backend])) ++backend;
    CHECK(oracle && backend < 4); XrXirProgram *program = worker_control_program(oracle, backend);
    XrXirValue tasks[4] = {{0}}; XrXirCallStatus expected[4];
    for (unsigned mode = 0; mode < 4; ++mode) expected[mode] = worker_retained_operation(program, oracle, mode, &tasks[mode]);
    xr_xir_compile_program_drop(program); CHECK(worker_code_releases == 1 && !worker_control_code);
    XrXirCallResult outcomes[8] = {{0}};
    for (unsigned mode = 0; mode < 4; ++mode) {
        worker_retained_outcome(&tasks[mode], expected[mode], &outcomes[2*mode]); xr_xir_value_drop(&tasks[mode]);
    }
    for (unsigned i = 0; i < 8; ++i) {
        CHECK(outcomes[i].status == expected[i/2]);
        if (outcomes[i].status == XR_XIR_CALL_RETURNED) CHECK(outcomes[i].value.type == XR_XIR_I64 && outcomes[i].value.payload == 7);
        xr_xir_call_result_drop(&outcomes[i]);
    }
    CHECK(worker_retained_pending == 4 && !runtime_live && !runtime_bytes); effects_source_owners_free();
    printf("Worker retained case=%s backend=%s captured pending=%u, root-cancel preserves child7, stop/free pending-cancel, task/outcome copies after all code owners, lease1, physical0/0\n",
        oracle->name, worker_backends[backend], worker_retained_pending);
    return 0;
}
int main(int argc, char **argv) { return worker_retained_main(argc, argv); }
