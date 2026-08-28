/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_target_plan_build.h - Program TargetPlan build/cache authority
 *
 * KEY CONCEPT:
 *   One request names one complete program semantic authority. A successful
 *   result owns exactly one independently verified program TargetPlan.
 */

#ifndef XR_PROGRAM_TARGET_PLAN_BUILD_H
#define XR_PROGRAM_TARGET_PLAN_BUILD_H

#include "xr_cache_artifact_verify.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

typedef enum XrProgramTargetPlanCancellationCheckpoint {
    XR_PROGRAM_TARGET_PLAN_CANCEL_NONE = 0,
    XR_PROGRAM_TARGET_PLAN_CANCEL_BEFORE_CACHE,
    XR_PROGRAM_TARGET_PLAN_CANCEL_AFTER_LOAD,
    XR_PROGRAM_TARGET_PLAN_CANCEL_AFTER_BUILD,
    XR_PROGRAM_TARGET_PLAN_CANCEL_AFTER_PUBLISH,
} XrProgramTargetPlanCancellationCheckpoint;

/* One build owns one token. Cancellation is monotonic: callers create a new
 * token instead of racing a reset with an active build. A scheduled checkpoint
 * lets a synchronous caller place the same bound without a timing race. */
typedef struct XrProgramTargetPlanCancellationToken {
    atomic_bool requested;
    atomic_uint scheduled_checkpoint;
} XrProgramTargetPlanCancellationToken;

XR_FUNC void
xr_program_target_plan_cancellation_token_init(XrProgramTargetPlanCancellationToken *token);
XR_FUNC void
xr_program_target_plan_cancellation_token_request(XrProgramTargetPlanCancellationToken *token);
XR_FUNC void xr_program_target_plan_cancellation_token_request_at_checkpoint(
    XrProgramTargetPlanCancellationToken *token,
    XrProgramTargetPlanCancellationCheckpoint checkpoint);
XR_FUNC bool xr_program_target_plan_cancellation_token_is_requested(
    const XrProgramTargetPlanCancellationToken *token);

typedef struct XrProgramTargetPlanBuildRequest {
    const XrSemanticPlan *semantic_plan;
    const XrSemanticPlan *const *semantic_dependencies;
    uint32_t semantic_dependency_count;
    /* Every module of the program, in program module row order, when the caller
     * has one canonical module set. A set of more than one module is planned as
     * one partitioned TargetPlan so that each module's target rows stay
     * attributable; a single module keeps the plain module-set path. */
    const XrSemanticPlan *const *program_modules;
    uint32_t program_module_count;
    XrTargetProfile *profile;
    XrCacheStore *cache_store;
    XrCacheFingerprint optimization_budget;
    bool rebuild;
    XrProgramTargetPlanCancellationToken *cancellation;
} XrProgramTargetPlanBuildRequest;

typedef struct XrProgramTargetPlanBuildResult {
    XrTargetPlan *plan;
    XrCacheLoadStatus load_status;
    XrCachePublishStatus publish_status;
    bool cache_enabled;
    bool rebuild_requested;
    bool cache_load_attempted;
    bool cache_hit;
    bool cache_candidate_rejected;
    bool cache_publish_attempted;
    bool cache_published;
    bool built;
    bool cancelled;
} XrProgramTargetPlanBuildResult;

/* Build or load the single TargetPlan owned by the supplied complete program
 * authority. The result never owns a partial or unverified plan. */
XR_FUNC bool xr_program_target_plan_build(const XrProgramTargetPlanBuildRequest *request,
                                          XrProgramTargetPlanBuildResult *result, char *error,
                                          size_t error_size);

/* Release the owned plan and clear the result. */
XR_FUNC void xr_program_target_plan_build_result_release(XrProgramTargetPlanBuildResult *result);

#endif  // XR_PROGRAM_TARGET_PLAN_BUILD_H
