/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_nominal_expression_fixture.h - Generated nested nominal instance closure
 */
#ifndef XIR_NOMINAL_EXPRESSION_FIXTURE_H
#define XIR_NOMINAL_EXPRESSION_FIXTURE_H
#include "xir/xxir_internal.h"
#include "xir/xxir_checked.h"
static XrXirArtifact *nominal_expression_fixture(void) {
    XrXirType t = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
    XrXirType box = (XrXirType)256, outer = (XrXirType)257;
    uint32_t constraint = 0;
    XrXirNominalField fields[] = {{{"value", 5}, t, 0}, {{"inner", 5}, box, 0}};
    XrXirNominalDeclaration definitions[] = {
        {{"alpha", 5}, {"Box", 3}, 1, &constraint, 1, fields, 1},
        {{"alpha", 5}, {"Outer", 5}, 1, &constraint, 1, fields + 1, 1}};
    XrXirNominalTable table = {definitions, 2, NULL};
    XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 1, {0, &t, 1, NULL, 0}},
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 1, {1, &t, 1, NULL, 0}}};
    XrXirTypes types = {nodes, 2, &table};
    XrXirInstruction entry[] = {
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 7},
        {XR_XIR_CALL, XR_XIR_I64, {0, 1}, {0, 1}, 1},
        {XR_XIR_CONST_INT, XR_XIR_U8, {0}, {0}, 9},
        {XR_XIR_CALL, XR_XIR_U8, {1, 1}, {1, 1}, 1},
        {XR_XIR_CONST_STRING, XR_XIR_STRING, {0}, {0}, 0},
        {XR_XIR_CALL, XR_XIR_STRING, {2, 1}, {2, 1}, 1},
        {XR_XIR_PRINT, XR_XIR_UNIT, {3, 3}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0}};
    XrXirInstruction body[] = {
        {XR_XIR_STRUCT_NEW, box, {0, 1}, {0}, 0},
        {XR_XIR_STRUCT_NEW, outer, {1, 1}, {0}, 0},
        {XR_XIR_STRUCT_GET, box, {2}, {0}, 0},
        {XR_XIR_STRUCT_GET, t, {3}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {4}, {0}, 0}};
    XrXirInstruction init = {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0};
    XrXirInstruction escape[] = {
        {XR_XIR_CONST_STRING, XR_XIR_STRING, {0}, {0}, 0},
        {XR_XIR_CALL, XR_XIR_STRING, {0, 1}, {0, 1}, 1},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0}};
    uint32_t operands[] = {0, 2, 4, 1, 3, 5}, body_operands[] = {0, 1}, escape_operand = 0;
    XrXirBlock blocks[] = {{0, 8}, {0, 5}, {0, 1}, {0, 3}};
    XrXirFunction functions[] = {
        {"root", 4, NULL, 0, XR_XIR_I64, blocks, 1, entry, 8, operands, 6},
        {"wrap", 4, &t, 1, t, blocks + 1, 1, body, 5, body_operands, 2},
        {"init", 4, NULL, 0, XR_XIR_UNIT, blocks + 2, 1, &init, 1, NULL, 0},
        {"escape", 6, NULL, 0, XR_XIR_STRING, blocks + 3, 1, escape, 3, &escape_operand, 1}};
    XrXirType arguments[] = {XR_XIR_I64, XR_XIR_U8, XR_XIR_STRING};
    XrXirGeneric generics[] = {{NULL, 0, arguments, 3}, {&constraint, 1, NULL, 0}, {0}, {NULL,0,arguments+2,1}};
    XrXirSourceModule source = {"alpha", 5, NULL, 0, 2};
    XrXirFunctionIdentity identities[4] = {{0}}; identities[3].exported = 1;
    XrXirLiteral literal = {"generic",7};
    XrXirDeclarations declarations = {&source, 1, identities, NULL, 0, &literal, 1, 0, 0};
    XrXirModule built = {XR_XIR_BUILT, functions, 4, &declarations, generics, &types, NULL};
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    return checked;
}
static inline XrXirArtifact *nominal_forwarding_checked(void) {
    XrXirArtifact *base = nominal_expression_fixture(), *checked = NULL, *closed = NULL;
    XrXirModule built = base->module; built.stage = XR_XIR_BUILT;
    XrXirFunction functions[5]; memcpy(functions, built.functions, 4 * sizeof(*functions));
    XrXirGeneric generics[5] = {0}; memcpy(generics, built.generics, 4 * sizeof(*generics));
    XrXirFunctionIdentity identities[5] = {{0}};
    memcpy(identities, built.declarations->functions, 4 * sizeof(*identities));
    XrXirDeclarations declarations = *built.declarations; declarations.functions = identities;
    XrXirType t = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE, outer = (XrXirType)257;
    uint32_t constraint = 0, operands[] = {0,1,2};
    XrXirInstruction body[6]; memcpy(body, functions[1].instructions, 2 * sizeof(*body));
    body[2] = (XrXirInstruction) {XR_XIR_CALL, outer, {2,1}, {0,1}, 4};
    body[3] = (XrXirInstruction) {XR_XIR_STRUCT_GET, (XrXirType)256, {3}, {0}, 0};
    body[4] = (XrXirInstruction) {XR_XIR_STRUCT_GET, t, {4}, {0}, 0};
    body[5] = (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {5}, {0}, 0};
    XrXirBlock block = {0,6}, identity_block = {0,2};
    functions[1].instructions = body; functions[1].instruction_count = 6;
    functions[1].blocks = &block; functions[1].operands = operands; functions[1].operand_count = 3;
    generics[1].arguments = &outer; generics[1].argument_count = 1;
    XrXirInstruction identity[] = {{XR_XIR_COPY,t,{0},{0},0}, {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0}};
    functions[4] = (XrXirFunction) {"identity",8,&t,1,t,&identity_block,1,identity,2,NULL,0};
    generics[4] = (XrXirGeneric) {&constraint,1,NULL,0};
    built.functions = functions; built.function_count = 5; built.generics = generics; built.declarations = &declarations;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(base);
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    CHECK(closed->module.function_count == 9 && closed->module.types->count == 8);
    CHECK(closed->module.provenance && closed->module.provenance->count == 9);
    xr_xir_artifact_free(checked);
    CHECK(xr_xir_artifact_verify(closed, NULL, NULL) == XR_XIR_OK);
    return closed;
}
static inline XrXirArtifact *nominal_expression_lowered(void) {
    XrXirArtifact *closed = nominal_forwarding_checked(), *decoded = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(closed, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(decoded, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    CHECK(lowered->module.types->count == 6 && lowered->module.provenance->source->module.types->count == 2);
    for (uint32_t i = 0; i < 3; ++i) {
        XrXirType expected = (XrXirType)(257 + 2 * i);
        CHECK(lowered->module.provenance->origins[6+i].arguments[0] == expected);
        CHECK(lowered->module.functions[6+i].parameters[0] == expected);
    }
    CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) == XR_XIR_OK);
    return lowered;
}
#endif // XIR_NOMINAL_EXPRESSION_FIXTURE_H
