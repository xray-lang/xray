/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_string_checked.c - String mutation origin and operand admission
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_consumer_context_owner.h"
static void string_checked(unsigned attack) {
    bool positive = !attack || (attack >= 5 && attack <= 8);
    XrXirTypeNode node = {.kind = XR_XIR_TYPE_CELL, .element = XR_XIR_STRING};
    XrXirTypes types = {&node, 1, NULL, NULL};
    XrXirInstruction done = {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    XrXirInstruction entry[] = {
        {XR_XIR_CONST_STRING, XR_XIR_STRING, {0}, {0}, 0, {0}},
        {XR_XIR_CELL_NEW, (XrXirType)256, {0}, {0}, 0, {0}},
        {XR_XIR_CONST_STRING, XR_XIR_STRING, {0}, {0}, 1, {0}},
        {XR_XIR_CELL_STRING_APPEND, XR_XIR_UNIT, {1, 2}, {0}, 0, {0}},
        {XR_XIR_CELL_READ, XR_XIR_STRING, {1}, {0}, 0, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {5}, {0}, 0, {0}}};
    XrXirInstruction scoped[] = {
        {XR_XIR_CONST_STRING, XR_XIR_STRING, {0}, {0}, 1, {0}},
        {XR_XIR_CELL_STRING_APPEND, XR_XIR_UNIT, {0, 1}, {0}, 0, {0}},
        {XR_XIR_CELL_READ, XR_XIR_STRING, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {3}, {0}, 0, {0}}};
    XrXirBlock blocks[] = {{0, 1, 0, 0}, {0, 7, 0, 0}, {0, 4, 0, 0}};
    XrXirType parameter = (XrXirType)256; uint32_t argument = 1;
    XrXirFunction functions[] = {
        {"init", 4, NULL, 0, XR_XIR_UNIT, &blocks[0], 1, &done, 1, NULL, 0},
        {"entry", 5, NULL, 0, XR_XIR_I64, &blocks[1], 1, entry, 7, NULL, 0},
        {"ref", 3, &parameter, 1, XR_XIR_STRING, &blocks[2], 1, scoped, 4, NULL, 0}};
    XrXirFunctionIdentity identities[3] = {{0}};
    XrXirSourceModule source = {"root", 4, NULL, 0, 0};
    XrXirLiteral literals[] = {{"a", 1}, {"b", 1}};
    XrXirDeclarations declarations = {&source, 1, identities, NULL, 0, literals, 2, 0, 1, NULL};
    XrXirModule module = {XR_XIR_BUILT, functions, 2, &declarations, NULL, &types, NULL, XR_XIR_PROGRAM, NULL};
    if (attack == 1) entry[3].type = XR_XIR_STRING;
    if (attack == 2) entry[3].args[0] = 0;
    if (attack == 3) entry[3].args[1] = 1;
    if (attack >= 5 && attack <= 8) {
        entry[3].immediate = 1;
        entry[2] = attack == 5 ? (XrXirInstruction){XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, -17, {0}} :
            attack == 6 ? (XrXirInstruction){XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 1, {0}} :
            attack == 7 ? (XrXirInstruction){XR_XIR_CONST_RUNE, XR_XIR_RUNE, {0}, {0}, 0x03a9, {0}} :
            (XrXirInstruction){XR_XIR_CONST_FLOAT, XR_XIR_F64, {0}, {0}, INT64_C(0x3ff8000000000000), {0}};
    }
    if (attack == 9 || attack == 10) entry[3].immediate = attack == 9 ? 2 : -1;
    if (attack == 11) entry[3].immediate = 1;
    if (attack == 12) entry[2] = (XrXirInstruction){XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 7, {0}};
    if (attack == 4 || attack == 13) {
        if (attack == 13) {
            scoped[0] = (XrXirInstruction){XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 7, {0}};
            scoped[1].immediate = 1;
        }
        module.function_count = 3;
        entry[3] = (XrXirInstruction){XR_XIR_CALL, XR_XIR_STRING, {0, 1}, {0}, 2, {0}};
        functions[1].operands = &argument; functions[1].operand_count = 1;
    }
    XrXirCompileContext context = consumer_context_ephemeral((XrCompileResourceLimits){67108864, 8388608, 128000000});
    XrCompileResourceStats baseline = consumer_context_stats(&context);
    XrXirArtifact *checked = NULL, *decoded = NULL; XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xir_fixture_check(&context, &module, &checked, &diagnostic);
    if (positive) {
        if (status != XR_XIR_OK) fprintf(stderr,"String owned status=%u at %u:%u\n",status,diagnostic.function,diagnostic.instruction);
        CHECK(status == XR_XIR_OK && checked);
        XrXirCheckedPacket packet = {0};
        CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(checked); checked = NULL;
        memset(entry, 0xcc, sizeof(entry)); memset(&node, 0xcc, sizeof(node));
        CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_artifact_verify(decoded, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(decoded); xr_xir_compile_checked_packet_free(&packet);
    } else {
        XrXirStatus expected = attack == 4 || attack == 13 ? XR_XIR_BAD_VALUE :
            attack == 9 || attack == 10 ? XR_XIR_BAD_STRUCTURE : XR_XIR_BAD_TYPE;
        if (status != expected)
            fprintf(stderr, "String Checked attack=%u status=%u location=%u:%u\n", attack, status, diagnostic.function, diagnostic.instruction);
        CHECK(status == expected && !checked);
    }
    consumer_context_ephemeral_free(&context, baseline);
}
int main(void) {
    _Static_assert(XR_XIR_CELL_STRING_APPEND == 153 && XR_XIR_OP_COUNT == 176 && XR_XIR_CHECKED_CONTRACT == 83,
        "String mutation instruction and semantic identity");
    consumer_context = consumer_context_default();
    for (unsigned attack = 0; attack < 14; ++attack) string_checked(attack);
    consumer_contexts_free();
    puts("String Checked owned root replay and wrong result/root/suffix/scoped origin rejection PASS"); return 0;
}
