/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_toolchain_binding.h - Bounded, target-neutral toolchain identities
 */
#ifndef XR_TOOLCHAIN_BINDING_H
#define XR_TOOLCHAIN_BINDING_H
#include "../base/xcompile_resources.h"
#include "../base/xstable_id.h"

#define XR_TOOLCHAIN_BINDING_SCHEMA_VERSION 2u
#define XR_TOOLCHAIN_TEXT_LIMIT 4096u
typedef enum XrToolchainBindingProvider {
    XR_TOOLCHAIN_BINDING_PROVIDER_INVALID = 0,
    XR_TOOLCHAIN_BINDING_PROVIDER_CLANG = 1,
    XR_TOOLCHAIN_BINDING_PROVIDER_GCC = 2,
    XR_TOOLCHAIN_BINDING_PROVIDER_MSVC = 3,
    XR_TOOLCHAIN_BINDING_PROVIDER_ZIG = 4
} XrToolchainBindingProvider;
typedef struct XrToolchainInput {
    uint32_t schema_version;
    uint8_t provider, reserved8[3];
    const char *provider_version, *target_triple, *codegen_options;
    XrFingerprint sysroot_id, runtime_sdk_id, target_profile_id;
} XrToolchainInput;
typedef struct XrToolchainBinding {
    uint32_t schema_version;
    uint8_t provider, reserved8[3];
    XrFingerprint provider_version_id, target_triple_id, codegen_options_id;
    XrFingerprint sysroot_id, runtime_sdk_id, target_profile_id, id;
} XrToolchainBinding;
typedef enum XrToolchainBindingStatus {
    XR_TOOLCHAIN_BINDING_OK,
    XR_TOOLCHAIN_BINDING_BAD_ARGUMENT,
    XR_TOOLCHAIN_BINDING_BAD_STRUCTURE,
    XR_TOOLCHAIN_BINDING_BUDGET
} XrToolchainBindingStatus;

/* These fixed values allocate nothing and retain no borrowed text. Identity
 * validation does not confer SDK or target authority. All failures preserve
 * outputs, including padding. Inputs and outputs require caller exclusion.
 * Work: scanned/read bytes, copied/cleared bytes, compared bytes (two per pair),
 * encoded bytes, SHA input bytes, one per SHA init/final and 32 digest bytes.
 * Charging precedes each operation; rejected charges perform no operation.
 * Text reads include the terminating NUL, with a maximum of 4097 reads. */
XR_FUNC XrToolchainBindingStatus xr_compile_toolchain_binding_build(
    XrCompileResources *resources, const XrToolchainInput *input, XrToolchainBinding *output);
XR_FUNC XrToolchainBindingStatus xr_compile_toolchain_binding_validate(
    XrCompileResources *resources, const XrToolchainBinding *binding);
XR_FUNC XrToolchainBindingStatus xr_compile_toolchain_binding_equal(
    XrCompileResources *resources, const XrToolchainBinding *left,
    const XrToolchainBinding *right, bool *output);
#endif // XR_TOOLCHAIN_BINDING_H
