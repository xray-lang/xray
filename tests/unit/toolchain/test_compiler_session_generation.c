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
#include "toolchain/xcompiler_session.h"

#include <string.h>

static bool snapshots_equal(XrCompilerSessionGenerationSnapshot left,
                            XrCompilerSessionGenerationSnapshot right) {
    return left.session_generation == right.session_generation &&
           left.workspace_generation == right.workspace_generation &&
           left.configuration_generation == right.configuration_generation &&
           left.target_generation == right.target_generation &&
           left.provider_generation == right.provider_generation;
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

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Compiler session generations");
RUN_TEST(generation_replay_is_deterministic);
RUN_TEST(generation_changes_are_domain_specific_and_transactional);
RUN_TEST(incremental_reset_advances_identity_and_clears_transient_state);
RUN_TEST(session_owns_configuration_paths);
TEST_MAIN_END()
