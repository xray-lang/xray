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
/* One published plan measures 3712 bytes for this fixture. The collector
 * admits a whole object but holds fewer of them than the readers publish. */
#define PLAN_COLLECT_QUOTA (8u * 1024u)
#define PLAN_COLLECT_MAX_ENTRY (4u * 1024u)

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
    XrProgramTargetPlanCancellationToken *cancellation;
} PlanSerialRequest;

typedef struct PlanSerialBuild {
    bool store_opened;
    bool ok;
    bool returned_plan;
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
        .cancellation = request->cancellation,
    };
    XrProgramTargetPlanBuildResult result = {0};
    out->ok = xr_test_program_plan_build(&input, &result, out->error, sizeof(out->error));
    out->returned_plan = result.plan != NULL;
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

typedef struct PlanCancellationCase {
    XrProgramTargetPlanCancellationCheckpoint checkpoint;
    bool load_attempted;
    bool built;
    bool publish_attempted;
    bool published;
} PlanCancellationCase;

/* Schedule each checkpoint explicitly. A timing sweep can pass without ever
 * reaching a narrow checkpoint, so it cannot qualify that checkpoint's
 * ownership and publication guarantees. */
TEST(cancellation_across_build_boundaries_is_fail_closed) {
    static const PlanCancellationCase kCases[] = {
        {XR_PROGRAM_TARGET_PLAN_CANCEL_BEFORE_CACHE, false, false, false, false},
        {XR_PROGRAM_TARGET_PLAN_CANCEL_AFTER_LOAD, true, false, false, false},
        {XR_PROGRAM_TARGET_PLAN_CANCEL_AFTER_BUILD, true, true, false, false},
        {XR_PROGRAM_TARGET_PLAN_CANCEL_AFTER_PUBLISH, true, true, true, true},
    };
    char root[XR_PATH_MAX];
    ASSERT_EQ_INT(xr_temp_dir_create("xray-plan-cache-cancel", root, sizeof(root)), 0);

    for (uint32_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        XrSemanticPlan *semantic = xr_test_program_plan_semantic(900u + i);
        XrTargetProfile *profile =
            xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        ASSERT_NOT_NULL(semantic);
        ASSERT_NOT_NULL(profile);

        XrProgramTargetPlanCancellationToken token;
        xr_program_target_plan_cancellation_token_init(&token);
        xr_program_target_plan_cancellation_token_request_at_checkpoint(
            &token, kCases[i].checkpoint);
        PlanSerialRequest request = {
            .root = root,
            .semantic = semantic,
            .profile = profile,
            .quota = PLAN_QUOTA,
            .max_entry = PLAN_MAX_ENTRY,
            .cancellation = &token,
        };
        PlanSerialBuild cancelled;
        serial_build(&request, &cancelled);
        ASSERT_TRUE(cancelled.store_opened);
        ASSERT_FALSE(cancelled.ok);
        ASSERT_FALSE(cancelled.returned_plan);
        ASSERT_NULL(cancelled.bytes);
        ASSERT_TRUE(cancelled.result.cancelled);
        ASSERT_EQ_INT(cancelled.result.cache_load_attempted, kCases[i].load_attempted);
        ASSERT_EQ_INT(cancelled.result.built, kCases[i].built);
        ASSERT_EQ_INT(cancelled.result.cache_publish_attempted, kCases[i].publish_attempted);
        ASSERT_EQ_INT(cancelled.result.cache_published, kCases[i].published);
        ASSERT_NOT_NULL(strstr(cancelled.error, "cancelled"));

        XrTestProgramPlanStoreInventory inventory;
        xr_test_program_plan_store_inventory(root, &inventory);
        ASSERT_EQ_UINT(inventory.objects, i + (kCases[i].published ? 1u : 0u));
        ASSERT_EQ_UINT(inventory.temps, 0u);

        /* An ordinary build and its warm repeat must still produce the same
         * canonical answer after every refusal point. */
        request.cancellation = NULL;
        PlanSerialBuild recovery;
        PlanSerialBuild repeated;
        serial_build(&request, &recovery);
        serial_build(&request, &repeated);
        ASSERT_TRUE(recovery.ok);
        ASSERT_TRUE(repeated.ok);
        ASSERT_TRUE(recovery.returned_plan);
        ASSERT_TRUE(repeated.returned_plan);
        ASSERT_TRUE(repeated.result.cache_hit);
        ASSERT_EQ_UINT(recovery.size, repeated.size);
        ASSERT_MEM_EQ(recovery.bytes, repeated.bytes, recovery.size);
        assert_store_is_whole(root, i + 1u);

        serial_build_release(&repeated);
        serial_build_release(&recovery);
        serial_build_release(&cancelled);
        xr_target_profile_free(profile);
        xr_semantic_plan_free(semantic);
    }
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

/* Publish one plan under each profile in its own root, then plant the foreign
 * bytes under the key the native profile looks up. Serving them would mean the
 * key alone was treated as proof of what the bytes mean. */
static void assert_foreign_plan_is_refused(const XrSemanticPlan *semantic, XrTargetProfile *native,
                                           XrTargetProfile *foreign, const char *label) {
    char native_root[XR_PATH_MAX];
    char foreign_root[XR_PATH_MAX];
    char foreign_label[64];
    (void) snprintf(foreign_label, sizeof(foreign_label), "%s-foreign", label);
    ASSERT_EQ_INT(xr_temp_dir_create(label, native_root, sizeof(native_root)), 0);
    ASSERT_EQ_INT(xr_temp_dir_create(foreign_label, foreign_root, sizeof(foreign_root)), 0);

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
    /* The facet has to reach the plan, or planting its bytes would prove
     * nothing about whether the key separates the two profiles. */
    ASSERT_TRUE(canonical.size != other.size ||
                memcmp(canonical.bytes, other.bytes, canonical.size) != 0);

    char *native_object = single_object_path(native_root, "xtp");
    char *foreign_object = single_object_path(foreign_root, "xtp");
    ASSERT_NOT_NULL(native_object);
    ASSERT_NOT_NULL(foreign_object);

    /* Two profiles that differ must not share an address. Building the foreign
     * one against the native root has to miss and publish beside the native
     * object rather than be served it. */
    PlanSerialRequest shared_request = native_request;
    shared_request.profile = foreign;
    PlanSerialBuild shared;
    serial_build(&shared_request, &shared);
    ASSERT_TRUE(shared.ok);
    ASSERT_TRUE(shared.result.cache_load_attempted);
    ASSERT_FALSE(shared.result.cache_hit);
    ASSERT_TRUE(shared.result.cache_published);
    XrTestProgramPlanStoreInventory inventory;
    xr_test_program_plan_store_inventory(native_root, &inventory);
    ASSERT_EQ_UINT(inventory.objects, 2u);
    ASSERT_EQ_UINT(inventory.temps, 0u);
    serial_build_release(&shared);

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
    xr_test_program_plan_store_remove(foreign_root);
    xr_test_program_plan_store_remove(native_root);
}

static XrTargetProfile *build_from_fixture(XrTestTargetProfileFixture *fixture) {
    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    if (!xr_target_profile_build(&fixture->input, &profile, error, sizeof(error)))
        return NULL;
    return profile;
}

TEST(a_plan_for_another_machine_is_refused_under_this_machines_key) {
    XrSemanticPlan *semantic = xr_test_program_plan_semantic(622u);
    XrTargetProfile *native = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrTargetProfile *foreign =
        xr_test_target_profile_build(true, XR_TARGET_RUNTIME_PROFILE_FREESTANDING);
    ASSERT_NOT_NULL(semantic);
    ASSERT_NOT_NULL(native);
    ASSERT_NOT_NULL(foreign);
    assert_foreign_plan_is_refused(semantic, native, foreign, "xray-plan-cache-machine");
    xr_target_profile_free(foreign);
    xr_target_profile_free(native);
    xr_semantic_plan_free(semantic);
}

/* The whole-machine case above moves the machine facts, the provider set, and
 * the runtime ABI at once, so it cannot say which of them the key actually
 * reads. Move exactly one facet at a time. */
TEST(each_target_facet_alone_separates_the_cache_identity) {
    XrSemanticPlan *semantic = xr_test_program_plan_semantic(655u);
    ASSERT_NOT_NULL(semantic);
    XrTestTargetProfileFixture base;
    ASSERT_TRUE(
        xr_test_target_profile_fixture_init(&base, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    XrTargetProfile *native = build_from_fixture(&base);
    ASSERT_NOT_NULL(native);

    XrTestTargetProfileFixture machine;
    ASSERT_TRUE(
        xr_test_target_profile_fixture_init(&machine, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    machine.input.machine.atomic_width_mask |= XR_TARGET_ATOMIC_WIDTH_128;
    XrTargetProfile *machine_profile = build_from_fixture(&machine);
    ASSERT_NOT_NULL(machine_profile);
    assert_foreign_plan_is_refused(semantic, native, machine_profile,
                                   "xray-plan-cache-facet-machine");
    xr_target_profile_free(machine_profile);

    XrTestTargetProfileFixture provider;
    ASSERT_TRUE(
        xr_test_target_profile_fixture_init(&provider, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    provider.providers[0].allocator_max_alignment = 128u;
    XrTargetProfile *provider_profile = build_from_fixture(&provider);
    ASSERT_NOT_NULL(provider_profile);
    assert_foreign_plan_is_refused(semantic, native, provider_profile,
                                   "xray-plan-cache-facet-provider");
    xr_target_profile_free(provider_profile);

    XrTestTargetProfileFixture runtime;
    ASSERT_TRUE(
        xr_test_target_profile_fixture_init(&runtime, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    runtime.runtime_abi.dynamic_value.tags[1].required_flags = 2u;
    XrTargetProfile *runtime_profile = build_from_fixture(&runtime);
    ASSERT_NOT_NULL(runtime_profile);
    assert_foreign_plan_is_refused(semantic, native, runtime_profile,
                                   "xray-plan-cache-facet-runtime");
    xr_target_profile_free(runtime_profile);

    /* A mismatched object header never reaches a profile at all, so the cache
     * has no second chance to be the layer that catches it. */
    XrTestTargetProfileFixture header;
    ASSERT_TRUE(
        xr_test_target_profile_fixture_init(&header, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    header.object_header_materialization.target_endian = XR_RUNTIME_ENDIAN_BIG;
    ASSERT_NULL(build_from_fixture(&header));

    xr_target_profile_free(native);
    xr_semantic_plan_free(semantic);
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

/* A collector whose budget holds fewer objects than the readers published
 * evicts under them while they read. Its entry budget must still admit a whole
 * object: a store that cannot hold one discards it when it opens, which
 * empties the root before the race rather than during it. */
typedef struct PlanCollector {
    PlanBarrier *barrier;
    const char *root;
    uint32_t rounds;
    bool ok;
    uint64_t evicted;
} PlanCollector;

static void *plan_collector_main(void *argument) {
    PlanCollector *collector = (PlanCollector *) argument;
    XrCacheStore *store = xr_test_program_plan_store_open(collector->root, PLAN_COLLECT_QUOTA,
                                                          PLAN_COLLECT_MAX_ENTRY);
    barrier_wait(collector->barrier);
    collector->ok = store != NULL;
    for (uint32_t round = 0; store && round < collector->rounds; round++) {
        XrCacheCollectStats stats = {0};
        if (!xr_cache_store_collect(store, &stats)) {
            collector->ok = false;
            break;
        }
        collector->evicted += stats.removed_entries;
    }
    xr_cache_store_close(store);
    return NULL;
}

TEST(collection_racing_readers_still_serves_one_canonical_plan) {
    char root[XR_PATH_MAX];
    ASSERT_EQ_INT(xr_temp_dir_create("xray-plan-cache-collect", root, sizeof(root)), 0);
    const uint32_t readers = PLAN_WORKERS - 1u;
    XrSemanticPlan *semantics[PLAN_WORKERS] = {0};
    XrTargetProfile *profiles[PLAN_WORKERS] = {0};
    PlanWorker workers[PLAN_WORKERS] = {0};
    PlanSerialBuild canonical[PLAN_WORKERS] = {0};

    /* Each reader names a different program, so the store holds more objects
     * than the collector's budget admits and eviction has something to take. */
    for (uint32_t i = 0; i < readers; i++) {
        semantics[i] = xr_test_program_plan_semantic(766u + i);
        profiles[i] = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        ASSERT_NOT_NULL(semantics[i]);
        ASSERT_NOT_NULL(profiles[i]);
        workers[i].root = root;
        workers[i].semantic = semantics[i];
        workers[i].profile = profiles[i];
        PlanSerialRequest warm = {
            .root = root,
            .semantic = semantics[i],
            .profile = profiles[i],
            .quota = PLAN_QUOTA,
            .max_entry = PLAN_MAX_ENTRY,
        };
        serial_build(&warm, &canonical[i]);
        ASSERT_TRUE(canonical[i].ok);
        ASSERT_TRUE(canonical[i].result.cache_published);
    }

    XrTestProgramPlanStoreInventory primed;
    xr_test_program_plan_store_inventory(root, &primed);
    ASSERT_EQ_UINT(primed.objects, readers);

    PlanBarrier barrier;
    barrier_init(&barrier, readers + 1u);
    PlanCollector collector = {.barrier = &barrier, .root = root, .rounds = 16u};
    xr_thread_t threads[PLAN_WORKERS];
    for (uint32_t i = 0; i < readers; i++) {
        workers[i].barrier = &barrier;
        ASSERT_TRUE(xr_thread_create(&threads[i], plan_worker_main, &workers[i]));
    }
    ASSERT_TRUE(xr_thread_create(&threads[readers], plan_collector_main, &collector));
    for (uint32_t i = 0; i <= readers; i++)
        ASSERT_EQ_INT(xr_thread_join(threads[i], NULL), 0);
    barrier_destroy(&barrier);

    ASSERT_TRUE(collector.ok);
    uint32_t recomputed = 0;
    for (uint32_t i = 0; i < readers; i++)
        recomputed += workers[i].result.built ? 1u : 0u;
    /* The collector opens its store before the barrier releases the readers,
     * and a store cannot open over more bytes than it admits, so at least one
     * object is gone before any reader looks. A reader that recomputed is the
     * proof the race had something to race; without it the readers found an
     * undisturbed store and the case shows nothing. */
    ASSERT_TRUE(recomputed > 0u);
    printf("\n    collect race: evicted=%llu readers=%u recomputed=%u\n    ",
           (unsigned long long) collector.evicted, readers, recomputed);
    for (uint32_t i = 0; i < readers; i++) {
        /* A reader served from the store and a reader whose object was evicted
         * mid-flight must be indistinguishable in what they produce. */
        ASSERT_TRUE(workers[i].ok);
        ASSERT_NOT_NULL(workers[i].bytes);
        ASSERT_EQ_UINT(workers[i].size, canonical[i].size);
        ASSERT_MEM_EQ(workers[i].bytes, canonical[i].bytes, canonical[i].size);
        ASSERT_TRUE(workers[i].result.cache_hit || workers[i].result.built);
    }
    XrTestProgramPlanStoreInventory inventory;
    xr_test_program_plan_store_inventory(root, &inventory);
    ASSERT_EQ_UINT(inventory.temps, 0u);

    workers_release(workers, readers);
    for (uint32_t i = 0; i < readers; i++) {
        serial_build_release(&canonical[i]);
        xr_target_profile_free(profiles[i]);
        xr_semantic_plan_free(semantics[i]);
    }
    xr_test_program_plan_store_remove(root);
}

/* A writer that died between its temp file and the atomic rename leaves a name
 * the store must never read as a published object. */
TEST(crash_temp_residue_is_never_served_and_is_reclaimed) {
    char root[XR_PATH_MAX];
    ASSERT_EQ_INT(xr_temp_dir_create("xray-plan-cache-residue", root, sizeof(root)), 0);
    XrSemanticPlan *semantic = xr_test_program_plan_semantic(777u);
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

    /* Name the residue after the live object, which is the name a writer
     * publishing this very key would have used. */
    char *object = single_object_path(root, "xtp");
    ASSERT_NOT_NULL(object);
    char *directory = xr_path_join(root, "xtp");
    ASSERT_NOT_NULL(directory);
    const char *base = strrchr(object, '/');
    char residue_name[XR_PATH_MAX];
    (void) snprintf(residue_name, sizeof(residue_name), ".tmp-%s-0", base ? base + 1 : "orphan");
    char *residue = xr_path_join(directory, residue_name);
    ASSERT_NOT_NULL(residue);
    static const uint8_t junk[] = "half-written program target plan";
    ASSERT_EQ_INT(xr_fs_write_new_file_sync(residue, junk, sizeof(junk) - 1u), 0);

    XrTestProgramPlanStoreInventory before;
    xr_test_program_plan_store_inventory(root, &before);
    ASSERT_EQ_UINT(before.objects, 1u);
    ASSERT_EQ_UINT(before.temps, 1u);

    PlanSerialBuild served;
    serial_build(&request, &served);
    ASSERT_TRUE(served.ok);
    ASSERT_TRUE(served.result.cache_hit);
    ASSERT_EQ_UINT(served.size, canonical.size);
    ASSERT_MEM_EQ(served.bytes, canonical.bytes, canonical.size);

    /* Reclaiming the residue must not take the object beside it. */
    XrCacheStore *store = xr_test_program_plan_store_open(root, PLAN_QUOTA, PLAN_MAX_ENTRY);
    ASSERT_NOT_NULL(store);
    XrCacheCollectStats stats = {0};
    ASSERT_TRUE(xr_cache_store_collect(store, &stats));
    xr_cache_store_close(store);

    PlanSerialBuild after;
    serial_build(&request, &after);
    ASSERT_TRUE(after.ok);
    ASSERT_EQ_UINT(after.size, canonical.size);
    ASSERT_MEM_EQ(after.bytes, canonical.bytes, canonical.size);

    serial_build_release(&after);
    serial_build_release(&served);
    serial_build_release(&canonical);
    xr_free(residue);
    xr_free(directory);
    xr_free(object);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    xr_test_program_plan_store_remove(root);
}

TEST_MAIN_BEGIN()
RUN_TEST(parallel_cold_builds_publish_one_canonical_plan);
RUN_TEST(parallel_warm_builds_serve_one_canonical_plan);
RUN_TEST(writer_and_readers_agree_on_one_canonical_plan);
RUN_TEST(cancellation_across_build_boundaries_is_fail_closed);
RUN_TEST(truncated_and_mutated_objects_are_refused_and_recomputed);
RUN_TEST(a_plan_for_another_machine_is_refused_under_this_machines_key);
RUN_TEST(each_target_facet_alone_separates_the_cache_identity);
RUN_TEST(over_budget_publication_is_refused_without_residue);
RUN_TEST(equal_content_authorities_share_one_cache_identity);
RUN_TEST(collection_racing_readers_still_serves_one_canonical_plan);
RUN_TEST(crash_temp_residue_is_never_served_and_is_reclaimed);
TEST_MAIN_END()
