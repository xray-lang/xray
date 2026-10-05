/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_method_packet_cases.h - Independently encoded generic method packets
 *
 * KEY CONCEPT:
 *   Rehashed malformed packets reach the decoder and semantic admission boundary.
 */
#ifndef XIR_GENERIC_METHOD_PACKET_CASES_H
#define XIR_GENERIC_METHOD_PACKET_CASES_H
#include "xir_generic_method_owned_fixture.h"
#include "xir_generic_method_golden.h"
static void generic_method_packet_cases(void) {
    GenericMethodOwnedFixture fixture; generic_method_owned_fixture(&fixture);
    XrXirArtifact *checked = NULL, *decoded = NULL;
    CHECK(xr_xir_compile_check(suite_context, &fixture.module, &checked, NULL) == XR_XIR_OK);
    memset(&fixture,0xCC,sizeof(fixture));
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    CHECK(packet.length == sizeof(generic_method_golden));
    CHECK(!memcmp(packet.bytes,generic_method_golden,sizeof(generic_method_golden)));
    CHECK(xr_xir_compile_checked_read(suite_context, generic_method_golden, sizeof(generic_method_golden), &decoded, NULL) == XR_XIR_OK);
    generic_method_owned_assert(xr_xir_compile_artifact_module(decoded)->types->interfaces);
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
    /* Exact old semantic revision with a freshly valid wire digest. */
    memcpy(packet.bytes,generic_method_golden,packet.length);
    put32(packet.bytes+12,48);digest_packet(&packet);decoded=NULL;
    XrXirDiagnostic previous={0};
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, &previous)==XR_XIR_BAD_STRUCTURE);
    CHECK(!decoded && previous.status==XR_XIR_BAD_STRUCTURE);
    const struct { size_t offset; uint32_t value; } attacks[] = {
        {8,19}, {12,50}, {8,UINT32_MAX}, {12,UINT32_MAX}, {64,UINT32_MAX}, {12,49}, {12,48}, {12,47}, {8,17}, {12,45}, {8,18}, {12,46},
        {GENERIC_METHOD_MAP_OWN,UINT32_MAX}, {GENERIC_METHOD_MAP_OWN,0},
        {GENERIC_METHOD_MAP_OWN,65536}, {GENERIC_METHOD_MAP_OWN+4,UINT32_MAX},
        {GENERIC_METHOD_MAP_OWN+8,UINT32_MAX}, {GENERIC_METHOD_MAP_OWN+12,2},
        {GENERIC_METHOD_MAP_OWN+16,UINT32_MAX},
        {GENERIC_METHOD_MAP_OWN+20,XR_XIR_TYPE_PARAMETER_BASE+2},
        {GENERIC_METHOD_COPY_OWN,UINT32_MAX}
    };
    for (uint32_t i = 0; i < sizeof(attacks)/sizeof(*attacks); ++i) {
        memcpy(packet.bytes,generic_method_golden,packet.length);
        put32(packet.bytes+attacks[i].offset,attacks[i].value); digest_packet(&packet);
        rejected(packet.bytes,packet.length);
    }
    for (size_t length = 64; length < sizeof(generic_method_golden); ++length) {
        memcpy(packet.bytes,generic_method_golden,sizeof(generic_method_golden));
        packet.length = length; put32(packet.bytes+24,(uint32_t)(length-64));
        digest_packet(&packet); rejected(packet.bytes,packet.length);
    }
    packet.length = sizeof(generic_method_golden);
    memcpy(packet.bytes,generic_method_golden,packet.length);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    memset(packet.bytes,0xCC,packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(decoded, NULL) == XR_XIR_OK);
    generic_method_owned_assert(xr_xir_compile_artifact_module(decoded)->types->interfaces);
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
}
#endif // XIR_GENERIC_METHOD_PACKET_CASES_H
