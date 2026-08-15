/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_compiler_session_generation.c - Compiler session generation tests
 */

#include "../test_framework.h"
#include "base/xarena.h"
#include "incremental/xr_cache_invalidate.h"
#include "incremental/xr_cache_store.h"
#include "os/os_dir.h"
#include "os/os_fs.h"
#include "os/os_temp.h"
#include "toolchain/xcompiler_session.h"
#include "api/xrepl.h"
#include "runtime/xisolate_api.h"
#include "runtime/value/xchunk.h"
#include "xray_vm.h"

#include <stdio.h>
#include <string.h>

static XrFingerprint test_fingerprint(uint8_t seed) {
    XrFingerprint fingerprint;
    for (size_t i = 0; i < sizeof(fingerprint.bytes); i++)
        fingerprint.bytes[i] = (uint8_t) (seed + (uint8_t) i);
    return fingerprint;
}

static bool init_summary(XrModuleSummary *summary, const char *key, uint8_t seed) {
    if (!xr_module_summary_init(summary, key))
        return false;
    for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++) {
        if (!xr_module_summary_set_fingerprint(
                summary, (XrModuleSummaryFacet) facet,
                test_fingerprint((uint8_t) (seed + facet)))) {
            xr_module_summary_finalize(summary);
            return false;
        }
    }
    return true;
}

static bool build_session_graph(XrDependencyGraph *graph, XrStableId *root_id,
                                XrStableId *consumer_id) {
    XrModuleSummary root = {0};
    XrModuleSummary consumer = {0};
    xr_dependency_graph_init(graph);
    if (!init_summary(&root, "pkg/root", 1) ||
        !init_summary(&consumer, "pkg/consumer", 40)) {
        xr_module_summary_finalize(&root);
        xr_module_summary_finalize(&consumer);
        return false;
    }
    *root_id = root.module_id;
    *consumer_id = consumer.module_id;
    bool ok = xr_dependency_graph_add_node(graph, &root) &&
              xr_dependency_graph_add_node(graph, &consumer);
    XrModuleFacetMask relation[XR_MODULE_FACET_COUNT] = {0};
    relation[XR_MODULE_FACET_PUBLIC_SIGNATURE] =
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_PUBLIC_SIGNATURE);
    if (ok)
        ok = xr_dependency_graph_add_edge(graph, *consumer_id, *root_id, relation);
    xr_module_summary_finalize(&consumer);
    xr_module_summary_finalize(&root);
    if (!ok) {
        xr_dependency_graph_finalize(graph);
        return false;
    }
    return xr_dependency_graph_validate(graph);
}

static bool change_root_signature(XrCompilerSession *session, uint8_t seed) {
    const XrDependencyGraph *graph = xr_compiler_session_dependency_graph(session);
    const XrModuleSummary *root =
        graph && graph->node_count ? xr_dependency_graph_node_at(graph, 0) : NULL;
    XrModuleSummary replacement;
    if (!root || !xr_module_summary_copy(&replacement, root))
        return false;
    bool ok = xr_module_summary_set_fingerprint(
        &replacement, XR_MODULE_FACET_PUBLIC_SIGNATURE, test_fingerprint(seed));
    XrInvalidationEvent event = {
        .reason = XR_INVALIDATION_SUMMARY_CHANGED,
        .root_id = root->module_id,
        .replacement_summary = &replacement,
    };
    if (ok)
        ok = xr_compiler_session_apply_invalidation(session, &event);
    xr_module_summary_finalize(&replacement);
    return ok;
}

