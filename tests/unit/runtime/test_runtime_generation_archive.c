/*
 * test_runtime_generation_archive.c - Runtime-only generation link boundary
 */

#include "xray_runtime_generation.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    XrRuntimeGenerationBudget budget = {
        .schema_version = XR_RUNTIME_GENERATION_SCHEMA_VERSION,
        .max_loaded_generations = 1,
        .max_total_pins = 8,
        .max_pins_per_generation = 8,
        .max_pins_by_kind = {8, 8, 8, 8, 8},
    };
    XrRuntimeGenerationAuthority *authority = NULL;
    XrLoadedModuleGeneration *generation =
        (XrLoadedModuleGeneration *) (uintptr_t) 1;
    char diagnostic[256] = {0};
    if (!xr_runtime_generation_authority_create(
            &budget, &authority, diagnostic, sizeof(diagnostic)) ||
        !authority || xr_runtime_generation_activation_available() ||
        xr_module_generation_load_verified_target_plan(
            authority, NULL, &generation, diagnostic, sizeof(diagnostic)) ||
        generation != NULL ||
        strstr(diagnostic, "XR_ARTIFACT_2004") == NULL ||
        !xr_runtime_generation_authority_destroy(
            &authority, diagnostic, sizeof(diagnostic)) ||
        authority != NULL) {
        fprintf(stderr, "runtime generation archive boundary failed: %s\n",
                diagnostic);
        return 1;
    }
    puts("runtime generation archive boundary passed");
    return 0;
}
