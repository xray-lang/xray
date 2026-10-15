/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_deep_source_lifetime.c - Execute after Source and caller ledger retirement
 */
#include "xir/xxir_source.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#define XR_DEEP_RESOURCE_MAIN
#include "test_xir_mixed_deep_recursion.c"
#include "xir_mixed_deep_compile.h"

static XrCompileResourceStats deep_source_compiled;
static XrXirProgram *deep_source_lifetime_program(unsigned backend) {
    CHECK(backend < 4 && !effects_compile_live && !effects_compile_bytes && !effects_source_owner_count);
    CHECK(!deep_code && !worker_control_code && !worker_code_releases && !runtime_live);
    DeepCompileState state = {0}; XrCompileResourceLimits caps = deep_compile_caps();
    state.context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps, &state.context.resources) == XR_COMPILE_RESOURCE_OK);
    CHECK(deep_compile_source(&state) && deep_compile_lower(&state));
    const XrXirModule *module = xr_xir_compile_artifact_module(state.lowered);
    CHECK(module->function_count == worker_deep_fixture_program.entry_count && module->function_count <= WORKER_FUNCTIONS);
    DeepCode *owner = NULL;
    CHECK(xr_compile_resources_calloc(state.context.resources, 1, sizeof(*owner), (void **)&owner) == XR_COMPILE_RESOURCE_OK);
    WorkerControlCode *code = &owner->base;
    CHECK(xr_compile_resources_calloc(state.context.resources, module->function_count, sizeof(*code->entries), (void **)&code->entries) == XR_COMPILE_RESOURCE_OK);
    code->functions = module->function_count; code->root = worker_deep_entry; worker_control_targets(code, module);
    XrXirProgramProof proof = xr_xir_compile_program_proof(state.lowered);
    CHECK(deep_compile_prepare(&state) && state.result.stage == 10);
    CHECK(state.root == worker_deep_entry && state.descend == worker_deep_descend);
    code->vm = state.vm; state.vm = NULL;
    for (uint32_t f = 0; f < code->functions; ++f) {
        code->native[f] = backend == 3 || (backend < 2 && (f == code->root) == (backend == 1));
        CHECK(state.native->entries[f].resume != code->vm->entries[f].resume);
        code->entries[f] = code->native[f] ? state.native->entries[f] : code->vm->entries[f];
        code->delegates[f] = code->entries[f].resume; code->entries[f].resume = deep_resume;
    }
    XrXirProgramSpec spec = worker_deep_fixture_program;
    spec.entries = code->entries; spec.types = state.native->types; spec.declarations = state.native->declarations;
    spec.proof = proof; spec.code = (XrXirCodeLease){owner, deep_code_free};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&state.context, &spec, &program) == XR_XIR_OK);
    deep_code = owner; worker_control_code = code;
    xr_xir_compile_c_source_free(&state.c); xr_xir_compile_program_drop(state.native); state.native = NULL;
    CHECK(!state.source.checked && !state.source.snapshot && !state.session && !state.packet.bytes);
    CHECK(!state.checked && !state.closed && !state.lowered && !state.vm);
    deep_compile_product(program, state.root, state.descend);
    CHECK(xr_compile_resources_stats(state.context.resources, &state.result.stats) == XR_COMPILE_RESOURCE_OK);
    deep_source_compiled = state.result.stats;
    CHECK(effects_compile_attempts && effects_compile_attempts < 20000);
    printf("Deep Source lifetime backend=%s compilerSites=%zu allocated=%llu peak=%llu work=%llu cbytes=%zu, sourceSessionDead packetPoisoned nativePreparationDead callerReferenceReleasedBeforeExecution\n",
        worker_backends[backend], effects_compile_attempts, (unsigned long long)state.result.stats.allocated_bytes,
        (unsigned long long)state.result.stats.peak_bytes, (unsigned long long)state.result.stats.work, state.result.c_bytes);
    xr_compile_resources_release(state.context.resources); state.context.resources = NULL;
    CHECK(effects_compile_live && effects_compile_bytes && !effects_source_owner_count); return program;
}

/* Terminal Tasks own their shared closed type pool, never the Program graph.
 * Match every remaining physical compiler block to that arena or its ledger. */
