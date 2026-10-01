/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cleanup_frontier_cases.h - Lexical registration and exit admission
 */
#ifndef XIR_CLEANUP_FRONTIER_CASES_H
#define XIR_CLEANUP_FRONTIER_CASES_H
static XrXirArtifact *cleanup_frontier_fixture(unsigned attack) {
    XrXirInstruction done = {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    XrXirInstruction ops[] = {
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 17, {0}},
        {XR_XIR_CLEANUP_REGISTER, XR_XIR_UNIT, {0, 1}, {1}, 2, {0}},
        {XR_XIR_CLEANUP_REGISTER, XR_XIR_UNIT, {1, 1}, {2}, 2, {0}},
        {XR_XIR_CLEANUP_LEAVE, XR_XIR_UNIT, {0}, {3}, 2, {0}},
        {XR_XIR_CLEANUP_LEAVE, XR_XIR_UNIT, {0}, {4}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirBlock one = {0, 1, 0, 0};
    XrXirBlock blocks[] = {{0, 2, 0, 0}, {2, 1, 0, 2}, {3, 1, 0, 3}, {4, 1, 0, 2}, {5, 1, 0, 0}};
    uint32_t operands[] = {0, 0};
    XrXirType capture = XR_XIR_I64;
    XrXirFunction functions[] = {
        {"init", 4, NULL, 0, XR_XIR_UNIT, &one, 1, &done, 1, NULL, 0},
        {"main", 4, NULL, 0, XR_XIR_I64, blocks, 5, ops, 6, operands, 2},
        {"cleanup", 7, &capture, 1, XR_XIR_UNIT, &one, 1, &done, 1, NULL, 0}};
    XrXirFunctionIdentity identities[3] = {{0}};
    identities[2].cleanup_owner = 2;
    XrXirSourceModule source = {"root", 4, NULL, 0, 0};
    XrXirDeclarations declarations = {&source, 1, identities, NULL, 0, NULL, 0, 0, 1, NULL};
    XrXirModule module = {XR_XIR_BUILT, functions, 3, &declarations, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    if (attack == 1) blocks[0].frontier = 2;
    if (attack == 2) blocks[1].frontier = UINT32_MAX;
    if (attack == 3) blocks[1].frontier = 1;
    if (attack == 4) blocks[2].frontier = 2;
    if (attack == 5) blocks[3].frontier = 3;
    if (attack == 6) ops[4].immediate = 3;
    if (attack == 7) ops[3].op = XR_XIR_JUMP, ops[3].immediate = 0;
    if (attack == 8) operands[1] = 1;
    if (attack == 9) operands[0] = 5;
    if (attack == 10) identities[2].cleanup_owner = 1;
    if (attack == 11) ops[1].immediate = 1;
    if (attack == 12) capture = XR_XIR_U8;
    if (attack == 13) ops[3].immediate = -1;
    if (attack == 14) ops[2].targets[0] = 1;
    if (attack == 15) ops[1].targets[0] = 3;
    XrXirArtifact *checked = NULL;
    XrXirStatus status = xr_xir_check(&module, NULL, &checked, NULL);
    CHECK(attack ? status != XR_XIR_OK && !checked : status == XR_XIR_OK);
    return checked;
}
static void cleanup_frontier_cases(void) {
    for (unsigned attack = 1; attack <= 15; ++attack) cleanup_frontier_fixture(attack);
    XrXirArtifact *checked = cleanup_frontier_fixture(0), *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    uint8_t encoded[80] = {0};
    const uint32_t fields[] = {0, 2, 0, 0, 2, 1, 0, 2, 3, 1, 0, 3, 4, 1, 0, 2, 5, 1, 0, 0};
    for (unsigned i = 0; i < 20; ++i) put32(encoded + i * 4, fields[i]);
    size_t at = 0; unsigned matches = 0;
    for (size_t i = 64; i + sizeof(encoded) <= packet.length; ++i)
        if (!memcmp(packet.bytes + i, encoded, sizeof(encoded))) { at = i; ++matches; }
    CHECK(matches == 1);
    for (unsigned b = 0; b < 5; ++b) {
        put32(packet.bytes + at + b * 16 + 12, 6); digest_packet(&packet);
        rejected(packet.bytes, packet.length);
        put32(packet.bytes + at + b * 16 + 12, fields[b * 4 + 3]);
    }
    digest_packet(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(decoded, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    CHECK(lowered->module.functions[1].blocks[2].frontier == 3);
    CHECK(lowered->module.functions[1].instructions[2].immediate == 2);
    CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(lowered);
    puts("Cleanup registration, lexical exits, hostile packets and lifetime passed");
}
static void cleanup_error_frontier(void) {
    XrXirArtifact *base = cleanup_frontier_fixture(0), *checked = NULL;
    XrXirFunction functions[4];
    memcpy(functions, base->module.functions, 3 * sizeof(*functions));
    XrXirType error = XR_XIR_ERROR;
    XrXirInstruction ops[] = {
        {XR_XIR_CLEANUP_REGISTER, XR_XIR_UNIT, {0}, {1}, 3, {0}},
        {XR_XIR_CLEANUP_ERROR, XR_XIR_UNIT, {0}, {2}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirBlock blocks[] = {{0, 1, 0, 0}, {1, 1, 0, 1}, {2, 1, 0, 0}};
    XrXirInstruction main_ops[] = {
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirBlock main_block = {0, 2, 0, 0};
    functions[1] = (XrXirFunction){"main", 4, NULL, 0, XR_XIR_I64, &main_block, 1, main_ops, 2, NULL, 0};
    functions[3] = functions[2]; functions[3].parameter_count = 0; functions[3].parameters = NULL;
    functions[2] = (XrXirFunction){"owner", 5, &error, 1, XR_XIR_UNIT, blocks, 3, ops, 3, NULL, 0};
    XrXirFunctionIdentity ids[4] = {{0}}; ids[3].cleanup_owner = 3;
    XrXirDeclarations declarations = *base->module.declarations; declarations.functions = ids;
    XrXirModule module = base->module; module.stage = XR_XIR_BUILT; module.declarations = &declarations;
    module.functions = functions; module.function_count = 4;
    CHECK(xr_xir_check(&module, NULL, &checked, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    error = XR_XIR_I64;
    CHECK(xr_xir_check(&module, NULL, &checked, NULL) == XR_XIR_BAD_TYPE && !checked);
    xr_xir_artifact_free(base);
}
#endif // XIR_CLEANUP_FRONTIER_CASES_H
