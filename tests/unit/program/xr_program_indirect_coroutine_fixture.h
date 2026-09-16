/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_indirect_coroutine_fixture.h - Indirect suspended-call inputs
 */

#ifndef XR_PROGRAM_INDIRECT_COROUTINE_FIXTURE_H
#define XR_PROGRAM_INDIRECT_COROUTINE_FIXTURE_H

#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"

#include <string.h>

typedef enum XrProgramIndirectCoroutineMutation {
    XR_PROGRAM_INDIRECT_COROUTINE_VALID = 0,
    XR_PROGRAM_INDIRECT_COROUTINE_BAD_IMMEDIATE,
    XR_PROGRAM_INDIRECT_COROUTINE_BAD_SAFEPOINT,
    XR_PROGRAM_INDIRECT_COROUTINE_NONSUSPENDING_SIGNATURE,
    XR_PROGRAM_INDIRECT_COROUTINE_NONCALLABLE_OPERAND,
    XR_PROGRAM_INDIRECT_COROUTINE_MISSING_LIVE,
    XR_PROGRAM_INDIRECT_COROUTINE_MISSING_CANCEL_ARGUMENT,
    XR_PROGRAM_COROUTINE_SEALED_STRING_RESULT,
    XR_PROGRAM_COROUTINE_SEALED_STRING_BUDGET,
} XrProgramIndirectCoroutineMutation;

enum {
    XR_PROGRAM_INDIRECT_COROUTINE_CALLABLE = 70,
};

