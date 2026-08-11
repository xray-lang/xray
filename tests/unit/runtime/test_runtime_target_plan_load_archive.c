/*
 * test_runtime_target_plan_load_archive.c - Installed archive link boundary
 */

#include "xray_target_plan_load.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    XrTargetPlan *plan = (XrTargetPlan *) (uintptr_t) 1;
    char diagnostic[256] = {0};
    XrRuntimeArtifactAuthorityIdentity identity;
    if (xr_runtime_artifact_authority_load_available() ||
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
    puts("runtime TargetPlan unavailable boundary passed");
    return 0;
}
