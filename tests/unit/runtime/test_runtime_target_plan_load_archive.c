/*
 * test_runtime_target_plan_load_archive.c - Installed archive link boundary
 */

#include "xray_target_plan_load.h"
#include "xray_runtime_generation.h"
#include "runtime_scalar_artifacts.h"
#include <stdio.h>
#include <string.h>

int main(void) {
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

    XrRuntimeGenerationBudget budget = {
        .schema_version = XR_RUNTIME_GENERATION_SCHEMA_VERSION,
        .max_loaded_generations = 1,
        .max_total_pins = 4,
        .max_pins_per_generation = 4,
        .max_pins_by_kind = {4, 4, 4, 4, 4},
    };
    XrRuntimeGenerationAuthority *generation_authority = NULL;
    XrLoadedModuleGeneration *generation = NULL;
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
    puts("runtime scalar artifact load and execution passed");
    return 0;
}
