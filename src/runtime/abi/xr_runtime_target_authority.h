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
#include "xr_runtime_string_object.h"
#include "xr_target_machine_facts.h"

#define XR_RUNTIME_TARGET_AUTHORITY_PROVIDER_COUNT 2

/* Owned, pointer-free snapshot. The native runtime constructs this from its
 * concrete C layouts and canonical registries; consumers may copy the value
 * but must not synthesize or patch individual fingerprints. */
typedef struct XrRuntimeTargetAuthority {
    XrTargetMachineFacts machine;
    XrRuntimeObjectHeaderMaterializationFacts object_header_materialization;
    XrRuntimeStringObjectContract string_contract;
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

/* Exact comparison is field-wise so C padding can never become authority.
 * The current native authority deliberately supports only scalar execution;
 * a nonzero vector feature or width therefore never matches. */
XR_FUNC bool xr_runtime_target_authority_machine_matches(
    const XrRuntimeTargetAuthority *authority,
    const XrTargetMachineFacts *candidate);

#endif  // XR_RUNTIME_TARGET_AUTHORITY_H
