/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_worker_waits.c - Captured child wake tokens preserve ownership and cancellation
 */
#include "xir/xxir_vm.h"
XR_DATA const XrXirProgramSpec worker_wait_0_program, worker_wait_1_program;
XR_DATA const uint32_t worker_wait_0_entry, worker_wait_1_entry;
static void worker_wait_observe(XrXirCallView *view, const XrXirAction *action);
#define XR_WORKER_CONTROL_CUSTOM_CASE {"yield",&worker_wait_0_program,&worker_wait_0_entry}, {"timer",&worker_wait_1_program,&worker_wait_1_entry}
#define XR_WORKER_CONTROL_ACTION_OBSERVER worker_wait_observe
#define XR_WORKER_CONTROL_RESOURCE_MAIN
#include "test_xir_mixed_worker_controls.c"
typedef struct WorkerWaitToken { uint64_t epoch, wake; XrXirWaitRequest request; } WorkerWaitToken;
static XrXirInstance *worker_wait_instance;
static XrXirValue *worker_wait_task;
static unsigned worker_wait_yields;
static void worker_wait_bytes(const XrXirValue *value) {
    const char *text = NULL; size_t length = 0;
    CHECK(value->type == XR_XIR_STRING && xr_xir_string_view(value, &text, &length));
    CHECK(length == 8 && !memcmp(text, "A\0B!tail", 8));
}
static void worker_wait_observe(XrXirCallView *view, const XrXirAction *action) {
    if (view->instance == worker_wait_instance && action->kind == XR_XIR_ACTION_SUSPEND) ++worker_wait_yields;
    if (view->instance != worker_wait_instance || !worker_wait_task || worker_wait_task->type || action->kind != XR_XIR_ACTION_AWAIT_TASK) return;
    CHECK(xr_xir_value_copy(&action->value, worker_wait_task) == XR_XIR_VALUE_OK);
}
static XrXirInstance *worker_wait_new(XrXirProgram *program) {
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instance = NULL; CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY); return instance;
}
static WorkerWaitToken worker_wait_capture(XrXirInstance *instance, const WorkerControlCase *oracle, XrXirValue *task) {
    CHECK(!worker_wait_instance && !worker_wait_task);
    CHECK(xr_xir_instance_start(instance, *oracle->entry, NULL, 0) == XR_XIR_CALL_READY);
    worker_wait_instance = instance; worker_wait_task = task;
    XrXirInstanceResult event = {0}; unsigned polls = 0;
    do { event = xr_xir_instance_poll_bounded(instance, 1); CHECK(++polls < 32768); }
    while (event.outcome.status == XR_XIR_CALL_READY);
    worker_wait_instance = NULL; worker_wait_task = NULL;
    CHECK(event.outcome.status == XR_XIR_CALL_SUSPENDED && event.epoch && event.outcome.wake && task->type);
    XrXirCallResult unavailable = {0};
    CHECK(xr_xir_task_copy_outcome(task, &unavailable) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&unavailable));
    WorkerWaitToken token = {event.epoch, event.outcome.wake, {0}};
    CHECK(xr_xir_instance_wait_request(instance, token.epoch, token.wake, &token.request) == XR_XIR_CALL_READY);
    bool timer = !strcmp(oracle->name, "timer");
    CHECK(token.request.kind == (uint32_t)(timer ? XR_XIR_WAIT_TIMER_MS : XR_XIR_WAIT_YIELD) && token.request.after_ms == (timer ? 7u : 0u));
    CHECK(!token.request.reserved && !token.request.subject && !token.request.generation && !token.request.ticket);
    return token;
}
static void worker_wait_reject(XrXirInstance *instance, uint64_t epoch, uint64_t wake) {
    XrXirWaitRequest occupied, original; memset(&occupied, 0xA5, sizeof(occupied)); original = occupied;
    XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(instance->domain); WorkerControlCode code = *worker_control_code;
    size_t attempts = runtime_attempts;
    CHECK(xr_xir_instance_wait_request(instance, epoch, wake, &occupied) == XR_XIR_CALL_BAD_STATE && !memcmp(&occupied, &original, sizeof(original)));
    CHECK(xr_xir_instance_resume(instance, epoch, wake) == XR_XIR_CALL_BAD_STATE);
    XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(instance->domain);
    CHECK(before.work == after.work && before.requested_bytes == after.requested_bytes && before.requested_call_bytes == after.requested_call_bytes);
    CHECK(runtime_attempts == attempts && !memcmp(&code, worker_control_code, sizeof(code)));
}
static void worker_wait_finish(XrXirInstance *instance, XrXirCallStatus expected, XrXirValue *returned) {
    XrXirInstanceResult event = {0}; unsigned polls = 0;
    do { event = xr_xir_instance_poll_bounded(instance, 1); CHECK(++polls < 32768); }
    while (event.outcome.status == XR_XIR_CALL_READY);
    CHECK(event.outcome.status == expected);
    if (expected == XR_XIR_CALL_RETURNED) {
        CHECK(xr_xir_instance_take_result(instance, returned) == XR_XIR_CALL_RETURNED); worker_wait_bytes(returned);
    } else {
        XrXirValue empty = {0}; CHECK(expected == XR_XIR_CALL_CANCELLED && !event.outcome.value.type && !event.outcome.value.payload);
        CHECK(xr_xir_instance_take_result(instance, &empty) == XR_XIR_CALL_BAD_STATE && !empty.type && !empty.payload);
    }
}
static WorkerWaitToken worker_wait_warm(XrXirInstance *instance, const WorkerControlCase *oracle) {
    XrXirValue task = {0}, value = {0}; WorkerWaitToken token = worker_wait_capture(instance, oracle, &task);
    CHECK(xr_xir_instance_resume(instance, token.epoch, token.wake) == XR_XIR_CALL_READY);
    worker_wait_reject(instance, token.epoch, token.wake); worker_wait_finish(instance, XR_XIR_CALL_RETURNED, &value);
    xr_xir_value_drop(&value); xr_xir_value_drop(&task); return token;
}
static void worker_wait_restart(XrXirInstance *instance, const WorkerControlCase *oracle, const WorkerWaitToken *old) {
    XrXirValue task = {0}, value = {0}; WorkerWaitToken next = worker_wait_capture(instance, oracle, &task);
    CHECK(next.epoch > old->epoch && next.wake == old->wake);
    worker_wait_reject(instance, old->epoch, old->wake);
    CHECK(xr_xir_instance_resume(instance, next.epoch, next.wake) == XR_XIR_CALL_READY);
    worker_wait_reject(instance, next.epoch, next.wake); worker_wait_finish(instance, XR_XIR_CALL_RETURNED, &value);
    xr_xir_value_drop(&value); xr_xir_value_drop(&task);
}
static void worker_wait_peer(XrXirProgram *program, XrXirInstance *victim,
    const WorkerControlCase *oracle, const WorkerWaitToken *current, const WorkerWaitToken *old) {
    XrXirInstance *peer = worker_wait_new(program); XrXirValue task = {0}, value = {0};
    WorkerWaitToken other = worker_wait_capture(peer, oracle, &task);
    CHECK(current->epoch > other.epoch && current->wake == other.wake && current->wake == old->wake);
    worker_wait_reject(peer, current->epoch, current->wake);
    worker_wait_reject(victim, other.epoch, other.wake); worker_wait_reject(victim, old->epoch, old->wake);
    XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(victim->domain);
    CHECK(xr_xir_instance_resume(peer, other.epoch, other.wake) == XR_XIR_CALL_READY);
    worker_wait_reject(peer, other.epoch, other.wake); worker_wait_finish(peer, XR_XIR_CALL_RETURNED, &value);
    CHECK(xr_xir_instance_free(peer) == XR_XIR_CALL_READY); xr_xir_value_drop(&value); xr_xir_value_drop(&task);
    XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(victim->domain);
    CHECK(before.work == after.work && before.requested_bytes == after.requested_bytes && before.requested_call_bytes == after.requested_call_bytes);
    XrXirWaitRequest request = {0}; CHECK(xr_xir_instance_wait_request(victim, current->epoch, current->wake, &request) == XR_XIR_CALL_READY);
    CHECK(!memcmp(&request, &current->request, sizeof(request)));
}
static void worker_wait_mode(XrXirProgram *program, const WorkerControlCase *oracle,
    unsigned mode, XrXirValue *task, XrXirValue *returned) {
    XrXirInstance *instance = worker_wait_new(program); XrXirDomain *domain = instance->domain; CHECK(xr_xir_domain_retain(domain));
    XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(domain); WorkerWaitToken old = {0};
    if (!mode) old = worker_wait_warm(instance, oracle);
    WorkerWaitToken token = worker_wait_capture(instance, oracle, task);
    worker_wait_reject(instance, token.epoch+1, token.wake); worker_wait_reject(instance, token.epoch, token.wake+1);
    if (!mode) worker_wait_peer(program, instance, oracle, &token, &old);
    if (mode == 1) {
        CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(xr_xir_instance_start(instance, *oracle->entry, NULL, 0) == XR_XIR_CALL_BUSY);
        XrXirWaitRequest request = {0}; CHECK(xr_xir_instance_wait_request(instance, token.epoch, token.wake, &request) == XR_XIR_CALL_READY);
        CHECK(!memcmp(&request, &token.request, sizeof(request)));
    }
    if (mode < 2) {
        CHECK(xr_xir_instance_resume(instance, token.epoch, token.wake) == XR_XIR_CALL_READY);
        worker_wait_reject(instance, token.epoch, token.wake);
        worker_wait_finish(instance, mode ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED, returned);
        if (mode == 1) worker_wait_restart(instance, oracle, &token);
    } else if (mode == 2) {
        CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY); worker_wait_reject(instance, token.epoch, token.wake);
        worker_wait_finish(instance, XR_XIR_CALL_CANCELLED, returned);
        CHECK(xr_xir_instance_start(instance, *oracle->entry, NULL, 0) == XR_XIR_CALL_BAD_STATE);
    }
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && !worker_code_releases);
    CHECK(xr_xir_value_valid(task)); const XirTask *held = (const XirTask *)(uintptr_t)task->payload;
    CHECK(!held->executor && !held->call && atomic_load_explicit(&held->state, memory_order_acquire) == XIR_TASK_TERMINAL);
    XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(domain);
    CHECK(after.work >= before.work && after.requested_bytes >= before.requested_bytes && after.requested_call_bytes >= before.requested_call_bytes);
    CHECK(!after.call_live && !after.metadata_live && after.call_allocations == after.call_frees && after.metadata_allocations == after.metadata_frees);
    xr_xir_domain_drop(domain);
}
static void worker_yield_mode(XrXirProgram *program, const WorkerControlCase *oracle,
    unsigned mode, XrXirValue *task, XrXirValue *returned) {
    XrXirInstance *instance = worker_wait_new(program); unsigned yields = worker_wait_yields;
    XrXirDomain *domain = instance->domain; CHECK(xr_xir_domain_retain(domain));
    CHECK(!worker_wait_instance && !worker_wait_task); worker_wait_instance = instance; worker_wait_task = task;
    CHECK(xr_xir_instance_start(instance, *oracle->entry, NULL, 0) == XR_XIR_CALL_READY);
    unsigned polls = 0;
    while (!task->type) {
        CHECK(xr_xir_instance_poll_bounded(instance, 1).outcome.status == XR_XIR_CALL_READY && ++polls < 32768);
    }
    XrXirCallResult unavailable = {0};
    CHECK(xr_xir_task_copy_outcome(task, &unavailable) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&unavailable));
    worker_wait_reject(instance, instance->epoch, 1);
    if (mode == 1) CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_CANCEL_REQUESTED);
    if (mode == 2) CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
    if (mode < 3) worker_wait_finish(instance, mode ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED, returned);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY); worker_wait_instance = NULL; worker_wait_task = NULL;
    if (mode < 2) CHECK(worker_wait_yields == yields+1);
    else CHECK(worker_wait_yields <= yields+1);
    CHECK(!worker_code_releases && xr_xir_value_valid(task)); const XirTask *held = (const XirTask *)(uintptr_t)task->payload;
    CHECK(!held->executor && !held->call && atomic_load_explicit(&held->state, memory_order_acquire) == XIR_TASK_TERMINAL);
    XrXirDomainBudgetStats cost = xr_xir_domain_budget_stats(domain);
    CHECK(!cost.call_live && !cost.metadata_live && cost.call_allocations == cost.call_frees && cost.metadata_allocations == cost.metadata_frees);
    xr_xir_domain_drop(domain);
}
static int worker_wait_main(int argc, char **argv) {
    CHECK(argc == 3); (void)worker_control_main; const WorkerControlCase *oracle = NULL; unsigned backend = 0;
    for (unsigned i = 0; i < 2; ++i) if (!strcmp(argv[1], worker_control_cases[i].name)) oracle = &worker_control_cases[i];
    while (backend < 4 && strcmp(argv[2], worker_backends[backend])) ++backend;
    CHECK(oracle && backend < 4); XrXirProgram *program = worker_control_program(oracle, backend);
    XrXirValue tasks[4] = {{0}}, returned[4] = {{0}};
    bool timer = !strcmp(oracle->name, "timer");
    for (unsigned mode = 0; mode < 4; ++mode) {
        if (timer) worker_wait_mode(program, oracle, mode, &tasks[mode], &returned[mode]);
        else worker_yield_mode(program, oracle, mode, &tasks[mode], &returned[mode]);
    }
    WorkerControlCode *code = worker_control_code;
    CHECK(code->root_dispatches >= (timer ? 7u : 4u) && code->worker_dispatches >= (timer ? 7u : 4u));
    if (backend == 2) CHECK(code->vm_dispatches && !code->native_dispatches);
    else if (backend == 3) CHECK(code->native_dispatches && !code->vm_dispatches);
    else CHECK(code->native_dispatches && code->vm_dispatches);
    xr_xir_compile_program_drop(program); CHECK(worker_code_releases == 1 && !worker_control_code);
    XrXirCallResult outcomes[8] = {{0}};
    for (unsigned mode = 0; mode < 4; ++mode) for (unsigned repeat = 0; repeat < 2; ++repeat) {
        XrXirCallResult *out = &outcomes[2*mode+repeat]; XrXirCallStatus expected = mode < 2 ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_CANCELLED;
        CHECK(xr_xir_task_copy_outcome(&tasks[mode], out) == expected && xr_xir_panic_empty(&out->panic));
        if (mode < 2) worker_wait_bytes(&out->value); else CHECK(!out->value.type && !out->value.payload);
    }
    for (unsigned mode = 0; mode < 4; ++mode) xr_xir_value_drop(&tasks[mode]);
    for (unsigned i = 0; i < 8; ++i) { if (outcomes[i].status == XR_XIR_CALL_RETURNED) worker_wait_bytes(&outcomes[i].value); xr_xir_call_result_drop(&outcomes[i]); }
    worker_wait_bytes(&returned[0]); for (unsigned i = 0; i < 4; ++i) xr_xir_value_drop(&returned[i]);
    CHECK(!runtime_live && !runtime_bytes); effects_source_owners_free();
    printf("Worker wait case=%s backend=%s %s, root-only cancel child returns, stop/free child cancels, late owned8bytes after code lease1/Task death, physical0/0\n",
        oracle->name, worker_backends[backend], timer ? "actual child timer7 token, reused wake1/new epoch, wrong/duplicate/old/cross-epoch rejects no fees/dispatch, child token preserved across rootcancel/restart" :
        "actual child yield automatically resumes inside executor, no host wake advertised, pending Task owned and no issued external signal");
    return 0;
}
int main(int argc, char **argv) { return worker_wait_main(argc, argv); }
