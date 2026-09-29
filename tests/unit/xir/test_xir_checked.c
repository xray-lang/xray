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
#include "xir_types_fixture.h"
#include "xir_struct_ops_fixture.h"
#include "xir_struct_set_fixture.h"
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
        XrXirStatus status = xr_xir_checked_read(packet->bytes, packet->length, &budget, &decoded, NULL);
        if (status != XR_XIR_BUDGET) fprintf(stderr, "budget case %u returned %d\n", i, status);
        CHECK(status == XR_XIR_BUDGET);
        CHECK(!decoded);
    }
}
static uint32_t wire32(const uint8_t *p) {
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}
static uint32_t packet32(const XrXirCheckedPacket *packet, size_t offset) {
    CHECK(offset <= packet->length && packet->length - offset >= 4);
    return wire32(packet->bytes + offset);
}
static void declaration_attacks(XrXirCheckedPacket *packet) {
    size_t at = 72;
    uint32_t functions = packet32(packet, 64);
    for (uint32_t f = 0; f < functions; ++f) {
        at += 4 + packet32(packet, at);
        at += 4 + (size_t) packet32(packet, at) * 4;
        at += 4;
        at += 4 + (size_t) packet32(packet, at) * 16;
        at += 4 + (size_t) packet32(packet, at) * 40;
        at += 4 + (size_t) packet32(packet, at) * 4;
    }
    size_t declarations = at, dependencies = 0, initializer = 0;
    uint32_t modules = packet32(packet, at), slots = packet32(packet, at + 4);
    at += 20;
    for (uint32_t m = 0; m < modules; ++m) {
        at += 4 + packet32(packet, at);
        uint32_t count = packet32(packet, at); at += 4;
        if (!m) dependencies = at;
        at += (size_t) count * 4;
        if (!m) initializer = at;
        at += 4;
    }
    size_t identities = at, slot_data = at + (size_t) functions * 20;
    size_t literal_data = slot_data + (size_t) slots * 12;
    const size_t offsets[] = {declarations, declarations + 12, declarations + 16,
        dependencies + 4, initializer, identities + 4 * 20 + 4, slot_data, literal_data + 4, identities + 4 * 20 + 8};
    const uint32_t values[] = {UINT32_MAX, 1, 0, 1, 3, 0, 1, UINT32_MAX, 1};
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
    const size_t offsets[] = {89, 97, 101, 113, 117, 121, 125, 133, 157, 165, 245};
    const uint32_t values[] = {99, 1, 3, UINT32_MAX, XR_XIR_SCALAR_COPY, 99, 9, 1, XR_XIR_CONST_INT, 4, 99};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        uint8_t saved[4]; memcpy(saved, packet->bytes + offsets[i], 4);
        put32(packet->bytes + offsets[i], values[i]); digest_packet(packet);
        rejected(packet->bytes, packet->length);
        memcpy(packet->bytes + offsets[i], saved, 4); digest_packet(packet);
    }
}
static void byte_order(void) {
    const XrXirOp identities[] = {XR_XIR_CONST_BOOL, XR_XIR_CONST_INT, XR_XIR_CONST_STRING,
        XR_XIR_SLOT_LOAD, XR_XIR_SLOT_INIT, XR_XIR_SLOT_STORE, XR_XIR_ATOMIC_I64_NEW,
        XR_XIR_ATOMIC_I64_LOAD, XR_XIR_ATOMIC_I64_FETCH_ADD, XR_XIR_COPY, XR_XIR_SCALAR_COPY,
        XR_XIR_OWNED_RETAIN, XR_XIR_CONCAT_STRING, XR_XIR_OUTPUT, XR_XIR_WRITE_STREAM,
        XR_XIR_PRINT, XR_XIR_ADD_INT, XR_XIR_EQ_INT, XR_XIR_LT_INT, XR_XIR_CALL,
        XR_XIR_SUSPEND, XR_XIR_THROW, XR_XIR_JUMP, XR_XIR_BRANCH, XR_XIR_RETURN};
    _Static_assert(XR_XIR_CHECKED_SCHEMA == 13 && XR_XIR_CHECKED_CONTRACT == 36 && XR_XIR_OP_COUNT == 104, "packet revision");
    _Static_assert(XR_XIR_PANIC_CATCH == 97 && XR_XIR_PANIC_CODE == 98 && XR_XIR_PANIC_MESSAGE == 99 &&
        XR_XIR_PANIC_INFO == 15, "panic wire identities");
    _Static_assert(XR_XIR_MATCH_FAIL == 89, "match fault wire operation");
    _Static_assert(XR_XIR_ENUM_NEW == 86 && XR_XIR_ENUM_TAG == 87 && XR_XIR_ENUM_GET == 88, "enum wire operations");
    _Static_assert(XR_XIR_STRING_INDEX_OF == 84 && XR_XIR_STRING_LAST_INDEX_OF == 85, "search wire operations");
    _Static_assert(XR_XIR_STRING_CONTAINS == 81 && XR_XIR_STRING_STARTS_WITH == 82 && XR_XIR_STRING_ENDS_WITH == 83, "string predicate wire operations");
    _Static_assert(XR_XIR_STRING_LEN == 78 && XR_XIR_EQ_STRING == 79 && XR_XIR_NE_STRING == 80, "string query wire operations");
    _Static_assert(XR_XIR_STRUCT_NEW == 74 && XR_XIR_STRUCT_GET == 75 && XR_XIR_STRUCT_SET == 76, "struct wire operations");
    _Static_assert(XR_XIR_F32 == 12 && XR_XIR_F64 == 13 && XR_XIR_CONVERT_NUMBER == 54 &&
        XR_XIR_CONST_FLOAT == 55 && XR_XIR_NEG_FLOAT == 56 && XR_XIR_EQ_FLOAT == 57 && XR_XIR_GE_FLOAT == 62, "numeric wire identities");
    _Static_assert(XR_XIR_FUNCTION_REF == 49 && XR_XIR_CALL_INDIRECT == 50, "callable wire operations");
    _Static_assert(XR_XIR_CELL_PLACE == 63 && XR_XIR_SLOT_PLACE == 64 && XR_XIR_ARRAY_NEW == 65 &&
        XR_XIR_ARRAY_GET == 66 && XR_XIR_ARRAY_SET == 67 && XR_XIR_ARRAY_PUSH == 68 &&
        XR_XIR_ARRAY_LEN == 69, "array wire operations");
    _Static_assert(XR_XIR_UNIT == 0 && XR_XIR_BOOL == 1 && XR_XIR_I64 == 2 && XR_XIR_STRING == 3 && XR_XIR_ATOMIC_I64 == 4, "wire type identities");
    for (unsigned i = 0; i < 25; ++i) CHECK((unsigned) identities[i] == i + 1);
    XrXirInstruction ops[] = {{XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, INT64_MIN, {0}},
                             {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirBlock block = {0, 2, 0, 0};
    XrXirFunction function = {"n", 1, NULL, 0, XR_XIR_I64, &block, 1, ops, 2, NULL, 0};
    XrXirModule built = {XR_XIR_BUILT, &function, 1, NULL, NULL, NULL, NULL};
    XrXirArtifact *checked = NULL, *decoded = NULL;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet;
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == 209 && packet.bytes[24] == 145);
    CHECK(packet.bytes[97] == 0 && packet.bytes[101] == 0 && packet.bytes[105] == 2 && packet.bytes[109] == XR_XIR_CONST_INT);
    for (unsigned i = 0; i < 7; ++i) CHECK(packet.bytes[133 + i] == 0);
    CHECK(packet.bytes[140] == 128);
    /* Independent fixed little-endian fixture, including the signed minimum
     * and the block's zero panic handler and cleanup frontier. */
    const uint8_t expected_digest[32] = {
        0x35, 0xdb, 0xf1, 0x26, 0xfb, 0x32, 0x08, 0xc1, 0x98, 0x72, 0x17, 0x70, 0x1a, 0x53, 0x1f, 0x19,
        0x1d, 0x4f, 0x7d, 0xa0, 0xbf, 0x0b, 0x05, 0x80, 0xd6, 0x02, 0x2b, 0x78, 0xbd, 0x65, 0xc1, 0x70};
    CHECK(!memcmp(packet.bytes + 32, expected_digest, 32));
    uint8_t original[209]; memcpy(original, packet.bytes, sizeof(original));
    for (unsigned offset = 141; offset <= 145; offset += 4) {
        put32(packet.bytes + offset, 1); digest_packet(&packet);
        rejected(packet.bytes, packet.length);
        memcpy(packet.bytes, original, sizeof(original));
    }
    put32(packet.bytes+149,XR_XIR_THROW); digest_packet(&packet);
    rejected(packet.bytes,packet.length);
    memcpy(packet.bytes,original,sizeof(original));
    /* An entry block cannot be protected, and a handler must name a block. */
    for (uint32_t handler = 0; handler < 2; ++handler) {
        put32(packet.bytes + 97, handler + 1); digest_packet(&packet);
        rejected(packet.bytes, packet.length);
        memcpy(packet.bytes, original, sizeof(original));
    }
    put32(packet.bytes + 81, XR_XIR_U8); put32(packet.bytes + 113, XR_XIR_U8);
    memset(packet.bytes + 133, 0, 8); put32(packet.bytes + 133, 256); digest_packet(&packet);
    rejected(packet.bytes, packet.length);
    put32(packet.bytes + 133, 255); digest_packet(&packet);
    XrXirArtifact *narrow = NULL;
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &narrow, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(narrow);
    put32(packet.bytes + 12, 30); digest_packet(&packet); rejected(packet.bytes, packet.length);
    memcpy(packet.bytes, original, sizeof(original));
    put32(packet.bytes + 8, 9); digest_packet(&packet); rejected(packet.bytes, packet.length);
    memcpy(packet.bytes, original, sizeof(original));
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    CHECK(xr_xir_artifact_module(decoded)->functions[0].instructions[0].immediate == INT64_MIN);
    xr_xir_artifact_free(decoded); xr_xir_artifact_free(checked); xr_xir_checked_packet_free(&packet);
}
static void callable_contracts(void) {
    XrXirArtifact *checked = callable_fixture(), *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    const uint32_t signature_wire[] = {3, 0,
        1, 0, 1, 2, 0, 3, 0,
        1, 0, 1, 256, 0, 256, 0,
        1, 0, 0, 0, 0};
    CHECK(packet.length >= sizeof(signature_wire));
    for (size_t i = 0; i < sizeof(signature_wire) / sizeof(signature_wire[0]); ++i)
        for (unsigned byte = 0; byte < 4; ++byte)
            CHECK(packet.bytes[packet.length - 4 - sizeof(signature_wire) + i * 4 + byte] ==
                (uint8_t) (signature_wire[i] >> (byte * 8)));
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked); memset(packet.bytes, 0xCC, packet.length); xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(decoded, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    XrXirModule module = *xr_xir_artifact_module(closed);
    CHECK(!module.generics && module.types->count == 3);
    const XrXirTypeNode *nested = xr_xir_callable_signature(module.types, (XrXirType) 257);
    CHECK(nested && nested->parameter_count == 1 && nested->parameters[0].type == 256 && nested->result == 256);
    CHECK(module.functions[1].result == XR_XIR_CONSTRUCTED_TYPE_BASE + 1);
    XrXirBudget sendable_budget = xr_xir_default_budget();
    CHECK(xr_xir_type_satisfies(&module, 0, (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE,
        XR_XIR_CONSTRAINT_SENDABLE, &sendable_budget) == XR_XIR_BAD_TYPE);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK && lowered);
    CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(lowered);
    XrXirTypeNode signatures[3]; memcpy(signatures, module.types->nodes, sizeof(signatures));
    XrXirCallableParameter parameter = signatures[0].parameters[0]; signatures[0].parameters = &parameter;
    XrXirTypes types = {signatures, 3, NULL}; module.types = &types;
    for (unsigned attack = 0; attack < 10; ++attack) {
        XrXirTypeNode saved = signatures[2];
        if (attack == 0) signatures[0].flags = 1;
        if (attack == 1) parameter.mode = 1;
        if (attack == 2) parameter.type = XR_XIR_UNIT;
        if (attack == 3) parameter.type = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
        if (attack == 4) parameter.type = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + 1);
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
static void generic_callable_depth(void) {
    XrXirArtifact *checked = generic_callable_fixture();
    XrXirModule module = *xr_xir_artifact_module(checked);
    XrXirTypeNode signatures[258] = {0};
    for (uint32_t i = 0; i < 129; ++i) {
        signatures[i].kind = XR_XIR_TYPE_CALLABLE;
        signatures[129+i].kind = XR_XIR_TYPE_CALLABLE;
        signatures[i].result = (XrXirType) (i ? XR_XIR_CONSTRUCTED_TYPE_BASE+i-1 : XR_XIR_TYPE_PARAMETER_BASE);
        signatures[i].parameter_span = 1;
        signatures[129+i].result = i ? (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+129+i-1) : XR_XIR_STRING;
    }
    XrXirTypes table = {signatures,258, NULL}; module.types = &table;
    XrXirBudget budget = xr_xir_default_budget();
    CHECK(xr_xir_types_verify(&table,&budget) == XR_XIR_OK);
    const XrXirInstruction *call = &module.functions[0].instructions[0];
    CHECK(xr_xir_call_type_matches(&module,0,call,(XrXirType)256,(XrXirType)385,&budget) == XR_XIR_OK);
    CHECK(xr_xir_call_type_matches(&module,0,call,(XrXirType)384,(XrXirType)513,&budget) == XR_XIR_OK);
    signatures[129].result = XR_XIR_I64;
    CHECK(xr_xir_call_type_matches(&module,0,call,(XrXirType)384,(XrXirType)513,&budget) == XR_XIR_BAD_TYPE);
    signatures[129].result = XR_XIR_STRING;
    budget.metadata_bytes = 1;
    CHECK(xr_xir_call_type_matches(&module,0,call,(XrXirType)384,(XrXirType)513,&budget) == XR_XIR_BUDGET);
    budget = xr_xir_default_budget();
    budget.work = 1;
    CHECK(xr_xir_call_type_matches(&module,0,call,(XrXirType)256,(XrXirType)385,&budget) == XR_XIR_BUDGET);
    xr_xir_artifact_free(checked);
}
static void generic_callable_contracts(void) {
    XrXirArtifact *checked = generic_callable_fixture(), *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    /* Two one-parameter signatures; the first span is independently forged. */
    CHECK(packet.length > 56 && wire32(packet.bytes + packet.length - 56) == 1);
    put32(packet.bytes + packet.length - 56,0); digest_packet(&packet);
    rejected(packet.bytes,packet.length);
    put32(packet.bytes + packet.length - 56,1); digest_packet(&packet);
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    XrXirModule module = *xr_xir_artifact_module(decoded);
    XrXirFunction functions[2]; memcpy(functions,module.functions,sizeof(functions)); module.functions = functions;
    XrXirType wrong[] = {(XrXirType)256,XR_XIR_STRING};
    const XrXirType *saved = functions[0].parameters; functions[0].parameters = wrong;
    CHECK(xr_xir_verify(&module,NULL,NULL) == XR_XIR_BAD_TYPE);
    functions[0].parameters = saved;
    XrXirTypeNode signatures[2]; memcpy(signatures,module.types->nodes,sizeof(signatures));
    XrXirTypes table = {signatures,2, NULL}; module.types = &table;
    XrXirCallableParameter bad = {(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1),0};
    signatures[0].parameters = &bad; signatures[0].parameter_span = 2;
    CHECK(xr_xir_verify(&module,NULL,NULL) == XR_XIR_BAD_TYPE);
    XrXirBudget budget = xr_xir_default_budget(); budget.work = 0;
    CHECK(xr_xir_call_type_matches(xr_xir_artifact_module(decoded),0,&functions[0].instructions[0],
        (XrXirType)256,(XrXirType)257,&budget) == XR_XIR_BUDGET);
    CHECK(xr_xir_specialize(decoded,NULL,&closed,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    const XrXirModule *m = xr_xir_artifact_module(closed);
    CHECK(!m->generics && m->types->count == 1 && !m->types->nodes[0].parameter_span);
    CHECK(m->functions[0].parameters[0] == 256 && m->functions[1].parameters[0] == 256);
    CHECK(m->functions[1].instructions[0].op == XR_XIR_CALL_INDIRECT && m->functions[1].instructions[0].type == XR_XIR_STRING);
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(lowered); xr_xir_artifact_free(closed);
}
#include "xir_capture_fixture.h"
static void capture_rejections(void) {
    XrXirArtifact *lowered = capture_fixture(false); xr_xir_artifact_free(lowered);
    for (unsigned mode = 0; mode < 8; ++mode) {
        XrXirArtifact *checked = capture_checked(false);
        XrXirModule *m = (XrXirModule *) xr_xir_artifact_module(checked);
        XrXirFunction *make = (XrXirFunction *) &m->functions[2];
        XrXirInstruction *ops = (XrXirInstruction *) make->instructions;
        switch (mode) {
        case 0: ops[1].args[1] = 3; break;
        case 1: ops[1].args[0] = 2; break;
        case 2:
            ((uint32_t *) make->operands)[0] = 2; make->operand_count = 1;
            ops[2] = ops[0]; ops[3].args[0] = 1; break;
        case 3: ((uint32_t *) make->operands)[1] = 0; break;
        case 4: ((XrXirType *) m->generics[2].arguments)[0] = XR_XIR_I64; break;
        case 5: ops[1].immediate = 0; break;
        case 6:
            ops[2] = ops[1]; ops[1] = (XrXirInstruction) {XR_XIR_LOCAL_NEW,XR_XIR_STRING,{0},{0},0, {0}};
            make->operand_count = 1; ((uint32_t *) make->operands)[0] = 1; break;
        case 7: ((XrXirTypeNode *) m->types->nodes)[0].result = XR_XIR_I64; break;
        }
        XrXirStatus status = xr_xir_artifact_verify(checked,NULL,NULL);
        CHECK(status != XR_XIR_OK);
        if (mode == 3 || mode == 4 || mode == 7) CHECK(status == XR_XIR_BAD_TYPE);
        if (mode == 2) CHECK(status == XR_XIR_BAD_DOMINANCE);
        if (mode == 6) CHECK(status == XR_XIR_BAD_VALUE);
        XrXirCheckedPacket packet = {0};
        CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL) != XR_XIR_OK && !packet.bytes);
        xr_xir_artifact_free(checked);
    }
}
static void capture_packet_rejection(void) {
    XrXirArtifact *checked = capture_checked(false), *decoded = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const uint32_t fields[] = {49,256,0,1,0,0,5,0,0,1};
    uint8_t record[40];
    for (unsigned i = 0; i < 10; ++i) put32(record+4*i,fields[i]);
    size_t found = 0; unsigned matches = 0;
    for (size_t p = 64; p + sizeof(record) <= packet.length; ++p)
        if (!memcmp(packet.bytes+p,record,sizeof(record))) { found = p; ++matches; }
    CHECK(matches == 1);
    put32(packet.bytes+found+12,2); digest_packet(&packet);
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_BAD_TYPE && !decoded);
    put32(packet.bytes+found+12,1); put32(packet.bytes+12,10); digest_packet(&packet);
    rejected(packet.bytes,packet.length);
    xr_xir_checked_packet_free(&packet);
}
#include "xir_cell_checked_cases.h"
static void constructed_contracts(void) {
    XrXirArtifact *checked = constructed_fixture(), *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0}, second = {0};
    CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
    CHECK(xr_xir_checked_write(decoded,NULL,&second,NULL) == XR_XIR_OK);
    CHECK(packet.length == second.length && !memcmp(packet.bytes,second.bytes,packet.length));
    xr_xir_checked_packet_free(&second);
    /* The final CELL node has a fixed kind/span/element wire payload. */
    CHECK(wire32(packet.bytes+packet.length-16) == XR_XIR_TYPE_CELL);
    put32(packet.bytes+packet.length-16,4); digest_packet(&packet);
    rejected(packet.bytes,packet.length);
    put32(packet.bytes+packet.length-16,XR_XIR_TYPE_CELL);
    put32(packet.bytes+packet.length-8,267); digest_packet(&packet);
    rejected(packet.bytes,packet.length);
    xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(decoded,NULL,&closed,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    const XrXirModule *module = xr_xir_artifact_module(closed);
    CHECK(!module->generics && module->function_count == 3 && module->types->count == 8);
    XrXirType first = module->functions[1].parameters[0], second_type = module->functions[2].parameters[0];
    CHECK(first != second_type);
    CHECK(xr_xir_array_element(module->types,xr_xir_array_element(module->types,first)) == XR_XIR_I64);
    CHECK(xr_xir_array_element(module->types,xr_xir_array_element(module->types,second_type)) == XR_XIR_STRING);
    for (uint32_t n = 0; n < module->types->count; ++n) CHECK(!module->types->nodes[n].parameter_span);
    CHECK(module->functions[1].parameters[1] != module->functions[2].parameters[1]);
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    CHECK(xr_xir_artifact_module(lowered)->functions[1].instructions[0].op == XR_XIR_OWNED_RETAIN);
    CHECK(xr_xir_artifact_verify(lowered,NULL,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(lowered);
}
#include "xir_array_checked_cases.h"

#include "xir_nominal_checked_fixture.h"
static void nominal_packet_cases(void) {
    for (unsigned arrays = 0; arrays < 4; ++arrays) {
        XrXirArtifact *source = nominal_checked_fixture(arrays), *decoded = NULL, *specialized = NULL;
        XrXirCheckedPacket packet = {0}, again = {0};
        CHECK(xr_xir_checked_write(source, NULL, &packet, NULL) == XR_XIR_OK);
        CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
        CHECK(xr_xir_checked_write(decoded, NULL, &again, NULL) == XR_XIR_OK);
        CHECK(packet.length == again.length && !memcmp(packet.bytes, again.bytes, packet.length));
        CHECK(xr_xir_specialize(decoded, NULL, &specialized, NULL) == XR_XIR_OK);
        CHECK(xr_xir_artifact_module(specialized)->types->nominals->count == 2);
        if (arrays == 2) {
            const XrXirTypes *types = xr_xir_artifact_module(specialized)->types;
            CHECK(types->count == 3 && types->nodes[0].nominal.arguments[0] == XR_XIR_I64 &&
                types->nodes[1].nominal.arguments[0] == XR_XIR_STRING && types->nodes[2].nominal.declaration == 1);
            CHECK(types->nodes[0].nominal.arguments != xr_xir_artifact_module(decoded)->types->nodes[0].nominal.arguments);
        }
        if (arrays >= 2) {
            const XrXirTypes *types = xr_xir_artifact_module(specialized)->types;
            uint32_t first = arrays == 3 ? 1 : 0;
            CHECK(types->count == (arrays == 3 ? 6u : 3u));
            CHECK(types->nodes[first].nominal.field_count == 2);
            CHECK(types->nodes[first].nominal.fields[1] == XR_XIR_STRING);
            CHECK(types->nodes[first].nominal.fields[0] == types->nodes[first + 2].nominal.fields[0]);
            CHECK(types->nodes[first].nominal.fields[0] != types->nodes[first + 1].nominal.fields[0]);
            if (arrays == 3) {
                CHECK(xr_xir_array_element(types, types->nodes[first].nominal.fields[0]) == XR_XIR_I64);
                CHECK(xr_xir_array_element(types, types->nodes[first + 1].nominal.fields[0]) == XR_XIR_STRING);
            } else CHECK(types->nodes[first].nominal.fields[0] == XR_XIR_I64);
            XrXirCheckedPacket closed_packet = {0}; XrXirArtifact *closed_read = NULL;
            CHECK(xr_xir_checked_write(specialized, NULL, &closed_packet, NULL) == XR_XIR_OK);
            CHECK(xr_xir_checked_read(closed_packet.bytes, closed_packet.length, NULL, &closed_read, NULL) == XR_XIR_OK);
            uint8_t field_wire[32] = {4, 0, 0, 0};
            put32(field_wire + 12, 1); put32(field_wire + 16, XR_XIR_I64); put32(field_wire + 20, 2);
            put32(field_wire + 24, arrays == 3 ? XR_XIR_CONSTRUCTED_TYPE_BASE + 4 : XR_XIR_I64);
            put32(field_wire + 28, XR_XIR_STRING);
            size_t field_offset = 0; unsigned field_matches = 0;
            for (size_t i = 64; i + sizeof(field_wire) <= closed_packet.length; ++i)
                if (!memcmp(closed_packet.bytes + i, field_wire, sizeof(field_wire))) { field_offset = i; ++field_matches; }
            CHECK(field_matches == 1);
            put32(closed_packet.bytes + field_offset + 24, XR_XIR_BOOL); digest_packet(&closed_packet);
            rejected(closed_packet.bytes, closed_packet.length);
            put32(closed_packet.bytes + field_offset + 24, wire32(field_wire + 24)); digest_packet(&closed_packet);
            XrXirArtifact *repeated = NULL; XrXirCheckedPacket repeated_packet = {0};
            CHECK(xr_xir_specialize(closed_read, NULL, &repeated, NULL) == XR_XIR_OK);
            CHECK(xr_xir_checked_write(repeated, NULL, &repeated_packet, NULL) == XR_XIR_OK);
            CHECK(repeated_packet.length == closed_packet.length &&
                !memcmp(repeated_packet.bytes, closed_packet.bytes, closed_packet.length));
            xr_xir_artifact_free(repeated); xr_xir_checked_packet_free(&repeated_packet);
            xr_xir_checked_packet_free(&closed_packet);
            XrXirType *fields = (XrXirType *) types->nodes[first].nominal.fields;
            XrXirType saved_field = fields[0]; fields[0] = XR_XIR_BOOL;
            CHECK(xr_xir_artifact_verify(specialized, NULL, NULL) == XR_XIR_BAD_TYPE);
            fields[0] = saved_field;
            xr_xir_artifact_free(specialized); specialized = NULL;
            CHECK(xr_xir_artifact_verify(closed_read, NULL, NULL) == XR_XIR_OK);
            xr_xir_artifact_free(closed_read);
        }
        xr_xir_artifact_free(specialized);
        if (arrays == 2) {
            uint8_t expected[20] = {4, 0, 0, 0};
            put32(expected + 12, 1); put32(expected + 16, XR_XIR_I64);
            size_t offset = 0; unsigned matches = 0;
            for (size_t i = 64; i + sizeof(expected) <= packet.length; ++i)
                if (!memcmp(packet.bytes + i, expected, sizeof(expected))) { offset = i; ++matches; }
            CHECK(matches == 1);
            const uint32_t attacks[] = {2, UINT32_MAX};
            for (unsigned i = 0; i < 2; ++i) {
                put32(packet.bytes + offset + 8, attacks[i]); digest_packet(&packet); rejected(packet.bytes, packet.length);
            }
            put32(packet.bytes + offset + 8, 0);
            put32(packet.bytes + offset + 16, XR_XIR_TYPE_PARAMETER_BASE);
            digest_packet(&packet); rejected(packet.bytes, packet.length);
            put32(packet.bytes + offset + 16, XR_XIR_UNIT);
            digest_packet(&packet); rejected(packet.bytes, packet.length);
            put32(packet.bytes + offset + 16, XR_XIR_I64); digest_packet(&packet);
        }
        uint32_t saved = XR_XIR_CHECKED_SCHEMA;
        put32(packet.bytes + 8, 8); digest_packet(&packet); rejected(packet.bytes, packet.length);
        put32(packet.bytes + 8, saved);
        size_t module_offset = 0;
        for (size_t i = 64; i + 5 <= packet.length; ++i)
            if (!memcmp(packet.bytes + i, "alpha", 5)) module_offset = i;
        CHECK(module_offset != 0);
        packet.bytes[module_offset] = 'z'; digest_packet(&packet); rejected(packet.bytes, packet.length);
        packet.bytes[module_offset] = 'a';
        put32(packet.bytes + packet.length - 12, 8); digest_packet(&packet); rejected(packet.bytes, packet.length);
        put32(packet.bytes + packet.length - 12, XR_XIR_FIELD_PRIVATE);
        put32(packet.bytes + packet.length - 16, XR_XIR_TYPE_PARAMETER_BASE + 1);
        digest_packet(&packet); rejected(packet.bytes, packet.length);
        put32(packet.bytes + packet.length - 16, XR_XIR_STRING);
        digest_packet(&packet);
        boundaries(&packet);
        xr_xir_artifact_free(source);
        memset(packet.bytes, 0xCC, packet.length); xr_xir_checked_packet_free(&packet);
        xr_xir_checked_packet_free(&again);
        CHECK(xr_xir_artifact_verify(decoded, NULL, NULL) == XR_XIR_OK);
        const XrXirNominalDeclaration *d = xr_xir_artifact_module(decoded)->types->nominals->declarations;
        CHECK(d[0].module.bytes[d[0].module.length] == 0 && d[0].fields[0].name.bytes[5] == 0);
        CHECK(!memcmp(d[0].fields[0].name.bytes, "value", 5));
        xr_xir_artifact_free(decoded);
    }
}

static void deep_nominal_fields(void) {
    enum { DEPTH = 160 };
    XrXirArtifact *base = nominal_checked_fixture(3), *checked = NULL, *closed = NULL;
    XrXirModule built = *xr_xir_artifact_module(base); built.stage = XR_XIR_BUILT;
    XrXirTypeNode nodes[DEPTH + 3];
    for (uint32_t i = 0; i < DEPTH; ++i)
        nodes[i] = (XrXirTypeNode) {XR_XIR_TYPE_ARRAY,
            (XrXirType) (i ? XR_XIR_CONSTRUCTED_TYPE_BASE + i - 1 : XR_XIR_TYPE_PARAMETER_BASE),
            NULL, 0, XR_XIR_UNIT, 0, 1, {0}};
    for (uint32_t i = 0; i < 3; ++i) nodes[DEPTH + i] = built.types->nodes[1 + i];
    XrXirNominalDeclaration declarations[2]; XrXirNominalField fields[2][2];
    memcpy(declarations, built.types->nominals->declarations, sizeof(declarations));
    for (uint32_t i = 0; i < 2; ++i) {
        memcpy(fields[i], declarations[i].fields, sizeof(fields[i]));
        fields[i][0].type = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + DEPTH - 1);
        declarations[i].fields = fields[i];
    }
    XrXirNominalTable table = {declarations, 2, NULL};
    XrXirTypes types = {nodes, DEPTH + 3, &table}; built.types = &types;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(base); memset(nodes, 0xCC, sizeof(nodes)); memset(fields, 0xCC, sizeof(fields));
    XrXirBudget budget = xr_xir_default_budget(); budget.work = 30000;
    CHECK(xr_xir_artifact_verify(checked, &budget, NULL) == XR_XIR_OK);
    CHECK(xr_xir_specialize(checked, &budget, &closed, NULL) == XR_XIR_BUDGET && !closed);
    CHECK(budget.work == 30000);
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirTypes *result = xr_xir_artifact_module(closed)->types;
    CHECK(result->count == DEPTH * 3 + 3);
    for (uint32_t n = 0; n < 3; ++n) {
        const XrXirNominalType *instance = &result->nodes[DEPTH + n].nominal;
        CHECK(instance->field_count == 2 && instance->fields[1] == XR_XIR_STRING);
        XrXirType field = instance->fields[0];
        for (uint32_t i = 0; i < DEPTH; ++i) {
            CHECK(xr_xir_type_is_array(result, field) && !xr_xir_type_span(result, field));
            field = xr_xir_array_element(result, field);
        }
        CHECK(field == (n == 1 ? XR_XIR_STRING : XR_XIR_I64));
    }
    CHECK(result->nodes[DEPTH].nominal.fields[0] == result->nodes[DEPTH + 2].nominal.fields[0]);
    CHECK(xr_xir_artifact_verify(closed, NULL, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
}

#include "xir_nominal_chain_fixture.h"
static void nominal_storage_layout(void) {
    XrXirArtifact *checked = nominal_chain_fixture(2, 2), *closed = NULL, *lowered = NULL;
    XrXirNominalDeclaration *d = (XrXirNominalDeclaration *) xr_xir_artifact_module(checked)->types->nominals->declarations;
    ((XrXirNominalField *) d[0].fields)[0].type = XR_XIR_BOOL;
    ((XrXirNominalField *) d[1].fields)[1].type = XR_XIR_I32;
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirLayout layout = {0}; uint32_t offsets[2] = {99, 99};
    XrXirBudget budget = xr_xir_default_budget(), original = budget;
    CHECK(xr_xir_nominal_layout(xr_xir_artifact_module(checked)->types, (XrXirType)256,
        &target, &budget, &layout, offsets, 2) == XR_XIR_BAD_LAYOUT);
    CHECK(!layout.size && !layout.alignment && offsets[0] == 99 && !memcmp(&budget, &original, sizeof(budget)));
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    for (unsigned mode = 0; mode < 2; ++mode) {
        const XrXirTypes *types = xr_xir_artifact_module(mode ? lowered : closed)->types;
        budget = original;
        CHECK(xr_xir_nominal_layout(types, (XrXirType)256, &target, &budget, &layout, offsets, 2) == XR_XIR_OK);
        CHECK(layout.size == 24 && layout.alignment == 8 && offsets[0] == 0 && offsets[1] == 8);
        budget = original;
        CHECK(xr_xir_nominal_layout(types, (XrXirType)257, &target, &budget, &layout, offsets, 2) == XR_XIR_OK);
        CHECK(layout.size == 16 && layout.alignment == 8 && offsets[0] == 0 && offsets[1] == 8);
        target.abi_version--;
        CHECK(xr_xir_nominal_layout(types, (XrXirType)256, &target, &budget, &layout, offsets, 2) == XR_XIR_BAD_LAYOUT);
        target.abi_version++;
    }
    xr_xir_artifact_free(closed); xr_xir_artifact_free(lowered);
    checked = nominal_chain_fixture(160, 2); closed = NULL;
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked); budget = original; offsets[0] = offsets[1] = 99;
    CHECK(xr_xir_nominal_layout(xr_xir_artifact_module(closed)->types, (XrXirType)256,
        &target, &budget, &layout, offsets, 2) == XR_XIR_BAD_LAYOUT);
    CHECK(!layout.size && !layout.alignment && offsets[0] == 99 && offsets[1] == 99 &&
        !memcmp(&budget, &original, sizeof(budget)));
    xr_xir_artifact_free(closed);
    checked = nominal_chain_fixture(160, 1); closed = NULL;
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked); budget = original;
    CHECK(xr_xir_nominal_layout(xr_xir_artifact_module(closed)->types, (XrXirType)256,
        &target, &budget, &layout, offsets, 1) == XR_XIR_OK);
    CHECK(layout.size == 8 && layout.alignment == 8 && offsets[0] == 0);
    xr_xir_artifact_free(closed);
    NominalIdentityFixture empty; nominal_identity_fixture(&empty);
    empty.identities[0].fields = NULL; empty.identities[0].field_count = 0;
    XrXirTypeNode node = {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {0}};
    XrXirTypes types = {&node, 1, &empty.table}; budget = original;
    CHECK(xr_xir_nominal_layout(&types, (XrXirType)256, &target, &budget, &layout, NULL, 0) == XR_XIR_OK);
    CHECK(!layout.size && layout.alignment == 1);
}
static void nominal_field_graph(void) {
    XrXirArtifact *checked = nominal_chain_fixture(160, 2), *closed = NULL, *lowered = NULL;
    XrXirNominalDeclaration *declarations = (XrXirNominalDeclaration *) xr_xir_artifact_module(checked)->types->nominals->declarations;
    XrXirNominalField *last = (XrXirNominalField *) declarations[159].fields;
    last[0].type = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
    CHECK(xr_xir_artifact_verify(checked, NULL, NULL) == XR_XIR_BAD_TYPE);
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_BAD_TYPE && !closed);
    last[0].type = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + 159);
    CHECK(xr_xir_artifact_verify(checked, NULL, NULL) == XR_XIR_BAD_TYPE);
    last[0].type = XR_XIR_STRING;
    const XrXirLiteral original = declarations[159].module;
    declarations[159].module = (XrXirLiteral) {"beta", 4};
    CHECK(xr_xir_artifact_verify(checked, NULL, NULL) == XR_XIR_BAD_TYPE);
    declarations[159].exported = 1;
    /* Export alone cannot supply the declaring module's missing import edge. */
    CHECK(xr_xir_artifact_verify(checked, NULL, NULL) == XR_XIR_BAD_TYPE);
    declarations[159].module = original; declarations[159].exported = 0;
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    XrXirTypes *types = (XrXirTypes *) xr_xir_artifact_module(lowered)->types;
    CHECK(types->count == 160 && types->nodes[0].nominal.fields[0] == (XrXirType)257);
    CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) == XR_XIR_OK);
    XrXirType *end = (XrXirType *) types->nodes[159].nominal.fields;
    end[0] = (XrXirType)256;
    CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) == XR_XIR_BAD_TYPE);
    end[0] = XR_XIR_STRING;
    xr_xir_artifact_free(lowered);
}
static void nominal_projection_cases(void) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        XrXirArtifact *lowered = nominal_lowered_fixture(mode), *rejected_artifact = NULL;
        const XrXirModule *module = xr_xir_artifact_module(lowered);
        const XrXirTypes *types = module->types;
        CHECK(types && types->nominals && !types->nominals->declarations && types->nominals->identities);
        CHECK(types->count == (mode == 3 ? 5u : mode == 2 ? 3u : 0u));
        CHECK(types->nominals->identities[0].arity == 1 && types->nominals->identities[0].field_count == 2);
        if (mode >= 2) {
            CHECK(types->nodes[0].nominal.fields[0] == (mode == 3 ? (XrXirType)259 : XR_XIR_I64));
            CHECK(types->nodes[1].nominal.fields[0] == (mode == 3 ? (XrXirType)260 : XR_XIR_STRING));
        }
        for (uint32_t i = 0; i < types->count; ++i) CHECK(!types->nodes[i].parameter_span);
        XrXirModule forged = *module; forged.stage = XR_XIR_CHECKED;
        CHECK(xr_xir_recheck(&forged, NULL, &rejected_artifact, NULL) == XR_XIR_BAD_STAGE && !rejected_artifact);
        XrXirTypes *copy = NULL;
        CHECK(xr_xir_types_clone(types, &copy) == XR_XIR_OK);
        xr_xir_artifact_free(lowered);
        XrXirBudget budget = xr_xir_default_budget();
        CHECK(xr_xir_types_verify(copy, &budget) == XR_XIR_OK);
        CHECK(!memcmp(copy->nominals->identities[0].module.bytes, "alpha", 5));
        xr_xir_types_free(copy);
    }
    XrXirArtifact *checked = nominal_checked_fixture(3), *lowered = NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(checked, &target, NULL, &lowered, NULL) == XR_XIR_BAD_STAGE && !lowered);
    xr_xir_artifact_free(checked);
}

