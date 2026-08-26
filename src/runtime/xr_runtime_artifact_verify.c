/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_artifact_verify.c - Independent runtime authority verifier
 *
 * KEY CONCEPT:
 *   Verification re-derives every packaged identity from retained immutable
 *   plans and the current canonical runtime. It never trusts artifact rows to
 *   declare which providers are available.
 */

#include "xr_runtime_artifact_authority_internal.h"
#include "abi/xr_runtime_target_authority.h"
#include "../plan/format/xr_xtp_internal.h"
#include "../plan/semantic/xr_semantic_verify.h"
#include "../plan/target/xr_target_plan_internal.h"
#include "../plan/target/xr_target_profile_internal.h"
#include "../plan/target/xr_target_capability.h"
#include "../plan/target/xr_target_verify.h"
#include <stdio.h>
#include <string.h>

static bool fail(char *diagnostic, size_t diagnostic_size, const char *code,
                 const char *detail) {
    if (diagnostic && diagnostic_size)
        snprintf(diagnostic, diagnostic_size, "%s: %s", code, detail);
    return false;
}

static bool bytes_equal(const uint8_t left[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE],
                        const uint8_t right[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE]) {
    return memcmp(left, right, XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE) == 0;
}

static bool fingerprint_equal_bytes(
    XrFingerprint fingerprint,
    const uint8_t bytes[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE]) {
    return memcmp(fingerprint.bytes, bytes,
                  XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE) == 0;
}

static bool bytes_are_zero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
}

static const XrSemanticPlan *verifier_program_entry(
    XrSemanticPlan *const *modules, uint32_t module_count) {
    const XrSemanticPlan *entry = NULL;
    for (uint32_t module = 0; module < module_count; module++) {
        size_t count = xr_semantic_plan_program_function_binding_count(
            modules[module]);
        for (uint32_t function = 0; function < count; function++) {
            const XrSemanticProgramFunctionBinding *binding =
                xr_semantic_plan_program_function_binding(
                    modules[module], function);
            if (!binding ||
                (binding->flags & XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) == 0)
                continue;
            if (entry)
                return NULL;
            entry = modules[module];
        }
    }
    return entry;
}

static bool current_runtime_identity(
    XrRuntimeTargetAuthority *runtime, uint64_t *provider_mask,
    XrFingerprint *runtime_fingerprint,
    XrFingerprint *provider_fingerprint, XrFingerprint *object_fingerprint) {
    XrRuntimeObjectHeaderAbi object_header;
    return xr_runtime_target_authority_native_hosted(runtime) ==
               XR_RUNTIME_ABI_OK &&
           xr_runtime_abi_contract_fingerprint(&runtime->runtime_abi,
                                               runtime_fingerprint) ==
               XR_RUNTIME_ABI_OK &&
           xr_target_provider_set_fingerprint(
               runtime->providers, runtime->provider_count, provider_mask,
               provider_fingerprint) == XR_RUNTIME_ABI_OK &&
           xr_runtime_object_header_abi_materialize(
               &runtime->object_header_materialization, &object_header) ==
               XR_RUNTIME_ABI_OK &&
           xr_runtime_object_header_abi_fingerprint(
               &object_header, object_fingerprint) == XR_RUNTIME_ABI_OK;
}

