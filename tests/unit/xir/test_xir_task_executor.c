/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_executor.c - Typed queued owners and complete physical release
 */
#include "xir/xxir_task_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#else
#include <pthread.h>
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
#include "base/xcompile_resources.c"
#include "xir_native_metadata_fixture.h"

typedef struct TaskWitness { uint32_t callbacks, releases, code_releases, cleanup_steps, trace[128], count; } TaskWitness;
static XrXirAction task_test_action(XrXirActionKind kind, XrXirValue value) {
    return (XrXirAction){kind, 0, NULL, 0, value, {0}, 0};
}
static XrXirAction task_test_init(XrXirCallView *view) {
    (void)view; return task_test_action(XR_XIR_ACTION_RETURN, (XrXirValue){0});
}
static void task_test_release(XrXirCallView *view, XrXirCallStatus reason) {
    (void)reason; ++((TaskWitness *)view->environment)->releases;
}
static XrXirAction task_test_scalar(XrXirCallView *view) {
    TaskWitness *witness = (TaskWitness *)view->environment;
    uint32_t *phase = view->state;
    ++witness->callbacks;
    CHECK(witness->count < 128);
    witness->trace[witness->count++] = view->arguments[0].payload == 21 ? 1u : 3u;
    if ((*phase)++ < 2) return task_test_action(XR_XIR_ACTION_CONTINUE, (XrXirValue){0});
    return task_test_action(XR_XIR_ACTION_RETURN, (XrXirValue){XR_XIR_I64, 0, view->arguments[0].payload * 2});
}
static XrXirAction task_test_string(XrXirCallView *view) {
    TaskWitness *witness = (TaskWitness *)view->environment;
    uint32_t *phase = view->state;
    ++witness->callbacks;
    CHECK(witness->count < 128); witness->trace[witness->count++] = 2;
    if (!(*phase)++) return task_test_action(XR_XIR_ACTION_SUSPEND, (XrXirValue){0});
    return task_test_action(XR_XIR_ACTION_RETURN, view->arguments[0]);
}
static XrXirAction task_test_failure(XrXirCallView *view) {
    (void)view; return xr_xir_call_bounds(-7, 2);
}
static XrXirAction task_test_cleanup_limit(XrXirCallView *view) {
    if (view->phase == XR_XIR_CALL_EXIT) {
        ++((TaskWitness *)view->environment)->cleanup_steps;
        return task_test_action(XR_XIR_ACTION_CONTINUE, (XrXirValue){0});
    }
    return task_test_action(XR_XIR_ACTION_SUSPEND, (XrXirValue){0});
}
typedef struct TaskGeneratedString { uint32_t phase; XrXirValue text; } TaskGeneratedString;
static XrXirAction task_test_generated_string(XrXirCallView *view) {
    TaskGeneratedString *frame = view->state;
    if (!frame->phase++) return task_test_action(XR_XIR_ACTION_SUSPEND, (XrXirValue){0});
    XrXirValueStatus status = xr_xir_string_new(xr_xir_call_admission(view)->domain, "A\0\xe4\xb8\xad", 5, &frame->text);
    if (status != XR_XIR_VALUE_OK) return xr_xir_call_fault(status == XR_XIR_VALUE_OOM ?
        XR_XIR_RUN_OUT_OF_MEMORY : XR_XIR_RUN_FRAME_LIMIT);
    return task_test_action(XR_XIR_ACTION_RETURN, frame->text);
}
static void task_test_generated_release(XrXirCallView *view, XrXirCallStatus reason) {
    TaskGeneratedString *frame = view->state;
    xr_xir_value_drop(&frame->text);
    task_test_release(view, reason);
}
static void task_test_code_drop(void *owner) { ++((TaskWitness *)owner)->code_releases; }
typedef struct TaskFixture {
    TaskWitness witness;
    NativeFixtureOwner compiler;
    XrXirProgram *program;
    XrXirTypeArena *arena;
    XrXirCallBudget budget;
} TaskFixture;
static void task_fixture_new(TaskFixture *fixture) {
    CHECK(native_fixture_owner_new(&fixture->compiler) == XR_XIR_OK);
    const XrXirType scalar = XR_XIR_I64, string = XR_XIR_STRING;
    XrXirCallEntry entries[] = {
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT, 0, task_test_init, NULL, NULL, 0, 0},
        {XR_XIR_CALL_ABI_VERSION, &scalar, 1, XR_XIR_I64, sizeof(uint32_t), task_test_scalar, task_test_release, &fixture->witness, 0, 0},
        {XR_XIR_CALL_ABI_VERSION, &string, 1, XR_XIR_STRING, sizeof(uint32_t), task_test_string, task_test_release, &fixture->witness, 0, 0},
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_I64, 0, task_test_failure, task_test_release, &fixture->witness, 0, 0},
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_STRING, sizeof(TaskGeneratedString), task_test_generated_string,
            task_test_generated_release, &fixture->witness, 0, 0},
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_I64, 0, task_test_cleanup_limit,
            task_test_release, &fixture->witness, XR_XIR_ENTRY_EXIT, 0},
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT, 0, task_test_init, NULL, NULL, 0, 6}};
    XrXirFunctionIdentity functions[7] = {0};
    for (unsigned i = 1; i < 6; ++i) functions[i].exported = 1;
    functions[6].cleanup_owner = 6;
    const XrXirSourceModule module = {"task-test", 9, NULL, 0, 0};
    XrXirDeclarations declarations = {&module, 1, functions, NULL, 0, NULL, 0, 0, 3, NULL};
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
        entries, 7, &declarations, {&fixture->witness, task_test_code_drop}, NULL, {0}};
    XrXirArtifact *proof = NULL;
    XrXirStatus proof_status = native_metadata_fixture(&fixture->compiler.context, &spec, &proof);
    if (proof_status != XR_XIR_OK) fprintf(stderr, "metadata proof status=%d\n", (int)proof_status);
    CHECK(proof_status == XR_XIR_OK);
    spec.proof = xr_xir_compile_program_proof(proof);
    CHECK(xr_xir_compile_program_seal(&fixture->compiler.context, &spec, &fixture->program) == XR_XIR_OK);
    xr_xir_compile_artifact_free(proof);
    const XrXirTypeNode nodes[] = {{.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_STRING}};
    const XrXirTypes types = {nodes, 2, NULL, NULL};
    CHECK(xr_xir_compile_type_arena_new(&fixture->compiler.context, &types, &fixture->arena) == XR_XIR_VALUE_OK);
}
static void task_fixture_drop(TaskFixture *fixture) {
    xr_xir_compile_program_drop(fixture->program); fixture->program = NULL;
    xr_xir_compile_type_arena_drop(fixture->arena); fixture->arena = NULL;
    native_fixture_owner_free(&fixture->compiler);
}
static XrXirTaskExecutorConfig task_config(TaskFixture *fixture, XrXirDomain *domain) {
    return (XrXirTaskExecutorConfig){.program = fixture->program, .domain = domain, .task_arena = fixture->arena,
        .call_limit = 1048576, .poll_limit = 1000000, .requested_value_limit = 2097152,
        .requested_call_limit = 2097152, .work_limit = 128000000, .depth_limit = 64, .budget = &fixture->budget};
}
static XrXirCallStatus task_test_executor_new(const XrXirTaskExecutorConfig *config, XrXirTaskExecutor **output) {
    if (!xr_xir_domain_budget_stats(config->domain).bound) {
        XrXirDomainBudgetControls controls = {config->requested_value_limit, config->requested_call_limit,
            config->work_limit, config->call_limit, config->call_limit};
        XrXirValueStatus bound = xr_xir_domain_budget_bind(config->domain, &controls);
        if (bound != XR_XIR_VALUE_OK) return task_core_value_status(bound);
        *config->budget = (XrXirCallBudget){.byte_limit = config->call_limit,
            .requested_limit = config->requested_call_limit, .resume_limit = config->poll_limit,
            .work_domain = config->domain};
    }
    return xr_xir_task_executor_new(config, output);
}
static void task_assert_bytes(const XrXirValue *value) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length));
    CHECK(length == 5 && !memcmp(bytes, "A\0\xe4\xb8\xad", 5));
}
static void task_normal(void) {
    TaskFixture fixture = {0}; task_fixture_new(&fixture);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    XrXirTaskExecutorConfig config = task_config(&fixture, domain);
    XrXirTaskExecutor *executor = NULL; CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
    XrXirValue argument = {XR_XIR_I64, 0, 21}, text = {0}, scalar_task = {0}, string_task = {0}, discarded = {0};
    CHECK(xr_xir_string_new(domain, "A\0\xe4\xb8\xad", 5, &text) == XR_XIR_VALUE_OK);
    XrXirCallRequest scalar_request = {1, &argument, 1}, string_request = {2, &text, 1};
    CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &scalar_request, &scalar_task) == XR_XIR_CALL_READY);
    CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)257, &string_request, &string_task) == XR_XIR_CALL_READY);
    argument.payload = 7;
    CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &scalar_request, &discarded) == XR_XIR_CALL_READY);
    xr_xir_value_drop(&discarded); xr_xir_value_drop(&text);
    XrXirCallResult first = {0}, second = {0};
    CHECK(xr_xir_task_copy_outcome(&scalar_task, &first) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY && fixture.witness.callbacks == 1);
    CHECK(xr_xir_task_copy_outcome(&scalar_task, &first) == XR_XIR_CALL_BAD_STATE);
    task_fixture_drop(&fixture); CHECK(!fixture.witness.code_releases);
    while (xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY) { }
    const uint32_t expected[] = {1, 2, 3, 1, 2, 3, 1, 3};
    CHECK(fixture.witness.count == 8 && !memcmp(fixture.witness.trace, expected, sizeof(expected)));
    CHECK(fixture.witness.releases == 3);
    XrXirDomainBudgetStats costs = xr_xir_domain_budget_stats(domain);
    CHECK(costs.bound && costs.work && costs.requested_bytes && costs.work < costs.work_limit);
    XrXirCallBudget accounting = {0};
    CHECK(xr_xir_task_executor_free(executor, &accounting) == XR_XIR_CALL_READY);
    CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees &&
        accounting.resumes == 8 && accounting.release_tickets == 3 && accounting.released_frames == 3);
    CHECK(fixture.witness.code_releases == 1);
    xr_xir_domain_drop(domain);
    CHECK(xr_xir_task_copy_outcome(&scalar_task, &first) == XR_XIR_CALL_RETURNED && first.value.payload == 42);
    CHECK(xr_xir_task_copy_outcome(&scalar_task, &second) == XR_XIR_CALL_RETURNED && second.value.payload == 42);
    xr_xir_call_result_drop(&first); xr_xir_call_result_drop(&second); xr_xir_value_drop(&scalar_task);
    CHECK(xr_xir_task_copy_outcome(&string_task, &first) == XR_XIR_CALL_RETURNED); task_assert_bytes(&first.value);
    CHECK(xr_xir_task_copy_outcome(&string_task, &second) == XR_XIR_CALL_RETURNED); task_assert_bytes(&second.value);
    xr_xir_value_drop(&string_task); task_assert_bytes(&first.value);
    xr_xir_call_result_drop(&first); task_assert_bytes(&second.value); xr_xir_call_result_drop(&second);
    CHECK(!runtime_live && !runtime_bytes);
    printf("Task true kind8 FIFO callbacks8, last-handle-no-cancel, repeated42/string5, codelease+domain escape physical0; costs value=%llu work=%llu call=%llu\n",
        (unsigned long long)costs.requested_bytes, (unsigned long long)costs.work,
        (unsigned long long)accounting.requested_bytes);
}
static void task_cancel_and_limit(void) {
    for (uint32_t mode = 0; mode < 3; ++mode) {
        TaskFixture fixture = {0}; task_fixture_new(&fixture);
        XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
        XrXirTaskExecutorConfig config = task_config(&fixture, domain);
        if (mode == 1) config.poll_limit = 2;
        if (mode == 2) config.work_limit = 7;
        XrXirTaskExecutor *executor = NULL; CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
        XrXirValue argument = {XR_XIR_I64, 0, 21}, handles[2] = {{0}, {0}};
        XrXirCallRequest request = {1, &argument, 1};
        CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &request, &handles[0]) == XR_XIR_CALL_READY);
        if (mode != 2) CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &request, &handles[1]) == XR_XIR_CALL_READY);
        if (!mode) {
            CHECK(xr_xir_task_executor_stop(executor) == XR_XIR_CALL_CANCEL_REQUESTED);
            CHECK(xr_xir_task_executor_stop(executor) == XR_XIR_CALL_CANCEL_REQUESTED);
        }
        task_fixture_drop(&fixture);
        XrXirCallBudget accounting = {0};
        if (mode) while (xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY) { }
        CHECK(xr_xir_task_executor_free(executor, &accounting) == (mode ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY));
        CHECK(accounting.release_tickets == accounting.released_frames && !accounting.live_bytes);
        xr_xir_domain_drop(domain);
        for (uint32_t i = 0; i < (mode == 2 ? 1u : 2u); ++i) {
            XrXirCallResult outcome = {0};
            CHECK(xr_xir_task_copy_outcome(&handles[i], &outcome) == (mode ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_CANCELLED));
            if (mode == 2) CHECK(xr_xir_call_result_empty(&outcome));
            const XirTask *task = (const XirTask *)(uintptr_t)handles[i].payload;
            CHECK(task->state == XIR_TASK_TERMINAL && task->outcome.status == (mode ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_CANCELLED));
            xr_xir_call_result_drop(&outcome); xr_xir_value_drop(&handles[i]);
        }
        CHECK(fixture.witness.code_releases == 1 && !runtime_live && !runtime_bytes);
    }
    puts("Task queued stop idempotent and shared resume/work limits preserve finite release tickets physical0");
}
static void task_sticky_failure(void) {
    TaskFixture fixture = {0}; task_fixture_new(&fixture);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    XrXirTaskExecutorConfig config = task_config(&fixture, domain);
    XrXirTaskExecutor *executor = NULL; CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
    XrXirValue handle = {0}; XrXirCallRequest request = {3, NULL, 0};
    CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &request, &handle) == XR_XIR_CALL_READY);
    while (xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY) { }
    task_fixture_drop(&fixture); CHECK(xr_xir_task_executor_free(executor, NULL) == XR_XIR_CALL_READY);
    xr_xir_domain_drop(domain);
    for (uint32_t i = 0; i < 2; ++i) {
        XrXirCallResult outcome = {0}; CHECK(xr_xir_task_copy_outcome(&handle, &outcome) == XR_XIR_CALL_BOUNDS);
        CHECK(outcome.panic.detail.index == -7 && outcome.panic.detail.length == 2);
        xr_xir_call_result_drop(&outcome);
    }
    xr_xir_value_drop(&handle); CHECK(!runtime_live && !runtime_bytes);
    puts("Task complete panic channel sticky twice after Program/Executor/domain host-drop physical0");
}
static void task_escaped_budget(void) {
    for (uint32_t mode = 0; mode < 2; ++mode) {
        TaskFixture fixture = {0}; task_fixture_new(&fixture);
        XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
        XrXirTaskExecutorConfig config = task_config(&fixture, domain);
        config.work_limit = 48;
        if (mode) config.requested_value_limit = sizeof(*domain) + sizeof(XirTask);
        XrXirTaskExecutor *executor = NULL; CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
        XrXirValue argument = {XR_XIR_I64, 0, 21}, handle = {0}, fresh = {0};
        XrXirCallRequest request = {1, &argument, 1};
        CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &request, &handle) == XR_XIR_CALL_READY);
        while (xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY) { }
        task_fixture_drop(&fixture);
        CHECK(xr_xir_task_executor_free(executor, NULL) == XR_XIR_CALL_READY);
        xr_xir_domain_drop(domain);
        XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(domain);
        CHECK(before.bound && before.work < before.work_limit);
        XrXirCallResult outcome = {0};
        if (!mode) {
            uint64_t copies = before.work_limit - before.work;
            for (uint64_t i = 0; i < copies; ++i) {
                CHECK(xr_xir_task_copy_outcome(&handle, &outcome) == XR_XIR_CALL_RETURNED && outcome.value.payload == 42);
                xr_xir_call_result_drop(&outcome);
            }
            CHECK(xr_xir_task_copy_outcome(&handle, &outcome) == XR_XIR_CALL_LIMIT && xr_xir_call_result_empty(&outcome));
            CHECK(xr_xir_value_copy(&handle, &fresh) == XR_XIR_VALUE_LIMIT && task_core_empty_value(&fresh));
        } else {
            CHECK(before.requested_bytes == before.requested_limit);
            CHECK(xr_xir_task_copy_outcome(&handle, &outcome) == XR_XIR_CALL_RETURNED && outcome.value.payload == 42);
            xr_xir_call_result_drop(&outcome);
        }
        CHECK(xr_xir_string_new(domain, "", 0, &fresh) == XR_XIR_VALUE_LIMIT && task_core_empty_value(&fresh));
        XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(domain);
        CHECK(after.bound && after.requested_bytes == before.requested_bytes && after.work <= after.work_limit);
        const XirTask *task = (const XirTask *)(uintptr_t)handle.payload;
        CHECK(task->state == XIR_TASK_TERMINAL && !task->executor && !task->call &&
            task->outcome.status == XR_XIR_CALL_RETURNED && task->outcome.value.payload == 42);
        xr_xir_value_drop(&handle);
        CHECK(fixture.witness.code_releases == 1 && !runtime_live && !runtime_bytes);
        printf("Task escaped terminal budget axis=%s remains bound after host-drop, sticky42 and physical0\n", mode ? "requested-value" : "work");
    }
}
static void task_failed_executor_epoch(void) {
    for (uint32_t mode = 0; mode < 3; ++mode) {
        TaskFixture fixture = {0}; task_fixture_new(&fixture);
        XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
        XrXirTaskExecutorConfig config = task_config(&fixture, domain);
        XrXirTaskExecutor *executor = NULL;
        if (!mode) { runtime_attempts = 0; runtime_fail_at = 0; }
        if (mode == 1) atomic_store(&fixture.program->references, UINT32_MAX);
        if (mode == 2) config.requested_call_limit = sizeof(XrXirTaskExecutor) - 1;
        CHECK(task_test_executor_new(&config, &executor) == (mode ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM));
        runtime_fail_at = SIZE_MAX;
        if (mode == 1) atomic_store(&fixture.program->references, 1);
        CHECK(!executor);
        XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(domain);
        CHECK(spent.bound && spent.requested_bytes == sizeof(*domain) && spent.work == (!mode ? 1u : 0u));
        size_t attempts = runtime_attempts;
        CHECK(task_test_executor_new(&config, &executor) == (mode == 2 ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY));
        XrXirDomainBudgetStats retry = xr_xir_domain_budget_stats(domain);
        CHECK(retry.bound && retry.requested_bytes == spent.requested_bytes &&
            retry.work_limit == spent.work_limit && retry.requested_limit == spent.requested_limit);
        CHECK(retry.work == spent.work + (mode == 2 ? 0u : 1u) &&
            retry.requested_call_bytes == spent.requested_call_bytes + (mode == 2 ? 0u : sizeof(*executor)) &&
            runtime_attempts == attempts + (mode == 2 ? 0u : 1u));
        CHECK(xr_xir_task_executor_free(executor, NULL) == XR_XIR_CALL_READY);
        xr_xir_domain_drop(domain); task_fixture_drop(&fixture);
        CHECK(fixture.witness.code_releases == 1 && !runtime_live && !runtime_bytes);
    }
    puts("Task failed Executor OOM/retain/requested-call construction preserves one bound epoch, retry cannot reset physical0");
}
static void task_borrowed_epoch(void) {
    TaskFixture fixture = {0}; task_fixture_new(&fixture);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    XrXirTaskExecutorConfig config = task_config(&fixture, domain);
    XrXirDomainBudgetControls controls = {config.requested_value_limit, config.requested_call_limit,
        config.work_limit, config.call_limit, config.call_limit};
    CHECK(xr_xir_domain_budget_bind(domain, &controls) == XR_XIR_VALUE_OK);
    fixture.budget = (XrXirCallBudget){.byte_limit = config.call_limit, .requested_limit = config.requested_call_limit,
        .resume_limit = config.poll_limit, .work_domain = domain};
    XrXirCallConfig root;
    XrXirCallAccounting root_accounting = {0};
    CHECK(xr_xir_call_config_init(&root, sizeof(root)) == XR_XIR_CALL_READY);
    root.entries = fixture.program->entries; root.entry_count = fixture.program->entry_count;
    root.byte_limit = config.call_limit; root.poll_limit = config.poll_limit; root.depth_limit = config.depth_limit;
    root.accounting = &root_accounting;
    root.admission = (XrXirValueAdmission){fixture.program->arena, domain, NULL, NULL, 1000000, 1048576};
    XrXirCallRequest root_request = {4, NULL, 0}; XrXirCall *call = NULL;
    CHECK(xr_xir_call_new_budgeted(&root, &root_request, &fixture.budget, &call) == XR_XIR_CALL_READY);
    XrXirCallResult generated;
    do {
        generated = xr_xir_call_poll_bounded(call, 1);
        if (generated.status == XR_XIR_CALL_SUSPENDED) {
            CHECK(xr_xir_call_resume(call, generated.wake) == XR_XIR_CALL_READY);
            generated.status = XR_XIR_CALL_READY;
        }
    } while (generated.status == XR_XIR_CALL_READY);
    CHECK(generated.status == XR_XIR_CALL_RETURNED);
    XrXirCallResult text = {0}; CHECK(xr_xir_call_take_outcome(call, &text) == XR_XIR_CALL_RETURNED);
    task_assert_bytes(&text.value);
    XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(domain);
    CHECK(before.requested_bytes > sizeof(*domain) && before.call_live && before.work);
    XrXirTaskExecutor *executor = NULL; CHECK(xr_xir_task_executor_new(&config, &executor) == XR_XIR_CALL_READY);
    XrXirDomainBudgetStats created = xr_xir_domain_budget_stats(domain);
    CHECK(created.requested_bytes == before.requested_bytes && created.work == before.work + 1 &&
        created.requested_call_bytes == before.requested_call_bytes + sizeof(*executor));
    XrXirCallRequest request = {2, &text.value, 1}; XrXirValue handle = {0};
    CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)257, &request, &handle) == XR_XIR_CALL_READY);
    while (xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY) { }
    XrXirCallBudget accounting = {0};
    CHECK(xr_xir_task_executor_free(executor, &accounting) == XR_XIR_CALL_READY);
    CHECK(accounting.live_bytes && fixture.budget.live_bytes == accounting.live_bytes &&
        fixture.budget.work_domain == domain && accounting.resumes == fixture.budget.resumes);
    CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY && !fixture.budget.live_bytes);
    XrXirDomainBudgetStats drained = xr_xir_domain_budget_stats(domain);
    CHECK(drained.bound && !drained.metadata_live && !drained.call_live && drained.work > before.work);
    task_fixture_drop(&fixture); xr_xir_domain_drop(domain);
    XrXirCallResult copied = {0}; CHECK(xr_xir_task_copy_outcome(&handle, &copied) == XR_XIR_CALL_RETURNED);
    task_assert_bytes(&text.value); task_assert_bytes(&copied.value);
    xr_xir_call_result_drop(&text); xr_xir_value_drop(&handle); task_assert_bytes(&copied.value);
    xr_xir_call_result_drop(&copied);
    CHECK(fixture.witness.code_releases == 1 && !runtime_live && !runtime_bytes);
    puts("Task executor borrows a used root budget: earlier string and live root Call preserved, no epoch reset, host-drop physical0");
}
static void task_cleanup_limit(void) {
    for (uint32_t mode = 0; mode < 2; ++mode) {
        TaskFixture fixture = {0}; task_fixture_new(&fixture);
        XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
        XrXirTaskExecutorConfig config = task_config(&fixture, domain);
        config.work_limit = mode ? 100 : 24;
        if (mode) config.poll_limit = 4;
        XrXirTaskExecutor *executor = NULL; CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
        XrXirValue handle = {0}; XrXirCallRequest request = {5, NULL, 0};
        CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &request, &handle) == XR_XIR_CALL_READY);
        CHECK(xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY);
        task_fixture_drop(&fixture);
        XrXirCallBudget accounting = {0};
        CHECK(xr_xir_task_executor_free(executor, &accounting) == XR_XIR_CALL_LIMIT);
        CHECK(fixture.witness.cleanup_steps && fixture.witness.releases == 1 &&
            !accounting.live_bytes && accounting.allocations == accounting.frees &&
            accounting.release_tickets == 1 && accounting.released_frames == 1);
        XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(domain);
        CHECK(spent.bound && spent.work <= spent.work_limit);
        const XirTask *task = (const XirTask *)(uintptr_t)handle.payload;
        CHECK(task->state == XIR_TASK_TERMINAL && task->outcome.status == XR_XIR_CALL_LIMIT && !task->executor && !task->call);
        xr_xir_domain_drop(domain); xr_xir_value_drop(&handle);
        CHECK(fixture.witness.code_releases == 1 && !runtime_live && !runtime_bytes);
        printf("Task incomplete EXIT cleanup axis=%s returns LIMIT after prepaid frame release physical0\n", mode ? "resume" : "work");
    }
}
static XrXirCallStatus task_fault_operation(TaskFixture *fixture) {
    XrXirDomain *domain = NULL;
    XrXirCallStatus status = task_core_value_status(xr_xir_domain_new(1048576, &domain));
    XrXirTaskExecutor *executor = NULL;
    XrXirValue handle = {0};
    XrXirCallResult outcome = {0};
    if (status == XR_XIR_CALL_READY) {
        XrXirTaskExecutorConfig config = task_config(fixture, domain);
        status = task_test_executor_new(&config, &executor);
    }
    if (status == XR_XIR_CALL_READY) {
        XrXirCallRequest request = {4, NULL, 0};
        status = xr_xir_task_executor_spawn(executor, (XrXirType)257, &request, &handle);
    }
    if (status == XR_XIR_CALL_READY) {
        do { status = xr_xir_task_executor_poll(executor, 1); } while (status == XR_XIR_CALL_READY);
        CHECK(status == XR_XIR_CALL_RETURNED);
        status = xr_xir_task_copy_outcome(&handle, &outcome);
        if (status == XR_XIR_CALL_RETURNED) task_assert_bytes(&outcome.value);
    }
    if (executor) {
        XrXirCallBudget accounting = {0};
        XrXirCallStatus freed = xr_xir_task_executor_free(executor, &accounting);
        CHECK(freed == XR_XIR_CALL_READY || (status == XR_XIR_CALL_OOM && freed == XR_XIR_CALL_OOM));
        CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees &&
            accounting.release_tickets == accounting.released_frames);
    }
    xr_xir_domain_drop(domain);
    xr_xir_call_result_drop(&outcome); xr_xir_value_drop(&handle);
    return status;
}
static void task_runtime_faults(void) {
    TaskFixture fixture = {0}; task_fixture_new(&fixture);
    size_t baseline_live = runtime_live, baseline_bytes = runtime_bytes, sites = 0;
    for (size_t ordinal = 0; ordinal <= sites; ++ordinal) {
        runtime_attempts = 0; runtime_fail_at = ordinal ? ordinal - 1 : SIZE_MAX;
        XrXirCallStatus status = task_fault_operation(&fixture);
        size_t attempts = runtime_attempts; runtime_fail_at = SIZE_MAX;
        if (!ordinal) { CHECK(status == XR_XIR_CALL_RETURNED); sites = attempts; CHECK(sites); }
        else CHECK(status == XR_XIR_CALL_OOM && attempts >= ordinal);
        CHECK(runtime_live == baseline_live && runtime_bytes == baseline_bytes && !fixture.witness.code_releases);
    }
    task_fixture_drop(&fixture);
    CHECK(fixture.witness.code_releases == 1 && !runtime_live && !runtime_bytes);
    printf("Task actual runtime OOM exact ordinals=%zu include queued child string allocation, both owned outcome domains and physical baseline preserved\n", sites);
}
#include "task_runtime_error_cases.h"
#include "task_terminal_thread_cases.h"
#include "task_wait_cases.h"
#include "task_root_activation_cases.h"
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    task_normal(); task_cancel_and_limit(); task_sticky_failure();
    task_escaped_budget(); task_failed_executor_epoch(); task_borrowed_epoch(); task_cleanup_limit(); task_runtime_faults();
    task_runtime_errors(); task_error_faults(); task_error_work_boundary();
    task_terminal_threads();
    task_wait_normal(); task_wait_cancel(); task_wait_identity();
    task_wait_faults_and_axes(); task_wait_retain_boundary();
    task_wait_closed_cycle();
    task_wait_string();
    task_root_normal(); task_wait_panic_protocol();
    native_fixture_owner_report();
    CHECK(!runtime_live && !runtime_bytes); return 0;
}
