/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_program_target_plan_build.c - Program TargetPlan build/cache tests
 */

#include "base/xfileio.h"
#include "base/xmalloc.h"
#include "incremental/xr_program_target_plan_build.h"
#include "ir/xi.h"
#include "ir/xi_module.h"
#include "os/os_dir.h"
#include "os/os_fs.h"
#include "os/os_temp.h"
#include "plan/format/xr_xtp_schema.h"
#include "plan/semantic/xr_semantic_builder.h"
#include "plan/target/xr_target_plan.h"
#include "runtime/value/xtype.h"
#include "../plan/target_profile_test_fixture.h"
#include "test_framework.h"
#include <string.h>

static XrType program_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static const char kProgramModuleIdentity[] =
    "memory-module-v1:id=36:program-target-plan-build-fixture-v1";

static XrSemanticPlan *build_semantic(uint32_t ordinal) {
    char name[48];
    (void) snprintf(name, sizeof(name), "program_target_%u", ordinal);
    XiFunc *function = xi_func_new(name, &program_int);
    if (!function)
        return NULL;
    function->module = xi_module_new("fixture/program_target_plan.xr", name, function);
    if (!function->module || !xi_module_set_identity(function->module, kProgramModuleIdentity)) {
        xi_func_free(function);
        return NULL;
    }
    XiBlock *entry = xi_block_new(function);
    XiValue *constant = entry ? xi_const_int(function, entry, ordinal, &program_int) : NULL;
    if (!entry || !constant) {
        xi_func_free(function);
        return NULL;
    }
    xi_block_set_return(entry, constant);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &semantic, error, sizeof(error));
    xi_func_free(function);
    return built ? semantic : NULL;
}

static void clear_directory(const char *path) {
    XrDirIter *iterator = xr_dir_open(path);
    if (iterator) {
        XrDirEntry entry;
        while (xr_dir_next(iterator, &entry)) {
            if (entry.is_dir)
                continue;
            char *child = xr_path_join(path, entry.name);
            if (child) {
                (void) xr_fs_remove(child);
                xr_free(child);
            }
        }
        xr_dir_close(iterator);
    }
    (void) xr_test_rmdir(path);
}

static void remove_store_root(const char *root) {
    char *xsm = xr_path_join(root, "xsm");
    char *xtp = xr_path_join(root, "xtp");
    if (xsm)
        clear_directory(xsm);
    if (xtp)
        clear_directory(xtp);
    xr_free(xsm);
    xr_free(xtp);
    clear_directory(root);
}

static XrCacheStore *open_store(const char *root) {
    XrCacheStoreConfig config = {
        .root = root,
        .quota_bytes = UINT64_C(16) * 1024u * 1024u,
        .max_entry_bytes = 4u * 1024u * 1024u,
        .stale_temp_age_ns = UINT64_C(1000000000),
    };
    return xr_cache_store_open(&config);
}

static bool run_build(const XrSemanticPlan *semantic, const XrSemanticPlan *const *dependencies,
                      uint32_t dependency_count, XrTargetProfile *profile, XrCacheStore *store,
                      bool rebuild, XrProgramTargetPlanCancellationToken *cancellation,
                      XrProgramTargetPlanBuildResult *result, char error[512]) {
    static const uint8_t policy[] = "program-target-plan-build-kat-v1";
    XrCacheFingerprint budget;
    xr_cache_fingerprint_bytes(policy, sizeof(policy) - 1u, &budget);
    XrProgramTargetPlanBuildRequest request = {
        .semantic_plan = semantic,
        .semantic_dependencies = dependencies,
        .semantic_dependency_count = dependency_count,
        .profile = profile,
        .cache_store = store,
        .optimization_budget = budget,
        .rebuild = rebuild,
        .cancellation = cancellation,
    };
    return xr_program_target_plan_build(&request, result, error, 512u);
}

static void assert_same_encoded_plan(const XrTargetPlan *expected, const XrTargetPlan *actual) {
    uint8_t *expected_bytes = NULL;
    uint8_t *actual_bytes = NULL;
    size_t expected_size = 0;
    size_t actual_size = 0;
    char error[512] = {0};
    ASSERT_TRUE(
        xr_xtp_encode_plan(expected, &expected_bytes, &expected_size, error, sizeof(error)));
    ASSERT_TRUE(xr_xtp_encode_plan(actual, &actual_bytes, &actual_size, error, sizeof(error)));
    ASSERT_EQ_UINT(expected_size, actual_size);
    ASSERT_MEM_EQ(expected_bytes, actual_bytes, expected_size);
    xr_xtp_encoded_free(expected_bytes);
    xr_xtp_encoded_free(actual_bytes);
}

