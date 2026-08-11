/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_plan_load.c - Runtime-owned verified TargetPlan load boundary
 */

#include "../../include/xray_target_plan_load.h"
#include "abi/xr_runtime_target_authority.h"
#include "../plan/format/xr_artifact_kind.h"
#include "../plan/format/xr_xtp_internal.h"
#include "../plan/target/xr_target_profile_internal.h"
#include <stdio.h>

static bool fail(char *diagnostic, size_t diagnostic_size,
                 const char *code, const char *detail) {
    if (diagnostic && diagnostic_size)
        snprintf(diagnostic, diagnostic_size, "%s: %s", code, detail);
    return false;
}

static bool fail_nested(char *diagnostic, size_t diagnostic_size,
                        const char *code, const char *stage,
                        const char *nested) {
    char detail[640];
    snprintf(detail, sizeof(detail), "%s: %s", stage,
             nested && nested[0] ? nested : "unspecified rejection");
    return fail(diagnostic, diagnostic_size, code, detail);
}

static bool native_runtime_authority_matches(
    const XrTargetProfile *profile, char *diagnostic,
    size_t diagnostic_size) {
    char nested[512] = {0};
    if (!profile || !xr_target_profile_verify(profile, nested, sizeof(nested)))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "expected target profile is not verified");

    XrRuntimeTargetAuthority authority;
    if (xr_runtime_target_authority_native_hosted(&authority) !=
        XR_RUNTIME_ABI_OK)
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "native runtime target authority is unavailable");

    XrFingerprint runtime_fingerprint;
    XrFingerprint provider_fingerprint;
    XrFingerprint object_fingerprint;
    XrRuntimeObjectHeaderAbi object_header;
    uint64_t provider_mask = 0;
    if (xr_runtime_abi_contract_fingerprint(
            &authority.runtime_abi, &runtime_fingerprint) != XR_RUNTIME_ABI_OK ||
        xr_target_provider_set_fingerprint(
            authority.providers, authority.provider_count, &provider_mask,
            &provider_fingerprint) != XR_RUNTIME_ABI_OK ||
        xr_runtime_object_header_abi_materialize(
            &authority.object_header_materialization,
            &object_header) != XR_RUNTIME_ABI_OK ||
        xr_runtime_object_header_abi_fingerprint(
            &object_header, &object_fingerprint) != XR_RUNTIME_ABI_OK)
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "native runtime target authority is invalid");

    const XrTargetProfileDraft *facts = xr_target_profile_facts(profile);
    if (!facts || facts->provider_mask != provider_mask ||
        !xr_fingerprint_equal(facts->runtime_abi_fingerprint,
                              runtime_fingerprint) ||
        !xr_fingerprint_equal(facts->provider_set_fingerprint,
                              provider_fingerprint) ||
        !xr_fingerprint_equal(facts->object_header_fingerprint,
                              object_fingerprint))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "target profile does not match the native runtime ABI authority");
    return true;
}

XRAY_API bool xr_runtime_target_plan_load(
    const uint8_t *artifact_bytes, size_t artifact_size,
    const XrSemanticPlan *verified_semantic_plan,
    const XrTargetProfile *exact_target_profile,
    XrTargetPlan **verified_target_plan, char *diagnostic,
    size_t diagnostic_size) {
    if (verified_target_plan)
        *verified_target_plan = NULL;
    if (!verified_target_plan || !artifact_bytes || !artifact_size ||
        !verified_semantic_plan || !exact_target_profile)
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "verified semantic and target authorities are required");

    size_t prefix_size = artifact_size < XR_ARTIFACT_PROBE_SIZE
                             ? artifact_size
                             : XR_ARTIFACT_PROBE_SIZE;
    if (xr_artifact_classify(NULL, artifact_bytes, prefix_size) !=
        XR_ARTIFACT_KIND_XTP)
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2000",
                    "runtime TargetPlan loading accepts only XTP v2 bytes");
    if (!native_runtime_authority_matches(exact_target_profile, diagnostic,
                                          diagnostic_size))
        return false;

    XrXtpCandidate *candidate = NULL;
    char nested[512] = {0};
    if (!xr_xtp_decode_candidate(artifact_bytes, artifact_size, &candidate,
                                 nested, sizeof(nested)))
        return fail_nested(diagnostic, diagnostic_size, "XR_ARTIFACT_2000",
                           "XTP v2 candidate decoding failed", nested);

    XrXtpIdentity identity;
    bool have_identity = xr_xtp_candidate_identity(candidate, &identity);
    XrTargetPlan *plan = NULL;
    nested[0] = '\0';
    bool materialized =
        have_identity && xr_xtp_materialize_target_plan(
                             candidate, verified_semantic_plan,
                             exact_target_profile, &plan, nested,
                             sizeof(nested));
    xr_xtp_candidate_release(candidate);
    if (!materialized)
        return fail_nested(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                           "XTP v2 materialization failed", nested);

    nested[0] = '\0';
    const XrTargetProfile *loaded_profile = xr_target_plan_profile(plan);
    if (!xr_target_plan_is_verified(plan) ||
        xr_target_plan_completed_family_mask(plan) !=
            XR_TARGET_REQUIRED_FAMILIES ||
        !xr_fingerprint_equal(xr_target_plan_fingerprint(plan),
                              identity.plan_fingerprint) ||
        !xr_fingerprint_equal(xr_target_plan_semantic_fingerprint(plan),
                              identity.semantic_fingerprint) ||
        !xr_target_profile_require_exact(exact_target_profile, loaded_profile,
                                         nested, sizeof(nested))) {
        xr_target_plan_free(plan);
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "materialized TargetPlan did not preserve exact verified identity");
    }

    *verified_target_plan = plan;
    return true;
}
