/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_api.c - Verified artifact runtime facade
 *
 * KEY CONCEPT:
 *   This file adds no execution authority. It composes the verified artifact
 *   authority, the TargetPlan load boundary, and the generation lifecycle into
 *   one product surface, and every fact it cannot re-derive from those owners
 *   is refused rather than assumed.
 */

#include "../../include/xray_runtime_api.h"
#include "../../include/xray_target_plan_load.h"
#include "../base/xmalloc.h"
#include "../os/os_thread.h"
#include "../plan/semantic/xr_semantic_plan.h"
#include "../plan/target/xr_target_plan.h"
#include "../vm/xr_typed_dispatch.h"
#include "xr_entry_cell.h"
#include "xr_dynamic_entry_runtime.h"
#include "xr_module_generation_internal.h"
#include <stdio.h>
#include <string.h>

/*
 * XrExport is the facade spelling of one canonical entry cell plus the exact
 * expectation captured by resolution. It carries no second module/function
 * execution handle: all execution authority and generation pins live in the
 * cell.
 */
struct XrExport {
    XrRuntimeEntryHandle *handle;
    XrRuntimeModuleActivation *activation;
};

struct XrModule {
    XrRuntime *runtime;
    XrRuntimeArtifactAuthority *artifact_authority;
    XrTargetPlan *plan;
    XrLoadedModuleGeneration *generation;
    XrRuntimeModuleActivation *activation;
    XrExport *exports;
    uint32_t function_count;
    bool unloading;
};

/* Program is a distinct public facade over the same artifact/generation
 * owners. It deliberately has no export registry or module-local entry cell.
 *
 * Unlike a module, a program has no entry cell to close, so its own gate is
 * what stops a new execution and proves to unload that none is in flight. The
 * count has to be taken before the generation handle is read: the generation's
 * pin is acquired inside the executor, and until then there is nothing keeping
 * a concurrent unload from freeing the generation out from under the call. */
struct XrProgram {
    XrRuntime *runtime;
    XrRuntimeArtifactAuthority *artifact_authority;
    XrTargetPlan *plan;
    XrLoadedModuleGeneration *generation;
    xr_mutex_t gate;
    uint32_t inflight_executions;
    bool unloading;
};

struct XrRuntime {
    xr_mutex_t gate;
    XrRuntimeGenerationAuthority *authority;
    uint32_t loaded_artifacts;
};

static bool fail(char *diagnostic, size_t diagnostic_size, const char *code,
                 const char *detail) {
    if (diagnostic && diagnostic_size)
        snprintf(diagnostic, diagnostic_size, "%s: %s", code, detail);
    return false;
}

XRAY_API bool xr_runtime_create(const XrRuntimeConfig *config,
                                XrRuntime **runtime, char *diagnostic,
                                size_t diagnostic_size) {
    if (runtime)
        *runtime = NULL;
    if (!runtime || !config ||
        config->schema_version != XR_RUNTIME_CONFIG_SCHEMA_VERSION ||
        config->reserved != 0)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "runtime creation requires an exact complete configuration");
    XrRuntime *created = (XrRuntime *) xr_calloc(1, sizeof(*created));
    if (!created)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "runtime allocation failed");
    /* The generation authority owns budget validation, so an incomplete or
     * zero budget is refused by the same checker the lifecycle uses rather
     * than by a second copy of the rule here. */
    if (!xr_runtime_generation_authority_create(&config->generation,
                                                &created->authority,
                                                diagnostic, diagnostic_size)) {
        xr_free(created);
        return false;
    }
    if (!xr_runtime_activation_registry_configure(
            created->authority, &config->activation, &config->providers,
            diagnostic, diagnostic_size)) {
        char nested[256] = {0};
        xr_runtime_generation_authority_destroy(
            &created->authority, nested, sizeof(nested));
        xr_free(created);
        return false;
    }
    xr_mutex_init(&created->gate);
    *runtime = created;
    return true;
}

