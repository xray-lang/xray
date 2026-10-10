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
#include "xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_internal.h"
#include "base/xsha256.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_consumer_context_owner.h"
#include "xir_checked_fixture.h"
#include "xir_checked_scalar57_golden.h"
#include "xir_checked_scalar58_golden.h"
#include "xir_checked_scalar59_golden.h"
#include "xir_checked_scalar60_golden.h"
#include "xir_checked_scalar64_golden.h"
#include "xir_checked_scalar65_golden.h"
#include "xir_checked_scalar70_golden.h"
#include "xir_checked_scalar71_golden.h"
#include "xir_checked_scalar72_golden.h"
#include "xir_checked_scalar73_golden.h"
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
    XrXirCompileContext probe=consumer_context_ephemeral((XrCompileResourceLimits){67108864,8388608,128000000});
    XrCompileResourceStats baseline=consumer_context_stats(&probe);
    XrXirArtifact *artifact = NULL;
    XrXirDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_checked_read(&probe, bytes, length, &artifact, &diagnostic) != XR_XIR_OK);
    CHECK(!artifact && diagnostic.status != XR_XIR_OK);
    /* Occupied output is a separate preservation check, not the malformed oracle. */
    artifact=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(&probe, bytes, length, &artifact, NULL)!=XR_XIR_OK);
    CHECK(artifact==(XrXirArtifact *)(uintptr_t)1);
    consumer_context_ephemeral_free(&probe,baseline);
}
static void checked_previous_identity_rejected(const void *bytes, size_t length) {
    XrXirCompileContext probe=consumer_context_ephemeral((XrCompileResourceLimits){67108864,8388608,128000000});
    const XrCompileResourceStats baseline=consumer_context_stats(&probe);
    XrXirArtifact *artifact=NULL;
    XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_checked_read(&probe,bytes,length,&artifact,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    CHECK(!artifact && diagnostic.status==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats after=consumer_context_stats(&probe);
    CHECK(after.allocation_count==baseline.allocation_count && after.allocated_bytes==baseline.allocated_bytes);
    CHECK(after.live_bytes==baseline.live_bytes);
    artifact=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(&probe,bytes,length,&artifact,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(artifact==(XrXirArtifact *)(uintptr_t)1);
    after=consumer_context_stats(&probe);
    CHECK(after.allocation_count==baseline.allocation_count && after.allocated_bytes==baseline.allocated_bytes);
    CHECK(after.live_bytes==baseline.live_bytes);
    consumer_context_ephemeral_free(&probe,baseline);
}
static void checked_current_callable_rejected(const void *bytes, size_t length) {
    XrXirCompileContext probe=consumer_context_ephemeral((XrCompileResourceLimits){67108864,8388608,128000000});
    const XrCompileResourceStats baseline=consumer_context_stats(&probe);
    XrXirArtifact *artifact=NULL;
    XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_checked_read(&probe,bytes,length,&artifact,&diagnostic)==XR_XIR_BAD_TYPE);
    CHECK(!artifact && diagnostic.status==XR_XIR_BAD_TYPE);
    CHECK(consumer_context_stats(&probe).live_bytes==baseline.live_bytes);
    artifact=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(&probe,bytes,length,&artifact,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(artifact==(XrXirArtifact *)(uintptr_t)1);
    CHECK(consumer_context_stats(&probe).live_bytes==baseline.live_bytes);
    consumer_context_ephemeral_free(&probe,baseline);
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
    const size_t fields[] = {8, 12, 16, 20, 64, 68, 72, 76};
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        uint8_t saved[4]; memcpy(saved, packet->bytes + fields[i], 4);
        put32(packet->bytes + fields[i], UINT32_MAX); digest_packet(packet);
        rejected(packet->bytes, packet->length);
        memcpy(packet->bytes + fields[i], saved, 4); digest_packet(packet);
    }
    XrXirCompileContext budget = consumer_context_default(); XrXirArtifact *decoded = NULL;
    for (unsigned i = 0; i < 9; ++i) {
        budget = consumer_context_default();
        if (i == 0) budget.limits.functions = 0;
        if (i == 1) budget.limits.blocks = 0;
        if (i == 2) budget.limits.instructions = 0;
        if (i == 3) budget = consumer_context_limits((XrCompileResourceLimits){80,8388608,128000000});
        if (i == 4) budget = consumer_context_limits((XrCompileResourceLimits){67108864,8388608,1});
        if (i == 5) budget = consumer_context_limits((XrCompileResourceLimits){67108864,80,128000000});
        if (i == 6) budget = consumer_context_limits((XrCompileResourceLimits){packet->length+80,8388608,128000000});
        if (i == 7) budget.limits.functions = 7;
        if (i == 8) budget.limits.parameters = 1;
        XrXirStatus status = xr_xir_compile_checked_read(&budget, packet->bytes, packet->length, &decoded, NULL);
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
    size_t at = 76;
    CHECK(packet32(packet, 64) == 0);
    uint32_t functions = packet32(packet, 68);
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
    size_t identities = at, slot_data = at + (size_t) functions * 36;
    size_t literal_data = slot_data + (size_t) slots * 12;
    const size_t offsets[] = {declarations, declarations + 12, declarations + 16,
        dependencies + 4, initializer, identities + 4 * 36 + 4, slot_data, literal_data + 4, identities + 4 * 36 + 8};
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
    const size_t offsets[] = {93, 101, 105, 117, 121, 125, 129, 137, 161, 169, 249};
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
        XR_XIR_SLOT_LOAD, XR_XIR_SLOT_INIT, XR_XIR_SLOT_STORE, XR_XIR_ATOMIC_NEW,
        XR_XIR_ATOMIC_LOAD, XR_XIR_ATOMIC_STORE, XR_XIR_ATOMIC_ADD, XR_XIR_ATOMIC_SUB,
        XR_XIR_ATOMIC_FETCH_ADD, XR_XIR_ATOMIC_FETCH_SUB, XR_XIR_ATOMIC_SWAP,
        XR_XIR_ATOMIC_COMPARE_EXCHANGE, XR_XIR_ATOMIC_TOGGLE, XR_XIR_ATOMIC_TO_STRING, XR_XIR_COPY, XR_XIR_SCALAR_COPY,
        XR_XIR_OWNED_RETAIN, XR_XIR_CONCAT_STRING, XR_XIR_OUTPUT, XR_XIR_WRITE_STREAM,
        XR_XIR_PRINT, XR_XIR_ADD_INT, XR_XIR_EQ_INT, XR_XIR_LT_INT, XR_XIR_CALL,
        XR_XIR_SUSPEND, XR_XIR_THROW, XR_XIR_JUMP, XR_XIR_BRANCH, XR_XIR_RETURN};
    _Static_assert(XR_XIR_CHECKED_SCHEMA == 28 && XR_XIR_CHECKED_CONTRACT == 73 && XR_XIR_OP_COUNT == 153, "packet revision");
    _Static_assert(XR_XIR_NULLABLE_NONE == 126 && XR_XIR_NULLABLE_SOME == 127, "typed nullable operations");
    _Static_assert(XR_XIR_EQUAL == 125, "typed value equality operation");
    _Static_assert(XR_XIR_INVOKE_DISCARD == 124, "typed normal result discard operation");
    _Static_assert(XR_XIR_ASSERT_CONDITION == 123, "typed assertion wire operation");
    _Static_assert(XR_XIR_CALL_DEFAULT == 121, "default purpose wire operation");
    _Static_assert(XR_XIR_INVOKE_DEFAULT == 122, "default error continuation wire operation");
    _Static_assert(XR_XIR_CLASS_NEW == 118 && XR_XIR_CLASS_GET == 119 && XR_XIR_CLASS_SET == 120, "class wire operations");
    _Static_assert(XR_XIR_PANIC_CATCH == 105 && XR_XIR_PANIC_CODE == 106 && XR_XIR_PANIC_MESSAGE == 107 &&
        XR_XIR_PANIC_INFO == 15, "panic wire identities");
    _Static_assert(XR_XIR_MATCH_FAIL == 97, "match fault wire operation");
    _Static_assert(XR_XIR_ENUM_NEW == 94 && XR_XIR_ENUM_TAG == 95 && XR_XIR_ENUM_GET == 96, "enum wire operations");
    _Static_assert(XR_XIR_STRING_INDEX_OF == 92 && XR_XIR_STRING_LAST_INDEX_OF == 93, "search wire operations");
    _Static_assert(XR_XIR_STRING_CONTAINS == 89 && XR_XIR_STRING_STARTS_WITH == 90 && XR_XIR_STRING_ENDS_WITH == 91, "string predicate wire operations");
    _Static_assert(XR_XIR_STRING_LEN == 86 && XR_XIR_EQ_STRING == 87 && XR_XIR_NE_STRING == 88, "string query wire operations");
    _Static_assert(XR_XIR_STRUCT_NEW == 82 && XR_XIR_STRUCT_GET == 83 && XR_XIR_STRUCT_SET == 84, "struct wire operations");
    _Static_assert(XR_XIR_F32 == 12 && XR_XIR_F64 == 13 && XR_XIR_CONVERT_NUMBER == 62 &&
        XR_XIR_CONST_FLOAT == 63 && XR_XIR_NEG_FLOAT == 64 && XR_XIR_EQ_FLOAT == 65 && XR_XIR_GE_FLOAT == 70, "numeric wire identities");
    _Static_assert(XR_XIR_FUNCTION_REF == 57 && XR_XIR_CALL_INDIRECT == 58, "callable wire operations");
    _Static_assert(XR_XIR_CELL_PLACE == 71 && XR_XIR_SLOT_PLACE == 72 && XR_XIR_ARRAY_NEW == 73 &&
        XR_XIR_ARRAY_GET == 74 && XR_XIR_ARRAY_SET == 75 && XR_XIR_ARRAY_PUSH == 76 &&
        XR_XIR_ARRAY_LEN == 77, "array wire operations");
    _Static_assert(XR_XIR_UNIT == 0 && XR_XIR_BOOL == 1 && XR_XIR_I64 == 2 && XR_XIR_STRING == 3 && XR_XIR_I8 == 5, "wire type identities");
    for (unsigned i = 0; i < sizeof(identities)/sizeof(*identities); ++i) CHECK((unsigned) identities[i] == i + 1);
    XrXirInstruction ops[] = {{XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, INT64_MIN, {0}},
                             {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirBlock block = {0, 2, 0, 0};
    XrXirFunction function = {"n", 1, NULL, 0, XR_XIR_I64, &block, 1, ops, 2, NULL, 0};
    XrXirModule built = {XR_XIR_BUILT, &function, 1, NULL, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL, *decoded = NULL;
    CHECK(xir_fixture_check(suite_context, &built, &checked, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    CHECK(!xr_xir_compile_artifact_module(checked)->provenance);
    CHECK(packet.length == 225 && packet.bytes[24] == 161);
    CHECK(packet.bytes[101] == 0 && packet.bytes[105] == 0 && packet.bytes[109] == 2 && packet.bytes[113] == XR_XIR_CONST_INT);
    for (unsigned i = 0; i < 7; ++i) CHECK(packet.bytes[137 + i] == 0);
    CHECK(packet.bytes[144] == 128);
    /* Independent fixed little-endian fixture, including the signed minimum
     * and the block's zero panic handler and cleanup frontier. */
    CHECK(!memcmp(packet.bytes + 32, checked_scalar73_digest, 32));
    CHECK(packet.length==sizeof(checked_scalar73_golden) && !memcmp(packet.bytes,checked_scalar73_golden,packet.length));
    checked_previous_identity_rejected(checked_scalar72_golden,sizeof(checked_scalar72_golden));
    checked_previous_identity_rejected(checked_scalar71_golden,sizeof(checked_scalar71_golden));
    checked_previous_identity_rejected(checked_scalar70_golden,sizeof(checked_scalar70_golden));
    checked_previous_identity_rejected(checked_scalar65_golden,sizeof(checked_scalar65_golden));
    checked_previous_identity_rejected(checked_scalar_previous69_constructed,sizeof(checked_scalar_previous69_constructed));
    checked_previous_identity_rejected(checked_scalar64_golden,sizeof(checked_scalar64_golden));
    checked_previous_identity_rejected(checked_scalar57_golden,sizeof(checked_scalar57_golden));
    checked_previous_identity_rejected(checked_scalar58_golden,sizeof(checked_scalar58_golden));
    checked_previous_identity_rejected(checked_scalar59_golden,sizeof(checked_scalar59_golden));
    checked_previous_identity_rejected(checked_scalar60_golden,sizeof(checked_scalar60_golden));
    uint8_t original[225]; memcpy(original, packet.bytes, sizeof(original));
    for (unsigned offset = 145; offset <= 149; offset += 4) {
        put32(packet.bytes + offset, 1); digest_packet(&packet);
        rejected(packet.bytes, packet.length);
        memcpy(packet.bytes, original, sizeof(original));
    }
    put32(packet.bytes+153,XR_XIR_THROW); digest_packet(&packet);
    rejected(packet.bytes,packet.length);
    memcpy(packet.bytes,original,sizeof(original));
    /* An entry block cannot be protected, and a handler must name a block. */
    for (uint32_t handler = 0; handler < 2; ++handler) {
        put32(packet.bytes + 101, handler + 1); digest_packet(&packet);
        rejected(packet.bytes, packet.length);
        memcpy(packet.bytes, original, sizeof(original));
    }
    put32(packet.bytes + 85, XR_XIR_U8); put32(packet.bytes + 117, XR_XIR_U8);
    memset(packet.bytes + 137, 0, 8); put32(packet.bytes + 137, 256); digest_packet(&packet);
    rejected(packet.bytes, packet.length);
    put32(packet.bytes + 137, 255); digest_packet(&packet);
    XrXirArtifact *narrow = NULL;
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &narrow, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(narrow);narrow=NULL;
    put32(packet.bytes + 12, 30); digest_packet(&packet); rejected(packet.bytes, packet.length);
    memcpy(packet.bytes, original, sizeof(original));
    put32(packet.bytes + 8, 9); digest_packet(&packet); rejected(packet.bytes, packet.length);
    memcpy(packet.bytes, original, sizeof(original));
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_module(decoded)->functions[0].instructions[0].immediate == INT64_MIN);
    xr_xir_compile_artifact_free(decoded);decoded=NULL; xr_xir_compile_artifact_free(checked);checked=NULL; xr_xir_compile_checked_packet_free(&packet);
}
static void callable_contracts(void) {
    XrXirArtifact *checked = callable_fixture(suite_context), *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    const uint32_t signature_wire[] = {3, 0, 0,
        1, 0, 1, 2, 0, 3, 8,
        1, 0, 1, 256, 0, 256, 8,
        1, 0, 0, 0, 8};
    CHECK(packet.length >= sizeof(signature_wire) + 12);
    for (size_t i = 0; i < sizeof(signature_wire) / sizeof(signature_wire[0]); ++i)
        for (unsigned byte = 0; byte < 4; ++byte)
            CHECK(packet.bytes[packet.length - 12 - sizeof(signature_wire) + i * 4 + byte] ==
                (uint8_t) (signature_wire[i] >> (byte * 8)));
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL; memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
    XrXirModule module = *xr_xir_compile_artifact_module(closed);
    CHECK(!module.generics && module.types->count == 3);
    const XrXirTypeNode *nested = xr_xir_callable_signature(module.types, (XrXirType) 257);
    CHECK(nested && nested->parameter_count == 1 && nested->parameters[0].type == 256 && nested->result == 256 && nested->flags == 8u);
    CHECK(module.functions[1].result == XR_XIR_CONSTRUCTED_TYPE_BASE + 1);
    XrXirCompileContext sendable_budget = consumer_context_default();
    CHECK(xr_xir_compile_type_satisfies(&sendable_budget, &module, 0, (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE, (XrXirConstraint){.markers = XR_XIR_CONSTRAINT_SENDABLE}) == XR_XIR_BAD_TYPE);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK && lowered);
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered);lowered=NULL;
    XrXirTypeNode signatures[3]; memcpy(signatures, module.types->nodes, sizeof(signatures));
    XrXirCallableParameter parameter = signatures[0].parameters[0]; signatures[0].parameters = &parameter;
    XrXirTypes types = {signatures, 3, NULL, NULL}; module.types = &types;
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
        CHECK(xir_fixture_verify(suite_context, &module, NULL) != XR_XIR_OK);
        parameter = (XrXirCallableParameter) {XR_XIR_I64, 0};
        signatures[0].flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED; signatures[2] = saved; types.count = 3;
    }
    CHECK(xir_fixture_verify(suite_context, &module, NULL) == XR_XIR_OK);
    XrXirCompileContext budget = consumer_context_limits((XrCompileResourceLimits){67108864,8388608,4});
    CHECK(xir_fixture_verify(&budget, &module, NULL) == XR_XIR_BUDGET);
    budget = consumer_context_default(); budget.limits.parameters = 1;
    CHECK(xir_fixture_verify(&budget, &module, NULL) == XR_XIR_BUDGET);
    xr_xir_compile_artifact_free(closed);closed=NULL;
}
static void function_ir_rejections(void) {
    XrXirArtifact *checked = function_ir_fixture(suite_context);
    XrXirModule module = *xr_xir_compile_artifact_module(checked);
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
        CHECK(xir_fixture_verify(suite_context, &module, NULL) != XR_XIR_OK);
        memcpy(ops, saved, sizeof(ops)); argument = 1;
    }
    CHECK(xir_fixture_verify(suite_context, &module, NULL) == XR_XIR_OK);
    XrXirArtifact *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    const XrXirModule *specialized = xr_xir_compile_artifact_module(closed);
    CHECK(!specialized->generics && specialized->function_count == 3);
    CHECK(specialized->functions[1].instructions[0].op == XR_XIR_FUNCTION_REF);
    CHECK(specialized->functions[2].parameters[0] == XR_XIR_I64);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered);lowered=NULL; xr_xir_compile_artifact_free(closed);closed=NULL;
    xr_xir_compile_artifact_free(checked);checked=NULL;
}
static void generic_callable_depth(void) {
    XrXirArtifact *checked = generic_callable_fixture(suite_context);
    XrXirModule module = *xr_xir_compile_artifact_module(checked);
    XrXirTypeNode signatures[258] = {0};
    for (uint32_t i = 0; i < 129; ++i) {
        signatures[i].kind = XR_XIR_TYPE_CALLABLE;
        signatures[129+i].kind = XR_XIR_TYPE_CALLABLE;
        signatures[i].flags = signatures[129+i].flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED;
        signatures[i].result = (XrXirType) (i ? XR_XIR_CONSTRUCTED_TYPE_BASE+i-1 : XR_XIR_TYPE_PARAMETER_BASE);
        signatures[i].parameter_span = 1;
        signatures[129+i].result = i ? (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+129+i-1) : XR_XIR_STRING;
    }
    XrXirTypes table = {signatures,258, NULL, NULL}; module.types = &table;
    XrXirCompileContext budget = consumer_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&budget, &table) == XR_XIR_OK);
    const XrXirInstruction *call = &module.functions[0].instructions[0];
    CHECK(xr_xir_compile_call_type_matches(&budget, &module, 0, call, (XrXirType)256, (XrXirType)385) == XR_XIR_OK);
    CHECK(xr_xir_compile_call_type_matches(&budget, &module, 0, call, (XrXirType)384, (XrXirType)513) == XR_XIR_OK);
    signatures[129].result = XR_XIR_I64;
    CHECK(xr_xir_compile_call_type_matches(&budget, &module, 0, call, (XrXirType)384, (XrXirType)513) == XR_XIR_BAD_TYPE);
    signatures[129].result = XR_XIR_STRING;
    XrXirCompileContext measure=consumer_context_ephemeral((XrCompileResourceLimits){67108864,8388608,128000000});
    XrCompileResourceStats baseline=consumer_context_stats(&measure);
    CHECK(xr_xir_compile_call_type_matches(&measure,&module,0,call,(XrXirType)384,(XrXirType)513)==XR_XIR_OK);
    XrCompileResourceStats required=consumer_context_stats(&measure);
    CHECK(required.live_bytes==baseline.live_bytes);
    consumer_context_ephemeral_free(&measure,baseline);
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits={67108864,8388608,128000000};
        if (!axis) limits.allocated_bytes=required.allocated_bytes-minus;
        if (axis==1) limits.live_bytes=required.peak_bytes-minus;
        if (axis==2) limits.work=required.work-minus;
        XrXirCompileContext tight=consumer_context_ephemeral(limits);
        XrCompileResourceStats before=consumer_context_stats(&tight);
        CHECK(xr_xir_compile_call_type_matches(&tight,&module,0,call,(XrXirType)384,(XrXirType)513)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        consumer_context_ephemeral_free(&tight,before);
    }
    budget=consumer_context_limits((XrCompileResourceLimits){67108864,8388608,1});
    CHECK(xr_xir_compile_call_type_matches(&budget,&module,0,call,(XrXirType)256,(XrXirType)385)==XR_XIR_BUDGET);
    xr_xir_compile_artifact_free(checked);checked=NULL;
}
static void generic_callable_contracts(void) {
    XrXirArtifact *checked = generic_callable_fixture(suite_context), *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    /* Two one-parameter signatures; the first span is independently forged. */
    CHECK(packet.length > 64 && wire32(packet.bytes + packet.length - 64) == 1);
    put32(packet.bytes + packet.length - 64,0); digest_packet(&packet);
    rejected(packet.bytes,packet.length);
    put32(packet.bytes + packet.length - 64,1); digest_packet(&packet);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    XrXirModule module = *xr_xir_compile_artifact_module(decoded);
    XrXirFunction functions[2]; memcpy(functions,module.functions,sizeof(functions)); module.functions = functions;
    XrXirType wrong[] = {(XrXirType)256,XR_XIR_STRING};
    const XrXirType *saved = functions[0].parameters; functions[0].parameters = wrong;
    CHECK(xir_fixture_verify(suite_context, &module, NULL) == XR_XIR_BAD_TYPE);
    functions[0].parameters = saved;
    XrXirTypeNode signatures[2]; memcpy(signatures,module.types->nodes,sizeof(signatures));
    XrXirTypes table = {signatures,2, NULL, NULL}; module.types = &table;
    XrXirCallableParameter bad = {(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1),0};
    signatures[0].parameters = &bad; signatures[0].parameter_span = 2;
    CHECK(xir_fixture_verify(suite_context, &module, NULL) == XR_XIR_BAD_TYPE);
    XrXirCompileContext budget = consumer_context_limits((XrCompileResourceLimits){67108864,8388608,1});
    CHECK(xr_xir_compile_call_type_matches(&budget, xr_xir_compile_artifact_module(decoded), 0, &functions[0].instructions[0], (XrXirType)256, (XrXirType)257) == XR_XIR_BUDGET);
    CHECK(xr_xir_compile_specialize(decoded, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
    const XrXirModule *m = xr_xir_compile_artifact_module(closed);
    CHECK(!m->generics && m->types->count == 1 && !m->types->nodes[0].parameter_span);
    CHECK(m->functions[0].parameters[0] == 256 && m->functions[1].parameters[0] == 256);
    CHECK(m->functions[1].instructions[0].op == XR_XIR_CALL_INDIRECT && m->functions[1].instructions[0].type == XR_XIR_STRING);
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered);lowered=NULL; xr_xir_compile_artifact_free(closed);closed=NULL;
}
#include "xir_capture_fixture.h"
static void capture_rejections(void) {
    XrXirArtifact *lowered = capture_fixture(suite_context, false); xr_xir_compile_artifact_free(lowered);lowered=NULL;
    for (unsigned mode = 0; mode < 8; ++mode) {
        XrXirArtifact *checked = capture_checked(suite_context, false);
        XrXirModule *m = (XrXirModule *) xr_xir_compile_artifact_module(checked);
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
        XrXirStatus status = xr_xir_compile_artifact_verify(checked, NULL);
        CHECK(status != XR_XIR_OK);
        if (mode == 3 || mode == 4 || mode == 7) CHECK(status == XR_XIR_BAD_TYPE);
        if (mode == 2) CHECK(status == XR_XIR_BAD_DOMINANCE);
        if (mode == 6) CHECK(status == XR_XIR_BAD_VALUE);
        XrXirCheckedPacket packet = {0};
        CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) != XR_XIR_OK && !packet.bytes);
        xr_xir_compile_artifact_free(checked);checked=NULL;
    }
}
static void capture_packet_rejection(void) {
    XrXirArtifact *checked = capture_checked(suite_context, false), *decoded = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    const uint32_t fields[] = {XR_XIR_FUNCTION_REF,256,0,1,0,0,5,0,0,1};
    uint8_t record[40];
    for (unsigned i = 0; i < 10; ++i) put32(record+4*i,fields[i]);
    size_t found = 0; unsigned matches = 0;
    for (size_t p = 64; p + sizeof(record) <= packet.length; ++p)
        if (!memcmp(packet.bytes+p,record,sizeof(record))) { found = p; ++matches; }
    CHECK(matches == 1);
    put32(packet.bytes+found+12,2); digest_packet(&packet);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_BAD_TYPE && !decoded);
    put32(packet.bytes+found+12,1); put32(packet.bytes+12,10); digest_packet(&packet);
    rejected(packet.bytes,packet.length);
    xr_xir_compile_checked_packet_free(&packet);
}
#include "xir_cell_checked_cases.h"
static void constructed_contracts(void) {
    XrXirArtifact *checked = constructed_fixture(suite_context), *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0}, second = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(decoded, &second, NULL) == XR_XIR_OK);
    CHECK(packet.length == second.length && !memcmp(packet.bytes,second.bytes,packet.length));
    xr_xir_compile_checked_packet_free(&second);
    /* The final CELL node has a fixed kind/span/element wire payload. */
    CHECK(wire32(packet.bytes+packet.length-24) == XR_XIR_TYPE_CELL);
    put32(packet.bytes+packet.length-24,4); digest_packet(&packet);
    rejected(packet.bytes,packet.length);
    put32(packet.bytes+packet.length-24,XR_XIR_TYPE_CELL);
    put32(packet.bytes+packet.length-16,267); digest_packet(&packet);
    rejected(packet.bytes,packet.length);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
    const XrXirModule *module = xr_xir_compile_artifact_module(closed);
    CHECK(!module->generics && module->function_count == 3 && module->types->count == 8);
    XrXirType first = module->functions[1].parameters[0], second_type = module->functions[2].parameters[0];
    CHECK(first != second_type);
    CHECK(xr_xir_array_element(module->types,xr_xir_array_element(module->types,first)) == XR_XIR_I64);
    CHECK(xr_xir_array_element(module->types,xr_xir_array_element(module->types,second_type)) == XR_XIR_STRING);
    for (uint32_t n = 0; n < module->types->count; ++n) CHECK(!module->types->nodes[n].parameter_span);
    CHECK(module->functions[1].parameters[1] != module->functions[2].parameters[1]);
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);closed=NULL;
    CHECK(xr_xir_compile_artifact_module(lowered)->functions[1].instructions[0].op == XR_XIR_OWNED_RETAIN);
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered);lowered=NULL;
}
#include "xir_array_checked_cases.h"

