/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_map_membership_packets.h - Independent fields and definition-site rejection
 */
#ifndef P09_MAP_MEMBERSHIP_PACKETS_H
#define P09_MAP_MEMBERSHIP_PACKETS_H
#include "xir_map_membership85_golden.h"
#include "xir_set_algebra79_golden.h"
_Static_assert(XR_XIR_OP_COUNT==184 && XR_XIR_CHECKED_CONTRACT==85,"value membership identity");
static void membership_packets(const XrXirCompileContext *context) {
    XrXirArtifact *artifact=NULL;XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_read(context,map_membership85_valid,sizeof(map_membership85_valid),&artifact,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_OK);
    CHECK(packet.length==sizeof(map_membership85_valid) && !memcmp(packet.bytes,map_membership85_valid,packet.length));
    xr_xir_compile_artifact_free(artifact);xr_xir_compile_checked_packet_free(&packet);
    const uint8_t *bad[]={map_membership85_bad_receiver,map_membership85_bad_value_width,map_membership85_bad_result,map_membership85_bad_equal,map_membership85_bad_key_proof};
    const size_t sizes[]={sizeof(map_membership85_bad_receiver),sizeof(map_membership85_bad_value_width),sizeof(map_membership85_bad_result),sizeof(map_membership85_bad_equal),sizeof(map_membership85_bad_key_proof)};
    for(unsigned i=0;i<5;++i) {artifact=NULL;CHECK(xr_xir_compile_checked_read(context,bad[i],sizes[i],&artifact,NULL)==XR_XIR_BAD_TYPE && !artifact);}
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(unsigned occupied=0;occupied<2;++occupied) {
        artifact=occupied?(XrXirArtifact *)(uintptr_t)1:NULL;XrXirArtifact *saved=artifact;
        CHECK(xr_xir_compile_checked_read(context,set_algebra79_union,sizeof(set_algebra79_union),&artifact,NULL)==XR_XIR_BAD_STRUCTURE && artifact==saved);
    }
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(before.allocation_count==after.allocation_count && before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes && before.peak_bytes==after.peak_bytes);
}
static void membership_rejections(const XrXirCompileContext *context) {
    const char *sources[]={
        "fn bad<T>(m:Map<i64,T>,value:T)->bool{return m.containsValue(value)}",
        "fn bad<T:Hashable>(m:Map<i64,T>,value:T)->bool{return m.containsValue(value)}",
        "export fn entry()->bool{const m=#{1:\"a\"}\nreturn m.containsValue(1)}",
        "export fn entry()->bool{const m=#{1:2}\nvar value=2\nreturn m.containsValue(ref value)}",
        "export fn entry()->bool{const m=#{1:2}\nreturn m.containsValue()}",
        "export fn entry()->bool{const m=#{1:2}\nreturn m.containsValue<i64>(2)}",
        "export fn entry()->bool{const s=#[1]\nreturn s.containsValue(1)}",
        "fn bad(m:Map<i64,Set<i64>>,value:Set<i64>)->bool{return m.containsValue(value)}",
        "interface Equal{};fn bad<T:Equal>(m:Map<i64,T>,value:T)->bool{return m.containsValue(value)}"};
    for(unsigned i=0;i<9;++i) {
        XrXirSourceResult result={0};
        CHECK(hash_source_check(context,sources[i],&result)==XR_XIR_BAD_TYPE && !result.checked && !result.snapshot);
    }
}
#endif // P09_MAP_MEMBERSHIP_PACKETS_H