XRAY_API bool xr_runtime_destroy(XrRuntime **runtime, char *diagnostic,
                                 size_t diagnostic_size) {
    if (!runtime || !*runtime)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "runtime is missing");
    XrRuntime *owned = *runtime;
    xr_mutex_lock(&owned->gate);
    bool empty = owned->loaded_artifacts == 0;
    xr_mutex_unlock(&owned->gate);
    if (!empty)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5006",
                    "runtime still owns loaded artifacts");
    if (!xr_runtime_generation_authority_destroy(&owned->authority, diagnostic,
                                                 diagnostic_size))
        return false;
    xr_mutex_destroy(&owned->gate);
    memset(owned, 0, sizeof(*owned));
    xr_free(owned);
    *runtime = NULL;
    return true;
}

/* Unwinds a partially loaded artifact through the lifecycle's own failure
 * branch. A generation that never activated rolls back to RETIRED with zero
 * pins, which is the only state unload accepts, so no artifact is abandoned in
 * a live state and no diagnostic from the original failure is overwritten. */
static void discard_partial_artifact(XrLoadedModuleGeneration *generation,
                                     XrTargetPlan *plan,
                                     XrRuntimeArtifactAuthority *authority) {
    char nested[512] = {0};
    if (generation) {
        xr_module_generation_rollback(generation, nested, sizeof(nested));
        xr_module_generation_retire(generation, nested, sizeof(nested));
        xr_module_generation_unload(&generation, nested, sizeof(nested));
    }
    xr_target_plan_free(plan);
    xr_runtime_artifact_authority_free(authority);
}

static bool initialize_exports(XrExport *exports, uint32_t count,
                               char *diagnostic,
                               size_t diagnostic_size) {
    for (uint32_t i = 0; i < count; i++) {
        if (xr_runtime_entry_handle_create(&exports[i].handle, diagnostic,
                                           diagnostic_size))
            continue;
        char ignored[1] = {0};
        for (uint32_t j = 0; j < i; j++)
            xr_runtime_entry_handle_release(&exports[j].handle, ignored,
                                            sizeof(ignored));
        return false;
    }
    return true;
}

static bool bind_export_entries(XrLoadedModuleGeneration *generation,
                                const XrTargetPlan *plan,
                                XrExport *exports, uint32_t function_count,
                                XrRuntimeModuleActivation **activation,
                                char *diagnostic,
                                size_t diagnostic_size) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(plan);
    size_t export_count = semantic
                              ? xr_semantic_plan_source_export_count(semantic)
                              : 0;
    for (size_t i = 0; i < export_count; i++) {
        const XrSemanticSourceExportRecord *record =
            xr_semantic_plan_source_export(semantic, (uint32_t) i);
        if (!record || record->function >= function_count ||
            xr_target_plan_function_execution_family_mask(
                plan, record->function) !=
                XR_TARGET_EXECUTION_SCALAR_I64_CLOSED)
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                        "module export is outside the installed execution family");
        XrEntryCellRegistration registration = {
            .generation = generation,
            .verified_plan = plan,
            .function = record->function,
            .executor_kind = XR_ENTRY_EXECUTOR_TYPED_VM,
        };
        bool already_bound = false;
        if (!xr_runtime_entry_handle_bind(
                exports[record->function].handle, &registration,
                &already_bound, diagnostic, diagnostic_size))
            return false;
    }
    XrRuntimeEntryHandle **handles =
        (XrRuntimeEntryHandle **) xr_calloc(function_count,
                                            sizeof(*handles));
    if (!handles)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "module entry publication allocation failed");
    for (uint32_t i = 0; i < function_count; i++)
        handles[i] = exports[i].handle;
    bool published = xr_runtime_activation_publish_module(
        generation->authority, generation, plan, handles, function_count,
        activation, diagnostic, diagnostic_size);
    xr_free(handles);
    if (published)
        for (uint32_t i = 0; i < function_count; i++)
            exports[i].activation = *activation;
    return published;
}