TEST(cold_warm_and_rebuild_preserve_one_program_plan) {
    char root[XR_PATH_MAX];
    ASSERT_EQ_INT(xr_temp_dir_create("xray-program-target-plan", root, sizeof(root)), 0);
    XrCacheStore *store = open_store(root);
    ASSERT_NOT_NULL(store);
    XrSemanticPlan *semantic = build_semantic(101u);
    ASSERT_NOT_NULL(semantic);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);

    XrProgramTargetPlanBuildResult cold = {0};
    char error[512] = {0};
    ASSERT_TRUE(run_build(semantic, NULL, 0u, profile, store, false, NULL, &cold, error));
    ASSERT_NOT_NULL(cold.plan);
    ASSERT_TRUE(cold.cache_enabled);
    ASSERT_TRUE(cold.cache_load_attempted);
    ASSERT_EQ_INT(cold.load_status, XR_CACHE_LOAD_MISS);
    ASSERT_FALSE(cold.cache_hit);
    ASSERT_TRUE(cold.built);
    ASSERT_TRUE(cold.cache_publish_attempted);
    ASSERT_EQ_INT(cold.publish_status, XR_CACHE_PUBLISH_OK);
    ASSERT_TRUE(cold.cache_published);

    XrProgramTargetPlanBuildResult warm = {0};
    memset(error, 0, sizeof(error));
    ASSERT_TRUE(run_build(semantic, NULL, 0u, profile, store, false, NULL, &warm, error));
    ASSERT_NOT_NULL(warm.plan);
    ASSERT_TRUE(warm.cache_load_attempted);
    ASSERT_EQ_INT(warm.load_status, XR_CACHE_LOAD_HIT);
    ASSERT_TRUE(warm.cache_hit);
    ASSERT_FALSE(warm.built);
    ASSERT_FALSE(warm.cache_publish_attempted);
    assert_same_encoded_plan(cold.plan, warm.plan);

    XrProgramTargetPlanBuildResult rebuilt = {0};
    memset(error, 0, sizeof(error));
    ASSERT_TRUE(run_build(semantic, NULL, 0u, profile, store, true, NULL, &rebuilt, error));
    ASSERT_NOT_NULL(rebuilt.plan);
    ASSERT_TRUE(rebuilt.rebuild_requested);
    ASSERT_FALSE(rebuilt.cache_load_attempted);
    ASSERT_FALSE(rebuilt.cache_hit);
    ASSERT_TRUE(rebuilt.built);
    ASSERT_TRUE(rebuilt.cache_publish_attempted);
    ASSERT_EQ_INT(rebuilt.publish_status, XR_CACHE_PUBLISH_EXISTS);
    ASSERT_FALSE(rebuilt.cache_published);
    assert_same_encoded_plan(cold.plan, rebuilt.plan);

    xr_program_target_plan_build_result_release(&rebuilt);
    xr_program_target_plan_build_result_release(&warm);
    xr_program_target_plan_build_result_release(&cold);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    xr_cache_store_close(store);
    remove_store_root(root);
}

TEST(cancel_is_fail_closed_and_preserves_the_warm_entry) {
    char root[XR_PATH_MAX];
    ASSERT_EQ_INT(xr_temp_dir_create("xray-program-target-cancel", root, sizeof(root)), 0);
    XrCacheStore *store = open_store(root);
    ASSERT_NOT_NULL(store);
    XrSemanticPlan *semantic = build_semantic(202u);
    ASSERT_NOT_NULL(semantic);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);

    XrProgramTargetPlanBuildResult result = {0};
    char error[512] = {0};
    ASSERT_TRUE(run_build(semantic, NULL, 0u, profile, store, false, NULL, &result, error));
    ASSERT_TRUE(result.cache_published);
    xr_program_target_plan_build_result_release(&result);

    XrProgramTargetPlanCancellationToken cancellation;
    xr_program_target_plan_cancellation_token_init(&cancellation);
    xr_program_target_plan_cancellation_token_request(&cancellation);
    memset(error, 0, sizeof(error));
    ASSERT_FALSE(
        run_build(semantic, NULL, 0u, profile, store, false, &cancellation, &result, error));
    ASSERT_TRUE(result.cancelled);
    ASSERT_NULL(result.plan);
    ASSERT_FALSE(result.cache_load_attempted);
    ASSERT_NOT_NULL(strstr(error, "cancelled"));
    xr_program_target_plan_build_result_release(&result);

    memset(error, 0, sizeof(error));
    ASSERT_TRUE(run_build(semantic, NULL, 0u, profile, store, false, NULL, &result, error));
    ASSERT_TRUE(result.cache_hit);
    ASSERT_FALSE(result.built);

    xr_program_target_plan_build_result_release(&result);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    xr_cache_store_close(store);
    remove_store_root(root);
}

TEST(wrong_program_authority_is_rejected_before_build_or_cache) {
    char root[XR_PATH_MAX];
    ASSERT_EQ_INT(xr_temp_dir_create("xray-program-target-authority", root, sizeof(root)), 0);
    XrCacheStore *store = open_store(root);
    ASSERT_NOT_NULL(store);
    XrSemanticPlan *semantic = build_semantic(303u);
    XrSemanticPlan *foreign = build_semantic(404u);
    ASSERT_NOT_NULL(semantic);
    ASSERT_NOT_NULL(foreign);
    const XrSemanticPlan *wrong_dependencies[] = {foreign};
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);

    XrProgramTargetPlanBuildResult result = {0};
    char error[512] = {0};
    ASSERT_FALSE(
        run_build(semantic, wrong_dependencies, 1u, profile, store, false, NULL, &result, error));
    ASSERT_TRUE(result.cache_enabled);
    ASSERT_NULL(result.plan);
    ASSERT_FALSE(result.built);
    ASSERT_FALSE(result.cache_load_attempted);
    ASSERT_FALSE(result.cache_publish_attempted);
    ASSERT_NOT_NULL(strstr(error, "wrong semantic authority"));

    xr_program_target_plan_build_result_release(&result);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(foreign);
    xr_semantic_plan_free(semantic);
    xr_cache_store_close(store);
    remove_store_root(root);
}

TEST_MAIN_BEGIN()
RUN_TEST(cold_warm_and_rebuild_preserve_one_program_plan);
RUN_TEST(cancel_is_fail_closed_and_preserves_the_warm_entry);
RUN_TEST(wrong_program_authority_is_rejected_before_build_or_cache);
TEST_MAIN_END()
