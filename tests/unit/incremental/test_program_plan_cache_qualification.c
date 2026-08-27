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

#include "base/xfileio.h"
#include "base/xmalloc.h"
#include "os/os_dir.h"
#include "os/os_fs.h"
#include "os/os_temp.h"
#include "os/os_thread.h"
#include "os/os_time.h"
#include <stdio.h>
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

/* One build run with no concurrency, used by the cases that need a known
 * canonical answer before and after a hostile artifact is planted. */
typedef struct PlanSerialRequest {
    const char *root;
    const XrSemanticPlan *semantic;
    XrTargetProfile *profile;
    uint64_t quota;
    size_t max_entry;
    bool rebuild;
} PlanSerialRequest;

typedef struct PlanSerialBuild {
    bool store_opened;
    bool ok;
    XrProgramTargetPlanBuildResult result;
    uint8_t *bytes;
    size_t size;
    char error[512];
} PlanSerialBuild;

static void serial_build(const PlanSerialRequest *request, PlanSerialBuild *out) {
    memset(out, 0, sizeof(*out));
    XrCacheStore *store =
        xr_test_program_plan_store_open(request->root, request->quota, request->max_entry);
    out->store_opened = store != NULL;
    if (!store)
        return;
    XrTestProgramPlanBuildInput input = {
        .semantic = request->semantic,
        .dependencies = NULL,
        .dependency_count = 0u,
        .profile = request->profile,
        .store = store,
        .rebuild = request->rebuild,
        .cancellation = NULL,
    };
    XrProgramTargetPlanBuildResult result = {0};
    out->ok = xr_test_program_plan_build(&input, &result, out->error, sizeof(out->error));
    if (out->ok && result.plan)
        (void) xr_test_program_plan_encode(result.plan, &out->bytes, &out->size);
    out->result = result;
    out->result.plan = NULL;
    xr_program_target_plan_build_result_release(&result);
    xr_cache_store_close(store);
}

static void serial_build_release(PlanSerialBuild *build) {
    xr_test_program_plan_encoded_free(build->bytes);
    build->bytes = NULL;
    build->size = 0;
}

/* The store names an object by its key, so the single file under a kind
 * directory is the artifact a hostile case must corrupt in place. */
static char *single_object_path(const char *root, const char *kind) {
    char *directory = xr_path_join(root, kind);
    if (!directory)
        return NULL;
    XrDirIter *iterator = xr_dir_open(directory);
    char *found = NULL;
    if (iterator) {
        XrDirEntry entry;
        while (xr_dir_next(iterator, &entry)) {
            if (entry.is_dir || strncmp(entry.name, ".tmp-", 5u) == 0)
                continue;
            if (found) {
                xr_free(found);
                found = NULL;
                break;
            }
            found = xr_path_join(directory, entry.name);
        }
        xr_dir_close(iterator);
    }
    xr_free(directory);
    return found;
}

static bool replace_object(const char *path, const uint8_t *bytes, size_t size) {
    return xr_fs_remove(path) == 0 && xr_fs_write_new_file_sync(path, bytes, size) == 0;
}

/* A refused candidate must cost a verified recomputation of the same answer.
 * Serving a weaker plan, or accepting the planted bytes, would both show up
 * as a byte difference against the canonical encoding. */
static void assert_refused_then_recomputed(const PlanSerialBuild *canonical,
                                           const PlanSerialBuild *recovered) {
    ASSERT_TRUE(recovered->ok);
    ASSERT_TRUE(recovered->result.cache_load_attempted);
    ASSERT_FALSE(recovered->result.cache_hit);
    ASSERT_TRUE(recovered->result.cache_candidate_rejected);
    ASSERT_TRUE(recovered->result.built);
    ASSERT_EQ_UINT(recovered->size, canonical->size);
    ASSERT_MEM_EQ(recovered->bytes, canonical->bytes, canonical->size);
}

typedef enum PlanCancelBoundary {
    PLAN_CANCEL_NONE = 0,      /* the build completed before the request landed */
    PLAN_CANCEL_BEFORE_CACHE,  /* refused before the store was consulted */
    PLAN_CANCEL_AFTER_LOAD,    /* refused after a load attempt, before building */
    PLAN_CANCEL_AFTER_BUILD,   /* refused with a plan in hand, before publishing */
    PLAN_CANCEL_AFTER_PUBLISH, /* refused after publication was attempted */
    PLAN_CANCEL_BOUNDARY_COUNT,
} PlanCancelBoundary;

