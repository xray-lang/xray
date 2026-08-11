/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_plan_load.c - Runtime-owned verified TargetPlan load boundary
 */

#include "../../include/xray_target_plan_load.h"
#include "xr_runtime_artifact_authority_internal.h"
#include "../plan/format/xr_artifact_kind.h"
#include "../plan/format/xr_xtp_internal.h"
#include <stdio.h>
#include <string.h>

static bool fail(char *diagnostic, size_t diagnostic_size,
                 const char *code, const char *detail) {
    if (diagnostic && diagnostic_size)
        snprintf(diagnostic, diagnostic_size, "%s: %s", code, detail);
    return false;
}

static bool propagate_nested(char *diagnostic, size_t diagnostic_size,
                             const char *code, const char *stage,
                             const char *nested) {
    if (nested && strncmp(nested, "XR_", 3) == 0) {
        if (diagnostic && diagnostic_size)
            snprintf(diagnostic, diagnostic_size, "%s", nested);
        return false;
    }
    char detail[640];
    snprintf(detail, sizeof(detail), "%s: %s", stage,
             nested && nested[0] ? nested : "unspecified rejection");
    return fail(diagnostic, diagnostic_size, code, detail);
}

XRAY_API bool xr_runtime_target_plan_load(
    const uint8_t *artifact_bytes, size_t artifact_size,
    const XrRuntimeArtifactAuthority *authority,
    XrTargetPlan **verified_target_plan, char *diagnostic,
    size_t diagnostic_size) {
    if (verified_target_plan)
        *verified_target_plan = NULL;
    if (!verified_target_plan || !artifact_bytes || !artifact_size || !authority)
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "a verified artifact authority package is required");
    if (!xr_runtime_artifact_authority_verify(authority, diagnostic,
                                              diagnostic_size))
        return false;

    size_t prefix_size = artifact_size < XR_ARTIFACT_PROBE_SIZE
                             ? artifact_size
                             : XR_ARTIFACT_PROBE_SIZE;
    XrArtifactProbeResult probe =
        xr_artifact_probe(NULL, artifact_bytes, prefix_size);
    if (probe.status != XR_ARTIFACT_PROBE_MATCH ||
        probe.kind != XR_ARTIFACT_KIND_XTP)
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2000",
                    "runtime TargetPlan loading accepts only XTP v5 bytes");
    XrXtpCandidate *candidate = NULL;
    char nested[512] = {0};
    if (!xr_xtp_decode_candidate(artifact_bytes, artifact_size, &candidate,
                                 nested, sizeof(nested)))
        return propagate_nested(diagnostic, diagnostic_size,
                                "XR_ARTIFACT_2000",
                                "XTP v5 candidate decoding failed", nested);

    XrXtpIdentity identity;
    bool have_identity = xr_xtp_candidate_identity(candidate, &identity);
    XrTargetPlan *plan = NULL;
    nested[0] = '\0';
    bool materialized =
        have_identity && xr_runtime_artifact_authority_bind_candidate(
                             authority, &identity, nested, sizeof(nested)) &&
        xr_xtp_materialize_target_plan(
            candidate, authority->semantic_plan, authority->target_profile,
            &plan, nested, sizeof(nested));
    xr_xtp_candidate_release(candidate);
    if (!materialized)
        return propagate_nested(diagnostic, diagnostic_size,
                                "XR_ARTIFACT_2004",
                                "XTP v5 materialization failed", nested);

    nested[0] = '\0';
    if (!xr_target_plan_is_verified(plan) ||
        !xr_fingerprint_equal(xr_target_plan_fingerprint(plan),
                              identity.plan_fingerprint)) {
        xr_target_plan_free(plan);
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "materialized TargetPlan did not preserve exact verified identity");
    }
    if (!xr_runtime_artifact_authority_bind_plan(
            authority, plan, nested, sizeof(nested))) {
        xr_target_plan_free(plan);
        return propagate_nested(diagnostic, diagnostic_size,
                                "XR_ARTIFACT_2004",
                                "TargetPlan authority binding failed", nested);
    }

    *verified_target_plan = plan;
    return true;
}
