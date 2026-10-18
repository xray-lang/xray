/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_nested_storage.c - Deep canonical Task storage validates iteratively
 */
#define main constructed_reference_main
#include "test_xir_task_constructed_results.c"
#undef main
int main(void) {
    enum { DEPTH = 8192 };
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    XrXirTypeNode *nodes = xr_calloc(DEPTH, sizeof(*nodes)); CHECK(nodes);
    for (unsigned i = 0; i < DEPTH; ++i)
        nodes[i] = (XrXirTypeNode){.kind=XR_XIR_TYPE_TASK,.element=i ? (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i-1) : XR_XIR_I64};
    XrXirTypes types = {nodes, DEPTH, NULL, NULL}; XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_compile_type_arena_new(context, &types, &arena) == XR_XIR_VALUE_OK);
    xr_free(nodes);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(UINT64_C(67108864), &domain) == XR_XIR_VALUE_OK);
    XrXirTaskExecutor constructor = {.config={.domain=domain,.task_arena=arena}};
    XrXirValue current = {XR_XIR_I64,0,42};
    for (unsigned i = 0; i < DEPTH; ++i) {
        XirTask *task = NULL;
        CHECK(task_core_task_prepare(&constructor, (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i), &task) == XR_XIR_VALUE_OK);
        task->outcome = (XrXirCallResult){.status=XR_XIR_CALL_RETURNED,.value=current};
        task->generation = 1; task->identity = i+1;
        atomic_store_explicit(&task->state, XIR_TASK_TERMINAL, memory_order_release);
        current = task_core_task_value(task);
    }
    CHECK(xr_xir_value_valid(&current));
    XirTask *top = (XirTask *)(uintptr_t)current.payload; const XrXirValue original = top->outcome.value;
    top->outcome.value = current; CHECK(!xr_xir_value_valid(&current)); top->outcome.value = original;
    XirTask *inner = (XirTask *)(uintptr_t)original.payload;
    uint64_t generation = inner->generation; inner->generation = 0;
    CHECK(!xr_xir_value_valid(&current)); inner->generation = generation;
    uint64_t identity = inner->identity; inner->identity = 0;
    CHECK(!xr_xir_value_valid(&current)); inner->identity = identity;
    uint32_t references = atomic_load(&inner->object.references); atomic_store(&inner->object.references, 0);
    CHECK(!xr_xir_value_valid(&current)); atomic_store(&inner->object.references, references);
    XrXirType inner_type = inner->object.type; inner->object.type = top->object.type;
    CHECK(!xr_xir_value_valid(&current)); inner->object.type = inner_type;
    inner->executor = &constructor; CHECK(!xr_xir_value_valid(&current)); inner->executor = NULL;
    top->outcome.value.reserved = 1; CHECK(!xr_xir_value_valid(&current)); top->outcome.value.reserved = 0;
    top->outcome.wake = 1; CHECK(!xr_xir_value_valid(&current)); top->outcome.wake = 0;
    atomic_store(&inner->state, (XirTaskState)99); CHECK(!xr_xir_value_valid(&current));
    atomic_store(&inner->state, XIR_TASK_TERMINAL);
    CHECK(xr_xir_value_valid(&current));
    xr_xir_compile_type_arena_drop(arena);
    xr_xir_value_drop(&current);
    CHECK(xr_xir_domain_stats(domain).live_bytes == sizeof(*domain)); xr_xir_domain_drop(domain);
    CHECK(!runtime_live && !runtime_bytes); effects_source_owners_free(); CHECK(!effects_compile_live && !effects_compile_bytes);
    printf("TASK_NESTED_STORAGE depth=%u actualCanonicalTasks iterativeValidation cycleRejected iterativeRelease physical=0/0\n",DEPTH);
    return 0;
}
