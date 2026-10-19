/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_task_fault_cleanup_fixture.h - Authentic child and caller cleanup paths
 */
#ifndef XIR_TASK_FAULT_CLEANUP_FIXTURE_H
#define XIR_TASK_FAULT_CLEANUP_FIXTURE_H
#include "xir/xxir.h"
#include "xir/xxir_declarations.h"
enum { TASK_FAULT_BODY, TASK_FAULT_CALL, TASK_FAULT_AWAIT, TASK_FAULT_CASES };
typedef struct TaskFaultFixture {
    XrXirTypeNode node;
    XrXirTypes types;
    XrXirType parameter;
    XrXirInstruction root[8], worker[3], root_cleanup[3], child_cleanup[3], init;
    XrXirBlock root_blocks[4], worker_blocks[2], cleanup_block, init_block;
    uint32_t operand;
    XrXirFunction functions[5];
    XrXirFunctionIdentity identities[5];
    XrXirSourceModule source;
    XrXirDeclarations declarations;
    XrXirModule module;
} TaskFaultFixture;
static void task_fault_fixture(TaskFaultFixture *f, unsigned mode) {
    *f = (TaskFaultFixture){0};
    f->parameter = XR_XIR_I64;
    f->node = (XrXirTypeNode){.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_I64};
    f->types = (XrXirTypes){&f->node, 1, NULL, NULL};
    f->root[0] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7};
    f->root_blocks[0] = (XrXirBlock){.count = mode == TASK_FAULT_BODY ? 3 : 2};
    f->functions[0] = (XrXirFunction){.name = "root", .name_length = 4, .result = XR_XIR_I64,
        .blocks = f->root_blocks, .block_count = 1, .instructions = f->root,
        .instruction_count = 3, .operands = &f->operand, .operand_count = 1};
    if (mode == TASK_FAULT_BODY) {
        f->root[1] = (XrXirInstruction){.op = XR_XIR_GO, .type = (XrXirType)256,
            .args = {0, 1}, .immediate = 1};
        f->root[2] = (XrXirInstruction){.op = XR_XIR_RETURN};
    } else {
        f->root[1] = (XrXirInstruction){.op = XR_XIR_CLEANUP_REGISTER, .targets = {1}, .immediate = 3};
        f->root_blocks[1] = (XrXirBlock){.first = 2, .count = 2, .frontier = 2};
        f->functions[0].block_count = 2;
        f->functions[0].instruction_count = 4;
        if (mode == TASK_FAULT_CALL) {
            f->root[2] = (XrXirInstruction){.op = XR_XIR_CALL, .type = XR_XIR_I64,
                .args = {0, 1}, .immediate = 1};
            f->root[3] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {2}};
        } else {
            f->root[2] = (XrXirInstruction){.op = XR_XIR_GO, .type = (XrXirType)256,
                .args = {0, 1}, .immediate = 1};
            f->root[3] = (XrXirInstruction){.op = XR_XIR_TASK_AWAIT, .args = {2}, .targets = {2, 3}};
            f->root[4] = (XrXirInstruction){.op = XR_XIR_INVOKE_RESULT, .type = XR_XIR_I64, .immediate = 3};
            f->root[5] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {4}};
            f->root[6] = (XrXirInstruction){.op = XR_XIR_INVOKE_ERROR, .type = XR_XIR_ERROR, .immediate = 3};
            f->root[7] = (XrXirInstruction){.op = XR_XIR_THROW, .args = {6}};
            f->root_blocks[2] = (XrXirBlock){.first = 4, .count = 2, .frontier = 2};
            f->root_blocks[3] = (XrXirBlock){.first = 6, .count = 2, .frontier = 2};
            f->functions[0].block_count = 4;
            f->functions[0].instruction_count = 8;
        }
    }
    f->worker[0] = (XrXirInstruction){.op = XR_XIR_CLEANUP_REGISTER, .targets = {1}, .immediate = 4};
    f->worker[1] = (XrXirInstruction){.op = XR_XIR_PRINT, .args = {0, 1}};
    f->worker[2] = (XrXirInstruction){.op = XR_XIR_RETURN};
    f->worker_blocks[0] = (XrXirBlock){.count = 1};
    f->worker_blocks[1] = (XrXirBlock){.first = 1, .count = 2, .frontier = 1};
    f->functions[1] = (XrXirFunction){.name = "worker", .name_length = 6,
        .parameters = &f->parameter, .parameter_count = 1, .result = XR_XIR_I64,
        .blocks = f->worker_blocks, .block_count = 2, .instructions = f->worker,
        .instruction_count = 3, .operands = &f->operand, .operand_count = 1};
    f->init = (XrXirInstruction){.op = XR_XIR_RETURN};
    f->init_block = (XrXirBlock){.count = 1};
    f->functions[2] = (XrXirFunction){.name = "init", .name_length = 4, .blocks = &f->init_block,
        .block_count = 1, .instructions = &f->init, .instruction_count = 1};
    f->cleanup_block = (XrXirBlock){.count = 3};
    f->root_cleanup[0] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 5};
    f->root_cleanup[1] = (XrXirInstruction){.op = XR_XIR_PRINT, .args = {0, 1}};
    f->root_cleanup[2] = (XrXirInstruction){.op = XR_XIR_RETURN};
    f->child_cleanup[0] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 6};
    f->child_cleanup[1] = (XrXirInstruction){.op = XR_XIR_PRINT, .args = {0, 1}};
    f->child_cleanup[2] = (XrXirInstruction){.op = XR_XIR_RETURN};
    f->functions[3] = (XrXirFunction){.name = "root_cleanup", .name_length = 12,
        .blocks = &f->cleanup_block, .block_count = 1, .instructions = f->root_cleanup,
        .instruction_count = 3, .operands = &f->operand, .operand_count = 1};
    f->functions[4] = (XrXirFunction){.name = "child_cleanup", .name_length = 13,
        .blocks = &f->cleanup_block, .block_count = 1, .instructions = f->child_cleanup,
        .instruction_count = 3, .operands = &f->operand, .operand_count = 1};
    f->identities[0].exported = 1;
    f->identities[3].cleanup_owner = 1;
    f->identities[4].cleanup_owner = 2;
    f->source = (XrXirSourceModule){"fault-cleanup", 13, NULL, 0, 2};
    f->declarations = (XrXirDeclarations){.modules = &f->source, .module_count = 1,
        .functions = f->identities, .root_module = 0, .entry_function = 0};
    f->module = (XrXirModule){.stage = XR_XIR_BUILT, .functions = f->functions,
        .function_count = 5, .declarations = &f->declarations,
        .types = mode == TASK_FAULT_CALL ? NULL : &f->types};
}
#endif // XIR_TASK_FAULT_CLEANUP_FIXTURE_H
