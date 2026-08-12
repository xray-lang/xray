/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_plan_tasks.c - Deterministic parallel TargetPlan construction
 */

#include "xr_target_plan_tasks.h"

#include "../base/xmalloc.h"
#include "../os/os_thread.h"
#include "../plan/format/xr_xtp_schema.h"
#include "../plan/target/xr_target_builder.h"
#include "../plan/target/xr_target_verify.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef struct XrTargetPlanTaskPool {
    const XrTargetPlanTaskInput *inputs;
    uint32_t input_count;
    XrTargetProfile *profile;
    XrCacheStore *cache_store;
    XrCacheFingerprint optimization_budget;
    bool rebuild;
    uint32_t worker_count;
    XrTargetPlanTaskResult *results;
} XrTargetPlanTaskPool;

typedef struct XrTargetPlanWorker {
    XrTargetPlanTaskPool *pool;
    uint32_t ordinal;
    atomic_bool finished;
} XrTargetPlanWorker;

#define XR_TARGET_PLAN_TASK_MAX_WORKERS 64u

static void set_error(char *error, size_t error_size, const char *format, ...) {
    if (!error || error_size == 0)
        return;
    va_list args;
    va_start(args, format);
    (void) vsnprintf(error, error_size, format, args);
    va_end(args);
    error[error_size - 1u] = '\0';
}

static bool rejected_load_status(XrCacheLoadStatus status) {
    return status == XR_CACHE_LOAD_REJECTED ||
           status == XR_CACHE_LOAD_CORRUPT ||
           status == XR_CACHE_LOAD_TOO_LARGE;
}

static bool rejected_publish_status(XrCachePublishStatus status) {
    return status == XR_CACHE_PUBLISH_REJECTED ||
           status == XR_CACHE_PUBLISH_CONFLICT ||
           status == XR_CACHE_PUBLISH_TOO_LARGE;
}

static bool process_task(XrTargetPlanTaskPool *pool, uint32_t index) {
    XrTargetPlanTaskResult *result = &pool->results[index];
    const XrSemanticPlan *semantic_plan = pool->inputs[index].semantic_plan;
    XrTargetPlan *plan = NULL;
    XrCacheKey key = {{0}};

    result->cache_enabled = pool->cache_store != NULL;
    result->rebuild_requested = pool->rebuild;
    result->load_status = XR_CACHE_LOAD_MISS;
    result->publish_status = XR_CACHE_PUBLISH_IO_ERROR;
    if (!semantic_plan) {
        set_error(result->error, sizeof(result->error),
                  "TargetPlan task lacks SemanticPlan authority");
        return false;
    }

    XrCacheXtpArtifactVerifyContext requirements = {
        .semantic_plan = semantic_plan,
        .semantic_dependencies =
            pool->inputs[index].semantic_dependencies,
        .semantic_dependency_count =
            pool->inputs[index].semantic_dependency_count,
        .target_profile = pool->profile,
        .optimization_budget = pool->optimization_budget,
    };
    if (pool->cache_store && !xr_cache_xtp_key(&requirements, &key)) {
        set_error(result->error, sizeof(result->error),
                  "TargetPlan cache key authority failed");
        return false;
    }

    if (pool->cache_store && !pool->rebuild) {
        XrCacheXtpArtifactLoadContext load = {
            .requirements = requirements,
        };
        XrCacheBlob blob = {0};
        result->cache_load_attempted = true;
        result->load_status = xr_cache_store_load(
            pool->cache_store, XR_CACHE_ARTIFACT_XTP, key,
            xr_cache_materialize_xtp_artifact, &load, &blob);
        xr_cache_blob_release(&blob);
        if (result->load_status == XR_CACHE_LOAD_HIT) {
            if (!load.accepted_plan) {
                set_error(result->error, sizeof(result->error),
                          "TargetPlan cache hit lacks a verified plan");
                return false;
            }
            plan = load.accepted_plan;
            result->cache_hit = true;
        } else {
            xr_target_plan_free(load.accepted_plan);
            result->cache_candidate_rejected =
                rejected_load_status(result->load_status);
        }
    }

    if (!plan) {
        if (!xr_target_plan_build_module_set(
                semantic_plan, pool->inputs[index].semantic_dependencies,
                pool->inputs[index].semantic_dependency_count, pool->profile,
                &plan, result->error, sizeof(result->error)))
            return false;
        result->built = true;
    }

    if (pool->cache_store && !result->cache_hit) {
        uint8_t *bytes = NULL;
        size_t size = 0;
        if (!xr_xtp_encode_plan(plan, &bytes, &size, result->error,
                                sizeof(result->error))) {
            xr_target_plan_free(plan);
            return false;
        }
        result->cache_publish_attempted = true;
        result->publish_status = xr_cache_store_publish(
            pool->cache_store, XR_CACHE_ARTIFACT_XTP, key, bytes, size,
            xr_cache_verify_xtp_artifact, &requirements);
        xr_xtp_encoded_free(bytes);
        result->cache_published =
            result->publish_status == XR_CACHE_PUBLISH_OK;
        if (rejected_publish_status(result->publish_status)) {
            set_error(result->error, sizeof(result->error),
                      "TargetPlan cache publication failed with status %d",
                      (int) result->publish_status);
            xr_target_plan_free(plan);
            return false;
        }
    }

    if (!plan || !xr_target_plan_is_verified(plan) ||
        !xr_target_plan_fingerprint_is_intact(plan) ||
        !xr_target_plan_verify(plan, result->error, sizeof(result->error))) {
        if (!result->error[0])
            set_error(result->error, sizeof(result->error),
                      "TargetPlan task produced an invalid plan");
        xr_target_plan_free(plan);
        return false;
    }
    result->plan = plan;
    result->complete = true;
    return true;
}