static PlanCancelBoundary classify_cancel(const XrProgramTargetPlanBuildResult *result) {
    if (!result->cancelled)
        return PLAN_CANCEL_NONE;
    if (result->cache_publish_attempted)
        return PLAN_CANCEL_AFTER_PUBLISH;
    if (result->built)
        return PLAN_CANCEL_AFTER_BUILD;
    if (result->cache_load_attempted)
        return PLAN_CANCEL_AFTER_LOAD;
    return PLAN_CANCEL_BEFORE_CACHE;
}

typedef struct PlanCanceller {
    PlanBarrier *barrier;
    XrProgramTargetPlanCancellationToken *token;
    uint64_t delay_ns;
} PlanCanceller;

static void *plan_canceller_main(void *argument) {
    PlanCanceller *canceller = (PlanCanceller *) argument;
    barrier_wait(canceller->barrier);
    if (canceller->delay_ns)
        xr_time_sleep_ns(canceller->delay_ns);
    xr_program_target_plan_cancellation_token_request(canceller->token);
    return NULL;
}

/* Time one uncancelled cold build so the sweep below can be expressed as
 * fractions of it. A fixed delay list would either land entirely inside the
 * build on a slow machine or entirely after it on a fast one. */
static uint64_t measure_cold_build_ns(const char *root, uint32_t ordinal) {
    XrSemanticPlan *semantic = xr_test_program_plan_semantic(ordinal);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    if (!semantic || !profile) {
        xr_target_profile_free(profile);
        xr_semantic_plan_free(semantic);
        return 0;
    }
    PlanSerialRequest request = {
        .root = root,
        .semantic = semantic,
        .profile = profile,
        .quota = PLAN_QUOTA,
        .max_entry = PLAN_MAX_ENTRY,
    };
    PlanSerialBuild build;
    uint64_t started = xr_time_monotonic_ns();
    serial_build(&request, &build);
    uint64_t elapsed = xr_time_monotonic_ns() - started;
    serial_build_release(&build);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    return build.ok ? elapsed : 0;
}

/* Sweep the request across the whole lifetime of a build instead of only
 * asking before it starts. Which internal checkpoint observes the request is
 * timing-dependent, so the case asserts the invariants that must hold at
 * every one of them and reports which were reached. */
