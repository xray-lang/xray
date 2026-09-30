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
#include "xir_nominal_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
static XrXirArtifact *struct_set_checked(unsigned invalid) {
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
        {XR_XIR_SLOT_INIT,XR_XIR_UNIT,{2},{0},0, {0}},
        {XR_XIR_SLOT_INIT,XR_XIR_UNIT,{2},{0},1, {0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0, {0}}};
    XrXirInstruction entry[] = {
        {XR_XIR_SLOT_LOAD,pair,{0},{0},0, {0}},
        {XR_XIR_LOCAL_NEW,pair,{0},{0},0, {0}},
        {XR_XIR_CELL_NEW,cell,{0},{0},0, {0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},23, {0}},
        {XR_XIR_STRUCT_SET,XR_XIR_UNIT,{1,3},{0},0, {0}},
        {XR_XIR_CELL_PLACE,pair,{2},{0},0, {0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},31, {0}},
        {XR_XIR_STRUCT_SET,XR_XIR_UNIT,{5,6},{0},0, {0}},
        {XR_XIR_SLOT_PLACE,pair,{0},{0},0, {0}},
        {XR_XIR_CALL,XR_XIR_I64,{0},{0},2, {0}},
        {XR_XIR_STRUCT_SET,XR_XIR_UNIT,{8,9},{0},0, {0}},
        {XR_XIR_LOCAL_READ,pair,{1},{0},0, {0}},
        {XR_XIR_STRUCT_GET,XR_XIR_I64,{11},{0},0, {0}},
        {XR_XIR_CELL_READ,pair,{2},{0},0, {0}},
        {XR_XIR_STRUCT_GET,XR_XIR_I64,{13},{0},0, {0}},
        {XR_XIR_SLOT_LOAD,pair,{0},{0},0, {0}},
        {XR_XIR_STRUCT_GET,XR_XIR_I64,{15},{0},0, {0}},
        {XR_XIR_STRUCT_GET,XR_XIR_STRING,{15},{0},1, {0}},
        {XR_XIR_OUTPUT,XR_XIR_UNIT,{17},{0},1, {0}},
        {XR_XIR_STRUCT_GET,XR_XIR_I64,{0},{0},0, {0}},
        {XR_XIR_SLOT_LOAD,pair,{0},{0},1, {0}},
        {XR_XIR_STRUCT_GET,XR_XIR_I64,{20},{0},0, {0}},
        {XR_XIR_ADD_INT,XR_XIR_I64,{12,14},{0},0, {0}},
        {XR_XIR_ADD_INT,XR_XIR_I64,{22,16},{0},0, {0}},
        {XR_XIR_ADD_INT,XR_XIR_I64,{23,19},{0},0, {0}},
        {XR_XIR_ADD_INT,XR_XIR_I64,{24,21},{0},0, {0}},
        {XR_XIR_PRINT,XR_XIR_UNIT,{0,5},{0},0, {0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{25},{0},0, {0}}};
    XrXirInstruction replace[] = {
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},73, {0}},
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},1, {0}},
        {XR_XIR_STRUCT_NEW,pair,{0,2},{0},0, {0}},
        {XR_XIR_SLOT_STORE,XR_XIR_UNIT,{2},{0},0, {0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41, {0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{4},{0},0, {0}}};
    uint32_t init_values[] = {1,0}, replace_values[] = {0,1};
    uint32_t printed[] = {12,14,16,19,21};
    XrXirBlock six = {0,6, 0, 0}, all = {0,28, 0, 0};
    XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,&six,1,init,6,init_values,2},
        {"entry",5,NULL,0,XR_XIR_I64,&all,1,entry,28,printed,5},
        {"replace",7,NULL,0,XR_XIR_I64,&six,1,replace,6,replace_values,2}};
    XrXirSourceModule source = {"alpha",5,NULL,0,0};
    XrXirFunctionIdentity identities[] = {{0,0,0, 0, 0, 0, XR_XIR_NON_MEMBER},{0,1,0, 0, 0, 0, XR_XIR_NON_MEMBER},{0,0,0, 0, 0, 0, XR_XIR_NON_MEMBER}};
    XrXirLiteral literals[] = {{"roots",5},{"replacement",11}};
    XrXirSlot slots[] = {{0,pair,1},{0,pair,0}};
    XrXirDeclarations declarations = {&source,1,identities,slots,2,literals,2,0,1, NULL};
    XrXirModule built = {XR_XIR_BUILT,functions,3,&declarations,NULL,&types, NULL};
    if (invalid == 1) entry[4].args[0] = 0;
    if (invalid == 2) nominal.fields[0].flags = 0;
    if (invalid == 3) slots[0].mutable = 0;
    if (invalid == 4) nominal.fields[0].flags |= XR_XIR_FIELD_PRIVATE;
    if (invalid == 5) entry[4].args[1] = 0;
    if (invalid == 6) entry[4].immediate = 2;
    if (invalid == 7) entry[4].immediate = -1;
    if (invalid == 8) entry[4].type = XR_XIR_I64;
    if (invalid == 9) entry[7].args[0] = 2;
    if (invalid == 10) entry[4].targets[0] = 1;
    if (invalid == 11) entry[4].args[0] = 11;
    XrXirArtifact *checked = NULL;
    XrXirStatus status = xr_xir_check(&built,NULL,&checked,NULL);
    if (invalid) CHECK(status != XR_XIR_OK && !checked);
    else CHECK(status == XR_XIR_OK && checked);
    return checked;
}
static inline XrXirArtifact *struct_set_lowered(void) {
    XrXirArtifact *checked = struct_set_checked(0), *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(decoded,NULL,&closed,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed); return lowered;
}
#endif // XIR_STRUCT_SET_FIXTURE_H
