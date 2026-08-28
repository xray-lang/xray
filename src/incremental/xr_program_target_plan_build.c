/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_target_plan_build.c - Program TargetPlan build/cache authority
 */

#include "xr_program_target_plan_build.h"

#include "../plan/format/xr_xtp_schema.h"
#include "../plan/semantic/xr_semantic_plan.h"
#include "../plan/semantic/xr_semantic_verify.h"
#include "../plan/target/xr_target_builder.h"
#include "../plan/target/xr_target_plan_internal.h"
#include "../plan/target/xr_target_verify.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void xr_program_target_plan_cancellation_token_init(XrProgramTargetPlanCancellationToken *token) {
    if (token) {
        atomic_init(&token->requested, false);
        atomic_init(&token->scheduled_checkpoint, XR_PROGRAM_TARGET_PLAN_CANCEL_NONE);
    }
}

void xr_program_target_plan_cancellation_token_request(
    XrProgramTargetPlanCancellationToken *token) {
    if (token)
        atomic_store_explicit(&token->requested, true, memory_order_release);
}

void xr_program_target_plan_cancellation_token_request_at_checkpoint(
    XrProgramTargetPlanCancellationToken *token,
    XrProgramTargetPlanCancellationCheckpoint checkpoint) {
    if (!token || checkpoint <= XR_PROGRAM_TARGET_PLAN_CANCEL_NONE ||
        checkpoint > XR_PROGRAM_TARGET_PLAN_CANCEL_AFTER_PUBLISH)
        return;
    atomic_store_explicit(&token->scheduled_checkpoint, (unsigned int) checkpoint,
                          memory_order_release);
}

bool xr_program_target_plan_cancellation_token_is_requested(
    const XrProgramTargetPlanCancellationToken *token) {
    return token && atomic_load_explicit(&token->requested, memory_order_acquire);
}

static bool cancellation_requested_at(XrProgramTargetPlanCancellationToken *token,
                                      XrProgramTargetPlanCancellationCheckpoint checkpoint) {
    if (!token)
        return false;
    if (atomic_load_explicit(&token->scheduled_checkpoint, memory_order_acquire) ==
        (unsigned int) checkpoint)
        xr_program_target_plan_cancellation_token_request(token);
    return xr_program_target_plan_cancellation_token_is_requested(token);
}

static void set_error(char *error, size_t error_size, const char *format, ...) {
    if (!error || error_size == 0)
        return;
    va_list args;
    va_start(args, format);
    (void) vsnprintf(error, error_size, format, args);
    va_end(args);
    error[error_size - 1u] = '\0';
}

static bool program_graph_family_is_supported(const XrSemanticPlan *semantic_plan) {
    const XrSemanticProgramProvenance *program = xr_semantic_plan_program_provenance(semantic_plan);
    return program &&
           (program->program_family == XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL ||
            program->program_family ==
                XR_PROGRAM_SEMANTIC_FAMILY_SOURCE_MODULE_SCALAR_PRIVATE_LEAF_CALL);
}

static bool rejected_load_status(XrCacheLoadStatus status) {
    return status == XR_CACHE_LOAD_REJECTED || status == XR_CACHE_LOAD_CORRUPT ||
           status == XR_CACHE_LOAD_TOO_LARGE;
}

static bool rejected_publish_status(XrCachePublishStatus status) {
    return status == XR_CACHE_PUBLISH_REJECTED || status == XR_CACHE_PUBLISH_CONFLICT ||
           status == XR_CACHE_PUBLISH_TOO_LARGE;
}

static bool cancel_build(XrProgramTargetPlanBuildResult *result, XrTargetPlan *plan, char *error,
                         size_t error_size) {
    xr_target_plan_free(plan);
    result->cancelled = true;
    set_error(error, error_size, "program TargetPlan build was cancelled");
    return false;
}

static bool program_authority_is_verified(const XrProgramTargetPlanBuildRequest *request) {
    char error[512] = {0};
    return request && request->semantic_plan &&
           xr_semantic_plan_is_verified(request->semantic_plan) &&
           xr_semantic_plan_verify_module_set(
               request->semantic_plan, request->semantic_dependencies,
               request->semantic_dependency_count, error, sizeof(error));
}

static bool collect_program_graph_semantics(const XrProgramTargetPlanBuildRequest *request,
                                            const XrSemanticPlan **semantic_modules,
                                            uint32_t *semantic_module_count, char *error,
                                            size_t error_size) {
    if (semantic_module_count)
        *semantic_module_count = 0u;
    if (!request || !request->semantic_plan || !semantic_modules || !semantic_module_count)
        return false;
    const XrSemanticProgramProvenance *entry =
        xr_semantic_plan_program_provenance(request->semantic_plan);
    if (!program_graph_family_is_supported(request->semantic_plan))
        return true;
    if (entry->module_count != 2u || request->semantic_dependency_count != 1u ||
        !request->semantic_dependencies) {
        set_error(error, error_size,
                  "program TargetPlan graph authority is not the exact bounded module set");
        return false;
    }
    const XrSemanticPlan *candidates[2] = {
        request->semantic_plan,
        request->semantic_dependencies[0],
    };
    for (uint32_t candidate = 0; candidate < 2u; candidate++) {
        const XrSemanticProgramProvenance *program =
            candidates[candidate] ? xr_semantic_plan_program_provenance(candidates[candidate])
                                  : NULL;
        if (!program || program->program_module_row >= 2u ||
            semantic_modules[program->program_module_row]) {
            set_error(error, error_size, "program TargetPlan graph authority is not canonical");
            return false;
        }
        semantic_modules[program->program_module_row] = candidates[candidate];
    }
    if (!xr_target_semantic_program_module_set_verify(semantic_modules, 2u, error, error_size))
        return false;
    *semantic_module_count = 2u;
    return true;
}