TEST(cancellation_across_build_boundaries_is_fail_closed) {
    /* Eighths of one measured build, extended past its end so the sweep is
     * proven to cross the whole lifetime rather than to stop inside it. */
    static const uint32_t kEighths[] = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 10u, 12u, 16u};
    char root[XR_PATH_MAX];
    ASSERT_EQ_INT(xr_temp_dir_create("xray-plan-cache-cancel", root, sizeof(root)), 0);
    uint32_t observed[PLAN_CANCEL_BOUNDARY_COUNT] = {0};
    uint64_t build_ns = measure_cold_build_ns(root, 899u);
    ASSERT_TRUE(build_ns > 0u);

    for (uint32_t round = 0; round < sizeof(kEighths) / sizeof(kEighths[0]); round++) {
        /* A fresh ordinal keys a fresh object, so every round races an actual
         * publication rather than settling into a warm hit. */
        XrSemanticPlan *semantic = xr_test_program_plan_semantic(900u + round);
        XrTargetProfile *profile =
            xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        ASSERT_NOT_NULL(semantic);
        ASSERT_NOT_NULL(profile);

        XrProgramTargetPlanCancellationToken token;
        xr_program_target_plan_cancellation_token_init(&token);
        PlanBarrier barrier;
        barrier_init(&barrier, 2u);
        PlanWorker worker = {
            .barrier = &barrier,
            .root = root,
            .semantic = semantic,
            .profile = profile,
            .cancellation = &token,
        };
        PlanCanceller canceller = {
            .barrier = &barrier,
            .token = &token,
            .delay_ns = build_ns * kEighths[round] / 8u,
        };
        xr_thread_t threads[2];
        ASSERT_TRUE(xr_thread_create(&threads[0], plan_worker_main, &worker));
        ASSERT_TRUE(xr_thread_create(&threads[1], plan_canceller_main, &canceller));
        ASSERT_EQ_INT(xr_thread_join(threads[0], NULL), 0);
        ASSERT_EQ_INT(xr_thread_join(threads[1], NULL), 0);
        barrier_destroy(&barrier);

        PlanCancelBoundary boundary = classify_cancel(&worker.result);
        observed[boundary]++;
        if (boundary == PLAN_CANCEL_NONE) {
            ASSERT_TRUE(worker.ok);
            ASSERT_NOT_NULL(worker.bytes);
        } else {
            ASSERT_FALSE(worker.ok);
            ASSERT_NULL(worker.bytes);
            ASSERT_NOT_NULL(strstr(worker.error, "cancelled"));
        }
        XrTestProgramPlanStoreInventory inventory;
        xr_test_program_plan_store_inventory(root, &inventory);
        ASSERT_EQ_UINT(inventory.temps, 0u);

        /* Whatever the cancellation interrupted, an ordinary build of the same
         * authority must still produce the one canonical answer. */
        PlanSerialRequest request = {
            .root = root,
            .semantic = semantic,
            .profile = profile,
            .quota = PLAN_QUOTA,
            .max_entry = PLAN_MAX_ENTRY,
        };
        PlanSerialBuild recovery;
        serial_build(&request, &recovery);
        ASSERT_TRUE(recovery.ok);
        ASSERT_NOT_NULL(recovery.bytes);
        if (worker.bytes) {
            ASSERT_EQ_UINT(recovery.size, worker.size);
            ASSERT_MEM_EQ(recovery.bytes, worker.bytes, worker.size);
        }
        serial_build_release(&recovery);
        xr_test_program_plan_encoded_free(worker.bytes);
        xr_target_profile_free(profile);
        xr_semantic_plan_free(semantic);
    }

    printf("\n    cancel sweep over %llu ns: none=%u before-cache=%u after-load=%u "
           "after-build=%u after-publish=%u\n    ",
           (unsigned long long) build_ns, observed[PLAN_CANCEL_NONE],
           observed[PLAN_CANCEL_BEFORE_CACHE], observed[PLAN_CANCEL_AFTER_LOAD],
           observed[PLAN_CANCEL_AFTER_BUILD], observed[PLAN_CANCEL_AFTER_PUBLISH]);
    /* The shortest and the longest delay must land on opposite sides of the
     * build, or the sweep never crossed its lifetime and proved nothing. */
    ASSERT_TRUE(observed[PLAN_CANCEL_NONE] > 0u);
    ASSERT_TRUE(observed[PLAN_CANCEL_NONE] < sizeof(kEighths) / sizeof(kEighths[0]));
    xr_test_program_plan_store_remove(root);
}

TEST(truncated_and_mutated_objects_are_refused_and_recomputed) {
    char root[XR_PATH_MAX];
    ASSERT_EQ_INT(xr_temp_dir_create("xray-plan-cache-hostile", root, sizeof(root)), 0);
    XrSemanticPlan *semantic = xr_test_program_plan_semantic(611u);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(semantic);
    ASSERT_NOT_NULL(profile);
    PlanSerialRequest request = {
        .root = root,
        .semantic = semantic,
        .profile = profile,
        .quota = PLAN_QUOTA,
        .max_entry = PLAN_MAX_ENTRY,
    };
    PlanSerialBuild canonical;
    serial_build(&request, &canonical);
    ASSERT_TRUE(canonical.ok);
    ASSERT_TRUE(canonical.result.cache_published);

    char *object = single_object_path(root, "xtp");
    ASSERT_NOT_NULL(object);
    uint8_t *original = NULL;
    size_t original_size = 0;
    ASSERT_EQ_INT(xr_fs_read_regular_file(object, PLAN_MAX_ENTRY, &original, &original_size), 0);
    ASSERT_TRUE(original_size > 8u);

    ASSERT_TRUE(replace_object(object, original, original_size / 2u));
    PlanSerialBuild truncated;
    serial_build(&request, &truncated);
    assert_refused_then_recomputed(&canonical, &truncated);
    serial_build_release(&truncated);

    /* Restore the full length and move one payload byte instead, so the
     * refusal cannot be credited to the shorter file alone. */
    uint8_t *mutated = xr_malloc(original_size);
    ASSERT_NOT_NULL(mutated);
    memcpy(mutated, original, original_size);
    mutated[original_size - 1u] = (uint8_t) (mutated[original_size - 1u] ^ 0xffu);
    ASSERT_TRUE(replace_object(object, mutated, original_size));
    PlanSerialBuild flipped;
    serial_build(&request, &flipped);
    assert_refused_then_recomputed(&canonical, &flipped);
    serial_build_release(&flipped);

    XrTestProgramPlanStoreInventory inventory;
    xr_test_program_plan_store_inventory(root, &inventory);
    ASSERT_EQ_UINT(inventory.temps, 0u);
    xr_free(mutated);
    xr_free(original);
    xr_free(object);
    serial_build_release(&canonical);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    xr_test_program_plan_store_remove(root);
}

