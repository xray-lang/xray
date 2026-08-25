/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_target_plan_tasks.c - Deterministic parallel TargetPlan task tests
 */

#include "base/xfileio.h"
#include "base/xmalloc.h"
#include "incremental/xr_target_plan_tasks.h"
#include "ir/xi.h"
#include "ir/xi_module.h"
#include "os/os_dir.h"
#include "os/os_fs.h"
#include "os/os_temp.h"
#include "os/os_thread.h"
#include "plan/semantic/xr_semantic_builder.h"
#include "plan/format/xr_xtp_schema.h"
#include "plan/target/xr_target_plan.h"
#include "runtime/value/xtype.h"
#include "../plan/target_profile_test_fixture.h"
#include "test_framework.h"

#define TASK_COUNT 8u

static XrType task_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static const char kTaskModuleIdentity[] =
    "memory-module-v1:id=31:parallel-target-plan-fixture-v1";

static XrSemanticPlan *build_semantic(uint32_t ordinal) {
    char name[48];
    (void) snprintf(name, sizeof(name), "parallel_target_%u", ordinal);
    XiFunc *function = xi_func_new(name, &task_int);
    if (!function)
        return NULL;
    function->module = xi_module_new("fixture/target_plan_tasks.xr", name, function);
    if (!function->module ||
        !xi_module_set_identity(function->module, kTaskModuleIdentity)) {
        xi_func_free(function);
        return NULL;
    }
    XiBlock *entry = xi_block_new(function);
    XiValue *constant = entry ? xi_const_int(function, entry, ordinal, &task_int)
                              : NULL;
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

static bool run_batch(const XrTargetPlanTaskInput inputs[TASK_COUNT],
                      XrTargetProfile *profile, XrCacheStore *store,
                      uint32_t workers, bool rebuild,
                      XrTargetPlanCancellationToken *cancellation,
                      XrTargetPlanTaskResult results[TASK_COUNT],
                      XrTargetPlanTaskStats *stats) {
    static const uint8_t policy[] = "parallel-target-plan-test-v1";
    XrCacheFingerprint budget;
    xr_cache_fingerprint_bytes(policy, sizeof(policy) - 1u, &budget);
    XrTargetPlanTaskBatch batch = {
        .inputs = inputs,
        .input_count = TASK_COUNT,
        .profile = profile,
        .cache_store = store,
        .optimization_budget = budget,
        .rebuild = rebuild,
        .worker_limit = workers,
        .cancellation = cancellation,
        .results = results,
    };
    char error[512] = {0};
    return xr_target_plan_tasks_run(&batch, stats, error, sizeof(error));
}

TEST(worker_counts_produce_identical_canonical_plans) {
    XrSemanticPlan *semantics[TASK_COUNT] = {0};
    XrTargetPlanTaskInput inputs[TASK_COUNT] = {0};
    XrFingerprint serial_fingerprints[TASK_COUNT] = {0};
    uint8_t *serial_bytes[TASK_COUNT] = {0};
    size_t serial_sizes[TASK_COUNT] = {0};
    XrTargetPlanTaskResult results[TASK_COUNT] = {0};
    XrTargetPlanTaskStats stats = {0};
    XrTargetProfile *profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    for (uint32_t i = 0; i < TASK_COUNT; i++) {
        semantics[i] = build_semantic(i + 1u);
        ASSERT_NOT_NULL(semantics[i]);
        inputs[i].semantic_plan = semantics[i];
    }

    ASSERT_TRUE(run_batch(inputs, profile, NULL, 1u, false, NULL, results, &stats));
    ASSERT_EQ_UINT(stats.worker_count, 1u);
    ASSERT_EQ_UINT(stats.first_failed_index, XR_TARGET_PLAN_TASK_INDEX_NONE);
    for (uint32_t i = 0; i < TASK_COUNT; i++) {
        ASSERT_TRUE(results[i].complete);
        ASSERT_TRUE(results[i].built);
        ASSERT_NOT_NULL(results[i].plan);
        serial_fingerprints[i] = xr_target_plan_fingerprint(results[i].plan);
        char error[512] = {0};
        ASSERT_TRUE(xr_xtp_encode_plan(results[i].plan, &serial_bytes[i],
                                       &serial_sizes[i], error, sizeof(error)));
    }
    xr_target_plan_task_results_release(results, TASK_COUNT);

    ASSERT_TRUE(run_batch(inputs, profile, NULL, 2u, false, NULL, results, &stats));
    ASSERT_EQ_UINT(stats.worker_count, 2u);
    for (uint32_t i = 0; i < TASK_COUNT; i++) {
        ASSERT_TRUE(xr_fingerprint_equal(
            serial_fingerprints[i], xr_target_plan_fingerprint(results[i].plan)));
        uint8_t *bytes = NULL;
        size_t size = 0;
        char error[512] = {0};
        ASSERT_TRUE(xr_xtp_encode_plan(results[i].plan, &bytes, &size, error,
                                       sizeof(error)));
        ASSERT_EQ_UINT(size, serial_sizes[i]);
        ASSERT_TRUE(memcmp(bytes, serial_bytes[i], size) == 0);
        xr_xtp_encoded_free(bytes);
    }
    xr_target_plan_task_results_release(results, TASK_COUNT);

    ASSERT_TRUE(run_batch(inputs, profile, NULL, TASK_COUNT, false, NULL, results, &stats));
    ASSERT_EQ_UINT(stats.worker_count, TASK_COUNT);
    for (uint32_t i = 0; i < TASK_COUNT; i++) {
        ASSERT_TRUE(xr_fingerprint_equal(
            serial_fingerprints[i], xr_target_plan_fingerprint(results[i].plan)));
        uint8_t *bytes = NULL;
        size_t size = 0;
        char error[512] = {0};
        ASSERT_TRUE(xr_xtp_encode_plan(results[i].plan, &bytes, &size, error,
                                       sizeof(error)));
        ASSERT_EQ_UINT(size, serial_sizes[i]);
        ASSERT_TRUE(memcmp(bytes, serial_bytes[i], size) == 0);
        xr_xtp_encoded_free(bytes);
    }
    xr_target_plan_task_results_release(results, TASK_COUNT);

    for (uint32_t i = 0; i < TASK_COUNT; i++) {
        xr_xtp_encoded_free(serial_bytes[i]);
        xr_semantic_plan_free(semantics[i]);
    }
    xr_target_profile_free(profile);
}

TEST(parallel_cache_publish_and_hit_preserve_input_order) {
    char root[XR_PATH_MAX];
    ASSERT_EQ_INT(xr_temp_dir_create("xray-target-tasks", root, sizeof(root)), 0);
    XrCacheStore *store = open_store(root);
    ASSERT_NOT_NULL(store);
    XrSemanticPlan *semantics[TASK_COUNT] = {0};
    XrTargetPlanTaskInput inputs[TASK_COUNT] = {0};
    XrFingerprint cold_fingerprints[TASK_COUNT] = {0};
    XrTargetPlanTaskResult results[TASK_COUNT] = {0};
    XrTargetPlanTaskStats stats = {0};
    XrTargetProfile *profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    for (uint32_t i = 0; i < TASK_COUNT; i++) {
        semantics[i] = build_semantic(100u + i);
        ASSERT_NOT_NULL(semantics[i]);
        inputs[i].semantic_plan = semantics[i];
    }

    ASSERT_TRUE(run_batch(inputs, profile, store, 4u, true, NULL, results, &stats));
    ASSERT_EQ_UINT(stats.worker_count, 4u);
    ASSERT_EQ_UINT(stats.hits, 0u);
    ASSERT_EQ_UINT(stats.misses, TASK_COUNT);
    ASSERT_EQ_UINT(stats.published, TASK_COUNT);
    for (uint32_t i = 0; i < TASK_COUNT; i++)
        cold_fingerprints[i] = xr_target_plan_fingerprint(results[i].plan);
    xr_target_plan_task_results_release(results, TASK_COUNT);

    ASSERT_TRUE(run_batch(inputs, profile, store, 3u, false, NULL, results, &stats));
    ASSERT_EQ_UINT(stats.worker_count, 3u);
    ASSERT_EQ_UINT(stats.hits, TASK_COUNT);
    ASSERT_EQ_UINT(stats.misses, 0u);
    ASSERT_EQ_UINT(stats.rejected, 0u);
    for (uint32_t i = 0; i < TASK_COUNT; i++) {
        ASSERT_TRUE(results[i].cache_hit);
        ASSERT_FALSE(results[i].built);
        ASSERT_TRUE(xr_fingerprint_equal(
            cold_fingerprints[i], xr_target_plan_fingerprint(results[i].plan)));
    }
    xr_target_plan_task_results_release(results, TASK_COUNT);

    for (uint32_t i = 0; i < TASK_COUNT; i++)
        xr_semantic_plan_free(semantics[i]);
    xr_target_profile_free(profile);
    xr_cache_store_close(store);
    remove_store_root(root);
}

TEST(failure_is_reported_by_lowest_canonical_index) {
    XrSemanticPlan *semantic = build_semantic(900u);
    ASSERT_NOT_NULL(semantic);
    XrTargetPlanTaskInput inputs[TASK_COUNT] = {0};
    XrTargetPlanTaskResult results[TASK_COUNT] = {0};
    XrTargetPlanTaskStats stats = {0};
    XrTargetProfile *profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    for (uint32_t i = 0; i < TASK_COUNT; i++)
        inputs[i].semantic_plan = semantic;
    inputs[5].semantic_plan = NULL;
    inputs[2].semantic_plan = NULL;

    const uint32_t worker_counts[] = {1u, 2u, TASK_COUNT};
    char canonical_error[XR_TARGET_PLAN_TASK_ERROR_SIZE] = {0};
    for (size_t run = 0; run < sizeof(worker_counts) / sizeof(worker_counts[0]);
         run++) {
        ASSERT_FALSE(run_batch(inputs, profile, NULL, worker_counts[run], false,
                               NULL, results, &stats));
        ASSERT_EQ_UINT(stats.first_failed_index, 2u);
        ASSERT_FALSE(results[2].complete);
        ASSERT_FALSE(results[5].complete);
        if (run == 0)
            (void) snprintf(canonical_error, sizeof(canonical_error), "%s",
                            results[2].error);
        else
            ASSERT_TRUE(strcmp(canonical_error, results[2].error) == 0);
        for (uint32_t i = 0; i < TASK_COUNT; i++) {
            if (i != 2u && i != 5u)
                ASSERT_TRUE(results[i].complete);
        }
        xr_target_plan_task_results_release(results, TASK_COUNT);
    }
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

typedef struct CancellationRequest {
    XrTargetPlanCancellationToken *token;
} CancellationRequest;

static void *request_cancellation(void *argument) {
    CancellationRequest *request = (CancellationRequest *) argument;
    xr_target_plan_cancellation_token_request(request->token);
    return NULL;
}

TEST(cancellation_discards_results_without_deleting_cache_entries) {
    char root[XR_PATH_MAX];
    ASSERT_EQ_INT(xr_temp_dir_create("xray-target-cancel", root, sizeof(root)), 0);
    XrCacheStore *store = open_store(root);
    ASSERT_NOT_NULL(store);
    XrSemanticPlan *semantics[TASK_COUNT] = {0};
    XrTargetPlanTaskInput inputs[TASK_COUNT] = {0};
    XrTargetPlanTaskResult results[TASK_COUNT] = {0};
    XrTargetPlanTaskStats stats = {0};
    XrTargetProfile *profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    for (uint32_t i = 0; i < TASK_COUNT; i++) {
        semantics[i] = build_semantic(200u + i);
        ASSERT_NOT_NULL(semantics[i]);
        inputs[i].semantic_plan = semantics[i];
    }

    ASSERT_TRUE(run_batch(inputs, profile, store, 4u, true, NULL, results,
                          &stats));
    ASSERT_EQ_UINT(stats.published, TASK_COUNT);
    xr_target_plan_task_results_release(results, TASK_COUNT);

    XrTargetPlanCancellationToken token;
    xr_target_plan_cancellation_token_init(&token);
    CancellationRequest request = {.token = &token};
    xr_thread_t requester;
    ASSERT_TRUE(xr_thread_create(&requester, request_cancellation, &request));
    while (!xr_target_plan_cancellation_token_is_requested(&token))
        xr_thread_yield();
    ASSERT_EQ_INT(xr_thread_join(requester, NULL), 0);
    ASSERT_FALSE(run_batch(inputs, profile, store, TASK_COUNT, false, &token,
                           results, &stats));
    ASSERT_EQ_UINT(stats.cancelled, TASK_COUNT);
    ASSERT_EQ_UINT(stats.first_failed_index, 0u);
    ASSERT_EQ_UINT(stats.hits, 0u);
    ASSERT_EQ_UINT(stats.misses, 0u);
    for (uint32_t i = 0; i < TASK_COUNT; i++) {
        ASSERT_TRUE(results[i].cancelled);
        ASSERT_NULL(results[i].plan);
    }
    xr_target_plan_task_results_release(results, TASK_COUNT);

    ASSERT_TRUE(run_batch(inputs, profile, store, 3u, false, NULL, results,
                          &stats));
    ASSERT_EQ_UINT(stats.hits, TASK_COUNT);
    ASSERT_EQ_UINT(stats.cancelled, 0u);
    xr_target_plan_task_results_release(results, TASK_COUNT);

    for (uint32_t i = 0; i < TASK_COUNT; i++)
        xr_semantic_plan_free(semantics[i]);
    xr_target_profile_free(profile);
    xr_cache_store_close(store);
    remove_store_root(root);
}

TEST_MAIN_BEGIN()
    RUN_TEST(worker_counts_produce_identical_canonical_plans);
    RUN_TEST(parallel_cache_publish_and_hit_preserve_input_order);
    RUN_TEST(failure_is_reported_by_lowest_canonical_index);
    RUN_TEST(cancellation_discards_results_without_deleting_cache_entries);
TEST_MAIN_END()
