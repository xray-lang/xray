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
#include "xir_construction_fixture.h"
#include "xir_generic_method_owned_fixture.h"
static size_t generic_method_packet_find(const XrXirCheckedPacket *packet,const uint8_t *row,size_t length) {
    size_t offset=0;unsigned matches=0;
    for(size_t i=64;i+length<=packet->length;++i) {
        if(!memcmp(packet->bytes+i,row,length)) {offset=i;++matches;}
    }
    CHECK(matches==1);return offset;
}
static void generic_method_packet_cases(void) {
    GenericMethodOwnedFixture fixture;generic_method_owned_fixture(&fixture);
    XrXirArtifact *checked=NULL,*decoded=NULL;
    CHECK(xir_fixture_check(suite_context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(!xr_xir_compile_artifact_module(checked)->provenance);
    memset(&fixture,0xcc,sizeof(fixture));
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    CHECK(xr_xir_compile_checked_read(suite_context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    generic_method_owned_assert(xr_xir_compile_artifact_module(decoded)->types->interfaces);
    CHECK(xr_xir_compile_artifact_module(decoded)->types->nodes[0].flags==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
    /* Complete literal method rows locate their own binders, without relying
     * on a historical whole-packet identity or an unrelated trailer size. */
    uint8_t map[39]={0},copy[40]={0},signature[36]={0};
    put32(map,3);memcpy(map+4,"map",3);put32(map+7,XR_XIR_CONSTRUCTED_TYPE_BASE);
    put32(map+15,1);put32(map+19,XR_XIR_CONSTRAINT_SENDABLE);put32(map+23,1);
    put32(map+31,1);put32(map+35,XR_XIR_TYPE_PARAMETER_BASE);
    put32(copy,4);memcpy(copy+4,"copy",4);put32(copy+8,XR_XIR_CONSTRUCTED_TYPE_BASE);
    put32(copy+16,1);put32(copy+24,1);put32(copy+32,1);put32(copy+36,XR_XIR_TYPE_PARAMETER_BASE);
    put32(signature,XR_XIR_TYPE_CALLABLE);put32(signature+4,2);put32(signature+8,1);
    put32(signature+12,XR_XIR_TYPE_PARAMETER_BASE+1);put32(signature+24,XR_XIR_TYPE_PARAMETER_BASE+1);
    put32(signature+28,XR_XIR_NEVER);put32(signature+32,XR_XIR_CALLABLE_ROOT_UNRESOLVED);
    size_t map_own=generic_method_packet_find(&packet,map,sizeof(map))+15;
    size_t copy_own=generic_method_packet_find(&packet,copy,sizeof(copy))+16;
    size_t flags=generic_method_packet_find(&packet,signature,sizeof(signature))+32;
    uint8_t *original=xr_malloc(packet.length);CHECK(original);memcpy(original,packet.bytes,packet.length);
    for(uint32_t flag=0;flag<2;++flag) {
        memcpy(packet.bytes,original,packet.length);put32(packet.bytes+flags,flag);digest_packet(&packet);
        checked_current_callable_rejected(packet.bytes,packet.length);
    }
    const struct {size_t offset;uint32_t value;} attacks[]={
        {64,UINT32_MAX},{map_own,UINT32_MAX},{map_own,0},{map_own,65536},
        {map_own+4,UINT32_MAX},{map_own+8,UINT32_MAX},{map_own+12,2},
        {map_own+16,UINT32_MAX},{map_own+20,XR_XIR_TYPE_PARAMETER_BASE+2},{copy_own,UINT32_MAX}};
    for(size_t i=0;i<sizeof(attacks)/sizeof(*attacks);++i) {
        memcpy(packet.bytes,original,packet.length);
        put32(packet.bytes+attacks[i].offset,attacks[i].value);digest_packet(&packet);
        rejected(packet.bytes,packet.length);
    }
    size_t length=packet.length;
    for(size_t short_length=64;short_length<length;++short_length) {
        memcpy(packet.bytes,original,length);packet.length=short_length;
        put32(packet.bytes+24,(uint32_t)(short_length-64));digest_packet(&packet);
        rejected(packet.bytes,short_length);
    }
    packet.length=length;memcpy(packet.bytes,original,length);xr_free(original);
    CHECK(xr_xir_compile_checked_read(suite_context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(decoded,NULL)==XR_XIR_OK);
    generic_method_owned_assert(xr_xir_compile_artifact_module(decoded)->types->interfaces);
    CHECK(xr_xir_compile_artifact_module(decoded)->types->nodes[0].flags==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
}
#endif // XIR_GENERIC_METHOD_PACKET_CASES_H
