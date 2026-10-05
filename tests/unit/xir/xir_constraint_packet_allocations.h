/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_constraint_packet_allocations.h - Deep constraint allocation rollback
 */
#ifndef XIR_CONSTRAINT_PACKET_ALLOCATIONS_H
#define XIR_CONSTRAINT_PACKET_ALLOCATIONS_H
#include "xir_constraint_packet_fixture.h"
static void constraint_copy_allocation_failures(void) {
    size_t sites = 0;
    for (size_t site = 0; site <= sites; ++site) {
        XrXirType arguments[] = {XR_XIR_I64,XR_XIR_STRING};
        XrXirInterfaceApplication applications[] = {{3,arguments,2},{4,NULL,0}};
        XrXirConstraint constraints[] = {{0,applications,2},{XR_XIR_CONSTRAINT_SENDABLE,applications,2},{0}};
        AllocationCompileOwner owner={0};allocation_compile_owner_new(&owner,&allocation_compile_limits);
        calls = 0; fail_at = site ? site - 1 : SIZE_MAX;
        XrXirConstraint *copy=NULL;
        XrXirStatus status = xr_xir_compile_constraint_array_copy_verified(&owner.context,constraints,3,&copy);
        if (!site) {
            CHECK(status == XR_XIR_OK && copy); sites = calls;
            memset(arguments,0xcc,sizeof(arguments)); memset(applications,0xcc,sizeof(applications));
            memset(constraints,0xcc,sizeof(constraints));
            CHECK(!copy[0].markers && copy[0].interface_count == 2);
            CHECK(copy[0].interfaces[0].declaration == 3 && copy[0].interfaces[0].argument_count == 2);
            CHECK(copy[0].interfaces[0].arguments[0] == XR_XIR_I64);
            CHECK(copy[1].markers == XR_XIR_CONSTRAINT_SENDABLE);
            CHECK(copy[1].interfaces[0].arguments[1] == XR_XIR_STRING);
            CHECK(copy[1].interfaces != copy[0].interfaces && copy[1].interfaces[0].arguments != copy[0].interfaces[0].arguments);
            CHECK(!copy[2].interfaces && !copy[2].interface_count);
        } else CHECK(status == XR_XIR_OUT_OF_MEMORY && !copy);
        xr_xir_compile_constraint_array_free(copy,3);fail_at=SIZE_MAX;allocation_compile_owner_drop(&owner);CHECK(!live);
    }
    fail_at = SIZE_MAX;
    printf("Owned constraint copies: %zu allocation failure sites\n",sites);
}
static void constraint_packet_allocation_failures(void) {
    CHECK(!live); constraint_copy_allocation_failures();
    for (unsigned mixed = 0; mixed < 2; ++mixed) {
        uint32_t markers = mixed ? XR_XIR_CONSTRAINT_SENDABLE : 0;
        AllocationCompileOwner owner={0};allocation_compile_owner_new(&owner,&allocation_compile_limits);
        XrXirArtifact *source=NULL,*decoded=NULL;
        CHECK(constraint_packet_fixture(&owner.context,markers,&source)==XR_XIR_OK);
        XrXirCheckedPacket packet = {0};
        size_t baseline = live; calls = 0;
        CHECK(xr_xir_compile_checked_write(source,&packet,NULL) == XR_XIR_OK);
        size_t writes = calls;
        xr_xir_compile_checked_packet_free(&packet); CHECK(live == baseline);
        for (size_t i = 0; i < writes; ++i) {
            calls = 0; fail_at = i;
            CHECK(xr_xir_compile_checked_write(source,&packet,NULL) == XR_XIR_OUT_OF_MEMORY);
            CHECK(!packet.bytes && !packet.length && live == baseline);
        }
        fail_at = SIZE_MAX;
        CHECK(xr_xir_compile_checked_write(source,&packet,NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(source); CHECK(live == owner.blocks+2); baseline = live; calls = 0;
        CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL) == XR_XIR_OK);
        size_t reads = calls;
        xr_xir_compile_artifact_free(decoded); decoded=NULL; CHECK(live == baseline);
        for (size_t i = 0; i < reads; ++i) {
            calls = 0; fail_at = i; decoded = NULL;
            CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL) == XR_XIR_OUT_OF_MEMORY);
            CHECK(!decoded && live == baseline);
        }
        fail_at = SIZE_MAX;
        CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL) == XR_XIR_OK);
        memset(packet.bytes,0xcc,packet.length); xr_xir_compile_checked_packet_free(&packet);
        constraint_packet_owned(decoded,markers);
        CHECK(xr_xir_compile_artifact_verify(decoded,NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(decoded); decoded=NULL; allocation_compile_owner_drop(&owner); CHECK(!live);
        printf("Constraint packets (mixed=%u): %zu writer and %zu reader allocation failure sites\n",mixed,writes,reads);
    }
}
#endif // XIR_CONSTRAINT_PACKET_ALLOCATIONS_H
