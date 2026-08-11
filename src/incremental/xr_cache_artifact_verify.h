/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_cache_artifact_verify.h - Plan-owned verification adapters for cache hits
 */

#ifndef XR_CACHE_ARTIFACT_VERIFY_H
#define XR_CACHE_ARTIFACT_VERIFY_H

#include "xr_cache_store.h"

typedef struct XrSemanticPlan XrSemanticPlan;
typedef struct XrTargetProfile XrTargetProfile;

typedef struct XrCacheXtpArtifactVerifyContext {
    const XrSemanticPlan *semantic_plan;
    const XrTargetProfile *target_profile;
    XrCacheFingerprint optimization_budget;
} XrCacheXtpArtifactVerifyContext;

/* The XSM adapter delegates every schema and semantic decision to the owning
 * decoder. Cache storage never reconstructs plan rows itself. */
XR_FUNC bool xr_cache_verify_xsm_artifact(XrCacheArtifactKind kind, XrCacheKey key,
                                          const uint8_t *bytes, size_t size, void *context);

/* The XTP key is derived from verified authorities plus the optimization
 * budget that selects the deterministic baseline plan. */
XR_FUNC bool xr_cache_xtp_key(const XrCacheXtpArtifactVerifyContext *context,
                              XrCacheKey *key);

/* A cache hit is accepted only after exact key reconstruction, owned XTP
 * decoding, typed materialization, and independent TargetPlan verification. */
XR_FUNC bool xr_cache_verify_xtp_artifact(XrCacheArtifactKind kind, XrCacheKey key,
                                          const uint8_t *bytes, size_t size, void *context);

#endif  // XR_CACHE_ARTIFACT_VERIFY_H
