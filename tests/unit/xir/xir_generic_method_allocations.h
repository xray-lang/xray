/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_method_allocations.h - Generic method ownership at every allocation
 *
 * KEY CONCEPT:
 *   Completed and partially initialized method constraints have one cleanup owner.
 */
#ifndef XIR_GENERIC_METHOD_ALLOCATIONS_H
#define XIR_GENERIC_METHOD_ALLOCATIONS_H
#include "xir_construction_fixture.h"
#include "xir_generic_method_owned_fixture.h"
static void generic_method_clone_allocations(void) {
    CHECK(!live); size_t sites = 0;
    for (size_t attempt = 0; attempt <= sites; ++attempt) {
        GenericMethodOwnedFixture fixture; generic_method_owned_fixture(&fixture);
        AllocationCompileOwner owner={0};allocation_compile_owner_new(&owner,&allocation_compile_limits); XrXirInterfaceTable *copy=NULL;
        calls = 0; fail_at = attempt ? attempt-1 : SIZE_MAX;
        XrXirStatus status = xr_xir_compile_interfaces_clone(&owner.context,&fixture.table,&fixture.types,&copy);
        if (!attempt) {
            CHECK(status == XR_XIR_OK && copy); sites = calls;
            CHECK(copy->declarations[1].methods != fixture.methods);
            for (uint32_t m = 0; m < 2; ++m) {
                const XrXirInterfaceMethod *method = &copy->declarations[1].methods[m];
                CHECK(method->name.bytes != fixture.methods[m].name.bytes);
                CHECK(method->constraints != &fixture.own[m]);
                CHECK(method->constraints[0].interfaces != &fixture.applications[m]);
                CHECK(method->constraints[0].interfaces[0].arguments != &fixture.parent);
            }
            memset(&fixture,0xCC,sizeof(fixture)); generic_method_owned_assert(copy);
        } else CHECK(status == XR_XIR_OUT_OF_MEMORY && !copy);
        xr_xir_compile_interfaces_free(copy);fail_at=SIZE_MAX;allocation_compile_owner_drop(&owner);CHECK(!live);
    }
    fail_at = SIZE_MAX;
    for (unsigned bad = 0; bad < 2; ++bad) {
        GenericMethodOwnedFixture fixture; generic_method_owned_fixture(&fixture);
        if (!bad) fixture.methods[0].own_parameter_count = 0;
        else fixture.methods[0].constraints = NULL;
        AllocationCompileOwner owner={0};allocation_compile_owner_new(&owner,&allocation_compile_limits); XrXirInterfaceTable *copy=NULL;
        CHECK(xr_xir_compile_interfaces_clone(&owner.context,&fixture.table,&fixture.types,&copy) == XR_XIR_BAD_STRUCTURE);
        CHECK(!copy);allocation_compile_owner_drop(&owner);CHECK(!live);
    }
    printf("Generic method clone: %zu allocation failure sites\n",sites);
}
static void generic_method_packet_allocations(void) {
    CHECK(!live); fail_at = SIZE_MAX;
    AllocationCompileOwner owner={0};allocation_compile_owner_new(&owner,&allocation_compile_limits);
    GenericMethodOwnedFixture fixture; generic_method_owned_fixture(&fixture);
    XrXirArtifact *source = NULL, *decoded = NULL;
    CHECK(xir_fixture_check(&owner.context, &fixture.module, &source, NULL) == XR_XIR_OK);
    memset(&fixture,0xCC,sizeof(fixture));
    XrXirCheckedPacket packet = {0}; size_t baseline = live; calls = 0;
    CHECK(xr_xir_compile_checked_write(source,&packet,NULL) == XR_XIR_OK);
    size_t writes = calls;
    xr_xir_compile_checked_packet_free(&packet); CHECK(live == baseline);
    for (size_t site = 0; site < writes; ++site) {
        calls = 0; fail_at = site;
        CHECK(xr_xir_compile_checked_write(source,&packet,NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!packet.bytes && !packet.length && live == baseline);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_compile_checked_write(source,&packet,NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(source); CHECK(live == owner.blocks+2); baseline = live; calls = 0;
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL) == XR_XIR_OK);
    size_t reads = calls;
    xr_xir_compile_artifact_free(decoded); decoded=NULL; CHECK(live == baseline);
    for (size_t site = 0; site < reads; ++site) {
        calls = 0; fail_at = site; decoded = NULL;
        CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!decoded && live == baseline);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL) == XR_XIR_OK);
    memset(packet.bytes,0xCC,packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(decoded,NULL) == XR_XIR_OK);
    generic_method_owned_assert(xr_xir_compile_artifact_module(decoded)->types->interfaces);
    xr_xir_compile_artifact_free(decoded); decoded=NULL; allocation_compile_owner_drop(&owner); CHECK(!live);
    printf("Generic method packet: %zu writer and %zu reader allocation failure sites\n",writes,reads);
}
#endif // XIR_GENERIC_METHOD_ALLOCATIONS_H
