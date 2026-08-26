/*
 * test_runtime_target_plan_load_archive.c - Installed archive link boundary
 */

#include "xray_target_plan_load.h"
#include "xray_runtime_generation.h"
#include "runtime_scalar_artifacts.h"
#include <stdio.h>
#include <string.h>

static void hex_bytes(const uint8_t *bytes, size_t size, char *hex) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; i++) {
        hex[i * 2] = digits[bytes[i] >> 4];
        hex[i * 2 + 1] = digits[bytes[i] & 15u];
    }
    hex[size * 2] = '\0';
}

static bool bytes_are_zero(const uint8_t *bytes, size_t size) {
    for (size_t i = 0; bytes && i < size; i++)
        if (bytes[i] != 0u)
            return false;
    return bytes != NULL;
}

static int write_evidence(const char *path,
                          const XrRuntimeArtifactAuthorityIdentity *authority,
                          const XrModuleGenerationIdentity *generation) {
    char artifact[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE * 2 + 1];
    char semantic[XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE * 2 + 1];
    char target[XR_RUNTIME_GENERATION_FINGERPRINT_SIZE * 2 + 1];
    char generation_id[XR_RUNTIME_GENERATION_FINGERPRINT_SIZE * 2 + 1];
    hex_bytes(authority->authority_fingerprint,
              XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE, artifact);
    hex_bytes(authority->semantic_fingerprint,
              XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE, semantic);
    hex_bytes(generation->target_plan_fingerprint,
              XR_RUNTIME_GENERATION_FINGERPRINT_SIZE, target);
    hex_bytes(generation->generation_fingerprint,
              XR_RUNTIME_GENERATION_FINGERPRINT_SIZE, generation_id);
    FILE *output = fopen(path, "wb");
    if (!output)
        return 0;
    int written = fprintf(
        output,
        "{\"schema\":1,\"artifact\":\"%s\",\"generation\":\"%s\","
        "\"semantic\":\"%s\",\"target\":\"%s\"}\n",
        artifact, generation_id, semantic, target);
    return fclose(output) == 0 && written > 0;
}