#include "xir_nominal_checked_fixture.h"
static void nominal_packet_cases(void) {
    for (unsigned arrays = 0; arrays < 4; ++arrays) {
        XrXirArtifact *source = nominal_checked_fixture(suite_context, arrays), *decoded = NULL, *specialized = NULL;
        XrXirCheckedPacket packet = {0}, again = {0};
        CHECK(xr_xir_compile_checked_write(source, &packet, NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_write(decoded, &again, NULL) == XR_XIR_OK);
        CHECK(packet.length == again.length && !memcmp(packet.bytes, again.bytes, packet.length));
        CHECK(xr_xir_compile_specialize(decoded, &specialized, NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_artifact_module(specialized)->types->nominals->count == 2);
        if (arrays == 2) {
            const XrXirTypes *types = xr_xir_compile_artifact_module(specialized)->types;
            CHECK(types->count == 5 && xr_xir_type_is_atomic(types, (XrXirType)259) &&
                xr_xir_type_is_cell(types,(XrXirType)260) && xr_xir_cell_element(types,(XrXirType)260)==XR_XIR_STRING && xr_xir_atomic_element(types, (XrXirType)259) == XR_XIR_I64 && types->nodes[0].nominal.arguments[0] == XR_XIR_I64 &&
                types->nodes[1].nominal.arguments[0] == XR_XIR_STRING && types->nodes[2].nominal.declaration == 1);
            CHECK(types->nodes[0].nominal.arguments != xr_xir_compile_artifact_module(decoded)->types->nodes[0].nominal.arguments);
        }
        if (arrays >= 2) {
            const XrXirTypes *types = xr_xir_compile_artifact_module(specialized)->types;
            uint32_t first = arrays == 3 ? 1 : 0;
            CHECK(types->count == (arrays == 3 ? 8u : 5u));
            CHECK(types->nodes[first].nominal.field_count == 2);
            CHECK(types->nodes[first].nominal.fields[1] == XR_XIR_STRING);
            CHECK(types->nodes[first].nominal.fields[0] == types->nodes[first + 2].nominal.fields[0]);
            CHECK(types->nodes[first].nominal.fields[0] != types->nodes[first + 1].nominal.fields[0]);
            if (arrays == 3) {
                CHECK(xr_xir_array_element(types, types->nodes[first].nominal.fields[0]) == XR_XIR_I64);
                CHECK(xr_xir_array_element(types, types->nodes[first + 1].nominal.fields[0]) == XR_XIR_STRING);
            } else CHECK(types->nodes[first].nominal.fields[0] == XR_XIR_I64);
            XrXirCheckedPacket closed_packet = {0}; XrXirArtifact *closed_read = NULL;
            CHECK(xr_xir_compile_checked_write(specialized, &closed_packet, NULL) == XR_XIR_OK);
            CHECK(xr_xir_compile_checked_read(suite_context, closed_packet.bytes, closed_packet.length, &closed_read, NULL) == XR_XIR_OK);
            uint8_t field_wire[32] = {4, 0, 0, 0};
            put32(field_wire + 12, 1); put32(field_wire + 16, XR_XIR_I64); put32(field_wire + 20, 2);
            put32(field_wire + 24, arrays == 3 ? XR_XIR_CONSTRUCTED_TYPE_BASE + 6 : XR_XIR_I64);
            put32(field_wire + 28, XR_XIR_STRING);
            size_t field_offset = 0; unsigned field_matches = 0;
            for (size_t i = 64; i + sizeof(field_wire) <= closed_packet.length; ++i)
                if (!memcmp(closed_packet.bytes + i, field_wire, sizeof(field_wire))) { field_offset = i; ++field_matches; }
            CHECK(field_matches == 1);
            put32(closed_packet.bytes + field_offset + 24, XR_XIR_BOOL); digest_packet(&closed_packet);
            rejected(closed_packet.bytes, closed_packet.length);
            put32(closed_packet.bytes + field_offset + 24, wire32(field_wire + 24)); digest_packet(&closed_packet);
            XrXirArtifact *repeated = NULL; XrXirCheckedPacket repeated_packet = {0};
            CHECK(xr_xir_compile_specialize(closed_read, &repeated, NULL) == XR_XIR_OK);
            CHECK(xr_xir_compile_checked_write(repeated, &repeated_packet, NULL) == XR_XIR_OK);
            CHECK(repeated_packet.length == closed_packet.length &&
                !memcmp(repeated_packet.bytes, closed_packet.bytes, closed_packet.length));
            xr_xir_compile_artifact_free(repeated);repeated=NULL; xr_xir_compile_checked_packet_free(&repeated_packet);
            xr_xir_compile_checked_packet_free(&closed_packet);
            XrXirType *fields = (XrXirType *) types->nodes[first].nominal.fields;
            XrXirType saved_field = fields[0]; fields[0] = XR_XIR_BOOL;
            CHECK(xr_xir_compile_artifact_verify(specialized, NULL) == XR_XIR_BAD_TYPE);
            fields[0] = saved_field;
            xr_xir_compile_artifact_free(specialized); specialized = NULL;
            CHECK(xr_xir_compile_artifact_verify(closed_read, NULL) == XR_XIR_OK);
            xr_xir_compile_artifact_free(closed_read);closed_read=NULL;
        }
        xr_xir_compile_artifact_free(specialized);specialized=NULL;
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
        put32(packet.bytes + packet.length - 52, 8); digest_packet(&packet); rejected(packet.bytes, packet.length);
        put32(packet.bytes + packet.length - 52, XR_XIR_FIELD_PRIVATE);
        put32(packet.bytes + packet.length - 56, XR_XIR_TYPE_PARAMETER_BASE + 1);
        digest_packet(&packet); rejected(packet.bytes, packet.length);
        put32(packet.bytes + packet.length - 56, XR_XIR_STRING);
        digest_packet(&packet);
        boundaries(&packet);
        xr_xir_compile_artifact_free(source);source=NULL;
        memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
        xr_xir_compile_checked_packet_free(&again);
        CHECK(xr_xir_compile_artifact_verify(decoded, NULL) == XR_XIR_OK);
        const XrXirNominalDeclaration *d = xr_xir_compile_artifact_module(decoded)->types->nominals->declarations;
        CHECK(d[0].module.bytes[d[0].module.length] == 0 && d[0].fields[0].name.bytes[5] == 0);
        CHECK(!memcmp(d[0].fields[0].name.bytes, "value", 5));
        xr_xir_compile_artifact_free(decoded);decoded=NULL;
    }
}

static void deep_nominal_fields(void) {
    enum { DEPTH = 160 };
    XrXirArtifact *base = nominal_checked_fixture(suite_context, 3), *checked = NULL, *closed = NULL;
    XrXirModule built = *xr_xir_compile_artifact_module(base); built.stage = XR_XIR_BUILT;
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
    XrXirTypes types = {nodes, DEPTH + 3, &table, NULL}; built.types = &types;
    CheckedAtomicPool atomic_pool;checked_atomic_pool(&built,&atomic_pool);
    XrXirCompileContext deep=consumer_context_default();
    CHECK(xir_fixture_check(&deep, &built, &checked, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(base);base=NULL; memset(nodes, 0xCC, sizeof(nodes)); memset(fields, 0xCC, sizeof(fields));
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    XrCompileResourceStats complete=consumer_context_stats(&deep);
    /* The failure ledger is sealed before replaying the whole producer prefix. */
    XrXirCompileContext tight=consumer_context_limits((XrCompileResourceLimits){67108864,8388608,complete.work-1});
    XrXirModule replay=*xr_xir_compile_artifact_module(checked);replay.stage=XR_XIR_BUILT;
    XrXirArtifact *bounded=NULL,*failed=NULL;
    CHECK(xir_fixture_check(&tight, &replay, &bounded, NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(bounded,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(bounded,&failed,NULL)==XR_XIR_BUDGET && !failed);
    CHECK(consumer_context_stats(&tight).work<=complete.work-1);
    xr_xir_compile_artifact_free(bounded);bounded=NULL;
    xr_xir_compile_artifact_free(checked);checked=NULL;
    const XrXirTypes *result = xr_xir_compile_artifact_module(closed)->types;
    CHECK(result->count == DEPTH * 3 + 5 &&
        xr_xir_type_is_cell(result,(XrXirType)(256+DEPTH+4)) &&
        xr_xir_cell_element(result,(XrXirType)(256+DEPTH+4))==XR_XIR_STRING && xr_xir_type_is_atomic(result, (XrXirType)(256 + DEPTH + 3)) && xr_xir_atomic_element(result, (XrXirType)(256 + DEPTH + 3)) == XR_XIR_I64);
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
    CHECK(xr_xir_compile_artifact_verify(closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);closed=NULL;
}

#include "xir_nominal_chain_fixture.h"
static void nominal_storage_layout(void) {
    XrXirArtifact *checked = nominal_chain_fixture(suite_context, 2, 2), *closed = NULL, *lowered = NULL;
    XrXirNominalDeclaration *d = (XrXirNominalDeclaration *) xr_xir_compile_artifact_module(checked)->types->nominals->declarations;
    ((XrXirNominalField *) d[0].fields)[0].type = XR_XIR_BOOL;
    ((XrXirNominalField *) d[1].fields)[1].type = XR_XIR_I32;
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirLayout layout = {0}; uint32_t offsets[2] = {99, 99};
    XrXirCompileContext budget = consumer_context_default(), original = budget;
    CHECK(xr_xir_compile_nominal_layout(&budget, xr_xir_compile_artifact_module(checked)->types, (XrXirType)256, &target, &layout, offsets, 2) == XR_XIR_BAD_LAYOUT);
    CHECK(!layout.size && !layout.alignment && offsets[0] == 99 && !memcmp(&budget, &original, sizeof(budget)));
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    for (unsigned mode = 0; mode < 2; ++mode) {
        const XrXirTypes *types = xr_xir_compile_artifact_module(mode ? lowered : closed)->types;
        budget = original;
        CHECK(xr_xir_compile_nominal_layout(&budget, types, (XrXirType)256, &target, &layout, offsets, 2) == XR_XIR_OK);
        CHECK(layout.size == 24 && layout.alignment == 8 && offsets[0] == 0 && offsets[1] == 8);
        budget = original;
        CHECK(xr_xir_compile_nominal_layout(&budget, types, (XrXirType)257, &target, &layout, offsets, 2) == XR_XIR_OK);
        CHECK(layout.size == 16 && layout.alignment == 8 && offsets[0] == 0 && offsets[1] == 8);
        target.abi_version--;
        CHECK(xr_xir_compile_nominal_layout(&budget, types, (XrXirType)256, &target, &layout, offsets, 2) == XR_XIR_BAD_LAYOUT);
        target.abi_version++;
    }
    xr_xir_compile_artifact_free(closed);closed=NULL; xr_xir_compile_artifact_free(lowered);lowered=NULL;
    checked = nominal_chain_fixture(suite_context, 160, 2); closed = NULL;
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL; budget = original; layout=(XrXirLayout){0};offsets[0] = offsets[1] = 99;
    CHECK(xr_xir_compile_nominal_layout(&budget, xr_xir_compile_artifact_module(closed)->types, (XrXirType)256, &target, &layout, offsets, 2) == XR_XIR_BAD_LAYOUT);
    CHECK(!layout.size && !layout.alignment && offsets[0] == 99 && offsets[1] == 99 &&
        !memcmp(&budget, &original, sizeof(budget)));
    xr_xir_compile_artifact_free(closed);closed=NULL;
    checked = nominal_chain_fixture(suite_context, 160, 1); closed = NULL;
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL; budget = original;
    CHECK(xr_xir_compile_nominal_layout(&budget, xr_xir_compile_artifact_module(closed)->types, (XrXirType)256, &target, &layout, offsets, 1) == XR_XIR_OK);
    CHECK(layout.size == 8 && layout.alignment == 8 && offsets[0] == 0);
    xr_xir_compile_artifact_free(closed);closed=NULL;
    NominalIdentityFixture empty; nominal_identity_fixture(&empty);
    empty.identities[0].fields = NULL; empty.identities[0].field_count = 0;
    XrXirTypeNode node = {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {0}};
    XrXirTypes types = {&node, 1, &empty.table, NULL}; budget = original;
    CHECK(xr_xir_compile_nominal_layout(&budget, &types, (XrXirType)256, &target, &layout, NULL, 0) == XR_XIR_OK);
    CHECK(!layout.size && layout.alignment == 1);
}
static void nominal_field_graph(void) {
    XrXirArtifact *checked = nominal_chain_fixture(suite_context, 160, 2), *closed = NULL, *lowered = NULL;
    XrXirNominalDeclaration *declarations = (XrXirNominalDeclaration *) xr_xir_compile_artifact_module(checked)->types->nominals->declarations;
    XrXirNominalField *last = (XrXirNominalField *) declarations[159].fields;
    last[0].type = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_BAD_TYPE && !closed);
    last[0].type = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + 159);
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_BAD_TYPE);
    last[0].type = XR_XIR_STRING;
    const XrXirLiteral original = declarations[159].module;
    declarations[159].module = (XrXirLiteral) {"beta", 4};
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_BAD_TYPE);
    declarations[159].exported = 1;
    /* Export alone cannot supply the declaring module's missing import edge. */
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_BAD_TYPE);
    declarations[159].module = original; declarations[159].exported = 0;
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);closed=NULL;
    XrXirTypes *types = (XrXirTypes *) xr_xir_compile_artifact_module(lowered)->types;
    CHECK(types->count == 162 && xr_xir_type_is_cell(types,(XrXirType)417) &&
        xr_xir_cell_element(types,(XrXirType)417)==XR_XIR_STRING && xr_xir_type_is_atomic(types, (XrXirType)416) && xr_xir_atomic_element(types, (XrXirType)416) == XR_XIR_I64 && types->nodes[0].nominal.fields[0] == (XrXirType)257);
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    XrXirType *end = (XrXirType *) types->nodes[159].nominal.fields;
    end[0] = (XrXirType)256;
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_BAD_TYPE);
    end[0] = XR_XIR_STRING;
    xr_xir_compile_artifact_free(lowered);lowered=NULL;
}
static void nominal_projection_cases(void) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        XrXirArtifact *lowered = nominal_lowered_fixture(suite_context, mode), *rejected_artifact = NULL;
        const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
        const XrXirTypes *types = module->types;
        CHECK(types && types->nominals && !types->nominals->declarations && types->nominals->identities);
        CHECK(types->count == (mode == 3 ? 7u : mode == 2 ? 5u : 2u));
        XrXirType cell=(XrXirType)(mode>=2 ? 260 : 257);
        CHECK(xr_xir_type_is_cell(types,cell) && xr_xir_cell_element(types,cell)==XR_XIR_STRING);
        CHECK(types->nominals->identities[0].arity == 1 && types->nominals->identities[0].field_count == 2);
        if (mode >= 2) {
            CHECK(types->nodes[0].nominal.fields[0] == (mode == 3 ? (XrXirType)261 : XR_XIR_I64));
            CHECK(types->nodes[1].nominal.fields[0] == (mode == 3 ? (XrXirType)262 : XR_XIR_STRING));
        }
        for (uint32_t i = 0; i < types->count; ++i) CHECK(!types->nodes[i].parameter_span);
        XrXirModule forged = *module; forged.stage = XR_XIR_CHECKED;
        CHECK(xir_fixture_recheck(suite_context, &forged, &rejected_artifact, NULL) == XR_XIR_BAD_STAGE && !rejected_artifact);
        XrXirTypes *copy = NULL;
        CHECK(xr_xir_compile_types_clone(suite_context, types, &copy) == XR_XIR_OK);
        xr_xir_compile_artifact_free(lowered);lowered=NULL;
        XrXirCompileContext budget = consumer_context_default();
        CHECK(xr_xir_compile_types_structure_verify(&budget, copy) == XR_XIR_OK);
        CHECK(!memcmp(copy->nominals->identities[0].module.bytes, "alpha", 5));
        xr_xir_compile_types_free(copy);
    }
    XrXirArtifact *checked = nominal_checked_fixture(suite_context, 3), *lowered = NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked, &target, &lowered, NULL) == XR_XIR_BAD_STAGE && !lowered);
    xr_xir_compile_artifact_free(checked);checked=NULL;
}

