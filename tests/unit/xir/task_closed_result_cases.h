/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * task_closed_result_cases.h - Owned terminal panic and reflected message edges
 */
#ifndef TASK_CLOSED_RESULT_CASES_H
#define TASK_CLOSED_RESULT_CASES_H
static XrXirAction task_closed_assertion(XrXirCallView *view) {
    TaskWitness *witness = (TaskWitness *)view->environment; XrXirValue *message = view->state;
    ++witness->callbacks;
    XrXirValueStatus status = xr_xir_string_new(xr_xir_call_admission(view)->domain,
        "A\0\xe4\xb8\xad",5,message);
    if (status != XR_XIR_VALUE_OK) return xr_xir_call_fault(status == XR_XIR_VALUE_OOM ?
        XR_XIR_RUN_OUT_OF_MEMORY : XR_XIR_RUN_FRAME_LIMIT);
    return xr_xir_call_assertion(message);
}
static void task_closed_assertion_release(XrXirCallView *view,XrXirCallStatus reason) {
    CHECK(reason == XR_XIR_CALL_ASSERTION);
    xr_xir_value_drop((XrXirValue *)view->state); task_test_release(view,reason);
}
static void task_closed_fixture_new(TaskFixture *fixture) {
    CHECK(native_fixture_owner_new(&fixture->compiler) == XR_XIR_OK);
    const XrXirType scalar = XR_XIR_I64, string = XR_XIR_STRING;
    XrXirCallEntry entries[] = {
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT, 0, task_test_init, NULL, NULL, 0, 0},
        {XR_XIR_CALL_ABI_VERSION, &scalar, 1, XR_XIR_I64, sizeof(uint32_t), task_test_scalar, task_test_release, &fixture->witness, 0, 0},
        {XR_XIR_CALL_ABI_VERSION, &string, 1, XR_XIR_STRING, sizeof(uint32_t), task_test_string, task_test_release, &fixture->witness, 0, 0},
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_I64, sizeof(XrXirValue), task_closed_assertion, task_closed_assertion_release, &fixture->witness, 0, 0},
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
static void task_closed_results(void) {
    TaskFixture fixture = {0}; task_closed_fixture_new(&fixture);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576,&domain) == XR_XIR_VALUE_OK);
    XrXirTaskExecutorConfig config = task_config(&fixture,domain);
    XrXirTaskExecutor *executor = NULL; CHECK(task_test_executor_new(&config,&executor) == XR_XIR_CALL_READY);
    XrXirCallRequest request = {.entry = 3, .arguments = NULL, .count = 0,
        .cell_role = (XrXirCellRoleResolver)0, .cell_context = NULL}; XrXirValue task = {0}, alias = {0};
    CHECK(xr_xir_task_executor_spawn(executor,(XrXirType)256,&request,&task) == XR_XIR_CALL_READY);
    CHECK(xr_xir_value_copy(&task,&alias) == XR_XIR_VALUE_OK);
    task_fixture_drop(&fixture); CHECK(!fixture.witness.code_releases);
    /* A callback consumes one quantum; request_exit only stores the panic.
     * The next quantum finishes the frame and publishes the Task outcome. */
    CHECK(xr_xir_task_executor_poll(executor,1) == XR_XIR_CALL_READY);
    CHECK(fixture.witness.callbacks == 1 && !fixture.witness.releases);
    XrXirCallResult pending = {0};
    CHECK(xr_xir_task_copy_outcome(&task,&pending) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&pending));
    CHECK(xr_xir_task_executor_poll(executor,1) == XR_XIR_CALL_RETURNED);
    CHECK(fixture.witness.callbacks == 1 && fixture.witness.releases == 1);
    CHECK(xr_xir_task_executor_free(executor,NULL) == XR_XIR_CALL_READY);
    CHECK(fixture.witness.code_releases == 1);
    size_t before = runtime_attempts; runtime_fail_at = before;
    xr_xir_domain_close(domain); CHECK(runtime_attempts == before);
    XrXirCallResult first = {0}, second = {0};
    CHECK(xr_xir_task_copy_outcome(&task,&first) == XR_XIR_CALL_ASSERTION);
    CHECK(xr_xir_task_copy_outcome(&alias,&second) == XR_XIR_CALL_ASSERTION);
    CHECK(!first.value.type && !second.value.type && first.panic.detail.code == 445 && second.panic.detail.code == 445);
    task_assert_bytes(&first.panic.message); task_assert_bytes(&second.panic.message);
    CHECK(runtime_attempts == before); runtime_fail_at = SIZE_MAX;
    XrXirValue info = {0}, info_alias = {0};
    CHECK(xr_xir_panic_info_new(domain,&first.panic,&info) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&info,&info_alias) == XR_XIR_VALUE_OK);
    before = runtime_attempts; runtime_fail_at = before;
    xr_xir_value_drop(&task); xr_xir_value_drop(&alias);
    xr_xir_call_result_drop(&first); task_assert_bytes(&second.panic.message);
    xr_xir_call_result_drop(&second); xr_xir_domain_drop(domain);
    XrXirFaultDetail detail = {0}; XrXirValue message = {0};
    CHECK(xr_xir_panic_info_detail(&info_alias,&detail) && detail.code == 445);
    CHECK(xr_xir_panic_info_message(&info_alias,&message) == XR_XIR_VALUE_OK); task_assert_bytes(&message);
    xr_xir_value_drop(&info); xr_xir_value_drop(&info_alias); task_assert_bytes(&message);
    xr_xir_value_drop(&message); CHECK(runtime_attempts == before && !runtime_live && !runtime_bytes);
    runtime_fail_at = SIZE_MAX;
    puts("Closed Task panic/String and PanicInfo: code445, exact5 bytes, producer death, post-close host ownership and physical0");
}

