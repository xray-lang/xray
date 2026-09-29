/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_struct_ops_fixture.h - Checked construction, private reads and suspended owners
 */
#ifndef XIR_STRUCT_OPS_FIXTURE_H
#define XIR_STRUCT_OPS_FIXTURE_H
#include "xir_nominal_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
static XrXirArtifact *struct_ops_checked(unsigned invalid) {
    const XrXirType pair = (XrXirType)256;
    XrXirType parameters[] = {XR_XIR_I64,XR_XIR_STRING}, argument = XR_XIR_I64;
    NominalFixture nominal; nominal_fixture(&nominal);
    nominal.declarations[1] = (XrXirNominalDeclaration) {{"alpha",5},{"Empty",5},1,NULL,0,NULL,0, XR_XIR_NOMINAL_STRUCT, NULL, 0};
    XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{0,&argument,1,NULL,0}},
        {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{1,NULL,0,NULL,0}}};
    XrXirTypes types = {nodes,2,&nominal.table};
    XrXirInstruction init = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0, {0}};
    XrXirInstruction entry[] = {
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},23, {0}},
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},0, {0}},
        {XR_XIR_CALL,pair,{0,2},{0},2, {0}},
        {XR_XIR_STRUCT_GET,XR_XIR_I64,{2},{0},0, {0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{3},{0},0, {0}}};
    XrXirInstruction make[] = {
        {XR_XIR_STRUCT_NEW,pair,{0,2},{0},0, {0}},
        {XR_XIR_STRUCT_GET,XR_XIR_STRING,{2},{0},1, {0}},
        {XR_XIR_OUTPUT,XR_XIR_UNIT,{3},{0},1, {0}},
        {XR_XIR_SUSPEND,XR_XIR_UNIT,{0},{0},0, {0}},
        {XR_XIR_STRUCT_NEW,(XrXirType)257,{0},{0},0, {0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0, {0}}};
    uint32_t operands[] = {0,1};
    XrXirBlock one = {0,1, 0}, five = {0,5, 0}, six = {0,6, 0};
    XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,&one,1,&init,1,NULL,0},
        {"entry",5,NULL,0,XR_XIR_I64,&five,1,entry,5,operands,2},
        {"make",4,parameters,2,pair,&six,1,make,6,operands,2}};
    XrXirSourceModule source = {"alpha",5,NULL,0,0};
    XrXirFunctionIdentity identities[] = {{0,0,0, 0},{0,1,0, 0},{0,1,1, 0}};
    XrXirLiteral literal = {"constructed",11};
    XrXirDeclarations declarations = {&source,1,identities,NULL,0,&literal,1,0,1};
    XrXirModule built = {XR_XIR_BUILT,functions,3,&declarations,NULL,&types, NULL};
    if (invalid == 1) identities[2].nominal_owner = 0;
    if (invalid == 2) make[1].immediate = -1;
    if (invalid == 3) entry[3].immediate = 1;
    if (invalid == 4) make[1].type = XR_XIR_BOOL;
    if (invalid == 5) make[0].args[1] = 1;
    if (invalid == 6) operands[0] = 1;
    if (invalid == 7) make[1].args[1] = 1;
    if (invalid == 8) make[1].args[0] = 6;
    if (invalid == 9) make[0].targets[0] = 1;
    if (invalid == 10) make[0].immediate = 1;
    if (invalid == 11) make[1].immediate = 2;
    XrXirArtifact *checked = NULL;
    XrXirStatus status = xr_xir_check(&built,NULL,&checked,NULL);
    if (invalid) CHECK(status != XR_XIR_OK && !checked);
    else CHECK(status == XR_XIR_OK && checked);
    return checked;
}
static inline XrXirArtifact *struct_ops_lowered(void) {
    XrXirArtifact *checked = struct_ops_checked(0), *decoded = NULL, *closed = NULL, *lowered = NULL;
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
#endif // XIR_STRUCT_OPS_FIXTURE_H
