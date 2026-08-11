/*
 * test_runtime_target_plan_load_archive.c - Installed archive link boundary
 */

#include "xray_target_plan_load.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    XrTargetPlan *plan = (XrTargetPlan *) (uintptr_t) 1;
    char diagnostic[256] = {0};
    if (xr_runtime_target_plan_load(NULL, 0, NULL, NULL, &plan, diagnostic,
                                    sizeof(diagnostic)) ||
        plan != NULL || strstr(diagnostic, "XR_ARTIFACT_2004") == NULL) {
        fprintf(stderr, "runtime TargetPlan archive boundary failed: %s\n",
                diagnostic);
        return 1;
    }
    puts("runtime TargetPlan archive link boundary passed");
    return 0;
}