static bool clear_exports(XrExport *exports, uint32_t count,
                          char *diagnostic, size_t diagnostic_size) {
    for (uint32_t i = 0; i < count; i++) {
        XrEntryCell *cell =
            xr_runtime_entry_handle_cell(exports[i].handle);
        if (!xr_entry_cell_clear(cell, diagnostic,
                                 diagnostic_size))
            return false;
    }
    return true;
}

static bool dispose_exports(XrExport *exports, uint32_t count,
                            char *diagnostic, size_t diagnostic_size) {
    for (uint32_t i = 0; i < count; i++) {
        if (!xr_runtime_entry_handle_release(&exports[i].handle, diagnostic,
                                             diagnostic_size))
            return false;
    }
    return true;
}

XRAY_API bool xr_module_load_target_plan(
    XrRuntime *runtime, const uint8_t *semantic_artifact_bytes,
    size_t semantic_artifact_size, const uint8_t *target_artifact_bytes,
    size_t target_artifact_size, XrModule **module, char *diagnostic,
    size_t diagnostic_size) {
    if (module)
        *module = NULL;
    if (!runtime || !module || !semantic_artifact_bytes ||
        !semantic_artifact_size || !target_artifact_bytes ||
        !target_artifact_size)
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "module load requires a runtime and both artifact images");

    XrRuntimeArtifactAuthority *artifact_authority = NULL;
    if (!xr_runtime_artifact_authority_load_xsm(
            semantic_artifact_bytes, semantic_artifact_size,
            &artifact_authority, diagnostic, diagnostic_size))
        return false;
    /* The load boundary already verified this authority. Re-deriving it here
     * costs one pass and makes the facade independent of that ordering. */
    if (!xr_runtime_artifact_authority_verify(artifact_authority, diagnostic,
                                              diagnostic_size)) {
        xr_runtime_artifact_authority_free(artifact_authority);
        return false;
    }

    XrTargetPlan *plan = NULL;
    if (!xr_runtime_target_plan_load(target_artifact_bytes,
                                     target_artifact_size, artifact_authority,
                                     &plan, diagnostic, diagnostic_size)) {
        xr_runtime_artifact_authority_free(artifact_authority);
        return false;
    }

    XrLoadedModuleGeneration *generation = NULL;
    if (!xr_module_generation_load_verified_target_plan(
            runtime->authority, plan, &generation, diagnostic,
            diagnostic_size)) {
        discard_partial_artifact(NULL, plan, artifact_authority);
        return false;
    }

    uint32_t function_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(plan, &function_count);
    XrModule *created = (XrModule *) xr_calloc(1, sizeof(*created));
    XrExport *exports =
        function_count ? (XrExport *) xr_calloc(function_count,
                                                sizeof(*exports))
                       : NULL;
    if (!functions || !function_count || !created || !exports ||
        !initialize_exports(exports, function_count, diagnostic,
                            diagnostic_size)) {
        xr_free(exports);
        xr_free(created);
        discard_partial_artifact(generation, plan, artifact_authority);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "module allocation failed or the plan declares no function");
    }
    /* A plan the installed executor does not own is rejected here, not at call
     * time, so a module handle never denotes a generation that cannot run. */
    if (!xr_module_generation_prepare(generation, diagnostic,
                                      diagnostic_size) ||
        !xr_module_generation_activate(generation, diagnostic,
                                       diagnostic_size) ||
        !bind_export_entries(generation, plan, exports, function_count,
                             &created->activation,
                             diagnostic, diagnostic_size)) {
        char ignored[1] = {0};
        clear_exports(exports, function_count, ignored, sizeof(ignored));
        dispose_exports(exports, function_count, ignored, sizeof(ignored));
        xr_free(exports);
        xr_free(created);
        discard_partial_artifact(generation, plan, artifact_authority);
        return false;
    }
    created->runtime = runtime;
    created->artifact_authority = artifact_authority;
    created->plan = plan;
    created->generation = generation;
    created->exports = exports;
    created->function_count = function_count;

    xr_mutex_lock(&runtime->gate);
    runtime->loaded_artifacts++;
    xr_mutex_unlock(&runtime->gate);
    *module = created;
    return true;
}

