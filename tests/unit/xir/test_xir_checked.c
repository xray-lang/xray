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
#include "xir_callable_fixture.h"
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
    _Static_assert(XR_XIR_CHECKED_SCHEMA == 3 && XR_XIR_CHECKED_CONTRACT == 8 && XR_XIR_OP_COUNT == 51, "packet revision");
    _Static_assert(XR_XIR_FUNCTION_REF == 49 && XR_XIR_CALL_INDIRECT == 50, "callable wire operations");
    _Static_assert(XR_XIR_UNIT == 0 && XR_XIR_BOOL == 1 && XR_XIR_I64 == 2 && XR_XIR_STRING == 3 && XR_XIR_ATOMIC_I64 == 4, "wire type identities");
    for (unsigned i = 0; i < 25; ++i) CHECK((unsigned) identities[i] == i + 1);
    XrXirInstruction ops[] = {{XR_XIR_CONST_I64, XR_XIR_I64, {0}, {0}, INT64_MIN},
                             {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0}};
    XrXirBlock block = {0, 2};
    XrXirFunction function = {"n", 1, NULL, 0, XR_XIR_I64, &block, 1, ops, 2, NULL, 0};
    XrXirModule built = {XR_XIR_BUILT, &function, 1, NULL, NULL, NULL};
    XrXirArtifact *checked = NULL, *decoded = NULL;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet;
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == 177 && packet.bytes[24] == 113);
    CHECK(packet.bytes[97] == 2 && packet.bytes[101] == XR_XIR_CONST_I64);
    for (unsigned i = 0; i < 7; ++i) CHECK(packet.bytes[125 + i] == 0);
    CHECK(packet.bytes[132] == 128);
    /* Independent fixed little-endian fixture, including the signed minimum. */
    const uint8_t expected_digest[32] = {
        0xdb, 0x77, 0xc0, 0x59, 0x3e, 0x01, 0xa6, 0x5f, 0x53, 0x4f, 0x8c, 0x77, 0xab, 0x0b, 0xbc, 0xd7,
        0xdc, 0x3f, 0x8a, 0x54, 0x70, 0x9a, 0xee, 0x02, 0xfc, 0x9c, 0x21, 0x01, 0xa5, 0x26, 0x7f, 0xb5};
    CHECK(!memcmp(packet.bytes + 32, expected_digest, 32));
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    CHECK(xr_xir_artifact_module(decoded)->functions[0].instructions[0].immediate == INT64_MIN);
    xr_xir_artifact_free(decoded); xr_xir_artifact_free(checked); xr_xir_checked_packet_free(&packet);
}
static void callable_contracts(void) {
    XrXirArtifact *checked = callable_fixture(), *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    const uint32_t signature_wire[] = {3, 1, 2, 0, 3, 0, 1, 256, 0, 256, 0, 0, 0, 0};
    CHECK(packet.length >= sizeof(signature_wire));
    for (size_t i = 0; i < sizeof(signature_wire) / sizeof(signature_wire[0]); ++i)
        for (unsigned byte = 0; byte < 4; ++byte)
            CHECK(packet.bytes[packet.length - sizeof(signature_wire) + i * 4 + byte] ==
                (uint8_t) (signature_wire[i] >> (byte * 8)));
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked); memset(packet.bytes, 0xCC, packet.length); xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(decoded, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    XrXirModule module = *xr_xir_artifact_module(closed);
    CHECK(!module.generics && module.callables->count == 3);
    const XrXirCallableSignature *nested = xr_xir_callable_signature(module.callables, (XrXirType) 257);
    CHECK(nested && nested->parameter_count == 1 && nested->parameters[0].type == 256 && nested->result == 256);
    CHECK(module.functions[1].result == XR_XIR_CALLABLE_TYPE_BASE + 1);
    CHECK(!xr_xir_type_satisfies(&module, 0, (XrXirType) XR_XIR_CALLABLE_TYPE_BASE, XR_XIR_CONSTRAINT_SENDABLE));
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK && lowered);
    CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(lowered);
    XrXirCallableSignature signatures[3]; memcpy(signatures, module.callables->signatures, sizeof(signatures));
    XrXirCallableParameter parameter = signatures[0].parameters[0]; signatures[0].parameters = &parameter;
    XrXirCallableTypes types = {signatures, 3}; module.callables = &types;
    for (unsigned attack = 0; attack < 10; ++attack) {
        XrXirCallableSignature saved = signatures[2];
        if (attack == 0) signatures[0].flags = 1;
        if (attack == 1) parameter.mode = 1;
        if (attack == 2) parameter.type = XR_XIR_UNIT;
        if (attack == 3) parameter.type = (XrXirType) XR_XIR_CALLABLE_TYPE_BASE;
        if (attack == 4) parameter.type = (XrXirType) (XR_XIR_CALLABLE_TYPE_BASE + 1);
        if (attack == 5) parameter.type = (XrXirType) 255;
        if (attack == 6) parameter.type = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
        if (attack == 7) signatures[2] = signatures[0];
        if (attack == 8) types.count = 0;
        if (attack == 9) signatures[2].parameters = &parameter;
        CHECK(xr_xir_verify(&module, NULL, NULL) != XR_XIR_OK);
        parameter = (XrXirCallableParameter) {XR_XIR_I64, 0};
        signatures[0].flags = 0; signatures[2] = saved; types.count = 3;
    }
    CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_OK);
    XrXirBudget budget = xr_xir_default_budget(); budget.work = 3;
    CHECK(xr_xir_verify(&module, &budget, NULL) == XR_XIR_BUDGET);
    budget = xr_xir_default_budget(); budget.parameters = 1;
    CHECK(xr_xir_verify(&module, &budget, NULL) == XR_XIR_BUDGET);
    xr_xir_artifact_free(closed);
}
static void function_ir_rejections(void) {
    XrXirArtifact *checked = function_ir_fixture();
    XrXirModule module = *xr_xir_artifact_module(checked);
    XrXirFunction functions[3]; memcpy(functions, module.functions, sizeof(functions));
    XrXirInstruction ops[4]; memcpy(ops, functions[1].instructions, sizeof(ops));
    uint32_t argument = 1;
    functions[1].instructions = ops; functions[1].operands = &argument; module.functions = functions;
    for (unsigned attack = 0; attack < 11; ++attack) {
        XrXirInstruction saved[4]; memcpy(saved, ops, sizeof(saved));
        if (attack == 0) ops[0].type = (XrXirType) 257;
        if (attack == 1) ops[0].immediate = 99;
        if (attack == 2) ops[0].immediate = 0;
        if (attack == 3) ops[0].immediate = 1;
        if (attack == 4) ops[2].immediate = 2;
        if (attack == 5) ops[2].immediate = 3;
        if (attack == 6) ops[2].args[1] = 0;
        if (attack == 7) argument = 0;
        if (attack == 8) ops[2].type = XR_XIR_BOOL;
        if (attack == 9) ops[0].targets[1] = 2;
        if (attack == 10) ops[2].immediate = -1;
        CHECK(xr_xir_verify(&module, NULL, NULL) != XR_XIR_OK);
        memcpy(ops, saved, sizeof(ops)); argument = 1;
    }
    CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_OK);
    XrXirArtifact *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    const XrXirModule *specialized = xr_xir_artifact_module(closed);
    CHECK(!specialized->generics && specialized->function_count == 3);
    CHECK(specialized->functions[1].instructions[0].op == XR_XIR_FUNCTION_REF);
    CHECK(specialized->functions[2].parameters[0] == XR_XIR_I64);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(lowered); xr_xir_artifact_free(closed);
    xr_xir_artifact_free(checked);
}
int main(void) {
    callable_contracts(); function_ir_rejections();
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
