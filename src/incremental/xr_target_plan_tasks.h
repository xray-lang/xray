/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_plan_tasks.h - Deterministic parallel TargetPlan construction
 *
 * KEY CONCEPT:
 *   Workers own disjoint result rows and never publish into a compiler bundle.
 *   The caller consumes rows in canonical input order after every worker joins.
 */

#ifndef XR_TARGET_PLAN_TASKS_H
#define XR_TARGET_PLAN_TASKS_H

#include "xr_cache_artifact_verify.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_TARGET_PLAN_TASK_ERROR_SIZE 512u
#define XR_TARGET_PLAN_TASK_INDEX_NONE UINT32_MAX

typedef struct XrTargetPlanTaskInput {
    const XrSemanticPlan *semantic_plan;
} XrTargetPlanTaskInput;

typedef struct XrTargetPlanTaskResult {
    XrTargetPlan *plan;
    XrCacheLoadStatus load_status;
    XrCachePublishStatus publish_status;
    bool complete;
    bool cache_enabled;
    bool rebuild_requested;
    bool cache_load_attempted;
    bool cache_hit;
    bool cache_candidate_rejected;
    bool cache_publish_attempted;
    bool cache_published;
    bool built;
    char error[XR_TARGET_PLAN_TASK_ERROR_SIZE];
} XrTargetPlanTaskResult;

typedef struct XrTargetPlanTaskStats {
    uint32_t worker_count;
    uint32_t first_failed_index;
    uint32_t hits;
    uint32_t misses;
    uint32_t rejected;
    uint32_t published;
} XrTargetPlanTaskStats;

typedef struct XrTargetPlanTaskBatch {
    const XrTargetPlanTaskInput *inputs;
    uint32_t input_count;
    XrTargetProfile *profile;
    XrCacheStore *cache_store;
    XrCacheFingerprint optimization_budget;
    bool rebuild;
    uint32_t worker_limit;
    XrTargetPlanTaskResult *results;
} XrTargetPlanTaskBatch;

/* Run one independent task per input. worker_limit=0 selects the host CPU
 * count. Results remain input-indexed regardless of completion order. */
XR_FUNC bool xr_target_plan_tasks_run(
    const XrTargetPlanTaskBatch *batch, XrTargetPlanTaskStats *stats,
    char *error, size_t error_size);

/* Release every owned plan and clear the result rows. */
XR_FUNC void xr_target_plan_task_results_release(
    XrTargetPlanTaskResult *results, uint32_t result_count);

#endif  // XR_TARGET_PLAN_TASKS_H
