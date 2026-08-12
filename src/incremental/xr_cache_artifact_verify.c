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

#include "../plan/format/xr_xtp_internal.h"
#include "../plan/format/xr_xsm_schema.h"
#include "../plan/semantic/xr_semantic_plan.h"
#include "../plan/semantic/xr_semantic_verify.h"
#include "../plan/target/xr_target_profile_internal.h"
#include "../plan/target/xr_target_verify.h"
#include <string.h>

#define XR_CACHE_XTP_SCHEMA_DESCRIPTOR_SIZE 24u

static void put_u32(uint8_t *bytes, uint32_t value) {
    for (size_t i = 0; i < 4; i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
}

static void put_u64(uint8_t *bytes, uint64_t value) {
    for (size_t i = 0; i < 8; i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
}

static void copy_fingerprint(XrFingerprint source, XrCacheFingerprint *target) {
    _Static_assert(sizeof(source.bytes) == sizeof(target->bytes),
                   "cache and plan fingerprints must have equal widths");
    memcpy(target->bytes, source.bytes, sizeof(target->bytes));
}

static bool verified_authorities(const XrCacheXtpArtifactVerifyContext *context,
                                 const XrTargetProfileDraft **profile_facts) {
    if (!context || !context->semantic_plan || !context->target_profile ||
        !profile_facts)
        return false;
    char error[512] = {0};
    if (!xr_semantic_plan_is_verified(context->semantic_plan) ||
        !xr_semantic_plan_verify(context->semantic_plan, error, sizeof(error)) ||
        !xr_target_profile_verify(context->target_profile, error, sizeof(error)))
        return false;
    *profile_facts = xr_target_profile_facts(context->target_profile);
    return *profile_facts != NULL;
}

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

bool xr_cache_xtp_key(const XrCacheXtpArtifactVerifyContext *context,
                      XrCacheKey *key) {
    if (key)
        memset(key, 0, sizeof(*key));
    const XrTargetProfileDraft *profile_facts = NULL;
    if (!key || !verified_authorities(context, &profile_facts))
        return false;

    XrTargetCacheKeyInput input = {0};
    copy_fingerprint(xr_semantic_plan_fingerprint(context->semantic_plan),
                     &input.semantic_plan);
    copy_fingerprint(xr_target_profile_fingerprint(context->target_profile),
                     &input.target_profile);
    copy_fingerprint(profile_facts->provider_set_fingerprint,
                     &input.provider_capabilities);
    copy_fingerprint(profile_facts->runtime_abi_fingerprint, &input.runtime_abi);

    uint8_t schema[XR_CACHE_XTP_SCHEMA_DESCRIPTOR_SIZE] = {0};
    put_u32(schema, xr_semantic_plan_schema(context->semantic_plan));
    put_u32(schema + 4, XR_TARGET_PROFILE_SCHEMA_VERSION);
    put_u32(schema + 8, XR_TARGET_PLAN_SCHEMA_VERSION);
    put_u32(schema + 12, XR_XTP_SCHEMA_VERSION);
    put_u64(schema + 16, XR_TARGET_REQUIRED_FAMILIES);
    xr_cache_fingerprint_bytes(schema, sizeof(schema), &input.planner_schema);
    input.optimization_budget = context->optimization_budget;
    xr_cache_key_target(&input, key);
    return true;
}

static bool materialize_verified_xtp(
    XrCacheArtifactKind kind, XrCacheKey key, const uint8_t *bytes, size_t size,
    const XrCacheXtpArtifactVerifyContext *requirements, XrTargetPlan **out) {
    if (out)
        *out = NULL;
    XrCacheKey expected = {{0}};
    if (!out || kind != XR_CACHE_ARTIFACT_XTP || (!bytes && size != 0) ||
        !xr_cache_xtp_key(requirements, &expected) ||
        !xr_cache_key_equal(key, expected))
        return false;

    XrXtpCandidate *candidate = NULL;
    char error[512] = {0};
    if (!xr_xtp_decode_candidate(bytes, size, &candidate, error, sizeof(error)))
        return false;
    XrTargetPlan *plan = NULL;
    bool materialized = xr_xtp_materialize_target_plan(
        candidate, requirements->semantic_plan, requirements->target_profile,
        &plan, error, sizeof(error));
    bool verified = materialized && plan && xr_target_plan_is_verified(plan) &&
                    xr_target_plan_fingerprint_is_intact(plan) &&
                    xr_target_plan_verify(plan, error, sizeof(error));
    xr_xtp_candidate_release(candidate);
    if (!verified) {
        xr_target_plan_free(plan);
        return false;
    }
    *out = plan;
    return true;
}

bool xr_cache_verify_xtp_artifact(XrCacheArtifactKind kind, XrCacheKey key,
                                  const uint8_t *bytes, size_t size, void *context) {
    XrTargetPlan *plan = NULL;
    bool verified = materialize_verified_xtp(
        kind, key, bytes, size,
        (const XrCacheXtpArtifactVerifyContext *) context, &plan);
    xr_target_plan_free(plan);
    return verified;
}

bool xr_cache_materialize_xtp_artifact(XrCacheArtifactKind kind, XrCacheKey key,
                                       const uint8_t *bytes, size_t size,
                                       void *context) {
    XrCacheXtpArtifactLoadContext *load =
        (XrCacheXtpArtifactLoadContext *) context;
    if (!load || load->accepted_plan)
        return false;
    return materialize_verified_xtp(kind, key, bytes, size,
                                    &load->requirements,
                                    &load->accepted_plan);
}
