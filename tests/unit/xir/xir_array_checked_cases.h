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
#include "xir_array_metadata_fixture.h"

static void array_checked_cases(void) {
    XirArrayMetadataFixture f; xir_array_metadata_init(&f);
    XrXirArtifact *checked = NULL, *decoded = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_check(&f.module, NULL, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked); memset(&f, 0xCC, sizeof(f));
    /* Schema-derived offsets: first record of entry, its operand table, slot. */
    const size_t entry = 281, operands = 845, slot = 1112;
    CHECK(packet.length == 1196 && packet.bytes[entry] == XR_XIR_CONST_INT &&
        packet.bytes[entry + 2 * 40] == XR_XIR_ARRAY_NEW && packet.bytes[operands + 8] == 3);
    uint8_t original[1196]; memcpy(original, packet.bytes, sizeof(original));
    for (uint32_t attack = 0; attack < 20; ++attack) {
        if (attack == 0) put32(packet.bytes + entry + 2 * 40, XR_XIR_OP_COUNT);
        if (attack == 1) put32(packet.bytes + entry + 2 * 40 + 4, 257);
        if (attack == 2) put32(packet.bytes + entry + 2 * 40 + 8, 1);
        if (attack == 3) put32(packet.bytes + entry + 9 * 40 + 12, 2);
        if (attack == 4) put32(packet.bytes + operands + 8, 2);
        if (attack == 5) put32(packet.bytes + operands + 16, 12);
        if (attack == 6) put32(packet.bytes + entry + 5 * 40 + 8, 2);
        if (attack == 7) put32(packet.bytes + entry + 6 * 40 + 24, 999);
        if (attack == 8) put32(packet.bytes + entry + 12 * 40 + 12, 1);
        if (attack == 9) put32(packet.bytes + entry + 8 * 40 + 4, XR_XIR_BOOL);
        if (attack == 10) put32(packet.bytes + entry + 8 * 40 + 12, UINT32_MAX);
        if (attack == 11 || attack == 12) {
            put32(packet.bytes + entry + 11 * 40, attack == 11 ? XR_XIR_COPY : XR_XIR_LOCAL_READ);
            put32(packet.bytes + entry + 11 * 40 + 4, 256);
            put32(packet.bytes + entry + 11 * 40 + 8, 5);
            put32(packet.bytes + entry + 11 * 40 + 12, 0);
        }
        if (attack == 13) {
            put32(packet.bytes + entry + 11 * 40, XR_XIR_CELL_WRITE);
            put32(packet.bytes + entry + 11 * 40 + 4, XR_XIR_UNIT);
            put32(packet.bytes + entry + 11 * 40 + 8, 4);
            put32(packet.bytes + entry + 11 * 40 + 12, 5);
        }
        if (attack == 14) put32(packet.bytes + entry + 13 * 40 + 8, 5);
        if (attack == 15) put32(packet.bytes + 1120, 257);
        if (attack == 16) { put32(packet.bytes + slot + 8, 0); put32(packet.bytes + operands + 8, 6); }
        if (attack == 17) put32(packet.bytes + 12, 14);
        if (attack == 18) put32(packet.bytes + entry + 6 * 40 + 8, 1);
        if (attack == 19) put32(packet.bytes + 116 + 8, 1);
        digest_packet(&packet);
        rejected(packet.bytes, packet.length);
        memcpy(packet.bytes, original, sizeof(original));
    }
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_artifact_verify(decoded, NULL, NULL) == XR_XIR_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(decoded, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    const XrXirFunctionLayout *layout = xr_xir_artifact_layout(lowered, 1);
    CHECK(layout->offsets[5] == UINT32_MAX && layout->offsets[6] == UINT32_MAX &&
        layout->owned_count == 3 && layout->outgoing_count == 2);
    xr_xir_artifact_free(lowered);
}
#endif // XIR_ARRAY_CHECKED_CASES_H
