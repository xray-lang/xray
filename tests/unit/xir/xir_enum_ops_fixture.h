/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_enum_ops_fixture.h - Owned enum instructions through the sole artifact pipeline
 */
#ifndef XIR_ENUM_OPS_FIXTURE_H
#define XIR_ENUM_OPS_FIXTURE_H
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_nominal.h"
static XrXirArtifact *enum_ops_checked(unsigned invalid, bool wrong_variant) {
    XrXirNominalVariant variants[] = {{{"Empty",5},0,0},{{"Some",4},0,2}};
    XrXirNominalField fields[] = {{{"value",5},XR_XIR_I64,0},{{"note",4},XR_XIR_STRING,0}};
    XrXirNominalDeclaration declaration = {{"alpha",5},{"Choice",6},1,NULL,0,fields,2,XR_XIR_NOMINAL_ENUM,variants,2};
    XrXirNominalTable table = {&declaration,1,NULL};
    XrXirTypeNode node = {0}; node.kind = XR_XIR_TYPE_NOMINAL;
    XrXirTypes types = {&node,1,&table};
    XrXirInstruction init = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0, {0}};
    XrXirInstruction entry[] = {
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},23, {0}},
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},0, {0}},
        {XR_XIR_ENUM_NEW,(XrXirType)256,{0,2},{0},1, {0}},
        {XR_XIR_ENUM_TAG,XR_XIR_I64,{2},{0},0, {0}},
        {XR_XIR_ENUM_GET,XR_XIR_I64,{2,0},{0},1, {0}},
        {XR_XIR_ENUM_GET,XR_XIR_STRING,{2,1},{0},1, {0}},
        {XR_XIR_ENUM_NEW,(XrXirType)256,{0},{0},0, {0}},
        {XR_XIR_ENUM_TAG,XR_XIR_I64,{6},{0},0, {0}},
        {XR_XIR_PRINT,XR_XIR_UNIT,{2,4},{0},0, {0}},
        {XR_XIR_SUSPEND,XR_XIR_UNIT,{0},{0},0, {0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0, {0}}};
    uint32_t operands[] = {0,1,3,4,7,5};
    XrXirInstruction main_ops[] = {{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0, {0}},{XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0, {0}}};
    XrXirBlock one = {0,1}, two = {0,2}, block = {0,11};
    XrXirFunction functions[] = {{"init",4,NULL,0,XR_XIR_UNIT,&one,1,&init,1,NULL,0},
        {"entry",5,NULL,0,XR_XIR_I64,&two,1,main_ops,2,NULL,0},
        {"make",4,NULL,0,(XrXirType)256,&block,1,entry,11,operands,6}};
    XrXirSourceModule source = {"alpha",5,NULL,0,0};
    XrXirFunctionIdentity identities[] = {{0,0,0,0},{0,1,0,0},{0,1,0,0}};
    XrXirLiteral literal = {"constructed",11};
    XrXirDeclarations declarations = {&source,1,identities,NULL,0,&literal,1,0,1};
    XrXirModule built = {XR_XIR_BUILT,functions,3,&declarations,NULL,&types,NULL};
    if (wrong_variant) {
        entry[2].immediate = 0; entry[2].args[1] = 0; entry[8].args[0] = 0;
        operands[0] = 3; operands[1] = 4; operands[2] = 7; operands[3] = 5; functions[2].operand_count = 4;
    }
    if (invalid == 1) entry[2].immediate = 2;
    if (invalid == 2) entry[4].immediate = -1;
    if (invalid == 3) entry[4].args[1] = 2;
    if (invalid == 4) entry[5].type = XR_XIR_BOOL;
    if (invalid == 5) entry[2].args[1] = 1;
    if (invalid == 6) operands[0] = 1;
    if (invalid == 7) entry[3].args[1] = 1;
    if (invalid == 8) entry[4].args[0] = 5;
    if (invalid == 9) entry[2].targets[0] = 1;
    if (invalid == 10) entry[3].immediate = 1;
    if (invalid == 11) entry[5].immediate = 0;
    if (invalid == 12) entry[3].args[0] = 0;
    if (invalid == 13) entry[2].op = XR_XIR_STRUCT_NEW;
    XrXirArtifact *checked = NULL; XrXirStatus status = xr_xir_check(&built,NULL,&checked,NULL);
    if (invalid) CHECK(status != XR_XIR_OK && !checked);
    else CHECK(status == XR_XIR_OK && checked);
    return checked;
}
static XrXirArtifact *enum_ops_lowered(bool wrong_variant) {
    XrXirArtifact *checked = enum_ops_checked(0, wrong_variant), *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL) == XR_XIR_OK); xr_xir_artifact_free(checked);
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK); xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(decoded,NULL,&closed,NULL) == XR_XIR_OK); xr_xir_artifact_free(decoded);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL) == XR_XIR_OK); xr_xir_artifact_free(closed);
    return lowered;
}
#endif // XIR_ENUM_OPS_FIXTURE_H
