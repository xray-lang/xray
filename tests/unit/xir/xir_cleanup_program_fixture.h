/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cleanup_program_fixture.h - Nested lexical exits with observable captures
 */
#ifndef XIR_CLEANUP_PROGRAM_FIXTURE_H
#define XIR_CLEANUP_PROGRAM_FIXTURE_H
static XrXirArtifact *cleanup_program_fixture(unsigned mode) {
    XrXirInstruction done = {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    XrXirInstruction ops[] = {
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 10, {0}},
        {XR_XIR_CLEANUP_REGISTER, XR_XIR_UNIT, {0, 1}, {1}, 2, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 20, {0}},
        {XR_XIR_CLEANUP_REGISTER, XR_XIR_UNIT, {1, 1}, {2}, 2, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}},
        {XR_XIR_COPY, XR_XIR_I64, {0}, {0}, 0, {0}},
        {XR_XIR_CLEANUP_LEAVE, XR_XIR_UNIT, {0}, {3}, 2, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 99, {0}},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {7}, {0}, 1, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}},
        {XR_XIR_PANIC_CATCH, XR_XIR_UNIT, {0}, {0}, 0, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 99, {0}},
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {11}, {0}, 1, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirInstruction cleanup[] = {
        {XR_XIR_OUTPUT, XR_XIR_UNIT, {0}, {0}, 1, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    XrXirBlock one = {0, 1, 0, 0}, two = {0, 2, 0, 0};
    XrXirBlock blocks[] = {{0, 2, 0, 0}, {2, 2, 0, 2}, {4, 3, 0, 4}, {7, 3, 0, 2}, {10, 4, 0, 0}};
    uint32_t operands[] = {0, 2}; XrXirType capture = XR_XIR_I64;
    XrXirFunction functions[] = {
        {"init", 4, NULL, 0, XR_XIR_UNIT, &one, 1, &done, 1, NULL, 0},
        {"main", 4, NULL, 0, XR_XIR_I64, blocks, 4, ops, 10, operands, 2},
        {"cleanup", 7, &capture, 1, XR_XIR_UNIT, &two, 1, cleanup, 2, NULL, 0}};
    if (mode == 1) ops[5] = (XrXirInstruction){XR_XIR_SUSPEND, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    if (mode == 2 || mode == 3 || mode == 5) {
        functions[1].block_count = 5; functions[1].instruction_count = 14;
        blocks[2].panic = 4; blocks[4].frontier = mode == 3 ? 2u : 0u;
        ops[5] = (XrXirInstruction){XR_XIR_DIV_INT, XR_XIR_I64, {0, 4}, {0}, 0, {0}};
    }
    if (mode == 5) {
        ops[0].immediate = 0; blocks[1].panic = 4;
        ops[2] = (XrXirInstruction){XR_XIR_DIV_INT, XR_XIR_I64, {0, 0}, {0}, 0, {0}};
    }
    if (mode == 4) {
        functions[1].block_count = 3; functions[1].instruction_count = 7;
        ops[6] = done;
    }
    XrXirFunctionIdentity identities[3] = {{0}}; identities[2].cleanup_owner = 2;
    XrXirSourceModule source = {"root", 4, NULL, 0, 0};
    XrXirDeclarations declarations = {&source, 1, identities, NULL, 0, NULL, 0, 0, 1, NULL};
    XrXirModule module = {XR_XIR_BUILT, functions, 3, &declarations, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_check(&module, NULL, &checked, NULL) == XR_XIR_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(checked, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    return lowered;
}
#endif // XIR_CLEANUP_PROGRAM_FIXTURE_H
