/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_call_fixture.h - Real XIR call and suspension graphs
 *
 * KEY CONCEPT:
 *   The caller and comparator execute instructions; native substitution only
 *   replaces a typed callee body in a trusted test table.
 */
#ifndef XIR_CALL_FIXTURE_H
#define XIR_CALL_FIXTURE_H
#include "xir/xxir.h"

static XrXirArtifact *call_fixture(uint32_t mode) {
    const uint32_t operands[] = {0, 1};
    const XrXirType params[] = {XR_XIR_I64, XR_XIR_I64};
    const XrXirBlock root_block = {0, 2};
    const XrXirBlock sort_blocks[] = {{0, 2}, {2, 1}, {3, 1}};
    const XrXirBlock compare_block = {0, 3};
    const XrXirInstruction root[] = {
        {XR_XIR_CALL, XR_XIR_I64, {0, 2}, {0, 0}, 1},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0, 0}, 0}
    };
    const XrXirInstruction sort[] = {
        {XR_XIR_CALL, XR_XIR_BOOL, {0, 2}, {0, 0}, 2},
        {XR_XIR_BRANCH, XR_XIR_UNIT, {2, 0}, {1, 2}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1, 0}, {0, 0}, 0}
    };
    XrXirInstruction compare[] = {
        {XR_XIR_SUSPEND, XR_XIR_UNIT, {0, 0}, {0, 0}, 0},
        {XR_XIR_LT_I64, XR_XIR_BOOL, {0, 1}, {0, 0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {3, 0}, {0, 0}, 0}
    };
    if (mode == 1) {
        compare[1] = (XrXirInstruction) {XR_XIR_CONST_I64, XR_XIR_I64, {0, 0}, {0, 0}, 91};
        compare[2] = (XrXirInstruction) {XR_XIR_THROW, XR_XIR_UNIT, {3, 0}, {0, 0}, 0};
    } else if (mode == 2) {
        compare[0] = (XrXirInstruction) {XR_XIR_CONST_I64, XR_XIR_I64, {0, 0}, {0, 0}, INT64_MAX};
        compare[1] = (XrXirInstruction) {XR_XIR_ADD_I64, XR_XIR_I64, {2, 0}, {0, 0}, 0};
        compare[2] = (XrXirInstruction) {XR_XIR_THROW, XR_XIR_UNIT, {3, 0}, {0, 0}, 0};
    }
    const XrXirFunction functions[] = {
        {"entry", 5, params, 2, XR_XIR_I64, &root_block, 1, root, 2, operands, 2},
        {"minimum", 7, params, 2, XR_XIR_I64, sort_blocks, 3, sort, 4, operands, 2},
        {"compare", 7, params, 2, XR_XIR_BOOL, &compare_block, 1, compare, 3, NULL, 0}
    };
    const XrXirModule module = {XR_XIR_BUILT, functions, 3, NULL};
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_check(&module, NULL, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_lower(checked, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    return lowered;
}
#endif // XIR_CALL_FIXTURE_H
