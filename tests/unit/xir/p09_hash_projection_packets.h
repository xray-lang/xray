/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_projection_packets.h - Independent projection fields and early rejection
 */
#ifndef P09_HASH_PROJECTION_PACKETS_H
#define P09_HASH_PROJECTION_PACKETS_H
#include "composed_wire/packet_064.inc.c"
#include "composed_wire/packet_065.inc.c"
#include "composed_wire/packet_066.inc.c"
#include "composed_wire/packet_067.inc.c"
#include "composed_wire/packet_068.inc.c"
#include "composed_wire/packet_069.inc.c"
#include "composed_wire/packet_070.inc.c"
#include "composed_wire/packet_071.inc.c"
#include "composed_wire/packet_072.inc.c"
#include "hash_protocol28_inputs/xir_map_index77_golden.h"
_Static_assert(XR_XIR_HASH_KEYS == 169 && XR_XIR_HASH_VALUES == 170 && XR_XIR_MAP_ENTRIES == 171 &&
    XR_XIR_OP_COUNT == 193 && XR_XIR_CHECKED_CONTRACT == 90, "ordered projection wire identities");
static void projection_packets(const XrXirCompileContext *context) {
    const uint8_t *valid[] = {packet_064_current, packet_065_current,
        packet_066_current, packet_067_current};
    const size_t sizes[] = {sizeof(packet_064_current), sizeof(packet_065_current),
        sizeof(packet_066_current), sizeof(packet_067_current)};
    for (uint32_t i = 0; i < 4; ++i) {
        XrXirArtifact *artifact = NULL; XrXirCheckedPacket packet = {0};
        CHECK(xr_xir_compile_checked_read(context, valid[i], sizes[i], &artifact, NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_write(artifact, &packet, NULL) == XR_XIR_OK);
        CHECK(packet.length == sizes[i] && !memcmp(packet.bytes, valid[i], sizes[i]));
        xr_xir_compile_artifact_free(artifact); xr_xir_compile_checked_packet_free(&packet);
    }
    const uint8_t *invalid[] = {packet_068_current, packet_069_current,
        packet_070_current, packet_071_current, packet_072_current};
    const size_t bad_sizes[] = {sizeof(packet_068_current), sizeof(packet_069_current),
        sizeof(packet_070_current), sizeof(packet_071_current), sizeof(packet_072_current)};
    for (uint32_t i = 0; i < 5; ++i) {
        XrXirArtifact *artifact = NULL;
        CHECK(xr_xir_compile_checked_read(context, invalid[i], bad_sizes[i], &artifact, NULL) == XR_XIR_BAD_TYPE && !artifact);
    }
    XrCompileResourceStats before = {0}, after = {0};
    CHECK(xr_compile_resources_stats(context->resources, &before) == XR_COMPILE_RESOURCE_OK);
    for (uint32_t occupied = 0; occupied < 2; ++occupied) {
        XrXirArtifact *artifact = occupied ? (XrXirArtifact *)(uintptr_t)1 : NULL, *saved = artifact;
        CHECK(xr_xir_compile_checked_read(context, map_index77_golden, sizeof(map_index77_golden), &artifact, NULL) ==
            XR_XIR_BAD_STRUCTURE && artifact == saved);
    }
    CHECK(xr_compile_resources_stats(context->resources, &after) == XR_COMPILE_RESOURCE_OK);
    CHECK(before.allocation_count == after.allocation_count && before.allocated_bytes == after.allocated_bytes &&
        before.live_bytes == after.live_bytes && before.peak_bytes == after.peak_bytes);
}
static void projection_rejections(const XrXirCompileContext *context) {
    const char *sources[] = {
        "export fn entry()->Array<string>{const m=#{1:2}\nreturn m.keys()}",
        "export fn entry()->Array<i64>{const m=#{1:\"a\"}\nreturn m.values()}",
        "export fn entry()->Array<i64>{const s=#[1]\nreturn s.keys()}",
        "export fn entry()->Array<i64>{const m=#{1:2}\nreturn m.keys(1)}",
        "export fn entry()->Array<(i64,i64)>{const m=#{1:2}\nreturn m.entries<i64>()}"};
    for (uint32_t i = 0; i < 5; ++i) {
        XrXirSourceResult result = {0};
        CHECK(hash_source_check(context, sources[i], &result) == XR_XIR_BAD_TYPE && !result.checked && !result.snapshot);
    }
}
#endif // P09_HASH_PROJECTION_PACKETS_H
