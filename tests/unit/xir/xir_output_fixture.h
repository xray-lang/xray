/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_output_fixture.h - Checked output groups for both execution engines
 *
 * KEY CONCEPT:
 *   Group boundaries survive physical lowering and native emission.
 */
#ifndef XIR_OUTPUT_FIXTURE_H
#define XIR_OUTPUT_FIXTURE_H
#include "xir/xxir.h"
static XrXirArtifact *output_fixture(void) {
    const XrXirType parameters[] = {XR_XIR_I64, XR_XIR_STRING};
    XrXirInstruction ops[] = {
        {XR_XIR_PRINT, XR_XIR_UNIT, {0, 0}, {0, 0}, 0},
        {XR_XIR_PRINT, XR_XIR_UNIT, {0, 1}, {0, 0}, 2},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {1, 0}, {0, 0}, 2},
        {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0, 0}, {0, 0}, 1},
        {XR_XIR_PRINT, XR_XIR_UNIT, {5, 0}, {0, 0}, 1},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0}
    };
    const XrXirBlock blocks[] = {{0, 6}};
    const XrXirFunction functions[] = {{"output", 6, parameters, 2, XR_XIR_UNIT, blocks, 1, ops, 6}};
    const XrXirModule built = {XR_XIR_BUILT, functions, 1, NULL};
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_lower(checked, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    return lowered;
}
#endif
