/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_set_algebra_packets.h - Independently framed Set roles and exact types
 */
#ifndef P09_SET_ALGEBRA_PACKETS_H
#define P09_SET_ALGEBRA_PACKETS_H
#include "composed_wire/packet_096.inc.c"
#include "composed_wire/packet_097.inc.c"
#include "composed_wire/packet_098.inc.c"
#include "composed_wire/packet_099.inc.c"
#include "composed_wire/packet_100.inc.c"
#include "composed_wire/packet_101.inc.c"
#include "composed_wire/packet_102.inc.c"
#include "composed_wire/packet_103.inc.c"
#include "composed_wire/packet_104.inc.c"
#include "composed_wire/packet_105.inc.c"
#include "composed_wire/packet_106.inc.c"
#include "composed_wire/packet_107.inc.c"
#include "hash_protocol28_inputs/xir_hash_projection78_golden.h"
_Static_assert(XR_XIR_SET_UNION == 172 && XR_XIR_SET_IS_SUPERSET == 177 &&
    XR_XIR_OP_COUNT == 193 && XR_XIR_CHECKED_CONTRACT == 90, "binary Set wire identities");
static void algebra_packets(const XrXirCompileContext *context) {
    const uint8_t *valid[] = {packet_096_current, packet_097_current, packet_098_current,
        packet_099_current, packet_100_current, packet_101_current};
    const size_t sizes[] = {sizeof(packet_096_current), sizeof(packet_097_current), sizeof(packet_098_current),
        sizeof(packet_099_current), sizeof(packet_100_current), sizeof(packet_101_current)};
    for (uint32_t i = 0; i < 6; ++i) {
        XrXirArtifact *artifact = NULL; XrXirCheckedPacket packet = {0};
        CHECK(xr_xir_compile_checked_read(context, valid[i], sizes[i], &artifact, NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_write(artifact, &packet, NULL) == XR_XIR_OK);
        CHECK(packet.length == sizes[i] && !memcmp(packet.bytes, valid[i], sizes[i]));
        xr_xir_compile_artifact_free(artifact); xr_xir_compile_checked_packet_free(&packet);
    }
    const uint8_t *invalid[] = {packet_102_current, packet_103_current, packet_104_current,
        packet_105_current, packet_106_current, packet_107_current};
    const size_t bad_sizes[] = {sizeof(packet_102_current), sizeof(packet_103_current), sizeof(packet_104_current),
        sizeof(packet_105_current), sizeof(packet_106_current), sizeof(packet_107_current)};
    for (uint32_t i = 0; i < 6; ++i) {
        XrXirArtifact *artifact = NULL;
        CHECK(xr_xir_compile_checked_read(context, invalid[i], bad_sizes[i], &artifact, NULL) == XR_XIR_BAD_TYPE && !artifact);
    }
    XrCompileResourceStats before = {0}, after = {0};
    CHECK(xr_compile_resources_stats(context->resources, &before) == XR_COMPILE_RESOURCE_OK);
    for (uint32_t occupied = 0; occupied < 2; ++occupied) {
        XrXirArtifact *artifact = occupied ? (XrXirArtifact *)(uintptr_t)1 : NULL, *saved = artifact;
        CHECK(xr_xir_compile_checked_read(context, hash_projection78_keys, sizeof(hash_projection78_keys), &artifact, NULL) ==
            XR_XIR_BAD_STRUCTURE && artifact == saved);
    }
    CHECK(xr_compile_resources_stats(context->resources, &after) == XR_COMPILE_RESOURCE_OK);
    CHECK(before.allocation_count == after.allocation_count && before.allocated_bytes == after.allocated_bytes &&
        before.live_bytes == after.live_bytes && before.peak_bytes == after.peak_bytes);
}
static void algebra_rejections(const XrXirCompileContext *context) {
    const char *sources[] = {
        "export fn f()->Set<i64>{const a=#[1]\nreturn a.union(#[\"x\"])}",
        "export fn f()->Set<i64>{const a=#[1]\nreturn a.union()}",
        "export fn f()->Set<i64>{const a=#[1]\nvar b=#[2]\nreturn a.union(ref b)}",
        "export fn f()->bool{const a=#[1]\nreturn a.isSubset<i64>(a)}",
        "export fn f()->Set<i64>{const a=#[1]\nreturn a.isSubset(a)}",
        "fn f<T>(a:Set<T>,b:Set<T>)->Set<T>{return a.union(b)}",
        "fn f<T:Equal>(a:Set<T>,b:Set<T>)->Set<T>{return a.union(b)}"};
    for (uint32_t i = 0; i < 7; ++i) {
        XrXirSourceResult result = {0};
        CHECK(hash_source_check(context, sources[i], &result) == XR_XIR_BAD_TYPE && !result.checked && !result.snapshot);
    }
}
#endif // P09_SET_ALGEBRA_PACKETS_H