static bool bounded_program_plan_is_exact(
    const XrTargetPlan *plan, uint32_t semantic_module_count) {
    uint32_t graph_count = 0;
    uint32_t partition_count = 0;
    uint32_t function_count = 0;
    const XrTargetProgramGraphRecord *graphs =
        xr_target_plan_program_graphs(plan, &graph_count);
    (void) xr_target_plan_module_partitions(plan, &partition_count);
    (void) xr_target_plan_functions(plan, &function_count);
    return graphs && graph_count == 1u && partition_count == 2u &&
           partition_count == semantic_module_count &&
           graphs[0].module_count == partition_count &&
           graphs[0].function_count == 2u && graphs[0].call_count == 1u &&
           graphs[0].argument_count == 1u &&
           graphs[0].flags ==
               (XR_TARGET_PROGRAM_GRAPH_SINGLE_PLAN |
                XR_TARGET_PROGRAM_GRAPH_DIRECT_I64) &&
           graphs[0].entry_target_function < function_count &&
           graphs[0].producer_target_function < function_count &&
           graphs[0].entry_target_function !=
               graphs[0].producer_target_function &&
           xr_target_plan_function_execution_family_mask(
               plan, graphs[0].entry_target_function) ==
               XR_TARGET_EXECUTION_SCALAR_I64_CLOSED &&
           xr_target_plan_function_execution_family_mask(
               plan, graphs[0].producer_target_function) ==
               XR_TARGET_EXECUTION_SCALAR_I64_CLOSED;
}

XRAY_API bool xr_program_load_target_plan(
    XrRuntime *runtime, const XrRuntimeArtifactImage *semantic_artifacts,
    uint32_t semantic_artifact_count,
    const uint8_t *target_artifact_bytes, size_t target_artifact_size,
    XrProgram **program, char *diagnostic, size_t diagnostic_size) {
    if (program)
        *program = NULL;
    if (!runtime || !program || !semantic_artifacts ||
        semantic_artifact_count != 2u || !target_artifact_bytes ||
        !target_artifact_size)
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "program load requires a runtime, a bounded XSM module set, and one XTP");

    XrRuntimeArtifactAuthority *artifact_authority = NULL;
    if (!xr_runtime_artifact_authority_load_xsm_module_set(
            semantic_artifacts, semantic_artifact_count,
            &artifact_authority, diagnostic, diagnostic_size) ||
        !xr_runtime_artifact_authority_verify(
            artifact_authority, diagnostic, diagnostic_size)) {
        xr_runtime_artifact_authority_free(artifact_authority);
        return false;
    }
    XrTargetPlan *plan = NULL;
    if (!xr_runtime_target_plan_load(
            target_artifact_bytes, target_artifact_size, artifact_authority,
            &plan, diagnostic, diagnostic_size)) {
        xr_runtime_artifact_authority_free(artifact_authority);
        return false;
    }
    if (!bounded_program_plan_is_exact(plan, semantic_artifact_count)) {
        xr_target_plan_free(plan);
        xr_runtime_artifact_authority_free(artifact_authority);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                    "program plan is outside the bounded direct-i64 capability");
    }
    XrProgram *created = (XrProgram *) xr_calloc(1, sizeof(*created));
    XrLoadedModuleGeneration *generation = NULL;
    if (!created) {
        xr_target_plan_free(plan);
        xr_runtime_artifact_authority_free(artifact_authority);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "program allocation failed");
    }
    if (
        !xr_module_generation_load_verified_target_plan(
            runtime->authority, plan, &generation, diagnostic,
            diagnostic_size) ||
        !xr_module_generation_prepare(generation, diagnostic,
                                      diagnostic_size) ||
        !xr_module_generation_activate(generation, diagnostic,
                                       diagnostic_size)) {
        xr_free(created);
        discard_partial_artifact(generation, plan, artifact_authority);
        return false;
    }
    created->runtime = runtime;
    created->artifact_authority = artifact_authority;
    created->plan = plan;
    created->generation = generation;
    xr_mutex_init(&created->gate);
    xr_mutex_lock(&runtime->gate);
    runtime->loaded_artifacts++;
    xr_mutex_unlock(&runtime->gate);
    *program = created;
    return true;
}