bool xr_program_target_plan_build(const XrProgramTargetPlanBuildRequest *request,
                                  XrProgramTargetPlanBuildResult *result, char *error,
                                  size_t error_size) {
    if (error && error_size)
        error[0] = '\0';
    if (!result)
        return false;
    memset(result, 0, sizeof(*result));
    result->load_status = XR_CACHE_LOAD_MISS;
    result->publish_status = XR_CACHE_PUBLISH_IO_ERROR;

    if (!request || !request->profile ||
        !xr_target_profile_verify(request->profile, error, error_size))
        return false;
    result->cache_enabled = request->cache_store != NULL;
    result->rebuild_requested = request->rebuild;
    if (cancellation_requested_at(request->cancellation,
                                  XR_PROGRAM_TARGET_PLAN_CANCEL_BEFORE_CACHE))
        return cancel_build(result, NULL, error, error_size);
    if (!program_authority_is_verified(request)) {
        set_error(error, error_size, "program TargetPlan build received wrong semantic authority");
        return false;
    }

    const XrSemanticPlan *program_semantic_modules[XR_TARGET_MAX_PROGRAM_MODULES] = {0};
    uint32_t program_semantic_module_count = 0u;
    if (!collect_program_graph_semantics(request, program_semantic_modules,
                                         &program_semantic_module_count, error, error_size))
        return false;

    XrCacheXtpArtifactVerifyContext requirements = {
        .semantic_plan = request->semantic_plan,
        .semantic_dependencies = request->semantic_dependencies,
        .semantic_dependency_count = request->semantic_dependency_count,
        .program_semantic_modules = program_semantic_module_count ? program_semantic_modules : NULL,
        .program_semantic_module_count = program_semantic_module_count,
        .target_profile = request->profile,
        .optimization_budget = request->optimization_budget,
    };
    XrCacheKey key = {{0}};
    if (request->cache_store && !xr_cache_xtp_key(&requirements, &key)) {
        set_error(error, error_size, "program TargetPlan cache authority failed");
        return false;
    }

    XrTargetPlan *plan = NULL;
    if (request->cache_store && !request->rebuild) {
        XrCacheXtpArtifactLoadContext load = {
            .requirements = requirements,
        };
        XrCacheBlob blob = {0};
        result->cache_load_attempted = true;
        result->load_status = xr_cache_store_load(request->cache_store, XR_CACHE_ARTIFACT_XTP, key,
                                                  xr_cache_materialize_xtp_artifact, &load, &blob);
        xr_cache_blob_release(&blob);
        if (cancellation_requested_at(request->cancellation,
                                      XR_PROGRAM_TARGET_PLAN_CANCEL_AFTER_LOAD))
            return cancel_build(result, load.accepted_plan, error, error_size);
        if (result->load_status == XR_CACHE_LOAD_HIT) {
            if (!load.accepted_plan) {
                set_error(error, error_size, "program TargetPlan cache hit lacks a verified plan");
                return false;
            }
            plan = load.accepted_plan;
            result->cache_hit = true;
        } else {
            xr_target_plan_free(load.accepted_plan);
            result->cache_candidate_rejected = rejected_load_status(result->load_status);
        }
    }

    if (!plan) {
        bool built =
            program_semantic_module_count
                ? xr_target_plan_build_program_graph(program_semantic_modules,
                                                     program_semantic_module_count,
                                                     request->profile, &plan, error, error_size)
                : xr_target_plan_build_module_set(request->semantic_plan,
                                                  request->semantic_dependencies,
                                                  request->semantic_dependency_count,
                                                  request->profile, &plan, error, error_size);
        if (!built)
            return false;
        result->built = true;
    }

    if (cancellation_requested_at(request->cancellation, XR_PROGRAM_TARGET_PLAN_CANCEL_AFTER_BUILD))
        return cancel_build(result, plan, error, error_size);

    if (request->cache_store && !result->cache_hit) {
        uint8_t *bytes = NULL;
        size_t size = 0;
        if (!xr_xtp_encode_plan(plan, &bytes, &size, error, error_size)) {
            xr_target_plan_free(plan);
            return false;
        }
        result->cache_publish_attempted = true;
        result->publish_status =
            xr_cache_store_publish(request->cache_store, XR_CACHE_ARTIFACT_XTP, key, bytes, size,
                                   xr_cache_verify_xtp_artifact, &requirements);
        xr_xtp_encoded_free(bytes);
        result->cache_published = result->publish_status == XR_CACHE_PUBLISH_OK;
        if (rejected_publish_status(result->publish_status)) {
            set_error(error, error_size,
                      "program TargetPlan cache publication failed with status %d",
                      (int) result->publish_status);
            xr_target_plan_free(plan);
            return false;
        }
    }

    if (cancellation_requested_at(request->cancellation,
                                  XR_PROGRAM_TARGET_PLAN_CANCEL_AFTER_PUBLISH))
        return cancel_build(result, plan, error, error_size);

    if (!plan || !xr_target_plan_is_verified(plan) || !xr_target_plan_fingerprint_is_intact(plan) ||
        !xr_target_plan_verify(plan, error, error_size)) {
        if (!error || !error_size || !error[0])
            set_error(error, error_size, "program TargetPlan build produced an invalid plan");
        xr_target_plan_free(plan);
        return false;
    }
    result->plan = plan;
    return true;
}

void xr_program_target_plan_build_result_release(XrProgramTargetPlanBuildResult *result) {
    if (!result)
        return;
    xr_target_plan_free(result->plan);
    memset(result, 0, sizeof(*result));
}
