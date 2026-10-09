/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_path_program_fixture.h - Projected mutation after logical root replacement
 */
#ifndef XIR_PATH_PROGRAM_FIXTURE_H
#define XIR_PATH_PROGRAM_FIXTURE_H
#include "xir_construction_fixture.h"
#include "xir_nominal_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
static XrXirArtifact *path_program_fixture(const XrXirCompileContext *context, unsigned kind) {
    const XrXirType items = (XrXirType)256, pair = (XrXirType)257;
    const XrXirType root = (XrXirType)258, cell = (XrXirType)259;
    NominalFixture nominal; nominal_fixture(&nominal); nominal.table.count = 1;
    nominal.fields[1].flags = XR_XIR_FIELD_MUTABLE;
    XrXirType argument = items;
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {0,&argument,1,NULL,0}},
        {.kind = XR_XIR_TYPE_ARRAY, .element = pair},
        {.kind = XR_XIR_TYPE_CELL, .element = root}};
    XrXirTypes types = {nodes,4,&nominal.table, NULL};
    XrXirInstruction init[] = {
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},11,{0}},
        {XR_XIR_ARRAY_NEW,items,{0,1},{0},0,{0}},
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},0,{0}},
        {XR_XIR_STRUCT_NEW,pair,{1,2},{0},0,{0}},
        {XR_XIR_ARRAY_NEW,root,{3,1},{0},0,{0}},
        {XR_XIR_SLOT_INIT,XR_XIR_UNIT,{4},{0},0,{0}},
        {XR_XIR_SLOT_INIT,XR_XIR_UNIT,{4},{0},1,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    uint32_t constructed[] = {0,1,2,3};
    uint32_t root_id = kind == 1 ? 1 : 3;
    XrXirInstruction entry[] = {
        {XR_XIR_SLOT_LOAD,root,{0},{0},0,{0}},
        {XR_XIR_LOCAL_NEW,root,{0},{0},0,{0}},
        {XR_XIR_CELL_NEW,cell,{0},{0},0,{0}},
        {XR_XIR_SLOT_PLACE,root,{0},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_INDEX_PLACE,pair,{root_id,4},{0},0,{0}},
        {XR_XIR_FIELD_PLACE,items,{5},{0},0,{0}},
        {XR_XIR_INDEX_PLACE,XR_XIR_I64,{6,4},{0},0,{0}},
        {XR_XIR_CALL,root,{0},{0},2,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},99,{0}},
        {XR_XIR_PLACE_WRITE,XR_XIR_UNIT,{7,10},{0},0,{0}},
        {XR_XIR_PLACE_READ,XR_XIR_I64,{7},{0},0,{0}},
        {XR_XIR_ARRAY_GET,XR_XIR_I64,{6,4},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},84,{0}},
        {XR_XIR_ARRAY_SET,XR_XIR_UNIT,{0,3},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},61,{0}},
        {XR_XIR_ARRAY_PUSH,XR_XIR_UNIT,{6,16},{0},0,{0}},
        {XR_XIR_ARRAY_LEN,XR_XIR_I64,{6},{0},0,{0}},
        {XR_XIR_ARRAY_GET,XR_XIR_I64,{6,4},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},1,{0}},
        {XR_XIR_ARRAY_GET,XR_XIR_I64,{6,20},{0},0,{0}},
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},2,{0}},
        {XR_XIR_STRUCT_SET,XR_XIR_UNIT,{5,22},{0},1,{0}},
        {XR_XIR_FIELD_PLACE,XR_XIR_STRING,{5},{0},1,{0}},
        {XR_XIR_PLACE_READ,XR_XIR_STRING,{24},{0},0,{0}},
        {XR_XIR_SLOT_LOAD,root,{0},{0},1,{0}},
        {XR_XIR_ARRAY_GET,pair,{26,4},{0},0,{0}},
        {XR_XIR_STRUCT_GET,items,{27},{0},0,{0}},
        {XR_XIR_ARRAY_GET,XR_XIR_I64,{28,4},{0},0,{0}},
        {XR_XIR_PRINT,XR_XIR_UNIT,{3,7},{0},0,{0}},
        {XR_XIR_PLACE_READ,root,{root_id},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{31},{0},0,{0}}};
    if (kind == 1) entry[9] = (XrXirInstruction){XR_XIR_LOCAL_WRITE,XR_XIR_UNIT,{1,8},{0},0,{0}};
    if (kind == 2) {
        entry[3] = (XrXirInstruction){XR_XIR_CELL_PLACE,root,{2},{0},0,{0}};
        entry[9] = (XrXirInstruction){XR_XIR_CELL_WRITE,XR_XIR_UNIT,{2,8},{0},0,{0}};
    }
    uint32_t values[] = {6,4,14,12,13,19,21,18,29,25};
    XrXirInstruction replace[] = {
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},73,{0}},
        {XR_XIR_ARRAY_NEW,items,{0,1},{0},0,{0}},
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},1,{0}},
        {XR_XIR_STRUCT_NEW,pair,{1,2},{0},0,{0}},
        {XR_XIR_ARRAY_NEW,root,{3,1},{0},0,{0}},
        {XR_XIR_SLOT_STORE,XR_XIR_UNIT,{4},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{4},{0},0,{0}}};
    XrXirInstruction read[] = {{XR_XIR_SLOT_LOAD,root,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction main[] = {{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction bounds[] = {
        {XR_XIR_SLOT_PLACE,root,{0},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_INDEX_PLACE,pair,{1,2},{0},0,{0}},
        {XR_XIR_FIELD_PLACE,items,{3},{0},0,{0}},
        {XR_XIR_INDEX_PLACE,XR_XIR_I64,{4,0},{0},0,{0}},
        {XR_XIR_PLACE_READ,XR_XIR_I64,{5},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{6},{0},0,{0}}};
    XrXirInstruction bad_write[7]; memcpy(bad_write,bounds,sizeof(bounds));
    bad_write[5] = (XrXirInstruction){XR_XIR_PLACE_WRITE,XR_XIR_UNIT,{5,2},{0},0,{0}};
    bad_write[6].args[0] = 2;
    XrXirType index_type = XR_XIR_I64;
    XrXirBlock blocks[] = {{0,8,0,0},{0,33,0,0},{0,7,0,0},{0,2,0,0}};
    XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,&blocks[0],1,init,8,constructed,4},
        {"entry",5,NULL,0,root,&blocks[1],1,entry,33,values,10},
        {"replace",7,NULL,0,root,&blocks[2],1,replace,7,constructed,4},
        {"read",4,NULL,0,root,&blocks[3],1,read,2,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,&blocks[3],1,main,2,NULL,0},
        {"bounds",6,&index_type,1,XR_XIR_I64,&blocks[2],1,bounds,7,NULL,0},
        {"badWrite",8,&index_type,1,XR_XIR_I64,&blocks[2],1,bad_write,7,NULL,0}};
    XrXirSourceModule source = {"alpha",5,NULL,0,0};
    XrXirFunctionIdentity identities[] = {{0},{.exported = 1},{0},{.exported = 1},{0},
        {.exported = 1},{.exported = 1}};
    XrXirSlot slots[] = {{0,root,1},{0,root,0}};
    XrXirLiteral literals[] = {{"old",3},{"new",3},{"edited",6}};
    XrXirDeclarations declarations = {&source,1,identities,slots,2,literals,3,0,4, NULL};
    XrXirModule built = {XR_XIR_BUILT,functions,7,&declarations,NULL,&types,NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xir_fixture_check(context, &built, &checked, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr,"Path check %u function %u instruction %u\n",
        (unsigned)status,diagnostic.function,diagnostic.instruction);
    CHECK(status == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);closed=NULL; return lowered;
}
#endif // XIR_PATH_PROGRAM_FIXTURE_H