static void nominal_member_authority(void) {
    XrXirArtifact *checked = nominal_checked_fixture(3), *decoded = NULL, *closed = NULL, *lowered = NULL;
    const XrXirModule *module = xr_xir_artifact_module(checked);
    XrXirFunctionIdentity *ids = (XrXirFunctionIdentity *) module->declarations->functions;
    uint64_t work = 100000;
    CHECK(xr_xir_nominal_access(module, 7, 0, 0, XR_XIR_NOMINAL_TYPE, &work) == XR_XIR_OK);
    CHECK(xr_xir_nominal_access(module, 5, 0, 0, XR_XIR_NOMINAL_TYPE, &work) == XR_XIR_BAD_TYPE);
    CHECK(xr_xir_nominal_access(module, 4, 0, 1, XR_XIR_NOMINAL_READ, &work) == XR_XIR_OK);
    CHECK(xr_xir_nominal_access(module, 7, 0, 1, XR_XIR_NOMINAL_READ, &work) == XR_XIR_BAD_TYPE);
    CHECK(xr_xir_nominal_access(module, 3, 0, 0, XR_XIR_NOMINAL_READ, &work) == XR_XIR_OK);
    CHECK(xr_xir_nominal_access(module, 5, 0, 0, XR_XIR_NOMINAL_READ, &work) == XR_XIR_BAD_TYPE);
    CHECK(xr_xir_nominal_access(module, 4, 0, 0, XR_XIR_NOMINAL_CONSTRUCT, &work) == XR_XIR_OK);
    CHECK(xr_xir_nominal_access(module, 7, 0, 0, XR_XIR_NOMINAL_CONSTRUCT, &work) == XR_XIR_BAD_TYPE);
    CHECK(xr_xir_nominal_access(module, 4, 0, 1, XR_XIR_NOMINAL_WRITE, &work) == XR_XIR_BAD_TYPE);
    CHECK(xr_xir_nominal_access(module, 4, 0, 0, XR_XIR_NOMINAL_WRITE, &work) == XR_XIR_OK);
    CHECK(xr_xir_nominal_access(module, 4, 0, 2, XR_XIR_NOMINAL_READ, &work) == XR_XIR_BAD_STRUCTURE);
    XrXirNominalDeclaration *d = (XrXirNominalDeclaration *) module->types->nominals->declarations;
    d[0].exported = 0;
    CHECK(xr_xir_nominal_access(module, 3, 0, 0, XR_XIR_NOMINAL_READ, &work) == XR_XIR_BAD_TYPE);
    d[0].exported = 1;
    XrXirNominalField *fields = (XrXirNominalField *) d[0].fields;
    fields[1].flags = XR_XIR_FIELD_PROTECTED;
    CHECK(xr_xir_nominal_access(module, 4, 0, 1, XR_XIR_NOMINAL_READ, &work) == XR_XIR_OK);
    CHECK(xr_xir_nominal_access(module, 7, 0, 1, XR_XIR_NOMINAL_READ, &work) == XR_XIR_BAD_TYPE);
    fields[1].flags = XR_XIR_FIELD_PRIVATE;
    const uint32_t invalid_functions[] = {7, 5, 2, 3};
    for (unsigned i = 0; i < 4; ++i) {
        ids[invalid_functions[i]].nominal_owner = i ? 1 : 3;
        CHECK(xr_xir_artifact_verify(checked, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
        ids[invalid_functions[i]].nominal_owner = 0;
    }
    work = 0;
    CHECK(xr_xir_nominal_access(module, 4, 0, 0, XR_XIR_NOMINAL_READ, &work) == XR_XIR_BUDGET);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(decoded, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed); module = xr_xir_artifact_module(lowered); work = 10000;
    CHECK(module->declarations->functions[4].nominal_owner == 1);
    CHECK(xr_xir_nominal_access(module, 4, 0, 1, XR_XIR_NOMINAL_READ, &work) == XR_XIR_OK);
    CHECK(xr_xir_nominal_access(module, 7, 0, 1, XR_XIR_NOMINAL_READ, &work) == XR_XIR_BAD_TYPE);
    xr_xir_artifact_free(lowered);
}
static void uninitialized_packet(void) {
    const XrXirType parameter = XR_XIR_I64;
    const XrXirInstruction ops[] = {
        {XR_XIR_LOCAL_UNINIT, XR_XIR_I64, {0}, {0}, 0, {0}},
        {XR_XIR_LOCAL_WRITE, XR_XIR_UNIT, {1, 0}, {0}, 0, {0}},
        {XR_XIR_LOCAL_READ, XR_XIR_I64, {1}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {3}, {0}, 0, {0}}
    };
    const XrXirBlock block = {0, 4, 0, 0};
    const XrXirFunction function = {"u", 1, &parameter, 1, XR_XIR_I64, &block, 1, ops, 4, NULL, 0};
    const XrXirModule built = {XR_XIR_BUILT, &function, 1, NULL, NULL, NULL, NULL};
    XrXirArtifact *checked = NULL, *decoded = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    CHECK(packet.length == 293 && packet.bytes[113] == XR_XIR_LOCAL_UNINIT && packet.bytes[153] == XR_XIR_LOCAL_WRITE);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    uint8_t original[293]; memcpy(original, packet.bytes, sizeof(original));
    /* A second write is valid for mutable storage, but not initialize-once. */
    put32(packet.bytes + 193, XR_XIR_LOCAL_WRITE); put32(packet.bytes + 197, XR_XIR_UNIT);
    put32(packet.bytes + 205, 0); put32(packet.bytes + 241, 0); digest_packet(&packet);
    decoded = NULL;
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded); decoded = NULL;
    put32(packet.bytes + 137, 1); digest_packet(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_VALUE && !decoded);
    memcpy(packet.bytes, original, sizeof(original));
    /* Repairing the digest must not hide removal of the initializing write. */
    put32(packet.bytes + 153, XR_XIR_SUSPEND); put32(packet.bytes + 161, 0);
    digest_packet(&packet);
    decoded = NULL;
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_VALUE);
    CHECK(!decoded);
    xr_xir_checked_packet_free(&packet);
}
static void nominal_callable_components(void) {
    XrXirArtifact *base = nominal_chain_fixture(1, 1), *checked = NULL, *decoded = NULL;
    XrXirModule built = *xr_xir_artifact_module(base); built.stage = XR_XIR_BUILT;
    XrXirCallableParameter parameter = {(XrXirType)256, 0};
    XrXirTypeNode nodes[2] = {built.types->nodes[0],
        {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT, &parameter, 1, (XrXirType)256, 0, 0, {0}}};
    XrXirTypes types = {nodes, 2, built.types->nominals}; built.types = &types;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded); xr_xir_artifact_free(checked); checked = NULL;
    uint8_t pattern[28]; const uint32_t words[] = {1, 0, 1, 256, 0, 256, 0};
    for (unsigned i = 0; i < 7; ++i) put32(pattern + i * 4, words[i]);
    size_t offset = 0; unsigned matches = 0;
    for (size_t i = 64; i + sizeof(pattern) <= packet.length; ++i)
        if (!memcmp(packet.bytes + i, pattern, sizeof(pattern))) { offset = i; ++matches; }
    CHECK(matches == 1);
    put32(packet.bytes + offset + 20, 257); digest_packet(&packet); rejected(packet.bytes, packet.length);
    xr_xir_checked_packet_free(&packet);
    nodes[1].result = (XrXirType)257;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
    nodes[1].result = (XrXirType)256; nodes[1].parameter_span = 1;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
    nodes[1].parameter_span = 0; parameter.type = XR_XIR_UNIT;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
    parameter.type = (XrXirType)257;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
    xr_xir_artifact_free(base);
}
static void string_query_contracts(void) {
    XrXirType parameters[] = {XR_XIR_STRING, XR_XIR_STRING};
    XrXirInstruction ops[] = {
        {XR_XIR_STRING_LEN, XR_XIR_I64, {0}, {0}, 0, {0}},
        {XR_XIR_EQ_STRING, XR_XIR_BOOL, {0,1}, {0}, 0, {0}},
        {XR_XIR_NE_STRING, XR_XIR_BOOL, {0,1}, {0}, 0, {0}},
        {XR_XIR_STRING_CONTAINS, XR_XIR_BOOL, {0,1}, {0}, 0, {0}},
        {XR_XIR_STRING_STARTS_WITH, XR_XIR_BOOL, {0,1}, {0}, 0, {0}},
        {XR_XIR_STRING_ENDS_WITH, XR_XIR_BOOL, {0,1}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {7,0}, {0}, 0, {0}}};
    XrXirBlock block = {0,7, 0, 0};
    XrXirFunction function = {"queries",7,parameters,2,XR_XIR_BOOL,&block,1,ops,7,NULL,0};
    XrXirModule built = {XR_XIR_BUILT,&function,1,NULL,NULL,NULL,NULL};
    XrXirArtifact *checked = NULL, *decoded = NULL;
    CHECK(xr_xir_check(&built,NULL,&checked,NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL) == XR_XIR_OK);
    for (unsigned i = 0; i < 6; ++i) {
        uint8_t pattern[40] = {0};
        put32(pattern, (uint32_t)ops[i].op); put32(pattern+4,(uint32_t)ops[i].type);
        put32(pattern+8,ops[i].args[0]); put32(pattern+12,ops[i].args[1]);
        size_t found = 0; unsigned matches = 0;
        for (size_t at = 64; at + sizeof(pattern) <= packet.length; ++at)
            if (!memcmp(packet.bytes+at,pattern,sizeof(pattern))) { found=at; ++matches; }
        CHECK(matches == 1);
        const unsigned offsets[] = {4,8,16,24};
        const uint32_t corrupt[] = {XR_XIR_STRING,2,1,1};
        for (unsigned n = 0; n < 4; ++n) {
            put32(packet.bytes+found+offsets[n],corrupt[n]); digest_packet(&packet);
            rejected(packet.bytes,packet.length);
            memcpy(packet.bytes+found,pattern,sizeof(pattern)); digest_packet(&packet);
        }
    }
    xr_xir_artifact_free(checked); checked=NULL;
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
    memset(packet.bytes,0xCC,packet.length); xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_artifact_verify(decoded,NULL,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    for (unsigned i = 0; i < 2; ++i) {
        parameters[i] = XR_XIR_I64;
        CHECK(xr_xir_check(&built,NULL,&checked,NULL) == XR_XIR_BAD_TYPE && !checked);
        parameters[i] = XR_XIR_STRING;
    }
}
static void string_search_packets(void) {
    XrXirType parameters[] = {XR_XIR_STRING, XR_XIR_STRING, XR_XIR_I64};
    uint32_t operands[] = {0,1,2};
    XrXirInstruction ops[] = {
        {XR_XIR_STRING_INDEX_OF, XR_XIR_I64, {0,3}, {0}, 0, {0}},
        {XR_XIR_STRING_LAST_INDEX_OF, XR_XIR_I64, {0,1}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {3,0}, {0}, 0, {0}}};
    XrXirBlock block = {0,3, 0, 0};
    XrXirFunction function = {"search",6,parameters,3,XR_XIR_I64,&block,1,ops,3,operands,3};
    XrXirModule built = {XR_XIR_BUILT,&function,1,NULL,NULL,NULL,NULL};
    XrXirArtifact *checked = NULL, *decoded = NULL;
    CHECK(xr_xir_check(&built,NULL,&checked,NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL) == XR_XIR_OK);
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t pattern[40] = {0};
        put32(pattern,ops[i].op); put32(pattern+4,XR_XIR_I64);
        put32(pattern+8,ops[i].args[0]); put32(pattern+12,ops[i].args[1]);
        size_t found = 0; unsigned matches = 0;
        for (size_t at = 64; at + 40 <= packet.length; ++at)
            if (!memcmp(packet.bytes+at,pattern,40)) { found=at; ++matches; }
        CHECK(matches == 1);
        const unsigned offsets[] = {4,8,12,16,24};
        const uint32_t values[] = {XR_XIR_BOOL,UINT32_MAX,i ? 2u : 2u,1,1};
        for (unsigned j = 0; j < 5; ++j) {
            put32(packet.bytes+found+offsets[j],values[j]); digest_packet(&packet);
            rejected(packet.bytes,packet.length);
            memcpy(packet.bytes+found,pattern,40); digest_packet(&packet);
        }
    }
    xr_xir_artifact_free(checked); checked=NULL;
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_artifact_verify(decoded,NULL,NULL) == XR_XIR_OK); xr_xir_artifact_free(decoded);
    for (unsigned i = 0; i < 3; ++i) {
        XrXirType saved = parameters[i]; parameters[i] = i==2 ? XR_XIR_STRING : XR_XIR_I64;
        CHECK(xr_xir_check(&built,NULL,&checked,NULL) == XR_XIR_BAD_TYPE && !checked);
        parameters[i] = saved;
    }
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t saved = operands[i]; operands[i] = i==2 ? 4 : 3;
        CHECK(xr_xir_check(&built,NULL,&checked,NULL) != XR_XIR_OK && !checked); operands[i]=saved;
    }
}
#include "xir_enum_checked_fixture.h"
static void enum_packet_cases(void) {
    XrXirArtifact *source = enum_checked_fixture(), *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(source, NULL, &packet, NULL) == XR_XIR_OK);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(source);
    CHECK(xr_xir_specialize(decoded, NULL, &closed, NULL) == XR_XIR_OK);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded); xr_xir_artifact_free(closed);
    const XrXirNominalIdentity *identity = xr_xir_artifact_module(lowered)->types->nominals->identities;
    CHECK(identity->kind == XR_XIR_NOMINAL_ENUM && identity->variant_count == 3);
    CHECK(identity->variants[2].field_begin == 1 && !memcmp(identity->variants[2].name.bytes, "Right", 5));
    size_t at = 0;
    for (size_t i = 64; i + 13 <= packet.length; ++i)
        if (!memcmp(packet.bytes + i, "Right", 5)) { at = i + 5; break; }
    CHECK(at); put32(packet.bytes + at, UINT32_MAX); digest_packet(&packet);
    rejected(packet.bytes, packet.length);
    xr_xir_checked_packet_free(&packet); xr_xir_artifact_free(lowered);
}