static void remove_empty_cache_root(const char *root) {
    char path[XR_PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/xsm", root) > 0)
        (void) xr_test_rmdir(path);
    if (snprintf(path, sizeof(path), "%s/xtp", root) > 0)
        (void) xr_test_rmdir(path);
    if (snprintf(path, sizeof(path), "%s/.cache-root.lock", root) > 0)
        (void) xr_test_unlink(path);
    (void) xr_test_rmdir(root);
}

static bool snapshots_equal(XrCompilerSessionGenerationSnapshot left,
                            XrCompilerSessionGenerationSnapshot right) {
    return left.session_generation == right.session_generation &&
           left.workspace_generation == right.workspace_generation &&
           left.configuration_generation == right.configuration_generation &&
           left.target_generation == right.target_generation &&
           left.provider_generation == right.provider_generation;
}

TEST(repl_declaration_generations_publish_and_abandon_without_reuse) {
    XrCompilerSessionConfig config = {.repl_mode = true};
    XrCompilerSession *session = xr_compiler_session_new(&config);
    ASSERT_NOT_NULL(session);
    XrCompilerSessionReplGenerationSnapshot initial =
        xr_compiler_session_repl_generation_snapshot(session);
    ASSERT_EQ_UINT(initial.next_generation,
                   XR_COMPILER_SESSION_INITIAL_REPL_DECLARATION_GENERATION);
    ASSERT_EQ_UINT(initial.published_generation, 0);
    ASSERT_EQ_UINT(initial.attempted_count, 0);
    ASSERT_FALSE(initial.active);

    XrCompilerSessionReplDeclarationScope first = {0};
    ASSERT_TRUE(xr_compiler_session_repl_declaration_begin(session, &first));
    ASSERT_EQ_UINT(first.generation, 1);
    XrCompilerSessionReplGenerationSnapshot active =
        xr_compiler_session_repl_generation_snapshot(session);
    ASSERT_EQ_UINT(active.next_generation, 2);
    ASSERT_EQ_UINT(active.attempted_count, 1);
    ASSERT_TRUE(active.active);
    ASSERT_FALSE(xr_compiler_session_apply_generation_change(
        session, XR_COMPILER_SESSION_CHANGE_SESSION));
    ASSERT_FALSE(xr_compiler_session_reset_incremental(session));
    XrCompilerSessionOperationScope operation = {0};
    ASSERT_FALSE(xr_compiler_session_operation_begin(session, &operation));
    XrCompilerSessionReplDeclarationScope overlapping = {0};
    ASSERT_FALSE(xr_compiler_session_repl_declaration_begin(session, &overlapping));

    XrCompilerSessionReplDeclarationScope forged = first;
    forged.generation++;
    ASSERT_FALSE(xr_compiler_session_repl_declaration_publish(&forged, 2));
    ASSERT_TRUE(xr_compiler_session_repl_generation_snapshot(session).active);
    ASSERT_TRUE(xr_compiler_session_repl_declaration_publish(&first, 2));

    XrCompilerSessionReplDeclarationScope second = {0};
    ASSERT_TRUE(xr_compiler_session_repl_declaration_begin(session, &second));
    ASSERT_EQ_UINT(second.generation, 2);
    ASSERT_TRUE(xr_compiler_session_repl_declaration_abandon(
        &second, XR_COMPILER_SESSION_REPL_DECLARATION_ABANDONED_COMPILE));

    XrCompilerSessionReplDeclarationScope third = {0};
    ASSERT_TRUE(xr_compiler_session_repl_declaration_begin(session, &third));
    ASSERT_EQ_UINT(third.generation, 3);
    ASSERT_TRUE(xr_compiler_session_repl_declaration_publish(&third, 1));

    XrCompilerSessionReplGenerationSnapshot final =
        xr_compiler_session_repl_generation_snapshot(session);
    ASSERT_EQ_UINT(final.next_generation, 4);
    ASSERT_EQ_UINT(final.published_generation, 3);
    ASSERT_EQ_UINT(final.attempted_count, 3);
    ASSERT_EQ_UINT(final.published_count, 2);
    ASSERT_EQ_UINT(final.abandoned_count, 1);
    ASSERT_FALSE(final.active);

    XrCompilerSessionReplDeclarationRecord record = {0};
    ASSERT_TRUE(xr_compiler_session_repl_declaration_at(session, 0, &record));
    ASSERT_EQ_UINT(record.generation, 1);
    ASSERT_EQ_UINT(record.parent_generation, 0);
    ASSERT_EQ_UINT(record.statement_count, 2);
    ASSERT_EQ_UINT(record.state, XR_COMPILER_SESSION_REPL_DECLARATION_PUBLISHED);
    ASSERT_TRUE(xr_compiler_session_repl_declaration_at(session, 1, &record));
    ASSERT_EQ_UINT(record.generation, 2);
    ASSERT_EQ_UINT(record.parent_generation, 1);
    ASSERT_EQ_UINT(record.statement_count, 0);
    ASSERT_EQ_UINT(record.state,
                   XR_COMPILER_SESSION_REPL_DECLARATION_ABANDONED_COMPILE);
    ASSERT_TRUE(xr_compiler_session_repl_declaration_at(session, 2, &record));
    ASSERT_EQ_UINT(record.generation, 3);
    ASSERT_EQ_UINT(record.parent_generation, 1);
    ASSERT_EQ_UINT(record.statement_count, 1);
    ASSERT_EQ_UINT(record.state, XR_COMPILER_SESSION_REPL_DECLARATION_PUBLISHED);
    ASSERT_FALSE(xr_compiler_session_repl_declaration_at(session, 3, &record));
    ASSERT_FALSE(xr_compiler_session_repl_declaration_at(session, 0, NULL));
    xr_compiler_session_delete(session);
}

TEST(production_repl_eval_publishes_and_abandons_declaration_generations) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(isolate);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    ASSERT_NOT_NULL(session);

    XrReplEvalResult first =
        xr_repl_eval(session, isolate, "var generation_value = 41\n");
    ASSERT_EQ_UINT(first.status, XR_REPL_EVAL_OK);
    ASSERT_NOT_NULL(first.proto);
    XrReplEvalResult rejected = xr_repl_eval(session, isolate, "var it = 1\n");
    ASSERT_EQ_UINT(rejected.status, XR_REPL_EVAL_COMPILE_ERROR);
    ASSERT_NULL(rejected.proto);
    XrReplEvalResult third = xr_repl_eval(
        session, isolate, "var generation_next = generation_value + 1\n");
    ASSERT_EQ_UINT(third.status, XR_REPL_EVAL_OK);
    ASSERT_NOT_NULL(third.proto);
    XrReplEvalResult runtime_rejected = xr_repl_eval(
        session, isolate, "enum ReplGenerationError { Bad }\n"
                          "throw ReplGenerationError.Bad\n");
    ASSERT_EQ_UINT(runtime_rejected.status, XR_REPL_EVAL_RUNTIME_ERROR);
    ASSERT_NOT_NULL(runtime_rejected.proto);

    XrCompilerSessionReplGenerationSnapshot snapshot =
        xr_compiler_session_repl_generation_snapshot(session);
    ASSERT_EQ_UINT(snapshot.next_generation, 5);
    ASSERT_EQ_UINT(snapshot.published_generation, 3);
    ASSERT_EQ_UINT(snapshot.attempted_count, 4);
    ASSERT_EQ_UINT(snapshot.published_count, 2);
    ASSERT_EQ_UINT(snapshot.abandoned_count, 2);
    ASSERT_FALSE(snapshot.active);

    XrCompilerSessionReplDeclarationRecord record = {0};
    ASSERT_TRUE(xr_compiler_session_repl_declaration_at(session, 0, &record));
    ASSERT_EQ_UINT(record.generation, 1);
    ASSERT_EQ_UINT(record.parent_generation, 0);
    ASSERT_EQ_UINT(record.statement_count, 1);
    ASSERT_EQ_UINT(record.state, XR_COMPILER_SESSION_REPL_DECLARATION_PUBLISHED);
    ASSERT_TRUE(xr_compiler_session_repl_declaration_at(session, 3, &record));
    ASSERT_EQ_UINT(record.generation, 4);
    ASSERT_EQ_UINT(record.parent_generation, 3);
    ASSERT_EQ_UINT(record.statement_count, 0);
    ASSERT_EQ_UINT(record.state,
                   XR_COMPILER_SESSION_REPL_DECLARATION_ABANDONED_RUNTIME);
    ASSERT_TRUE(xr_compiler_session_repl_declaration_at(session, 1, &record));
    ASSERT_EQ_UINT(record.generation, 2);
    ASSERT_EQ_UINT(record.parent_generation, 1);
    ASSERT_EQ_UINT(record.state,
                   XR_COMPILER_SESSION_REPL_DECLARATION_ABANDONED_COMPILE);
    ASSERT_TRUE(xr_compiler_session_repl_declaration_at(session, 2, &record));
    ASSERT_EQ_UINT(record.generation, 3);
    ASSERT_EQ_UINT(record.parent_generation, 1);
    ASSERT_EQ_UINT(record.statement_count, 1);
    ASSERT_EQ_UINT(record.state, XR_COMPILER_SESSION_REPL_DECLARATION_PUBLISHED);

    int64_t value = 0;
    ASSERT_TRUE(xr_repl_peek_int(isolate, "generation_next", &value));
    ASSERT_EQ_INT(value, 42);
    xr_free_code(isolate, runtime_rejected.proto);
    xr_free_code(isolate, third.proto);
    xr_free_code(isolate, first.proto);
    xray_vm_delete(isolate);
}

TEST(generation_replay_is_deterministic) {
    XrCompilerSession *first = xr_compiler_session_new(NULL);
    XrCompilerSession *second = xr_compiler_session_new(NULL);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);

    XrCompilerSessionGenerationSnapshot initial = {
        .session_generation = XR_COMPILER_SESSION_INITIAL_GENERATION,
        .workspace_generation = XR_COMPILER_SESSION_INITIAL_GENERATION,
        .configuration_generation = XR_COMPILER_SESSION_INITIAL_GENERATION,
        .target_generation = XR_COMPILER_SESSION_INITIAL_GENERATION,
        .provider_generation = XR_COMPILER_SESSION_INITIAL_GENERATION,
    };
    ASSERT_TRUE(snapshots_equal(xr_compiler_session_generation_snapshot(first), initial));
    ASSERT_TRUE(snapshots_equal(xr_compiler_session_generation_snapshot(second), initial));

    const uint32_t replay[] = {
        XR_COMPILER_SESSION_CHANGE_CONFIGURATION,
        XR_COMPILER_SESSION_CHANGE_TARGET | XR_COMPILER_SESSION_CHANGE_PROVIDER,
        XR_COMPILER_SESSION_CHANGE_WORKSPACE,
        XR_COMPILER_SESSION_CHANGE_NONE,
    };
    for (size_t i = 0; i < sizeof(replay) / sizeof(replay[0]); i++) {
        ASSERT_TRUE(xr_compiler_session_apply_generation_change(first, replay[i]));
        ASSERT_TRUE(xr_compiler_session_apply_generation_change(second, replay[i]));
        ASSERT_TRUE(snapshots_equal(xr_compiler_session_generation_snapshot(first),
                                    xr_compiler_session_generation_snapshot(second)));
    }

    xr_compiler_session_delete(second);
    xr_compiler_session_delete(first);
}

