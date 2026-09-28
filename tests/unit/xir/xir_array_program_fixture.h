/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_program_fixture.h - Independent Array programs with all root kinds
 *
 * KEY CONCEPT:
 *   Shared module values diverge only at mutation; temporary build storage dies
 *   before either backend consumes the verified program.
 */
#ifndef XIR_ARRAY_PROGRAM_FIXTURE_H
#define XIR_ARRAY_PROGRAM_FIXTURE_H
#include "xir/xxir_types.h"
#include "xir/xxir_generic.h"
#include "xir_nominal_fixture.h"
static XrXirArtifact *array_program_fixture(bool fail_init, bool nominal) {
    XrXirType a = (XrXirType) (nominal ? 257 : 256), cell = (XrXirType) (nominal ? 258 : 257);
    XrXirTypeNode nodes[4] = {
        {XR_XIR_TYPE_ARRAY, XR_XIR_STRING, NULL, 0, XR_XIR_UNIT, 0, 0, {0}},
        {XR_XIR_TYPE_CELL, a, NULL, 0, XR_XIR_UNIT, 0, 0, {0}}};
    NominalFixture f; nominal_fixture(&f);
    XrXirType argument = XR_XIR_STRING;
    if (nominal) {
        nodes[2] = nodes[1]; nodes[1] = nodes[0];
        nodes[0].element = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
        nodes[0].parameter_span = 1;
        nodes[3] = (XrXirTypeNode) {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0,
            XR_XIR_UNIT, 0, 0, {0, &argument, 1, NULL, 0}};
        f.declarations[0].module = (XrXirLiteral) {"root", 4};
        f.declarations[1].module = (XrXirLiteral) {"library", 7};
        f.fields[0].type = (XrXirType) 256;
    }
    XrXirTypes types = {nodes, nominal ? 4 : 2, nominal ? &f.table : NULL};
    XrXirInstruction init[] = {
        {XR_XIR_CALL, a, {0}, {0}, 3, {0}},
        {XR_XIR_SLOT_INIT, XR_XIR_UNIT, {0}, {0}, 1, {0}},
        {XR_XIR_SLOT_INIT, XR_XIR_UNIT, {0}, {0}, 2, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirInstruction lib[] = {
        {XR_XIR_CONST_STRING, XR_XIR_STRING, {0}, {0}, 0, {0}},
        {XR_XIR_CONST_STRING, XR_XIR_STRING, {0}, {0}, 1, {0}},
        {XR_XIR_ARRAY_NEW, a, {0, 2}, {0}, 0, {0}},
        {XR_XIR_SLOT_INIT, XR_XIR_UNIT, {2}, {0}, 0, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, fail_init ? -1 : 0, {0}},
        {XR_XIR_ARRAY_GET, XR_XIR_STRING, {2, 4}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    uint32_t lib_operands[] = {0, 1};
    XrXirInstruction main[] = {
        {XR_XIR_SLOT_PLACE, a, {0}, {0}, 1, {0}},
        {XR_XIR_SLOT_PLACE, a, {0}, {0}, 2, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 1, {0}},
        {XR_XIR_ARRAY_GET, XR_XIR_STRING, {0, 2}, {0}, 0, {0}},
        {XR_XIR_PRINT, XR_XIR_UNIT, {0, 1}, {0}, 0, {0}},
        {XR_XIR_ARRAY_GET, XR_XIR_STRING, {1, 3}, {0}, 0, {0}},
        {XR_XIR_PRINT, XR_XIR_UNIT, {1, 1}, {0}, 0, {0}},
        {XR_XIR_ARRAY_LEN, XR_XIR_I64, {0}, {0}, 0, {0}},
        {XR_XIR_PRINT, XR_XIR_UNIT, {2, 1}, {0}, 0, {0}},
        {XR_XIR_CONST_STRING, XR_XIR_STRING, {0}, {0}, 2, {0}},
        {XR_XIR_ARRAY_SET, XR_XIR_UNIT, {3, 3}, {0}, 0, {0}},
        {XR_XIR_ARRAY_GET, XR_XIR_STRING, {1, 3}, {0}, 0, {0}},
        {XR_XIR_PRINT, XR_XIR_UNIT, {6, 1}, {0}, 0, {0}},
        {XR_XIR_ARRAY_GET, XR_XIR_STRING, {0, 3}, {0}, 0, {0}},
        {XR_XIR_PRINT, XR_XIR_UNIT, {7, 1}, {0}, 0, {0}},
        {XR_XIR_ARRAY_PUSH, XR_XIR_UNIT, {1, 12}, {0}, 0, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 2, {0}},
        {XR_XIR_ARRAY_GET, XR_XIR_STRING, {1, 17}, {0}, 0, {0}},
        {XR_XIR_PRINT, XR_XIR_UNIT, {8, 1}, {0}, 0, {0}},
        {XR_XIR_ARRAY_LEN, XR_XIR_I64, {1}, {0}, 0, {0}},
        {XR_XIR_PRINT, XR_XIR_UNIT, {9, 1}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {20}, {0}, 0, {0}}};
    uint32_t main_operands[] = {4, 6, 8, 1, 3, 10, 12, 14, 18, 20};
    XrXirInstruction make[] = {{XR_XIR_SLOT_LOAD, a, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirInstruction escape[] = {{XR_XIR_SLOT_LOAD, a, {0}, {0}, 2, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirInstruction bounds[] = {{XR_XIR_SLOT_PLACE, a, {0}, {0}, 2, {0}},
        {XR_XIR_ARRAY_GET, XR_XIR_STRING, {1, 0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2}, {0}, 0, {0}}};
    XrXirInstruction local[] = {
        {XR_XIR_SLOT_LOAD, a, {0}, {0}, 1, {0}},
        {XR_XIR_LOCAL_NEW, a, {0}, {0}, 0, {0}},
        {XR_XIR_CONST_STRING, XR_XIR_STRING, {0}, {0}, 2, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}},
        {XR_XIR_ARRAY_SET, XR_XIR_UNIT, {0, 3}, {0}, 0, {0}},
        {XR_XIR_ARRAY_GET, XR_XIR_STRING, {1, 3}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {5}, {0}, 0, {0}}};
    uint32_t local_operands[] = {1, 3, 2};
    XrXirInstruction captured[] = {
        {XR_XIR_SLOT_LOAD, a, {0}, {0}, 1, {0}},
        {XR_XIR_CELL_NEW, cell, {0}, {0}, 0, {0}},
        {XR_XIR_CELL_PLACE, a, {1}, {0}, 0, {0}},
        {XR_XIR_CONST_STRING, XR_XIR_STRING, {0}, {0}, 2, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}},
        {XR_XIR_ARRAY_SET, XR_XIR_UNIT, {0, 3}, {0}, 0, {0}},
        {XR_XIR_ARRAY_GET, XR_XIR_STRING, {2, 4}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {6}, {0}, 0, {0}}};
    uint32_t captured_operands[] = {2, 4, 3};
    XrXirBlock blocks[] = {{0,4},{0,7},{0,23},{0,2},{0,3},{0,8}};
    XrXirType index_type = XR_XIR_I64;
    XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,&blocks[0],1,init,4,NULL,0},
        {"lib",3,NULL,0,XR_XIR_UNIT,&blocks[1],1,lib,7,lib_operands,2},
        {"main",4,NULL,0,XR_XIR_I64,&blocks[2],1,main,23,main_operands,10},
        {"make",4,NULL,0,a,&blocks[3],1,make,2,NULL,0},
        {"escape",6,NULL,0,a,&blocks[3],1,escape,2,NULL,0},
        {"bounds",6,&index_type,1,XR_XIR_STRING,&blocks[4],1,bounds,3,NULL,0},
        {"local",5,NULL,0,XR_XIR_STRING,&blocks[1],1,local,7,local_operands,3},
        {"captured",8,NULL,0,XR_XIR_STRING,&blocks[5],1,captured,8,captured_operands,3}};
    uint32_t dependency = 1;
    XrXirSourceModule modules[] = {{"root",4,&dependency,1,0},{"library",7,NULL,0,1}};
    XrXirFunctionIdentity identities[] = {{0,0, 0, 0},{1,0, 0, 0},{0,0, 0, 0},{1,1, 0, 0},{0,1, 0, 0},{0,1, 0, 0},{0,1, 0, 0},{0,1, 0, 0}};
    XrXirSlot slots[] = {{1,a,0},{0,a,1},{0,a,1}};
    XrXirLiteral literals[] = {{"red",3},{"blue",4},{"green",5}};
    XrXirDeclarations declarations = {modules,2,identities,slots,3,literals,3,0,2};
    XrXirModule built = {XR_XIR_BUILT,functions,8,&declarations,NULL,&types, NULL};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_check(&built,NULL,&checked,&diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr,"Array check %u function %u instruction %u\n",
        (unsigned) status, diagnostic.function, diagnostic.instruction);
    CHECK(status == XR_XIR_OK);
    if (nominal) {
        XrXirArtifact *specialized = NULL;
        CHECK(xr_xir_specialize(checked, NULL, &specialized, NULL) == XR_XIR_OK);
        xr_xir_artifact_free(checked); checked = specialized;
    }
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(checked,&target,NULL,&lowered,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    return lowered;
}
#endif // XIR_ARRAY_PROGRAM_FIXTURE_H
