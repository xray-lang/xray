/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_checked_cases.h - Rehashed Array packets do not grant place authority
 *
 * KEY CONCEPT:
 *   Fixed wire offsets attack the semantic reader independently of producers.
 */
#ifndef XIR_ARRAY_CHECKED_CASES_H
#define XIR_ARRAY_CHECKED_CASES_H
#include "xir_construction_fixture.h"
#include "xir_array_metadata_fixture.h"

_Static_assert(XR_XIR_CHECKED_SCHEMA == 28 && XR_XIR_CHECKED_CONTRACT == 73,
    "array checked wire identity");

static void array_checked_const_slot(XrXirCheckedPacket *packet, size_t entry, size_t slot) {
    /* A valid const Array slot must precede the hostile write; a Cell with
     * mutable=0 would fail declaration admission before testing authority. */
    put32(packet->bytes + 116 + 40, XR_XIR_COPY);
    put32(packet->bytes + 116 + 40 + 4, 256);
    put32(packet->bytes + slot + 4, 256);
    put32(packet->bytes + slot + 8, 0);
    for (uint32_t i = 6; i <= 7; ++i) {
        put32(packet->bytes + entry + i * 40, XR_XIR_SLOT_PLACE);
        put32(packet->bytes + entry + i * 40 + 4, 256);
        put32(packet->bytes + entry + i * 40 + 8, 0);
    }
    digest_packet(packet);
    XrXirCompileContext probe = consumer_context_ephemeral(
        (XrCompileResourceLimits){67108864, 8388608, 128000000});
    const XrCompileResourceStats baseline = consumer_context_stats(&probe);
    XrXirArtifact *decoded = NULL;
    CHECK(xr_xir_compile_checked_read(&probe, packet->bytes, packet->length, &decoded, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded); decoded = NULL;
    consumer_context_ephemeral_free(&probe, baseline);
}

static void array_checked_cases(void) {
    XirArrayMetadataFixture f; xir_array_metadata_init(&f);
    XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xir_fixture_check(suite_context, &f.module, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL; memset(&f, 0xCC, sizeof(f));
    /* Independent 28/73 fields: module76 + function records204/665/128;
     * declarations20 + source15 + identities3*36 + slot12 + impl-count4;
     * generics4 + types(12+12+12+20) + construction4 + defaults4 + provenance4 = 1304. */
    const size_t entry = 321, operands = 925, slot = 1216;
    CHECK(packet.length == 1304);
    fprintf(stderr,"array packet actual length=%zu entry=%u array=%u operand=%u\n",packet.length,packet.bytes[entry],packet.bytes[entry+2*40],packet.bytes[operands+8]);
    CHECK(packet.bytes[entry] == XR_XIR_CONST_INT && packet.bytes[116 + 40] == XR_XIR_CELL_NEW &&
        packet.bytes[entry + 2 * 40] == XR_XIR_ARRAY_NEW && packet.bytes[operands + 8] == 3);
    CHECK(packet.bytes[entry + 6 * 40] == XR_XIR_SLOT_LOAD &&
        packet.bytes[entry + 7 * 40] == XR_XIR_CELL_PLACE && packet32(&packet, slot + 4) == 257);
    uint8_t original[1304]; memcpy(original, packet.bytes, sizeof(original));
    for (uint32_t attack = 0; attack < 20; ++attack) {
        if (attack == 0) put32(packet.bytes + entry + 2 * 40, XR_XIR_OP_COUNT);
        if (attack == 1) put32(packet.bytes + entry + 2 * 40 + 4, 257);
        if (attack == 2) put32(packet.bytes + entry + 2 * 40 + 8, 1);
        if (attack == 3) put32(packet.bytes + entry + 10 * 40 + 12, 2);
        if (attack == 4) put32(packet.bytes + operands + 8, 2);
        if (attack == 5) put32(packet.bytes + operands + 16, 13);
        if (attack == 6) put32(packet.bytes + entry + 5 * 40 + 8, 2);
        if (attack == 7) put32(packet.bytes + entry + 6 * 40 + 24, 999);
        if (attack == 8) put32(packet.bytes + entry + 13 * 40 + 12, 1);
        if (attack == 9) put32(packet.bytes + entry + 9 * 40 + 4, XR_XIR_BOOL);
        if (attack == 10) put32(packet.bytes + entry + 9 * 40 + 12, UINT32_MAX);
        if (attack == 11 || attack == 12) {
            put32(packet.bytes + entry + 12 * 40, attack == 11 ? XR_XIR_COPY : XR_XIR_LOCAL_READ);
            put32(packet.bytes + entry + 12 * 40 + 4, 256);
            put32(packet.bytes + entry + 12 * 40 + 8, 5);
            put32(packet.bytes + entry + 12 * 40 + 12, 0);
        }
        if (attack == 13) {
            put32(packet.bytes + entry + 12 * 40, XR_XIR_CELL_WRITE);
            put32(packet.bytes + entry + 12 * 40 + 4, XR_XIR_UNIT);
            put32(packet.bytes + entry + 12 * 40 + 8, 4);
            put32(packet.bytes + entry + 12 * 40 + 12, 5);
        }
        if (attack == 14) put32(packet.bytes + entry + 14 * 40 + 8, 5);
        if (attack == 15) put32(packet.bytes + slot + 8, 257);
        if (attack == 16) {
            array_checked_const_slot(&packet, entry, slot);
            put32(packet.bytes + operands + 8, 6);
        }
        if (attack == 17) put32(packet.bytes + 12, 14);
        if (attack == 18) put32(packet.bytes + entry + 6 * 40 + 8, 1);
        if (attack == 19) put32(packet.bytes + 116 + 8, 1);
        digest_packet(&packet);
        rejected(packet.bytes, packet.length);
        memcpy(packet.bytes, original, sizeof(original));
    }
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(decoded, NULL) == XR_XIR_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);closed=NULL;
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
    const XrXirFunctionLayout *layout = xr_xir_compile_artifact_layout(lowered, 1);
    CHECK(layout->offsets[5] == UINT32_MAX && layout->offsets[7] == UINT32_MAX &&
        layout->offsets[6] != UINT32_MAX && layout->owned_count == 4 && layout->outgoing_count == 2);
    xr_xir_compile_artifact_free(lowered);lowered=NULL;
}
#endif // XIR_ARRAY_CHECKED_CASES_H
