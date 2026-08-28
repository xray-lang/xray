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

#include <stdbool.h>
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
#define XR_TARGET_PROVIDER_MASK_ALL                                                                \
    ((UINT64_C(1) << (uint8_t) XR_TARGET_PROVIDER_KIND_COUNT) - UINT64_C(2))

/* Target capabilities are semantic requirements, not aliases for provider
 * kinds.  The first two identities intentionally equal their provider kinds
 * because they are the scalar execution foundation.  Assertion requirements
 * have independent stable identities so IO reporting, typed-error capture,
 * and unwind-capable panic capture cannot be substituted for each other.
 * Grouped output has its own identity for the same reason: an assertion
 * report provider must not stand in for ordinary program output. */
typedef enum XrTargetCapabilityKind {
    XR_TARGET_CAPABILITY_INVALID = 0,
    XR_TARGET_CAPABILITY_ALLOCATOR = XR_TARGET_PROVIDER_ALLOCATOR,
    XR_TARGET_CAPABILITY_PANIC = XR_TARGET_PROVIDER_PANIC,
    XR_TARGET_CAPABILITY_ASSERTION_REPORT = 9,
    XR_TARGET_CAPABILITY_TYPED_ERROR_BOUNDARY = 10,
    XR_TARGET_CAPABILITY_PANIC_BOUNDARY = 11,
    XR_TARGET_CAPABILITY_OUTPUT_WRITE = 12,
    XR_TARGET_CAPABILITY_KIND_COUNT = 13,
} XrTargetCapabilityKind;

#define XR_TARGET_CAPABILITY_MASK(kind) (UINT64_C(1) << (uint8_t) (kind))
#define XR_TARGET_CAPABILITY_MASK_ALL                                                              \
    (XR_TARGET_PROVIDER_MASK_ALL |                                                                 \
     XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_ASSERTION_REPORT) |                            \
     XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_TYPED_ERROR_BOUNDARY) |                        \
     XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_PANIC_BOUNDARY) |                              \
     XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_OUTPUT_WRITE))

/* Every currently materialized scalar TargetPlan requires these runtime
 * services before an executor may allocate a frame or report a fatal fault. */
#define XR_TARGET_FOUNDATION_CAPABILITY_MASK                                                       \
    (XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_ALLOCATOR) |                                   \
     XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_PANIC))

/* These bits are derived from exact provider contracts.  Typed-error capture
 * is a language execution boundary and therefore appears only as an explicit
 * TargetPlan capability row, never as a fabricated runtime provider. */
#define XR_TARGET_PROVIDER_DERIVED_CAPABILITY_MASK                                                 \
    (XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_ASSERTION_REPORT) |                            \
     XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_PANIC_BOUNDARY))

static inline bool xr_target_capability_kind_valid(uint32_t capability) {
    return capability == XR_TARGET_CAPABILITY_ALLOCATOR ||
           capability == XR_TARGET_CAPABILITY_PANIC ||
           capability == XR_TARGET_CAPABILITY_ASSERTION_REPORT ||
           capability == XR_TARGET_CAPABILITY_TYPED_ERROR_BOUNDARY ||
           capability == XR_TARGET_CAPABILITY_PANIC_BOUNDARY ||
           capability == XR_TARGET_CAPABILITY_OUTPUT_WRITE;
}

static inline uint64_t xr_target_capability_mask(uint32_t capability) {
    return xr_target_capability_kind_valid(capability) ? XR_TARGET_CAPABILITY_MASK(capability)
                                                       : UINT64_C(0);
}

static inline uint16_t xr_target_capability_provider(uint32_t capability) {
    switch ((XrTargetCapabilityKind) capability) {
        case XR_TARGET_CAPABILITY_ALLOCATOR:
            return XR_TARGET_PROVIDER_ALLOCATOR;
        case XR_TARGET_CAPABILITY_PANIC:
        case XR_TARGET_CAPABILITY_PANIC_BOUNDARY:
            return XR_TARGET_PROVIDER_PANIC;
        case XR_TARGET_CAPABILITY_ASSERTION_REPORT:
        case XR_TARGET_CAPABILITY_OUTPUT_WRITE:
            return XR_TARGET_PROVIDER_IO;
        case XR_TARGET_CAPABILITY_TYPED_ERROR_BOUNDARY:
        default:
            return XR_TARGET_PROVIDER_INVALID;
    }
}

#endif  // XR_TARGET_RUNTIME_PROFILE_H
