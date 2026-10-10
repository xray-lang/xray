/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * task_cross_domain_cases.h - Local residual ring owns real foreign Tasks
 */
#ifndef TASK_CROSS_DOMAIN_CASES_H
#define TASK_CROSS_DOMAIN_CASES_H
static void task_cross_fixture_new(TaskFixture *fixture) {
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
    const XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_STRING},
        {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_UNIT, .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)258},
        {.kind = XR_XIR_TYPE_CELL, .element = (XrXirType)259}};
    const XrXirTypes types = {nodes, 5, NULL, NULL};
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
        entries, 7, &declarations, {&fixture->witness, task_test_code_drop}, &types, {0}};
    XrXirArtifact *proof = NULL;
    XrXirStatus proof_status = native_metadata_fixture(&fixture->compiler.context, &spec, &proof);
    if (proof_status != XR_XIR_OK) fprintf(stderr, "metadata proof status=%d\n", (int)proof_status);
    CHECK(proof_status == XR_XIR_OK);
    spec.proof = xr_xir_compile_program_proof(proof);
    CHECK(xr_xir_compile_program_seal(&fixture->compiler.context, &spec, &fixture->program) == XR_XIR_OK);
    xr_xir_compile_artifact_free(proof);
    CHECK(fixture->program->arena && xr_xir_compile_type_arena_retain(fixture->program->arena));
    fixture->arena = fixture->program->arena;
    CHECK(fixture->program->types == xr_xir_compile_type_arena_types(fixture->arena));
}

static void task_cross_binding_release(void *owner) { ++*(size_t *)owner; }
static XrXirValueStatus task_cross_binding_admit(void *context, const XrXirFunctionBinding *binding,
    XrXirType type, uint64_t *work) {
    uint64_t cost = (uint64_t)binding->capture_count + 1;
    if (*work < cost) return XR_XIR_VALUE_LIMIT;
    *work -= cost;
    return type == (XrXirType)258 && binding->owner == context && binding->release == task_cross_binding_release &&
        binding->entry == 0 && binding->capture_count == 3 ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
}

static void task_cross_outcomes(const XrXirValue tasks[2], bool stopped) {
    for (unsigned i = 0; i < 2; ++i) {
        XrXirCallResult first = {0}, second = {0};
        XrXirCallStatus expected = stopped ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED;
        CHECK(xr_xir_task_copy_outcome(&tasks[i], &first) == expected);
        CHECK(xr_xir_task_copy_outcome(&tasks[i], &second) == expected);
        CHECK(xr_xir_panic_empty(&first.panic) && xr_xir_panic_empty(&second.panic));
        if (stopped) CHECK(!first.value.type && !second.value.type);
        else if (!i) CHECK(first.value.type == XR_XIR_I64 && first.value.payload == 42 &&
            second.value.type == XR_XIR_I64 && second.value.payload == 42);
        else { task_assert_bytes(&first.value); task_assert_bytes(&second.value); }
        xr_xir_call_result_drop(&first);
        if (!stopped && i) task_assert_bytes(&second.value);
        xr_xir_call_result_drop(&second);
    }
}

/* Modes: terminal, stop while pending, A owner closes while B is pending.
 * B is always drained before its execution owner closes. No Task fields are written. */
