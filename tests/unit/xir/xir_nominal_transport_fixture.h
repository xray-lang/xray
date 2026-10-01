/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_nominal_transport_fixture.h - Owned nominal calls through local and PHI storage
 */
#ifndef XIR_NOMINAL_TRANSPORT_FIXTURE_H
#define XIR_NOMINAL_TRANSPORT_FIXTURE_H
#include "xir_nominal_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_types.h"
static XrXirArtifact *nominal_transport_fixture(void) {
    const XrXirType pair = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirType parameters[] = {pair, XR_XIR_BOOL}, argument = XR_XIR_I64;
    NominalFixture nominal; nominal_fixture(&nominal); nominal.table.count = 1;
    XrXirTypeNode node = {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0,
        {0, &argument, 1, NULL, 0}};
    XrXirTypes types = {&node, 1, &nominal.table, NULL};
    XrXirInstruction init[] = {{XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirInstruction entry[] = {{XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 7, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirInstruction body[] = {
        {XR_XIR_LOCAL_NEW, pair, {0}, {0}, 0, {0}},
        {XR_XIR_LOCAL_READ, pair, {2}, {0}, 0, {0}},
        {XR_XIR_BRANCH, XR_XIR_UNIT, {1}, {1, 2}, 0, {0}},
        {XR_XIR_COPY, pair, {3}, {0}, 0, {0}},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {3}, 0, {0}},
        {XR_XIR_COPY, pair, {0}, {0}, 0, {0}},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {3}, 0, {0}},
        {XR_XIR_PHI, pair, {0, 4}, {0}, 0, {0}},
        {XR_XIR_SUSPEND, XR_XIR_UNIT, {0}, {0}, 0, {0}},
        {XR_XIR_CALL, pair, {4, 1}, {0}, 3, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {11}, {0}, 0, {0}}};
    XrXirInstruction leaf[] = {{XR_XIR_COPY, pair, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0, {0}}};
    XrXirBlock one = {0, 1, 0, 0}, two = {0, 2, 0, 0}, blocks[] = {{0, 3, 0, 0}, {3, 2, 0, 0}, {5, 2, 0, 0}, {7, 4, 0, 0}};
    uint32_t operands[] = {1, 5, 2, 7, 9};
    XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,&one,1,init,1,NULL,0},
        {"entry",5,NULL,0,XR_XIR_I64,&two,1,entry,2,NULL,0},
        {"carry",5,parameters,2,pair,blocks,4,body,11,operands,5},
        {"leaf",4,parameters,1,pair,&two,1,leaf,2,NULL,0}};
    XrXirSourceModule source = {"alpha",5,NULL,0,0};
    XrXirFunctionIdentity identities[] = {{0,0,0, 0, 0, 0, XR_XIR_NON_MEMBER},{0,1,0, 0, 0, 0, XR_XIR_NON_MEMBER},{0,1,0, 0, 0, 0, XR_XIR_NON_MEMBER},{0,0,0, 0, 0, 0, XR_XIR_NON_MEMBER}};
    XrXirDeclarations declarations = {&source,1,identities,NULL,0,NULL,0,0,1, NULL};
    XrXirModule built = {XR_XIR_BUILT,functions,4,&declarations,NULL,&types, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_check(&built,NULL,&checked,NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(decoded,NULL,&closed,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    CHECK(xr_xir_artifact_layout(lowered,2)->owned_count == 8);
    const XrXirModule *module = xr_xir_artifact_module(lowered);
    CHECK(module->functions[2].instructions[0].op == XR_XIR_OWNED_LOCAL_NEW);
    CHECK(module->functions[2].instructions[3].op == XR_XIR_OWNED_RETAIN);
    XrXirLayout layout = {0};
    CHECK(xr_xir_layout(module->types,pair,&target,XR_XIR_LAYOUT_FRAME,&layout) == XR_XIR_OK && layout.size == 8);
    CHECK(xr_xir_layout(module->types,pair,&target,XR_XIR_LAYOUT_PARAMETER,&layout) == XR_XIR_OK && layout.size == 16);
    CHECK(xr_xir_layout(module->types,pair,&target,XR_XIR_LAYOUT_STORAGE,&layout) == XR_XIR_BAD_LAYOUT);
    return lowered;
}
#endif // XIR_NOMINAL_TRANSPORT_FIXTURE_H
