/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XIR_IMPLEMENTATION_PACKET_ALLOCATIONS_H
#define XIR_IMPLEMENTATION_PACKET_ALLOCATIONS_H
#include "xir_implementation_packet_fixture.h"
static void implementation_packet_allocations(void) {
    fail_at = SIZE_MAX;
    XrXirArtifact *source = implementation_packet_fixture(), *decoded = NULL;
    XrXirCheckedPacket packet = {0}; size_t baseline = live;
    calls = 0;
    CHECK(xr_xir_checked_write(source,NULL,&packet,NULL) == XR_XIR_OK);
    size_t writes = calls; xr_xir_checked_packet_free(&packet); CHECK(live == baseline);
    for (size_t i = 0; i < writes; ++i) {
        calls = 0; fail_at = i;
        CHECK(xr_xir_checked_write(source,NULL,&packet,NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!packet.bytes && live == baseline);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_checked_write(source,NULL,&packet,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(source); CHECK(live == 1); baseline = live; calls = 0;
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
    size_t reads = calls; xr_xir_artifact_free(decoded); CHECK(live == baseline);
    for (size_t i = 0; i < reads; ++i) {
        calls = 0; fail_at = i; decoded = (void *)(uintptr_t)1;
        CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!decoded && live == baseline);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length); xr_xir_checked_packet_free(&packet);
    implementation_packet_owned(decoded);
    xr_xir_artifact_free(decoded); CHECK(!live);
    printf("Implementation packet: %zu writer and %zu reader allocation failure sites\n",writes,reads);
}
#endif // XIR_IMPLEMENTATION_PACKET_ALLOCATIONS_H