#include "xir_enum_layout_cases.h"
#include "xir_enum_generic_fixture.h"
#include "xir_enum_packet_cases.h"
static void match_fault_shape(void) {
    XrXirInstruction op={XR_XIR_MATCH_FAIL,XR_XIR_UNIT,{0},{0},0, {0}};
    const XrXirBlock block={0,1, 0, 0};
    const XrXirFunction fn={"fault",5,NULL,0,XR_XIR_UNIT,&block,1,&op,1,NULL,0};
    const XrXirModule module={XR_XIR_BUILT,&fn,1,NULL,NULL,NULL,NULL};
    for (unsigned variant=0;variant<7;++variant) {
        op=(XrXirInstruction){XR_XIR_MATCH_FAIL,XR_XIR_UNIT,{0},{0},0, {0}};
        switch (variant) {
        case 1: op.args[0]=1; break;
        case 2: op.args[1]=1; break;
        case 3: op.targets[0]=1; break;
        case 4: op.targets[1]=1; break;
        case 5: op.immediate=442; break;
        case 6: op.type=XR_XIR_I64; break;
        default: break;
        }
        XrXirArtifact *checked=NULL;
        XrXirStatus status=xr_xir_check(&module,NULL,&checked,NULL);
        if (variant) { CHECK(status!=XR_XIR_OK && !checked); continue; }
        CHECK(status==XR_XIR_OK && checked);
        XrXirCheckedPacket packet={0}; XrXirArtifact *decoded=NULL;
        CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL)==XR_XIR_OK);
        xr_xir_artifact_free(checked);
        CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL)==XR_XIR_OK);
        CHECK(xr_xir_artifact_module(decoded)->functions[0].instructions[0].op==XR_XIR_MATCH_FAIL);
        xr_xir_checked_packet_free(&packet); xr_xir_artifact_free(decoded);
    }
}
#include "xir_invoke_checked_cases.h"
#include "xir_cleanup_role_cases.h"
#include "xir_cleanup_frontier_cases.h"
int main(void) {
    cleanup_role_cases();
    cleanup_frontier_cases();
    cleanup_error_frontier();
    invoke_checked_cases();
    match_fault_shape();
    enum_generic_cases(); enum_instruction_packet_cases();
    enum_storage_layout(); enum_tag_widths();
    enum_packet_cases();
    string_search_packets();
    string_query_contracts();
    nominal_callable_components();
    uninitialized_packet();
    for (unsigned invalid = 1; invalid <= 11; ++invalid) CHECK(!struct_set_checked(invalid));
    for (unsigned invalid = 1; invalid <= 11; ++invalid) CHECK(!struct_ops_checked(invalid));
    nominal_member_authority();
    nominal_storage_layout();
    nominal_field_graph();
    nominal_projection_cases();
    deep_nominal_fields();
    nominal_packet_cases();
    array_checked_cases();
    constructed_contracts();
    cell_checked_cases();
    capture_rejections(); capture_packet_rejection();
    generic_callable_contracts(); generic_callable_depth();
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