XRAY_API bool xr_program_execute_direct_i64(
    const XrProgram *program, int64_t *result, char *diagnostic,
    size_t diagnostic_size) {
    if (result)
        *result = 0;
    if (!program || !program->runtime || !program->generation || !result)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "program execution requires a loaded program and result");
    /* The in-flight count is the program's own quiescence proof rather than
     * caller-visible state, so it is taken through the const handle: a caller
     * holding a read-only program may still execute it, and unload has to be
     * able to see that it did. */
    XrProgram *owner = (XrProgram *) program;
    xr_mutex_lock(&owner->gate);
    if (owner->unloading) {
        xr_mutex_unlock(&owner->gate);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "program execution requires a loaded program and result");
    }
    owner->inflight_executions++;
    xr_mutex_unlock(&owner->gate);

    bool executed = xr_module_generation_execute_program_direct_i64(
        owner->generation, result, diagnostic, diagnostic_size);

    xr_mutex_lock(&owner->gate);
    owner->inflight_executions--;
    xr_mutex_unlock(&owner->gate);
    return executed;
}

XRAY_API bool xr_program_unload(XrProgram **program, char *diagnostic,
                                size_t diagnostic_size) {
    if (!program || !*program || !(*program)->runtime)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "program is missing");
    XrProgram *owned = *program;
    /* Quiescence is decided before a single teardown step runs. A program that
     * is still executing, or whose generation still owns a pin, is refused
     * untouched, so a refused unload leaves it callable instead of stranding
     * it drained and unreachable. */
    XrModuleGenerationSnapshot snapshot;
    xr_mutex_lock(&owned->gate);
    if (owned->unloading) {
        xr_mutex_unlock(&owned->gate);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "program is already unloading");
    }
    if (owned->inflight_executions != 0) {
        xr_mutex_unlock(&owned->gate);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5006",
                    "program cannot unload while an execution is in flight");
    }
    if (!xr_module_generation_snapshot(owned->generation, &snapshot)) {
        xr_mutex_unlock(&owned->gate);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "program generation state is unreadable");
    }
    if (snapshot.total_pins != 0) {
        xr_mutex_unlock(&owned->gate);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5006",
                    "program cannot unload while its generation owns pins");
    }
    owned->unloading = true;
    xr_mutex_unlock(&owned->gate);
    if (snapshot.state == XR_MODULE_GENERATION_ACTIVE &&
        !xr_module_generation_begin_drain(owned->generation, diagnostic,
                                          diagnostic_size))
        return false;
    if (!xr_module_generation_retire(owned->generation, diagnostic,
                                     diagnostic_size) ||
        !xr_module_generation_unload(&owned->generation, diagnostic,
                                     diagnostic_size))
        return false;
    xr_mutex_destroy(&owned->gate);
    xr_target_plan_free(owned->plan);
    xr_runtime_artifact_authority_free(owned->artifact_authority);
    XrRuntime *runtime = owned->runtime;
    xr_mutex_lock(&runtime->gate);
    if (runtime->loaded_artifacts)
        runtime->loaded_artifacts--;
    xr_mutex_unlock(&runtime->gate);
    *program = NULL;
    memset(owned, 0, sizeof(*owned));
    xr_free(owned);
    return true;
}

