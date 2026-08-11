/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_runtime_profile.h - Canonical target runtime/provider identities
 *
 * KEY CONCEPT:
 *   Runtime profiles and provider kinds have one numeric namespace shared by
 *   planning, runtime ABI validation, artifacts, and provider registries.
 *   Consumers use these values directly; compatibility remapping is invalid.
 */

#ifndef XR_TARGET_RUNTIME_PROFILE_H
#define XR_TARGET_RUNTIME_PROFILE_H

#include <stdint.h>

typedef enum XrTargetRuntimeProfile {
    XR_TARGET_RUNTIME_PROFILE_INVALID = 0,
    XR_TARGET_RUNTIME_PROFILE_HOSTED = 1,
    XR_TARGET_RUNTIME_PROFILE_FREESTANDING = 2,
} XrTargetRuntimeProfile;

typedef enum XrTargetProviderKind {
    XR_TARGET_PROVIDER_INVALID = 0,
    XR_TARGET_PROVIDER_ALLOCATOR = 1,
    XR_TARGET_PROVIDER_PANIC = 2,
    XR_TARGET_PROVIDER_CLOCK = 3,
    XR_TARGET_PROVIDER_RANDOM = 4,
    XR_TARGET_PROVIDER_SCHEDULER = 5,
    XR_TARGET_PROVIDER_IO = 6,
    XR_TARGET_PROVIDER_TLS = 7,
    XR_TARGET_PROVIDER_FFI = 8,
    XR_TARGET_PROVIDER_KIND_COUNT = 9,
} XrTargetProviderKind;

#define XR_TARGET_PROVIDER_MASK(kind) (UINT64_C(1) << (uint8_t) (kind))
#define XR_TARGET_PROVIDER_MASK_ALL                                                     \
    ((UINT64_C(1) << (uint8_t) XR_TARGET_PROVIDER_KIND_COUNT) - UINT64_C(2))

#endif  // XR_TARGET_RUNTIME_PROFILE_H