/* A plan built for another machine, planted under the key this machine would
 * look up. The key alone must not be treated as proof of what the bytes mean. */
TEST(foreign_target_plan_under_the_right_key_is_refused) {
    char native_root[XR_PATH_MAX];
    char foreign_root[XR_PATH_MAX];
    ASSERT_EQ_INT(xr_temp_dir_create("xray-plan-cache-native", native_root, sizeof(native_root)),
                  0);
    ASSERT_EQ_INT(xr_temp_dir_create("xray-plan-cache-foreign", foreign_root, sizeof(foreign_root)),
                  0);
    XrSemanticPlan *semantic = xr_test_program_plan_semantic(622u);
    XrTargetProfile *native = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrTargetProfile *foreign =
        xr_test_target_profile_build(true, XR_TARGET_RUNTIME_PROFILE_FREESTANDING);
    ASSERT_NOT_NULL(semantic);
    ASSERT_NOT_NULL(native);
    ASSERT_NOT_NULL(foreign);

    PlanSerialRequest native_request = {
        .root = native_root,
        .semantic = semantic,
        .profile = native,
        .quota = PLAN_QUOTA,
        .max_entry = PLAN_MAX_ENTRY,
    };
    PlanSerialRequest foreign_request = native_request;
    foreign_request.root = foreign_root;
    foreign_request.profile = foreign;

    PlanSerialBuild canonical;
    PlanSerialBuild other;
    serial_build(&native_request, &canonical);
    serial_build(&foreign_request, &other);
    ASSERT_TRUE(canonical.ok);
    ASSERT_TRUE(other.ok);
    ASSERT_TRUE(canonical.size != other.size ||
                memcmp(canonical.bytes, other.bytes, canonical.size) != 0);

    char *native_object = single_object_path(native_root, "xtp");
    char *foreign_object = single_object_path(foreign_root, "xtp");
    ASSERT_NOT_NULL(native_object);
    ASSERT_NOT_NULL(foreign_object);
    uint8_t *foreign_bytes = NULL;
    size_t foreign_size = 0;
    ASSERT_EQ_INT(
        xr_fs_read_regular_file(foreign_object, PLAN_MAX_ENTRY, &foreign_bytes, &foreign_size), 0);
    ASSERT_TRUE(replace_object(native_object, foreign_bytes, foreign_size));

    PlanSerialBuild recovered;
    serial_build(&native_request, &recovered);
    assert_refused_then_recomputed(&canonical, &recovered);

    serial_build_release(&recovered);
    xr_free(foreign_bytes);
    xr_free(foreign_object);
    xr_free(native_object);
    serial_build_release(&other);
    serial_build_release(&canonical);
    xr_target_profile_free(foreign);
    xr_target_profile_free(native);
    xr_semantic_plan_free(semantic);
    xr_test_program_plan_store_remove(foreign_root);
    xr_test_program_plan_store_remove(native_root);
}

/* An object the store refuses to hold must refuse the build, not quietly
 * hand back a plan nobody can prove was cached. */
TEST(over_budget_publication_is_refused_without_residue) {
    char root[XR_PATH_MAX];
    ASSERT_EQ_INT(xr_temp_dir_create("xray-plan-cache-budget", root, sizeof(root)), 0);
    XrSemanticPlan *semantic = xr_test_program_plan_semantic(633u);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(semantic);
    ASSERT_NOT_NULL(profile);

    PlanSerialRequest request = {
        .root = root,
        .semantic = semantic,
        .profile = profile,
        .quota = PLAN_QUOTA,
        .max_entry = 16u,
    };
    PlanSerialBuild refused;
    serial_build(&request, &refused);
    ASSERT_TRUE(refused.store_opened);
    ASSERT_FALSE(refused.ok);
    ASSERT_TRUE(refused.result.built);
    ASSERT_TRUE(refused.result.cache_publish_attempted);
    ASSERT_EQ_INT(refused.result.publish_status, XR_CACHE_PUBLISH_TOO_LARGE);
    ASSERT_FALSE(refused.result.cache_published);
    ASSERT_NULL(refused.bytes);
    assert_store_is_whole(root, 0u);

    /* The same authority under a budget that admits the object must still
     * publish one object, so the refusal was the budget and not the plan. */
    request.max_entry = PLAN_MAX_ENTRY;
    PlanSerialBuild admitted;
    serial_build(&request, &admitted);
    ASSERT_TRUE(admitted.ok);
    ASSERT_TRUE(admitted.result.cache_published);
    assert_store_is_whole(root, 1u);

    serial_build_release(&admitted);
    serial_build_release(&refused);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    xr_test_program_plan_store_remove(root);
}

