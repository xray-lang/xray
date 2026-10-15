/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_string_worker.c - Owned child string bytes outlive Task and executable owners
 */
#include "xir/xxir_vm.h"
XR_DATA const XrXirProgramSpec worker_string_fixture_program;
XR_DATA const uint32_t worker_string_entry;
static void string_worker_observe(XrXirCallView *view, const XrXirAction *action);
#define XR_WORKER_CONTROL_CUSTOM_CASE {"owned-string", &worker_string_fixture_program, &worker_string_entry}
#define XR_WORKER_CONTROL_ACTION_OBSERVER string_worker_observe
#define XR_WORKER_CONTROL_RESOURCE_MAIN
#include "test_xir_mixed_worker_controls.c"
static XrXirInstance *string_worker_instance;
static XrXirValue *string_worker_pending;
static void string_worker_bytes(const XrXirValue *value) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(value->type == XR_XIR_STRING && xr_xir_string_view(value, &bytes, &length));
    CHECK(length == 8 && !memcmp(bytes, "A\0B!tail", 8));
}
static void string_worker_observe(XrXirCallView *view, const XrXirAction *action) {
    if (view->instance != string_worker_instance || !string_worker_pending || string_worker_pending->type ||
        action->kind != XR_XIR_ACTION_AWAIT_TASK) return;
    CHECK(xr_xir_value_valid(&action->value));
    CHECK(xr_xir_value_copy(&action->value, string_worker_pending) == XR_XIR_VALUE_OK);
}
static void string_worker_finish(XrXirInstance *instance, XrXirCallStatus expected, XrXirValue *returned) {
    unsigned polls = 0; XrXirInstanceResult event = {0};
    do { event = xr_xir_instance_poll_bounded(instance, 1); CHECK(++polls < 32768); }
    while (event.outcome.status == XR_XIR_CALL_READY);
    CHECK(event.outcome.status == expected);
    if (expected == XR_XIR_CALL_RETURNED) {
        CHECK(xr_xir_instance_take_result(instance, returned) == XR_XIR_CALL_RETURNED); string_worker_bytes(returned);
    } else CHECK(!event.outcome.value.type && !event.outcome.value.payload);
}
static void string_worker_capture(XrXirInstance *instance, const WorkerControlCase *oracle, XrXirValue *task) {
    CHECK(!string_worker_instance && !string_worker_pending);
    string_worker_instance = instance; string_worker_pending = task;
    CHECK(xr_xir_instance_start(instance, *oracle->entry, NULL, 0) == XR_XIR_CALL_READY);
    unsigned polls = 0;
    while (!task->type) {
        CHECK(xr_xir_instance_poll_bounded(instance, 1).outcome.status == XR_XIR_CALL_READY && ++polls < 32768);
    }
    string_worker_instance = NULL; string_worker_pending = NULL;
    XrXirCallResult unavailable = {0};
    CHECK(xr_xir_task_copy_outcome(task, &unavailable) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&unavailable));
}
static void string_worker_mode(XrXirProgram *program, const WorkerControlCase *oracle,
    unsigned mode, XrXirValue *task, XrXirValue *returned) {
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instance = NULL; CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirDomain *domain = instance->domain; CHECK(xr_xir_domain_retain(domain));
    string_worker_capture(instance, oracle, task);
    if (mode == 1) CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_CANCEL_REQUESTED);
    if (mode == 2) CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
    if (mode < 3) string_worker_finish(instance, mode ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED, returned);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && !worker_code_releases);
    CHECK(xr_xir_value_valid(task)); const XirTask *held = (const XirTask *)(uintptr_t)task->payload;
    CHECK(!held->call && !held->executor && atomic_load_explicit(&held->state, memory_order_acquire) == XIR_TASK_TERMINAL);
    XrXirDomainBudgetStats cost = xr_xir_domain_budget_stats(domain);
    CHECK(!cost.call_live && !cost.metadata_live && cost.call_allocations == cost.call_frees &&
        cost.metadata_allocations == cost.metadata_frees);
    xr_xir_domain_drop(domain);
}
static void string_worker_copy(const XrXirValue *task, XrXirCallStatus expected, XrXirCallResult *owned) {
    XrXirCallResult occupied = {.status=XR_XIR_CALL_RETURNED, .value={XR_XIR_I64,0,73}}, before = occupied;
    CHECK(xr_xir_task_copy_outcome(task, &occupied) == XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&occupied, &before, sizeof(before)));
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        CHECK(xr_xir_task_copy_outcome(task, &owned[repeat]) == expected && xr_xir_panic_empty(&owned[repeat].panic));
        if (expected == XR_XIR_CALL_RETURNED) string_worker_bytes(&owned[repeat].value);
        else CHECK(expected == XR_XIR_CALL_CANCELLED && !owned[repeat].value.type && !owned[repeat].value.payload);
    }
}
static int string_worker_main(int argc, char **argv) {
    CHECK(argc == 2); (void)worker_control_main;
    unsigned backend = 0; while (backend < 4 && strcmp(argv[1], worker_backends[backend])) ++backend;
    CHECK(backend < 4); const WorkerControlCase *oracle = &worker_control_cases[0];
    XrXirProgram *program = worker_control_program(oracle, backend);
    XrXirValue tasks[4] = {{0}}, root_result[4] = {{0}}, alias = {0};
    for (unsigned mode = 0; mode < 4; ++mode) string_worker_mode(program, oracle, mode, &tasks[mode], &root_result[mode]);
    WorkerControlCode *code = worker_control_code;
    CHECK(code->root_dispatches >= 4 && code->worker_dispatches >= 2);
    if (backend == 2) CHECK(code->vm_dispatches && !code->native_dispatches);
    else if (backend == 3) CHECK(code->native_dispatches && !code->vm_dispatches);
    else CHECK(code->vm_dispatches && code->native_dispatches);
    xr_xir_compile_program_drop(program); CHECK(worker_code_releases == 1 && !worker_control_code);
    CHECK(xr_xir_value_copy(&tasks[1], &alias) == XR_XIR_VALUE_OK);
    XrXirCallResult outcomes[10] = {{0}};
    for (unsigned mode = 0; mode < 4; ++mode) {
        string_worker_copy(&tasks[mode], mode < 2 ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_CANCELLED, &outcomes[2*mode]);
        xr_xir_value_drop(&tasks[mode]);
    }
    string_worker_copy(&alias, XR_XIR_CALL_RETURNED, &outcomes[8]); xr_xir_value_drop(&alias);
    for (unsigned i = 0; i < 10; ++i) {
        if (outcomes[i].status == XR_XIR_CALL_RETURNED) string_worker_bytes(&outcomes[i].value);
        xr_xir_call_result_drop(&outcomes[i]);
    }
    string_worker_bytes(&root_result[0]);
    for (unsigned i = 0; i < 4; ++i) xr_xir_value_drop(&root_result[i]);
    CHECK(!runtime_live && !runtime_bytes); effects_source_owners_free();
    printf("Owned string worker backend=%s four pending Task modes, independent8 bytes A-NUL-B!tail, root-only cancel child returns, stop/free child cancels, late alias/copies/root-result after all code owners, lease1, physical0/0\n", worker_backends[backend]);
    return 0;
}
int main(int argc, char **argv) { return string_worker_main(argc, argv); }