/* Owner close precedes host aliases/outcomes ending; all text is independently
 * fixed to the five bytes A, NUL, E4, B8, AD. No result-family extension. */
static void task_closed_string(void) {
    TaskFixture fixture = {0}; task_fixture_new(&fixture);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    XrXirTaskExecutorConfig config = task_config(&fixture, domain);
    XrXirTaskExecutor *executor = NULL; CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
    XrXirCallRequest request = {.entry = 4, .arguments = NULL, .count = 0,
        .cell_role = (XrXirCellRoleResolver)0, .cell_context = NULL}; XrXirValue task = {0}, alias = {0};
    CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)257, &request, &task) == XR_XIR_CALL_READY);
    task_fixture_drop(&fixture); CHECK(!fixture.witness.code_releases);
    CHECK(xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY); /* yield */
    CHECK(xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY); /* return pending */
    CHECK(!fixture.witness.releases);
    CHECK(xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_RETURNED); /* finish_exit */
    XrXirCallBudget accounting = {0};
    CHECK(xr_xir_task_executor_free(executor, &accounting) == XR_XIR_CALL_READY);
    CHECK(accounting.resumes == 2 && accounting.release_tickets == 1 && accounting.released_frames == 1 &&
        !accounting.live_bytes && accounting.allocations == accounting.frees);
    CHECK(fixture.witness.releases == 1 && fixture.witness.code_releases == 1);
    size_t before = runtime_attempts; runtime_fail_at = before;
    xr_xir_domain_close(domain);
    CHECK(xr_xir_value_copy(&task, &alias) == XR_XIR_VALUE_OK);
    XrXirCallResult first = {0}, second = {0}, third = {0};
    CHECK(xr_xir_task_copy_outcome(&task, &first) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_task_copy_outcome(&alias, &second) == XR_XIR_CALL_RETURNED);
    task_assert_bytes(&first.value); task_assert_bytes(&second.value);
    CHECK(xr_xir_panic_empty(&first.panic) && xr_xir_panic_empty(&second.panic));
    xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    xr_xir_value_drop(&task);
    CHECK(xr_xir_task_copy_outcome(&alias, &third) == XR_XIR_CALL_RETURNED);
    task_assert_bytes(&third.value); xr_xir_value_drop(&alias);
    xr_xir_call_result_drop(&first); task_assert_bytes(&second.value);
    xr_xir_call_result_drop(&second); task_assert_bytes(&third.value);
    xr_xir_call_result_drop(&third);
    CHECK(runtime_attempts == before && !runtime_live && !runtime_bytes); runtime_fail_at = SIZE_MAX;
    puts("Closed terminal Task<String>: producer death, post-close alias, three exact5 outcomes, physical0");
}