XRAY_API bool xr_runtime_artifact_authority_verify(
    const XrRuntimeArtifactAuthority *authority, char *diagnostic,
    size_t diagnostic_size) {
    if (!authority || !authority->semantic_plan || !authority->target_profile)
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "artifact authority package is incomplete");
    const XrRuntimeArtifactAuthorityIdentity *identity = &authority->identity;
    bool program = identity->authority_kind ==
                   XR_RUNTIME_ARTIFACT_AUTHORITY_PROGRAM_MODULE_SET;
    bool ordinary = identity->authority_kind ==
                    XR_RUNTIME_ARTIFACT_AUTHORITY_ORDINARY_MODULE;
    if (identity->schema_version !=
            XR_RUNTIME_ARTIFACT_AUTHORITY_SCHEMA_VERSION ||
        identity->reserved != 0 || (!ordinary && !program) ||
        (ordinary &&
         (identity->semantic_module_count != 1u ||
          authority->semantic_modules || authority->semantic_module_count)) ||
        (program &&
         (!authority->semantic_modules ||
          identity->semantic_module_count != authority->semantic_module_count ||
          authority->semantic_module_count < 2u ||
          authority->semantic_module_count > XR_TARGET_MAX_PROGRAM_MODULES)))
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2000",
                    "artifact authority schema is not exactly supported");
    if (identity->required_family_mask != XR_TARGET_REQUIRED_FAMILIES)
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1001",
                    "artifact authority family closure is incomplete");
    uint64_t expected_capability_mask = 0;
    const XrTargetProfileDraft *profile_facts =
        xr_target_profile_facts(authority->target_profile);
    char nested[512] = {0};
    bool semantic_exact = false;
    if (ordinary) {
        semantic_exact = profile_facts &&
                         xr_semantic_plan_program_provenance(
                             authority->semantic_plan) == NULL &&
                         xr_semantic_plan_verify(authority->semantic_plan,
                                                 nested, sizeof(nested)) &&
                         xr_target_semantic_capability_mask(
                             authority->semantic_plan,
                             profile_facts->machine.runtime_profile,
                             &expected_capability_mask) &&
                         bytes_are_zero(identity->program_fingerprint,
                                        sizeof(identity->program_fingerprint)) &&
                         bytes_are_zero(
                             identity->program_module_set_fingerprint,
                             sizeof(identity->program_module_set_fingerprint)) &&
                         bytes_are_zero(identity->generation_closure_id,
                                        sizeof(identity->generation_closure_id));
    } else {
        const XrSemanticPlan *entry = verifier_program_entry(
            authority->semantic_modules, authority->semantic_module_count);
        XrFingerprint module_set = {{0}};
        const XrSemanticProgramProvenance *program_provenance =
            authority->semantic_modules[0]
                ? xr_semantic_plan_program_provenance(
                      authority->semantic_modules[0])
                : NULL;
        semantic_exact =
            entry == authority->semantic_plan && profile_facts &&
            xr_target_semantic_program_module_set_verify(
                (const XrSemanticPlan *const *) authority->semantic_modules,
                authority->semantic_module_count, nested, sizeof(nested)) &&
            xr_target_semantic_capability_requirements(
                (const XrSemanticPlan *const *) authority->semantic_modules,
                authority->semantic_module_count, authority->target_profile,
                &expected_capability_mask, nested, sizeof(nested)) &&
            xr_target_semantic_module_set_fingerprint(
                (const XrSemanticPlan *const *) authority->semantic_modules,
                authority->semantic_module_count, &module_set) &&
            program_provenance &&
            fingerprint_equal_bytes(program_provenance->program_fingerprint,
                                    identity->program_fingerprint) &&
            fingerprint_equal_bytes(module_set,
                                    identity->program_module_set_fingerprint) &&
            memcmp(program_provenance->generation_identity.bytes,
                   identity->generation_closure_id,
                   sizeof(identity->generation_closure_id)) == 0;
        for (uint32_t module = 0;
             semantic_exact && module < authority->semantic_module_count;
             module++)
            semantic_exact = xr_fingerprint_equal(
                xr_semantic_plan_operation_registry_fingerprint(
                    authority->semantic_modules[module]),
                xr_semantic_plan_operation_registry_fingerprint(
                    authority->semantic_plan));
    }
    if (!semantic_exact ||
        identity->required_capability_mask != expected_capability_mask)
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1004",
                    "artifact authority capability closure is not exact");

    if (!xr_target_profile_verify(authority->target_profile, nested,
                                  sizeof(nested)))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "artifact authority retained an unverified input");
    if (!fingerprint_equal_bytes(
            xr_semantic_plan_fingerprint(authority->semantic_plan),
            identity->semantic_fingerprint) ||
        !fingerprint_equal_bytes(
            xr_semantic_plan_operation_registry_fingerprint(
                authority->semantic_plan),
            identity->operation_registry_fingerprint) ||
        !fingerprint_equal_bytes(
            xr_target_profile_fingerprint(authority->target_profile),
            identity->target_profile_fingerprint))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "artifact authority semantic or target fingerprint changed");

    XrRuntimeTargetAuthority runtime;
    uint64_t provider_mask = 0;
    XrFingerprint runtime_fingerprint;
    XrFingerprint provider_fingerprint;
    XrFingerprint object_fingerprint;
    if (!current_runtime_identity(&runtime, &provider_mask,
                                  &runtime_fingerprint,
                                  &provider_fingerprint,
                                  &object_fingerprint))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "canonical native runtime authority is invalid");
    const XrTargetProfileDraft *facts =
        xr_target_profile_facts(authority->target_profile);
    if (!facts || !xr_runtime_target_authority_machine_matches(
                      &runtime, &facts->machine))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "artifact authority does not bind the canonical native machine facts");
    if (identity->provider_mask != provider_mask ||
        facts->provider_mask != provider_mask ||
        !xr_target_capability_mask_is_backed(
            identity->required_capability_mask, provider_mask) ||
        !fingerprint_equal_bytes(runtime_fingerprint,
                                 identity->runtime_abi_fingerprint) ||
        !fingerprint_equal_bytes(provider_fingerprint,
                                 identity->provider_set_fingerprint) ||
        !fingerprint_equal_bytes(object_fingerprint,
                                 identity->object_header_fingerprint) ||
        !xr_fingerprint_equal(facts->runtime_abi_fingerprint,
                              runtime_fingerprint) ||
        !xr_fingerprint_equal(facts->provider_set_fingerprint,
                              provider_fingerprint) ||
        !xr_fingerprint_equal(facts->object_header_fingerprint,
                              object_fingerprint))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "artifact authority does not bind the exact runtime provider set");

    uint8_t actual[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE];
    xr_runtime_artifact_authority_compute_fingerprint(identity, actual);
    if (!bytes_equal(actual, identity->authority_fingerprint))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "artifact authority package fingerprint changed");
    return true;
}

