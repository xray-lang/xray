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
#include "xir/xxir_generic.h"
#include "xir_error_fixture.h"
static XrXirArtifact *string_fixture(uint32_t mode) {
    const uint32_t operands[] = {0, 1, 0, 1};
    const XrXirType parameters[] = {XR_XIR_STRING, XR_XIR_STRING, XR_XIR_STRING, XR_XIR_STRING};
    XrXirInstruction root[] = {
        {XR_XIR_CALL, XR_XIR_STRING, {0, 4}, {0, 0}, 1, {0}},
        {XR_XIR_COPY, XR_XIR_STRING, {2, 0}, {0, 0}, 0, {0}},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {3, 0}, {0, 0}, 1, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {3, 0}, {0, 0}, 0, {0}}
    };
    XrXirInstruction child[] = {
        {XR_XIR_COPY, XR_XIR_STRING, {2, 0}, {0, 0}, 0, {0}},
        {XR_XIR_CONCAT_STRING, XR_XIR_STRING, {4, 3}, {0, 0}, 0, {0}},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {5, 0}, {0, 0}, 2, {0}},
        {XR_XIR_SUSPEND, XR_XIR_UNIT, {0, 0}, {0, 0}, 0, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0, 0}, {0, 0}, 91, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {5, 0}, {0, 0}, 0, {0}}
    };
    XrXirInstruction loop[] = {
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {1, 0}, 0, {0}},
        {XR_XIR_CONCAT_STRING, XR_XIR_STRING, {0, 1}, {0, 0}, 0, {0}},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {3, 0}, {0, 0}, 1, {0}},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {1, 0}, 0, {0}}
    };
    const uint32_t error_operand=5;
    if (mode) {
        child[4]=(XrXirInstruction){XR_XIR_ENUM_NEW,(XrXirType)256,{0,1},{0},1, {0}};
        child[5]=(XrXirInstruction){XR_XIR_THROW,XR_XIR_UNIT,{8},{0},0, {0}};
    }
    XrXirInstruction init={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0, {0}};
    XrXirInstruction entry[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0, {0}},{XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0, {0}}};
    const XrXirBlock init_block={0,1, 0, 0},entry_block={0,2, 0, 0};
    const XrXirBlock root_blocks[] = {{0, 4, 0, 0}}, child_blocks[] = {{0, 6, 0, 0}}, loop_blocks[] = {{0, 1, 0, 0}, {1, 3, 0, 0}};
    const XrXirFunction functions[] = {
        {"root", 4, parameters, 2, XR_XIR_STRING, root_blocks, 1, root, 4, operands, 4},
        {"child", 5, parameters, 4, XR_XIR_STRING, child_blocks, 1, child, 6, mode ? &error_operand : NULL, mode ? 1u : 0u},
        {"loop", 4, parameters, 2, XR_XIR_UNIT, loop_blocks, 2, loop, 4, NULL, 0},
        {"init",4,NULL,0,XR_XIR_UNIT,&init_block,1,&init,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,&entry_block,1,entry,2,NULL,0}
    };
    ErrorFixture error; error_fixture_init(&error,false);
    const XrXirSourceModule source={"alpha",5,NULL,0,3};
    const XrXirFunctionIdentity ids[5]={{0}};
    const XrXirDeclarations declarations={&source,1,ids,NULL,0,NULL,0,0,4, NULL};
    const XrXirModule built = {XR_XIR_BUILT, functions, 5, &declarations, NULL, mode ? &error.types : NULL, NULL};
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    XrXirArtifact *closed=NULL; CHECK(xr_xir_specialize(checked,NULL,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    xr_xir_artifact_free(checked);
    return lowered;
}
#endif
