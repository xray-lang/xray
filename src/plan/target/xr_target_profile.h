/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_profile.h - Immutable backend-neutral target facts
 *
 * KEY CONCEPT:
 *   A target profile is an exact, immutable value. It contains numeric ABI,
 *   runtime, and provider identities but no backend spelling or compiler
 *   object, so every executor can compare the same fingerprint.
 */

#ifndef XR_TARGET_PROFILE_H
#define XR_TARGET_PROFILE_H

#include "../semantic/xr_semantic_ids.h"
#include "../../runtime/abi/xr_runtime_contract.h"
#include "../../runtime/abi/xr_runtime_object_header.h"
#include "../../runtime/abi/xr_runtime_string_object.h"
#include "../../runtime/abi/xr_target_machine_facts.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_TARGET_PROFILE_SCHEMA_VERSION UINT32_C(2)
#define XR_TARGET_PLAN_SCHEMA_VERSION UINT32_C(34)

typedef struct XrTargetProfile XrTargetProfile;

/* Numeric codegen selection after backend option resolution. This is not a
 * target identity; the production authority validates it against one. */
typedef struct XrTargetCodegenFacts {
    uint64_t vector_feature_mask;
    uint16_t maximum_vector_bits;
    uint16_t reserved16;
    uint32_t reserved32;
} XrTargetCodegenFacts;

/* Structured inputs are consumed during the call and are never retained. */
typedef struct XrTargetProfileBuildInput {
    XrTargetMachineFacts machine;
    const XrRuntimeAbiContract *runtime_abi;
    const XrRuntimeObjectHeaderMaterializationFacts *object_header_materialization;
    const XrRuntimeStringObjectContract *string_contract;
    const XrTargetProviderContract *providers;
    size_t provider_count;
} XrTargetProfileBuildInput;

XR_FUNC bool xr_target_profile_build(const XrTargetProfileBuildInput *input,
                                     XrTargetProfile **out, char *error,
                                     size_t error_size);
XR_FUNC bool xr_target_profile_require_exact(const XrTargetProfile *expected,
                                             const XrTargetProfile *actual,
                                             char *error, size_t error_size);
XR_FUNC XrTargetProfile *xr_target_profile_retain(XrTargetProfile *profile);
XR_FUNC void xr_target_profile_free(XrTargetProfile *profile);
XR_FUNC bool xr_target_profile_is_frozen(const XrTargetProfile *profile);
XR_FUNC bool xr_target_profile_verify(const XrTargetProfile *profile, char *error,
                                      size_t error_size);
XR_FUNC XrFingerprint xr_target_profile_fingerprint(const XrTargetProfile *profile);
XR_FUNC const XrTargetMachineFacts *xr_target_profile_machine_facts(
    const XrTargetProfile *profile);

#endif  // XR_TARGET_PROFILE_H
