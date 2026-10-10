/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * task_root_activation_cases.h - Real root ownership and bounded child completion
 *
 * KEY CONCEPT:
 *   A root retains its own outcome while the executor completes accepted children.
 */
#ifndef TASK_ROOT_ACTIVATION_CASES_H
#define TASK_ROOT_ACTIVATION_CASES_H
typedef struct TaskRootWitness {
    XrXirTaskExecutor *executor;
    XrXirValue child;
    XrXirCallAccounting accounting;
    uint32_t mode, callbacks;
} TaskRootWitness;
static XrXirAction task_root_entry(XrXirCallView *view) {
    TaskRootWitness *witness = (TaskRootWitness *)view->environment;
    uint32_t *phase = view->state;
    ++witness->callbacks;
    bool child = true;
    CHECK(xr_xir_task_executor_view_member(witness->executor, view, &child) && !child);
    XrXirCallView forged = *view;
    CHECK(!xr_xir_task_executor_view_member(witness->executor, &forged, &child));
    if (!(*phase)++) {
        XrXirValue argument = {XR_XIR_I64, 0, 21};
        XrXirCallRequest request = {.entry = 1, .arguments = &argument, .count = 1,
            .cell_role = (XrXirCellRoleResolver)0, .cell_context = NULL};
        CHECK(xr_xir_task_executor_spawn_from_view(witness->executor, view, (XrXirType)256,
            &request, &witness->child) == XR_XIR_CALL_READY);
        CHECK(xr_xir_task_executor_spawn_from_view(witness->executor, &forged, (XrXirType)256,
            &request, &(XrXirValue){0}) == XR_XIR_CALL_BUSY);
        if (witness->mode == 1) return task_test_action(XR_XIR_ACTION_AWAIT_TASK, witness->child);
        if (witness->mode == 3) return task_test_action(XR_XIR_ACTION_TIMER, (XrXirValue){XR_XIR_I64, 0, 7});
    } else if (witness->mode == 1) {
        CHECK(view->inbox.status == XR_XIR_CALL_RETURNED && view->inbox.value.payload == 42);
        if (*phase == 2) return task_test_action(XR_XIR_ACTION_AWAIT_TASK, witness->child);
    }
    return task_test_action(XR_XIR_ACTION_RETURN, (XrXirValue){XR_XIR_I64, 0, 42});
}
static XrXirCall *task_root_call(TaskFixture *fixture, XrXirDomain *domain,
    const XrXirCallEntry entries[7], XrXirCallAccounting *accounting) {
    XrXirCallConfig config;
    CHECK(xr_xir_call_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.entries = entries; config.entry_count = 7; config.accounting = accounting;
    config.byte_limit = fixture->budget.byte_limit; config.poll_limit = fixture->budget.resume_limit;
    config.admission = (XrXirValueAdmission){fixture->arena, domain, NULL, NULL, 1000000, 1048576};
    XrXirCallRequest request = {.entry = 3, .arguments = NULL, .count = 0,
        .cell_role = (XrXirCellRoleResolver)0, .cell_context = NULL}; XrXirCall *call = NULL;
    CHECK(xr_xir_call_new_budgeted(&config, &request, &fixture->budget, &call) == XR_XIR_CALL_READY);
    return call;
}
static void task_root_normal(void) {
    for (uint32_t mode = 0; mode < 4; ++mode) {
        TaskFixture fixture = {0}; task_fixture_new(&fixture);
        TaskRootWitness witness = {.mode = mode};
        XrXirCallEntry entries[7]; memcpy(entries, fixture.program->entries, sizeof(entries));
        entries[3].resume = task_root_entry; entries[3].state_bytes = sizeof(uint32_t);
        entries[3].environment = &witness; entries[3].release = NULL;
        if (mode == 2) entries[1].resume = task_test_failure;
        XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
        XrXirTaskExecutorConfig config = task_config(&fixture, domain);
        config.entries = entries; config.entry_count = 7;
        config.admission = (XrXirValueAdmission){fixture.arena, domain, NULL, NULL, 1000000, 1048576};
        CHECK(task_test_executor_new(&config, &witness.executor) == XR_XIR_CALL_READY);
        XrXirCall *root = task_root_call(&fixture, domain, entries, &witness.accounting);
        CHECK(xr_xir_task_executor_attach_root(witness.executor, root, 1) == XR_XIR_CALL_READY);
        CHECK(xr_xir_task_executor_attach_root(witness.executor, root, 1) == XR_XIR_CALL_BUSY);
        XrXirCallResult result = {0}; bool retained_root = false, observed_wait = false;
        for (uint32_t step = 0; step < 64; ++step) {
            result = xr_xir_task_executor_poll_root(witness.executor, 1);
            if (xr_xir_call_state(root) == XR_XIR_CALL_RETURNED && witness.executor->active) {
                CHECK(result.status == XR_XIR_CALL_READY); retained_root = true;
            }
            if (result.status == XR_XIR_CALL_SUSPENDED) {
                CHECK(mode == 3); XrXirWaitRequest wait = {0};
                CHECK(xr_xir_task_executor_root_wait(witness.executor, 1, result.wake, &wait) == XR_XIR_CALL_READY);
                CHECK(wait.kind == XR_XIR_WAIT_TIMER_MS && wait.after_ms == 7 && !wait.reserved &&
                    !wait.subject && !wait.generation && !wait.ticket);
                CHECK(xr_xir_task_executor_resume_root(witness.executor, 2, result.wake) == XR_XIR_CALL_BAD_STATE);
                CHECK(xr_xir_task_executor_resume_root(witness.executor, 1, result.wake) == XR_XIR_CALL_READY);
                CHECK(xr_xir_task_executor_resume_root(witness.executor, 1, result.wake) == XR_XIR_CALL_BAD_STATE);
                observed_wait = true; continue;
            }
            if (result.status != XR_XIR_CALL_READY) break;
        }
        CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.payload == 42 &&
            xr_xir_task_executor_root_idle(witness.executor));
        CHECK(mode == 1 ? witness.callbacks == 3 : mode == 3 ? witness.callbacks == 2 : witness.callbacks == 1);
        CHECK(mode != 0 || retained_root); CHECK(mode != 3 || observed_wait);
        XrXirCallResult child = {0};
        CHECK(xr_xir_task_copy_outcome(&witness.child, &child) == (mode == 2 ? XR_XIR_CALL_BOUNDS : XR_XIR_CALL_RETURNED));
        CHECK(mode == 2 ? child.panic.detail.index == -7 && child.panic.detail.length == 2 : child.value.payload == 42);
        xr_xir_call_result_drop(&child);
        task_fixture_drop(&fixture); CHECK(!fixture.witness.code_releases);
        CHECK(xr_xir_task_executor_free(witness.executor, NULL) == XR_XIR_CALL_READY);
        CHECK(xr_xir_call_free(root) == XR_XIR_CALL_READY); xr_xir_domain_drop(domain);
        CHECK(xr_xir_task_copy_outcome(&witness.child, &child) == (mode == 2 ? XR_XIR_CALL_BOUNDS : XR_XIR_CALL_RETURNED));
        xr_xir_call_result_drop(&child); xr_xir_value_drop(&witness.child);
        CHECK(fixture.witness.code_releases == 1 && !runtime_live && !runtime_bytes);
        printf("Task real root mode%u stored42/drain, repeated await or ordinarychild failure isolation, exact external epoch, physical0\n", mode);
    }
}
typedef struct TaskWaitPanicWitness { XrXirValue subject; uint32_t callbacks, flags; XrXirCallStatus reason; } TaskWaitPanicWitness;
static XrXirAction task_wait_panic_entry(XrXirCallView *view) {
    TaskWaitPanicWitness *witness = (TaskWaitPanicWitness *)view->environment;
    if (!witness->callbacks++) {
        XrXirAction action = task_test_action(XR_XIR_ACTION_AWAIT_TASK, witness->subject);
        action.flags = witness->flags; return action;
    }
    CHECK(witness->flags == XR_XIR_ACTION_PROTECTED && view->inbox.status == XR_XIR_CALL_BOUNDS &&
        view->inbox.panic.detail.index == -7 && view->inbox.panic.detail.length == 2);
    return task_test_action(XR_XIR_ACTION_RETURN, (XrXirValue){XR_XIR_I64, 0, 42});
}
static XrXirAction task_wait_fault_subject(XrXirCallView *view) {
    TaskWaitPanicWitness *witness = (TaskWaitPanicWitness *)view->environment;
    return task_test_action(XR_XIR_ACTION_FAULT, (XrXirValue){XR_XIR_I64, 0, witness->reason});
}
static void task_wait_panic_protocol(void) {
    for (uint32_t mode = 0; mode < 6; ++mode) {
        TaskFixture fixture = {0}; task_fixture_new(&fixture);
        TaskWaitPanicWitness witness = {.flags = mode == 1 ? XR_XIR_ACTION_PROTECTED : mode == 2 ? XR_XIR_ACTION_CLEANUP : 0,
            .reason = mode == 3 ? XR_XIR_CALL_OOM : mode == 4 ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_HOST_ERROR};
        XrXirCallEntry entries[7]; memcpy(entries, fixture.program->entries, sizeof(entries));
        entries[1].resume = task_wait_panic_entry; entries[1].environment = &witness; entries[1].release = NULL;
        if (mode >= 3) { entries[3].resume = task_wait_fault_subject; entries[3].environment = &witness; entries[3].release = NULL; }
        XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
        XrXirTaskExecutorConfig config = task_config(&fixture, domain);
        config.entries = entries; config.entry_count = 7;
        config.admission = (XrXirValueAdmission){fixture.arena, domain, NULL, NULL, 1000000, 1048576};
        XrXirTaskExecutor *executor = NULL; CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
        XrXirCallRequest subject = {.entry = 3, .arguments = NULL, .count = 0,
            .cell_role = (XrXirCellRoleResolver)0, .cell_context = NULL};
        CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &subject, &witness.subject) == XR_XIR_CALL_READY);
        XrXirValue marker = {XR_XIR_I64, 0, 1}, handle = {0}; XrXirCallRequest request = {.entry = 1, .arguments = &marker, .count = 1,
            .cell_role = (XrXirCellRoleResolver)0, .cell_context = NULL};
        CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &request, &handle) == XR_XIR_CALL_READY);
        while (xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY) { }
        XrXirCallResult outcome = {0};
        CHECK(xr_xir_task_copy_outcome(&handle, &outcome) == (mode == 1 ? XR_XIR_CALL_RETURNED :
            mode == 2 ? XR_XIR_CALL_BAD_STATE : mode >= 3 ? witness.reason : XR_XIR_CALL_BOUNDS));
        CHECK(witness.callbacks == (mode == 1 ? 2u : 1u));
        if (mode == 1) CHECK(outcome.value.payload == 42);
        if (!mode) CHECK(outcome.panic.detail.index == -7 && outcome.panic.detail.length == 2);
        xr_xir_call_result_drop(&outcome); task_fixture_drop(&fixture);
        CHECK(xr_xir_task_executor_free(executor, NULL) == (mode == 2 ? XR_XIR_CALL_BAD_STATE : mode >= 3 ? witness.reason : XR_XIR_CALL_READY));
        xr_xir_domain_drop(domain); xr_xir_value_drop(&witness.subject); xr_xir_value_drop(&handle);
        CHECK(!runtime_live && !runtime_bytes);
        printf("Task AWAIT panic mode%u protected inbox vs unprotected unwind, foreign flag reject or original nonpanic fault propagation, physical0\n", mode);
    }
}
#endif // TASK_ROOT_ACTIVATION_CASES_H
