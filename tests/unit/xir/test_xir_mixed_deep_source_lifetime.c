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
#include "xir_mixed_deep_source_prepare.h"

static XrCompileResourceStats deep_source_compiled;
static XrXirProgram *deep_source_lifetime_program(unsigned backend) {
    DeepSourcePrepared prepared = deep_source_prepare(backend, SIZE_MAX, deep_compile_caps());
    CHECK(prepared.compile.status == XR_XIR_OK && prepared.compile.stage == 13 && prepared.program);
    deep_source_compiled = prepared.compile.stats;
    CHECK(prepared.compile.sites && prepared.compile.sites < 20000);
    printf("Deep Source lifetime backend=%s compilerSites=%zu allocated=%llu peak=%llu work=%llu cbytes=%zu, sourceSessionDead packetPoisoned nativePreparationDead callerReferenceReleasedBeforeExecution\n",
        worker_backends[backend], prepared.compile.sites, (unsigned long long)prepared.compile.stats.allocated_bytes,
        (unsigned long long)prepared.compile.stats.peak_bytes, (unsigned long long)prepared.compile.stats.work, prepared.compile.c_bytes);
    CHECK(effects_compile_live && effects_compile_bytes && !effects_source_owner_count); return prepared.program;
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

static int deep_source_lifetime_main(int argc, char **argv) {
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
#ifndef XR_DEEP_SOURCE_PREPARATION_MAIN
int main(int argc, char **argv) { return deep_source_lifetime_main(argc, argv); }
#endif
