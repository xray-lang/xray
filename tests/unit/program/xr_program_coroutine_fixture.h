#ifndef XR_PROGRAM_COROUTINE_FIXTURE_H
#define XR_PROGRAM_COROUTINE_FIXTURE_H

#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"

#include <string.h>

typedef enum XrProgramCoroutineFixtureMutation {
    XR_PROGRAM_COROUTINE_FIXTURE_VALID = 0,
    XR_PROGRAM_COROUTINE_FIXTURE_UNUSED_LIVE,
    XR_PROGRAM_COROUTINE_FIXTURE_MISSING_CANCEL_EDGE,
    XR_PROGRAM_COROUTINE_FIXTURE_NORMAL_EDGE_TO_CANCEL,
} XrProgramCoroutineFixtureMutation;

static XrCoreIrKey xr_program_coroutine_fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrProgramBuildStatus
xr_program_coroutine_fixture_write_mutated(XrProgramCoroutineFixtureMutation mutation,
                                           XrProgramArtifact *artifact, char *diagnostic,
                                           size_t diagnostic_size) {
    XrCoreIrConstantInput constants[] = {
        {.key = xr_program_coroutine_fixture_key("coro:forty"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 40},
        {.key = xr_program_coroutine_fixture_key("coro:two"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 2},
    };
    XrCoreIrKey entry_key = xr_program_coroutine_fixture_key("coro:entry");
    XrCoreIrKey resume_key = xr_program_coroutine_fixture_key("coro:resume");
    XrCoreIrKey cancel_key = xr_program_coroutine_fixture_key("coro:cancel");
    XrCoreIrKey forty = xr_program_coroutine_fixture_key("coro:value:forty");
    XrCoreIrKey resumed = xr_program_coroutine_fixture_key("coro:value:resumed");
    XrCoreIrKey two = xr_program_coroutine_fixture_key("coro:value:two");
    XrCoreIrKey sum = xr_program_coroutine_fixture_key("coro:value:sum");
    XrCoreIrKey yield_operands[] = {forty};
    XrCoreIrKey yield_successors[] = {resume_key, cancel_key};
    XrCoreIrInstructionInput entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = forty,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_COROUTINE_YIELD,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = yield_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0u,
         .successors = yield_successors,
         .successor_count = mutation == XR_PROGRAM_COROUTINE_FIXTURE_MISSING_CANCEL_EDGE ? 1u : 2u},
    };
    XrCoreIrValueInput resume_argument = {
        .key = resumed,
        .type_id = XR_CORE_TYPE_I64,
    };
    XrCoreIrKey resume_arguments[] = {resumed};
    XrCoreIrKey add_operands[] = {
        mutation == XR_PROGRAM_COROUTINE_FIXTURE_UNUSED_LIVE ? two : resumed,
        two,
    };
    XrCoreIrKey return_operand[] = {sum};
    XrCoreIrInstructionInput resume_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = resume_arguments,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = two,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_ADD_I64,
         .result = sum,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = add_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 1u},
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
    XrCoreIrKey cancel_successor[] = {cancel_key};
    if (mutation == XR_PROGRAM_COROUTINE_FIXTURE_NORMAL_EDGE_TO_CANCEL) {
        resume_instructions[3].operation_id = XR_CORE_OP_CORE_BRANCH;
        resume_instructions[3].operands = NULL;
        resume_instructions[3].operand_count = 0u;
        resume_instructions[3].successors = cancel_successor;
        resume_instructions[3].successor_count = 1u;
    }
    XrCoreIrBlockInput blocks[] = {
        {.key = entry_key, .instructions = entry_instructions, .instruction_count = 2u},
        {.key = resume_key,
         .arguments = &resume_argument,
         .argument_count = 1u,
         .instructions = resume_instructions,
         .instruction_count = 4u},
        {.key = cancel_key, .instructions = &cancel_instruction, .instruction_count = 1u},
    };
    XrCoreIrCoroutineStateInput states[] = {
        {.state_id = 0u, .continuation_block = entry_key},
        {.state_id = 1u, .continuation_block = resume_key},
    };
    XrCoreIrKey live_values[] = {forty};
    XrCoreIrCoroutineSafepointInput safepoint = {
        .safepoint_id = 0u,
        .resume_state_id = 1u,
        .live_values = live_values,
        .live_value_count = 1u,
    };
    XrCoreIrFunctionInput function = {
        .key = xr_program_coroutine_fixture_key("coro:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = XR_CORE_EFFECT_CANCEL | XR_CORE_EFFECT_SUSPEND | XR_CORE_EFFECT_TRAP,
        .capability_mask = XR_CORE_CAPABILITY_RUNTIME_COOPERATIVE_YIELD,
        .entry_block = entry_key,
        .blocks = blocks,
        .block_count = 3u,
        .coroutine_states = states,
        .coroutine_state_count = 2u,
        .coroutine_safepoints = &safepoint,
        .coroutine_safepoint_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = xr_program_coroutine_fixture_key("coro:module"),
        .constants = constants,
        .constant_count = 2u,
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

static XrProgramBuildStatus xr_program_coroutine_fixture_write(XrProgramArtifact *artifact,
                                                               char *diagnostic,
                                                               size_t diagnostic_size) {
    return xr_program_coroutine_fixture_write_mutated(XR_PROGRAM_COROUTINE_FIXTURE_VALID, artifact,
                                                      diagnostic, diagnostic_size);
}

typedef enum XrProgramCoroutineOwnerFixtureMutation {
    XR_PROGRAM_COROUTINE_OWNER_FIXTURE_VALID = 0,
    XR_PROGRAM_COROUTINE_OWNER_FIXTURE_MISSING_CANCEL_ARGUMENT,
    XR_PROGRAM_COROUTINE_OWNER_FIXTURE_CANCEL_OWNER_NOT_DROPPED,
    XR_PROGRAM_COROUTINE_OWNER_FIXTURE_EXTRA_CANCEL_ARGUMENT,
} XrProgramCoroutineOwnerFixtureMutation;

static XrProgramBuildStatus
xr_program_coroutine_owner_fixture_write_mutated(XrProgramCoroutineOwnerFixtureMutation mutation,
                                                 XrProgramArtifact *artifact, char *diagnostic,
                                                 size_t diagnostic_size) {
    XrCoreIrConstantInput constant = {
        .key = xr_program_coroutine_fixture_key("owner-coro:zero"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 0,
    };
    XrCoreIrKey main_block_key = xr_program_coroutine_fixture_key("owner-coro:main:block");
    XrCoreIrKey zero = xr_program_coroutine_fixture_key("owner-coro:main:zero");
    XrCoreIrKey main_return_operand[] = {zero};
    XrCoreIrInstructionInput main_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = zero,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant.key},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = main_return_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput main_block = {
        .key = main_block_key,
        .instructions = main_instructions,
        .instruction_count = 2u,
    };

    XrCoreIrKey owner_entry_key = xr_program_coroutine_fixture_key("owner-coro:owner:entry");
    XrCoreIrKey owner_resume_key = xr_program_coroutine_fixture_key("owner-coro:owner:resume");
    XrCoreIrKey owner_cancel_key = xr_program_coroutine_fixture_key("owner-coro:owner:cancel");
    XrCoreIrKey parameter = xr_program_coroutine_fixture_key("owner-coro:value:parameter");
    XrCoreIrKey resumed = xr_program_coroutine_fixture_key("owner-coro:value:resumed");
    XrCoreIrKey cancelled = xr_program_coroutine_fixture_key("owner-coro:value:cancelled");
    XrCoreIrKey extra = xr_program_coroutine_fixture_key("owner-coro:value:extra");
    XrCoreIrValueInput entry_argument = {
        .key = parameter,
        .type_id = XR_CORE_TYPE_PANIC_INFO,
        .category = XR_CORE_IR_VALUE,
        .ownership = XR_CORE_IR_OWNER,
    };
    XrCoreIrKey entry_arguments[] = {parameter};
    XrCoreIrKey yield_operands[] = {parameter, parameter};
    XrCoreIrKey yield_successors[] = {owner_resume_key, owner_cancel_key};
    XrCoreIrInstructionInput owner_entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = entry_arguments,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_COROUTINE_YIELD,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = yield_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0u,
         .successors = yield_successors,
         .successor_count = 2u},
    };
    XrCoreIrValueInput resume_argument = {
        .key = resumed,
        .type_id = XR_CORE_TYPE_PANIC_INFO,
        .category = XR_CORE_IR_VALUE,
        .ownership = XR_CORE_IR_OWNER,
    };
    XrCoreIrKey resume_arguments[] = {resumed};
    XrCoreIrInstructionInput owner_resume_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = resume_arguments,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = resume_arguments,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrValueInput cancel_arguments[] = {
        {.key = cancelled,
         .type_id = XR_CORE_TYPE_PANIC_INFO,
         .category = XR_CORE_IR_VALUE,
         .ownership = XR_CORE_IR_OWNER},
        {.key = extra,
         .type_id = XR_CORE_TYPE_PANIC_INFO,
         .category = XR_CORE_IR_VALUE,
         .ownership = XR_CORE_IR_OWNER},
    };
    XrCoreIrKey cancel_argument_keys[] = {cancelled, extra};
    XrCoreIrKey drop_operand[] = {cancelled};
    XrCoreIrInstructionInput owner_cancel_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = cancel_argument_keys,
         .operand_count =
             mutation == XR_PROGRAM_COROUTINE_OWNER_FIXTURE_EXTRA_CANCEL_ARGUMENT ? 2u : 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = drop_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CANCEL_PUBLISH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    uint32_t cancel_argument_count =
        mutation == XR_PROGRAM_COROUTINE_OWNER_FIXTURE_MISSING_CANCEL_ARGUMENT
            ? 0u
            : (mutation == XR_PROGRAM_COROUTINE_OWNER_FIXTURE_EXTRA_CANCEL_ARGUMENT ? 2u : 1u);
    uint32_t cancel_instruction_start = cancel_argument_count == 0u ? 2u : 0u;
    uint32_t cancel_instruction_count =
        cancel_argument_count == 0u
            ? 1u
            : (mutation == XR_PROGRAM_COROUTINE_OWNER_FIXTURE_CANCEL_OWNER_NOT_DROPPED ? 2u : 3u);
    if (mutation == XR_PROGRAM_COROUTINE_OWNER_FIXTURE_CANCEL_OWNER_NOT_DROPPED)
        owner_cancel_instructions[1] = owner_cancel_instructions[2];
    XrCoreIrBlockInput owner_blocks[] = {
        {.key = owner_entry_key,
         .arguments = &entry_argument,
         .argument_count = 1u,
         .instructions = owner_entry_instructions,
         .instruction_count = 2u},
        {.key = owner_resume_key,
         .arguments = &resume_argument,
         .argument_count = 1u,
         .instructions = owner_resume_instructions,
         .instruction_count = 2u},
        {.key = owner_cancel_key,
         .arguments = cancel_argument_count == 0u ? NULL : cancel_arguments,
         .argument_count = cancel_argument_count,
         .instructions = &owner_cancel_instructions[cancel_instruction_start],
         .instruction_count = cancel_instruction_count},
    };
    XrCoreIrCoroutineStateInput states[] = {
        {.state_id = 0u, .continuation_block = owner_entry_key},
        {.state_id = 1u, .continuation_block = owner_resume_key},
    };
    XrCoreIrKey live_values[] = {parameter};
    XrCoreIrCoroutineSafepointInput safepoint = {
        .safepoint_id = 0u,
        .resume_state_id = 1u,
        .live_values = live_values,
        .live_value_count = 1u,
    };
    uint16_t parameter_type = XR_CORE_TYPE_PANIC_INFO;
    XrParamMode parameter_mode = XR_PARAM_MOVE;
    XrCoreIrFunctionInput functions[] = {
        {.key = xr_program_coroutine_fixture_key("owner-coro:main"),
         .result_type_id = XR_CORE_TYPE_I64,
         .entry_block = main_block_key,
         .blocks = &main_block,
         .block_count = 1u,
         .flags = XR_PROGRAM_FUNCTION_ENTRY},
        {.key = xr_program_coroutine_fixture_key("owner-coro:owner"),
         .parameter_types = &parameter_type,
         .parameter_modes = &parameter_mode,
         .parameter_count = 1u,
         .result_type_id = XR_CORE_TYPE_PANIC_INFO,
         .result_ownership = XR_CORE_IR_OWNER,
         .effect_mask = XR_CORE_EFFECT_CANCEL | XR_CORE_EFFECT_SUSPEND,
         .capability_mask = XR_CORE_CAPABILITY_RUNTIME_COOPERATIVE_YIELD,
         .entry_block = owner_entry_key,
         .blocks = owner_blocks,
         .block_count = 3u,
         .coroutine_states = states,
         .coroutine_state_count = 2u,
         .coroutine_safepoints = &safepoint,
         .coroutine_safepoint_count = 1u},
    };
    XrCoreIrModuleInput module = {
        .key = xr_program_coroutine_fixture_key("owner-coro:module"),
        .constants = &constant,
        .constant_count = 1u,
        .functions = functions,
        .function_count = 2u,
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

static XrProgramBuildStatus xr_program_coroutine_owner_fixture_write(XrProgramArtifact *artifact,
                                                                     char *diagnostic,
                                                                     size_t diagnostic_size) {
    return xr_program_coroutine_owner_fixture_write_mutated(
        XR_PROGRAM_COROUTINE_OWNER_FIXTURE_VALID, artifact, diagnostic, diagnostic_size);
}

#endif