static XrCoreIrKey xr_program_indirect_coroutine_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrProgramBuildStatus
xr_program_indirect_coroutine_fixture_write(XrProgramIndirectCoroutineMutation mutation,
                                            XrProgramArtifact *artifact, char *diagnostic,
                                            size_t diagnostic_size) {
    XrCoreIrKey constant_key = xr_program_indirect_coroutine_key("indirect-coro:constant:21");
    XrCoreIrConstantInput constant = {
        .key = constant_key,
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 21,
    };

    uint16_t parameter_type = XR_CORE_TYPE_I64;
    XrParamMode parameter_mode = XR_PARAM_READ;
    bool nonsuspending = mutation == XR_PROGRAM_INDIRECT_COROUTINE_NONSUSPENDING_SIGNATURE;
    XrCoreIrCallableSignatureInput callable_signature = {
        .parameter_types = &parameter_type,
        .parameter_modes = &parameter_mode,
        .parameter_count = 1u,
        .result_type_id = XR_CORE_TYPE_I64,
        .result_ownership = XR_CORE_IR_NON_OWNER,
        .error_type_id = XR_CORE_TYPE_VOID,
        .panic_type_id = XR_CORE_TYPE_VOID,
        .effect_mask = nonsuspending ? 0u : XR_CORE_EFFECT_CANCEL | XR_CORE_EFFECT_SUSPEND,
        .capability_mask = nonsuspending ? 0u : XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION,
    };
    XrCoreIrTypeInput callable_type = {
        .key = xr_program_indirect_coroutine_key("indirect-coro:type:callable"),
        .local_id = XR_PROGRAM_INDIRECT_COROUTINE_CALLABLE,
        .kind = XR_CORE_IR_TYPE_CALLABLE,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .callable_signature = &callable_signature,
    };

    XrCoreIrKey child_key = xr_program_indirect_coroutine_key("indirect-coro:child");
    XrCoreIrKey child_entry_key = xr_program_indirect_coroutine_key("indirect-coro:child:entry");
    XrCoreIrKey child_resume_key = xr_program_indirect_coroutine_key("indirect-coro:child:resume");
    XrCoreIrKey child_cancel_key = xr_program_indirect_coroutine_key("indirect-coro:child:cancel");
    XrCoreIrKey child_argument_key =
        xr_program_indirect_coroutine_key("indirect-coro:child:argument");
    XrCoreIrKey child_resumed_key =
        xr_program_indirect_coroutine_key("indirect-coro:child:resumed");
    XrCoreIrValueInput child_argument = {
        .key = child_argument_key,
        .type_id = XR_CORE_TYPE_I64,
        .category = XR_CORE_IR_VALUE,
        .ownership = XR_CORE_IR_NON_OWNER,
    };
    XrCoreIrValueInput child_resumed = child_argument;
    child_resumed.key = child_resumed_key;
    XrCoreIrKey child_entry_arguments[] = {child_argument_key};
    XrCoreIrKey child_resume_arguments[] = {child_resumed_key};
    XrCoreIrKey child_successors[] = {child_resume_key, child_cancel_key};
    XrCoreIrInstructionInput child_entry_instructions[3] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = child_entry_arguments,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_COROUTINE_YIELD,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = child_entry_arguments,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0u,
         .successors = child_successors,
         .successor_count = 2u},
    };
    XrCoreIrInstructionInput child_resume_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = child_resume_arguments,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = child_resume_arguments,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrInstructionInput child_cancel_instruction = {
        .operation_id = XR_CORE_OP_CORE_CANCEL_PUBLISH,
        .result_type_id = XR_CORE_TYPE_VOID,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    XrCoreIrBlockInput child_blocks[] = {
        {.key = child_entry_key,
         .arguments = &child_argument,
         .argument_count = 1u,
         .instructions = child_entry_instructions,
         .instruction_count = 2u},
        {.key = child_resume_key,
         .arguments = &child_resumed,
         .argument_count = 1u,
         .instructions = child_resume_instructions,
         .instruction_count = 2u},
        {.key = child_cancel_key,
         .instructions = &child_cancel_instruction,
         .instruction_count = 1u},
    };
    XrCoreIrCoroutineStateInput child_states[] = {
        {.state_id = 0u, .continuation_block = child_entry_key},
        {.state_id = 1u, .continuation_block = child_resume_key},
    };
    XrCoreIrKey child_live_values[] = {child_argument_key};
    XrCoreIrCoroutineSafepointInput child_safepoint = {
        .safepoint_id = 0u,
        .resume_state_id = 1u,
        .live_values = child_live_values,
        .live_value_count = 1u,
    };
    XrCoreIrFunctionInput child = {
        .key = child_key,
        .parameter_types = &parameter_type,
        .parameter_modes = &parameter_mode,
        .parameter_count = 1u,
        .result_type_id = XR_CORE_TYPE_I64,
        .result_ownership = XR_CORE_IR_NON_OWNER,
        .error_type_id = XR_CORE_TYPE_VOID,
        .panic_type_id = XR_CORE_TYPE_VOID,
        .effect_mask = XR_CORE_EFFECT_CANCEL | XR_CORE_EFFECT_SUSPEND,
        .capability_mask = XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION,
        .entry_block = child_entry_key,
        .blocks = child_blocks,
        .block_count = 3u,
        .coroutine_states = child_states,
        .coroutine_state_count = 2u,
        .coroutine_safepoints = &child_safepoint,
        .coroutine_safepoint_count = 1u,
    };

    XrCoreIrKey sync_key = xr_program_indirect_coroutine_key("indirect-coro:sync-child");
    XrCoreIrKey sync_entry_key =
        xr_program_indirect_coroutine_key("indirect-coro:sync-child:entry");
    XrCoreIrKey sync_argument_key =
        xr_program_indirect_coroutine_key("indirect-coro:sync-child:argument");
    XrCoreIrValueInput sync_argument = child_argument;
    sync_argument.key = sync_argument_key;
    XrCoreIrKey sync_operands[] = {sync_argument_key};
    XrCoreIrInstructionInput sync_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = sync_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = sync_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput sync_block = {
        .key = sync_entry_key,
        .arguments = &sync_argument,
        .argument_count = 1u,
        .instructions = sync_instructions,
        .instruction_count = 2u,
    };
    XrCoreIrFunctionInput sync_child = {
        .key = sync_key,
        .parameter_types = &parameter_type,
        .parameter_modes = &parameter_mode,
        .parameter_count = 1u,
        .result_type_id = XR_CORE_TYPE_I64,
        .result_ownership = XR_CORE_IR_NON_OWNER,
        .error_type_id = XR_CORE_TYPE_VOID,
        .panic_type_id = XR_CORE_TYPE_VOID,
        .entry_block = sync_entry_key,
        .blocks = &sync_block,
        .block_count = 1u,
    };

    XrCoreIrKey parent_key = xr_program_indirect_coroutine_key("indirect-coro:parent");
    XrCoreIrKey parent_entry_key = xr_program_indirect_coroutine_key("indirect-coro:parent:entry");
    XrCoreIrKey parent_normal_key =
        xr_program_indirect_coroutine_key("indirect-coro:parent:normal");
    XrCoreIrKey parent_cancel_key =
        xr_program_indirect_coroutine_key("indirect-coro:parent:cancel");
    XrCoreIrKey value_key = xr_program_indirect_coroutine_key("indirect-coro:parent:value");
    XrCoreIrKey callable_key = xr_program_indirect_coroutine_key("indirect-coro:parent:callable");
    XrCoreIrKey result_key = xr_program_indirect_coroutine_key("indirect-coro:parent:result");
    XrCoreIrKey live_result_key =
        xr_program_indirect_coroutine_key("indirect-coro:parent:live-result");
    XrCoreIrKey cancel_argument_key =
        xr_program_indirect_coroutine_key("indirect-coro:parent:cancel-argument");
    XrCoreIrKey call_operands[] = {callable_key, value_key};
    XrCoreIrKey parent_successors[] = {parent_normal_key, parent_cancel_key};
    XrCoreIrInstructionInput parent_entry_instructions[4] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = value_key,
         .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant_key},
        {.operation_id = XR_CORE_OP_CORE_CALLABLE_PACK,
         .result = callable_key,
         .result_type_id = XR_PROGRAM_INDIRECT_COROUTINE_CALLABLE,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = nonsuspending ? sync_key : child_key},
        {.operation_id = XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = call_operands,
         .operand_count = 2u,
         .immediate_kind = mutation == XR_PROGRAM_INDIRECT_COROUTINE_BAD_IMMEDIATE
                               ? XR_CORE_IR_IMMEDIATE_NONE
                               : XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = mutation == XR_PROGRAM_INDIRECT_COROUTINE_BAD_SAFEPOINT ? 1u : 0u,
         .successors = parent_successors,
         .successor_count = 2u},
    };
    if (mutation == XR_PROGRAM_INDIRECT_COROUTINE_NONCALLABLE_OPERAND)
        call_operands[0] = value_key;
    XrCoreIrValueInput normal_arguments[] = {
        {.key = result_key,
         .type_id = XR_CORE_TYPE_I64,
         .category = XR_CORE_IR_VALUE,
         .ownership = XR_CORE_IR_NON_OWNER},
        {.key = live_result_key,
         .type_id = XR_CORE_TYPE_I64,
         .category = XR_CORE_IR_VALUE,
         .ownership = XR_CORE_IR_NON_OWNER},
    };
    XrCoreIrKey normal_block_operands[] = {result_key, live_result_key};
    XrCoreIrKey normal_return_operand[] = {result_key};
    XrCoreIrInstructionInput normal_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = normal_block_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = normal_return_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrValueInput cancel_argument = {
        .key = cancel_argument_key,
        .type_id = XR_CORE_TYPE_I64,
        .category = XR_CORE_IR_VALUE,
        .ownership = XR_CORE_IR_NON_OWNER,
    };
    XrCoreIrKey cancel_block_operand[] = {cancel_argument_key};
    XrCoreIrInstructionInput cancel_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = cancel_block_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CANCEL_PUBLISH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    bool missing_live = mutation == XR_PROGRAM_INDIRECT_COROUTINE_MISSING_LIVE;
    bool missing_cancel = mutation == XR_PROGRAM_INDIRECT_COROUTINE_MISSING_CANCEL_ARGUMENT;
    XrCoreIrBlockInput parent_blocks[] = {
        {.key = parent_entry_key,
         .instructions = parent_entry_instructions,
         .instruction_count = 3u},
        {.key = parent_normal_key,
         .arguments = normal_arguments,
         .argument_count = missing_live ? 2u : 1u,
         .instructions = normal_instructions,
         .instruction_count = 2u},
        {.key = parent_cancel_key,
         .arguments = missing_cancel ? &cancel_argument : NULL,
         .argument_count = missing_cancel ? 1u : 0u,
         .instructions = missing_cancel ? cancel_instructions : &cancel_instructions[1],
         .instruction_count = missing_cancel ? 2u : 1u},
    };
    normal_instructions[0].operand_count = missing_live ? 2u : 1u;
    XrCoreIrCoroutineStateInput parent_states[] = {
        {.state_id = 0u, .continuation_block = parent_entry_key},
        {.state_id = 1u, .continuation_block = parent_entry_key},
    };
    XrCoreIrKey parent_live_values[] = {value_key};
    XrCoreIrCoroutineSafepointInput parent_safepoint = {
        .safepoint_id = 0u,
        .resume_state_id = 1u,
        .live_values = missing_live ? parent_live_values : NULL,
        .live_value_count = missing_live ? 1u : 0u,
    };
    XrCoreIrFunctionInput parent = {
        .key = parent_key,
        .result_type_id = XR_CORE_TYPE_I64,
        .result_ownership = XR_CORE_IR_NON_OWNER,
        .error_type_id = XR_CORE_TYPE_VOID,
        .panic_type_id = XR_CORE_TYPE_VOID,
        .effect_mask = XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_CANCEL | XR_CORE_EFFECT_SUSPEND,
        .capability_mask = XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION,
        .entry_block = parent_entry_key,
        .blocks = parent_blocks,
        .block_count = 3u,
        .coroutine_states = parent_states,
        .coroutine_state_count = 2u,
        .coroutine_safepoints = &parent_safepoint,
        .coroutine_safepoint_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };

    bool string_budget = mutation == XR_PROGRAM_COROUTINE_SEALED_STRING_BUDGET;
    bool string_result = mutation == XR_PROGRAM_COROUTINE_SEALED_STRING_RESULT || string_budget;
    XrCoreIrKey string_result_key = xr_program_indirect_coroutine_key("indirect-coro:child:string");
    XrCoreIrKey cancel_string_key = xr_program_indirect_coroutine_key("indirect-coro:child:cancel-string");
    XrCoreIrKey child_string_operands[] = {string_result_key, string_result_key};
    XrCoreIrValueInput cancel_string_argument = {
        .key = cancel_string_key, .type_id = XR_CORE_TYPE_STRING,
        .category = XR_CORE_IR_VALUE, .ownership = XR_CORE_IR_OWNER,
    };
    XrCoreIrInstructionInput cancel_string_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT, .result_type_id = XR_CORE_TYPE_VOID,
         .operands = &cancel_string_key, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .result_type_id = XR_CORE_TYPE_VOID,
         .operands = &cancel_string_key, .operand_count = 1u},
        child_cancel_instruction,
    };
    XrCoreIrKey parent_string_key = xr_program_indirect_coroutine_key("indirect-coro:parent:string");
    if (string_result) {
        parent_entry_instructions[2].operation_id = XR_CORE_OP_CORE_COROUTINE_CALL_SEALED;
        parent_entry_instructions[2].operands = &call_operands[1];
        parent_entry_instructions[2].operand_count = 1u;
        parent_entry_instructions[2].immediate_kind = XR_CORE_IR_IMMEDIATE_COROUTINE_CALL;
        parent_entry_instructions[2].immediate.coroutine_call.callee = child_key;
        parent_entry_instructions[2].immediate.coroutine_call.safepoint_id = 0u;
        parent_entry_instructions[1] = parent_entry_instructions[2];
        parent_blocks[0].instruction_count = 2u;
        callable_signature.result_type_id = XR_CORE_TYPE_STRING;
        callable_signature.result_ownership = XR_CORE_IR_OWNER;
        child.result_type_id = XR_CORE_TYPE_STRING;
        child.result_ownership = XR_CORE_IR_OWNER;
        parent.result_type_id = XR_CORE_TYPE_STRING;
        parent.result_ownership = XR_CORE_IR_OWNER;
        normal_arguments[0].type_id = XR_CORE_TYPE_STRING;
        normal_arguments[0].ownership = XR_CORE_IR_OWNER;
        child_entry_instructions[2] = child_entry_instructions[1];
        child_entry_instructions[2].operands = child_string_operands;
        child_entry_instructions[2].operand_count = 2u;
        child_entry_instructions[1] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_STRING_FROM_I64,
            .result = string_result_key,
            .result_type_id = XR_CORE_TYPE_STRING,
            .result_category = XR_CORE_IR_VALUE,
            .result_ownership = XR_CORE_IR_OWNER,
            .operands = child_entry_arguments,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
        child_blocks[0].instruction_count = 3u;
        child_live_values[0] = string_result_key;
        child_resumed.type_id = XR_CORE_TYPE_STRING;
        child_resumed.ownership = XR_CORE_IR_OWNER;
        child_blocks[2].arguments = &cancel_string_argument;
        child_blocks[2].argument_count = 1u;
        child_blocks[2].instructions = cancel_string_instructions;
        child_blocks[2].instruction_count = 3u;
    }
    if (string_budget) {
        parent_entry_instructions[3] = parent_entry_instructions[1];
        parent_entry_instructions[1] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_STRING_FROM_I64,
            .result = parent_string_key, .result_type_id = XR_CORE_TYPE_STRING,
            .result_category = XR_CORE_IR_VALUE, .result_ownership = XR_CORE_IR_OWNER,
            .operands = &value_key, .operand_count = 1u,
        };
        parent_entry_instructions[2] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP, .result_type_id = XR_CORE_TYPE_VOID,
            .operands = &parent_string_key, .operand_count = 1u,
        };
        parent_blocks[0].instruction_count = 4u;
    }
    XrCoreIrFunctionInput functions[] = {child, sync_child, parent};
    XrCoreIrModuleInput module = {
        .key = xr_program_indirect_coroutine_key("indirect-coro:module"),
        .constants = &constant,
        .constant_count = 1u,
        .functions = functions,
        .function_count = 3u,
    };
    uint8_t profile[XR_PROGRAM_DIGEST_SIZE] = {0};
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = profile,
        .required_features = &feature,
        .required_feature_count = 1u,
        .types = string_result ? NULL : &callable_type,
        .type_count = string_result ? 0u : 1u,
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

#endif /* XR_PROGRAM_INDIRECT_COROUTINE_FIXTURE_H */
