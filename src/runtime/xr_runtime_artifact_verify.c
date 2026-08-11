/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_artifact_verify.c - Independent runtime authority verifier
 *
 * KEY CONCEPT:
 *   Verification re-derives every packaged identity from retained immutable
 *   plans and the current canonical runtime. It never trusts artifact rows to
 *   declare which providers are available.
 */

#include "xr_runtime_artifact_authority_internal.h"
#include "abi/xr_runtime_target_authority.h"
#include "../plan/semantic/xr_semantic_verify.h"
#include "../plan/target/xr_target_profile_internal.h"
#include "../plan/target/xr_target_verify.h"
#include <stdio.h>
#include <string.h>

static bool fail(char *diagnostic, size_t diagnostic_size, const char *code,
                 const char *detail) {
    if (diagnostic && diagnostic_size)
        snprintf(diagnostic, diagnostic_size, "%s: %s", code, detail);
    return false;
}

static bool bytes_equal(const uint8_t left[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE],
                        const uint8_t right[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE]) {
    return memcmp(left, right, XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE) == 0;
}

static bool fingerprint_equal_bytes(
    XrFingerprint fingerprint,
    const uint8_t bytes[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE]) {
    return memcmp(fingerprint.bytes, bytes,
                  XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE) == 0;
}

static bool current_runtime_identity(
    uint64_t *provider_mask, XrFingerprint *runtime_fingerprint,
    XrFingerprint *provider_fingerprint, XrFingerprint *object_fingerprint) {
    XrRuntimeTargetAuthority runtime;
    XrRuntimeObjectHeaderAbi object_header;
    return xr_runtime_target_authority_native_hosted(&runtime) ==
               XR_RUNTIME_ABI_OK &&
           xr_runtime_abi_contract_fingerprint(&runtime.runtime_abi,
                                               runtime_fingerprint) ==
               XR_RUNTIME_ABI_OK &&
           xr_target_provider_set_fingerprint(
               runtime.providers, runtime.provider_count, provider_mask,
               provider_fingerprint) == XR_RUNTIME_ABI_OK &&
           xr_runtime_object_header_abi_materialize(
               &runtime.object_header_materialization, &object_header) ==
               XR_RUNTIME_ABI_OK &&
           xr_runtime_object_header_abi_fingerprint(
               &object_header, object_fingerprint) == XR_RUNTIME_ABI_OK;
}

XRAY_API bool xr_runtime_artifact_authority_verify(
    const XrRuntimeArtifactAuthority *authority, char *diagnostic,
    size_t diagnostic_size) {
    if (!authority || !authority->semantic_plan || !authority->target_profile)
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "artifact authority package is incomplete");
    const XrRuntimeArtifactAuthorityIdentity *identity = &authority->identity;
    if (identity->schema_version !=
            XR_RUNTIME_ARTIFACT_AUTHORITY_SCHEMA_VERSION ||
        identity->reserved != 0)
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2000",
                    "artifact authority schema is not exactly supported");
    if (identity->required_family_mask != XR_TARGET_REQUIRED_FAMILIES)
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1001",
                    "artifact authority family closure is incomplete");
    if (identity->required_capability_mask !=
        XR_TARGET_FOUNDATION_CAPABILITY_MASK)
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1004",
                    "artifact authority capability closure is not exact");

    char nested[512] = {0};
    if (!xr_semantic_plan_verify(authority->semantic_plan, nested,
                                 sizeof(nested)) ||
        !xr_target_profile_verify(authority->target_profile, nested,
                                  sizeof(nested)))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "artifact authority retained an unverified input");
    if (!fingerprint_equal_bytes(
            xr_semantic_plan_fingerprint(authority->semantic_plan),
            identity->semantic_fingerprint) ||
        !fingerprint_equal_bytes(
            xr_semantic_plan_operation_registry_fingerprint(
                authority->semantic_plan),
            identity->operation_registry_fingerprint) ||
        !fingerprint_equal_bytes(
            xr_target_profile_fingerprint(authority->target_profile),
            identity->target_profile_fingerprint))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "artifact authority semantic or target fingerprint changed");

    uint64_t provider_mask = 0;
    XrFingerprint runtime_fingerprint;
    XrFingerprint provider_fingerprint;
    XrFingerprint object_fingerprint;
    if (!current_runtime_identity(&provider_mask, &runtime_fingerprint,
                                  &provider_fingerprint,
                                  &object_fingerprint))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "canonical native runtime authority is invalid");
    const XrTargetProfileDraft *facts =
        xr_target_profile_facts(authority->target_profile);
    if (!facts || identity->provider_mask != provider_mask ||
        facts->provider_mask != provider_mask ||
        (identity->required_capability_mask & ~provider_mask) != 0 ||
        !fingerprint_equal_bytes(runtime_fingerprint,
                                 identity->runtime_abi_fingerprint) ||
        !fingerprint_equal_bytes(provider_fingerprint,
                                 identity->provider_set_fingerprint) ||
        !fingerprint_equal_bytes(object_fingerprint,
                                 identity->object_header_fingerprint) ||
        !xr_fingerprint_equal(facts->runtime_abi_fingerprint,
                              runtime_fingerprint) ||
        !xr_fingerprint_equal(facts->provider_set_fingerprint,
                              provider_fingerprint) ||
        !xr_fingerprint_equal(facts->object_header_fingerprint,
                              object_fingerprint))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "artifact authority does not bind the exact runtime provider set");

    uint8_t actual[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE];
    xr_runtime_artifact_authority_compute_fingerprint(identity, actual);
    if (!bytes_equal(actual, identity->authority_fingerprint))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "artifact authority package fingerprint changed");
    return true;
}

