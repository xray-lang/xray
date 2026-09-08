/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_coroutine_branch_fixture.h - Coroutine parallel edge assignment
 *
 * KEY CONCEPT:
 *   A self edge swaps two block arguments before exiting. Subtracting both
 *   values distinguishes a simultaneous transfer from an overwritten source.
 */

#ifndef XR_PROGRAM_COROUTINE_BRANCH_FIXTURE_H
#define XR_PROGRAM_COROUTINE_BRANCH_FIXTURE_H

#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"

#include <string.h>

static XrCoreIrKey xr_program_coroutine_branch_fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrProgramBuildStatus xr_program_coroutine_branch_fixture_encode(
    const XrCoreIrBlockInput *blocks, const XrCoreIrConstantInput *constants,
    const XrCoreIrKey *live_values, XrProgramArtifact *artifact, char *diagnostic,
    size_t diagnostic_size) {
    XrCoreIrCoroutineStateInput states[] = {
        {.state_id = 0u, .continuation_block = blocks[0].key},
        {.state_id = 1u, .continuation_block = blocks[1].key},
    };
    XrCoreIrCoroutineSafepointInput safepoint = {
        .safepoint_id = 0u,
        .resume_state_id = 1u,
        .live_values = live_values,
        .live_value_count = 3u,
    };
    XrCoreIrFunctionInput function = {
        .key = xr_program_coroutine_branch_fixture_key("coro-branch:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = XR_CORE_EFFECT_SUSPEND | XR_CORE_EFFECT_CANCEL | XR_CORE_EFFECT_TRAP,
        .capability_mask = XR_CORE_CAPABILITY_RUNTIME_COOPERATIVE_YIELD,
        .entry_block = blocks[0].key,
        .blocks = blocks,
        .block_count = 4u,
        .coroutine_states = states,
        .coroutine_state_count = 2u,
        .coroutine_safepoints = &safepoint,
        .coroutine_safepoint_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = xr_program_coroutine_branch_fixture_key("coro-branch:module"),
        .constants = constants,
        .constant_count = 4u,
        .functions = &function,
        .function_count = 1u,
    };
    uint8_t profile[XR_PROGRAM_DIGEST_SIZE] = {0};
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = profile,
        .required_features = &feature,
        .required_feature_count = 1u,
        .modules = &module,
        .module_count = 1u,
    };
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&input, &program, diagnostic, diagnostic_size);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, diagnostic_size);
    xr_core_ir_program_free(program);
    return status;
}

/* The first step suspends after four instructions. Resume visits the loop
 * twice and returns 22 - 11 after thirteen total instructions; cancellation
 * publishes its separate terminal after five. Sequential self-edge writes
 * instead overwrite both i64 arguments with 22 and incorrectly return zero. */