TEST(generation_changes_are_domain_specific_and_transactional) {
    XrCompilerSession *session = xr_compiler_session_new(NULL);
    ASSERT_NOT_NULL(session);
    XrCompilerSessionGenerationSnapshot before =
        xr_compiler_session_generation_snapshot(session);

    ASSERT_TRUE(xr_compiler_session_apply_generation_change(
        session, XR_COMPILER_SESSION_CHANGE_CONFIGURATION | XR_COMPILER_SESSION_CHANGE_PROVIDER));
    XrCompilerSessionGenerationSnapshot changed =
        xr_compiler_session_generation_snapshot(session);
    ASSERT_EQ_UINT(changed.session_generation, before.session_generation);
    ASSERT_EQ_UINT(changed.workspace_generation, before.workspace_generation);
    ASSERT_EQ_UINT(changed.configuration_generation, before.configuration_generation + 1);
    ASSERT_EQ_UINT(changed.target_generation, before.target_generation);
    ASSERT_EQ_UINT(changed.provider_generation, before.provider_generation + 1);

    ASSERT_TRUE(xr_compiler_session_apply_generation_change(
        session, XR_COMPILER_SESSION_CHANGE_NONE));
    ASSERT_TRUE(snapshots_equal(xr_compiler_session_generation_snapshot(session), changed));
    ASSERT_FALSE(xr_compiler_session_apply_generation_change(session, 1u << 31));
    ASSERT_TRUE(snapshots_equal(xr_compiler_session_generation_snapshot(session), changed));

    xr_compiler_session_delete(session);
}

TEST(incremental_reset_advances_identity_and_clears_transient_state) {
    XrCompilerSession *session = xr_compiler_session_new(NULL);
    ASSERT_NOT_NULL(session);
    XrCompileUnitIdentity identity = {
        .kind = XR_COMPILE_UNIT_STDLIB,
        .canonical_module = "stdlib:probe",
    };
    XrArena arena;
    XrCompilerSessionScope scope;
    xr_arena_init(&arena, 1024);
    ASSERT_TRUE(xr_compiler_session_push_arena(session, &arena, "reset.xr", &scope));
    xr_compiler_session_set_module_graph(session, (struct XrModuleGraph *) (uintptr_t) 3);
    xr_compiler_session_set_compile_unit_identity(session, &identity);
    xr_compiler_session_set_ast_node_id(session, 41);
    XrCompilerSessionGenerationSnapshot before =
        xr_compiler_session_generation_snapshot(session);

    ASSERT_TRUE(xr_compiler_session_reset_incremental(session));
    XrCompilerSessionGenerationSnapshot after =
        xr_compiler_session_generation_snapshot(session);
    ASSERT_EQ_UINT(after.session_generation, before.session_generation + 1);
    ASSERT_EQ_UINT(after.workspace_generation, before.workspace_generation + 1);
    ASSERT_EQ_UINT(after.configuration_generation, before.configuration_generation);
    ASSERT_EQ_UINT(after.target_generation, before.target_generation);
    ASSERT_EQ_UINT(after.provider_generation, before.provider_generation);
    ASSERT_NULL(xr_compiler_session_current_arena(session));
    ASSERT_NULL(xr_compiler_session_string_pool(session));
    ASSERT_NULL(xr_compiler_session_module_graph(session));
    ASSERT_EQ_UINT(xr_compiler_session_ast_node_id(session), 0);
    ASSERT_NULL(xr_compiler_session_compile_unit_identity(session).canonical_module);
    xr_compiler_session_pop_arena(&scope);
    ASSERT_NULL(xr_compiler_session_current_arena(session));
    ASSERT_NULL(xr_compiler_session_string_pool(session));

    xr_arena_destroy(&arena);
    xr_compiler_session_delete(session);
}

TEST(session_owns_configuration_paths) {
    char project_root[] = "workspace/root";
    char source_file[] = "src/main.xr";
    XrCompilerSessionConfig config = {
        .project_root = project_root,
        .source_file = source_file,
    };
    XrCompilerSession *session = xr_compiler_session_new(&config);
    ASSERT_NOT_NULL(session);
    ASSERT_NE(xr_compiler_session_project_root(session), project_root);
    ASSERT_NE(xr_compiler_session_source_file(session), source_file);

    project_root[0] = 'X';
    source_file[0] = 'Y';
    ASSERT_STR_EQ(xr_compiler_session_project_root(session), "workspace/root");
    ASSERT_STR_EQ(xr_compiler_session_source_file(session), "src/main.xr");
    ASSERT_TRUE(xr_compiler_session_reset_incremental(session));
    ASSERT_STR_EQ(xr_compiler_session_project_root(session), "workspace/root");
    ASSERT_STR_EQ(xr_compiler_session_source_file(session), "src/main.xr");

    xr_compiler_session_delete(session);
}

TEST(target_and_provider_updates_advance_only_their_generation) {
    XrCompilerSession *session = xr_compiler_session_new(NULL);
    ASSERT_NOT_NULL(session);
    XrCompilerSessionGenerationSnapshot before =
        xr_compiler_session_generation_snapshot(session);
    XrTargetDataLayout layout;
    ASSERT_TRUE(xr_target_data_layout_init_native(&layout));
    ASSERT_TRUE(xr_compiler_session_set_target_data_layout(session, &layout));
    XrCompilerSessionGenerationSnapshot target =
        xr_compiler_session_generation_snapshot(session);
    ASSERT_EQ_UINT(target.target_generation, before.target_generation + 1);
    ASSERT_EQ_UINT(target.provider_generation, before.provider_generation);
    xr_compiler_session_set_native_package_plan(
        session, (const struct XrNativePackagePlan *) (uintptr_t) 1);
    XrCompilerSessionGenerationSnapshot provider =
        xr_compiler_session_generation_snapshot(session);
    ASSERT_EQ_UINT(provider.target_generation, target.target_generation);
    ASSERT_EQ_UINT(provider.provider_generation, target.provider_generation + 1);
    xr_compiler_session_delete(session);
}

