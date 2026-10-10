/* Typed native outcomes retain their actual closed enum authority. */
#include "xir/xxir_enum.h"
typedef struct TaskErrorState { XrXirValue field, error, erased; } TaskErrorState;
typedef struct TaskErrorFixture { TaskFixture base; uint32_t mode; } TaskErrorFixture;
static XrXirAction task_test_error(XrXirCallView *view) {
    uint32_t mode = *(const uint32_t *)view->environment;
    TaskErrorState *state = view->state;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    if (mode == 1) state->field = (XrXirValue){XR_XIR_I64, 0, 7};
    if (mode == 2) status = xr_xir_string_new(admission->domain, "A\0\xe4\xb8\xad", 5, &state->field);
    if (status == XR_XIR_VALUE_OK) status = xr_xir_enum_new((XrXirType)(mode == 3 ? 258 : 257),
        mode == 3 ? 0 : mode, mode == 1 || mode == 2 ? &state->field : NULL,
        mode == 1 || mode == 2 ? 1 : 0, admission, &state->error);
    if (status != XR_XIR_VALUE_OK) return xr_xir_call_fault(status == XR_XIR_VALUE_OOM ?
        XR_XIR_RUN_OUT_OF_MEMORY : XR_XIR_RUN_FRAME_LIMIT);
    return task_test_action(XR_XIR_ACTION_THROW, state->error);
}
static void task_test_error_release(XrXirCallView *view, XrXirCallStatus reason) {
    (void)reason;
    TaskErrorState *state = view->state;
    xr_xir_value_drop(&state->erased); xr_xir_value_drop(&state->error); xr_xir_value_drop(&state->field);
}
static void task_error_fixture_new(TaskErrorFixture *f) {
    CHECK(native_fixture_owner_new(&f->base.compiler) == XR_XIR_OK);
    const XrXirNominalField fields[] = {{{"number", 6}, XR_XIR_I64, 0}, {{"text", 4}, XR_XIR_STRING, 0}};
    const XrXirNominalField bad_field = {{"hidden", 6}, (XrXirType)256, 0};
    const XrXirNominalVariant variants[] = {{{"Empty", 5}, 0, 0}, {{"Number", 6}, 0, 1}, {{"Text", 4}, 1, 1}};
    const XrXirNominalVariant bad_variants[] = {{{"Empty", 5}, 0, 0}, {{"Unsafe", 6}, 0, 1}};
    const XrXirNominalDeclaration nominals[] = {
        {.module = {"task-errors", 11}, .name = {"Local", 5}, .kind = XR_XIR_NOMINAL_CLASS, .flags = XR_XIR_NOMINAL_FINAL},
        {.module = {"task-errors", 11}, .name = {"Failure", 7}, .kind = XR_XIR_NOMINAL_ENUM,
            .fields = fields, .field_count = 2, .variants = variants, .variant_count = 3},
        {.module = {"task-errors", 11}, .name = {"UnsafeFailure", 13}, .kind = XR_XIR_NOMINAL_ENUM,
            .fields = &bad_field, .field_count = 1, .variants = bad_variants, .variant_count = 2}};
    const XrXirNominalTable table = {nominals, 3, NULL};
    const XrXirType safe_types[] = {XR_XIR_I64, XR_XIR_STRING}, bad_type = (XrXirType)256;
    const XrXirTypeNode nodes[] = {{.kind = XR_XIR_TYPE_NOMINAL},
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {1, NULL, 0, safe_types, 2}},
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {2, NULL, 0, &bad_type, 1}}};
    const XrXirTypes types = {nodes, 3, &table, NULL};
    const XrXirCallEntry entries[] = {
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT, 0, task_test_init, NULL, NULL, 0, 0},
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_I64, sizeof(TaskErrorState), task_test_error,
            task_test_error_release, &f->mode, 0, 0},
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_I64, 0, task_test_failure, NULL, NULL, 0, 0}};
    XrXirFunctionIdentity identities[3] = {0}; identities[1].exported = identities[2].exported = 1;
    const XrXirSourceModule source = {"task-errors", 11, NULL, 0, 0};
    const XrXirDeclarations declarations = {.modules = &source, .module_count = 1, .functions = identities,
        .root_module = 0, .entry_function = 2};
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
        entries, 3, &declarations, {&f->base.witness, task_test_code_drop}, &types, {0}};
    XrXirArtifact *proof = NULL;
    CHECK(native_metadata_fixture(&f->base.compiler.context, &spec, &proof) == XR_XIR_OK);
    const XrXirModule *lowered = xr_xir_compile_artifact_module(proof);
    spec.types = lowered->types; spec.declarations = lowered->declarations; spec.proof = xr_xir_compile_program_proof(proof);
    CHECK(xr_xir_compile_program_seal(&f->base.compiler.context, &spec, &f->base.program) == XR_XIR_OK);
    xr_xir_compile_artifact_free(proof);
    const XrXirTypeNode task_node = {.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_I64};
    const XrXirTypes task_types = {&task_node, 1, NULL, NULL};
    CHECK(xr_xir_compile_type_arena_new(&f->base.compiler.context, &task_types, &f->base.arena) == XR_XIR_VALUE_OK);
}
static XrXirValueStatus task_error_expected(const XrXirCallResult *result, uint32_t mode, XrXirDomain *domain) {
    if (mode == 3) { CHECK(result->status == XR_XIR_CALL_BAD_STATE && !result->value.type); return XR_XIR_VALUE_OK; }
    CHECK(result->status == XR_XIR_CALL_THROWN && result->value.type == 257);
    XrXirValue underlying = {0}, field = {0}; uint32_t variant = UINT32_MAX;
    underlying = result->value;
    CHECK(xr_xir_enum_variant(&underlying, &variant) == XR_XIR_VALUE_OK && variant == mode);
    if (mode) {
        XrXirValueAdmission admission = {xr_xir_value_arena(&underlying), domain, NULL, NULL, 1000000, 1048576};
        XrXirValueStatus status = xr_xir_enum_get(&underlying, mode, 0, &admission, &field);
        if (status != XR_XIR_VALUE_OK) { CHECK(!field.type); return status; }
        if (mode == 1) CHECK(field.type == XR_XIR_I64 && field.payload == 7);
        else task_assert_bytes(&field);
        xr_xir_value_drop(&field);
    }
    return XR_XIR_VALUE_OK;
}
static void task_runtime_errors(void) {
    for (uint32_t mode = 0; mode < 4; ++mode) {
        TaskErrorFixture f = {.mode = mode}; task_error_fixture_new(&f);
        XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
        XrXirTaskExecutorConfig config = task_config(&f.base, domain);
        XrXirTaskExecutor *executor = NULL; CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
        XrXirCallRequest request = {.entry = 1, .arguments = NULL, .count = 0,
            .cell_role = (XrXirCellRoleResolver)0, .cell_context = NULL}; XrXirValue handle = {0};
        CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &request, &handle) == XR_XIR_CALL_READY);
        while (xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY) { }
        XrXirCallResult result = {0};
        XrXirCallStatus copied = xr_xir_task_copy_outcome(&handle, &result);
        if (copied != (mode == 3 ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_THROWN))
            fprintf(stderr, "native throw mode=%u status=%u stored=%u\n", mode, copied,
                ((const XirTask *)(uintptr_t)handle.payload)->outcome.status);
        CHECK(copied == (mode == 3 ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_THROWN));
        CHECK(task_error_expected(&result, mode, domain) == XR_XIR_VALUE_OK); xr_xir_call_result_drop(&result);
        task_fixture_drop(&f.base);
        CHECK(xr_xir_task_executor_free(executor, NULL) == (mode == 3 ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_READY));
        xr_xir_domain_drop(domain);
        if (!mode) {
            const XirTask *task = (const XirTask *)(uintptr_t)handle.payload;
            XrXirTypeArena *arena = (XrXirTypeArena *)xr_xir_value_arena(&task->outcome.value);
            uint32_t references = atomic_load(&arena->references);
            atomic_store(&arena->references, UINT32_MAX);
            CHECK(xr_xir_task_copy_outcome(&handle, &result) == XR_XIR_CALL_LIMIT && xr_xir_call_result_empty(&result));
            CHECK(task->outcome.status == XR_XIR_CALL_THROWN && task->outcome.value.type == 257);
            atomic_store(&arena->references, references);
        }
        for (uint32_t repeat = 0; repeat < 2; ++repeat) {
            CHECK(xr_xir_task_copy_outcome(&handle, &result) == (mode == 3 ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_THROWN));
            CHECK(task_error_expected(&result, mode, domain) == XR_XIR_VALUE_OK); xr_xir_call_result_drop(&result);
        }
        xr_xir_value_drop(&handle);
        CHECK(f.base.witness.code_releases == 1 && !runtime_live && !runtime_bytes);
    }
    puts("Task true native THROW empty/i64/string sticky after host-drop; hidden unsafe variant rejects before publication physical0");
}
static XrXirCallStatus task_error_operation(TaskErrorFixture *f, uint64_t work_limit,
    XrXirDomainBudgetStats *cost) {
    XrXirDomain *domain = NULL;
    XrXirCallStatus status = task_core_value_status(xr_xir_domain_new(1048576, &domain));
    XrXirTaskExecutor *executor = NULL; XrXirValue handle = {0}; XrXirCallResult outcome = {0};
    if (status == XR_XIR_CALL_READY) {
        XrXirTaskExecutorConfig config = task_config(&f->base, domain); config.work_limit = work_limit;
        status = task_test_executor_new(&config, &executor);
    }
    if (status == XR_XIR_CALL_READY) {
        XrXirCallRequest request = {.entry = 1, .arguments = NULL, .count = 0,
            .cell_role = (XrXirCellRoleResolver)0, .cell_context = NULL};
        status = xr_xir_task_executor_spawn(executor, (XrXirType)256, &request, &handle);
    }
    if (status == XR_XIR_CALL_READY) {
        do { status = xr_xir_task_executor_poll(executor, 1); } while (status == XR_XIR_CALL_READY);
        CHECK(status == XR_XIR_CALL_RETURNED);
        status = xr_xir_task_copy_outcome(&handle, &outcome);
        if (status == XR_XIR_CALL_THROWN) {
            XrXirValueStatus observed = task_error_expected(&outcome, f->mode, domain);
            if (observed != XR_XIR_VALUE_OK) status = task_core_value_status(observed);
        }
    }
    if (cost && domain) *cost = xr_xir_domain_budget_stats(domain);
    if (executor) {
        XrXirCallBudget accounting = {0};
        XrXirCallStatus freed = xr_xir_task_executor_free(executor, &accounting);
        CHECK(freed == XR_XIR_CALL_READY || freed == status);
        CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees &&
            accounting.release_tickets == accounting.released_frames);
    }
    xr_xir_domain_drop(domain); xr_xir_call_result_drop(&outcome); xr_xir_value_drop(&handle);
    return status;
}
static void task_error_faults(void) {
    for (uint32_t mode = 0; mode < 3; ++mode) {
        TaskErrorFixture f = {.mode = mode}; task_error_fixture_new(&f);
        size_t baseline_live = runtime_live, baseline_bytes = runtime_bytes, sites = 0;
        for (size_t ordinal = 0; ordinal <= sites; ++ordinal) {
            runtime_attempts = 0; runtime_fail_at = ordinal ? ordinal - 1 : SIZE_MAX;
            XrXirCallStatus status = task_error_operation(&f, 128000000, NULL);
            size_t attempts = runtime_attempts; runtime_fail_at = SIZE_MAX;
            if (!ordinal) { CHECK(status == XR_XIR_CALL_THROWN); sites = attempts; CHECK(sites); }
            else CHECK(status == XR_XIR_CALL_OOM && attempts >= ordinal);
            CHECK(runtime_live == baseline_live && runtime_bytes == baseline_bytes);
        }
        task_fixture_drop(&f.base); CHECK(!runtime_live && !runtime_bytes);
        printf("Task native complete error mode=%u actual runtime OOM ordinals=%zu physical0\n", mode, sites);
    }
}
static void task_error_work_boundary(void) {
    TaskErrorFixture f = {0}; task_error_fixture_new(&f);
    XrXirDomainBudgetStats normal = {0};
    CHECK(task_error_operation(&f, 128000000, &normal) == XR_XIR_CALL_THROWN);
    CHECK(normal.work > 1);
    for (uint32_t minus = 0; minus < 2; ++minus) {
        XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
        XrXirTaskExecutorConfig config = task_config(&f.base, domain); config.work_limit = normal.work - minus;
        XrXirTaskExecutor *executor = NULL; CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
        XrXirCallRequest request = {.entry = 1, .arguments = NULL, .count = 0,
            .cell_role = (XrXirCellRoleResolver)0, .cell_context = NULL}; XrXirValue handle = {0};
        CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)256, &request, &handle) == XR_XIR_CALL_READY);
        while (xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY) { }
        CHECK(xr_xir_task_executor_free(executor, NULL) == XR_XIR_CALL_READY);
        xr_xir_domain_drop(domain);
        const XirTask *task = (const XirTask *)(uintptr_t)handle.payload;
        CHECK(task->outcome.status == XR_XIR_CALL_THROWN);
        XrXirCallResult output = {0};
        CHECK(xr_xir_task_copy_outcome(&handle, &output) == (minus ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_THROWN));
        xr_xir_call_result_drop(&output);
        CHECK(xr_xir_task_copy_outcome(&handle, &output) == XR_XIR_CALL_LIMIT && xr_xir_call_result_empty(&output));
        CHECK(task->outcome.status == XR_XIR_CALL_THROWN && task->outcome.value.type == 257);
        xr_xir_value_drop(&handle);
    }
    task_fixture_drop(&f.base); CHECK(!runtime_live && !runtime_bytes);
    printf("Task terminal empty-error retain saturation and exact/minus1 rootwork=%llu keep sticky outcome physical0\n",
        (unsigned long long)normal.work);
}
