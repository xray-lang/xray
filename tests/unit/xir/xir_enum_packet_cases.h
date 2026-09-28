/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_enum_packet_cases.h - Resigned packet mutations cannot grant payload authority
 */
#ifndef XIR_ENUM_PACKET_CASES_H
#define XIR_ENUM_PACKET_CASES_H
#include "xir_enum_ops_fixture.h"
static void enum_instruction_packet_cases(void) {
    XrXirArtifact *lowered = enum_ops_lowered(false); xr_xir_artifact_free(lowered);
    XrXirArtifact *checked = enum_ops_checked(0, false);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL) == XR_XIR_OK); xr_xir_artifact_free(checked);
    const uint32_t patterns[][8] = {
        {88,2,2,0,0,0,1,0}, {86,256,0,2,0,0,1,0}, {87,2,2,0,0,0,0,0}};
    const uint32_t offsets[] = {12,24,12}, replacements[] = {2,0,1};
    for (uint32_t i = 0; i < 3; ++i) {
        uint8_t expected[32]; for (uint32_t w = 0; w < 8; ++w) put32(expected + 4*w,patterns[i][w]);
        size_t found = 0; unsigned matches = 0;
        for (size_t at = 64; at + 32 <= packet.length; ++at)
            if (!memcmp(packet.bytes+at,expected,32)) { found = at; ++matches; }
        CHECK(matches == 1);
        put32(packet.bytes+found+offsets[i],replacements[i]); digest_packet(&packet);
        rejected(packet.bytes,packet.length);
        memcpy(packet.bytes+found,expected,32); digest_packet(&packet);
        XrXirArtifact *decoded = NULL;
        CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
        xr_xir_artifact_free(decoded);
    }
    xr_xir_checked_packet_free(&packet);
}
#endif // XIR_ENUM_PACKET_CASES_H
