/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_worker_admission.c - Raw spawn preserves authentic worker proof
 */
#define main constructed_reference_main
#include "test_xir_task_constructed_results.c"
#undef main
int main(int argc, char **argv) {
    CHECK(argc == 2); ConstructedFixture fixture = {0}; constructed_build(&fixture, argv[1]);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(fixture.program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirDomain *domain = instance->domain;
    XrXirTaskExecutor *executor = instance->executor;
    XrXirProgram *program = executor->config.program;
    CHECK(program->permissions && program->permissions->function_count == program->entry_count);
    const XrXirTypeNode node = {.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_UNIT};
    const XrXirTypes types = {&node,1,NULL,NULL}; XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_compile_type_arena_new(xr_xir_compile_artifact_context(fixture.lowered), &types, &arena) == XR_XIR_VALUE_OK);
    XrXirTaskExecutorConfig executor_config = executor->config; executor_config.task_arena = arena;
    XrXirTaskExecutor *private_executor = NULL;
    CHECK(xr_xir_task_executor_new(&executor_config, &private_executor) == XR_XIR_CALL_READY);
    unsigned rejected = 0;
    for (uint32_t m = 0; m < program->declarations->module_count; ++m) {
        uint32_t f = program->declarations->modules[m].initializer;
        CHECK(f < program->entry_count && program->entries[f].result == XR_XIR_UNIT);
        CHECK(program->permissions->entries[f].worker != XR_XIR_OK);
        XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(domain);
        size_t attempts = runtime_attempts; uint64_t identity = private_executor->next_identity;
        XrXirCallRequest request = {.entry=f}; XrXirValue output = {0};
        CHECK(xr_xir_task_executor_spawn(private_executor, (XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,
            &request, &output) == XR_XIR_CALL_BAD_ARGUMENT);
        XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(domain);
        CHECK(task_core_empty_value(&output) && runtime_attempts == attempts && private_executor->next_identity == identity);
        CHECK(before.work == after.work && before.requested_bytes == after.requested_bytes &&
            before.requested_call_bytes == after.requested_call_bytes && !private_executor->active && !private_executor->ready_head);
        ++rejected;
    }
    CHECK(rejected == program->declarations->module_count);
    CHECK(xr_xir_task_executor_free(private_executor, NULL) == XR_XIR_CALL_READY);
    xr_xir_compile_type_arena_drop(arena);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(fixture.program);
    CHECK(fixture.code_releases == 1 && !fixture.lowered);
    CHECK(!runtime_live && !runtime_bytes);
    effects_source_owners_free(); CHECK(!effects_compile_live && !effects_compile_bytes);
    printf("TASK_WORKER_ADMISSION rejected=%u noAllocation noLedgerDebit noTicket noPublication physical=0/0\n", rejected);
    return 0;
}
