/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_members.c - Source hash operations survive every owned stage
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "xir/xxir_types.h"
#include "p09_hash_members_source.h"
#include "p09_hash_members_program.h"
#include "xir_hash_members84_golden.h"
#include "hash_protocol28_inputs/xir_hash75_golden.h"
_Static_assert(XR_XIR_MAP_NEW == 159 && XR_XIR_LOCAL_NEW == 34 &&
    XR_XIR_MAP_SET == 164 && XR_XIR_MAP_GET == 161, "independent hash wire ordinals");
static void members_packet(const XrXirCompileContext *context) {
    XrXirArtifact *artifact = NULL; XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_read(context, hash_members84_golden,
        sizeof(hash_members84_golden), &artifact, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(artifact, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == sizeof(hash_members84_golden) && !memcmp(packet.bytes, hash_members84_golden, packet.length));
    xr_xir_compile_artifact_free(artifact); xr_xir_compile_checked_packet_free(&packet);
    XrCompileResourceStats before = {0}, after = {0};
    CHECK(xr_compile_resources_stats(context->resources, &before) == XR_COMPILE_RESOURCE_OK);
    for (uint32_t occupied = 0; occupied < 2; ++occupied) {
        artifact = occupied ? (XrXirArtifact *)(uintptr_t)1 : NULL;
        XrXirArtifact *saved = artifact;
        CHECK(xr_xir_compile_checked_read(context, hash75_golden, sizeof(hash75_golden), &artifact, NULL) ==
            XR_XIR_BAD_STRUCTURE && artifact == saved);
    }
    CHECK(xr_compile_resources_stats(context->resources, &after) == XR_COMPILE_RESOURCE_OK);
    CHECK(before.allocation_count == after.allocation_count && before.allocated_bytes == after.allocated_bytes &&
        before.live_bytes == after.live_bytes && before.peak_bytes == after.peak_bytes);
}
int main(int argc, char **argv) {
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    members_packet(&owner.context);
    XrXirSourceResult result = {0}; CHECK(hash_source_check(&owner.context, members_source, &result) == XR_XIR_OK);
    members_query(xr_xir_compile_source_snapshot_view(result.snapshot));
    XrXirArtifact *lowered = hash_source_replay(&owner.context, &result);
    uint32_t entries[5]; members_entries(xr_xir_compile_artifact_module(lowered), entries);
    if (argc == 2) {
        XrXirCSource source = {0};
        CHECK(xr_xir_compile_emit_c(lowered, "p09_hash_members", 1048576, &source) == XR_XIR_OK);
        CHECK(!strstr(source.text, "({")); FILE *file = fopen(argv[1], "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst uint32_t p09_hash_members_ids[5]={%u,%u,%u,%u,%u};\n",
            entries[0], entries[1], entries[2], entries[3], entries[4]) > 0 && !fclose(file));
        xr_xir_compile_c_source_free(&source); xr_xir_compile_artifact_free(lowered);
    } else {
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
        members_program_run(program, entries);
    }
    source_fixture_owner_free(&owner);
    puts("Source hash construction, lookup, COW mutation, generic and nested paths PASS"); return 0;
}
