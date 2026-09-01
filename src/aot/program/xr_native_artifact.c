/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_native_artifact.c - Exact AOT toolchain and native-byte identity
 */

#include "xr_backend_ir.h"

#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"

#include <string.h>

static void hash_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4];
    for (size_t index = 0; index < sizeof(bytes); ++index)
        bytes[index] = (uint8_t) (value >> (index * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static XrFingerprint text_fingerprint(const char *domain, const char *text) {
    XrSHA256Context context;
    XrFingerprint fingerprint;
    xr_sha256_init(&context);
    xr_sha256_update(&context, (const uint8_t *) domain, strlen(domain));
    xr_sha256_update(&context, (const uint8_t *) text, strlen(text));
    xr_sha256_final(&context, fingerprint.bytes);
    return fingerprint;
}

static bool fingerprint_is_zero(XrFingerprint fingerprint) {
    uint8_t combined = 0u;
    for (size_t index = 0; index < sizeof(fingerprint.bytes); ++index)
        combined |= fingerprint.bytes[index];
    return combined == 0u;
}

static XrToolchainId toolchain_binding_id(const XrAotToolchainBinding *binding) {
    XrSHA256Context context;
    XrToolchainId id;
    xr_sha256_init(&context);
    xr_sha256_update(&context, (const uint8_t *) "xray:aot-toolchain:v1",
                     strlen("xray:aot-toolchain:v1"));
    hash_u32(&context, binding->schema_version);
    hash_u32(&context, binding->provider);
    xr_sha256_update(&context, binding->provider_version_id.bytes,
                     sizeof(binding->provider_version_id.bytes));
    xr_sha256_update(&context, binding->target_triple_id.bytes,
                     sizeof(binding->target_triple_id.bytes));
    xr_sha256_update(&context, binding->codegen_options_id.bytes,
                     sizeof(binding->codegen_options_id.bytes));
    xr_sha256_update(&context, binding->sysroot_id.bytes, sizeof(binding->sysroot_id.bytes));
    xr_sha256_update(&context, binding->runtime_objects_id.bytes,
                     sizeof(binding->runtime_objects_id.bytes));
    xr_sha256_update(&context, binding->target_profile_id.bytes,
                     sizeof(binding->target_profile_id.bytes));
    xr_sha256_final(&context, id.bytes);
    return id;
}

static bool toolchain_binding_valid(const XrAotToolchainBinding *binding) {
    if (!binding || binding->schema_version != XR_AOT_TOOLCHAIN_SCHEMA_VERSION ||
        binding->provider < XR_AOT_TOOLCHAIN_CLANG || binding->provider > XR_AOT_TOOLCHAIN_ZIG ||
        fingerprint_is_zero(binding->provider_version_id) ||
        fingerprint_is_zero(binding->target_triple_id) ||
        fingerprint_is_zero(binding->codegen_options_id) ||
        fingerprint_is_zero(binding->sysroot_id) ||
        fingerprint_is_zero(binding->runtime_objects_id) ||
        fingerprint_is_zero(binding->target_profile_id))
        return false;
    return xr_fingerprint_equal(toolchain_binding_id(binding), binding->id);
}

static XrNativeArtifactId native_artifact_id(const XrNativeArtifact *artifact) {
    XrSHA256Context context;
    XrNativeArtifactId id;
    xr_sha256_init(&context);
    xr_sha256_update(&context, (const uint8_t *) "xray:native-artifact:v1",
                     strlen("xray:native-artifact:v1"));
    xr_sha256_update(&context, artifact->execution_id.bytes, sizeof(artifact->execution_id.bytes));
    xr_sha256_update(&context, artifact->backend_id.bytes, sizeof(artifact->backend_id.bytes));
    xr_sha256_update(&context, artifact->toolchain_id.bytes, sizeof(artifact->toolchain_id.bytes));
    xr_sha256_update(&context, artifact->optimization_policy_id.bytes,
                     sizeof(artifact->optimization_policy_id.bytes));
    xr_sha256_update(&context, artifact->bytes, artifact->size);
    xr_sha256_final(&context, id.bytes);
    return id;
}

bool xr_aot_toolchain_binding_build(const XrAotToolchainInput *input,
                                    XrAotToolchainBinding *binding_out) {
    if (binding_out)
        memset(binding_out, 0, sizeof(*binding_out));
    if (!input || !binding_out || input->schema_version != XR_AOT_TOOLCHAIN_SCHEMA_VERSION ||
        input->provider < XR_AOT_TOOLCHAIN_CLANG || input->provider > XR_AOT_TOOLCHAIN_ZIG ||
        !input->provider_version || input->provider_version[0] == '\0' || !input->target_triple ||
        input->target_triple[0] == '\0' || !input->codegen_options ||
        fingerprint_is_zero(input->sysroot_id) || fingerprint_is_zero(input->runtime_objects_id) ||
        fingerprint_is_zero(input->target_profile_id))
        return false;
    *binding_out = (XrAotToolchainBinding) {
        .schema_version = input->schema_version,
        .provider = input->provider,
        .provider_version_id =
            text_fingerprint("xray:toolchain:provider-version:v1", input->provider_version),
        .target_triple_id =
            text_fingerprint("xray:toolchain:target-triple:v1", input->target_triple),
        .codegen_options_id =
            text_fingerprint("xray:toolchain:codegen-options:v1", input->codegen_options),
        .sysroot_id = input->sysroot_id,
        .runtime_objects_id = input->runtime_objects_id,
        .target_profile_id = input->target_profile_id,
    };
    binding_out->id = toolchain_binding_id(binding_out);
    return true;
}

bool xr_aot_toolchain_binding_equal(const XrAotToolchainBinding *left,
                                    const XrAotToolchainBinding *right) {
    return toolchain_binding_valid(left) && toolchain_binding_valid(right) &&
           left->schema_version == right->schema_version && left->provider == right->provider &&
           xr_fingerprint_equal(left->provider_version_id, right->provider_version_id) &&
           xr_fingerprint_equal(left->target_triple_id, right->target_triple_id) &&
           xr_fingerprint_equal(left->codegen_options_id, right->codegen_options_id) &&
           xr_fingerprint_equal(left->sysroot_id, right->sysroot_id) &&
           xr_fingerprint_equal(left->runtime_objects_id, right->runtime_objects_id) &&
           xr_fingerprint_equal(left->target_profile_id, right->target_profile_id) &&
           xr_fingerprint_equal(left->id, right->id);
}

XrBackendStatus xr_native_artifact_seal(const XrGeneratedC *generated,
                                        const XrAotToolchainBinding *toolchain,
                                        const uint8_t *native_bytes, size_t native_size,
                                        XrNativeArtifact *artifact_out) {
    if (artifact_out)
        memset(artifact_out, 0, sizeof(*artifact_out));
    if (!generated || !generated->bytes || generated->size == 0u || !toolchain || !native_bytes ||
        native_size == 0u || !artifact_out || !toolchain_binding_valid(toolchain) ||
        !xr_fingerprint_equal(generated->target_profile_id, toolchain->target_profile_id))
        return XR_BACKEND_TOOLCHAIN_REJECTED;
    uint8_t *copy = xr_malloc(native_size);
    if (!copy)
        return XR_BACKEND_OUT_OF_MEMORY;
    memcpy(copy, native_bytes, native_size);
    artifact_out->bytes = copy;
    artifact_out->size = native_size;
    artifact_out->schema_version = XR_NATIVE_ARTIFACT_SCHEMA_VERSION;
    artifact_out->execution_id = generated->execution_id;
    artifact_out->backend_id = generated->backend_id;
    artifact_out->toolchain_id = toolchain->id;
    artifact_out->toolchain_binding = *toolchain;
    artifact_out->optimization_policy_id = generated->optimization_policy_id;
    artifact_out->target_profile_id = generated->target_profile_id;
    xr_semantic_fingerprint(copy, native_size, &artifact_out->native_digest);
    artifact_out->id = native_artifact_id(artifact_out);
    return XR_BACKEND_OK;
}

bool xr_native_artifact_verify(const XrNativeArtifact *artifact, XrExecutionId execution_id,
                               XrBackendId backend_id,
                               XrOptimizationPolicyId optimization_policy_id,
                               const XrAotToolchainBinding *toolchain) {
    if (!artifact || !artifact->bytes || artifact->size == 0u || !toolchain ||
        artifact->schema_version != XR_NATIVE_ARTIFACT_SCHEMA_VERSION ||
        !xr_fingerprint_equal(artifact->execution_id, execution_id) ||
        !xr_fingerprint_equal(artifact->backend_id, backend_id) ||
        !xr_fingerprint_equal(artifact->optimization_policy_id, optimization_policy_id) ||
        !xr_fingerprint_equal(artifact->toolchain_id, toolchain->id) ||
        !xr_aot_toolchain_binding_equal(&artifact->toolchain_binding, toolchain) ||
        !xr_fingerprint_equal(artifact->target_profile_id, toolchain->target_profile_id))
        return false;
    XrFingerprint digest;
    xr_semantic_fingerprint(artifact->bytes, artifact->size, &digest);
    if (!xr_fingerprint_equal(digest, artifact->native_digest))
        return false;
    XrNativeArtifactId expected = native_artifact_id(artifact);
    return xr_fingerprint_equal(expected, artifact->id);
}

void xr_native_artifact_free(XrNativeArtifact *artifact) {
    if (!artifact)
        return;
    xr_free(artifact->bytes);
    memset(artifact, 0, sizeof(*artifact));
}
