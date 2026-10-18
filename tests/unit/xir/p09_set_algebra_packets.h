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
#include "xir_set_algebra86_golden.h"
#include "xir_hash_projection78_golden.h"
_Static_assert(XR_XIR_SET_UNION == 167 && XR_XIR_SET_IS_SUPERSET == 172 &&
    XR_XIR_OP_COUNT == 184 && XR_XIR_CHECKED_CONTRACT == 86, "binary Set wire identities");
static void algebra_packets(const XrXirCompileContext *context) {
    const uint8_t *valid[] = {set_algebra86_union, set_algebra86_intersection, set_algebra86_difference,
        set_algebra86_symmetric_difference, set_algebra86_is_subset, set_algebra86_is_superset};
    const size_t sizes[] = {sizeof(set_algebra86_union), sizeof(set_algebra86_intersection), sizeof(set_algebra86_difference),
        sizeof(set_algebra86_symmetric_difference), sizeof(set_algebra86_is_subset), sizeof(set_algebra86_is_superset)};
    for (uint32_t i = 0; i < 6; ++i) {
        XrXirArtifact *artifact = NULL; XrXirCheckedPacket packet = {0};
        CHECK(xr_xir_compile_checked_read(context, valid[i], sizes[i], &artifact, NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_write(artifact, &packet, NULL) == XR_XIR_OK);
        CHECK(packet.length == sizes[i] && !memcmp(packet.bytes, valid[i], sizes[i]));
        xr_xir_compile_artifact_free(artifact); xr_xir_compile_checked_packet_free(&packet);
    }
    const uint8_t *invalid[] = {set_algebra86_bad_receiver, set_algebra86_bad_other_kind, set_algebra86_bad_other_element,
        set_algebra86_bad_set_result, set_algebra86_bad_relation_result, set_algebra86_bad_hashable};
    const size_t bad_sizes[] = {sizeof(set_algebra86_bad_receiver), sizeof(set_algebra86_bad_other_kind), sizeof(set_algebra86_bad_other_element),
        sizeof(set_algebra86_bad_set_result), sizeof(set_algebra86_bad_relation_result), sizeof(set_algebra86_bad_hashable)};
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