/* The verified source export table is sorted by name and its names are unique,
 * both re-derived by the semantic verifier, so an exact match is the only
 * match and a bounded scan finds it. */
static const XrSemanticSourceExportRecord *lookup_source_export(
    const XrSemanticPlan *semantic, const char *export_name,
    uint32_t *source_export) {
    if (source_export)
        *source_export = UINT32_MAX;
    size_t count = xr_semantic_plan_source_export_count(semantic);
    for (size_t i = 0; i < count; i++) {
        const XrSemanticSourceExportRecord *record =
            xr_semantic_plan_source_export(semantic, (uint32_t) i);
        if (!record || !record->name)
            return NULL;
        int order = strcmp(record->name, export_name);
        if (order == 0) {
            if (source_export)
                *source_export = (uint32_t) i;
            return record;
        }
        if (order > 0)
            return NULL;
    }
    return NULL;
}

XRAY_API bool xr_module_find_export(const XrModule *module,
                                    const char *export_name,
                                    const XrExport **export_handle,
                                    char *diagnostic, size_t diagnostic_size) {
    if (export_handle)
        *export_handle = NULL;
    if (!module || !export_handle || !export_name || !export_name[0])
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "export lookup requires a loaded module and a nonempty name");

    /* Names are a semantic-artifact fact. The TargetPlan carries dense numeric
     * tables and no spelling at all, so resolution reads the semantic plan the
     * verified TargetPlan retains and never a name reconstructed at runtime. */
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(module->plan);
    if (!semantic)
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "module plan does not retain its verified semantic authority");
    const XrSemanticSourceExportRecord *record =
        lookup_source_export(semantic, export_name, NULL);
    if (!record)
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "module publishes no export under that exact name");
    /* Target function ids are dense and identical to their semantic function
     * index; the target verifier proves that equality, so this is a checked
     * bound rather than an assumed correspondence. */
    if (record->function >= module->function_count)
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1002",
                    "exported function index is outside the verified plan");

    XrModuleGenerationSnapshot snapshot;
    if (!xr_module_generation_snapshot(module->generation, &snapshot) ||
        snapshot.state != XR_MODULE_GENERATION_ACTIVE || snapshot.poisoned ||
        snapshot.rollback_requested)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "export resolution requires a healthy active generation");
    if (xr_target_plan_function_execution_family_mask(
            module->plan, record->function) !=
        XR_TARGET_EXECUTION_SCALAR_I64_CLOSED)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                    "exported function is outside the installed scalar i64 execution family");
    /* Activation published every source export as one transaction. Lookup is
     * read-only: a failed or duplicate registration never produced a module
     * handle that could reach this point. */
    XrExport *slot = &module->exports[record->function];
    xr_mutex_lock(&module->runtime->gate);
    if (module->unloading) {
        xr_mutex_unlock(&module->runtime->gate);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "export resolution cannot resurrect a draining module");
    }
    xr_mutex_unlock(&module->runtime->gate);
    const XrEntryCellExpectation *expectation =
        xr_runtime_entry_handle_expectation(slot->handle);
    if (!expectation || expectation->abi.schema_version !=
                            XR_ENTRY_ABI_SCHEMA_VERSION)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                    "module export registration is unavailable");
    *export_handle = slot;
    return true;
}

