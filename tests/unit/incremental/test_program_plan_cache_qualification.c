/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_program_plan_cache_qualification.c - Program plan cache qualification
 *
 * KEY CONCEPT:
 *   The unit suite pins one serial builder against one store. This suite
 *   qualifies the same builder when several of them share one store root:
 *   the surviving result must still be one canonical plan, publication must
 *   stay all-or-nothing, and a refused artifact must cost a verified
 *   recomputation rather than a silently weaker answer.
 */

#include "os/os_fs.h"
#include "os/os_temp.h"
#include "os/os_thread.h"
#include "plan/semantic/xr_semantic_plan.h"
#include "plan/target/xr_target_plan.h"
#include "program_plan_cache_fixture.h"
#include "../plan/target_profile_test_fixture.h"
#include "test_framework.h"
#include <string.h>

#define PLAN_WORKERS 4u
#define PLAN_QUOTA (UINT64_C(16) * 1024u * 1024u)
#define PLAN_MAX_ENTRY (4u * 1024u * 1024u)

/* Threads that publish into one store root must start inside the same window,
 * or the first one finishes before the rest begin and the test degrades into a
 * serial run that proves nothing about concurrent publication. */
typedef struct PlanBarrier {
    xr_mutex_t mutex;
    xr_cond_t cond;
    uint32_t waiting;
    uint32_t target;
    uint32_t generation;
} PlanBarrier;

static void barrier_init(PlanBarrier *barrier, uint32_t target) {
    xr_mutex_init(&barrier->mutex);
    xr_cond_init(&barrier->cond);
    barrier->waiting = 0u;
    barrier->target = target;
    barrier->generation = 0u;
}

static void barrier_destroy(PlanBarrier *barrier) {
    xr_cond_destroy(&barrier->cond);
    xr_mutex_destroy(&barrier->mutex);
}

static void barrier_wait(PlanBarrier *barrier) {
    xr_mutex_lock(&barrier->mutex);
    uint32_t generation = barrier->generation;
    if (++barrier->waiting == barrier->target) {
        barrier->waiting = 0u;
        barrier->generation++;
        xr_cond_broadcast(&barrier->cond);
    } else {
        while (generation == barrier->generation)
            xr_cond_wait(&barrier->cond, &barrier->mutex);
    }
    xr_mutex_unlock(&barrier->mutex);
}

/* The semantic authority and the target profile are built serially by the
 * harness. Only the store handle and the build itself are exercised
 * concurrently, so a failure names the cache boundary under test instead of
 * the fixture that produced its inputs. */
typedef struct PlanWorker {
    PlanBarrier *barrier;
    const char *root;
    const XrSemanticPlan *semantic;
    XrTargetProfile *profile;
    bool rebuild;
    XrProgramTargetPlanCancellationToken *cancellation;
    bool store_opened;
    bool ok;
    XrProgramTargetPlanBuildResult result;
    uint8_t *bytes;
    size_t size;
    char error[512];
} PlanWorker;

static void *plan_worker_main(void *argument) {
    PlanWorker *worker = (PlanWorker *) argument;
    XrCacheStore *store = xr_test_program_plan_store_open(worker->root, PLAN_QUOTA, PLAN_MAX_ENTRY);
    worker->store_opened = store != NULL;
    XrTestProgramPlanBuildInput input = {
        .semantic = worker->semantic,
        .dependencies = NULL,
        .dependency_count = 0u,
        .profile = worker->profile,
        .store = store,
        .rebuild = worker->rebuild,
        .cancellation = worker->cancellation,
    };
    XrProgramTargetPlanBuildResult result = {0};
    barrier_wait(worker->barrier);
    if (store) {
        worker->ok =
            xr_test_program_plan_build(&input, &result, worker->error, sizeof(worker->error));
        if (worker->ok && result.plan)
            (void) xr_test_program_plan_encode(result.plan, &worker->bytes, &worker->size);
        /* Releasing the result clears it, so record what the assertions read
         * first and drop the plan owner the caller must not observe. */
        worker->result = result;
        worker->result.plan = NULL;
        xr_program_target_plan_build_result_release(&result);
    }
    xr_cache_store_close(store);
    return NULL;
}

