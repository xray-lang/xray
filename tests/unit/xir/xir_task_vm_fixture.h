/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_task_vm_fixture.h - Owned Task transfer and dedicated await successors
 *
 * KEY CONCEPT:
 *   Task copies and local storage share one real carrier; the awaited result
 *   belongs to the dedicated invoke successor, including Unit and discard.
 */
#ifndef XIR_TASK_VM_FIXTURE_H
#define XIR_TASK_VM_FIXTURE_H
#include "xir/xxir.h"
#include "xir/xxir_declarations.h"
typedef struct TaskVmFixture {
    XrXirTypeNode node;
    XrXirTypes types;
    XrXirType element;
    XrXirInstruction root[10], worker[4], init;
    XrXirBlock root_blocks[3], worker_block, init_block;
    uint32_t argument;
    XrXirFunction functions[3];
    XrXirFunctionIdentity identities[3];
    XrXirSourceModule source;
    XrXirLiteral literal;
    XrXirDeclarations declarations;
    XrXirModule module;
} TaskVmFixture;
enum { TASK_VM_I64, TASK_VM_STRING, TASK_VM_UNIT, TASK_VM_DISCARD, TASK_VM_PANIC, TASK_VM_MODE_COUNT };
static void task_vm_fixture(TaskVmFixture *f, uint32_t mode) {
    *f = (TaskVmFixture){0};
    f->element = mode == TASK_VM_STRING || mode == TASK_VM_DISCARD ? XR_XIR_STRING :
        mode == TASK_VM_UNIT ? XR_XIR_UNIT : XR_XIR_I64;
    f->node = (XrXirTypeNode){.kind = XR_XIR_TYPE_TASK, .element = f->element};
    f->types = (XrXirTypes){&f->node, 1, NULL, NULL};
    f->literal = (XrXirLiteral){"a\0b", 3};
    f->root[0] = f->element == XR_XIR_STRING ?
        (XrXirInstruction){.op = XR_XIR_CONST_STRING, .type = XR_XIR_STRING} :
        (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7};
    f->root[1] = (XrXirInstruction){.op = XR_XIR_GO, .type = (XrXirType)256,
        .args = {0, f->element == XR_XIR_UNIT ? 0 : 1}, .immediate = 1};
    f->root[2] = (XrXirInstruction){.op = XR_XIR_COPY, .type = (XrXirType)256, .args = {1}};
    f->root[3] = (XrXirInstruction){.op = XR_XIR_LOCAL_NEW, .type = (XrXirType)256, .args = {2}};
    f->root[4] = (XrXirInstruction){.op = XR_XIR_LOCAL_READ, .type = (XrXirType)256, .args = {3}};
    f->root[5] = (XrXirInstruction){.op = XR_XIR_TASK_AWAIT, .args = {4}, .targets = {1, 2}};
    f->root_blocks[0] = (XrXirBlock){.count = 6};
    bool unit = mode == TASK_VM_UNIT;
    bool discard = mode == TASK_VM_DISCARD;
    uint32_t error_first = unit ? 7 : 8;
    if (unit) f->root[6] = (XrXirInstruction){.op = XR_XIR_RETURN};
    else {
        f->root[6] = discard ? (XrXirInstruction){.op = XR_XIR_INVOKE_DISCARD, .immediate = 5} :
            (XrXirInstruction){.op = XR_XIR_INVOKE_RESULT, .type = f->element, .immediate = 5};
        f->root[7] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {discard ? 0 : 6}};
    }
    f->root[error_first] = (XrXirInstruction){.op = XR_XIR_INVOKE_ERROR, .type = XR_XIR_ERROR, .immediate = 5};
    f->root[error_first + 1] = (XrXirInstruction){.op = XR_XIR_THROW, .args = {error_first}};
    f->root_blocks[1] = (XrXirBlock){.first = 6, .count = unit ? 1 : 2};
    f->root_blocks[2] = (XrXirBlock){.first = error_first, .count = 2};
    f->worker[0] = (XrXirInstruction){.op = XR_XIR_SUSPEND};
    f->worker[1] = (XrXirInstruction){.op = XR_XIR_RETURN};
    f->worker_block = (XrXirBlock){.count = 2};
    if (mode == TASK_VM_PANIC) {
        f->worker[1] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64};
        f->worker[2] = (XrXirInstruction){.op = XR_XIR_DIV_INT, .type = XR_XIR_I64, .args = {0, 2}};
        f->worker[3] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {3}};
        f->worker_block.count = 4;
    }
    f->init = (XrXirInstruction){.op = XR_XIR_RETURN};
    f->init_block = (XrXirBlock){.count = 1};
    f->functions[0] = (XrXirFunction){.name = "root", .name_length = 4,
        .result = unit || discard ? XR_XIR_UNIT : f->element,
        .blocks = f->root_blocks, .block_count = 3, .instructions = f->root,
        .instruction_count = error_first + 2, .operands = &f->argument, .operand_count = unit ? 0 : 1};
    f->functions[1] = (XrXirFunction){.name = "worker", .name_length = 6,
        .parameters = unit ? NULL : &f->element, .parameter_count = unit ? 0 : 1,
        .result = f->element, .blocks = &f->worker_block, .block_count = 1,
        .instructions = f->worker, .instruction_count = f->worker_block.count};
    f->functions[2] = (XrXirFunction){.name = "init", .name_length = 4,
        .blocks = &f->init_block, .block_count = 1, .instructions = &f->init, .instruction_count = 1};
    f->identities[0].exported = 1;
    f->source = (XrXirSourceModule){"task-vm", 7, NULL, 0, 2};
    f->declarations = (XrXirDeclarations){.modules = &f->source, .module_count = 1,
        .functions = f->identities, .literals = &f->literal, .literal_count = 1,
        .root_module = 0, .entry_function = 0};
    f->module = (XrXirModule){.stage = XR_XIR_BUILT, .functions = f->functions,
        .function_count = 3, .declarations = &f->declarations, .types = &f->types};
}
#endif // XIR_TASK_VM_FIXTURE_H
