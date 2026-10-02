/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_toolchain_binding.c - Canonical domain and length-framed identities
 */
#include "xr_toolchain_binding.h"
#include "xr_identity_work.h"

_Static_assert(offsetof(XrToolchainInput, provider_version) == 8,
    "The schema/provider header must occupy eight bytes");
_Static_assert(offsetof(XrToolchainBinding, provider_version_id) == 8,
    "The schema/provider header must occupy eight bytes");

static XrToolchainBindingStatus binding_failure(const XrIdentityWork *work) {
    return work->failed ? XR_TOOLCHAIN_BINDING_BUDGET : XR_TOOLCHAIN_BINDING_BAD_STRUCTURE;
}
static bool binding_header(XrIdentityWork *work, const void *input) {
    struct { uint32_t schema; uint8_t provider, reserved[3]; } header;
    if (!identity_copy(work, &header, input, sizeof(header))) return false;
    return header.schema == XR_TOOLCHAIN_BINDING_SCHEMA_VERSION &&
        header.provider >= XR_TOOLCHAIN_PROVIDER_CLANG && header.provider <= XR_TOOLCHAIN_PROVIDER_ZIG &&
        !header.reserved[0] && !header.reserved[1] && !header.reserved[2];
}
static bool binding_text(XrIdentityWork *work, const char *domain, size_t domain_length,
    const char *text, bool empty, XrFingerprint *output) {
    if (!text) return false;
    size_t length = 0;
    for (;;) {
        if (!identity_work(work, 1)) return false;
        if (!text[length]) break;
        if (length == XR_TOOLCHAIN_TEXT_LIMIT) return false;
        ++length;
    }
    if (!length && !empty) return false;
    XrSHA256Context hash;
    return identity_begin(work, &hash) && identity_bytes(work, &hash, domain, domain_length) &&
        identity_integer(work, &hash, length, 4) && identity_bytes(work, &hash, text, length) &&
        identity_end(work, &hash, output->bytes);
}
static bool binding_id(XrIdentityWork *work, const XrToolchainBinding *binding, XrFingerprint *out) {
    static const char domain[] = "xray:aot-toolchain:v2";
    XrSHA256Context hash;
    return identity_begin(work, &hash) && identity_bytes(work, &hash, domain, sizeof(domain) - 1) &&
        identity_integer(work, &hash, binding->schema_version, 4) &&
        identity_integer(work, &hash, binding->provider, 4) &&
        identity_bytes(work, &hash, binding->provider_version_id.bytes, 32) &&
        identity_bytes(work, &hash, binding->target_triple_id.bytes, 32) &&
        identity_bytes(work, &hash, binding->codegen_options_id.bytes, 32) &&
        identity_bytes(work, &hash, binding->sysroot_id.bytes, 32) &&
        identity_bytes(work, &hash, binding->runtime_sdk_id.bytes, 32) &&
        identity_bytes(work, &hash, binding->target_profile_id.bytes, 32) &&
        identity_end(work, &hash, out->bytes);
}
XR_FUNC XrToolchainBindingStatus xr_compile_toolchain_binding_build(
    XrCompileResources *resources, const XrToolchainInput *input, XrToolchainBinding *output) {
    if (!resources || !input || !output) return XR_TOOLCHAIN_BINDING_BAD_ARGUMENT;
    XrIdentityWork work = {resources, false};
    if (!binding_header(&work, input) || !identity_nonzero(&work, input->sysroot_id.bytes) ||
        !identity_nonzero(&work, input->runtime_sdk_id.bytes) ||
        !identity_nonzero(&work, input->target_profile_id.bytes)) return binding_failure(&work);
    XrToolchainBinding result;
    if (!identity_zero(&work, &result, sizeof(result)) ||
        !identity_copy(&work, &result, input, 8) ||
        !identity_copy(&work, &result.sysroot_id, &input->sysroot_id, sizeof(XrFingerprint)) ||
        !identity_copy(&work, &result.runtime_sdk_id, &input->runtime_sdk_id, sizeof(XrFingerprint)) ||
        !identity_copy(&work, &result.target_profile_id, &input->target_profile_id, sizeof(XrFingerprint)))
        return binding_failure(&work);
    static const char version[] = "xray:toolchain:provider-version:v2";
    static const char target[] = "xray:toolchain:target-triple:v2";
    static const char options[] = "xray:toolchain:codegen-options:v2";
    if (!binding_text(&work, version, sizeof(version) - 1, input->provider_version, false, &result.provider_version_id) ||
        !binding_text(&work, target, sizeof(target) - 1, input->target_triple, false, &result.target_triple_id) ||
        !binding_text(&work, options, sizeof(options) - 1, input->codegen_options, true, &result.codegen_options_id) ||
        !binding_id(&work, &result, &result.id) ||
        !identity_copy(&work, output, &result, sizeof(result))) return binding_failure(&work);
    return XR_TOOLCHAIN_BINDING_OK;
}
XR_FUNC XrToolchainBindingStatus xr_compile_toolchain_binding_validate(
    XrCompileResources *resources, const XrToolchainBinding *binding) {
    if (!resources || !binding) return XR_TOOLCHAIN_BINDING_BAD_ARGUMENT;
    XrIdentityWork work = {resources, false};
    XrFingerprint expected;
    if (!binding_header(&work, binding) ||
        !identity_nonzero(&work, binding->provider_version_id.bytes) ||
        !identity_nonzero(&work, binding->target_triple_id.bytes) ||
        !identity_nonzero(&work, binding->codegen_options_id.bytes) ||
        !identity_nonzero(&work, binding->sysroot_id.bytes) ||
        !identity_nonzero(&work, binding->runtime_sdk_id.bytes) ||
        !identity_nonzero(&work, binding->target_profile_id.bytes) ||
        !binding_id(&work, binding, &expected) || !identity_same(&work, expected.bytes, binding->id.bytes))
        return binding_failure(&work);
    return XR_TOOLCHAIN_BINDING_OK;
}
XR_FUNC XrToolchainBindingStatus xr_compile_toolchain_binding_equal(
    XrCompileResources *resources, const XrToolchainBinding *left,
    const XrToolchainBinding *right, bool *output) {
    if (!resources || !left || !right || !output) return XR_TOOLCHAIN_BINDING_BAD_ARGUMENT;
    XrToolchainBindingStatus status = xr_compile_toolchain_binding_validate(resources, left);
    if (status != XR_TOOLCHAIN_BINDING_OK) return status;
    status = xr_compile_toolchain_binding_validate(resources, right);
    if (status != XR_TOOLCHAIN_BINDING_OK) return status;
    XrIdentityWork work = {resources, false};
    bool same = identity_same(&work, left->id.bytes, right->id.bytes);
    if (work.failed || !identity_copy(&work, output, &same, sizeof(same))) return binding_failure(&work);
    return XR_TOOLCHAIN_BINDING_OK;
}