TEST(session_owns_dependency_graph_and_isolates_workspaces) {
    XrDependencyGraph source;
    XrStableId root_id;
    XrStableId consumer_id;
    ASSERT_TRUE(build_session_graph(&source, &root_id, &consumer_id));
    XrCompilerSession *first = xr_compiler_session_new(NULL);
    XrCompilerSession *second = xr_compiler_session_new(NULL);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);

    ASSERT_TRUE(xr_compiler_session_publish_dependency_graph(first, &source));
    ASSERT_TRUE(xr_compiler_session_publish_dependency_graph(second, &source));
    const XrDependencyGraph *owned = xr_compiler_session_dependency_graph(first);
    ASSERT_NOT_NULL(owned);
    ASSERT_NE(owned->nodes, source.nodes);
    ASSERT_NE(owned->edges, source.edges);
    ASSERT_NOT_NULL(xr_dependency_graph_find_node(owned, root_id));
    xr_dependency_graph_finalize(&source);
    ASSERT_NOT_NULL(xr_dependency_graph_find_node(
        xr_compiler_session_dependency_graph(first), root_id));

    ASSERT_TRUE(change_root_signature(first, 210));
    ASSERT_EQ_UINT(xr_compiler_session_incremental_stats(first).invalidation_history_count, 1);
    ASSERT_EQ_UINT(xr_compiler_session_incremental_stats(second).invalidation_history_count, 0);
    const XrModuleSummary *unchanged = xr_dependency_graph_find_node(
        xr_compiler_session_dependency_graph(second), root_id);
    ASSERT_NOT_NULL(unchanged);
    ASSERT_MEM_EQ(unchanged->facets[XR_MODULE_FACET_PUBLIC_SIGNATURE].bytes,
                  test_fingerprint((uint8_t) (1 + XR_MODULE_FACET_PUBLIC_SIGNATURE)).bytes,
                  sizeof(XrFingerprint));
    ASSERT_NOT_NULL(xr_dependency_graph_find_node(
        xr_compiler_session_dependency_graph(second), consumer_id));

    xr_compiler_session_delete(second);
    xr_compiler_session_delete(first);
}

typedef struct SessionModuleTaskState {
    uint32_t task_index;
    XrFingerprint artifact;
} SessionModuleTaskState;

typedef struct SessionModuleTaskContext {
    uint32_t fail_first;
    uint32_t fail_second;
    uint32_t fail_preflight_task;
    uint32_t fail_publish_task;
    bool fail;
    bool fail_preflight;
    bool fail_publish;
    uint32_t publish_order[8];
    XrFingerprint artifacts[8];
    char diagnostics[8][XR_MODULE_TASK_DIAGNOSTIC_CAPACITY];
    bool cache_present[8];
    XrFingerprint cache_artifacts[8];
    uint32_t exact_revalidations;
    uint32_t publish_count;
} SessionModuleTaskContext;

static bool prepare_session_module_task(const XrModuleTaskGraph *graph,
                                        uint32_t task_index,
                                        XrModuleTaskOutput *output,
                                        void *task_state, void *context) {
    SessionModuleTaskState *state = (SessionModuleTaskState *) task_state;
    const SessionModuleTaskContext *run =
        (const SessionModuleTaskContext *) context;
    XrModuleTaskView view;
    if (!state || !run ||
        !xr_module_task_graph_task(graph, task_index, &view))
        return false;
    state->task_index = task_index;
    state->artifact = test_fingerprint(
        (uint8_t) (80u + task_index + view.level * 9u));
    output->artifact_fingerprint = state->artifact;
    (void) snprintf(output->diagnostic, sizeof(output->diagnostic),
                    "task=%u level=%u", task_index, view.level);
    return !run->fail ||
           (task_index != run->fail_first &&
            task_index != run->fail_second);
}

static bool preflight_session_module_task(
    const XrModuleTaskGraph *graph, uint32_t task_index,
    const XrModuleTaskOutput *output, const void *task_state, void *context,
    char *error, size_t error_size) {
    (void) graph;
    (void) error;
    (void) error_size;
    const SessionModuleTaskState *state =
        (const SessionModuleTaskState *) task_state;
    const SessionModuleTaskContext *run =
        (const SessionModuleTaskContext *) context;
    return state && run && output && output->complete && output->succeeded &&
           state->task_index == task_index &&
           xr_fingerprint_equal(state->artifact,
                                output->artifact_fingerprint) &&
           (!run->fail_preflight ||
            task_index != run->fail_preflight_task);
}

static bool publish_session_module_task(
    const XrModuleTaskGraph *graph, uint32_t task_index,
    const XrModuleTaskOutput *output, void *task_state, void *context,
    char *error, size_t error_size) {
    (void) graph;
    (void) error;
    (void) error_size;
    SessionModuleTaskState *state = (SessionModuleTaskState *) task_state;
    SessionModuleTaskContext *run = (SessionModuleTaskContext *) context;
    if (!state || !run || !output || !output->complete ||
        !output->succeeded || state->task_index != task_index ||
        run->publish_count >= 8u ||
        (run->fail_publish && task_index == run->fail_publish_task))
        return false;
    if (run->cache_present[task_index]) {
        if (!xr_fingerprint_equal(run->cache_artifacts[task_index],
                                  state->artifact))
            return false;
        run->exact_revalidations++;
    } else {
        run->cache_present[task_index] = true;
        run->cache_artifacts[task_index] = state->artifact;
    }
    uint32_t cursor = run->publish_count++;
    run->publish_order[cursor] = task_index;
    run->artifacts[cursor] = state->artifact;
    (void) snprintf(run->diagnostics[cursor],
                    sizeof(run->diagnostics[cursor]), "%s",
                    output->diagnostic);
    return xr_fingerprint_equal(state->artifact,
                                output->artifact_fingerprint);
}

static bool build_parallel_session_graph(XrDependencyGraph *graph) {
    XrModuleSummary left = {0};
    XrModuleSummary right = {0};
    XrModuleSummary join = {0};
    xr_dependency_graph_init(graph);
    if (!init_summary(&left, "pkg/left", 10u) ||
        !init_summary(&right, "pkg/right", 40u) ||
        !init_summary(&join, "pkg/join", 70u))
        goto fail;
    XrModuleFacetMask relation[XR_MODULE_FACET_COUNT] = {0};
    relation[XR_MODULE_FACET_BODY_EVIDENCE] =
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE);
    bool ok = xr_dependency_graph_add_node(graph, &join) &&
              xr_dependency_graph_add_node(graph, &right) &&
              xr_dependency_graph_add_node(graph, &left) &&
              xr_dependency_graph_add_edge(graph, join.module_id,
                                           left.module_id, relation) &&
              xr_dependency_graph_add_edge(graph, join.module_id,
                                           right.module_id, relation) &&
              xr_dependency_graph_validate(graph);
    xr_module_summary_finalize(&join);
    xr_module_summary_finalize(&right);
    xr_module_summary_finalize(&left);
    return ok;
fail:
    xr_module_summary_finalize(&join);
    xr_module_summary_finalize(&right);
    xr_module_summary_finalize(&left);
    xr_dependency_graph_finalize(graph);
    return false;
}

