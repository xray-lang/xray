/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_checked_legacy_packets.h - Complete previous semantic packets reject before allocation
 */
#ifndef XIR_CHECKED_LEGACY_PACKETS_H
#define XIR_CHECKED_LEGACY_PACKETS_H
typedef struct XirLegacyPacket {const uint8_t *bytes;size_t length;} XirLegacyPacket;
static void checked_legacy_packets(const XrXirCompileContext *context,const XirLegacyPacket *packets,size_t count) {
    for(size_t i=0;i<count;++i) {
        XrCompileResourceStats before={0},after={0};
        CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
        XrXirArtifact *decoded=NULL;
        CHECK(xr_xir_compile_checked_read(context,packets[i].bytes,packets[i].length,&decoded,NULL)==XR_XIR_BAD_STRUCTURE && !decoded);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes);
    }
}
#endif // XIR_CHECKED_LEGACY_PACKETS_H