int main(int argc, char **argv) {
    XrTargetPlan *plan = (XrTargetPlan *) (uintptr_t) 1;
    XrRuntimeArtifactAuthority *authority =
        (XrRuntimeArtifactAuthority *) (uintptr_t) 1;
    char diagnostic[256] = {0};
    XrRuntimeArtifactAuthorityIdentity identity;
    if (!xr_runtime_artifact_authority_load_available() ||
        xr_runtime_artifact_authority_load_xsm(
            NULL, 0, &authority, diagnostic, sizeof(diagnostic)) ||
        authority != NULL || strstr(diagnostic, "XR_ARTIFACT_2004") == NULL ||
        xr_runtime_artifact_authority_verify(NULL, diagnostic,
                                             sizeof(diagnostic)) ||
        xr_runtime_artifact_authority_identity(NULL, &identity) ||
        xr_runtime_target_plan_load(NULL, 0, NULL, &plan, diagnostic,
                                    sizeof(diagnostic)) ||
        plan != NULL || strstr(diagnostic, "XR_ARTIFACT_2004") == NULL) {
        fprintf(stderr, "runtime TargetPlan unavailable boundary failed: %s\n",
                diagnostic);
        return 1;
    }

    if (!xr_runtime_artifact_authority_load_xsm(
            xr_runtime_scalar_xsm, sizeof(xr_runtime_scalar_xsm), &authority,
            diagnostic, sizeof(diagnostic)) ||
        !authority || !xr_runtime_artifact_authority_verify(
                          authority, diagnostic, sizeof(diagnostic)) ||
        !xr_runtime_target_plan_load(
            xr_runtime_scalar_xtp, sizeof(xr_runtime_scalar_xtp), authority,
            &plan, diagnostic, sizeof(diagnostic)) ||
        !plan) {
        fprintf(stderr, "runtime artifact load failed: %s\n", diagnostic);
        xr_target_plan_free(plan);
        xr_runtime_artifact_authority_free(authority);
        return 1;
    }
    if (!xr_runtime_artifact_authority_identity(authority, &identity)) {
        fprintf(stderr, "runtime artifact identity unavailable\n");
        xr_target_plan_free(plan);
        xr_runtime_artifact_authority_free(authority);
        return 1;
    }
    if (identity.schema_version !=
            XR_RUNTIME_ARTIFACT_AUTHORITY_SCHEMA_VERSION ||
        identity.authority_kind !=
            XR_RUNTIME_ARTIFACT_AUTHORITY_ORDINARY_MODULE ||
        identity.semantic_module_count != 1u ||
        !bytes_are_zero(identity.program_fingerprint,
                        sizeof(identity.program_fingerprint)) ||
        !bytes_are_zero(identity.program_module_set_fingerprint,
                        sizeof(identity.program_module_set_fingerprint)) ||
        !bytes_are_zero(identity.generation_closure_id,
                        sizeof(identity.generation_closure_id))) {
        fprintf(stderr, "ordinary runtime artifact identity is not exact v3\n");
        xr_target_plan_free(plan);
        xr_runtime_artifact_authority_free(authority);
        return 1;
    }

    XrRuntimeGenerationBudget budget = {
        .schema_version = XR_RUNTIME_GENERATION_SCHEMA_VERSION,
        .max_loaded_generations = 1,
        .max_total_pins = 4,
        .max_pins_per_generation = 4,
        .max_pins_by_kind = {4, 4, 4, 4, 4},
    };
    XrRuntimeGenerationAuthority *generation_authority = NULL;
    XrLoadedModuleGeneration *generation = NULL;
    XrModuleGenerationSnapshot active_snapshot;
    memset(&active_snapshot, 0, sizeof(active_snapshot));
    int64_t result = 0;
    bool executed = xr_runtime_generation_authority_create(
                        &budget, &generation_authority, diagnostic,
                        sizeof(diagnostic)) &&
                    xr_module_generation_load_verified_target_plan(
                        generation_authority, plan, &generation, diagnostic,
                        sizeof(diagnostic)) &&
                    xr_module_generation_prepare(generation, diagnostic,
                                                 sizeof(diagnostic)) &&
                    xr_module_generation_activate(generation, diagnostic,
                                                  sizeof(diagnostic)) &&
                    xr_module_generation_execute_sole_scalar_i64(
                        generation, &result, diagnostic,
                        sizeof(diagnostic)) &&
                    result == 42 &&
                    xr_module_generation_snapshot(generation,
                                                  &active_snapshot) &&
                    xr_module_generation_begin_drain(
                        generation, diagnostic, sizeof(diagnostic)) &&
                    xr_module_generation_retire(
                        generation, diagnostic, sizeof(diagnostic)) &&
                    xr_module_generation_unload(
                        &generation, diagnostic, sizeof(diagnostic)) &&
                    xr_runtime_generation_authority_destroy(
                        &generation_authority, diagnostic, sizeof(diagnostic));
    xr_target_plan_free(plan);
    xr_runtime_artifact_authority_free(authority);
    if (!executed) {
        fprintf(stderr, "runtime scalar artifact execution failed: %s\n",
                diagnostic);
        return 1;
    }
    if (argc == 3 && strcmp(argv[1], "--evidence-json") == 0 &&
        !write_evidence(argv[2], &identity, &active_snapshot.identity)) {
        fprintf(stderr, "runtime scalar evidence write failed\n");
        return 1;
    }
    if (argc != 1 && argc != 3) {
        fprintf(stderr, "usage: %s [--evidence-json <path>]\n", argv[0]);
        return 1;
    }
    puts("runtime scalar artifact load and execution passed");
    return 0;
}
