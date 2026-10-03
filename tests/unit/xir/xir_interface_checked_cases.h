/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_interface_checked_cases.h - Owned interface packets and hostile metadata
 */
#ifndef XIR_INTERFACE_CHECKED_CASES_H
#define XIR_INTERFACE_CHECKED_CASES_H
#include "xir/xxir_interface.h"

static XrXirArtifact *interface_checked_fixture(void) {
    XrXirConstraint constraint = {0};
    XrXirType parameter = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE, concrete = XR_XIR_I64;
    XrXirTypeNode signature = {0};
    signature.kind = XR_XIR_TYPE_CALLABLE; signature.result = parameter; signature.parameter_span = 1;
    XrXirInterfaceMethod method = {{"measure",7}, (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE, 0,0,NULL};
    XrXirInterfaceApplication parents[] = {{0,&parameter,1}, {0,&concrete,1}};
    XrXirInterfaceDeclaration declarations[] = {
        {{"alpha",5},{"Measure",7},1,&constraint,1,NULL,0,&method,1},
        {{"alpha",5},{"Child",5},1,&constraint,1,parents,1,NULL,0},
        {{"other",5},{"Concrete",8},1,NULL,0,parents+1,1,NULL,0}};
    XrXirInterfaceTable table = {declarations,3};
    XrXirTypes types = {&signature,1,NULL,&table};
    XrXirInstruction ops[] = {
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirBlock block = {0,2,0,0};
    XrXirInstruction init = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirBlock init_block = {0,1,0,0};
    XrXirFunction functions[] = {
        {"main",4,NULL,0,XR_XIR_I64,&block,1,ops,2,NULL,0},
        {"init",4,NULL,0,XR_XIR_UNIT,&init_block,1,&init,1,NULL,0},
        {"init",4,NULL,0,XR_XIR_UNIT,&init_block,1,&init,1,NULL,0}};
    uint32_t dependency = 0;
    XrXirSourceModule modules[] = {{"alpha",5,NULL,0,1},{"other",5,&dependency,1,2}};
    XrXirFunctionIdentity identities[] = {{0,1,0,0,0,0, XR_XIR_NON_MEMBER, 0, 0},{0,0,0,0,0,0, XR_XIR_NON_MEMBER, 0, 0},{1,0,0,0,0,0, XR_XIR_NON_MEMBER, 0, 0}};
    XrXirDeclarations program = {modules,2,identities,NULL,0,NULL,0,0,0, NULL};
    XrXirModule module = {XR_XIR_BUILT,functions,3,&program,NULL,&types,NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *artifact = NULL;
    CHECK(xr_xir_check(&module,NULL,&artifact,NULL) == XR_XIR_OK);
    return artifact;
}
static size_t interface_packet_name_end(const XrXirCheckedPacket *packet,
    const char *name, size_t length) {
    size_t end = 0; unsigned matches = 0;
    for (size_t at = 64; at + length <= packet->length; ++at)
        if (!memcmp(packet->bytes + at,name,length)) { end = at + length; ++matches; }
    CHECK(matches == 1); return end;
}
static void interface_packet_attack(XrXirCheckedPacket *packet, size_t at, uint32_t value) {
    CHECK(at + 4 <= packet->length);
    uint8_t saved[4]; memcpy(saved,packet->bytes + at,4);
    put32(packet->bytes + at,value); digest_packet(packet);
    rejected(packet->bytes,packet->length);
    memcpy(packet->bytes + at,saved,4); digest_packet(packet);
}
static void interface_checked_cases(void) {
    XrXirArtifact *source = interface_checked_fixture(), *decoded = NULL;
    XrXirCheckedPacket packet = {0}, second = {0};
    CHECK(xr_xir_checked_write(source,NULL,&packet,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(source);
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
    CHECK(xr_xir_checked_write(decoded,NULL,&second,NULL) == XR_XIR_OK);
    CHECK(packet.length == second.length && !memcmp(packet.bytes,second.bytes,packet.length));
    xr_xir_checked_packet_free(&second);
    size_t measure = interface_packet_name_end(&packet,"Measure",7);
    size_t child = interface_packet_name_end(&packet,"Child",5);
    size_t concrete = interface_packet_name_end(&packet,"Concrete",8);
    size_t method = interface_packet_name_end(&packet,"measure",7);
    interface_packet_attack(&packet,8,XR_XIR_CHECKED_SCHEMA - 1);
    interface_packet_attack(&packet,12,XR_XIR_CHECKED_CONTRACT - 1);
    interface_packet_attack(&packet,measure,2);
    interface_packet_attack(&packet,measure + 8,UINT32_MAX);
    interface_packet_attack(&packet,child + 20,UINT32_MAX);
    interface_packet_attack(&packet,child + 20,1);
    interface_packet_attack(&packet,concrete + 20,XR_XIR_UNIT);
    interface_packet_attack(&packet,method,XR_XIR_I64);
    interface_packet_attack(&packet,method + 4,1);
    for (size_t length = 64; length < packet.length; ++length) {
        size_t original = packet.length; packet.length = length;
        put32(packet.bytes + 24,(uint32_t)(length - 64)); digest_packet(&packet);
        rejected(packet.bytes,length); packet.length = original;
    }
    put32(packet.bytes + 24,(uint32_t)(packet.length - 64)); digest_packet(&packet);
    for (unsigned i = 0; i < 4; ++i) {
        XrXirBudget budget = xr_xir_default_budget(); XrXirArtifact *failed = NULL;
        if (i == 0) budget.parameters = 1;
        if (i == 1) budget.metadata_bytes = packet.length;
        if (i == 2) budget.scratch_bytes = 0;
        if (i == 3) budget.work = packet.length * 4 - 1;
        CHECK(xr_xir_checked_read(packet.bytes,packet.length,&budget,&failed,NULL) == XR_XIR_BUDGET);
        CHECK(!failed);
    }
    memset(packet.bytes,0xcc,packet.length); xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_artifact_verify(decoded,NULL,NULL) == XR_XIR_OK);
    const XrXirInterfaceTable *table = xr_xir_artifact_module(decoded)->types->interfaces;
    CHECK(table && table->count == 3);
    CHECK(table->declarations[0].name.length == 7 && !memcmp(table->declarations[0].name.bytes,"Measure",7));
    CHECK(table->declarations[0].methods[0].name.length == 7);
    CHECK(!memcmp(table->declarations[0].methods[0].name.bytes,"measure",7));
    CHECK(table->declarations[1].parents[0].arguments[0] == XR_XIR_TYPE_PARAMETER_BASE);
    CHECK(table->declarations[2].parents[0].arguments[0] == XR_XIR_I64);
    XrXirArtifact *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_specialize(decoded,NULL,&closed,NULL) == XR_XIR_OK);
    const XrXirModule *projected = xr_xir_artifact_module(closed);
    CHECK(!projected->types || !projected->types->interfaces);
    CHECK(projected->provenance && projected->functions[0].instructions[0].immediate == 41);
    CHECK(xr_xir_artifact_verify(closed,NULL,NULL) == XR_XIR_OK);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL) == XR_XIR_OK);
    CHECK(xr_xir_artifact_verify(lowered,NULL,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(lowered); xr_xir_artifact_free(closed);
    xr_xir_artifact_free(decoded);
}
#endif // XIR_INTERFACE_CHECKED_CASES_H
