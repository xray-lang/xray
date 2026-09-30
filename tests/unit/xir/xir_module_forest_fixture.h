/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_module_forest_fixture.h - Checked declarations outside the execution root
 */
#ifndef XIR_MODULE_FOREST_FIXTURE_H
#define XIR_MODULE_FOREST_FIXTURE_H
#include "xir/xxir_generic.h"
static XrXirArtifact *module_forest_fixture(bool invalid) {
    XrXirInstruction extra[] = {
        {XR_XIR_CONST_STRING, XR_XIR_STRING, {0}, {0}, 0, {0}},
        {XR_XIR_SLOT_INIT, XR_XIR_UNIT, {0}, {0}, 0, {0}},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {0}, {0}, 1, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}
    };
    if (invalid) extra[3].op = XR_XIR_OP_COUNT;
    const XrXirInstruction root[] = {
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 22, {0}},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {0}, {0}, 1, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}
    };
    XrXirInstruction leaf[3]; memcpy(leaf, root, sizeof(leaf)); leaf[0].immediate = 11;
    const XrXirInstruction answer[] = {
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 41, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}
    };
    const XrXirBlock blocks[] = {{0, 4, 0, 0}, {0, 3, 0, 0}, {0, 2, 0, 0}};
    const XrXirType callable = (XrXirType)256;
    const XrXirTypeNode signature = {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT, NULL, 0, XR_XIR_I64, 0, 0, {0}};
    const XrXirTypes types = {&signature, 1, NULL, NULL};
    const XrXirInstruction bind[] = {
        {XR_XIR_FUNCTION_REF, (XrXirType)256, {0}, {0}, 5, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}
    };
    const XrXirInstruction invoke[] = {
        {XR_XIR_CALL_INDIRECT, XR_XIR_I64, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0, {0}}
    };
    const XrXirFunction functions[] = {
        {"extra_init", 10, NULL, 0, XR_XIR_UNIT, &blocks[0], 1, extra, 4, NULL, 0},
        {"root_init", 9, NULL, 0, XR_XIR_UNIT, &blocks[1], 1, root, 3, NULL, 0},
        {"leaf_init", 9, NULL, 0, XR_XIR_UNIT, &blocks[1], 1, leaf, 3, NULL, 0},
        {"main", 4, NULL, 0, XR_XIR_I64, &blocks[2], 1, answer, 2, NULL, 0},
        {"extra_call", 10, NULL, 0, XR_XIR_I64, &blocks[2], 1, answer, 2, NULL, 0},
        {"leaf_call", 9, NULL, 0, XR_XIR_I64, &blocks[2], 1, answer, 2, NULL, 0},
        {"bind_leaf", 9, NULL, 0, (XrXirType)256, &blocks[2], 1, bind, 2, NULL, 0},
        {"invoke_leaf", 11, &callable, 1, XR_XIR_I64, &blocks[2], 1, invoke, 2, NULL, 0}
    };
    const uint32_t dependency = 2;
    const XrXirSourceModule modules[] = {
        {"extra", 5, NULL, 0, 0}, {"root", 4, &dependency, 1, 1}, {"leaf", 4, NULL, 0, 2}
    };
    const XrXirFunctionIdentity identities[] = {
        {0}, {1,0,0,0,0,0, XR_XIR_NON_MEMBER}, {2,0,0,0,0,0, XR_XIR_NON_MEMBER}, {1,0,0,0,0,0, XR_XIR_NON_MEMBER}, {0,1,0,0,0,0, XR_XIR_NON_MEMBER}, {2,1,0,0,0,0, XR_XIR_NON_MEMBER},
        {1,1,0,0,0,0, XR_XIR_NON_MEMBER}, {1,1,0,0,0,0, XR_XIR_NON_MEMBER}
    };
    const XrXirSlot slot = {0, XR_XIR_STRING, 0};
    const XrXirLiteral literal = {"must not initialize", 19};
    const XrXirDeclarations declarations = {modules, 3, identities, &slot, 1, &literal, 1, 1, 3, NULL};
    const XrXirModule built = {XR_XIR_BUILT, functions, 8, &declarations, NULL, &types, NULL};
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_check(&built, NULL, &checked, &diagnostic);
    if (invalid) { CHECK(status != XR_XIR_OK && !checked); return NULL; }
    if (status != XR_XIR_OK) fprintf(stderr, "forest: status=%u function=%u block=%u instruction=%u reason=%u\n",
        status, diagnostic.function, diagnostic.block, diagnostic.instruction, diagnostic.reason);
    CHECK(status == XR_XIR_OK);
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed); xr_xir_artifact_free(checked);
    return lowered;
}
#endif // XIR_MODULE_FOREST_FIXTURE_H