/* Run `count` builds that all name one identical authority against one store
 * root. Ownership of every per-worker input stays with the caller. */
static void run_workers(PlanWorker *workers, uint32_t count) {
    PlanBarrier barrier;
    xr_thread_t threads[PLAN_WORKERS];
    barrier_init(&barrier, count);
    for (uint32_t i = 0; i < count; i++) {
        workers[i].barrier = &barrier;
        ASSERT_TRUE(xr_thread_create(&threads[i], plan_worker_main, &workers[i]));
    }
    for (uint32_t i = 0; i < count; i++)
        ASSERT_EQ_INT(xr_thread_join(threads[i], NULL), 0);
    barrier_destroy(&barrier);
}

static void workers_release(PlanWorker *workers, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        xr_test_program_plan_encoded_free(workers[i].bytes);
        workers[i].bytes = NULL;
        workers[i].size = 0;
    }
}

/* Every successful worker must own byte-identical plan bytes. Comparing the
 * encoded form rather than a fingerprint is what rules out two plans that
 * agree on their identity while disagreeing on their content. */
static void assert_one_canonical_result(PlanWorker *workers, uint32_t count) {
    const uint8_t *canonical = NULL;
    size_t canonical_size = 0;
    for (uint32_t i = 0; i < count; i++) {
        ASSERT_TRUE(workers[i].store_opened);
        ASSERT_TRUE(workers[i].ok);
        ASSERT_NOT_NULL(workers[i].bytes);
        ASSERT_TRUE(workers[i].size > 0);
        if (!canonical) {
            canonical = workers[i].bytes;
            canonical_size = workers[i].size;
            continue;
        }
        ASSERT_EQ_UINT(workers[i].size, canonical_size);
        ASSERT_MEM_EQ(workers[i].bytes, canonical, canonical_size);
    }
}

/* A store root that survived concurrent publication owns exactly the objects
 * it published and no unfinished temp residue. */
static void assert_store_is_whole(const char *root, size_t expected_objects) {
    XrTestProgramPlanStoreInventory inventory;
    xr_test_program_plan_store_inventory(root, &inventory);
    ASSERT_EQ_UINT(inventory.objects, expected_objects);
    ASSERT_EQ_UINT(inventory.temps, 0u);
}

typedef struct PlanHarness {
    char root[XR_PATH_MAX];
    XrSemanticPlan *semantics[PLAN_WORKERS];
    XrTargetProfile *profiles[PLAN_WORKERS];
    PlanWorker workers[PLAN_WORKERS];
} PlanHarness;

/* Build one independent semantic authority and target profile per worker. They
 * are separate objects that derive one identical cache key, which is what a
 * concurrent product build looks like from the store's side. */
static bool harness_init(PlanHarness *harness, const char *name, uint32_t ordinal) {
    memset(harness, 0, sizeof(*harness));
    if (xr_temp_dir_create(name, harness->root, sizeof(harness->root)) != 0)
        return false;
    for (uint32_t i = 0; i < PLAN_WORKERS; i++) {
        harness->semantics[i] = xr_test_program_plan_semantic(ordinal);
        harness->profiles[i] =
            xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        if (!harness->semantics[i] || !harness->profiles[i])
            return false;
        harness->workers[i].root = harness->root;
        harness->workers[i].semantic = harness->semantics[i];
        harness->workers[i].profile = harness->profiles[i];
    }
    return true;
}

static void harness_release(PlanHarness *harness) {
    workers_release(harness->workers, PLAN_WORKERS);
    for (uint32_t i = 0; i < PLAN_WORKERS; i++) {
        xr_target_profile_free(harness->profiles[i]);
        xr_semantic_plan_free(harness->semantics[i]);
    }
    xr_test_program_plan_store_remove(harness->root);
}

/* Publication is a race the store must resolve to one winner: exactly one
 * worker may report a published object, and every other worker must have been
 * told the object already exists or have been served that object. */
