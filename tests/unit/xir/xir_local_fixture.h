/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_local_fixture.h - Owned local snapshots across a branch and suspension
 *
 * KEY CONCEPT:
 *   A local place can be updated without changing an earlier read.
 */
#ifndef XIR_LOCAL_FIXTURE_H
#define XIR_LOCAL_FIXTURE_H
static XrXirArtifact *local_fixture(void) {
    const XrXirType parameters[] = {XR_XIR_STRING, XR_XIR_STRING, XR_XIR_BOOL};
    const XrXirInstruction ops[] = {
        {XR_XIR_LOCAL_NEW, XR_XIR_STRING, {0}, {0}, 0},
        {XR_XIR_LOCAL_READ, XR_XIR_STRING, {3}, {0}, 0},
        {XR_XIR_BRANCH, XR_XIR_UNIT, {2}, {1, 2}, 0},
        {XR_XIR_LOCAL_WRITE, XR_XIR_UNIT, {3, 1}, {0}, 0},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {3}, 0},
        {XR_XIR_LOCAL_WRITE, XR_XIR_UNIT, {3, 4}, {0}, 0},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {3}, 0},
        {XR_XIR_SUSPEND, XR_XIR_UNIT, {0}, {0}, 0},
        {XR_XIR_LOCAL_READ, XR_XIR_STRING, {3}, {0}, 0},
        {XR_XIR_CONCAT_STRING, XR_XIR_STRING, {4, 11}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {12}, {0}, 0}
    };
    const XrXirBlock blocks[] = {{0, 3}, {3, 2}, {5, 2}, {7, 4}};
    const XrXirFunction function = {"local", 5, parameters, 3, XR_XIR_STRING, blocks, 4, ops, 11, NULL, 0};
    const XrXirType fault_parameters[] = {XR_XIR_STRING, XR_XIR_STRING, XR_XIR_I64};
    const XrXirInstruction fault_ops[] = {
        {XR_XIR_CONCAT_STRING, XR_XIR_STRING, {0, 1}, {0}, 0},
        {XR_XIR_LOCAL_NEW, XR_XIR_STRING, {3}, {0}, 0},
        {XR_XIR_CONST_I64, XR_XIR_I64, {0}, {0}, 7},
        {XR_XIR_DIV_I64, XR_XIR_I64, {5, 2}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {6}, {0}, 0}
    };
    const XrXirBlock fault_block = {0, 5};
    const XrXirInstruction phi_ops[] = {
        {XR_XIR_CONST_I64, XR_XIR_I64, {0}, {0}, 0},
        {XR_XIR_CONST_I64, XR_XIR_I64, {0}, {0}, 1},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {1}, 0},
        {XR_XIR_PHI, XR_XIR_STRING, {0, 4}, {0}, 0},
        {XR_XIR_PHI, XR_XIR_STRING, {4, 4}, {0}, 0},
        {XR_XIR_PHI, XR_XIR_I64, {8, 4}, {0}, 0},
        {XR_XIR_LT_I64, XR_XIR_BOOL, {8, 2}, {0}, 0},
        {XR_XIR_BRANCH, XR_XIR_UNIT, {9}, {2, 3}, 0},
        {XR_XIR_SUSPEND, XR_XIR_UNIT, {0}, {0}, 0},
        {XR_XIR_ADD_I64, XR_XIR_I64, {8, 4}, {0}, 0},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {1}, 0},
        {XR_XIR_CONCAT_STRING, XR_XIR_STRING, {6, 7}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {14}, {0}, 0}
    };
    const uint32_t phi_inputs[] = {0, 0, 2, 7, 0, 1, 2, 6, 0, 3, 2, 12};
    const XrXirBlock phi_blocks[] = {{0, 3}, {3, 5}, {8, 3}, {11, 2}};
    const XrXirFunction functions[] = {function,
        {"fault", 5, fault_parameters, 3, XR_XIR_I64, &fault_block, 1, fault_ops, 5, NULL, 0},
        {"swap", 4, fault_parameters, 3, XR_XIR_STRING, phi_blocks, 4, phi_ops, 13, phi_inputs, 12}};
    const XrXirModule built = {XR_XIR_BUILT, functions, 3, NULL, NULL, NULL};
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    return checked;
}
#endif
