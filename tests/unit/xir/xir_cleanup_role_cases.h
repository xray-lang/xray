/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cleanup_role_cases.h - Cleanup identity and definition-time rejection
 */
#ifndef XIR_CLEANUP_ROLE_CASES_H
#define XIR_CLEANUP_ROLE_CASES_H
#include "xir_cleanup_role_fixture.h"
static void cleanup_role_rejections(XrXirArtifact *checked) {
    XrXirModule *module = &checked->module;
    XrXirFunctionIdentity *ids = (XrXirFunctionIdentity *)module->declarations->functions;
    const uint32_t owners[] = {0, 4, 5, UINT32_MAX};
    for (uint32_t i = 1; i < 4; ++i) {
        ids[3].cleanup_owner = owners[i];
        CHECK(xr_xir_verify(module, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    }
    ids[3].cleanup_owner = 3;
    ids[3].exported = 1; CHECK(xr_xir_verify(module, NULL, NULL) == XR_XIR_BAD_STRUCTURE); ids[3].exported = 0;
    XrXirInstruction *call = (XrXirInstruction *)module->functions[1].instructions;
    call[0].immediate = 3; CHECK(xr_xir_verify(module, NULL, NULL) == XR_XIR_BAD_STRUCTURE); call[0].immediate = 2;
    XrXirInstruction saved_call = call[0];
    XrXirTypeNode signature = {0}; signature.kind = XR_XIR_TYPE_CALLABLE; signature.result = XR_XIR_UNIT;
    XrXirTypes types = {&signature, 1, NULL}; module->types = &types;
    call[0].op = XR_XIR_FUNCTION_REF; call[0].type = (XrXirType)256;
    CHECK(xr_xir_verify(module, NULL, NULL) == XR_XIR_OK);
    call[0].immediate = 3; CHECK(xr_xir_verify(module, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    module->types = NULL; call[0] = saved_call;
    XrXirGeneric *generics = (XrXirGeneric *)module->generics;
    XrXirType *argument = (XrXirType *)generics[2].arguments;
    *argument = XR_XIR_I64;
    CHECK(xr_xir_verify(module, NULL, NULL) == XR_XIR_BAD_TYPE);
    *argument = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    uint32_t *constraint = (uint32_t *)generics[3].constraints;
    *constraint = XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(xr_xir_verify(module, NULL, NULL) == XR_XIR_BAD_TYPE); *constraint = 0;
    XrXirFunction *body = (XrXirFunction *)&module->functions[3], saved = *body;
    XrXirInstruction suspend[] = {{XR_XIR_SUSPEND, XR_XIR_UNIT, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirBlock block = {0, 2, 0, 0}; body->blocks = &block; body->block_count = 1;
    body->instructions = suspend; body->instruction_count = 2;
    XrXirGeneric saved_generic = generics[3]; generics[3].arguments = NULL; generics[3].argument_count = 0;
    XrXirDiagnostic diagnostic = {0};
    CHECK(xr_xir_verify(module, NULL, &diagnostic) == XR_XIR_BAD_TYPE && diagnostic.function == 3);
    CHECK(diagnostic.reason == XR_XIR_DIAGNOSTIC_CLEANUP_SUSPEND);
    CHECK(diagnostic.block == 0 && diagnostic.instruction == 0);
    XrXirType error = XR_XIR_ERROR;
    XrXirInstruction escaping = {XR_XIR_THROW, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    block.count = 1; body->instructions = &escaping; body->instruction_count = 1; body->parameters = &error; body->parameter_count = 1;
    XrXirFunction *owner = (XrXirFunction *)&module->functions[2], saved_owner = *owner;
    XrXirGeneric saved_owner_generic = generics[2];
    owner->blocks = &block; owner->block_count = 1; owner->instructions = &suspend[1]; owner->instruction_count = 1;
    generics[2].arguments = NULL; generics[2].argument_count = 0;
    CHECK(xr_xir_verify(module, NULL, &diagnostic) == XR_XIR_BAD_TYPE && diagnostic.function == 3);
    CHECK(diagnostic.reason == XR_XIR_DIAGNOSTIC_CLEANUP_THROW);
    *owner = saved_owner; generics[2] = saved_owner_generic;
    *body = saved;
    generics[3] = saved_generic;
    CHECK(xr_xir_verify(module, NULL, NULL) == XR_XIR_OK);
}
static void cleanup_role_cases(void) {
    XrXirArtifact *checked = cleanup_role_fixture(), *decoded = NULL, *closed = NULL, *lowered = NULL;
    cleanup_role_rejections(checked);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    uint8_t identities[100] = {0}; put32(identities + 3 * 20 + 16, 3); put32(identities + 4 * 20 + 16, 4);
    size_t at = 0; uint32_t matches = 0;
    for (size_t i = 64; i + sizeof(identities) <= packet.length; ++i)
        if (!memcmp(packet.bytes + i, identities, sizeof(identities))) { at = i; ++matches; }
    CHECK(matches == 1);
    put32(packet.bytes + 8, 12); digest_packet(&packet); rejected(packet.bytes, packet.length);
    put32(packet.bytes + 8, 13); put32(packet.bytes + 12, 34); digest_packet(&packet); rejected(packet.bytes, packet.length);
    put32(packet.bytes + 12, 35); digest_packet(&packet); rejected(packet.bytes, packet.length);
    put32(packet.bytes + 12, 36); digest_packet(&packet); rejected(packet.bytes, packet.length);
    put32(packet.bytes + 12, 37); digest_packet(&packet); rejected(packet.bytes, packet.length);
    put32(packet.bytes + 12, 38); digest_packet(&packet); rejected(packet.bytes, packet.length);
    put32(packet.bytes + 12, 39); digest_packet(&packet);
    put32(packet.bytes + at + 3 * 20 + 16, 4); digest_packet(&packet); rejected(packet.bytes, packet.length);
    put32(packet.bytes + at + 3 * 20 + 16, 3); digest_packet(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked); xr_xir_checked_packet_free(&packet);
    CHECK(decoded->module.declarations->functions[4].cleanup_owner == 4);
    CHECK(xr_xir_specialize(decoded, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    CHECK(closed->module.function_count == 8 && closed->module.declarations->functions[4].cleanup_owner == 3 &&
        closed->module.declarations->functions[5].cleanup_owner == 4 &&
        closed->module.declarations->functions[6].cleanup_owner == 5 &&
        closed->module.declarations->functions[7].cleanup_owner == 6);
    CHECK(closed->module.functions[2].instructions[0].immediate == 4 &&
        closed->module.functions[3].instructions[0].immediate == 5 &&
        closed->module.functions[4].instructions[0].immediate == 6 &&
        closed->module.functions[5].instructions[0].immediate == 7);
    CHECK(closed->module.functions[2].blocks[1].frontier == 1 &&
        closed->module.functions[5].blocks[1].frontier == 1);
    XrXirFunctionIdentity *ids = (XrXirFunctionIdentity *)closed->module.declarations->functions;
    ids[4].cleanup_owner = 0;
    CHECK(xr_xir_verify(&closed->module, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    ids[4].cleanup_owner = 4;
    CHECK(xr_xir_verify(&closed->module, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    ids[4].cleanup_owner = 3;
    XrXirProvenance *proof = (XrXirProvenance *)closed->module.provenance;
    --closed->module.function_count; --proof->count;
    CHECK(xr_xir_verify(&closed->module, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    ++closed->module.function_count; ++proof->count;
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) == XR_XIR_OK);
    CHECK(lowered->module.declarations->functions[7].cleanup_owner == 6);
    xr_xir_artifact_free(lowered);
    puts("Cleanup body roles, definition effects, packets and instance provenance passed");
}
#endif // XIR_CLEANUP_ROLE_CASES_H
