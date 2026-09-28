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
#include "xir/xxir_generic.h"
#include "xir/xxir_checked.h"
#include "xir_error_fixture.h"

static XrXirArtifact *call_fixture(uint32_t mode) {
    bool guarded = mode >= 4, rethrow = mode >= 8 && mode < 10, unit = mode >= 10 && mode < 12, generic = mode >= 12;
    mode = generic ? mode - 12 : unit ? mode - 10 : mode % 4;
    const uint32_t operands[] = {0, 1};
    const XrXirType params[] = {XR_XIR_I64, XR_XIR_I64};
    const XrXirBlock root_block = {0, 2};
    const XrXirBlock sort_blocks[] = {{0, 2}, {2, 1}, {3, 1}};
    const XrXirBlock compare_block = {0, 4};
    const XrXirInstruction root[] = {
        {XR_XIR_CALL, XR_XIR_I64, {0, 2}, {0, 0}, 1, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0, 0}, 0, {0}}
    };
    XrXirInstruction sort[] = {
        {XR_XIR_CALL, XR_XIR_BOOL, {0, 2}, {0, 0}, 2, {0}},
        {XR_XIR_BRANCH, XR_XIR_UNIT, {2, 0}, {1, 2}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1, 0}, {0, 0}, 0, {0}}
    };
    XrXirInstruction compare[] = {
        {XR_XIR_SUSPEND, XR_XIR_UNIT, {0}, {0}, 0, {0}},
        {XR_XIR_LT_INT, XR_XIR_BOOL, {0,1}, {0}, 0, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 91, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {3}, {0}, 0, {0}}
    };
    const uint32_t error_operand=3;
    if (mode == 1) {
        compare[1]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},91, {0}};
        compare[2]=(XrXirInstruction){XR_XIR_ENUM_NEW,(XrXirType)256,{0,1},{0},0, {0}};
        compare[3]=(XrXirInstruction){XR_XIR_THROW,XR_XIR_UNIT,{4},{0},0, {0}};
    } else if (mode == 2) {
        compare[0]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0, {0}};
        compare[1]=(XrXirInstruction){XR_XIR_DIV_INT,XR_XIR_I64,{0,2},{0},0, {0}};
        compare[2]=(XrXirInstruction){XR_XIR_CONST_BOOL,XR_XIR_BOOL,{0},{0},0, {0}};
        compare[3].args[0]=4;
    } else if (mode == 3) compare[3]=(XrXirInstruction){XR_XIR_MATCH_FAIL,XR_XIR_UNIT,{0},{0},0, {0}};
    XrXirInstruction init={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0, {0}};
    XrXirInstruction entry[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0, {0}},{XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0, {0}}};
    XrXirBlock init_block={0,1};
    XrXirFunction functions[] = {
        {"entry", 5, params, 2, XR_XIR_I64, &root_block, 1, root, 2, operands, 2},
        {"minimum", 7, params, 2, XR_XIR_I64, sort_blocks, 3, sort, 4, operands, 2},
        {"compare", 7, params, 2, XR_XIR_BOOL, &compare_block, 1, compare, 4, mode == 1 ? &error_operand : NULL, mode == 1 ? 1u : 0u},
        {"init",4,NULL,0,XR_XIR_UNIT,&init_block,1,&init,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,&root_block,1,entry,2,NULL,0}
    };
    XrXirInstruction invoke[] = {
        {XR_XIR_INVOKE,XR_XIR_I64,{0,2},{1,2},1,{0}},
        {XR_XIR_INVOKE_RESULT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{3},{0},0,{0}},
        {XR_XIR_INVOKE_ERROR,XR_XIR_ERROR,{0},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},77,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{6},{0},0,{0}},
        {XR_XIR_THROW,XR_XIR_UNIT,{5},{0},0,{0}}};
    XrXirBlock guarded_blocks[] = {{0,1},{1,2},{3,3}};
    if (guarded) {
        functions[0].instructions = invoke; functions[0].instruction_count = 6;
        functions[0].blocks = guarded_blocks; functions[0].block_count = 3;
        if (unit) {
            functions[1].result = XR_XIR_UNIT; sort[2].args[0] = sort[3].args[0] = 0;
            invoke[0].type = XR_XIR_UNIT;
            invoke[1] = (XrXirInstruction) {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},44,{0}};
        }
        if (rethrow) {
            invoke[4] = (XrXirInstruction) {XR_XIR_SUSPEND,XR_XIR_UNIT,{0},{0},0,{0}};
            invoke[5] = (XrXirInstruction) {XR_XIR_CALL,XR_XIR_I64,{0},{0},4,{0}};
            guarded_blocks[2].count = 4; functions[0].instruction_count = 7;
        }
    }
    XrXirType template_parameters[] = {(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,(XrXirType)XR_XIR_TYPE_PARAMETER_BASE};
    XrXirType concrete = XR_XIR_I64; uint32_t constraint = XR_XIR_CONSTRAINT_SENDABLE;
    XrXirGeneric generics[] = {{NULL,0,&concrete,1},{&constraint,1,NULL,0},{0},{0},{0}};
    XrXirInstruction identity[] = {{XR_XIR_SUSPEND,XR_XIR_UNIT,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    XrXirBlock generic_block = {0,mode ? 4u : 2u};
    if (generic) {
        invoke[0].type_arguments[1] = 1;
        functions[1].parameters = template_parameters; functions[1].result = template_parameters[0];
        functions[1].blocks = &generic_block; functions[1].block_count = 1;
        functions[1].instructions = mode ? compare : identity; functions[1].instruction_count = generic_block.count;
        functions[1].operands = mode ? &error_operand : NULL; functions[1].operand_count = mode ? 1u : 0u;
    }
    ErrorFixture error; error_fixture_init(&error,false);
    const XrXirSourceModule source={"alpha",5,NULL,0,3};
    const XrXirFunctionIdentity ids[5]={{0}};
    const XrXirDeclarations declarations={&source,1,ids,NULL,0,NULL,0,0,4};
    const XrXirModule module = {XR_XIR_BUILT, functions, 5, &declarations, generic ? generics : NULL, &error.types, NULL};
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_check(&module,NULL,&checked,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"call fixture mode=%u status=%u function=%u block=%u instruction=%u\n",
        mode,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL)==XR_XIR_OK);
    xr_xir_artifact_free(checked); checked=NULL;
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&checked,NULL)==XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    XrXirArtifact *closed=NULL;
    CHECK(xr_xir_specialize(checked,NULL,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    xr_xir_artifact_free(checked);
    return lowered;
}
#endif // XIR_CALL_FIXTURE_H
