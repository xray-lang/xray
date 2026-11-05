/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_toolchain_binding.c - Canonical domain and length-framed identities
 */
#include "xr_toolchain_binding.h"
#include "../base/xsha256.h"
#include <string.h>

_Static_assert(offsetof(XrToolchainInput, provider_version) == 8,
    "The schema/provider header must occupy eight bytes");
_Static_assert(offsetof(XrToolchainBinding, provider_version_id) == 8,
    "The schema/provider header must occupy eight bytes");

static bool binding_header(const void *input) {
    struct { uint32_t schema; uint8_t provider, reserved[3]; } header;
    memcpy(&header, input, sizeof(header));
    return header.schema == XR_TOOLCHAIN_BINDING_SCHEMA_VERSION &&
        header.provider >= XR_TOOLCHAIN_BINDING_PROVIDER_CLANG && header.provider <= XR_TOOLCHAIN_BINDING_PROVIDER_ZIG &&
        !header.reserved[0] && !header.reserved[1] && !header.reserved[2];
}
static bool binding_nonzero(const uint8_t bytes[32]) {
    uint8_t bits = 0;
    for (unsigned i = 0; i < 32; ++i) bits |= bytes[i];
    return bits != 0;
}
static void binding_integer(XrSHA256Context *hash, uint32_t value) {
    uint8_t bytes[4];
    for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)(value >> (8u * i));
    xr_sha256_update(hash, bytes, sizeof(bytes));
}
static bool binding_text(const char *domain, size_t domain_length,
    const char *text, bool empty, XrFingerprint *output) {
    if (!text) return false;
    size_t length = 0;
    while (text[length]) {
        if (length == XR_TOOLCHAIN_TEXT_LIMIT) return false;
        ++length;
    }
    if (!length && !empty) return false;
    XrSHA256Context hash;
    xr_sha256_init(&hash);
    xr_sha256_update(&hash, (const uint8_t *)domain, domain_length);
    binding_integer(&hash, (uint32_t)length);
    xr_sha256_update(&hash, (const uint8_t *)text, length);
    xr_sha256_final(&hash, output->bytes);
    return true;
}
static void binding_id(const XrToolchainBinding *binding, XrFingerprint *out) {
    static const char domain[] = "xray:aot-toolchain:v2";
    XrSHA256Context hash;
    xr_sha256_init(&hash);
    xr_sha256_update(&hash, (const uint8_t *)domain, sizeof(domain) - 1);
    binding_integer(&hash, binding->schema_version);
    binding_integer(&hash, binding->provider);
    xr_sha256_update(&hash, binding->provider_version_id.bytes, 32);
    xr_sha256_update(&hash, binding->target_triple_id.bytes, 32);
    xr_sha256_update(&hash, binding->codegen_options_id.bytes, 32);
    xr_sha256_update(&hash, binding->sysroot_id.bytes, 32);
    xr_sha256_update(&hash, binding->runtime_sdk_id.bytes, 32);
    xr_sha256_update(&hash, binding->target_profile_id.bytes, 32);
    xr_sha256_final(&hash, out->bytes);
}
XR_FUNC XrToolchainBindingStatus xr_compile_toolchain_binding_build(
    const XrToolchainInput *input, XrToolchainBinding *output) {
    if (!input || !output) return XR_TOOLCHAIN_BINDING_BAD_ARGUMENT;
    if (!binding_header(input) || !binding_nonzero(input->sysroot_id.bytes) ||
        !binding_nonzero(input->runtime_sdk_id.bytes) || !binding_nonzero(input->target_profile_id.bytes))
        return XR_TOOLCHAIN_BINDING_BAD_STRUCTURE;
    XrToolchainBinding result;
    memset(&result, 0, sizeof(result));
    memcpy(&result, input, 8);
    result.sysroot_id = input->sysroot_id;
    result.runtime_sdk_id = input->runtime_sdk_id;
    result.target_profile_id = input->target_profile_id;
    static const char version[] = "xray:toolchain:provider-version:v2";
    static const char target[] = "xray:toolchain:target-triple:v2";
    static const char options[] = "xray:toolchain:codegen-options:v2";
    if (!binding_text(version, sizeof(version) - 1, input->provider_version, false, &result.provider_version_id) ||
        !binding_text(target, sizeof(target) - 1, input->target_triple, false, &result.target_triple_id) ||
        !binding_text(options, sizeof(options) - 1, input->codegen_options, true, &result.codegen_options_id))
        return XR_TOOLCHAIN_BINDING_BAD_STRUCTURE;
    binding_id(&result, &result.id);
    memcpy(output, &result, sizeof(result));
    return XR_TOOLCHAIN_BINDING_OK;
}
XR_FUNC XrToolchainBindingStatus xr_compile_toolchain_binding_validate(const XrToolchainBinding *binding) {
    if (!binding) return XR_TOOLCHAIN_BINDING_BAD_ARGUMENT;
    if (!binding_header(binding) || !binding_nonzero(binding->provider_version_id.bytes) ||
        !binding_nonzero(binding->target_triple_id.bytes) || !binding_nonzero(binding->codegen_options_id.bytes) ||
        !binding_nonzero(binding->sysroot_id.bytes) || !binding_nonzero(binding->runtime_sdk_id.bytes) ||
        !binding_nonzero(binding->target_profile_id.bytes)) return XR_TOOLCHAIN_BINDING_BAD_STRUCTURE;
    XrFingerprint expected;
    binding_id(binding, &expected);
    return memcmp(expected.bytes, binding->id.bytes, 32) ? XR_TOOLCHAIN_BINDING_BAD_STRUCTURE : XR_TOOLCHAIN_BINDING_OK;
}
XR_FUNC XrToolchainBindingStatus xr_compile_toolchain_binding_equal(
    const XrToolchainBinding *left, const XrToolchainBinding *right, bool *output) {
    if (!left || !right || !output) return XR_TOOLCHAIN_BINDING_BAD_ARGUMENT;
    XrToolchainBindingStatus status = xr_compile_toolchain_binding_validate(left);
    if (status != XR_TOOLCHAIN_BINDING_OK) return status;
    status = xr_compile_toolchain_binding_validate(right);
    if (status != XR_TOOLCHAIN_BINDING_OK) return status;
    *output = memcmp(left->id.bytes, right->id.bytes, 32) == 0;
    return XR_TOOLCHAIN_BINDING_OK;
}
