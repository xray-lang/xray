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
        {XR_XIR_CELL_NEW,cell,{4},{0},0,{0}},
        {XR_XIR_SLOT_INIT,XR_XIR_UNIT,{5},{0},0,{0}},
        {XR_XIR_SLOT_INIT,XR_XIR_UNIT,{4},{0},1,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    uint32_t constructed[] = {0,1,2,3};
    uint32_t root_id = kind == 1 ? 2 : 4;
    XrXirInstruction entry[] = {
        {XR_XIR_SLOT_LOAD,cell,{0},{0},0,{0}},
        {XR_XIR_CELL_READ,root,{0},{0},0,{0}},
        {XR_XIR_LOCAL_NEW,root,{1},{0},0,{0}},
        {XR_XIR_CELL_NEW,cell,{1},{0},0,{0}},
        {XR_XIR_CELL_PLACE,root,{0},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_INDEX_PLACE,pair,{root_id,5},{0},0,{0}},
        {XR_XIR_FIELD_PLACE,items,{6},{0},0,{0}},
        {XR_XIR_INDEX_PLACE,XR_XIR_I64,{7,5},{0},0,{0}},
        {XR_XIR_CALL,root,{0},{0},2,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},99,{0}},
        {XR_XIR_PLACE_WRITE,XR_XIR_UNIT,{8,11},{0},0,{0}},
        {XR_XIR_PLACE_READ,XR_XIR_I64,{8},{0},0,{0}},
        {XR_XIR_ARRAY_GET,XR_XIR_I64,{7,5},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},84,{0}},
        {XR_XIR_ARRAY_SET,XR_XIR_UNIT,{0,3},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},61,{0}},
        {XR_XIR_ARRAY_PUSH,XR_XIR_UNIT,{7,17},{0},0,{0}},
        {XR_XIR_ARRAY_LEN,XR_XIR_I64,{7},{0},0,{0}},
        {XR_XIR_ARRAY_GET,XR_XIR_I64,{7,5},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},1,{0}},
        {XR_XIR_ARRAY_GET,XR_XIR_I64,{7,21},{0},0,{0}},
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},2,{0}},
        {XR_XIR_STRUCT_SET,XR_XIR_UNIT,{6,23},{0},1,{0}},
        {XR_XIR_FIELD_PLACE,XR_XIR_STRING,{6},{0},1,{0}},
        {XR_XIR_PLACE_READ,XR_XIR_STRING,{25},{0},0,{0}},
        {XR_XIR_SLOT_LOAD,root,{0},{0},1,{0}},
        {XR_XIR_ARRAY_GET,pair,{27,5},{0},0,{0}},
        {XR_XIR_STRUCT_GET,items,{28},{0},0,{0}},
        {XR_XIR_ARRAY_GET,XR_XIR_I64,{29,5},{0},0,{0}},
        {XR_XIR_PRINT,XR_XIR_UNIT,{3,7},{0},0,{0}},
        {XR_XIR_PLACE_READ,root,{root_id},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{32},{0},0,{0}}};
    if (kind == 1) entry[10] = (XrXirInstruction){XR_XIR_LOCAL_WRITE,XR_XIR_UNIT,{2,9},{0},0,{0}};
    if (kind == 2) {
        entry[4] = (XrXirInstruction){XR_XIR_CELL_PLACE,root,{3},{0},0,{0}};
        entry[10] = (XrXirInstruction){XR_XIR_CELL_WRITE,XR_XIR_UNIT,{3,9},{0},0,{0}};
    }
    uint32_t values[] = {7,5,15,13,14,20,22,19,30,26};
    XrXirInstruction replace[] = {
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},73,{0}},
        {XR_XIR_ARRAY_NEW,items,{0,1},{0},0,{0}},
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},1,{0}},
        {XR_XIR_STRUCT_NEW,pair,{1,2},{0},0,{0}},
        {XR_XIR_ARRAY_NEW,root,{3,1},{0},0,{0}},
        {XR_XIR_SLOT_LOAD,cell,{0},{0},0,{0}},
        {XR_XIR_CELL_WRITE,XR_XIR_UNIT,{5,4},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{4},{0},0,{0}}};
    XrXirInstruction read[] = {{XR_XIR_SLOT_LOAD,cell,{0},{0},0,{0}},
        {XR_XIR_CELL_READ,root,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    XrXirInstruction main[] = {{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction bounds[] = {
        {XR_XIR_SLOT_LOAD,cell,{0},{0},0,{0}},
        {XR_XIR_CELL_PLACE,root,{1},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_INDEX_PLACE,pair,{2,3},{0},0,{0}},
        {XR_XIR_FIELD_PLACE,items,{4},{0},0,{0}},
        {XR_XIR_INDEX_PLACE,XR_XIR_I64,{5,0},{0},0,{0}},
        {XR_XIR_PLACE_READ,XR_XIR_I64,{6},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{7},{0},0,{0}}};
    XrXirInstruction bad_write[8]; memcpy(bad_write,bounds,sizeof(bounds));
    bad_write[6] = (XrXirInstruction){XR_XIR_PLACE_WRITE,XR_XIR_UNIT,{6,3},{0},0,{0}};
    bad_write[7].args[0] = 3;
    XrXirType index_type = XR_XIR_I64;
    XrXirBlock blocks[] = {{0,9,0,0},{0,34,0,0},{0,8,0,0},{0,2,0,0},{0,3,0,0}};
    XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,&blocks[0],1,init,9,constructed,4},
        {"entry",5,NULL,0,root,&blocks[1],1,entry,34,values,10},
        {"replace",7,NULL,0,root,&blocks[2],1,replace,8,constructed,4},
        {"read",4,NULL,0,root,&blocks[4],1,read,3,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,&blocks[3],1,main,2,NULL,0},
        {"bounds",6,&index_type,1,XR_XIR_I64,&blocks[2],1,bounds,8,NULL,0},
        {"badWrite",8,&index_type,1,XR_XIR_I64,&blocks[2],1,bad_write,8,NULL,0}};
    XrXirSourceModule source = {"alpha",5,NULL,0,0};
    XrXirFunctionIdentity identities[] = {{0},{.exported = 1},{0},{.exported = 1},{0},
        {.exported = 1},{.exported = 1}};
    XrXirSlot slots[] = {{0,cell,1},{0,root,0}};
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
