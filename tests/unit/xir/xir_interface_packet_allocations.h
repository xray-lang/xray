/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_interface_packet_allocations.h - Interface packet failure ownership
 */
#ifndef XIR_INTERFACE_PACKET_ALLOCATIONS_H
#define XIR_INTERFACE_PACKET_ALLOCATIONS_H
static XrXirArtifact *interface_allocation_fixture(void) {
    XrXirConstraint constraint = {0};
    XrXirType parameter = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE, argument = XR_XIR_I64;
    XrXirTypeNode signature = {0};
    signature.kind = XR_XIR_TYPE_CALLABLE; signature.result = parameter; signature.parameter_span = 1;
    XrXirInterfaceMethod method = {{"measure",7},(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,0};
    XrXirInterfaceApplication parents[] = {{0,&parameter,1},{0,&argument,1}};
    XrXirInterfaceDeclaration declarations[] = {
        {{"alpha",5},{"Measure",7},1,&constraint,1,NULL,0,&method,1},
        {{"alpha",5},{"Child",5},1,&constraint,1,parents,1,NULL,0},
        {{"other",5},{"Concrete",8},1,NULL,0,parents+1,1,NULL,0}};
    XrXirInterfaceTable interfaces = {declarations,3};
    XrXirTypes types = {&signature,1,NULL,&interfaces};
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
    XrXirFunctionIdentity identities[] = {{0,1,0,0,0,0},{0,0,0,0,0,0},{1,0,0,0,0,0}};
    XrXirDeclarations program = {modules,2,identities,NULL,0,NULL,0,0,0};
    XrXirModule module = {XR_XIR_BUILT,functions,3,&program,NULL,&types,NULL};
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_check(&module,NULL,&checked,NULL) == XR_XIR_OK);
    return checked;
}
static void interface_packet_allocation_failures(void) {
    CHECK(!live); fail_at = SIZE_MAX;
    XrXirArtifact *source = interface_allocation_fixture(), *decoded = NULL;
    size_t baseline = live;
    XrXirCheckedPacket packet = {0};
    calls = 0;
    CHECK(xr_xir_checked_write(source,NULL,&packet,NULL) == XR_XIR_OK);
    size_t writes = calls;
    xr_xir_checked_packet_free(&packet); CHECK(live == baseline);
    for (size_t i = 0; i < writes; ++i) {
        calls = 0; fail_at = i;
        CHECK(xr_xir_checked_write(source,NULL,&packet,NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!packet.bytes && !packet.length && live == baseline);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_checked_write(source,NULL,&packet,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(source); CHECK(live == 1);
    baseline = live; calls = 0;
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
    size_t reads = calls;
    xr_xir_artifact_free(decoded); CHECK(live == baseline);
    for (size_t i = 0; i < reads; ++i) {
        calls = 0; fail_at = i; decoded = (XrXirArtifact *)(uintptr_t)1;
        CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!decoded && live == baseline);
    }
    fail_at = SIZE_MAX;
    for (size_t i = 64; i < packet.length; ++i) {
        packet.bytes[i] ^= 0xff;
        checked_digest(packet.bytes,packet.length,packet.bytes + 32);
        XrXirStatus status = xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL);
        if (status == XR_XIR_OK) xr_xir_artifact_free(decoded);
        else CHECK(!decoded);
        CHECK(live == baseline); packet.bytes[i] ^= 0xff;
    }
    checked_digest(packet.bytes,packet.length,packet.bytes + 32);
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length); xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_artifact_verify(decoded,NULL,NULL) == XR_XIR_OK);
    const XrXirInterfaceTable *table = xr_xir_artifact_module(decoded)->types->interfaces;
    CHECK(table && table->count == 3);
    CHECK(table->declarations[2].parents[0].arguments[0] == XR_XIR_I64);
    CHECK(!memcmp(table->declarations[0].methods[0].name.bytes,"measure",7));
    xr_xir_artifact_free(decoded); CHECK(!live);
    printf("Interface packet physical release: %zu writer and %zu reader allocation sites\n",writes,reads);
}
#endif // XIR_INTERFACE_PACKET_ALLOCATIONS_H
