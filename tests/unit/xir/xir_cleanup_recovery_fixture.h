/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cleanup_recovery_fixture.h - Actual registration and lexical retirement
 */
#ifndef XIR_CLEANUP_RECOVERY_FIXTURE_H
#define XIR_CLEANUP_RECOVERY_FIXTURE_H
#include "xir/xxir.h"
typedef struct CleanupRecoveryFixture {
    XrXirInstruction initializer, root[9], cleanup[3];
    XrXirBlock init_block, root_blocks[6], cleanup_block;
    XrXirFunction functions[3];
    XrXirFunctionIdentity identities[3];
    XrXirType capture;
    uint32_t operands[2];
    XrXirSourceModule source;
    XrXirDeclarations declarations;
    XrXirModule module;
} CleanupRecoveryFixture;
static void cleanup_recovery_fixture(CleanupRecoveryFixture *f, bool cold, bool nested) {
    *f = (CleanupRecoveryFixture){0};
    f->capture = XR_XIR_I64;
    f->initializer = (XrXirInstruction){.op = XR_XIR_RETURN};
    f->init_block = (XrXirBlock){.count = 1};
    f->root[0] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7};
    f->root[1] = (XrXirInstruction){.op = XR_XIR_CONST_BOOL, .type = XR_XIR_BOOL, .immediate = !cold};
    f->root[2] = (XrXirInstruction){.op = XR_XIR_BRANCH, .args = {1}, .targets = {1, 4}};
    f->root[3] = (XrXirInstruction){.op = XR_XIR_CLEANUP_REGISTER, .args = {0, 1}, .targets = {2}, .immediate = 2};
    f->root[4] = (XrXirInstruction){.op = XR_XIR_OUTPUT, .args = {0}, .immediate = 1};
    f->root[5] = (XrXirInstruction){.op = XR_XIR_CLEANUP_LEAVE, .targets = {3}};
    f->root[6] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {0}};
    f->root[7] = (XrXirInstruction){.op = XR_XIR_OUTPUT, .args = {0}, .immediate = 1};
    f->root[8] = (XrXirInstruction){.op = XR_XIR_JUMP, .targets = {3}};
    f->root_blocks[0] = (XrXirBlock){.count = 3};
    f->root_blocks[1] = (XrXirBlock){.first = 3, .count = 1};
    f->root_blocks[2] = (XrXirBlock){.first = 4, .count = 2, .frontier = 4};
    f->root_blocks[3] = (XrXirBlock){.first = 6, .count = 1};
    f->root_blocks[4] = (XrXirBlock){.first = 7, .count = 2};
    f->cleanup[0] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 41};
    f->cleanup[1] = (XrXirInstruction){.op = XR_XIR_OUTPUT, .args = {1}, .immediate = 1};
    f->cleanup[2] = (XrXirInstruction){.op = XR_XIR_RETURN};
    f->cleanup_block = (XrXirBlock){.count = 3};
    f->functions[0] = (XrXirFunction){.name = "init", .name_length = 4,
        .blocks = &f->init_block, .block_count = 1, .instructions = &f->initializer, .instruction_count = 1};
    f->functions[1] = (XrXirFunction){.name = "root", .name_length = 4, .result = XR_XIR_I64,
        .blocks = f->root_blocks, .block_count = 5, .instructions = f->root, .instruction_count = 9,
        .operands = f->operands, .operand_count = 1};
    f->functions[2] = (XrXirFunction){.name = "cleanup", .name_length = 7,
        .parameters = &f->capture, .parameter_count = 1, .blocks = &f->cleanup_block,
        .block_count = 1, .instructions = f->cleanup, .instruction_count = 3};
    if (nested) {
        f->root[2].targets[1] = 5;
        f->root[4] = (XrXirInstruction){.op = XR_XIR_CLEANUP_REGISTER,
            .args = {1, 1}, .targets = {3}, .immediate = 2};
        f->root[5] = (XrXirInstruction){.op = XR_XIR_OUTPUT, .args = {0}, .immediate = 1};
        f->root[6] = (XrXirInstruction){.op = XR_XIR_CLEANUP_LEAVE, .targets = {4}, .immediate = 4};
        f->root[7] = (XrXirInstruction){.op = XR_XIR_CLEANUP_LEAVE, .targets = {5}};
        f->root[8] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {0}};
        f->root_blocks[2] = (XrXirBlock){.first = 4, .count = 1, .frontier = 4};
        f->root_blocks[3] = (XrXirBlock){.first = 5, .count = 2, .frontier = 5};
        f->root_blocks[4] = (XrXirBlock){.first = 7, .count = 1, .frontier = 4};
        f->root_blocks[5] = (XrXirBlock){.first = 8, .count = 1};
        f->functions[1].instruction_count = 9;
        f->functions[1].block_count = 6;
        f->functions[1].operand_count = 2;
    }
    f->identities[1].exported = 1;
    f->identities[2].cleanup_owner = 2;
    f->source = (XrXirSourceModule){"root", 4, NULL, 0, 0};
    f->declarations = (XrXirDeclarations){.modules = &f->source, .module_count = 1,
        .functions = f->identities, .root_module = 0, .entry_function = 1};
    f->module = (XrXirModule){.stage = XR_XIR_BUILT, .functions = f->functions, .function_count = 3,
        .declarations = &f->declarations};
}
#endif // XIR_CLEANUP_RECOVERY_FIXTURE_H