static void *worker_main(void *argument) {
    XrTargetPlanWorker *worker = (XrTargetPlanWorker *) argument;
    XrTargetPlanTaskPool *pool = worker ? worker->pool : NULL;
    if (!pool)
        return NULL;
    for (uint32_t index = worker->ordinal; index < pool->input_count;
         index += pool->worker_count)
        (void) process_task(pool, index);
    atomic_store_explicit(&worker->finished, true, memory_order_release);
    return NULL;
}

static uint32_t select_worker_count(uint32_t input_count, uint32_t limit) {
    uint32_t workers = limit;
    if (workers == 0) {
        unsigned int cpus = xr_os_cpu_count();
        workers = (uint32_t) cpus;
    }
    if (workers == 0)
        workers = 1;
    if (workers > XR_TARGET_PLAN_TASK_MAX_WORKERS)
        workers = XR_TARGET_PLAN_TASK_MAX_WORKERS;
    return workers < input_count ? workers : input_count;
}

static void collect_stats(const XrTargetPlanTaskResult *results,
                          uint32_t result_count, uint32_t worker_count,
                          XrTargetPlanTaskStats *stats) {
    memset(stats, 0, sizeof(*stats));
    stats->worker_count = worker_count;
    stats->first_failed_index = XR_TARGET_PLAN_TASK_INDEX_NONE;
    for (uint32_t i = 0; i < result_count; i++) {
        const XrTargetPlanTaskResult *result = &results[i];
        if (!result->complete &&
            stats->first_failed_index == XR_TARGET_PLAN_TASK_INDEX_NONE)
            stats->first_failed_index = i;
        if (!result->cache_enabled ||
            (!result->cache_load_attempted &&
             !result->rebuild_requested))
            continue;
        if (result->cache_hit) {
            stats->hits++;
        } else {
            stats->misses++;
            if (result->cache_candidate_rejected)
                stats->rejected++;
        }
        if (result->cache_published)
            stats->published++;
    }
}

bool xr_target_plan_tasks_run(
    const XrTargetPlanTaskBatch *batch, XrTargetPlanTaskStats *stats,
    char *error, size_t error_size) {
    if (error && error_size)
        error[0] = '\0';
    if (stats) {
        memset(stats, 0, sizeof(*stats));
        stats->first_failed_index = XR_TARGET_PLAN_TASK_INDEX_NONE;
    }
    if (!batch || !batch->inputs || batch->input_count == 0 ||
        !batch->profile || !batch->results || !stats ||
        batch->input_count > SIZE_MAX / sizeof(*batch->results) ||
        !xr_target_profile_verify(batch->profile, error, error_size))
        return false;
    memset(batch->results, 0,
           batch->input_count * sizeof(*batch->results));

    uint32_t worker_count =
        select_worker_count(batch->input_count, batch->worker_limit);
    XrTargetPlanTaskPool pool = {
        .inputs = batch->inputs,
        .input_count = batch->input_count,
        .profile = batch->profile,
        .cache_store = batch->cache_store,
        .optimization_budget = batch->optimization_budget,
        .rebuild = batch->rebuild,
        .worker_count = worker_count,
        .results = batch->results,
    };
    XrTargetPlanWorker *workers = (XrTargetPlanWorker *) xr_calloc(
        worker_count, sizeof(*workers));
    xr_thread_t *threads = worker_count > 1
                               ? (xr_thread_t *) xr_calloc(
                                     worker_count - 1u, sizeof(*threads))
                               : NULL;
    if (!workers || (worker_count > 1 && !threads)) {
        xr_free(threads);
        xr_free(workers);
        set_error(error, error_size,
                  "failed to allocate TargetPlan worker state");
        return false;
    }

    uint32_t created = 0;
    bool threads_ok = true;
    for (uint32_t i = 0; i < worker_count; i++) {
        workers[i].pool = &pool;
        workers[i].ordinal = i;
        atomic_init(&workers[i].finished, false);
        if (i != 0 &&
            !xr_thread_create(&threads[i - 1u], worker_main, &workers[i])) {
            threads_ok = false;
            break;
        }
        if (i != 0)
            created++;
    }
    (void) worker_main(&workers[0]);
    for (uint32_t i = 0; i < created; i++) {
        if (xr_thread_join(threads[i], NULL) != 0) {
            threads_ok = false;
            while (!atomic_load_explicit(&workers[i + 1u].finished,
                                         memory_order_acquire))
                xr_thread_yield();
            xr_thread_detach(threads[i]);
        }
    }
    collect_stats(batch->results, batch->input_count, worker_count, stats);
    xr_free(threads);
    xr_free(workers);

    if (!threads_ok) {
        set_error(error, error_size,
                  "failed to create or join every TargetPlan worker");
        return false;
    }
    if (stats->first_failed_index != XR_TARGET_PLAN_TASK_INDEX_NONE) {
        uint32_t failed = stats->first_failed_index;
        set_error(error, error_size, "TargetPlan task %u failed: %s", failed,
                  batch->results[failed].error[0]
                      ? batch->results[failed].error
                      : "unknown error");
        return false;
    }
    return true;
}

void xr_target_plan_task_results_release(XrTargetPlanTaskResult *results,
                                         uint32_t result_count) {
    if (!results || result_count > SIZE_MAX / sizeof(*results))
        return;
    for (uint32_t i = 0; i < result_count; i++)
        xr_target_plan_free(results[i].plan);
    memset(results, 0, result_count * sizeof(*results));
}
