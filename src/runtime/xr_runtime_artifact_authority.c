/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_artifact_authority.c - Immutable runtime artifact authority package
 */

#include "xr_runtime_artifact_authority_internal.h"
#include "abi/xr_runtime_target_authority.h"
#include "abi/xr_runtime_target_profile.h"
#include "../base/xmalloc.h"
#include "../base/xsha256.h"
#include "../plan/format/xr_xsm_schema.h"
#include "../plan/semantic/xr_semantic_verify.h"
#include "../plan/target/xr_target_profile_internal.h"
#include "../plan/target/xr_target_capability.h"
#include <stdio.h>
#include <string.h>

static bool fail(char *diagnostic, size_t diagnostic_size, const char *code,
                 const char *detail) {
    if (diagnostic && diagnostic_size)
        snprintf(diagnostic, diagnostic_size, "%s: %s", code, detail);
    return false;
}

static void hash_u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

XR_FUNCDEF void xr_runtime_artifact_authority_compute_fingerprint(
    const XrRuntimeArtifactAuthorityIdentity *identity,
    uint8_t out[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE]) {
    static const uint8_t domain[] = "xray-runtime-artifact-authority-v2\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_u64(&context, identity->schema_version);
    hash_u64(&context, identity->reserved);
    hash_u64(&context, identity->required_family_mask);
    hash_u64(&context, identity->required_capability_mask);
    hash_u64(&context, identity->provider_mask);
    xr_sha256_update(&context, identity->semantic_fingerprint,
                     XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE);
    xr_sha256_update(&context, identity->operation_registry_fingerprint,
                     XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE);
    xr_sha256_update(&context, identity->target_profile_fingerprint,
                     XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE);
    xr_sha256_update(&context, identity->runtime_abi_fingerprint,
                     XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE);
    xr_sha256_update(&context, identity->provider_set_fingerprint,
                     XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE);
    xr_sha256_update(&context, identity->object_header_fingerprint,
                     XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE);
    xr_sha256_final(&context, out);
}

static void copy_fingerprint(uint8_t out[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE],
                             XrFingerprint fingerprint) {
    memcpy(out, fingerprint.bytes, XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE);
}

static bool populate_identity(
    const XrSemanticPlan *semantic_plan, const XrTargetProfile *target_profile,
    XrRuntimeArtifactAuthorityIdentity *identity, char *diagnostic,
    size_t diagnostic_size) {
    char nested[512] = {0};
    if (!xr_semantic_plan_verify(semantic_plan, nested, sizeof(nested)))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "artifact authority requires a verified SemanticPlan");
    if (!xr_target_profile_verify(target_profile, nested, sizeof(nested)))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "artifact authority requires a verified TargetProfile");

    XrRuntimeTargetAuthority runtime;
    if (xr_runtime_target_authority_native_hosted(&runtime) != XR_RUNTIME_ABI_OK)
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "native runtime target authority is unavailable");
    XrFingerprint runtime_fingerprint;
    XrFingerprint provider_fingerprint;
    XrFingerprint object_fingerprint;
    XrRuntimeObjectHeaderAbi object_header;
    uint64_t provider_mask = 0;
    if (xr_runtime_abi_contract_fingerprint(&runtime.runtime_abi,
                                            &runtime_fingerprint) !=
            XR_RUNTIME_ABI_OK ||
        xr_target_provider_set_fingerprint(
            runtime.providers, runtime.provider_count, &provider_mask,
            &provider_fingerprint) != XR_RUNTIME_ABI_OK ||
        xr_runtime_object_header_abi_materialize(
            &runtime.object_header_materialization, &object_header) !=
            XR_RUNTIME_ABI_OK ||
        xr_runtime_object_header_abi_fingerprint(&object_header,
                                                 &object_fingerprint) !=
            XR_RUNTIME_ABI_OK)
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "native runtime target authority is invalid");

    const XrTargetProfileDraft *profile_facts =
        xr_target_profile_facts(target_profile);
    if (!profile_facts || !xr_runtime_target_authority_machine_matches(
                              &runtime, &profile_facts->machine))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "TargetProfile does not match the canonical native machine authority");
    if (profile_facts->provider_mask != provider_mask ||
        !xr_fingerprint_equal(profile_facts->runtime_abi_fingerprint,
                              runtime_fingerprint) ||
        !xr_fingerprint_equal(profile_facts->provider_set_fingerprint,
                              provider_fingerprint) ||
        !xr_fingerprint_equal(profile_facts->object_header_fingerprint,
                              object_fingerprint))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "TargetProfile does not match the native runtime authority");
    if ((provider_mask & XR_TARGET_FOUNDATION_CAPABILITY_MASK) !=
        XR_TARGET_FOUNDATION_CAPABILITY_MASK)
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1004",
                    "native runtime authority lacks a foundation capability");
    uint64_t required_capability_mask = 0;
    if (!xr_target_semantic_capability_mask(semantic_plan,
                                            profile_facts->machine.runtime_profile,
                                            &required_capability_mask) ||
        !xr_target_capability_mask_is_backed(required_capability_mask,
                                             provider_mask))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1004",
                    "semantic capability closure lacks an exact runtime provider");

    memset(identity, 0, sizeof(*identity));
    identity->schema_version = XR_RUNTIME_ARTIFACT_AUTHORITY_SCHEMA_VERSION;
    identity->required_family_mask = XR_TARGET_REQUIRED_FAMILIES;
    identity->required_capability_mask = required_capability_mask;
    identity->provider_mask = provider_mask;
    copy_fingerprint(identity->semantic_fingerprint,
                     xr_semantic_plan_fingerprint(semantic_plan));
    copy_fingerprint(identity->operation_registry_fingerprint,
                     xr_semantic_plan_operation_registry_fingerprint(
                         semantic_plan));
    copy_fingerprint(identity->target_profile_fingerprint,
                     xr_target_profile_fingerprint(target_profile));
    copy_fingerprint(identity->runtime_abi_fingerprint, runtime_fingerprint);
    copy_fingerprint(identity->provider_set_fingerprint, provider_fingerprint);
    copy_fingerprint(identity->object_header_fingerprint, object_fingerprint);
    xr_runtime_artifact_authority_compute_fingerprint(
        identity, identity->authority_fingerprint);
    return true;
}

