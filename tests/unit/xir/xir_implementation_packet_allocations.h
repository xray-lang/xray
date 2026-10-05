/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XIR_IMPLEMENTATION_PACKET_ALLOCATIONS_H
#define XIR_IMPLEMENTATION_PACKET_ALLOCATIONS_H
#include "xir_implementation_packet_fixture.h"
static void implementation_packet_allocations(void) {
    fail_at = SIZE_MAX;
    AllocationCompileOwner owner={0};allocation_compile_owner_new(&owner,&allocation_compile_limits);
    XrXirArtifact *source=NULL,*decoded=NULL;
    CHECK(implementation_packet_fixture(&owner.context,&source)==XR_XIR_OK);
    XrXirCheckedPacket packet = {0}; size_t baseline = live;
    calls = 0;
    CHECK(xr_xir_compile_checked_write(source,&packet,NULL) == XR_XIR_OK);
    size_t writes = calls; xr_xir_compile_checked_packet_free(&packet); CHECK(live == baseline);
    for (size_t i = 0; i < writes; ++i) {
        calls = 0; fail_at = i;
        CHECK(xr_xir_compile_checked_write(source,&packet,NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!packet.bytes && live == baseline);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_compile_checked_write(source,&packet,NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(source); CHECK(live == owner.blocks+2); baseline = live; calls = 0;
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL) == XR_XIR_OK);
    size_t reads = calls; xr_xir_compile_artifact_free(decoded); decoded=NULL; CHECK(live == baseline);
    for (size_t i = 0; i < reads; ++i) {
        calls = 0; fail_at = i; decoded = NULL;
        CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!decoded && live == baseline);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL) == XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length); xr_xir_compile_checked_packet_free(&packet);
    implementation_packet_owned(decoded);
    xr_xir_compile_artifact_free(decoded); decoded=NULL; allocation_compile_owner_drop(&owner); CHECK(!live);
    printf("Implementation packet: %zu writer and %zu reader allocation failure sites\n",writes,reads);
}
#endif // XIR_IMPLEMENTATION_PACKET_ALLOCATIONS_H