XR_FUNCDEF bool xr_runtime_artifact_authority_bind_candidate(
    const XrRuntimeArtifactAuthority *authority,
    const XrXtpCandidate *candidate, const XrXtpIdentity *identity,
    char *diagnostic, size_t diagnostic_size) {
    if (!candidate || !identity || !xr_runtime_artifact_authority_verify(
                         authority, diagnostic, diagnostic_size))
        return false;
    const XrRuntimeArtifactAuthorityIdentity *expected = &authority->identity;
    if (identity->completed_family_mask != expected->required_family_mask)
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1001",
                    "artifact family closure does not match its authority");
    const uint8_t *expected_semantic =
        expected->authority_kind ==
                XR_RUNTIME_ARTIFACT_AUTHORITY_PROGRAM_MODULE_SET
            ? expected->program_module_set_fingerprint
            : expected->semantic_fingerprint;
    if (!fingerprint_equal_bytes(identity->semantic_fingerprint,
                                 expected_semantic) ||
        !fingerprint_equal_bytes(
            identity->operation_registry_fingerprint,
            expected->operation_registry_fingerprint) ||
        !fingerprint_equal_bytes(identity->profile_fingerprint,
                                 expected->target_profile_fingerprint) ||
        !fingerprint_equal_bytes(identity->runtime_fingerprint,
                                 expected->runtime_abi_fingerprint) ||
        !fingerprint_equal_bytes(identity->provider_fingerprint,
                                 expected->provider_set_fingerprint) ||
        !fingerprint_equal_bytes(identity->object_fingerprint,
                                 expected->object_header_fingerprint))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "artifact identity does not match its authority package");
    const XrXtpSectionView *graphs = xr_xtp_candidate_section(
        candidate, XR_XTP_SECTION_PROGRAM_GRAPHS);
    const XrXtpSectionView *partitions = xr_xtp_candidate_section(
        candidate, XR_XTP_SECTION_MODULE_PARTITIONS);
    if (!graphs || !partitions)
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "artifact candidate omits its program authority sections");
    if (expected->authority_kind ==
        XR_RUNTIME_ARTIFACT_AUTHORITY_ORDINARY_MODULE) {
        if (graphs->count || partitions->count)
            return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                        "ordinary authority cannot admit a program candidate");
    } else {
        XrTargetProgramGraphRecord graph = {0};
        if (graphs->count != 1u ||
            partitions->count != expected->semantic_module_count ||
            !xr_xtp_decode_rows(
                XR_XTP_SECTION_PROGRAM_GRAPHS,
                candidate->bytes + graphs->offset, graphs->count, &graph) ||
            graph.module_count != expected->semantic_module_count ||
            !fingerprint_equal_bytes(graph.program_fingerprint,
                                     expected->program_fingerprint) ||
            memcmp(graph.generation_identity.bytes,
                   expected->generation_closure_id,
                   sizeof(expected->generation_closure_id)) != 0)
            return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                        "program candidate does not bind the exact authority identity");
    }
    return true;
}