static void task_cross_domain_case(unsigned mode, unsigned order) {
    CHECK(mode < 3 && order < 2 && !runtime_live && !runtime_bytes);
    CHECK(mode != 2 || order == 0);
    TaskFixture fixture = {0}; task_cross_fixture_new(&fixture);
    XrXirDomain *domains[2] = {0};
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_domain_new(1048576, &domains[i]) == XR_XIR_VALUE_OK);
    XrXirTaskExecutorConfig config = task_config(&fixture, domains[1]);
    XrXirTaskExecutor *executor = NULL;
    CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
    XrXirValue tasks[2] = {{0}}, aliases[2] = {{0}};
    XrXirValue argument = {XR_XIR_I64, 0, 21};
    XrXirCallRequest scalar = {.entry = 1, .arguments = &argument, .count = 1,
        .cell_role = (XrXirCellRoleResolver)0, .cell_context = NULL}, string = {.entry = 4, .arguments = NULL, .count = 0,
        .cell_role = (XrXirCellRoleResolver)0, .cell_context = NULL};
    CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &scalar, &tasks[0]) == XR_XIR_CALL_READY);
    CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)257, &string, &tasks[1]) == XR_XIR_CALL_READY);
    size_t binding_releases = 0;
    XrXirValueAdmission local = {fixture.arena, domains[0], task_cross_binding_admit, &binding_releases, 10000000, 65536};
    XrXirValue cell = {0}, array = {0}, function = {0};
    CHECK(xr_xir_array_new((XrXirType)259, NULL, 0, &local, &array) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domains[0], fixture.arena, (XrXirType)260, &array, &local, &cell) == XR_XIR_VALUE_OK);
    XrXirValue captures[] = {cell, tasks[0], tasks[1]};
    XrXirFunctionBinding binding = {&binding_releases, task_cross_binding_release, 0, captures, 3};
    CHECK(xr_xir_function_new(domains[0], fixture.arena, (XrXirType)258, &binding, &local, &function) == XR_XIR_VALUE_OK);
    XrXirValuePlace place = {(XrXirType)259, &array.payload};
    CHECK(xr_xir_array_push(&place, &function, &local) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_write(&cell, &array, &local) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_arena(&function) == fixture.arena && xr_xir_value_arena(&tasks[0]) == fixture.arena &&
        xr_xir_value_arena(&tasks[1]) == fixture.arena && fixture.arena == fixture.program->arena);
    CHECK(object_pointer(&function)->domain == domains[0] && object_pointer(&tasks[0])->domain == domains[1] &&
        object_pointer(&tasks[1])->domain == domains[1]);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_value_copy(&tasks[i], &aliases[i]) == XR_XIR_VALUE_OK);
        XrXirCallResult pending = {0};
        CHECK(xr_xir_task_copy_outcome(&aliases[i], &pending) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&pending));
        xr_xir_value_drop(&tasks[i]);
    }
    xr_xir_value_drop(&function); xr_xir_value_drop(&array); xr_xir_value_drop(&cell);
    CHECK(!binding_releases);
    task_fixture_drop(&fixture); CHECK(!fixture.witness.code_releases);
    if (mode == 2) {
        size_t before = runtime_attempts; runtime_fail_at = before;
        xr_xir_domain_close(domains[0]);
        CHECK(binding_releases == 1 && runtime_attempts == before); runtime_fail_at = SIZE_MAX;
        for (unsigned i = 0; i < 2; ++i) {
            XrXirCallResult pending = {0};
            CHECK(xr_xir_task_copy_outcome(&aliases[i], &pending) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&pending));
        }
    }
    if (mode == 1) {
        CHECK(xr_xir_task_executor_stop(executor) == XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(xr_xir_task_executor_stop(executor) == XR_XIR_CALL_CANCEL_REQUESTED);
    } else {
        XrXirCallStatus status = XR_XIR_CALL_READY;
        /* Three scalar callbacks and two String callbacks, plus both finish_exit steps. */
        for (unsigned poll = 0; poll < 7 && status == XR_XIR_CALL_READY; ++poll)
            status = xr_xir_task_executor_poll(executor, 1);
        CHECK(status == XR_XIR_CALL_RETURNED && fixture.witness.callbacks == 3 && fixture.witness.releases == 2);
    }
    XrXirCallBudget accounting = {0};
    size_t before = runtime_attempts; runtime_fail_at = before;
    CHECK(xr_xir_task_executor_free(executor, &accounting) == XR_XIR_CALL_READY);
    CHECK(runtime_attempts == before && !accounting.live_bytes && accounting.allocations == accounting.frees &&
        accounting.release_tickets == 2 && accounting.released_frames == 2);
    CHECK(fixture.witness.code_releases == 1 && fixture.witness.releases == 2);
    xr_xir_domain_close(domains[order]); xr_xir_domain_close(domains[1u - order]);
    CHECK(binding_releases == 1);
    task_cross_outcomes(aliases, mode == 1);
    xr_xir_domain_drop(domains[0]); xr_xir_domain_drop(domains[1]);
    task_cross_outcomes(aliases, mode == 1);
    for (unsigned i = 0; i < 2; ++i) xr_xir_value_drop(&aliases[i]);
    CHECK(runtime_attempts == before && !runtime_live && !runtime_bytes); runtime_fail_at = SIZE_MAX;
    printf("TASK_CROSS_DOMAIN mode=%u close_order=%u shared_program_arena=1 local_ring_release=1 codelease_release=1 physical=0 cleanup=0\n", mode, order);
}

static void task_cross_domain_cases(void) {
    for (unsigned mode = 0; mode < 2; ++mode)
        for (unsigned order = 0; order < 2; ++order) task_cross_domain_case(mode, order);
    task_cross_domain_case(2, 0);
}
#endif // TASK_CROSS_DOMAIN_CASES_H