static bool run_session_module_tasks(
    XrCompilerSession *session, const XrDependencyGraph *graph,
    uint32_t worker_limit, SessionModuleTaskContext *context,
    XrCompilerSessionModuleTaskStats *stats, char *error,
    size_t error_size) {
    XrCompilerSessionModuleTaskBatch batch = {
        .dependency_graph = graph,
        .worker_limit = worker_limit,
        .task_state_size = sizeof(SessionModuleTaskState),
        .prepare = prepare_session_module_task,
        .preflight = preflight_session_module_task,
        .publish = publish_session_module_task,
        .context = context,
    };
    return xr_compiler_session_publish_module_tasks(
        session, &batch, stats, error, error_size);
}

TEST(module_task_session_is_worker_stable_and_publishes_atomically) {
    XrDependencyGraph graph;
    ASSERT_TRUE(build_parallel_session_graph(&graph));
    XrCompilerSession *serial = xr_compiler_session_new(NULL);
    XrCompilerSession *dual = xr_compiler_session_new(NULL);
    XrCompilerSession *wide = xr_compiler_session_new(NULL);
    ASSERT_NOT_NULL(serial);
    ASSERT_NOT_NULL(dual);
    ASSERT_NOT_NULL(wide);
    SessionModuleTaskContext serial_run = {0};
    SessionModuleTaskContext dual_run = {0};
    SessionModuleTaskContext wide_run = {0};
    XrCompilerSessionModuleTaskStats serial_stats;
    XrCompilerSessionModuleTaskStats dual_stats;
    XrCompilerSessionModuleTaskStats wide_stats;
    char serial_error[512];
    char dual_error[512];
    char wide_error[512];
    ASSERT_TRUE(run_session_module_tasks(serial, &graph, 1u, &serial_run,
                                         &serial_stats, serial_error,
                                         sizeof(serial_error)));
    ASSERT_TRUE(run_session_module_tasks(dual, &graph, 2u, &dual_run,
                                         &dual_stats, dual_error,
                                         sizeof(dual_error)));
    ASSERT_TRUE(run_session_module_tasks(wide, &graph, 8u, &wide_run,
                                         &wide_stats, wide_error,
                                         sizeof(wide_error)));
    ASSERT_EQ_UINT(serial_stats.task_count, 3u);
    ASSERT_EQ_UINT(serial_stats.execution.worker_count, 1u);
    ASSERT_EQ_UINT(dual_stats.execution.worker_count, 2u);
    ASSERT_EQ_UINT(wide_stats.execution.worker_count, 2u);
    ASSERT_EQ_UINT(serial_run.publish_count, 3u);
    ASSERT_EQ_UINT(dual_run.publish_count, serial_run.publish_count);
    ASSERT_EQ_UINT(wide_run.publish_count, serial_run.publish_count);
    for (uint32_t task = 0; task < serial_run.publish_count; task++) {
        ASSERT_EQ_UINT(dual_run.publish_order[task],
                       serial_run.publish_order[task]);
        ASSERT_EQ_UINT(wide_run.publish_order[task],
                       serial_run.publish_order[task]);
        ASSERT_TRUE(xr_fingerprint_equal(dual_run.artifacts[task],
                                         serial_run.artifacts[task]));
        ASSERT_TRUE(xr_fingerprint_equal(wide_run.artifacts[task],
                                         serial_run.artifacts[task]));
        ASSERT_TRUE(strcmp(dual_run.diagnostics[task],
                           serial_run.diagnostics[task]) == 0);
        ASSERT_TRUE(strcmp(wide_run.diagnostics[task],
                           serial_run.diagnostics[task]) == 0);
    }

    XrFingerprint serial_graph;
    XrFingerprint dual_graph;
    XrFingerprint wide_graph;
    ASSERT_TRUE(xr_dependency_graph_fingerprint(
        xr_compiler_session_dependency_graph(serial), &serial_graph));
    ASSERT_TRUE(xr_dependency_graph_fingerprint(
        xr_compiler_session_dependency_graph(dual), &dual_graph));
    ASSERT_TRUE(xr_dependency_graph_fingerprint(
        xr_compiler_session_dependency_graph(wide), &wide_graph));
    ASSERT_TRUE(xr_fingerprint_equal(serial_graph, dual_graph));
    ASSERT_TRUE(xr_fingerprint_equal(serial_graph, wide_graph));

    XrCompilerSessionGenerationSnapshot serial_before =
        xr_compiler_session_generation_snapshot(serial);
    XrCompilerSessionGenerationSnapshot dual_before =
        xr_compiler_session_generation_snapshot(dual);
    XrCompilerSessionGenerationSnapshot wide_before =
        xr_compiler_session_generation_snapshot(wide);
    SessionModuleTaskContext serial_failure = {
        .fail_first = 0u,
        .fail_second = 1u,
        .fail = true,
    };
    SessionModuleTaskContext dual_failure = serial_failure;
    SessionModuleTaskContext wide_failure = serial_failure;
    ASSERT_FALSE(run_session_module_tasks(
        serial, &graph, 1u, &serial_failure, &serial_stats, serial_error,
        sizeof(serial_error)));
    ASSERT_FALSE(run_session_module_tasks(
        dual, &graph, 2u, &dual_failure, &dual_stats, dual_error,
        sizeof(dual_error)));
    ASSERT_FALSE(run_session_module_tasks(
        wide, &graph, 8u, &wide_failure, &wide_stats, wide_error,
        sizeof(wide_error)));
    ASSERT_EQ_UINT(serial_failure.publish_count, 0u);
    ASSERT_EQ_UINT(dual_failure.publish_count, 0u);
    ASSERT_EQ_UINT(wide_failure.publish_count, 0u);
    ASSERT_EQ_UINT(serial_stats.execution.first_failed_task, 0u);
    ASSERT_EQ_UINT(dual_stats.execution.first_failed_task, 0u);
    ASSERT_EQ_UINT(wide_stats.execution.first_failed_task, 0u);
    ASSERT_TRUE(strcmp(serial_error, dual_error) == 0);
    ASSERT_TRUE(strcmp(serial_error, wide_error) == 0);
    ASSERT_TRUE(strstr(wide_error, "module task 0 failed") != NULL);
    XrCompilerSessionGenerationSnapshot serial_after =
        xr_compiler_session_generation_snapshot(serial);
    XrCompilerSessionGenerationSnapshot dual_after =
        xr_compiler_session_generation_snapshot(dual);
    XrCompilerSessionGenerationSnapshot wide_after =
        xr_compiler_session_generation_snapshot(wide);
    ASSERT_EQ_UINT(serial_after.workspace_generation,
                   serial_before.workspace_generation);
    ASSERT_EQ_UINT(dual_after.workspace_generation,
                   dual_before.workspace_generation);
    ASSERT_EQ_UINT(wide_after.workspace_generation,
                   wide_before.workspace_generation);
    ASSERT_TRUE(serial_after.session_generation >
                serial_before.session_generation);
    ASSERT_TRUE(dual_after.session_generation > dual_before.session_generation);
    ASSERT_TRUE(wide_after.session_generation > wide_before.session_generation);
    XrFingerprint serial_after_failure;
    XrFingerprint dual_after_failure;
    XrFingerprint after_failure;
    ASSERT_TRUE(xr_dependency_graph_fingerprint(
        xr_compiler_session_dependency_graph(serial),
        &serial_after_failure));
    ASSERT_TRUE(xr_dependency_graph_fingerprint(
        xr_compiler_session_dependency_graph(dual),
        &dual_after_failure));
    ASSERT_TRUE(xr_dependency_graph_fingerprint(
        xr_compiler_session_dependency_graph(wide), &after_failure));
    ASSERT_TRUE(xr_fingerprint_equal(serial_after_failure, serial_graph));
    ASSERT_TRUE(xr_fingerprint_equal(dual_after_failure, dual_graph));
    ASSERT_TRUE(xr_fingerprint_equal(after_failure, wide_graph));

    XrDependencyGraph old_graph;
    XrStableId old_root;
    XrStableId old_consumer;
    ASSERT_TRUE(build_session_graph(&old_graph, &old_root,
                                    &old_consumer));
    XrCompilerSession *publication = xr_compiler_session_new(NULL);
    ASSERT_NOT_NULL(publication);
    ASSERT_TRUE(xr_compiler_session_publish_dependency_graph(publication,
                                                             &old_graph));
    XrFingerprint old_fingerprint;
    ASSERT_TRUE(xr_dependency_graph_fingerprint(
        xr_compiler_session_dependency_graph(publication),
        &old_fingerprint));

    SessionModuleTaskContext preflight_failure = {
        .fail_preflight_task = 1u,
        .fail_preflight = true,
    };
    XrCompilerSessionGenerationSnapshot before_preflight =
        xr_compiler_session_generation_snapshot(publication);
    ASSERT_FALSE(run_session_module_tasks(
        publication, &graph, 8u, &preflight_failure, &wide_stats,
        wide_error, sizeof(wide_error)));
    ASSERT_EQ_UINT(preflight_failure.publish_count, 0u);
    ASSERT_EQ_UINT(wide_stats.preflighted_task_count, 1u);
    ASSERT_EQ_UINT(wide_stats.published_task_count, 0u);
    ASSERT_EQ_UINT(wide_stats.execution.first_failed_task, 1u);
    XrCompilerSessionGenerationSnapshot after_preflight =
        xr_compiler_session_generation_snapshot(publication);
    ASSERT_EQ_UINT(after_preflight.workspace_generation,
                   before_preflight.workspace_generation);
    ASSERT_TRUE(after_preflight.session_generation >
                before_preflight.session_generation);

    SessionModuleTaskContext publish_failure = {
        .fail_publish_task = 1u,
        .fail_publish = true,
    };
    XrCompilerSessionGenerationSnapshot before_publish =
        xr_compiler_session_generation_snapshot(publication);
    ASSERT_FALSE(run_session_module_tasks(
        publication, &graph, 8u, &publish_failure, &wide_stats,
        wide_error, sizeof(wide_error)));
    ASSERT_EQ_UINT(publish_failure.publish_count, 1u);
    ASSERT_EQ_UINT(wide_stats.preflighted_task_count, 3u);
    ASSERT_EQ_UINT(wide_stats.published_task_count, 1u);
    ASSERT_EQ_UINT(wide_stats.execution.first_failed_task, 1u);
    ASSERT_TRUE(publish_failure.cache_present[0]);
    ASSERT_FALSE(publish_failure.cache_present[1]);
    ASSERT_FALSE(publish_failure.cache_present[2]);
    XrCompilerSessionGenerationSnapshot after_publish =
        xr_compiler_session_generation_snapshot(publication);
    ASSERT_EQ_UINT(after_publish.workspace_generation,
                   before_publish.workspace_generation);
    ASSERT_TRUE(after_publish.session_generation >
                before_publish.session_generation);
    XrFingerprint after_publish_failure;
    ASSERT_TRUE(xr_dependency_graph_fingerprint(
        xr_compiler_session_dependency_graph(publication),
        &after_publish_failure));
    ASSERT_TRUE(xr_fingerprint_equal(after_publish_failure,
                                     old_fingerprint));

    publish_failure.fail_publish = false;
    publish_failure.publish_count = 0u;
    ASSERT_TRUE(run_session_module_tasks(
        publication, &graph, 2u, &publish_failure, &wide_stats,
        wide_error, sizeof(wide_error)));
    ASSERT_EQ_UINT(publish_failure.exact_revalidations, 1u);
    ASSERT_EQ_UINT(wide_stats.published_task_count, 3u);
    XrFingerprint published_fingerprint;
    ASSERT_TRUE(xr_dependency_graph_fingerprint(
        xr_compiler_session_dependency_graph(publication),
        &published_fingerprint));
    ASSERT_TRUE(xr_fingerprint_equal(published_fingerprint,
                                     serial_graph));

    xr_compiler_session_delete(publication);
    xr_dependency_graph_finalize(&old_graph);

    xr_compiler_session_delete(wide);
    xr_compiler_session_delete(dual);
    xr_compiler_session_delete(serial);
    xr_dependency_graph_finalize(&graph);
}

