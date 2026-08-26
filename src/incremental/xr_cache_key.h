/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_cache_key.h - Domain-separated incremental artifact cache keys
 *
 * KEY CONCEPT:
 *   Cache identities are derived exclusively from immutable content and
 *   contract fingerprints. Target-plan schemas remain owned by the plan
 *   layer; this boundary accepts only opaque fingerprints from that owner.
 */

#ifndef XR_CACHE_KEY_H
#define XR_CACHE_KEY_H

#include "../base/xdefs.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_CACHE_KEY_BYTES 32u
#define XR_CACHE_KEY_HEX_SIZE (XR_CACHE_KEY_BYTES * 2u + 1u)

typedef struct XrCacheFingerprint {
    uint8_t bytes[XR_CACHE_KEY_BYTES];
} XrCacheFingerprint;

typedef struct XrCacheKey {
    uint8_t bytes[XR_CACHE_KEY_BYTES];
} XrCacheKey;

typedef enum XrCacheArtifactKind {
    XR_CACHE_ARTIFACT_XSM = 1,
    XR_CACHE_ARTIFACT_XTP = 2,
} XrCacheArtifactKind;

typedef struct XrSemanticCacheKeyInput {
    XrCacheFingerprint normalized_source;
    XrCacheFingerprint program_semantic_closure;
    XrCacheFingerprint generation_closure;
    XrCacheFingerprint compiler;
    XrCacheFingerprint semantic_schema;
    XrCacheFingerprint contract;
    XrCacheFingerprint language_configuration;
    XrCacheFingerprint semantic_dependencies;
    XrCacheFingerprint declaration_identities;
} XrSemanticCacheKeyInput;

typedef struct XrTargetCacheKeyInput {
    XrCacheFingerprint program_semantic_closure;
    XrCacheFingerprint generation_closure;
    XrCacheFingerprint semantic_plan;
    XrCacheFingerprint target_profile;
    XrCacheFingerprint provider_capabilities;
    XrCacheFingerprint runtime_abi;
    XrCacheFingerprint planner_schema;
    XrCacheFingerprint optimization_budget;
} XrTargetCacheKeyInput;

XR_FUNC void xr_cache_fingerprint_bytes(const uint8_t *bytes, size_t size,
                                        XrCacheFingerprint *out);
XR_FUNC void xr_cache_key_semantic(const XrSemanticCacheKeyInput *input, XrCacheKey *out);
XR_FUNC void xr_cache_key_target(const XrTargetCacheKeyInput *input, XrCacheKey *out);
XR_FUNC bool xr_cache_key_equal(XrCacheKey left, XrCacheKey right);
XR_FUNC int xr_cache_key_compare(XrCacheKey left, XrCacheKey right);
XR_FUNC void xr_cache_key_hex(XrCacheKey key, char out[XR_CACHE_KEY_HEX_SIZE]);
XR_FUNC bool xr_cache_key_from_hex(const char *hex, XrCacheKey *out);

#endif  // XR_CACHE_KEY_H
