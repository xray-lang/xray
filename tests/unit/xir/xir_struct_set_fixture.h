/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_struct_set_fixture.h - Real-root mutation after RHS replacement
 */
#ifndef XIR_STRUCT_SET_FIXTURE_H
#define XIR_STRUCT_SET_FIXTURE_H
#include "xir_construction_fixture.h"
#include "xir_nominal_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
static XrXirArtifact *struct_set_checked(const XrXirCompileContext *context, unsigned invalid) {
    const XrXirType pair = (XrXirType)256, cell = (XrXirType)257;
    XrXirType argument = XR_XIR_I64;
    NominalFixture nominal; nominal_fixture(&nominal); nominal.table.count = 1; nominal.fields[1].flags = 0;
    XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{0,&argument,1,NULL,0}},
        {XR_XIR_TYPE_CELL,pair,NULL,0,XR_XIR_UNIT,0,0,{0}}};
    XrXirTypes types = {nodes,2,&nominal.table, NULL};
    XrXirInstruction init[] = {
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},0, {0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},7, {0}},
        {XR_XIR_STRUCT_NEW,pair,{0,2},{0},0, {0}},
        {XR_XIR_CELL_NEW,cell,{2},{0},0, {0}},
        {XR_XIR_SLOT_INIT,XR_XIR_UNIT,{3},{0},0, {0}},
        {XR_XIR_SLOT_INIT,XR_XIR_UNIT,{2},{0},1, {0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0, {0}}};
    XrXirInstruction entry[] = {
        {XR_XIR_SLOT_LOAD,cell,{0},{0},0, {0}},
        {XR_XIR_CELL_READ,pair,{0},{0},0, {0}},
        {XR_XIR_LOCAL_NEW,pair,{1},{0},0, {0}},
        {XR_XIR_CELL_NEW,cell,{1},{0},0, {0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},23, {0}},
        {XR_XIR_STRUCT_SET,XR_XIR_UNIT,{2,4},{0},0, {0}},
        {XR_XIR_CELL_PLACE,pair,{3},{0},0, {0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},31, {0}},
        {XR_XIR_STRUCT_SET,XR_XIR_UNIT,{6,7},{0},0, {0}},
        {XR_XIR_CELL_PLACE,pair,{0},{0},0, {0}},
        {XR_XIR_CALL,XR_XIR_I64,{0},{0},2, {0}},
        {XR_XIR_STRUCT_SET,XR_XIR_UNIT,{9,10},{0},0, {0}},
        {XR_XIR_LOCAL_READ,pair,{2},{0},0, {0}},
        {XR_XIR_STRUCT_GET,XR_XIR_I64,{12},{0},0, {0}},
        {XR_XIR_CELL_READ,pair,{3},{0},0, {0}},
        {XR_XIR_STRUCT_GET,XR_XIR_I64,{14},{0},0, {0}},
        {XR_XIR_CELL_READ,pair,{0},{0},0, {0}},
        {XR_XIR_STRUCT_GET,XR_XIR_I64,{16},{0},0, {0}},
        {XR_XIR_STRUCT_GET,XR_XIR_STRING,{16},{0},1, {0}},
        {XR_XIR_OUTPUT,XR_XIR_UNIT,{18},{0},1, {0}},
        {XR_XIR_STRUCT_GET,XR_XIR_I64,{1},{0},0, {0}},
        {XR_XIR_SLOT_LOAD,pair,{0},{0},1, {0}},
        {XR_XIR_STRUCT_GET,XR_XIR_I64,{21},{0},0, {0}},
        {XR_XIR_ADD_INT,XR_XIR_I64,{13,15},{0},0, {0}},
        {XR_XIR_ADD_INT,XR_XIR_I64,{23,17},{0},0, {0}},
        {XR_XIR_ADD_INT,XR_XIR_I64,{24,20},{0},0, {0}},
        {XR_XIR_ADD_INT,XR_XIR_I64,{25,22},{0},0, {0}},
        {XR_XIR_PRINT,XR_XIR_UNIT,{0,5},{0},0, {0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{26},{0},0, {0}}};
    XrXirInstruction replace[] = {
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},73, {0}},
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},1, {0}},
        {XR_XIR_STRUCT_NEW,pair,{0,2},{0},0, {0}},
        {XR_XIR_SLOT_LOAD,cell,{0},{0},0, {0}},
        {XR_XIR_CELL_WRITE,XR_XIR_UNIT,{3,2},{0},0, {0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41, {0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{5},{0},0, {0}}};
    uint32_t init_values[] = {1,0}, replace_values[] = {0,1};
    uint32_t printed[] = {13,15,17,20,22};
    XrXirBlock seven = {0,7, 0, 0}, all = {0,29, 0, 0};
    XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,&seven,1,init,7,init_values,2},
        {"entry",5,NULL,0,XR_XIR_I64,&all,1,entry,29,printed,5},
        {"replace",7,NULL,0,XR_XIR_I64,&seven,1,replace,7,replace_values,2}};
    XrXirSourceModule source = {"alpha",5,NULL,0,0};
    XrXirFunctionIdentity identities[] = {{0,0,0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0},{0,1,0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0},{0,0,0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}};
    XrXirLiteral literals[] = {{"roots",5},{"replacement",11}};
    XrXirSlot slots[] = {{0,cell,1},{0,pair,0}};
    XrXirDeclarations declarations = {&source,1,identities,slots,2,literals,2,0,1, NULL};
    XrXirModule built = {XR_XIR_BUILT,functions,3,&declarations,NULL,&types, NULL, XR_XIR_PROGRAM, NULL};
    if (invalid == 1) entry[5].args[0] = 1;
    if (invalid == 2) nominal.fields[0].flags = 0;
    if (invalid == 3) {
        /* Keep the immutable binding representation valid before rejecting its writes. */
        slots[0] = (XrXirSlot){0,pair,0};
        init[3] = (XrXirInstruction){XR_XIR_COPY,pair,{2},{0},0,{0}};
        entry[0].type = pair;
        entry[1] = (XrXirInstruction){XR_XIR_COPY,pair,{0},{0},0,{0}};
        entry[9] = (XrXirInstruction){XR_XIR_SLOT_PLACE,pair,{0},{0},0,{0}};
        entry[16] = (XrXirInstruction){XR_XIR_SLOT_LOAD,pair,{0},{0},0,{0}};
        replace[3] = (XrXirInstruction){XR_XIR_COPY,pair,{2},{0},0,{0}};
        replace[4] = (XrXirInstruction){XR_XIR_SLOT_STORE,XR_XIR_UNIT,{2},{0},0,{0}};
    }
    if (invalid == 4) nominal.fields[0].flags |= XR_XIR_FIELD_PRIVATE;
    if (invalid == 5) entry[5].args[1] = 1;
    if (invalid == 6) entry[5].immediate = 2;
    if (invalid == 7) entry[5].immediate = -1;
    if (invalid == 8) entry[5].type = XR_XIR_I64;
    if (invalid == 9) entry[8].args[0] = 3;
    if (invalid == 10) entry[5].targets[0] = 1;
    if (invalid == 11) entry[5].args[0] = 12;
    XrXirArtifact *checked = NULL;
    XrXirStatus status = xir_fixture_check(context, &built, &checked, NULL);
    if (invalid) CHECK(status != XR_XIR_OK && !checked);
    else CHECK(status == XR_XIR_OK && checked);
    return checked;
}
static inline XrXirArtifact *struct_set_lowered(const XrXirCompileContext *context) {
    XrXirArtifact *checked = struct_set_checked(context, 0), *decoded = NULL, *closed = NULL, *lowered = NULL;
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
#endif // XIR_STRUCT_SET_FIXTURE_H