TEST(operation_abort_is_transactional_and_invalidates_old_scopes) {
    XrCompilerSession *session = xr_compiler_session_new(NULL);
    ASSERT_NOT_NULL(session);
    XrCompilerSessionOperationScope owner;
    XrCompilerSessionOperationScope nested;
    ASSERT_TRUE(xr_compiler_session_operation_begin(session, &owner));
    ASSERT_TRUE(xr_compiler_session_operation_begin(session, &nested));
    ASSERT_FALSE(nested.owns_operation);

    XrArena arena;
    XrCompilerSessionScope scope;
    xr_arena_init(&arena, 1024);
    ASSERT_TRUE(xr_compiler_session_push_arena(session, &arena, "cancel.xr", &scope));
    xr_compiler_session_set_module_graph(session, (struct XrModuleGraph *) (uintptr_t) 7);
    XrCompileUnitIdentity identity = {
        .kind = XR_COMPILE_UNIT_USER,
        .canonical_module = "pkg/cancel",
    };
    xr_compiler_session_set_compile_unit_identity(session, &identity);
    XrCompilerSessionGenerationSnapshot before =
        xr_compiler_session_generation_snapshot(session);
    ASSERT_TRUE(xr_compiler_session_operation_fail(
        &nested, XR_COMPILER_SESSION_OPERATION_CANCELLED));
    ASSERT_TRUE(xr_compiler_session_incremental_stats(session).operation_active);
    xr_compiler_session_pop_arena(&scope);
    ASSERT_FALSE(xr_compiler_session_operation_succeed(&owner));
    ASSERT_EQ_UINT(xr_compiler_session_generation_snapshot(session).session_generation,
                   before.session_generation + 1u);
    ASSERT_NULL(xr_compiler_session_current_arena(session));
    ASSERT_NULL(xr_compiler_session_string_pool(session));
    ASSERT_NULL(xr_compiler_session_module_graph(session));
    ASSERT_NULL(xr_compiler_session_current_arena(session));

    XrCompilerSessionIncrementalStats cancelled =
        xr_compiler_session_incremental_stats(session);
    ASSERT_EQ_UINT(cancelled.cancelled_operations, 1);
    ASSERT_EQ_UINT(cancelled.fatal_operations, 0);
    ASSERT_EQ_UINT(cancelled.last_outcome, XR_COMPILER_SESSION_OPERATION_CANCELLED);
    ASSERT_FALSE(cancelled.operation_active);

    ASSERT_TRUE(xr_compiler_session_operation_begin(session, &owner));
    ASSERT_TRUE(xr_compiler_session_operation_fail(
        &owner, XR_COMPILER_SESSION_OPERATION_FATAL));
    ASSERT_TRUE(xr_compiler_session_operation_begin(session, &owner));
    ASSERT_TRUE(xr_compiler_session_operation_succeed(&owner));
    XrCompilerSessionIncrementalStats final =
        xr_compiler_session_incremental_stats(session);
    ASSERT_EQ_UINT(final.completed_operations, 1);
    ASSERT_EQ_UINT(final.cancelled_operations, 1);
    ASSERT_EQ_UINT(final.fatal_operations, 1);
    ASSERT_EQ_UINT(final.last_outcome, XR_COMPILER_SESSION_OPERATION_SUCCEEDED);

    xr_arena_destroy(&arena);
    xr_compiler_session_delete(session);
}