/* Two independently allocated authorities that state the same content must
 * derive one cache identity, and two that differ must not. If any part of the
 * key derivation or the load verifier reached for an address instead of
 * content, a build would miss the object it published itself as soon as the
 * authority was rebuilt at another address, which is what every process after
 * the first one does. */
TEST(equal_content_authorities_share_one_cache_identity) {
    char root[XR_PATH_MAX];
    ASSERT_EQ_INT(xr_temp_dir_create("xray-plan-cache-identity", root, sizeof(root)), 0);
    XrSemanticPlan *first = xr_test_program_plan_semantic(644u);
    XrSemanticPlan *second = xr_test_program_plan_semantic(644u);
    XrSemanticPlan *other = xr_test_program_plan_semantic(645u);
    XrTargetProfile *first_profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrTargetProfile *second_profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);
    ASSERT_NOT_NULL(other);
    ASSERT_NOT_NULL(first_profile);
    ASSERT_NOT_NULL(second_profile);
    ASSERT_TRUE(first != second);
    ASSERT_TRUE(first_profile != second_profile);

    PlanSerialRequest request = {
        .root = root,
        .semantic = first,
        .profile = first_profile,
        .quota = PLAN_QUOTA,
        .max_entry = PLAN_MAX_ENTRY,
    };
    PlanSerialBuild published;
    serial_build(&request, &published);
    ASSERT_TRUE(published.ok);
    ASSERT_TRUE(published.result.cache_published);

    request.semantic = second;
    request.profile = second_profile;
    PlanSerialBuild served;
    serial_build(&request, &served);
    ASSERT_TRUE(served.ok);
    ASSERT_TRUE(served.result.cache_hit);
    ASSERT_FALSE(served.result.built);
    ASSERT_EQ_UINT(served.size, published.size);
    ASSERT_MEM_EQ(served.bytes, published.bytes, published.size);

    request.semantic = other;
    PlanSerialBuild distinct;
    serial_build(&request, &distinct);
    ASSERT_TRUE(distinct.ok);
    ASSERT_FALSE(distinct.result.cache_hit);
    ASSERT_TRUE(distinct.result.cache_published);
    ASSERT_TRUE(distinct.size != published.size ||
                memcmp(distinct.bytes, published.bytes, published.size) != 0);

    XrTestProgramPlanStoreInventory inventory;
    xr_test_program_plan_store_inventory(root, &inventory);
    ASSERT_EQ_UINT(inventory.objects, 2u);
    ASSERT_EQ_UINT(inventory.temps, 0u);
    ASSERT_TRUE(inventory.object_bytes >= published.size);

    serial_build_release(&distinct);
    serial_build_release(&served);
    serial_build_release(&published);
    xr_target_profile_free(second_profile);
    xr_target_profile_free(first_profile);
    xr_semantic_plan_free(other);
    xr_semantic_plan_free(second);
    xr_semantic_plan_free(first);
    xr_test_program_plan_store_remove(root);
}

TEST_MAIN_BEGIN()
RUN_TEST(parallel_cold_builds_publish_one_canonical_plan);
RUN_TEST(parallel_warm_builds_serve_one_canonical_plan);
RUN_TEST(writer_and_readers_agree_on_one_canonical_plan);
RUN_TEST(cancellation_across_build_boundaries_is_fail_closed);
RUN_TEST(truncated_and_mutated_objects_are_refused_and_recomputed);
RUN_TEST(foreign_target_plan_under_the_right_key_is_refused);
RUN_TEST(over_budget_publication_is_refused_without_residue);
RUN_TEST(equal_content_authorities_share_one_cache_identity);
TEST_MAIN_END()
