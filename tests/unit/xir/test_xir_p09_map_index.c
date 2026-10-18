/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_map_index.c - Real Source indexing through owned stage replay
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_hash_source_check.h"
#include "p09_map_index_input.h"
#include "xir_runtime_allocations.h"
#include "p09_map_index_program.h"
#include "composed_wire/packet_073.inc.c"
#include "composed_wire/packet_074.inc.c"
#include "composed_wire/packet_075.inc.c"
#include "hash_protocol28_inputs/xir_hash_members76_golden.h"
_Static_assert(XR_XIR_MAP_INDEX == 168 && XR_XIR_OP_COUNT == 193 && XR_XIR_CHECKED_CONTRACT == 90,
    "required Map lookup has its own admitted wire identity");
static void index_packet(const XrXirCompileContext *context) {
    XrXirArtifact *artifact = NULL; XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_read(context, packet_073_current, sizeof(packet_073_current), &artifact, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(artifact, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == sizeof(packet_073_current) && !memcmp(packet.bytes, packet_073_current, packet.length));
    xr_xir_compile_artifact_free(artifact); xr_xir_compile_checked_packet_free(&packet);
    const uint8_t *invalid[] = {packet_074_current, packet_075_current};
    const size_t lengths[] = {sizeof(packet_074_current), sizeof(packet_075_current)};
    for (uint32_t i = 0; i < 2; ++i) {
        artifact = NULL;
        CHECK(xr_xir_compile_checked_read(context, invalid[i], lengths[i], &artifact, NULL) == XR_XIR_BAD_TYPE && !artifact);
    }
    XrCompileResourceStats before = {0}, after = {0};
    CHECK(xr_compile_resources_stats(context->resources, &before) == XR_COMPILE_RESOURCE_OK);
    for (uint32_t occupied = 0; occupied < 2; ++occupied) {
        artifact = occupied ? (XrXirArtifact *)(uintptr_t)1 : NULL;
        XrXirArtifact *saved = artifact;
        CHECK(xr_xir_compile_checked_read(context, hash_members76_golden, sizeof(hash_members76_golden), &artifact, NULL) ==
            XR_XIR_BAD_STRUCTURE && artifact == saved);
    }
    CHECK(xr_compile_resources_stats(context->resources, &after) == XR_COMPILE_RESOURCE_OK);
    CHECK(before.allocation_count == after.allocation_count && before.allocated_bytes == after.allocated_bytes &&
        before.live_bytes == after.live_bytes && before.peak_bytes == after.peak_bytes);
}
static void index_rejections(const XrXirCompileContext *context) {
    const char *sources[] = {
        "export fn bad()->i64{const m=#{1:9}\nreturn m[true]}",
        "export fn bad(){var m=#{1:9}\nm[true]=2}",
        "export fn bad(){var m=#{1:9}\nm[1]=true}",
        "export fn bad(){const m=#{1:9}\nm[1]=2}",
        "export fn bad()->i64{const s=#[1]\nreturn s[1]}"
    };
    for (uint32_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
        XrXirSourceResult source = {0};
        CHECK(hash_source_check(context, sources[i], &source) == XR_XIR_BAD_TYPE);
        CHECK(!source.checked && !source.snapshot);
    }
    XrXirFaultDetail detail = {431, 0, 0, 0};
    CHECK(xr_xir_fault_runtime_valid(detail));
    detail.reserved = 1; CHECK(!xr_xir_fault_panic_valid(detail));
    detail.reserved = 0; detail.index = 1; CHECK(!xr_xir_fault_panic_valid(detail));
    detail.index = 0; detail.length = 1; CHECK(!xr_xir_fault_panic_valid(detail));
}
int main(int argc, char **argv) {
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    index_packet(&owner.context);
    if (argc != 2) index_rejections(&owner.context);
    XrXirSourceResult source = {0};
    CHECK(hash_source_check(&owner.context, index_source, &source) == XR_XIR_OK);
    XrXirArtifact *lowered = hash_source_replay(&owner.context, &source);
    uint32_t entries[INDEX_COUNT]; index_entries(xr_xir_compile_artifact_module(lowered), entries);
    if (argc == 2) {
        XrXirCSource generated = {0};
        CHECK(xr_xir_compile_emit_c(lowered, "p09_map_index", 1048576, &generated) == XR_XIR_OK);
        CHECK(!strstr(generated.text, "({")); FILE *file = fopen(argv[1], "wb"); CHECK(file);
        CHECK(fwrite(generated.text, 1, generated.length, file) == generated.length);
        CHECK(fprintf(file, "\nconst uint32_t p09_map_index_ids[%u]={", INDEX_COUNT) > 0);
        for (uint32_t i = 0; i < INDEX_COUNT; ++i) CHECK(fprintf(file, "%s%u", i ? "," : "", entries[i]) > 0);
        CHECK(fputs("};\n", file) >= 0 && !fclose(file));
        xr_xir_compile_c_source_free(&generated); xr_xir_compile_artifact_free(lowered);
    } else {
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
        index_program_run(program, entries); CHECK(!runtime_live && !runtime_bytes);
    }
    source_fixture_owner_free(&owner);
    puts("Map index insertion, exact lookup, E0431 and evaluation order PASS"); return 0;
}