TEST(invalidation_history_is_bounded_and_idle_cleanup_is_observable) {
    XrDependencyGraph source;
    XrStableId root_id;
    XrStableId consumer_id;
    ASSERT_TRUE(build_session_graph(&source, &root_id, &consumer_id));
    (void) root_id;
    (void) consumer_id;
    XrCompilerSession *session = xr_compiler_session_new(NULL);
    ASSERT_NOT_NULL(session);
    ASSERT_TRUE(xr_compiler_session_publish_dependency_graph(session, &source));
    xr_dependency_graph_finalize(&source);

    for (size_t i = 0; i < XR_COMPILER_SESSION_INVALIDATION_HISTORY_LIMIT + 5u; i++)
        ASSERT_TRUE(change_root_signature(session, (uint8_t) (100u + i)));
    XrCompilerSessionIncrementalStats full =
        xr_compiler_session_incremental_stats(session);
    ASSERT_EQ_UINT(full.invalidation_history_count,
                   XR_COMPILER_SESSION_INVALIDATION_HISTORY_LIMIT);
    ASSERT_EQ_UINT(full.invalidation_history_limit,
                   XR_COMPILER_SESSION_INVALIDATION_HISTORY_LIMIT);
    ASSERT_GT(full.logical_bytes, 0);
    ASSERT_GE(full.peak_logical_bytes, full.logical_bytes);
    ASSERT_NOT_NULL(xr_compiler_session_invalidation_at(
        session, XR_COMPILER_SESSION_INVALIDATION_HISTORY_LIMIT - 1u));
    ASSERT_NULL(xr_compiler_session_invalidation_at(
        session, XR_COMPILER_SESSION_INVALIDATION_HISTORY_LIMIT));

    XrCompilerSessionOperationScope operation_scope;
    ASSERT_TRUE(xr_compiler_session_operation_begin(session, &operation_scope));
    ASSERT_FALSE(xr_compiler_session_incremental_idle_cleanup(session, 2));
    ASSERT_TRUE(xr_compiler_session_operation_succeed(&operation_scope));
    ASSERT_TRUE(xr_compiler_session_incremental_idle_cleanup(session, 2));
    XrCompilerSessionIncrementalStats trimmed =
        xr_compiler_session_incremental_stats(session);
    ASSERT_EQ_UINT(trimmed.invalidation_history_count, 2);
    ASSERT_LE(trimmed.logical_bytes, full.logical_bytes);
    ASSERT_EQ_UINT(trimmed.peak_logical_bytes, full.peak_logical_bytes);

    ASSERT_TRUE(xr_compiler_session_reset_incremental(session));
    XrCompilerSessionIncrementalStats reset =
        xr_compiler_session_incremental_stats(session);
    ASSERT_EQ_UINT(reset.module_count, 0);
    ASSERT_EQ_UINT(reset.dependency_count, 0);
    ASSERT_EQ_UINT(reset.invalidation_history_count, 0);
    ASSERT_EQ_UINT(reset.logical_bytes, 0);
    ASSERT_EQ_UINT(reset.peak_logical_bytes, 0);
    ASSERT_EQ_UINT(reset.completed_operations, 0);
    ASSERT_EQ_UINT(reset.cancelled_operations, 0);
    ASSERT_EQ_UINT(reset.fatal_operations, 0);
    xr_compiler_session_delete(session);
}

TEST(operation_scope_rejects_forged_and_stale_commits) {
    XrCompilerSession *session = xr_compiler_session_new(NULL);
    ASSERT_NOT_NULL(session);
    XrCompilerSessionOperationScope owner;
    XrCompilerSessionOperationScope nested;
    ASSERT_TRUE(xr_compiler_session_operation_begin(session, &owner));
    ASSERT_TRUE(xr_compiler_session_operation_begin(session, &nested));

    XrCompilerSessionOperationScope forged = nested;
    forged.session_generation++;
    ASSERT_FALSE(xr_compiler_session_operation_fail(
        &nested, XR_COMPILER_SESSION_OPERATION_NONE));
    ASSERT_FALSE(xr_compiler_session_operation_fail(
        &nested, XR_COMPILER_SESSION_OPERATION_SUCCEEDED));
    ASSERT_TRUE(nested.active);
    ASSERT_FALSE(xr_compiler_session_operation_succeed(&forged));
    ASSERT_TRUE(xr_compiler_session_incremental_stats(session).operation_active);

    ASSERT_TRUE(xr_compiler_session_operation_fail(
        &nested, XR_COMPILER_SESSION_OPERATION_FATAL));
    ASSERT_FALSE(xr_compiler_session_operation_succeed(&owner));
    ASSERT_FALSE(xr_compiler_session_incremental_stats(session).operation_active);

    ASSERT_FALSE(xr_compiler_session_operation_succeed(&nested));
    ASSERT_FALSE(xr_compiler_session_operation_fail(
        &forged, XR_COMPILER_SESSION_OPERATION_CANCELLED));
    ASSERT_FALSE(xr_compiler_session_operation_begin(NULL, &owner));
    ASSERT_FALSE(xr_compiler_session_operation_begin(session, NULL));
    ASSERT_FALSE(xr_compiler_session_operation_fail(
        NULL, XR_COMPILER_SESSION_OPERATION_FATAL));

    xr_compiler_session_delete(session);
}