static void nominal_member_authority(void) {
    XrXirArtifact *checked = nominal_checked_fixture(suite_context, 3), *decoded = NULL, *closed = NULL, *lowered = NULL;
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    XrXirFunctionIdentity *ids = (XrXirFunctionIdentity *) module->declarations->functions;
    XrXirCompileContext access=consumer_context_limits((XrCompileResourceLimits){67108864,8388608,100001});
    CHECK(xr_xir_compile_nominal_access(&access, module, 7, 0, 0, XR_XIR_NOMINAL_TYPE) == XR_XIR_OK);
    CHECK(xr_xir_compile_nominal_access(&access, module, 5, 0, 0, XR_XIR_NOMINAL_TYPE) == XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_nominal_access(&access, module, 4, 0, 1, XR_XIR_NOMINAL_READ) == XR_XIR_OK);
    CHECK(xr_xir_compile_nominal_access(&access, module, 7, 0, 1, XR_XIR_NOMINAL_READ) == XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_nominal_access(&access, module, 3, 0, 0, XR_XIR_NOMINAL_READ) == XR_XIR_OK);
    CHECK(xr_xir_compile_nominal_access(&access, module, 5, 0, 0, XR_XIR_NOMINAL_READ) == XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_nominal_access(&access, module, 4, 0, 0, XR_XIR_NOMINAL_CONSTRUCT) == XR_XIR_OK);
    CHECK(xr_xir_compile_nominal_access(&access, module, 7, 0, 0, XR_XIR_NOMINAL_CONSTRUCT) == XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_nominal_access(&access, module, 4, 0, 1, XR_XIR_NOMINAL_WRITE) == XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_nominal_access(&access, module, 4, 0, 0, XR_XIR_NOMINAL_WRITE) == XR_XIR_OK);
    CHECK(xr_xir_compile_nominal_access(&access, module, 4, 0, 2, XR_XIR_NOMINAL_READ) == XR_XIR_BAD_STRUCTURE);
    XrXirNominalDeclaration *d = (XrXirNominalDeclaration *) module->types->nominals->declarations;
    d[0].exported = 0;
    CHECK(xr_xir_compile_nominal_access(&access, module, 3, 0, 0, XR_XIR_NOMINAL_READ) == XR_XIR_BAD_TYPE);
    d[0].exported = 1;
    XrXirNominalField *fields = (XrXirNominalField *) d[0].fields;
    fields[1].flags = XR_XIR_FIELD_PROTECTED;
    CHECK(xr_xir_compile_nominal_access(&access, module, 4, 0, 1, XR_XIR_NOMINAL_READ) == XR_XIR_OK);
    CHECK(xr_xir_compile_nominal_access(&access, module, 7, 0, 1, XR_XIR_NOMINAL_READ) == XR_XIR_BAD_TYPE);
    fields[1].flags = XR_XIR_FIELD_PRIVATE;
    const uint32_t invalid_functions[] = {7, 5, 2, 3};
    for (unsigned i = 0; i < 4; ++i) {
        ids[invalid_functions[i]].nominal_owner = i ? 1 : 3;
        CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_BAD_STRUCTURE);
        ids[invalid_functions[i]].nominal_owner = 0;
    }
    access=consumer_context_limits((XrCompileResourceLimits){67108864,8388608,1});
    CHECK(xr_xir_compile_nominal_access(&access, module, 4, 0, 0, XR_XIR_NOMINAL_READ) == XR_XIR_BUDGET);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);closed=NULL; module = xr_xir_compile_artifact_module(lowered); access=consumer_context_limits((XrCompileResourceLimits){67108864,8388608,10001});
    CHECK(module->declarations->functions[4].nominal_owner == 1);
    CHECK(xr_xir_compile_nominal_access(&access, module, 4, 0, 1, XR_XIR_NOMINAL_READ) == XR_XIR_OK);
    CHECK(xr_xir_compile_nominal_access(&access, module, 7, 0, 1, XR_XIR_NOMINAL_READ) == XR_XIR_BAD_TYPE);
    xr_xir_compile_artifact_free(lowered);lowered=NULL;
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
    const XrXirModule built = {XR_XIR_BUILT, &function, 1, NULL, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL, *decoded = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xir_fixture_check(suite_context, &built, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    CHECK(packet.length == 309 && packet.bytes[117] == XR_XIR_LOCAL_UNINIT && packet.bytes[157] == XR_XIR_LOCAL_WRITE);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
    uint8_t original[309]; memcpy(original, packet.bytes, sizeof(original));
    /* A second write is valid for mutable storage, but not initialize-once. */
    put32(packet.bytes + 197, XR_XIR_LOCAL_WRITE); put32(packet.bytes + 201, XR_XIR_UNIT);
    put32(packet.bytes + 209, 0); put32(packet.bytes + 245, 0); digest_packet(&packet);
    decoded = NULL;
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded); decoded = NULL;
    put32(packet.bytes + 141, 1); digest_packet(&packet);
    XrXirDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, &diagnostic) == XR_XIR_BAD_VALUE && !decoded);
    CHECK(diagnostic.status == XR_XIR_BAD_VALUE && diagnostic.reason == XR_XIR_DIAGNOSTIC_READONLY_WRITE &&
        diagnostic.function == 0 && diagnostic.block == 0 && diagnostic.instruction == 2);
    memcpy(packet.bytes, original, sizeof(original));
    /* Repairing the digest must not hide removal of the initializing write. */
    put32(packet.bytes + 157, XR_XIR_SUSPEND); put32(packet.bytes + 165, 0);
    digest_packet(&packet);
    decoded = NULL;
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, &diagnostic) == XR_XIR_BAD_VALUE);
    CHECK(diagnostic.status == XR_XIR_BAD_VALUE && diagnostic.reason == XR_XIR_DIAGNOSTIC_UNINITIALIZED_READ &&
        diagnostic.function == 0 && diagnostic.block == 0 && diagnostic.instruction == 2);
    CHECK(!decoded);
    xr_xir_compile_checked_packet_free(&packet);
}
static void nominal_callable_components(void) {
    XrXirArtifact *base = nominal_chain_fixture(suite_context, 1, 1), *checked = NULL, *decoded = NULL;
    XrXirModule built = *xr_xir_compile_artifact_module(base); built.stage = XR_XIR_BUILT;
    XrXirCallableParameter parameter = {(XrXirType)256, 0};
    XrXirTypeNode nodes[2] = {built.types->nodes[0],
        {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT, &parameter, 1, (XrXirType)256, XR_XIR_CALLABLE_ROOT_UNRESOLVED, 0, {0}}};
    XrXirTypes types = {nodes, 2, built.types->nominals, NULL}; built.types = &types;
    CheckedAtomicPool atomic_pool;checked_atomic_pool(&built,&atomic_pool);
    CHECK(xir_fixture_check(suite_context, &built, &checked, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);decoded=NULL; xr_xir_compile_artifact_free(checked); checked = NULL;
    uint8_t pattern[28]; const uint32_t words[] = {1, 0, 1, 256, 0, 256, 8};
    for (unsigned i = 0; i < 7; ++i) put32(pattern + i * 4, words[i]);
    size_t offset = 0; unsigned matches = 0;
    for (size_t i = 64; i + sizeof(pattern) <= packet.length; ++i)
        if (!memcmp(packet.bytes + i, pattern, sizeof(pattern))) { offset = i; ++matches; }
    CHECK(matches == 1);
    put32(packet.bytes + offset + 20, 257); digest_packet(&packet); rejected(packet.bytes, packet.length);
    xr_xir_compile_checked_packet_free(&packet);
    atomic_pool.nodes[1].result = (XrXirType)257;
    CHECK(xir_fixture_check(suite_context, &built, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
    atomic_pool.nodes[1].result = (XrXirType)256; atomic_pool.nodes[1].parameter_span = 1;
    CHECK(xir_fixture_check(suite_context, &built, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
    atomic_pool.nodes[1].parameter_span = 0; parameter.type = XR_XIR_UNIT;
    CHECK(xir_fixture_check(suite_context, &built, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
    parameter.type = (XrXirType)257;
    CHECK(xir_fixture_check(suite_context, &built, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
    xr_xir_compile_artifact_free(base);base=NULL;
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
    XrXirModule built = {XR_XIR_BUILT,&function,1,NULL,NULL,NULL,NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL, *decoded = NULL;
    CHECK(xir_fixture_check(suite_context, &built, &checked, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
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
    xr_xir_compile_artifact_free(checked); checked=NULL;
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    memset(packet.bytes,0xCC,packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
    for (unsigned i = 0; i < 2; ++i) {
        parameters[i] = XR_XIR_I64;
        CHECK(xir_fixture_check(suite_context, &built, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
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
    XrXirModule built = {XR_XIR_BUILT,&function,1,NULL,NULL,NULL,NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL, *decoded = NULL;
    CHECK(xir_fixture_check(suite_context, &built, &checked, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
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
    xr_xir_compile_artifact_free(checked); checked=NULL;
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(decoded, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(decoded);decoded=NULL;
    for (unsigned i = 0; i < 3; ++i) {
        XrXirType saved = parameters[i]; parameters[i] = i==2 ? XR_XIR_STRING : XR_XIR_I64;
        CHECK(xir_fixture_check(suite_context, &built, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
        parameters[i] = saved;
    }
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t saved = operands[i]; operands[i] = i==2 ? 4 : 3;
        CHECK(xir_fixture_check(suite_context, &built, &checked, NULL) != XR_XIR_OK && !checked); operands[i]=saved;
    }
}
#include "xir_enum_checked_fixture.h"
static void enum_packet_cases(void) {
    XrXirArtifact *source = enum_checked_fixture(suite_context), *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(source, &packet, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(source);source=NULL;
    CHECK(xr_xir_compile_specialize(decoded, &closed, NULL) == XR_XIR_OK);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);decoded=NULL; xr_xir_compile_artifact_free(closed);closed=NULL;
    const XrXirNominalIdentity *identity = xr_xir_compile_artifact_module(lowered)->types->nominals->identities;
    CHECK(identity->kind == XR_XIR_NOMINAL_ENUM && identity->variant_count == 3);
    CHECK(identity->variants[2].field_begin == 1 && !memcmp(identity->variants[2].name.bytes, "Right", 5));
    size_t at = 0;
    for (size_t i = 64; i + 13 <= packet.length; ++i)
        if (!memcmp(packet.bytes + i, "Right", 5)) { at = i + 5; break; }
    CHECK(at); put32(packet.bytes + at, UINT32_MAX); digest_packet(&packet);
    rejected(packet.bytes, packet.length);
    xr_xir_compile_checked_packet_free(&packet); xr_xir_compile_artifact_free(lowered);lowered=NULL;
}

#include "xir_enum_layout_cases.h"
#include "xir_enum_generic_fixture.h"
#include "xir_enum_packet_cases.h"
static void match_fault_shape(void) {
    XrXirInstruction op={XR_XIR_MATCH_FAIL,XR_XIR_UNIT,{0},{0},0, {0}};
    const XrXirBlock block={0,1, 0, 0};
    const XrXirFunction fn={"fault",5,NULL,0,XR_XIR_UNIT,&block,1,&op,1,NULL,0};
    const XrXirModule module={XR_XIR_BUILT,&fn,1,NULL,NULL,NULL,NULL, XR_XIR_PROGRAM, NULL};
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
        XrXirStatus status=xir_fixture_check(suite_context, &module, &checked, NULL);
        if (variant) { CHECK(status!=XR_XIR_OK && !checked); continue; }
        CHECK(status==XR_XIR_OK && checked);
        XrXirCheckedPacket packet={0}; XrXirArtifact *decoded=NULL;
        CHECK(xr_xir_compile_checked_write(checked, &packet, NULL)==XR_XIR_OK);
        xr_xir_compile_artifact_free(checked);checked=NULL;
        CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_artifact_module(decoded)->functions[0].instructions[0].op==XR_XIR_MATCH_FAIL);
        xr_xir_compile_checked_packet_free(&packet); xr_xir_compile_artifact_free(decoded);decoded=NULL;
    }
}
#include "xir_invoke_checked_cases.h"
#include "xir_cleanup_role_cases.h"
#include "xir_cleanup_frontier_cases.h"
#include "xir_path_checked_cases.h"
#include "xir_interface_checked_cases.h"
#include "xir_constraint_packet_cases.h"
#include "xir_implementation_packet_cases.h"
#include "xir_generic_method_packet_cases.h"
#include "xir_generic_method_boundary_cases.h"
#include "xir_default_binding_cases.h"
#include "xir_default_owner_tuple_cases.h"
#include "xir_default_invoke_cases.h"
int main(void) {
    consumer_context=consumer_context_default();
    consumer_context=consumer_context_default();
    default_invoke_cases();
    consumer_context=consumer_context_default();
    default_binding_cases();
    consumer_context=consumer_context_default();
    default_owner_tuple_cases();
    consumer_context=consumer_context_default();
    generic_method_packet_cases();
    consumer_context=consumer_context_default();
    generic_method_boundary_cases();
    consumer_context=consumer_context_default();
    implementation_packet_cases();
    consumer_context=consumer_context_default();
    constraint_packet_cases();
    consumer_context=consumer_context_default();
    interface_checked_cases();
    consumer_context=consumer_context_default();
    path_array_checked_cases(); path_field_checked_cases(); path_ancestor_checked_cases();
    consumer_context=consumer_context_default();
    cleanup_role_cases();
    consumer_context=consumer_context_default();
    cleanup_frontier_cases();
    cleanup_error_frontier();
    consumer_context=consumer_context_default();
    invoke_checked_cases();
    consumer_context=consumer_context_default();
    match_fault_shape();
    consumer_context=consumer_context_default();
    enum_generic_cases(); enum_instruction_packet_cases();
    consumer_context=consumer_context_default();
    enum_storage_layout(); enum_tag_widths();
    consumer_context=consumer_context_default();
    enum_packet_cases();
    consumer_context=consumer_context_default();
    string_search_packets();
    consumer_context=consumer_context_default();
    string_query_contracts();
    consumer_context=consumer_context_default();
    nominal_callable_components();
    consumer_context=consumer_context_default();
    uninitialized_packet();
    for (unsigned invalid = 1; invalid <= 11; ++invalid) CHECK(!struct_set_checked(suite_context, invalid));
    for (unsigned invalid = 1; invalid <= 11; ++invalid) CHECK(!struct_ops_checked(suite_context, invalid));
    consumer_context=consumer_context_default();
    nominal_member_authority();
    consumer_context=consumer_context_default();
    nominal_storage_layout();
    consumer_context=consumer_context_default();
    nominal_field_graph();
    consumer_context=consumer_context_default();
    nominal_projection_cases();
    consumer_context=consumer_context_default();
    deep_nominal_fields();
    consumer_context=consumer_context_default();
    nominal_packet_cases();
    consumer_context=consumer_context_default();
    array_checked_cases();
    consumer_context=consumer_context_default();
    constructed_contracts();
    consumer_context=consumer_context_default();
    cell_checked_cases();
    consumer_context=consumer_context_default();
    capture_rejections(); capture_packet_rejection();
    consumer_context=consumer_context_default();
    generic_callable_contracts(); generic_callable_depth();
    consumer_context=consumer_context_default();
    callable_contracts(); function_ir_rejections();
    XrXirArtifact *checked = checked_fixture(suite_context), *decoded = NULL;
    XrXirCheckedPacket packet = {0}, second = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked, &second, NULL) == XR_XIR_OK);
    CHECK(packet.length == second.length && !memcmp(packet.bytes, second.bytes, packet.length));
    xr_xir_compile_checked_packet_free(&second);
    checked->module.stage = XR_XIR_BUILT;
    CHECK(xr_xir_compile_checked_write(checked, &second, NULL) == XR_XIR_BAD_STAGE && !second.bytes);
    checked->module.stage = XR_XIR_LOWERED;
    CHECK(xr_xir_compile_checked_write(checked, &second, NULL) == XR_XIR_BAD_STAGE && !second.bytes);
    checked->module.stage = XR_XIR_CHECKED;
    checked->target.architecture = XR_XIR_ARCH_X86_64;
    CHECK(xr_xir_compile_checked_write(checked, &second, NULL) == XR_XIR_BAD_LAYOUT && !second.bytes);
    checked->target.architecture = 0;
    boundaries(&packet); semantic_attacks(&packet); declaration_attacks(&packet); byte_order();
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(decoded, &second, NULL) == XR_XIR_OK);
    CHECK(packet.length == second.length && !memcmp(packet.bytes, second.bytes, packet.length));
    xr_xir_compile_artifact_free(checked);checked=NULL;
    memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_checked_packet_free(&second);
    CHECK(xr_xir_compile_artifact_verify(decoded, NULL) == XR_XIR_OK);
    const XrXirDeclarations *d = xr_xir_compile_artifact_module(decoded)->declarations;
    CHECK(d->literal_count == 5 && d->literals[0].length == 5 && d->literals[0].bytes[1] == 0);
    CHECK(d->literals[4].length == 0 && !d->literals[4].bytes);
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
    puts("Checked packet canonical bytes, hostile input and independent ownership passed");
    consumer_contexts_free();
    return 0;
}
