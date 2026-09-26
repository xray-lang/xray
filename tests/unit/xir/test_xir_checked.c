/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_checked.c - Hostile packet and independent ownership admission
 *
 * KEY CONCEPT:
 *   A valid digest does not confer declaration or execution authority.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_internal.h"
#include "base/xsha256.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_checked_fixture.h"
static void digest_packet(XrXirCheckedPacket *p) {
    XrSHA256Context sha; xr_sha256_init(&sha);
    xr_sha256_update(&sha, p->bytes, 32);
    xr_sha256_update(&sha, p->bytes + 64, p->length - 64);
    xr_sha256_final(&sha, p->bytes + 32);
}
static void put32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t) (n >> (i * 8));
}
static void rejected(const void *bytes, size_t length) {
    XrXirArtifact *artifact = (XrXirArtifact *) (uintptr_t) 1;
    XrXirDiagnostic diagnostic = {0};
    CHECK(xr_xir_checked_read(bytes, length, NULL, &artifact, &diagnostic) != XR_XIR_OK);
    CHECK(!artifact && diagnostic.status != XR_XIR_OK);
}
static void boundaries(XrXirCheckedPacket *packet) {
    for (size_t i = 0; i < packet->length; ++i) rejected(packet->bytes, i);
    for (size_t i = 0; i < packet->length; ++i) {
        packet->bytes[i] ^= 1; rejected(packet->bytes, packet->length); packet->bytes[i] ^= 1;
    }
    uint8_t *trailing = xr_malloc(packet->length + 1); CHECK(trailing);
    memcpy(trailing, packet->bytes, packet->length); trailing[packet->length] = 0;
    rejected(trailing, packet->length + 1);
    XrXirCheckedPacket extra = {trailing, packet->length + 1};
    put32(trailing + 24, (uint32_t) (extra.length - 64)); digest_packet(&extra);
    rejected(trailing, extra.length); xr_free(trailing);
    const size_t fields[] = {8, 12, 16, 20, 64, 68, 72};
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        uint8_t saved[4]; memcpy(saved, packet->bytes + fields[i], 4);
        put32(packet->bytes + fields[i], UINT32_MAX); digest_packet(packet);
        rejected(packet->bytes, packet->length);
        memcpy(packet->bytes + fields[i], saved, 4); digest_packet(packet);
    }
    XrXirBudget budget = xr_xir_default_budget(); XrXirArtifact *decoded = NULL;
    for (unsigned i = 0; i < 9; ++i) {
        budget = xr_xir_default_budget();
        if (i == 0) budget.functions = 0;
        if (i == 1) budget.blocks = 0;
        if (i == 2) budget.instructions = 0;
        if (i == 3) budget.metadata_bytes = packet->length - 1;
        if (i == 4) budget.work = packet->length * 4 - 1;
        if (i == 5) budget.scratch_bytes = 0;
        if (i == 6) budget.metadata_bytes = packet->length;
        if (i == 7) budget.functions = 7;
        if (i == 8) budget.parameters = 1;
        CHECK(xr_xir_checked_read(packet->bytes, packet->length, &budget, &decoded, NULL) == XR_XIR_BUDGET);
        CHECK(!decoded);
    }
}
static uint32_t wire32(const uint8_t *p) {
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}
static void declaration_attacks(XrXirCheckedPacket *packet) {
    const uint8_t *p = packet->bytes; size_t at = 72;
    uint32_t functions = wire32(p + 64);
    for (uint32_t f = 0; f < functions; ++f) {
        at += 4 + wire32(p + at);
        at += 4 + (size_t) wire32(p + at) * 4;
        at += 4;
        at += 4 + (size_t) wire32(p + at) * 8;
        at += 4 + (size_t) wire32(p + at) * 32;
        at += 4 + (size_t) wire32(p + at) * 4;
    }
    size_t declarations = at, dependencies = 0, initializer = 0;
    uint32_t modules = wire32(p + at), slots = wire32(p + at + 4);
    at += 20;
    for (uint32_t m = 0; m < modules; ++m) {
        at += 4 + wire32(p + at);
        uint32_t count = wire32(p + at); at += 4;
        if (!m) dependencies = at;
        at += (size_t) count * 4;
        if (!m) initializer = at;
        at += 4;
    }
    size_t identities = at, slot_data = at + (size_t) functions * 8;
    size_t literal_data = slot_data + (size_t) slots * 12;
    const size_t offsets[] = {declarations, declarations + 12, declarations + 16,
        dependencies + 4, initializer, identities + 4 * 8 + 4, slot_data, literal_data + 4};
    const uint32_t values[] = {UINT32_MAX, 1, 0, 1, 3, 0, 1, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        CHECK(offsets[i] + 4 <= packet->length);
        uint8_t saved[4]; memcpy(saved, packet->bytes + offsets[i], 4);
        put32(packet->bytes + offsets[i], values[i]); digest_packet(packet);
        rejected(packet->bytes, packet->length);
        memcpy(packet->bytes + offsets[i], saved, 4); digest_packet(packet);
    }
}
static void semantic_attacks(XrXirCheckedPacket *packet) {
    /* First function: name 9 bytes, no parameters, one block, four instructions. */
    const size_t offsets[] = {89, 97, 101, 105, 109, 113, 117, 125, 141, 149, 213};
    const uint32_t values[] = {99, 1, 3, UINT32_MAX, XR_XIR_SCALAR_COPY, 99, 9, 1, XR_XIR_CONST_I64, 4, 99};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        uint8_t saved[4]; memcpy(saved, packet->bytes + offsets[i], 4);
        put32(packet->bytes + offsets[i], values[i]); digest_packet(packet);
        rejected(packet->bytes, packet->length);
        memcpy(packet->bytes + offsets[i], saved, 4); digest_packet(packet);
    }
}
static void byte_order(void) {
    const XrXirOp identities[] = {XR_XIR_CONST_BOOL, XR_XIR_CONST_I64, XR_XIR_CONST_STRING,
        XR_XIR_SLOT_LOAD, XR_XIR_SLOT_INIT, XR_XIR_SLOT_STORE, XR_XIR_ATOMIC_I64_NEW,
        XR_XIR_ATOMIC_I64_LOAD, XR_XIR_ATOMIC_I64_FETCH_ADD, XR_XIR_COPY, XR_XIR_SCALAR_COPY,
        XR_XIR_OWNED_RETAIN, XR_XIR_CONCAT_STRING, XR_XIR_OUTPUT, XR_XIR_WRITE_STREAM,
        XR_XIR_PRINT, XR_XIR_ADD_I64, XR_XIR_EQ_I64, XR_XIR_LT_I64, XR_XIR_CALL,
        XR_XIR_SUSPEND, XR_XIR_THROW, XR_XIR_JUMP, XR_XIR_BRANCH, XR_XIR_RETURN};
    _Static_assert(XR_XIR_CHECKED_SCHEMA == 2 && XR_XIR_CHECKED_CONTRACT == 2 && XR_XIR_OP_COUNT == 26, "packet revision");
    _Static_assert(XR_XIR_UNIT == 0 && XR_XIR_BOOL == 1 && XR_XIR_I64 == 2 && XR_XIR_STRING == 3 && XR_XIR_ATOMIC_I64 == 4, "wire type identities");
    for (unsigned i = 0; i < 25; ++i) CHECK((unsigned) identities[i] == i + 1);
    XrXirInstruction ops[] = {{XR_XIR_CONST_I64, XR_XIR_I64, {0}, {0}, INT64_MIN},
                             {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0}};
    XrXirBlock block = {0, 2};
    XrXirFunction function = {"n", 1, NULL, 0, XR_XIR_I64, &block, 1, ops, 2, NULL, 0};
    XrXirModule built = {XR_XIR_BUILT, &function, 1, NULL, NULL};
    XrXirArtifact *checked = NULL, *decoded = NULL;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet;
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == 173 && packet.bytes[24] == 109);
    CHECK(packet.bytes[97] == 2 && packet.bytes[101] == XR_XIR_CONST_I64);
    for (unsigned i = 0; i < 7; ++i) CHECK(packet.bytes[125 + i] == 0);
    CHECK(packet.bytes[132] == 128);
    /* Independent fixed little-endian fixture, including the signed minimum. */
    const uint8_t expected_digest[32] = {
        0x35,0x95,0xf6,0xad,0x67,0xf7,0xb7,0x43,0xa4,0xab,0x17,0x85,0x18,0x9c,0xa8,0x3e,
        0xfb,0x4b,0xb7,0x50,0x97,0x7a,0x91,0x2a,0x6d,0x60,0x2c,0xca,0xb0,0x00,0xab,0xdd};
    CHECK(!memcmp(packet.bytes + 32, expected_digest, 32));
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    CHECK(xr_xir_artifact_module(decoded)->functions[0].instructions[0].immediate == INT64_MIN);
    xr_xir_artifact_free(decoded); xr_xir_artifact_free(checked); xr_xir_checked_packet_free(&packet);
}
int main(void) {
    XrXirArtifact *checked = checked_fixture(), *decoded = NULL;
    XrXirCheckedPacket packet = {0}, second = {0};
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    CHECK(xr_xir_checked_write(checked, NULL, &second, NULL) == XR_XIR_OK);
    CHECK(packet.length == second.length && !memcmp(packet.bytes, second.bytes, packet.length));
    xr_xir_checked_packet_free(&second);
    checked->module.stage = XR_XIR_BUILT;
    CHECK(xr_xir_checked_write(checked, NULL, &second, NULL) == XR_XIR_BAD_STAGE && !second.bytes);
    checked->module.stage = XR_XIR_LOWERED;
    CHECK(xr_xir_checked_write(checked, NULL, &second, NULL) == XR_XIR_BAD_STAGE && !second.bytes);
    checked->module.stage = XR_XIR_CHECKED;
    checked->target.architecture = XR_XIR_ARCH_X86_64;
    CHECK(xr_xir_checked_write(checked, NULL, &second, NULL) == XR_XIR_BAD_LAYOUT && !second.bytes);
    checked->target.architecture = 0;
    boundaries(&packet); semantic_attacks(&packet); declaration_attacks(&packet); byte_order();
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    CHECK(xr_xir_checked_write(decoded, NULL, &second, NULL) == XR_XIR_OK);
    CHECK(packet.length == second.length && !memcmp(packet.bytes, second.bytes, packet.length));
    xr_xir_artifact_free(checked);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_checked_packet_free(&packet);
    xr_xir_checked_packet_free(&second);
    CHECK(xr_xir_artifact_verify(decoded, NULL, NULL) == XR_XIR_OK);
    const XrXirDeclarations *d = xr_xir_artifact_module(decoded)->declarations;
    CHECK(d->literal_count == 5 && d->literals[0].length == 5 && d->literals[0].bytes[1] == 0);
    CHECK(d->literals[4].length == 0 && !d->literals[4].bytes);
    xr_xir_artifact_free(decoded);
    puts("Checked packet canonical bytes, hostile input and independent ownership passed");
    return 0;
}