static void deep_source_type_owners(const XrXirValue *tasks) {
    const XrXirTypeArena *arena = NULL; unsigned remaining = 0;
    for (unsigned mode = 0; mode < 6; ++mode) {
        if (!tasks[mode].type) continue;
        XirTask *task = (XirTask *)(uintptr_t)tasks[mode].payload;
        CHECK(xr_xir_value_valid(&tasks[mode]) && !task->executor && !task->call);
        CHECK(atomic_load_explicit(&task->state, memory_order_acquire) == XIR_TASK_TERMINAL);
        if (!arena) arena = task->object.arena;
        CHECK(arena && task->object.arena == arena); ++remaining;
        const XrXirTypeNode *node = xr_xir_type_node(xr_xir_compile_type_arena_types(arena), task->object.type);
        CHECK(node && node->kind == XR_XIR_TYPE_TASK && node->element == XR_XIR_I64);
    }
    if (!remaining) { CHECK(!effects_compile_live && !effects_compile_bytes); return; }
    const CompileAllocation *allocation = (const CompileAllocation *)(const void *)arena - 1;
    XrCompileResources *resources = allocation->info.resources;
    CHECK(resources && resources->references == 1 && effects_compile_live == 2);
    const size_t bytes = sizeof(*resources) + sizeof(*allocation) + allocation->info.bytes;
    CHECK(effects_compile_bytes == bytes);
    for (size_t slot = 0; slot < 32768; ++slot) {
        EffectsCompileAllocation live = effects_compile_allocations[slot];
        if (live.pointer == resources) CHECK(live.bytes == sizeof(*resources));
        else if (live.pointer == allocation) CHECK(live.bytes == sizeof(*allocation) + allocation->info.bytes);
        else CHECK(!live.pointer);
    }
    XrCompileResourceStats stats = {0}; CHECK(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes == bytes && stats.allocated_bytes == deep_source_compiled.allocated_bytes);
    CHECK(stats.peak_bytes == deep_source_compiled.peak_bytes && stats.work == deep_source_compiled.work);
    printf("Deep Source survivor remainingTasks=%u compilerBlocks=2 compilerBytes=%zu, exactTypeArenaAndLedgerOnly codeAndProgramDead\n", remaining, bytes);
}

static void deep_source_lifetime_outcomes(XrXirValue *tasks) {
    XrXirCallResult outcomes[6] = {{0}};
    for (unsigned mode = 0; mode < 6; ++mode) {
        XrXirCallStatus expected = mode < 3 ? XR_XIR_CALL_RETURNED : mode == 5 ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_CANCELLED;
        CHECK(xr_xir_task_copy_outcome(&tasks[mode], &outcomes[mode]) == expected && outcomes[mode].status == expected);
        CHECK(!xr_xir_call_result_empty(&outcomes[mode]) && xr_xir_panic_empty(&outcomes[mode].panic));
        xr_xir_value_drop(&tasks[mode]);
        deep_source_type_owners(tasks);
    }
    CHECK(!effects_compile_live && !effects_compile_bytes);
    for (unsigned mode = 0; mode < 6; ++mode) {
        if (mode < 3) CHECK(outcomes[mode].value.type == XR_XIR_I64 && outcomes[mode].value.payload == (int64_t)(mode ? DEEP_RECURSION : 16u)+7);
        else CHECK(!outcomes[mode].value.type && !outcomes[mode].value.payload);
        xr_xir_call_result_drop(&outcomes[mode]);
    }
    CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);
}

int main(int argc, char **argv) {
    CHECK(argc == 2); (void)deep_main; (void)deep_compile_operation;
    unsigned backend = 0; while (backend < 4 && strcmp(argv[1], worker_backends[backend])) ++backend;
    XrXirProgram *program = deep_source_lifetime_program(backend); XrXirValue tasks[6] = {{0}}; DeepMetrics metrics[6];
    for (unsigned mode = 0; mode < 6; ++mode) {
        metrics[mode] = deep_operation(program, mode, &tasks[mode]); DeepRecord *record = &deep_code->record;
        if (backend == 2) CHECK(record->vm && !record->native);
        else if (backend == 3) CHECK(record->native && !record->vm);
        else CHECK(record->native && record->vm);
        printf("Deep Source execution backend=%s mode=%u walks=%u peak=%u beforeYieldStack=%llu cStack=%llu pool=%llu work=%llu, authenticFrames closed depthLimit64\n",
            worker_backends[backend], mode, metrics[mode].walks, metrics[mode].peak,
            (unsigned long long)metrics[mode].before_yield_stack, (unsigned long long)metrics[mode].stack,
            (unsigned long long)metrics[mode].pool, (unsigned long long)metrics[mode].work);
    }
    CHECK(metrics[1].pool > metrics[0].pool && metrics[1].peak > metrics[0].peak && metrics[1].walks > metrics[0].walks);
    CHECK(metrics[1].before_yield_stack <= metrics[0].before_yield_stack+4096 && metrics[1].stack <= metrics[0].stack+4096);
    xr_xir_compile_program_drop(program);
    CHECK(worker_code_releases == 1 && !worker_control_code && !deep_code);
    CHECK(runtime_live && runtime_bytes); deep_source_type_owners(tasks);
    deep_source_lifetime_outcomes(tasks);
    printf("Deep Source lifetime backend=%s sixLateTasks, exactTypeOwnerOnlyBeforeCopy compilerPhysical0AfterLastTask allTaskHandlesDeadBeforeOutcomeDrop physical0/0\n", worker_backends[backend]);
    return 0;
}
