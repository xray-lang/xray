/*
 * Link the installed compiler product's exact authority surface without
 * xray_core. Invalid inputs exercise only fail-closed preconditions while the
 * link itself proves every authority and its static transitive closure resolve.
 */

#include "aot/emit_c/xr_c_emission_plan.h"
#include "plan/format/xr_xsm_schema.h"
#include "plan/format/xr_xtp_schema.h"
#include "plan/target/xr_target_builder.h"

#include <stddef.h>
#include <stdint.h>

int main(void) {
    char error[256] = {0};
    uint8_t *bytes = NULL;
    size_t size = 0;
    XrTargetPlan *target = NULL;
    XrCEmissionPlan *emission = NULL;
    XrFingerprint zero = {{0}};

    if (xr_target_plan_build(NULL, NULL, &target, error, sizeof(error)) || target)
        return 1;
    if (xr_xsm_encode(NULL, &bytes, &size, error, sizeof(error)) || bytes || size)
        return 2;
    if (xr_xtp_encode_plan(NULL, &bytes, &size, error, sizeof(error)) || bytes || size)
        return 3;
    if (xr_c_emission_plan_build(NULL, zero, &emission, error, sizeof(error)) || emission)
        return 4;
    return 0;
}
