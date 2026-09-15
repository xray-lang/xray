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
 *   Stable contract IDs identify providers. Roles identify only special
 *   runtime services; semantic capabilities have an independent namespace.
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

typedef enum XrTargetProviderRole {
    XR_TARGET_PROVIDER_ROLE_INVALID = 0,
    XR_TARGET_PROVIDER_ROLE_ALLOCATOR = 1,
    XR_TARGET_PROVIDER_ROLE_PANIC = 2,
    XR_TARGET_PROVIDER_ROLE_OPERATIONS = 3,
    XR_TARGET_PROVIDER_ROLE_COUNT = 4,
} XrTargetProviderRole;

/* Capabilities are derived only from verified contract facts. No role or
 * service category implicitly grants a capability. Reporting an assertion,
 * writing program output, and capturing typed errors are distinct demands. */
typedef enum XrTargetCapabilityKind {
    XR_TARGET_CAPABILITY_INVALID = 0,
    XR_TARGET_CAPABILITY_ALLOCATOR = 1,
    XR_TARGET_CAPABILITY_PANIC = 2,
    XR_TARGET_CAPABILITY_ASSERTION_REPORT = 9,
    XR_TARGET_CAPABILITY_TYPED_ERROR_BOUNDARY = 10,
    XR_TARGET_CAPABILITY_PANIC_BOUNDARY = 11,
    XR_TARGET_CAPABILITY_OUTPUT_WRITE = 12,
    XR_TARGET_CAPABILITY_KIND_COUNT = 13,
} XrTargetCapabilityKind;

#define XR_TARGET_CAPABILITY_MASK(kind) (UINT64_C(1) << (uint8_t) (kind))
#define XR_TARGET_CAPABILITY_MASK_ALL                                                              \
    (XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_ALLOCATOR) |                                   \
     XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_PANIC) |                                       \
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
    (XR_TARGET_FOUNDATION_CAPABILITY_MASK |                                                        \
     XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_ASSERTION_REPORT) |                            \
     XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_PANIC_BOUNDARY) |                              \
     XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_OUTPUT_WRITE))

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

static inline uint16_t xr_target_capability_provider_role(uint32_t capability) {
    switch ((XrTargetCapabilityKind) capability) {
        case XR_TARGET_CAPABILITY_ALLOCATOR:
            return XR_TARGET_PROVIDER_ROLE_ALLOCATOR;
        case XR_TARGET_CAPABILITY_PANIC:
        case XR_TARGET_CAPABILITY_PANIC_BOUNDARY:
            return XR_TARGET_PROVIDER_ROLE_PANIC;
        case XR_TARGET_CAPABILITY_ASSERTION_REPORT:
        case XR_TARGET_CAPABILITY_OUTPUT_WRITE:
            return XR_TARGET_PROVIDER_ROLE_OPERATIONS;
        case XR_TARGET_CAPABILITY_TYPED_ERROR_BOUNDARY:
        default:
            return XR_TARGET_PROVIDER_ROLE_INVALID;
    }
}

#endif  // XR_TARGET_RUNTIME_PROFILE_H