TEST(production_compile_entry_commits_or_aborts_one_operation) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(isolate);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    ASSERT_NOT_NULL(session);

    XrCompilerSessionIncrementalStats initial =
        xr_compiler_session_incremental_stats(session);
    XrProto *proto = xr_compile_source_with_path(session, "print(42)\n", "session-ok.xr");
    ASSERT_NOT_NULL(proto);
    xr_instruction_unit_free(proto);
    XrCompilerSessionIncrementalStats succeeded =
        xr_compiler_session_incremental_stats(session);
    ASSERT_EQ_UINT(succeeded.completed_operations, initial.completed_operations + 1u);
    ASSERT_EQ_UINT(succeeded.fatal_operations, initial.fatal_operations);
    ASSERT_EQ_UINT(succeeded.last_outcome, XR_COMPILER_SESSION_OPERATION_SUCCEEDED);
    ASSERT_FALSE(succeeded.operation_active);

    XrCompilerSessionGenerationSnapshot before_failure =
        xr_compiler_session_generation_snapshot(session);
    proto = xr_compile_source_with_path(session, "fn broken( {\n", "session-fail.xr");
    ASSERT_NULL(proto);
    XrCompilerSessionGenerationSnapshot after_failure =
        xr_compiler_session_generation_snapshot(session);
    XrCompilerSessionIncrementalStats failed =
        xr_compiler_session_incremental_stats(session);
    ASSERT_EQ_UINT(after_failure.session_generation,
                   before_failure.session_generation + 1u);
    ASSERT_EQ_UINT(failed.fatal_operations, initial.fatal_operations + 1u);
    ASSERT_EQ_UINT(failed.last_outcome, XR_COMPILER_SESSION_OPERATION_FATAL);
    ASSERT_FALSE(failed.operation_active);
    ASSERT_NULL(xr_compiler_session_current_arena(session));
    ASSERT_NULL(xr_compiler_session_string_pool(session));
    ASSERT_NULL(xr_compiler_session_module_graph(session));

    xray_vm_delete(isolate);
}

TEST(rejected_invalidation_preserves_published_state) {
    XrDependencyGraph source;
    XrStableId root_id;
    XrStableId consumer_id;
    ASSERT_TRUE(build_session_graph(&source, &root_id, &consumer_id));
    (void) consumer_id;
    XrCompilerSession *session = xr_compiler_session_new(NULL);
    ASSERT_NOT_NULL(session);
    ASSERT_TRUE(xr_compiler_session_publish_dependency_graph(session, &source));
    xr_dependency_graph_finalize(&source);

    XrFingerprint before_graph;
    ASSERT_TRUE(xr_dependency_graph_fingerprint(
        xr_compiler_session_dependency_graph(session), &before_graph));
    XrCompilerSessionGenerationSnapshot before_generation =
        xr_compiler_session_generation_snapshot(session);
    XrCompilerSessionIncrementalStats before_stats =
        xr_compiler_session_incremental_stats(session);

    XrModuleSummary wrong_identity;
    ASSERT_TRUE(init_summary(&wrong_identity, "pkg/not-root", 190));
    XrInvalidationEvent forged = {
        .reason = XR_INVALIDATION_SUMMARY_CHANGED,
        .root_id = root_id,
        .replacement_summary = &wrong_identity,
    };
    ASSERT_FALSE(xr_compiler_session_apply_invalidation(session, &forged));
    xr_module_summary_finalize(&wrong_identity);

    XrFingerprint after_graph;
    ASSERT_TRUE(xr_dependency_graph_fingerprint(
        xr_compiler_session_dependency_graph(session), &after_graph));
    ASSERT_MEM_EQ(after_graph.bytes, before_graph.bytes, sizeof(before_graph.bytes));
    ASSERT_TRUE(snapshots_equal(xr_compiler_session_generation_snapshot(session),
                                before_generation));
    XrCompilerSessionIncrementalStats after_stats =
        xr_compiler_session_incremental_stats(session);
    ASSERT_EQ_UINT(after_stats.invalidation_history_count,
                   before_stats.invalidation_history_count);
    ASSERT_EQ_UINT(after_stats.logical_bytes, before_stats.logical_bytes);
    ASSERT_EQ_UINT(after_stats.peak_logical_bytes, before_stats.peak_logical_bytes);
    xr_compiler_session_delete(session);
}

TEST(session_owns_cache_store_handle) {
    char root[XR_PATH_MAX];
    ASSERT_EQ_INT(xr_temp_dir_create("xray-session-cache", root, sizeof(root)), 0);
    XrCacheStoreConfig cache = {
        .root = root,
        .quota_bytes = 1u << 20,
        .max_entry_bytes = 1u << 16,
        .stale_temp_age_ns = UINT64_C(1000000000),
    };
    XrCompilerSessionConfig config = {.incremental_cache = &cache};
    XrCompilerSession *session = xr_compiler_session_new(&config);
    ASSERT_NOT_NULL(session);
    ASSERT_NOT_NULL(xr_compiler_session_cache_store(session));
    ASSERT_TRUE(xr_compiler_session_incremental_stats(session).cache_store_open);
    cache.root = "caller-mutated-invalid-root";
    xr_compiler_session_delete(session);
    remove_empty_cache_root(root);
}

TEST(session_installs_one_cache_before_operations) {
    char root[XR_PATH_MAX];
    ASSERT_EQ_INT(xr_temp_dir_create("xray-session-cache-install", root,
                                     sizeof(root)), 0);
    XrCacheStoreConfig cache = {
        .root = root,
        .quota_bytes = 1u << 20,
        .max_entry_bytes = 1u << 16,
        .stale_temp_age_ns = UINT64_C(1000000000),
    };
    XrCompilerSession *session = xr_compiler_session_new(NULL);
    ASSERT_NOT_NULL(session);
    XrCompilerSessionGenerationSnapshot before =
        xr_compiler_session_generation_snapshot(session);
    ASSERT_TRUE(xr_compiler_session_open_incremental_cache(session, &cache));
    XrCompilerSessionGenerationSnapshot after =
        xr_compiler_session_generation_snapshot(session);
    ASSERT_EQ_UINT(after.configuration_generation,
                   before.configuration_generation + 1u);
    ASSERT_NOT_NULL(xr_compiler_session_cache_store(session));
    ASSERT_FALSE(xr_compiler_session_open_incremental_cache(session, &cache));
    XrCompilerSessionOperationScope operation = {0};
    ASSERT_TRUE(xr_compiler_session_operation_begin(session, &operation));
    ASSERT_FALSE(xr_compiler_session_open_incremental_cache(session, &cache));
    ASSERT_TRUE(xr_compiler_session_operation_succeed(&operation));
    xr_compiler_session_delete(session);
    remove_empty_cache_root(root);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Compiler session generations");
RUN_TEST(generation_replay_is_deterministic);
RUN_TEST(repl_declaration_generations_publish_and_abandon_without_reuse);
RUN_TEST(production_repl_eval_publishes_and_abandons_declaration_generations);
RUN_TEST(generation_changes_are_domain_specific_and_transactional);
RUN_TEST(incremental_reset_advances_identity_and_clears_transient_state);
RUN_TEST(session_owns_configuration_paths);
RUN_TEST(target_and_provider_updates_advance_only_their_generation);
RUN_TEST(session_owns_dependency_graph_and_isolates_workspaces);
RUN_TEST(module_task_session_is_worker_stable_and_publishes_atomically);
RUN_TEST(operation_abort_is_transactional_and_invalidates_old_scopes);
RUN_TEST(operation_scope_rejects_forged_and_stale_commits);
RUN_TEST(invalidation_history_is_bounded_and_idle_cleanup_is_observable);
RUN_TEST(rejected_invalidation_preserves_published_state);
RUN_TEST(session_owns_cache_store_handle);
RUN_TEST(session_installs_one_cache_before_operations);
RUN_TEST(production_compile_entry_commits_or_aborts_one_operation);
TEST_MAIN_END()
