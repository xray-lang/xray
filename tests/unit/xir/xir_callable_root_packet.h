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
static void bound_put32(uint8_t *bytes,uint32_t value) {
    for(uint32_t i=0;i<4;++i)bytes[i]=(uint8_t)(value>>(8*i));
}
static void bound_packet_digest(XrXirCheckedPacket *packet) {
    XrSHA256Context digest;xr_sha256_init(&digest);xr_sha256_update(&digest,packet->bytes,32);
    xr_sha256_update(&digest,packet->bytes+64,packet->length-64);xr_sha256_final(&digest,packet->bytes+32);
}
static void bound_packets(void) {
    XrXirCompileContext context=bound_owner(bound_caps());XrXirArtifact *checked=NULL,*decoded=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(bound_fixture(&context,0,3,&checked,&diagnostic)==XR_XIR_OK && checked);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,&diagnostic)==XR_XIR_OK);
    CHECK(packet.length>=84 && packet.bytes[8]==26 && packet.bytes[12]==71);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&decoded,&diagnostic)==XR_XIR_OK && decoded);
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
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
    bound_put32(packet.bytes+offset,2);bound_put32(packet.bytes+12,69);bound_packet_digest(&packet);
    XrCompileResourceStats before=bound_stats(&context);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&decoded,&diagnostic)==XR_XIR_BAD_STRUCTURE && !decoded);
    CHECK(bound_stats(&context).allocation_count==before.allocation_count && bound_stats(&context).live_bytes==before.live_bytes);
    bound_put32(packet.bytes+12,70);bound_packet_digest(&packet);
    before=bound_stats(&context);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&decoded,&diagnostic)==XR_XIR_BAD_STRUCTURE && !decoded);
    CHECK(bound_stats(&context).allocation_count==before.allocation_count && bound_stats(&context).live_bytes==before.live_bytes);
    bound_put32(packet.bytes+12,71);bound_packet_digest(&packet);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&decoded,&diagnostic)==XR_XIR_OK && decoded);
    xr_xir_compile_artifact_free(decoded);xr_xir_compile_artifact_free(checked);
    xr_xir_compile_checked_packet_free(&packet);bound_owner_free(&context);
}
#endif // XIR_CALLABLE_ROOT_PACKET_H