/* Internal fail-closed qualification only. Production Instance teardown drains
 * its executor before close. These early closes deliberately probe incomplete
 * Task objects using real preparation/spawn; no fabricated RC or owned edges. */
static void task_closed_incomplete(void) {
    for (unsigned mode = 0; mode < 2; ++mode) {
        TaskFixture fixture = {0}; task_fixture_new(&fixture);
        XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
        XrXirTaskExecutorConfig config = task_config(&fixture, domain);
        XrXirTaskExecutor *executor = NULL; CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
        XrXirValue handle = {0}; XirTask *task = NULL;
        if (!mode) {
            CHECK(task_core_task_prepare(executor, (XrXirType)256, &task) == XR_XIR_VALUE_OK);
            handle = task_core_task_value(task);
            CHECK(atomic_load_explicit(&task->state, memory_order_acquire) == XIR_TASK_PREPARING);
        } else {
            XrXirValue argument = {XR_XIR_I64, 0, 21}; XrXirCallRequest request = {.entry = 1, .arguments = &argument, .count = 1,
                .cell_role = (XrXirCellRoleResolver)0, .cell_context = NULL};
            CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &request, &handle) == XR_XIR_CALL_READY);
            task = (XirTask *)object_pointer(&handle);
            CHECK(atomic_load_explicit(&task->state, memory_order_acquire) == XIR_TASK_PENDING);
        }
        size_t before = runtime_attempts, live = runtime_live, bytes = runtime_bytes;
        runtime_fail_at = before; xr_xir_domain_close(domain);
        CHECK(runtime_attempts == before && runtime_live == live && runtime_bytes == bytes);
        CHECK(xr_xir_value_valid(&handle) && xr_xir_task_storage_valid(task));
        XrXirCallResult outcome = {0};
        CHECK(xr_xir_task_copy_outcome(&handle, &outcome) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&outcome));
        runtime_fail_at = SIZE_MAX;
        if (mode) {
            CHECK(xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY);
            CHECK(atomic_load_explicit(&task->state, memory_order_acquire) == XIR_TASK_RUNNING);
            CHECK(fixture.witness.callbacks == 1 && !fixture.witness.releases);
            xr_xir_value_drop(&handle); /* executor is now the only Task owner */
            before = runtime_attempts; live = runtime_live; bytes = runtime_bytes;
            runtime_fail_at = before; xr_xir_domain_close(domain);
            CHECK(runtime_attempts == before && runtime_live == live && runtime_bytes == bytes);
            runtime_fail_at = SIZE_MAX;
            CHECK(xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY);
            CHECK(xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY); /* return pending */
            CHECK(fixture.witness.callbacks == 3 && !fixture.witness.releases);
            CHECK(xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_RETURNED); /* finish_exit */
            const uint32_t expected[] = {1, 1, 1};
            CHECK(fixture.witness.count == 3 && !memcmp(fixture.witness.trace, expected, sizeof(expected)));
            CHECK(fixture.witness.callbacks == 3 && fixture.witness.releases == 1);
        } else xr_xir_value_drop(&handle);
        task_fixture_drop(&fixture);
        XrXirCallBudget accounting = {0};
        CHECK(xr_xir_task_executor_free(executor, &accounting) == XR_XIR_CALL_READY);
        CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees &&
            accounting.release_tickets == accounting.released_frames && accounting.resumes == (mode ? 3u : 0u));
        CHECK(fixture.witness.code_releases == 1);
        before = runtime_attempts; runtime_fail_at = before;
        xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
        CHECK(runtime_attempts == before && !runtime_live && !runtime_bytes); runtime_fail_at = SIZE_MAX;
    }
    puts("Closed-domain internal early-close guard: real PREPARING/PENDING/RUNNING retained, ordinary RC/drain physical0");
}

#endif // TASK_CLOSED_RESULT_CASES_H