static XrProgramBuildStatus xr_program_coroutine_branch_fixture_write(XrProgramArtifact *artifact,
                                                                      char *diagnostic,
                                                                      size_t diagnostic_size) {
    if (!artifact)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    XrCoreIrConstantInput constants[] = {
        {.key = xr_program_coroutine_branch_fixture_key("coro-branch:constant:eleven"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 11},
        {.key = xr_program_coroutine_branch_fixture_key("coro-branch:constant:twenty-two"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 22},
        {.key = xr_program_coroutine_branch_fixture_key("coro-branch:constant:true"),
         .type_id = XR_CORE_TYPE_BOOL,
         .kind = XR_CORE_IR_CONSTANT_BOOL,
         .value.boolean = true},
        {.key = xr_program_coroutine_branch_fixture_key("coro-branch:constant:false"),
         .type_id = XR_CORE_TYPE_BOOL,
         .kind = XR_CORE_IR_CONSTANT_BOOL,
         .value.boolean = false},
    };
    XrCoreIrKey entry = xr_program_coroutine_branch_fixture_key("coro-branch:entry");
    XrCoreIrKey loop = xr_program_coroutine_branch_fixture_key("coro-branch:loop");
    XrCoreIrKey exit = xr_program_coroutine_branch_fixture_key("coro-branch:exit");
    XrCoreIrKey cancel = xr_program_coroutine_branch_fixture_key("coro-branch:cancel");
    XrCoreIrKey eleven = xr_program_coroutine_branch_fixture_key("coro-branch:value:eleven");
    XrCoreIrKey twenty_two =
        xr_program_coroutine_branch_fixture_key("coro-branch:value:twenty-two");
    XrCoreIrKey first = xr_program_coroutine_branch_fixture_key("coro-branch:value:first");
    XrCoreIrKey left = xr_program_coroutine_branch_fixture_key("coro-branch:value:left");
    XrCoreIrKey right = xr_program_coroutine_branch_fixture_key("coro-branch:value:right");
    XrCoreIrKey again = xr_program_coroutine_branch_fixture_key("coro-branch:value:again");
    XrCoreIrKey stop = xr_program_coroutine_branch_fixture_key("coro-branch:value:stop");
    XrCoreIrKey final_left =
        xr_program_coroutine_branch_fixture_key("coro-branch:value:final-left");
    XrCoreIrKey final_right =
        xr_program_coroutine_branch_fixture_key("coro-branch:value:final-right");
    XrCoreIrKey difference =
        xr_program_coroutine_branch_fixture_key("coro-branch:value:difference");
    /* Writer live IDs follow definition order, not the key's lexical order. */
    XrCoreIrKey live_values[] = {eleven, twenty_two, first};
    XrCoreIrKey yield_successors[] = {loop, cancel};
    XrCoreIrInstructionInput entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = eleven,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = twenty_two,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_BOOL,
         .result = first,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[2].key},
        {.operation_id = XR_CORE_OP_CORE_COROUTINE_YIELD,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = live_values,
         .operand_count = 3u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0u,
         .successors = yield_successors,
         .successor_count = 2u},
    };
    XrCoreIrValueInput loop_arguments[] = {
        {.key = left, .type_id = XR_CORE_TYPE_I64},
        {.key = right, .type_id = XR_CORE_TYPE_I64},
        {.key = again, .type_id = XR_CORE_TYPE_BOOL},
    };
    XrCoreIrKey loop_argument_keys[] = {left, right, again};
    XrCoreIrKey conditional_operands[] = {again, right, left, stop, left, right};
    XrCoreIrKey conditional_successors[] = {loop, exit};
    XrCoreIrInstructionInput loop_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = loop_argument_keys,
         .operand_count = 3u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_BOOL,
         .result = stop,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[3].key},
        {.operation_id = XR_CORE_OP_CORE_CONDITIONAL_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = conditional_operands,
         .operand_count = 6u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = conditional_successors,
         .successor_count = 2u},
    };
    XrCoreIrValueInput exit_arguments[] = {
        {.key = final_left, .type_id = XR_CORE_TYPE_I64},
        {.key = final_right, .type_id = XR_CORE_TYPE_I64},
    };
    XrCoreIrKey exit_argument_keys[] = {final_left, final_right};
    XrCoreIrKey return_operand[] = {difference};
    XrCoreIrInstructionInput exit_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = exit_argument_keys,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_SUB_I64,
         .result = difference,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = exit_argument_keys,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = return_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrInstructionInput cancel_instruction = {
        .operation_id = XR_CORE_OP_CORE_CANCEL_PUBLISH,
        .result_type_id = XR_CORE_TYPE_VOID,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    XrCoreIrBlockInput blocks[] = {
        {.key = entry, .instructions = entry_instructions, .instruction_count = 4u},
        {.key = loop,
         .arguments = loop_arguments,
         .argument_count = 3u,
         .instructions = loop_instructions,
         .instruction_count = 3u},
        {.key = exit,
         .arguments = exit_arguments,
         .argument_count = 2u,
         .instructions = exit_instructions,
         .instruction_count = 3u},
        {.key = cancel, .instructions = &cancel_instruction, .instruction_count = 1u},
    };
    return xr_program_coroutine_branch_fixture_encode(blocks, constants, live_values, artifact,
                                                      diagnostic, diagnostic_size);
}

#endif  // XR_PROGRAM_COROUTINE_BRANCH_FIXTURE_H
