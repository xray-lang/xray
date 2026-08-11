/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_cache_artifact_verify.c - Plan-owned verification adapters for cache hits
 */

#include "xr_cache_artifact_verify.h"

#include "../plan/format/xr_xsm_schema.h"
#include "../plan/semantic/xr_semantic_plan.h"

bool xr_cache_verify_xsm_artifact(XrCacheArtifactKind kind, XrCacheKey key,
                                  const uint8_t *bytes, size_t size, void *context) {
    (void) key;
    (void) context;
    if (kind != XR_CACHE_ARTIFACT_XSM || (!bytes && size != 0))
        return false;
    XrSemanticPlan *plan = NULL;
    char error[256];
    if (!xr_xsm_decode(bytes, size, &plan, error, sizeof(error)))
        return false;
    xr_semantic_plan_free(plan);
    return true;
}