static bool fail_typed_dispatch(XrTypedDispatchStatus status, char *diagnostic,
                                size_t diagnostic_size) {
    switch (status) {
        case XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                        "export plan identity changed");
        case XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                        "exported function has no installed execution authority");
        case XR_TYPED_DISPATCH_FRAME_ERROR:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5001",
                        "export frame rejected a slot representation");
        case XR_TYPED_DISPATCH_ARGUMENT_MISMATCH:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                        "export argument vector does not match the verified signature");
        /* Program faults, not authority failures: the export was callable and
         * every executed row was verified. They keep their own code so an
         * operator cannot read them as a verification problem. */
        case XR_TYPED_DISPATCH_DIVIDE_BY_ZERO:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5009",
                        "exported function divided by zero");
        case XR_TYPED_DISPATCH_MODULO_BY_ZERO:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5009",
                        "exported function took a modulo by zero");
        case XR_TYPED_DISPATCH_STEP_LIMIT_EXCEEDED:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5009",
                        "exported function exceeded the executor step budget");
        case XR_TYPED_DISPATCH_CALL_DEPTH_EXCEEDED:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5009",
                        "exported function exceeded the executor call-depth budget");
        case XR_TYPED_DISPATCH_SUSPENDED:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5000",
                        "non-suspending export unexpectedly suspended");
        case XR_TYPED_DISPATCH_ENTRY_UNAVAILABLE:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                        "exported function dynamic entry is unavailable");
        case XR_TYPED_DISPATCH_ENTRY_AUTHORITY_MISMATCH:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                        "exported function dynamic entry authority changed");
        case XR_TYPED_DISPATCH_ENTRY_BUDGET_EXCEEDED:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                        "exported function dynamic entry budget is exhausted");
        case XR_TYPED_DISPATCH_ENTRY_RETIRE_DEFERRED:
            return fail(diagnostic, diagnostic_size, "XR_OWN_3003",
                        "exported function dynamic entry retirement was deferred");
        case XR_TYPED_DISPATCH_DEBUG_IDENTITY_MISMATCH:
        case XR_TYPED_DISPATCH_DEBUG_CONTROL_ERROR:
        case XR_TYPED_DISPATCH_DEBUG_TERMINATED:
        case XR_TYPED_DISPATCH_DEBUG_STOP_REJECTED:
        case XR_TYPED_DISPATCH_TRACE_REJECTED:
        case XR_TYPED_DISPATCH_INVALID_ARGUMENT:
        case XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED:
        case XR_TYPED_DISPATCH_PROGRAM_INVALID:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5000",
                        "verified export execution failed");
        case XR_TYPED_DISPATCH_OK:
            return true;
    }
    return fail(diagnostic, diagnostic_size, "XR_EXEC_5000",
                "export dispatcher returned an unknown status");
}

XRAY_API bool xr_export_call(const XrExport *export_handle,
                             const XrExportValue *arguments,
                             uint32_t argument_count, XrExportValue *result,
                             char *diagnostic, size_t diagnostic_size) {
    if (result)
        memset(result, 0, sizeof(*result));
    if (!export_handle || !result || (argument_count && !arguments))
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "export call requires a resolved handle and a result cell");
    const XrEntryCellExpectation *expectation =
        xr_runtime_entry_handle_expectation(export_handle->handle);
    XrEntryCell *cell =
        xr_runtime_entry_handle_cell(export_handle->handle);
    if (!expectation || !cell ||
        !export_handle->activation ||
        argument_count > XR_TARGET_INSTRUCTION_MAX_PARAMETERS ||
        argument_count != expectation->abi.parameter_count)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                    "export argument count does not match the verified signature");

    /* Values carry their kind so a caller built against a widened executor
     * cannot have an unowned payload silently read as an i64. */
    int64_t scalars[XR_TARGET_INSTRUCTION_MAX_PARAMETERS];
    for (uint32_t i = 0; i < argument_count; i++) {
        if (arguments[i].kind != XR_EXPORT_VALUE_I64 ||
            arguments[i].reserved != 0)
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5001",
                        "export argument kind is outside the installed scalar i64 family");
        scalars[i] = arguments[i].i64;
    }

    if (!xr_runtime_activation_provider_acquire(
            export_handle->activation, XR_TARGET_PROVIDER_ALLOCATOR,
            diagnostic, diagnostic_size))
        return false;
    if (!xr_runtime_activation_provider_acquire(
            export_handle->activation, XR_TARGET_PROVIDER_PANIC,
            diagnostic, diagnostic_size)) {
        char nested[256] = {0};
        xr_runtime_activation_provider_release(
            export_handle->activation, nested, sizeof(nested));
        return false;
    }

    int64_t executed = 0;
    uint32_t executor_status = 0;
    XrEntryInvokeStatus status = xr_entry_cell_invoke_i64(
        cell, expectation,
        argument_count ? scalars : NULL, argument_count, &executed,
        &executor_status, diagnostic, diagnostic_size);
    char release_diagnostic[256] = {0};
    bool panic_released = xr_runtime_activation_provider_release(
        export_handle->activation, release_diagnostic,
        sizeof(release_diagnostic));
    bool allocator_released = xr_runtime_activation_provider_release(
        export_handle->activation, release_diagnostic,
        sizeof(release_diagnostic));
    if (!panic_released || !allocator_released)
        return fail(diagnostic, diagnostic_size, "XR_OWN_3003",
                    "export provider lease release failed");
    if (status == XR_ENTRY_INVOKE_VM_ERROR)
        return fail_typed_dispatch((XrTypedDispatchStatus) executor_status,
                                   diagnostic, diagnostic_size);
    if (status != XR_ENTRY_INVOKE_OK) {
        if (status == XR_ENTRY_INVOKE_NATIVE_ERROR ||
            status == XR_ENTRY_INVOKE_CANCELLED)
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5009",
                        "export entry returned an unsupported native outcome");
        return false;
    }
    result->kind = XR_EXPORT_VALUE_I64;
    result->i64 = executed;
    return true;
}

