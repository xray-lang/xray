/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_foreach_packets.h - Independent positional fields and exact callback rejection
 */
#ifndef P09_HASH_FOREACH_PACKETS_H
#define P09_HASH_FOREACH_PACKETS_H
#include "xir_hash_position84_golden.h"
#include "xir_map_membership80_golden.h"
_Static_assert(XR_XIR_OP_COUNT==176 && XR_XIR_CHECKED_CONTRACT==84,"hash positional identity");
static void foreach_packets(const XrXirCompileContext *context) {
    const uint8_t *valid[]={hash_position84_map_key,hash_position84_map_value,hash_position84_set_key};
    const size_t lengths[]={sizeof(hash_position84_map_key),sizeof(hash_position84_map_value),sizeof(hash_position84_set_key)};
    for(unsigned i=0;i<3;++i) {
        XrXirArtifact *artifact=NULL;XrXirCheckedPacket packet={0};
        CHECK(xr_xir_compile_checked_read(context,valid[i],lengths[i],&artifact,NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_OK);
        CHECK(packet.length==lengths[i] && !memcmp(packet.bytes,valid[i],packet.length));
        xr_xir_compile_artifact_free(artifact);xr_xir_compile_checked_packet_free(&packet);
    }
    const uint8_t *bad[]={hash_position84_bad_key_receiver,hash_position84_bad_value_receiver,
        hash_position84_bad_key_result,hash_position84_bad_value_result,hash_position84_bad_index,hash_position84_bad_key_proof};
    const size_t sizes[]={sizeof(hash_position84_bad_key_receiver),sizeof(hash_position84_bad_value_receiver),
        sizeof(hash_position84_bad_key_result),sizeof(hash_position84_bad_value_result),sizeof(hash_position84_bad_index),sizeof(hash_position84_bad_key_proof)};
    for(unsigned i=0;i<6;++i) {XrXirArtifact *artifact=NULL;
        CHECK(xr_xir_compile_checked_read(context,bad[i],sizes[i],&artifact,NULL)==XR_XIR_BAD_TYPE && !artifact);}
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(unsigned occupied=0;occupied<2;++occupied) {
        XrXirArtifact *artifact=occupied?(XrXirArtifact *)(uintptr_t)1:NULL,*saved=artifact;
        CHECK(xr_xir_compile_checked_read(context,map_membership80_valid,sizeof(map_membership80_valid),&artifact,NULL)==XR_XIR_BAD_STRUCTURE && artifact==saved);
    }
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(before.allocation_count==after.allocation_count && before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes && before.peak_bytes==after.peak_bytes);
}
static void foreach_rejections(const XrXirCompileContext *context) {
    const char *sources[]={
        "export fn entry(){const m=#{1:2}\nm.forEach(fn(k){})}",
        "export fn entry(){const m=#{1:2}\nm.forEach(fn(k,v,i){})}",
        "export fn entry(){const s=#[1]\ns.forEach(fn(v,i){})}",
        "export fn entry(){const m=#{1:2}\nm.forEach(fn(k:string,v:i64){})}",
        "export fn entry(){const m=#{1:2}\nm.forEach(fn(k:i64,v:string){})}",
        "export fn entry(){const m=#{1:2}\nm.forEach(fn(k,v)->i64{return 1})}",
        "export fn entry(){const m=#{1:2}\nm.forEach(fn(k:ref i64,v:i64){})}",
        "export fn entry(){const m=#{1:2}\nm.forEach()}",
        "export fn entry(){const s=#[1]\ns.forEach<i64>(fn(v){})}",
        "fn bad<T>(s:Set<T>,visit:fn(T)){s.forEach(visit)}",
        "interface Hashable{};fn bad<T:Hashable>(s:Set<T>,visit:fn(T)){s.forEach(visit)}",
        "export fn entry(){const m=#{1:2}\nm.forEach(3)}"};
    for(unsigned i=0;i<12;++i) {
        XrXirSourceResult result={0};
        CHECK(hash_source_check(context,sources[i],&result)==XR_XIR_BAD_TYPE && !result.checked && !result.snapshot);
    }
}
#endif // P09_HASH_FOREACH_PACKETS_H