XRAY_API bool xr_runtime_artifact_authority_load_available(void) {
    return true;
}

XRAY_API bool xr_runtime_artifact_authority_load_xsm(
    const uint8_t *artifact_bytes, size_t artifact_size,
    XrRuntimeArtifactAuthority **authority, char *diagnostic,
    size_t diagnostic_size) {
    if (authority)
        *authority = NULL;
    if (!authority || !artifact_bytes || artifact_size == 0)
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "exact XSM bytes and an authority output are required");

    XrSemanticPlan *semantic_plan = NULL;
    if (!xr_xsm_decode(artifact_bytes, artifact_size, &semantic_plan,
                       diagnostic, diagnostic_size))
        return false;
    bool created = xr_runtime_artifact_authority_create_internal(
        semantic_plan, authority, diagnostic, diagnostic_size);
    xr_semantic_plan_free(semantic_plan);
    return created;
}

XR_FUNCDEF bool xr_runtime_artifact_authority_create_internal(
    const XrSemanticPlan *verified_semantic_plan,
    XrRuntimeArtifactAuthority **authority, char *diagnostic,
    size_t diagnostic_size) {
    if (authority)
        *authority = NULL;
    if (!authority || !verified_semantic_plan)
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "verified semantic authority is required");

    XrTargetProfile *native_profile = NULL;
    if (!xr_runtime_target_profile_build_native_hosted(
            &native_profile, diagnostic, diagnostic_size))
        return false;

    XrRuntimeArtifactAuthorityIdentity identity;
    if (!populate_identity(verified_semantic_plan, native_profile, &identity,
                           diagnostic, diagnostic_size)) {
        xr_target_profile_free(native_profile);
        return false;
    }
    XrRuntimeArtifactAuthority *created =
        (XrRuntimeArtifactAuthority *) xr_calloc(1, sizeof(*created));
    if (!created) {
        xr_target_profile_free(native_profile);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "artifact authority allocation failed");
    }
    created->semantic_plan =
        xr_semantic_plan_retain((XrSemanticPlan *) verified_semantic_plan);
    created->target_profile = native_profile;
    created->identity = identity;
    if (!created->semantic_plan || !created->target_profile ||
        !xr_runtime_artifact_authority_verify(created, diagnostic,
                                              diagnostic_size)) {
        xr_runtime_artifact_authority_free(created);
        return false;
    }
    *authority = created;
    return true;
}

XRAY_API bool xr_runtime_artifact_authority_identity(
    const XrRuntimeArtifactAuthority *authority,
    XrRuntimeArtifactAuthorityIdentity *identity) {
    if (!identity)
        return false;
    memset(identity, 0, sizeof(*identity));
    if (!xr_runtime_artifact_authority_verify(authority, NULL, 0))
        return false;
    *identity = authority->identity;
    return true;
}

XRAY_API void xr_runtime_artifact_authority_free(
    XrRuntimeArtifactAuthority *authority) {
    if (!authority)
        return;
    xr_semantic_plan_free(authority->semantic_plan);
    xr_target_profile_free(authority->target_profile);
    memset(authority, 0, sizeof(*authority));
    xr_free(authority);
}
