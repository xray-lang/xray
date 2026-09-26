/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_string_fixture.h - Verified managed string instruction fixtures
 *
 * KEY CONCEPT:
 *   Assert independent expected values and explicit ownership at every exit.
 */

#ifndef XIR_STRING_FIXTURE_H
#define XIR_STRING_FIXTURE_H
#include "xir/xxir.h"
static XrXirArtifact *string_fixture(uint32_t mode) {
    const uint32_t operands[] = {0, 1, 0, 1};
    const XrXirType parameters[] = {XR_XIR_STRING, XR_XIR_STRING, XR_XIR_STRING, XR_XIR_STRING};
    XrXirInstruction root[] = {
        {XR_XIR_CALL, XR_XIR_STRING, {0, 4}, {0, 0}, 1},
        {XR_XIR_COPY, XR_XIR_STRING, {2, 0}, {0, 0}, 0},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {3, 0}, {0, 0}, 1},
        {XR_XIR_RETURN, XR_XIR_UNIT, {3, 0}, {0, 0}, 0}
    };
    XrXirInstruction child[] = {
        {XR_XIR_COPY, XR_XIR_STRING, {2, 0}, {0, 0}, 0},
        {XR_XIR_CONCAT_STRING, XR_XIR_STRING, {4, 3}, {0, 0}, 0},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {5, 0}, {0, 0}, 2},
        {XR_XIR_SUSPEND, XR_XIR_UNIT, {0, 0}, {0, 0}, 0},
        {XR_XIR_CONST_I64, XR_XIR_I64, {0, 0}, {0, 0}, 91},
        {XR_XIR_RETURN, XR_XIR_UNIT, {5, 0}, {0, 0}, 0}
    };
    XrXirInstruction loop[] = {
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {1, 0}, 0},
        {XR_XIR_CONCAT_STRING, XR_XIR_STRING, {0, 1}, {0, 0}, 0},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {3, 0}, {0, 0}, 1},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {1, 0}, 0}
    };
    if (mode) child[5] = (XrXirInstruction) {XR_XIR_THROW, XR_XIR_UNIT, {8, 0}, {0, 0}, 0};
    const XrXirBlock root_blocks[] = {{0, 4}}, child_blocks[] = {{0, 6}}, loop_blocks[] = {{0, 1}, {1, 3}};
    const XrXirFunction functions[] = {
        {"root", 4, parameters, 2, XR_XIR_STRING, root_blocks, 1, root, 4, operands, 4},
        {"child", 5, parameters, 4, XR_XIR_STRING, child_blocks, 1, child, 6, NULL, 0},
        {"loop", 4, parameters, 2, XR_XIR_UNIT, loop_blocks, 2, loop, 4, NULL, 0}
    };
    const XrXirModule built = {XR_XIR_BUILT, functions, 3, NULL};
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_lower(checked, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    return lowered;
}
#endif
