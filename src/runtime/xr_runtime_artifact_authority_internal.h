/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_artifact_authority_internal.h - Runtime artifact authority storage
 */

#ifndef XR_RUNTIME_ARTIFACT_AUTHORITY_INTERNAL_H
#define XR_RUNTIME_ARTIFACT_AUTHORITY_INTERNAL_H

#include "../../include/xray_target_plan_load.h"
#include "../plan/format/xr_xtp_schema.h"

struct XrRuntimeArtifactAuthority {
    XrSemanticPlan *semantic_plan;
    XrTargetProfile *target_profile;
    XrRuntimeArtifactAuthorityIdentity identity;
};

XR_FUNC bool xr_runtime_artifact_authority_create_internal(
    const XrSemanticPlan *verified_semantic_plan,
    XrRuntimeArtifactAuthority **authority, char *diagnostic,
    size_t diagnostic_size);
XR_FUNC void xr_runtime_artifact_authority_compute_fingerprint(
    const XrRuntimeArtifactAuthorityIdentity *identity,
    uint8_t out[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE]);
XR_FUNC bool xr_runtime_artifact_authority_bind_candidate(
    const XrRuntimeArtifactAuthority *authority, const XrXtpIdentity *identity,
    char *diagnostic, size_t diagnostic_size);
XR_FUNC bool xr_runtime_artifact_authority_bind_plan(
    const XrRuntimeArtifactAuthority *authority, const XrTargetPlan *plan,
    char *diagnostic, size_t diagnostic_size);

#endif  // XR_RUNTIME_ARTIFACT_AUTHORITY_INTERNAL_H