XR_FUNCDEF bool xr_runtime_artifact_authority_bind_plan(
    const XrRuntimeArtifactAuthority *authority, const XrTargetPlan *plan,
    char *diagnostic, size_t diagnostic_size) {
    if (!xr_runtime_artifact_authority_verify(authority, diagnostic,
                                              diagnostic_size))
        return false;
    char nested[512] = {0};
    if (!plan || !xr_target_plan_is_verified(plan) ||
        !xr_target_plan_verify(plan, nested, sizeof(nested)))
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "authority binding requires an independently verified TargetPlan");
    const XrRuntimeArtifactAuthorityIdentity *identity = &authority->identity;
    const uint8_t *expected_semantic =
        identity->authority_kind ==
                XR_RUNTIME_ARTIFACT_AUTHORITY_PROGRAM_MODULE_SET
            ? identity->program_module_set_fingerprint
            : identity->semantic_fingerprint;
    if (xr_target_plan_completed_family_mask(plan) !=
            identity->required_family_mask ||
        !fingerprint_equal_bytes(xr_target_plan_semantic_fingerprint(plan),
                                 expected_semantic) ||
        !xr_target_profile_require_exact(
            authority->target_profile, xr_target_plan_profile(plan), nested,
            sizeof(nested)))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "TargetPlan identity does not match its authority package");
    uint64_t capability_mask = 0;
    if (!xr_target_plan_capability_mask(plan, &capability_mask) ||
        capability_mask != identity->required_capability_mask ||
        !xr_target_capability_mask_is_backed(
            capability_mask, identity->provider_mask))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1004",
                    "TargetPlan capability closure is missing or unbound");
    uint32_t graph_count = 0;
    uint32_t partition_count = 0;
    const XrTargetProgramGraphRecord *graphs =
        xr_target_plan_program_graphs(plan, &graph_count);
    (void) xr_target_plan_module_partitions(plan, &partition_count);
    if (identity->authority_kind ==
        XR_RUNTIME_ARTIFACT_AUTHORITY_PROGRAM_MODULE_SET) {
        XrFingerprint module_set = {{0}};
        if (!graphs || graph_count != 1u ||
            partition_count != identity->semantic_module_count ||
            graphs[0].module_count != identity->semantic_module_count ||
            !fingerprint_equal_bytes(graphs[0].program_fingerprint,
                                     identity->program_fingerprint) ||
            memcmp(graphs[0].generation_identity.bytes,
                   identity->generation_closure_id,
                   sizeof(identity->generation_closure_id)) != 0 ||
            !xr_target_plan_program_module_set_fingerprint(plan, &module_set) ||
            !fingerprint_equal_bytes(
                module_set, identity->program_module_set_fingerprint))
            return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                        "program TargetPlan identity does not match its module-set authority");
    } else if (graph_count || partition_count) {
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "ordinary artifact authority cannot bind a program TargetPlan");
    }
    return true;
}
