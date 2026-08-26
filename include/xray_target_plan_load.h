/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_target_plan_load.h - Verified TargetPlan artifact load boundary
 *
 * KEY CONCEPT:
 *   An authority package binds immutable semantic and target identities to
 *   the exact runtime/provider capability closure. Loading returns a verified
 *   TargetPlan but performs no provider registration, activation, or entry
 *   execution.
 */

#ifndef XRAY_TARGET_PLAN_LOAD_H
#define XRAY_TARGET_PLAN_LOAD_H

#include "xray_export.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XrSemanticPlan XrSemanticPlan;
typedef struct XrTargetProfile XrTargetProfile;
typedef struct XrTargetPlan XrTargetPlan;
typedef struct XrRuntimeArtifactAuthority XrRuntimeArtifactAuthority;

#define XR_RUNTIME_ARTIFACT_AUTHORITY_SCHEMA_VERSION UINT32_C(3)
#define XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE 32u
#define XR_RUNTIME_ARTIFACT_GENERATION_CLOSURE_ID_SIZE 16u

typedef enum XrRuntimeArtifactAuthorityKind {
    XR_RUNTIME_ARTIFACT_AUTHORITY_ORDINARY_MODULE = 0,
    XR_RUNTIME_ARTIFACT_AUTHORITY_PROGRAM_MODULE_SET,
    XR_RUNTIME_ARTIFACT_AUTHORITY_KIND_COUNT,
} XrRuntimeArtifactAuthorityKind;

typedef struct XrRuntimeArtifactAuthorityIdentity {
    uint32_t schema_version;
    uint32_t authority_kind;
    uint32_t semantic_module_count;
    uint32_t reserved;
    uint64_t required_family_mask;
    uint64_t required_capability_mask;
    uint64_t provider_mask;
    uint8_t semantic_fingerprint[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE];
    uint8_t program_fingerprint[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE];
    uint8_t program_module_set_fingerprint[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE];
    uint8_t generation_closure_id[XR_RUNTIME_ARTIFACT_GENERATION_CLOSURE_ID_SIZE];
    uint8_t operation_registry_fingerprint[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE];
    uint8_t target_profile_fingerprint[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE];
    uint8_t runtime_abi_fingerprint[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE];
    uint8_t provider_set_fingerprint[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE];
    uint8_t object_header_fingerprint[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE];
    uint8_t authority_fingerprint[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE];
} XrRuntimeArtifactAuthorityIdentity;

typedef struct XrRuntimeArtifactImage {
    const uint8_t *bytes;
    size_t size;
} XrRuntimeArtifactImage;

/* Availability covers only exact XSM-backed authority construction. */
XRAY_API bool xr_runtime_artifact_authority_load_available(void);
XRAY_API bool xr_runtime_artifact_authority_load_xsm(
    const uint8_t *artifact_bytes, size_t artifact_size,
    XrRuntimeArtifactAuthority **authority, char *diagnostic,
    size_t diagnostic_size);
/* Loads the currently admitted exact two-fragment program semantic module set
 * into the same authority owner used by the ordinary single-XSM route. Input
 * order is irrelevant; every XSM supplies its verified program-module row and
 * exact dependencies. A standalone XSM is not accepted here and no
 * module-local fallback exists. */
XRAY_API bool xr_runtime_artifact_authority_load_xsm_module_set(
    const XrRuntimeArtifactImage *semantic_artifacts,
    uint32_t semantic_artifact_count,
    XrRuntimeArtifactAuthority **authority, char *diagnostic,
    size_t diagnostic_size);
XRAY_API bool xr_runtime_artifact_authority_verify(
    const XrRuntimeArtifactAuthority *authority, char *diagnostic,
    size_t diagnostic_size);
XRAY_API bool xr_runtime_artifact_authority_identity(
    const XrRuntimeArtifactAuthority *authority,
    XrRuntimeArtifactAuthorityIdentity *identity);
XRAY_API void xr_runtime_artifact_authority_free(
    XrRuntimeArtifactAuthority *authority);

XRAY_API bool xr_runtime_target_plan_load(
    const uint8_t *artifact_bytes, size_t artifact_size,
    const XrRuntimeArtifactAuthority *authority,
    XrTargetPlan **verified_target_plan, char *diagnostic,
    size_t diagnostic_size);

/* Existing TargetPlan lifetime owner; this is a declaration, not an alias. */
XRAY_API void xr_target_plan_free(XrTargetPlan *plan);

#endif  // XRAY_TARGET_PLAN_LOAD_H