XR_FUNCDEF bool xr_runtime_artifact_authority_bind_candidate(
    const XrRuntimeArtifactAuthority *authority, const XrXtpIdentity *identity,
    char *diagnostic, size_t diagnostic_size) {
    if (!identity || !xr_runtime_artifact_authority_verify(
                         authority, diagnostic, diagnostic_size))
        return false;
    const XrRuntimeArtifactAuthorityIdentity *expected = &authority->identity;
    if (identity->completed_family_mask != expected->required_family_mask)
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1001",
                    "artifact family closure does not match its authority");
    if (!fingerprint_equal_bytes(identity->semantic_fingerprint,
                                 expected->semantic_fingerprint) ||
        !fingerprint_equal_bytes(
            identity->operation_registry_fingerprint,
            expected->operation_registry_fingerprint) ||
        !fingerprint_equal_bytes(identity->profile_fingerprint,
                                 expected->target_profile_fingerprint) ||
        !fingerprint_equal_bytes(identity->runtime_fingerprint,
                                 expected->runtime_abi_fingerprint) ||
        !fingerprint_equal_bytes(identity->provider_fingerprint,
                                 expected->provider_set_fingerprint) ||
        !fingerprint_equal_bytes(identity->object_fingerprint,
                                 expected->object_header_fingerprint))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "artifact identity does not match its authority package");
    return true;
}

static bool plan_capability_mask(const XrTargetPlan *plan, uint64_t *out) {
    uint32_t count = 0;
    const XrTargetCapabilityRecord *capabilities =
        xr_target_plan_capabilities(plan, &count);
    uint64_t mask = 0;
    for (uint32_t i = 0; i < count; i++) {
        const XrTargetCapabilityRecord *record = &capabilities[i];
        if (record->id != i ||
            record->capability <= XR_TARGET_PROVIDER_INVALID ||
            record->capability >= XR_TARGET_PROVIDER_KIND_COUNT ||
            record->provider != record->capability ||
            record->flags != XR_TARGET_CAPABILITY_REQUIRED)
            return false;
        uint64_t bit = XR_TARGET_PROVIDER_MASK(record->provider);
        if ((mask & bit) != 0)
            return false;
        mask |= bit;
    }
    *out = mask;
    return true;
}

XR_FUNCDEF bool xr_runtime_artifact_authority_bind_plan(
    const XrRuntimeArtifactAuthority *authority, const XrTargetPlan *plan,
    char *diagnostic, size_t diagnostic_size) {
    if (!xr_runtime_artifact_authority_verify(authority, diagnostic,
                                              diagnostic_size))
        return false;
    char nested[512] = {0};
    if (!plan || !xr_target_plan_is_verified(plan) ||
        !xr_target_plan_verify(plan, nested, sizeof(nested)))
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "authority binding requires an independently verified TargetPlan");
    if (xr_target_plan_completed_family_mask(plan) !=
            authority->identity.required_family_mask ||
        !fingerprint_equal_bytes(xr_target_plan_semantic_fingerprint(plan),
                                 authority->identity.semantic_fingerprint) ||
        !xr_target_profile_require_exact(
            authority->target_profile, xr_target_plan_profile(plan), nested,
            sizeof(nested)))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "TargetPlan identity does not match its authority package");
    uint64_t capability_mask = 0;
    if (!plan_capability_mask(plan, &capability_mask) ||
        capability_mask != authority->identity.required_capability_mask ||
        (capability_mask & ~authority->identity.provider_mask) != 0)
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1004",
                    "TargetPlan capability closure is missing or unbound");
    return true;
}