static void assert_publication_is_singular(PlanWorker *workers, uint32_t count) {
    uint32_t published = 0;
    uint32_t existing = 0;
    uint32_t hits = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (workers[i].result.cache_published)
            published++;
        if (workers[i].result.cache_publish_attempted &&
            workers[i].result.publish_status == XR_CACHE_PUBLISH_EXISTS)
            existing++;
        if (workers[i].result.cache_hit)
            hits++;
    }
    ASSERT_EQ_UINT(published, 1u);
    ASSERT_EQ_UINT(published + existing + hits, count);
}

TEST(parallel_cold_builds_publish_one_canonical_plan) {
    PlanHarness harness;
    ASSERT_TRUE(harness_init(&harness, "xray-plan-cache-parallel-cold", 511u));
    run_workers(harness.workers, PLAN_WORKERS);
    assert_one_canonical_result(harness.workers, PLAN_WORKERS);
    assert_publication_is_singular(harness.workers, PLAN_WORKERS);
    assert_store_is_whole(harness.root, 1u);
    harness_release(&harness);
}

TEST(parallel_warm_builds_serve_one_canonical_plan) {
    PlanHarness harness;
    ASSERT_TRUE(harness_init(&harness, "xray-plan-cache-parallel-warm", 522u));

    PlanWorker cold = harness.workers[0];
    PlanBarrier single;
    barrier_init(&single, 1u);
    cold.barrier = &single;
    (void) plan_worker_main(&cold);
    ASSERT_TRUE(cold.ok);
    ASSERT_TRUE(cold.result.cache_published);
    barrier_destroy(&single);

    run_workers(harness.workers, PLAN_WORKERS);
    assert_one_canonical_result(harness.workers, PLAN_WORKERS);
    for (uint32_t i = 0; i < PLAN_WORKERS; i++) {
        ASSERT_TRUE(harness.workers[i].result.cache_hit);
        ASSERT_FALSE(harness.workers[i].result.built);
        ASSERT_FALSE(harness.workers[i].result.cache_publish_attempted);
        ASSERT_EQ_UINT(harness.workers[i].size, cold.size);
        ASSERT_MEM_EQ(harness.workers[i].bytes, cold.bytes, cold.size);
    }
    assert_store_is_whole(harness.root, 1u);
    xr_test_program_plan_encoded_free(cold.bytes);
    harness_release(&harness);
}

/* One writer forced past the cache while readers load the object it is
 * republishing. No reader may observe a partial object, and the writer's
 * recomputation must reproduce the bytes the readers were served. */
TEST(writer_and_readers_agree_on_one_canonical_plan) {
    PlanHarness harness;
    ASSERT_TRUE(harness_init(&harness, "xray-plan-cache-writer-readers", 533u));

    PlanWorker cold = harness.workers[0];
    PlanBarrier single;
    barrier_init(&single, 1u);
    cold.barrier = &single;
    (void) plan_worker_main(&cold);
    ASSERT_TRUE(cold.ok);
    barrier_destroy(&single);

    harness.workers[0].rebuild = true;
    run_workers(harness.workers, PLAN_WORKERS);
    assert_one_canonical_result(harness.workers, PLAN_WORKERS);
    ASSERT_TRUE(harness.workers[0].result.built);
    ASSERT_FALSE(harness.workers[0].result.cache_hit);
    ASSERT_EQ_INT(harness.workers[0].result.publish_status, XR_CACHE_PUBLISH_EXISTS);
    for (uint32_t i = 1; i < PLAN_WORKERS; i++)
        ASSERT_TRUE(harness.workers[i].result.cache_hit);
    ASSERT_EQ_UINT(harness.workers[0].size, cold.size);
    ASSERT_MEM_EQ(harness.workers[0].bytes, cold.bytes, cold.size);
    assert_store_is_whole(harness.root, 1u);
    xr_test_program_plan_encoded_free(cold.bytes);
    harness_release(&harness);
}

TEST_MAIN_BEGIN()
RUN_TEST(parallel_cold_builds_publish_one_canonical_plan);
RUN_TEST(parallel_warm_builds_serve_one_canonical_plan);
RUN_TEST(writer_and_readers_agree_on_one_canonical_plan);
TEST_MAIN_END()