XRAY_API bool xr_module_unload(XrModule **module, char *diagnostic,
                               size_t diagnostic_size) {
    if (!module || !*module || !(*module)->runtime)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "module is missing");
    XrModule *owned = *module;
    xr_mutex_lock(&owned->runtime->gate);
    owned->unloading = true;
    xr_mutex_unlock(&owned->runtime->gate);
    for (uint32_t i = 0; i < owned->function_count; i++) {
        if (!xr_runtime_entry_registry_unpublish(
                owned->generation->authority, owned->exports[i].handle,
                diagnostic, diagnostic_size))
            return false;
    }
    /* Clearing every cell first releases the published static-root pins and
     * prevents a new call from starting. An already acquired call keeps its
     * independent in-flight pin, so retirement below refuses until it exits. */
    if (!clear_exports(owned->exports, owned->function_count, diagnostic,
                       diagnostic_size))
        return false;
    /* Retirement is the lifecycle's own gate: it refuses while any pin
     * remains, so an in-flight call keeps this from succeeding rather than
     * being torn out from under. Draining is entered only from ACTIVE, and a
     * previous unload that a pin refused already left this generation
     * draining, so that earlier attempt must not strand the module. */
    XrModuleGenerationSnapshot snapshot;
    if (!xr_module_generation_snapshot(owned->generation, &snapshot))
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "module generation state is unreadable");
    if (snapshot.state == XR_MODULE_GENERATION_ACTIVE &&
        !xr_module_generation_begin_drain(owned->generation, diagnostic,
                                          diagnostic_size))
        return false;
    if (!xr_module_generation_retire(owned->generation, diagnostic,
                                     diagnostic_size) ||
        !xr_runtime_activation_unpublish(&owned->activation, diagnostic,
                                         diagnostic_size) ||
        !xr_module_generation_unload(&owned->generation, diagnostic,
                                     diagnostic_size))
        return false;
    if (!dispose_exports(owned->exports, owned->function_count, diagnostic,
                         diagnostic_size))
        return false;
    xr_target_plan_free(owned->plan);
    xr_runtime_artifact_authority_free(owned->artifact_authority);

    XrRuntime *runtime = owned->runtime;
    xr_mutex_lock(&runtime->gate);
    if (runtime->loaded_artifacts)
        runtime->loaded_artifacts--;
    xr_mutex_unlock(&runtime->gate);

    xr_free(owned->exports);
    memset(owned, 0, sizeof(*owned));
    xr_free(owned);
    *module = NULL;
    return true;
}
