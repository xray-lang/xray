/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XIR_IMPLEMENTATION_PACKET_CASES_H
#define XIR_IMPLEMENTATION_PACKET_CASES_H
#include "xir_implementation_packet_fixture.h"
static void implementation_packet_cases(void) {
    XrXirArtifact *source = implementation_packet_fixture(), *decoded = NULL;
    XrXirCheckedPacket packet = {0}, second = {0};
    CHECK(xr_xir_checked_write(source,NULL,&packet,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(source);
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
    implementation_packet_owned(decoded);
    CHECK(xr_xir_checked_write(decoded,NULL,&second,NULL) == XR_XIR_OK);
    CHECK(packet.length == second.length && !memcmp(packet.bytes,second.bytes,packet.length));
    xr_xir_checked_packet_free(&second);
    /* Independent schema words: count, nominal, app, binding count, app, member/function. */
    const uint32_t words[] = {1,0,0,1,XR_XIR_I64,1,0,1,XR_XIR_I64,0,2};
    uint8_t pattern[sizeof(words)];
    for (unsigned i = 0; i < sizeof(words)/sizeof(words[0]); ++i) put32(pattern+4*i,words[i]);
    size_t offset = 0; unsigned matches = 0;
    for (size_t i = 64; i + sizeof(pattern) <= packet.length; ++i)
        if (!memcmp(packet.bytes+i,pattern,sizeof(pattern))) { offset = i; ++matches; }
    CHECK(matches == 1);
    const uint32_t fields[] = {0,1,2,3,4,5,6,7,8,9,10,8};
    const uint32_t values[] = {UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,
        XR_XIR_TYPE_PARAMETER_BASE,UINT32_MAX,UINT32_MAX,UINT32_MAX,
        XR_XIR_TYPE_PARAMETER_BASE,UINT32_MAX,UINT32_MAX,XR_XIR_BOOL};
    for (unsigned i = 0; i < sizeof(fields)/sizeof(fields[0]); ++i) {
        put32(packet.bytes+offset+4*fields[i],values[i]); digest_packet(&packet);
        rejected(packet.bytes,packet.length);
        put32(packet.bytes+offset+4*fields[i],words[fields[i]]); digest_packet(&packet);
    }
    memset(packet.bytes,0xcc,packet.length); xr_xir_checked_packet_free(&packet);
    implementation_packet_owned(decoded);
    CHECK(xr_xir_artifact_verify(decoded,NULL,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
}
#endif // XIR_IMPLEMENTATION_PACKET_CASES_H
