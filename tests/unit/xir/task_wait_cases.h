/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * task_wait_cases.h - Exact task-wait ownership and late completion rejection
 *
 * KEY CONCEPT:
 *   These canonical-call probes do not grant a separate arena to source programs.
 */
#ifndef TASK_WAIT_CASES_H
#define TASK_WAIT_CASES_H
typedef struct TaskWaitWitness {
    XrXirValue subject;
    uint32_t callbacks, waits, returns, trace[4];
} TaskWaitWitness;
static XrXirAction task_wait_subject(XrXirCallView *view) {
    uint32_t *phase = view->state;
    if ((*phase)++ < 2) return task_test_action(XR_XIR_ACTION_CONTINUE, (XrXirValue){0});
    return task_test_action(XR_XIR_ACTION_RETURN, (XrXirValue){XR_XIR_I64, 0, 42});
}
static XrXirAction task_wait_entry(XrXirCallView *view) {
    TaskWaitWitness *witness = (TaskWaitWitness *)view->environment;
    uint32_t *phase = view->state;
    ++witness->callbacks;
    if ((*phase)++) {
        CHECK(view->inbox.status == XR_XIR_CALL_RETURNED && view->inbox.value.type == XR_XIR_I64 &&
            view->inbox.value.payload == 42 && xr_xir_panic_empty(&view->inbox.panic));
        if (*phase == 3) {
            CHECK(witness->returns < 4);
            witness->trace[witness->returns++] = (uint32_t)view->arguments[0].payload;
            return task_test_action(XR_XIR_ACTION_RETURN, view->inbox.value);
        }
    }
    ++witness->waits;
    return task_test_action(XR_XIR_ACTION_AWAIT_TASK, witness->subject);
}
static void task_wait_entries(TaskFixture *fixture, TaskWaitWitness *witness, XrXirCallEntry entries[7]) {
    memcpy(entries, fixture->program->entries, 7 * sizeof(*entries));
    entries[1].resume = task_wait_entry; entries[1].environment = witness; entries[1].release = NULL;
    entries[3].resume = task_wait_subject; entries[3].state_bytes = sizeof(uint32_t); entries[3].release = NULL;
}
static void task_wait_normal(void) {
    TaskFixture fixture = {0}; task_fixture_new(&fixture);
    TaskWaitWitness witness = {0}; XrXirCallEntry entries[7]; task_wait_entries(&fixture, &witness, entries);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    XrXirTaskExecutorConfig config = task_config(&fixture, domain);
    config.entries = entries; config.entry_count = 7;
    config.admission = (XrXirValueAdmission){fixture.arena, domain, NULL, NULL, 1000000, 1048576};
    XrXirTaskExecutor *executor = NULL; CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
    XrXirCallRequest subject = {3, NULL, 0};
    CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &subject, &witness.subject) == XR_XIR_CALL_READY);
    XrXirValue markers[2] = {{XR_XIR_I64, 0, 1}, {XR_XIR_I64, 0, 2}}, handles[2] = {{0}};
    for (uint32_t i = 0; i < 2; ++i) {
        XrXirCallRequest request = {1, &markers[i], 1};
        CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &request, &handles[i]) == XR_XIR_CALL_READY);
    }
    CHECK(xr_xir_task_executor_poll(executor, 3) == XR_XIR_CALL_READY && witness.waits == 2);
    XirTask *target = (XirTask *)(uintptr_t)witness.subject.payload;
    XirTask *waiter = (XirTask *)(uintptr_t)handles[0].payload;
    CHECK(target->waiter_head == waiter->call && target->waiter_tail != waiter->call);
    XrXirTaskWaitToken token = {0}; CHECK(xr_xir_call_task_wait_token(waiter->call, &token));
    CHECK(token.request.subject == target && token.request.generation == executor->generation &&
        token.request.ticket == target->identity && !token.request.after_ms && !token.request.reserved);
    CHECK(xr_xir_call_resume(waiter->call, token.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_call_link_task_wait(waiter->call, &token) == XR_XIR_CALL_BAD_STATE);
    XrXirCallResult forged = {.status = XR_XIR_CALL_RETURNED, .value = {XR_XIR_I64, 0, 42}};
    XrXirTaskWaitToken wrong = token; ++wrong.binding.generation;
    CHECK(xr_xir_call_complete_task_wait(waiter->call, &wrong, &forged) == XR_XIR_CALL_BAD_STATE && forged.value.payload == 42);
    wrong = token; ++wrong.wake;
    CHECK(xr_xir_call_complete_task_wait(waiter->call, &wrong, &forged) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_call_complete_task_wait(waiter->call, &token, &forged) == XR_XIR_CALL_BAD_STATE);
    while (xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY) { }
    CHECK(witness.waits == 4 && witness.callbacks == 6 && witness.returns == 2 &&
        witness.trace[0] == 1 && witness.trace[1] == 2 && !target->waiter_head && !target->waiter_tail);
    for (uint32_t i = 0; i < 2; ++i) {
        XrXirCallResult outcome = {0};
        CHECK(xr_xir_task_copy_outcome(&handles[i], &outcome) == XR_XIR_CALL_RETURNED && outcome.value.payload == 42);
        xr_xir_call_result_drop(&outcome); xr_xir_value_drop(&handles[i]);
    }
    xr_xir_value_drop(&witness.subject); task_fixture_drop(&fixture);
    CHECK(xr_xir_task_executor_free(executor, NULL) == XR_XIR_CALL_READY);
    xr_xir_domain_drop(domain); CHECK(!runtime_live && !runtime_bytes);
    puts("Task real WAIT subject/generation/ticket, pending+repeated sticky42, FIFO waiters1/2, forged/old/host wake rejection physical0");
}
static void task_wait_cancel(void) {
    TaskFixture fixture = {0}; task_fixture_new(&fixture);
    TaskWaitWitness witness = {0}; XrXirCallEntry entries[7]; task_wait_entries(&fixture, &witness, entries);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    XrXirTaskExecutorConfig config = task_config(&fixture, domain);
    config.entries = entries; config.entry_count = 7;
    config.admission = (XrXirValueAdmission){fixture.arena, domain, NULL, NULL, 1000000, 1048576};
    XrXirTaskExecutor *executor = NULL; CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
    XrXirCallRequest subject = {3, NULL, 0};
    CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &subject, &witness.subject) == XR_XIR_CALL_READY);
    XrXirValue marker = {XR_XIR_I64, 0, 1}, handle = {0}; XrXirCallRequest request = {1, &marker, 1};
    CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &request, &handle) == XR_XIR_CALL_READY);
    CHECK(xr_xir_task_executor_poll(executor, 2) == XR_XIR_CALL_READY);
    XirTask *waiter = (XirTask *)(uintptr_t)handle.payload;
    XirTask *target = (XirTask *)(uintptr_t)witness.subject.payload;
    XrXirTaskWaitToken token = {0}; CHECK(xr_xir_call_task_wait_token(waiter->call, &token));
    CHECK(xr_xir_task_executor_stop(executor) == XR_XIR_CALL_CANCEL_REQUESTED && !target->waiter_head && !target->waiter_tail);
    XrXirCallResult late = {.status = XR_XIR_CALL_RETURNED, .value = {XR_XIR_I64, 0, 42}};
    CHECK(xr_xir_call_complete_task_wait(waiter->call, &token, &late) == XR_XIR_CALL_BAD_STATE && late.value.payload == 42);
    while (xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY) { }
    CHECK(waiter->state == XIR_TASK_TERMINAL && waiter->outcome.status == XR_XIR_CALL_CANCELLED &&
        target->state == XIR_TASK_TERMINAL && target->outcome.status == XR_XIR_CALL_CANCELLED && witness.callbacks == 1);
    task_fixture_drop(&fixture); CHECK(xr_xir_task_executor_free(executor, NULL) == XR_XIR_CALL_READY);
    xr_xir_domain_drop(domain); xr_xir_value_drop(&handle); xr_xir_value_drop(&witness.subject);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Task cancellation unlinks before lease-drop, late completion never wins or reenters callback, terminal host-drop physical0");
}
static void task_wait_identity(void) {
    for (uint32_t mode = 0; mode < 2; ++mode) {
        TaskFixture fixture = {0}; task_fixture_new(&fixture);
        TaskWaitWitness witness = {0}; XrXirCallEntry entries[7]; task_wait_entries(&fixture, &witness, entries);
        XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
        XrXirTaskExecutorConfig config = task_config(&fixture, domain);
        config.entries = entries; config.entry_count = 7;
        config.admission = (XrXirValueAdmission){fixture.arena, domain, NULL, NULL, 1000000, 1048576};
        XrXirTaskExecutor *executor = NULL, *other = NULL;
        CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
        XrXirValue marker = {XR_XIR_I64, 0, 1}, handle = {0}; XrXirCallRequest request = {1, &marker, 1};
        CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &request, &handle) == XR_XIR_CALL_READY);
        if (!mode) CHECK(xr_xir_value_copy(&handle, &witness.subject) == XR_XIR_VALUE_OK);
        else {
            CHECK(task_test_executor_new(&config, &other) == XR_XIR_CALL_READY && other->generation != executor->generation);
            XrXirCallRequest subject = {3, NULL, 0};
            CHECK(xr_xir_task_executor_spawn(other, (XrXirType)256, &subject, &witness.subject) == XR_XIR_CALL_READY);
        }
        while (xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY) { }
        const XirTask *waiter = (const XirTask *)(uintptr_t)handle.payload;
        CHECK(waiter->state == XIR_TASK_TERMINAL && waiter->outcome.status == XR_XIR_CALL_BAD_STATE && witness.callbacks == 1);
        CHECK(xr_xir_task_executor_free(executor, NULL) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_task_executor_free(other, NULL) == XR_XIR_CALL_READY);
        task_fixture_drop(&fixture); xr_xir_domain_drop(domain);
        xr_xir_value_drop(&handle); xr_xir_value_drop(&witness.subject); CHECK(!runtime_live && !runtime_bytes);
        printf("Task WAIT identity rejection=%s no published waiter physical0\n", mode ? "cross-executor" : "self");
    }
}
typedef struct TaskWaitCosts {
    uint64_t value, call, work;
} TaskWaitCosts;
static XrXirCallStatus task_wait_operation(TaskFixture *fixture, TaskWaitCosts limits, TaskWaitCosts *costs) {
    TaskWaitWitness witness = {0}; XrXirCallEntry entries[7]; task_wait_entries(fixture, &witness, entries);
    XrXirDomain *domain = NULL; XrXirTaskExecutor *executor = NULL;
    XrXirValue handle = {0}; XrXirCallResult outcome = {0};
    XrXirCallStatus status = task_core_value_status(xr_xir_domain_new(1048576, &domain));
    if (status == XR_XIR_CALL_READY) {
        XrXirTaskExecutorConfig config = task_config(fixture, domain);
        config.entries = entries; config.entry_count = 7;
        config.admission = (XrXirValueAdmission){fixture->arena, domain, NULL, NULL, 1000000, 1048576};
        if (limits.value) config.requested_value_limit = limits.value;
        if (limits.call) config.requested_call_limit = limits.call;
        if (limits.work) config.work_limit = limits.work;
        status = task_test_executor_new(&config, &executor);
    }
    if (status == XR_XIR_CALL_READY) {
        XrXirCallRequest subject = {3, NULL, 0};
        status = xr_xir_task_executor_spawn(executor, (XrXirType)256, &subject, &witness.subject);
    }
    if (status == XR_XIR_CALL_READY) {
        XrXirValue marker = {XR_XIR_I64, 0, 1}; XrXirCallRequest waiter = {1, &marker, 1};
        status = xr_xir_task_executor_spawn(executor, (XrXirType)256, &waiter, &handle);
    }
    if (status == XR_XIR_CALL_READY) {
        do { status = xr_xir_task_executor_poll(executor, 1); } while (status == XR_XIR_CALL_READY);
        CHECK(status == XR_XIR_CALL_RETURNED);
        status = xr_xir_task_copy_outcome(&handle, &outcome);
        if (status == XR_XIR_CALL_RETURNED) CHECK(outcome.value.type == XR_XIR_I64 && outcome.value.payload == 42);
    }
    if (domain && costs) {
        XrXirDomainBudgetStats measured = xr_xir_domain_budget_stats(domain);
        *costs = (TaskWaitCosts){measured.requested_bytes, measured.requested_call_bytes, measured.work};
    }
    if (executor) {
        XrXirCallStatus freed = xr_xir_task_executor_free(executor, NULL);
        CHECK(freed == XR_XIR_CALL_READY || freed == status);
    }
    xr_xir_call_result_drop(&outcome); xr_xir_value_drop(&handle); xr_xir_value_drop(&witness.subject);
    xr_xir_domain_drop(domain);
    return status;
}
static void task_wait_faults_and_axes(void) {
    TaskFixture fixture = {0}; task_fixture_new(&fixture);
    size_t baseline_live = runtime_live, baseline_bytes = runtime_bytes, sites = 0;
    TaskWaitCosts costs = {0};
    for (size_t ordinal = 0; ordinal <= sites; ++ordinal) {
        runtime_attempts = 0; runtime_fail_at = ordinal ? ordinal - 1 : SIZE_MAX;
        XrXirCallStatus status = task_wait_operation(&fixture, (TaskWaitCosts){0}, ordinal ? NULL : &costs);
        size_t attempts = runtime_attempts; runtime_fail_at = SIZE_MAX;
        if (!ordinal) { CHECK(status == XR_XIR_CALL_RETURNED); sites = attempts; CHECK(sites); }
        else CHECK(status == XR_XIR_CALL_OOM && attempts >= ordinal);
        CHECK(runtime_live == baseline_live && runtime_bytes == baseline_bytes);
    }
    CHECK(costs.value && costs.call && costs.work);
    for (uint32_t axis = 0; axis < 3; ++axis) {
        for (uint32_t less = 0; less < 2; ++less) {
            TaskWaitCosts limits = {0};
            if (!axis) limits.value = costs.value - less;
            else if (axis == 1) limits.call = costs.call - less;
            else limits.work = costs.work - less;
            TaskWaitCosts actual = {0};
            XrXirCallStatus status = task_wait_operation(&fixture, limits, &actual);
            CHECK(status == (less ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_RETURNED));
            CHECK(actual.value <= (limits.value ? limits.value : 2097152) &&
                actual.call <= (limits.call ? limits.call : 2097152) &&
                actual.work <= (limits.work ? limits.work : 128000000));
            CHECK(runtime_live == baseline_live && runtime_bytes == baseline_bytes);
        }
    }
    task_fixture_drop(&fixture); CHECK(!runtime_live && !runtime_bytes);
    printf("Task WAIT complete operation actual runtime OOM=%zu, value/call/work exact-minus1=%llu/%llu/%llu, physical0\n",
        sites, (unsigned long long)costs.value, (unsigned long long)costs.call, (unsigned long long)costs.work);
}
static void task_wait_retain_boundary(void) {
    for (uint32_t phase = 0; phase < 2; ++phase) {
        TaskFixture fixture = {0}; task_fixture_new(&fixture);
        TaskWaitWitness witness = {0}; XrXirCallEntry entries[7]; task_wait_entries(&fixture, &witness, entries);
        XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
        XrXirTaskExecutorConfig config = task_config(&fixture, domain);
        config.entries = entries; config.entry_count = 7;
        config.admission = (XrXirValueAdmission){fixture.arena, domain, NULL, NULL, 1000000, 1048576};
        XrXirTaskExecutor *executor = NULL; CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
        XrXirCallRequest subject = {3, NULL, 0};
        CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &subject, &witness.subject) == XR_XIR_CALL_READY);
        XrXirValue marker = {XR_XIR_I64, 0, 1}, handle = {0}; XrXirCallRequest request = {1, &marker, 1};
        CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &request, &handle) == XR_XIR_CALL_READY);
        XirTask *target = (XirTask *)(uintptr_t)witness.subject.payload;
        if (!phase) {
            CHECK(xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY);
            uint32_t references = atomic_load_explicit(&target->object.references, memory_order_relaxed);
            atomic_store_explicit(&target->object.references, UINT32_MAX, memory_order_relaxed);
            CHECK(xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY && !target->waiter_head);
            atomic_store_explicit(&target->object.references, references, memory_order_relaxed);
        } else {
            CHECK(xr_xir_task_executor_poll(executor, 2) == XR_XIR_CALL_READY && target->waiter_head);
            XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(domain);
            CHECK(xr_xir_domain_work(domain, before.work_limit - before.work));
        }
        while (xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY) { }
        const XirTask *waiter = (const XirTask *)(uintptr_t)handle.payload;
        CHECK(waiter->state == XIR_TASK_TERMINAL && waiter->outcome.status == XR_XIR_CALL_LIMIT &&
            !target->waiter_head && !target->waiter_tail);
        CHECK(xr_xir_task_executor_free(executor, NULL) == XR_XIR_CALL_LIMIT);
        task_fixture_drop(&fixture); xr_xir_domain_drop(domain);
        xr_xir_value_drop(&handle); xr_xir_value_drop(&witness.subject); CHECK(!runtime_live && !runtime_bytes);
        printf("Task WAIT boundary=%s never publishes partial inbox/waiter and physical0\n", phase ? "work-exhausted" : "retain-saturated");
    }
}
static XrXirAction task_wait_cycle_entry(XrXirCallView *view) {
    TaskWaitWitness *witness = (TaskWaitWitness *)view->environment;
    ++witness->callbacks;
    return task_test_action(XR_XIR_ACTION_AWAIT_TASK, witness->subject);
}
static void task_wait_closed_cycle(void) {
    TaskFixture fixture = {0}; task_fixture_new(&fixture);
    TaskWaitWitness witnesses[2] = {{0}}; XrXirCallEntry entries[7];
    task_wait_entries(&fixture, &witnesses[0], entries);
    entries[1].resume = task_wait_cycle_entry;
    entries[3].resume = task_wait_cycle_entry; entries[3].environment = &witnesses[1];
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    XrXirTaskExecutorConfig config = task_config(&fixture, domain);
    config.entries = entries; config.entry_count = 7;
    config.admission = (XrXirValueAdmission){fixture.arena, domain, NULL, NULL, 1000000, 1048576};
    XrXirTaskExecutor *executor = NULL; CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
    XrXirValue marker = {XR_XIR_I64, 0, 1}, handles[2] = {{0}};
    XrXirCallRequest first = {1, &marker, 1}, second = {3, NULL, 0};
    CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &first, &handles[0]) == XR_XIR_CALL_READY);
    CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &second, &handles[1]) == XR_XIR_CALL_READY);
    CHECK(xr_xir_value_copy(&handles[1], &witnesses[0].subject) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&handles[0], &witnesses[1].subject) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_task_executor_poll(executor, 2) == XR_XIR_CALL_READY);
    while (xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY) { }
    for (uint32_t i = 0; i < 2; ++i) {
        const XirTask *task = (const XirTask *)(uintptr_t)handles[i].payload;
        CHECK(task->state == XIR_TASK_TERMINAL && task->outcome.status == XR_XIR_CALL_BAD_STATE &&
            !task->waiter_head && !task->waiter_tail && witnesses[i].callbacks == 1);
    }
    CHECK(xr_xir_task_executor_free(executor, NULL) == XR_XIR_CALL_BAD_STATE);
    task_fixture_drop(&fixture); xr_xir_domain_drop(domain);
    for (uint32_t i = 0; i < 2; ++i) {
        xr_xir_value_drop(&handles[i]); xr_xir_value_drop(&witnesses[i].subject);
    }
    CHECK(!runtime_live && !runtime_bytes);
    puts("Task closed two-node wait graph rejects without external timer, no busy spin or callback reentry physical0");
}
static XrXirAction task_wait_string_entry(XrXirCallView *view) {
    TaskWaitWitness *witness = (TaskWaitWitness *)view->environment;
    uint32_t *phase = view->state;
    ++witness->callbacks;
    if ((*phase)++) {
        CHECK(view->inbox.status == XR_XIR_CALL_RETURNED);
        task_assert_bytes(&view->inbox.value);
        if (*phase == 3) return task_test_action(XR_XIR_ACTION_RETURN, view->inbox.value);
    }
    ++witness->waits;
    return task_test_action(XR_XIR_ACTION_AWAIT_TASK, witness->subject);
}
static void task_wait_string(void) {
    for (uint32_t saturated = 0; saturated < 2; ++saturated) {
        TaskFixture fixture = {0}; task_fixture_new(&fixture);
        TaskWaitWitness witness = {0}; XrXirCallEntry entries[7]; task_wait_entries(&fixture, &witness, entries);
        entries[2].resume = task_wait_string_entry; entries[2].environment = &witness; entries[2].release = NULL;
        XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
        XrXirTaskExecutorConfig config = task_config(&fixture, domain);
        config.entries = entries; config.entry_count = 7;
        config.admission = (XrXirValueAdmission){fixture.arena, domain, NULL, NULL, 1000000, 1048576};
        XrXirTaskExecutor *executor = NULL; CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
        XrXirCallRequest subject = {4, NULL, 0};
        CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)257, &subject, &witness.subject) == XR_XIR_CALL_READY);
        XirTask *target = (XirTask *)(uintptr_t)witness.subject.payload;
        XirObject *string = NULL; uint32_t references = 0;
        if (saturated) {
            while (xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY) { }
            CHECK(target->state == XIR_TASK_TERMINAL && target->outcome.status == XR_XIR_CALL_RETURNED);
            string = (XirObject *)(uintptr_t)target->outcome.value.payload;
            references = atomic_load_explicit(&string->references, memory_order_relaxed);
            atomic_store_explicit(&string->references, UINT32_MAX, memory_order_relaxed);
        }
        XrXirValue marker = {0}, handle = {0};
        CHECK(xr_xir_string_new(domain, "", 0, &marker) == XR_XIR_VALUE_OK);
        XrXirCallRequest request = {2, &marker, 1};
        CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)257, &request, &handle) == XR_XIR_CALL_READY);
        xr_xir_value_drop(&marker);
        CHECK(xr_xir_task_executor_poll(executor, saturated ? 1 : 2) == XR_XIR_CALL_READY);
        if (saturated) {
            CHECK(target->state == XIR_TASK_TERMINAL && target->outcome.status == XR_XIR_CALL_RETURNED &&
                atomic_load_explicit(&string->references, memory_order_relaxed) == UINT32_MAX);
            atomic_store_explicit(&string->references, references, memory_order_relaxed);
        }
        task_fixture_drop(&fixture);
        while (xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY) { }
        CHECK(!target->waiter_head && !target->waiter_tail && target->outcome.status == XR_XIR_CALL_RETURNED);
        const XirTask *waiter = (const XirTask *)(uintptr_t)handle.payload;
        CHECK(waiter->state == XIR_TASK_TERMINAL &&
            waiter->outcome.status == (saturated ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_RETURNED));
        CHECK(witness.callbacks == (saturated ? 1u : 3u) && witness.waits == (saturated ? 1u : 2u));
        CHECK(xr_xir_task_executor_free(executor, NULL) == (saturated ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY));
        xr_xir_domain_drop(domain);
        XrXirCallResult outcome = {0};
        CHECK(xr_xir_task_copy_outcome(&witness.subject, &outcome) == XR_XIR_CALL_RETURNED);
        task_assert_bytes(&outcome.value); xr_xir_value_drop(&witness.subject);
        if (!saturated) {
            XrXirCallResult second = {0};
            CHECK(xr_xir_task_copy_outcome(&handle, &second) == XR_XIR_CALL_RETURNED);
            task_assert_bytes(&second.value); xr_xir_call_result_drop(&second);
        }
        xr_xir_value_drop(&handle); task_assert_bytes(&outcome.value); xr_xir_call_result_drop(&outcome);
        CHECK(fixture.witness.code_releases == 1 && !runtime_live && !runtime_bytes);
        printf("Task WAIT owned string5=%s preserves original sticky outcome after Program/Executor/host-drop physical0\n",
            saturated ? "copy-retain-limit" : "pending-and-repeat");
    }
}
#endif // TASK_WAIT_CASES_H
