/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_target_authority.h - Native hosted runtime ABI authority
 */

#ifndef XR_RUNTIME_TARGET_AUTHORITY_H
#define XR_RUNTIME_TARGET_AUTHORITY_H

#include "xr_runtime_contract.h"
#include "xr_runtime_object_header.h"

#define XR_RUNTIME_TARGET_AUTHORITY_PROVIDER_COUNT 2

/* Owned, pointer-free snapshot. The native runtime constructs this from its
 * concrete C layouts and canonical registries; consumers may copy the value
 * but must not synthesize or patch individual fingerprints. */
typedef struct XrRuntimeTargetAuthority {
    XrRuntimeObjectHeaderMaterializationFacts object_header_materialization;
    XrRuntimeAbiContract runtime_abi;
    XrTargetProviderContract
        providers[XR_RUNTIME_TARGET_AUTHORITY_PROVIDER_COUNT];
    size_t provider_count;
} XrRuntimeTargetAuthority;

/* The current production owner is the native hosted runtime. Cross-target and
 * freestanding authorities require independently validated manifests and are
 * deliberately not inferred from the host. */
XR_FUNC XrRuntimeAbiStatus xr_runtime_target_authority_native_hosted(
    XrRuntimeTargetAuthority *out);

#endif  // XR_RUNTIME_TARGET_AUTHORITY_H
