/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_callable_root_packet.h - Current shape with independent old-authority rejects
 */
#ifndef XIR_CALLABLE_ROOT_PACKET_H
#define XIR_CALLABLE_ROOT_PACKET_H
#include "base/xsha256.h"
#include "xir_checked_scalar57_golden.h"
#include "xir_checked_scalar58_golden.h"
#include "xir_checked_scalar59_golden.h"
#include "xir_checked_scalar60_golden.h"
#include "xir_checked_scalar64_golden.h"
#include "xir_checked_scalar65_golden.h"
#include "xir_checked_scalar70_golden.h"
#include "xir_generic_method_golden.h"
#include "xir_generic_method64_golden.h"
#include "xir_generic_method65_golden.h"
#include "xir_generic_method70_golden.h"
static void bound_put32(uint8_t *bytes,uint32_t value) {
    for(uint32_t i=0;i<4;++i)bytes[i]=(uint8_t)(value>>(8*i));
}
static void bound_packet_digest(XrXirCheckedPacket *packet) {
    XrSHA256Context digest;xr_sha256_init(&digest);xr_sha256_update(&digest,packet->bytes,32);
    xr_sha256_update(&digest,packet->bytes+64,packet->length-64);xr_sha256_final(&digest,packet->bytes+32);
}
static void bound_previous_packet_rejected(const XrXirCompileContext *context,
    const uint8_t *bytes,size_t length) {
    XrCompileResourceStats before=bound_stats(context);XrXirArtifact *decoded=NULL;
    XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_checked_read(context,bytes,length,&decoded,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    CHECK(!decoded && diagnostic.status==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats after=bound_stats(context);
    CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes &&
        after.live_bytes==before.live_bytes);
    decoded=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(context,bytes,length,&decoded,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(decoded==(XrXirArtifact *)(uintptr_t)1);
    after=bound_stats(context);
    CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes &&
        after.live_bytes==before.live_bytes);
}
static void bound_historical_packets(const XrXirCompileContext *context) {
    static const struct { const uint8_t *bytes;size_t length; } packets[]={
#define BOUND_PACKET(NAME) {NAME,sizeof(NAME)}
        BOUND_PACKET(checked_scalar57_golden),BOUND_PACKET(checked_scalar58_golden),
        BOUND_PACKET(checked_scalar59_golden),BOUND_PACKET(checked_scalar60_golden),
        BOUND_PACKET(checked_scalar64_golden),BOUND_PACKET(checked_scalar65_golden),
        BOUND_PACKET(generic_method_golden),BOUND_PACKET(generic_method64_golden),
        BOUND_PACKET(generic_method65_golden),BOUND_PACKET(checked_scalar70_golden),
        BOUND_PACKET(checked_scalar_previous69_constructed),BOUND_PACKET(generic_method70_golden),
        BOUND_PACKET(generic_method_previous69_constructed),BOUND_PACKET(generic_method70_invalid_flags0),
        BOUND_PACKET(generic_method70_invalid_flags1)
#undef BOUND_PACKET
    };
    _Static_assert(XR_COUNTOF(packets)==15,"Complete frozen scalar and generic packet inventory");
    for(uint32_t p=0;p<XR_COUNTOF(packets);++p)
        bound_previous_packet_rejected(context,packets[p].bytes,packets[p].length);
}
static void bound_packets(void) {
    XrXirCompileContext context=bound_owner(bound_caps());XrXirArtifact *checked=NULL,*decoded=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(bound_fixture(&context,0,3,&checked,&diagnostic)==XR_XIR_OK && checked);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,&diagnostic)==XR_XIR_OK);
    static const uint8_t prefix[]={
        'X','R','C','H','K',0,0,0,28,0,0,0,89,0,0,0,2,0,0,0,0,0,0,0};
    CHECK(packet.length>=84 && !memcmp(packet.bytes,prefix,sizeof(prefix)));
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&decoded,&diagnostic)==XR_XIR_OK && decoded);
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
    bound_historical_packets(&context);
    /* Locate one independently specified zero-parameter callable record; do
     * not call a production decoder to choose the corruption offset. */
    uint8_t record[20]={0};bound_put32(record,1);bound_put32(record+12,(uint32_t)XR_XIR_I64);bound_put32(record+16,2);
    size_t offset=0;uint32_t hits=0;
    for(size_t i=64;i+sizeof(record)<=packet.length;++i)if(!memcmp(packet.bytes+i,record,sizeof(record))) { offset=i+16;++hits; }
    CHECK(hits==1);
    for(uint32_t old=0;old<2;++old) {
        bound_put32(packet.bytes+offset,old);bound_packet_digest(&packet);
        CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&decoded,&diagnostic)==XR_XIR_BAD_TYPE && !decoded);
    }
    bound_put32(packet.bytes+offset,2);
    for(uint32_t semantic=69;semantic<89;++semantic) {
        bound_put32(packet.bytes+12,semantic);bound_packet_digest(&packet);
        bound_previous_packet_rejected(&context,packet.bytes,packet.length);
    }
    bound_put32(packet.bytes+12,89);bound_packet_digest(&packet);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&decoded,&diagnostic)==XR_XIR_OK && decoded);
    xr_xir_compile_artifact_free(decoded);xr_xir_compile_artifact_free(checked);
    xr_xir_compile_checked_packet_free(&packet);bound_owner_free(&context);
}
#endif // XIR_CALLABLE_ROOT_PACKET_H
