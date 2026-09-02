#ifndef XR_PROGRAM_PANIC_FIXTURE_H
#define XR_PROGRAM_PANIC_FIXTURE_H

#include "../../../src/core/xr_core_spec_gen.h"
#include "../../../src/program/xr_program.h"

#include <string.h>

#define XR_PANIC_FIXTURE_AFFINE_TYPE UINT16_C(16)

typedef enum XrProgramPanicFixtureMutation {
    XR_PANIC_FIXTURE_VALID = 0,
    XR_PANIC_FIXTURE_DIRECT_PANICFUL,
    XR_PANIC_FIXTURE_COPY_PANIC,
    XR_PANIC_FIXTURE_PANIC_AS_ERROR,
    XR_PANIC_FIXTURE_WRONG_CHANNEL_TYPE,
    XR_PANIC_FIXTURE_MISSING_NORMAL_OWNER,
    XR_PANIC_FIXTURE_DUPLICATE_NORMAL_OWNER,
    XR_PANIC_FIXTURE_MISSING_PANIC_OWNER,
    XR_PANIC_FIXTURE_DUPLICATE_PANIC_OWNER,
} XrProgramPanicFixtureMutation;

static XrCoreIrKey xr_panic_fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

/* A callee with independent business-error and panic channels, plus a caller
 * whose affine local is closed on normal, error, and panic continuations.
 * PanicInfo itself is moved exactly once and published only on the panic path. */
static XrProgramBuildStatus
xr_program_panic_fixture_write_mutated(XrProgramPanicFixtureMutation mutation,
                                       XrProgramArtifact *artifact, char *diagnostic,
                                       size_t diagnostic_size) {
    uint16_t affine_fields[] = {XR_CORE_TYPE_I64};
    XrCoreIrTypeInput affine_type = {
        .key = xr_panic_fixture_key("panic:type:affine"),
        .local_id = XR_PANIC_FIXTURE_AFFINE_TYPE,
        .kind = XR_CORE_IR_TYPE_AGGREGATE,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .field_types = affine_fields,
        .field_count = 1u,
    };
    XrCoreIrConstantInput constant = {
        .key = xr_panic_fixture_key("panic:constant:42"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 42,
    };

    XrCoreIrKey callee_key = xr_panic_fixture_key("panic:callee");
    XrCoreIrKey callee_entry_key = xr_panic_fixture_key("panic:callee:entry");
    XrCoreIrKey callee_normal_key = xr_panic_fixture_key("panic:callee:normal");
    XrCoreIrKey callee_panic_key = xr_panic_fixture_key("panic:callee:panic");
    XrCoreIrKey callee_condition = xr_panic_fixture_key("panic:callee:condition");
    XrCoreIrKey callee_payload = xr_panic_fixture_key("panic:callee:payload");
    XrCoreIrKey callee_normal_payload = xr_panic_fixture_key("panic:callee:normal-payload");
    XrCoreIrKey callee_panic_payload = xr_panic_fixture_key("panic:callee:panic-payload");
    XrCoreIrKey callee_result = xr_panic_fixture_key("panic:callee:result");
    XrCoreIrValueInput callee_entry_arguments[] = {
        {.key = callee_condition, .type_id = XR_CORE_TYPE_BOOL},
        {.key = callee_payload, .type_id = XR_CORE_TYPE_PANIC_INFO, .ownership = XR_CORE_IR_OWNER},
    };
    XrCoreIrKey callee_entry_values[] = {callee_condition, callee_payload};
    XrCoreIrKey callee_branch_operands[] = {callee_condition, callee_payload, callee_payload};
    XrCoreIrKey callee_successors[] = {callee_normal_key, callee_panic_key};
    XrCoreIrInstructionInput callee_entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = callee_entry_values,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONDITIONAL_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = callee_branch_operands,
         .operand_count = 3u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = callee_successors,
         .successor_count = 2u},
    };
    XrCoreIrValueInput callee_normal_argument = {
        .key = callee_normal_payload,
        .type_id = XR_CORE_TYPE_PANIC_INFO,
        .ownership = XR_CORE_IR_OWNER,
    };
    XrCoreIrKey callee_normal_args[] = {callee_normal_payload};
    XrCoreIrKey callee_return[] = {callee_result};
    XrCoreIrInstructionInput callee_normal_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = callee_normal_args,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = callee_normal_args,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = callee_result,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant.key},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = callee_return,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrValueInput callee_panic_argument = {
        .key = callee_panic_payload,
        .type_id = XR_CORE_TYPE_PANIC_INFO,
        .ownership = XR_CORE_IR_OWNER,
    };
    XrCoreIrKey callee_panic_args[] = {callee_panic_payload};
    XrCoreIrInstructionInput callee_panic_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = callee_panic_args,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_PANIC_PUBLISH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = callee_panic_args,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput callee_blocks[] = {
        {.key = callee_entry_key,
         .arguments = callee_entry_arguments,
         .argument_count = 2u,
         .instructions = callee_entry_instructions,
         .instruction_count = 2u},
        {.key = callee_normal_key,
         .arguments = &callee_normal_argument,
         .argument_count = 1u,
         .instructions = callee_normal_instructions,
         .instruction_count = 4u},
        {.key = callee_panic_key,
         .arguments = &callee_panic_argument,
         .argument_count = 1u,
         .instructions = callee_panic_instructions,
         .instruction_count = 2u},
    };
    uint16_t callee_parameters[] = {XR_CORE_TYPE_BOOL, XR_CORE_TYPE_PANIC_INFO};
    XrParamMode callee_modes[] = {XR_PARAM_READ, XR_PARAM_MOVE};
    XrCoreIrFunctionInput callee_function = {
        .key = callee_key,
        .parameter_types = callee_parameters,
        .parameter_modes = callee_modes,
        .parameter_count = 2u,
        .result_type_id = XR_CORE_TYPE_I64,
        .error_type_id = mutation == XR_PANIC_FIXTURE_PANIC_AS_ERROR ? XR_CORE_TYPE_PANIC_INFO
                                                                     : XR_CORE_TYPE_ERROR,
        .panic_type_id = XR_CORE_TYPE_PANIC_INFO,
        .effect_mask = XR_CORE_EFFECT_ERROR | XR_CORE_EFFECT_PANIC,
        .entry_block = callee_entry_key,
        .blocks = callee_blocks,
        .block_count = 3u,
    };

    XrCoreIrKey entry_key = xr_panic_fixture_key("panic:entry");
    XrCoreIrKey entry_block_key = xr_panic_fixture_key("panic:entry:block");
    XrCoreIrKey normal_key = xr_panic_fixture_key("panic:entry:normal");
    XrCoreIrKey error_key = xr_panic_fixture_key("panic:entry:error");
    XrCoreIrKey panic_key = xr_panic_fixture_key("panic:entry:panic");
    XrCoreIrKey condition = xr_panic_fixture_key("panic:entry:condition");
    XrCoreIrKey payload = xr_panic_fixture_key("panic:entry:payload");
    XrCoreIrKey seed = xr_panic_fixture_key("panic:entry:seed");
    XrCoreIrKey owner = xr_panic_fixture_key("panic:entry:owner");
    XrCoreIrKey direct_result = xr_panic_fixture_key("panic:entry:direct-result");
    XrCoreIrKey copied_panic = xr_panic_fixture_key("panic:entry:copied-panic");
    XrCoreIrKey normal_result = xr_panic_fixture_key("panic:entry:normal-result");
    XrCoreIrKey normal_owner = xr_panic_fixture_key("panic:entry:normal-owner");
    XrCoreIrKey normal_owner_2 = xr_panic_fixture_key("panic:entry:normal-owner-2");
    XrCoreIrKey edge_error = xr_panic_fixture_key("panic:entry:edge-error");
    XrCoreIrKey error_owner = xr_panic_fixture_key("panic:entry:error-owner");
    XrCoreIrKey edge_panic = xr_panic_fixture_key("panic:entry:edge-panic");
    XrCoreIrKey panic_owner = xr_panic_fixture_key("panic:entry:panic-owner");
    XrCoreIrKey panic_owner_2 = xr_panic_fixture_key("panic:entry:panic-owner-2");
    XrCoreIrValueInput entry_arguments[] = {
        {.key = condition, .type_id = XR_CORE_TYPE_BOOL},
        {.key = payload, .type_id = XR_CORE_TYPE_PANIC_INFO, .ownership = XR_CORE_IR_OWNER},
        {.key = seed, .type_id = XR_CORE_TYPE_I64},
    };
    XrCoreIrKey entry_values[] = {condition, payload, seed};
    XrCoreIrKey construct_operands[] = {seed};
    uint32_t normal_owner_count = mutation == XR_PANIC_FIXTURE_DUPLICATE_NORMAL_OWNER ? 2u : 1u;
    uint32_t panic_owner_count = mutation == XR_PANIC_FIXTURE_DUPLICATE_PANIC_OWNER ? 2u : 1u;
    uint32_t normal_transfer_count =
        mutation == XR_PANIC_FIXTURE_MISSING_NORMAL_OWNER ? 0u : normal_owner_count;
    uint32_t panic_transfer_count =
        mutation == XR_PANIC_FIXTURE_MISSING_PANIC_OWNER ? 0u : panic_owner_count;
    XrCoreIrKey invoke_operands[7] = {condition, payload};
    uint32_t invoke_operand_count = 2u;
    for (uint32_t index = 0u; index < normal_transfer_count; ++index)
        invoke_operands[invoke_operand_count++] = owner;
    invoke_operands[invoke_operand_count++] = owner;
    for (uint32_t index = 0u; index < panic_transfer_count; ++index)
        invoke_operands[invoke_operand_count++] = owner;
    XrCoreIrKey invoke_successors[] = {normal_key, error_key, panic_key};
    bool direct_panicful = mutation == XR_PANIC_FIXTURE_DIRECT_PANICFUL;
    XrCoreIrKey direct_return[] = {direct_result};
    bool copy_panic = mutation == XR_PANIC_FIXTURE_COPY_PANIC;
    XrCoreIrInstructionInput entry_instructions[5] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = entry_values,
         .operand_count = 3u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
         .result = owner,
         .result_type_id = XR_PANIC_FIXTURE_AFFINE_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = construct_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = direct_panicful ? XR_CORE_OP_CORE_CALL_SEALED_DIRECT
                                         : XR_CORE_OP_CORE_CALL_SEALED_INVOKE,
         .result = direct_panicful ? direct_result : (XrCoreIrKey) {{0}},
         .result_type_id = direct_panicful ? XR_CORE_TYPE_I64 : XR_CORE_TYPE_VOID,
         .operands = invoke_operands,
         .operand_count = direct_panicful ? 2u : invoke_operand_count,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = callee_key,
         .successors = direct_panicful ? NULL : invoke_successors,
         .successor_count = direct_panicful ? 0u : 3u},
    };
    uint32_t entry_instruction_count = 3u;
    if (copy_panic) {
        entry_instructions[3] = entry_instructions[2];
        entry_instructions[2] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_COPY,
            .result = copied_panic,
            .result_type_id = XR_CORE_TYPE_PANIC_INFO,
            .result_ownership = XR_CORE_IR_OWNER,
            .operands = &entry_values[1],
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
        entry_instruction_count = 4u;
    } else if (direct_panicful) {
        entry_instructions[3] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_RETURN,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = direct_return,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
        entry_instruction_count = 4u;
    }

    XrCoreIrValueInput normal_arguments[] = {
        {.key = normal_result, .type_id = XR_CORE_TYPE_I64},
        {.key = normal_owner,
         .type_id = XR_PANIC_FIXTURE_AFFINE_TYPE,
         .ownership = XR_CORE_IR_OWNER},
        {.key = normal_owner_2,
         .type_id = XR_PANIC_FIXTURE_AFFINE_TYPE,
         .ownership = XR_CORE_IR_OWNER},
    };
    XrCoreIrKey normal_args[] = {normal_result, normal_owner, normal_owner_2};
    XrCoreIrKey normal_drop[][1] = {{normal_owner}, {normal_owner_2}};
    XrCoreIrKey normal_return[] = {normal_result};
    XrCoreIrInstructionInput normal_instructions[4] = {{
        .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = normal_args,
        .operand_count = 1u + normal_owner_count,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    }};
    uint32_t normal_instruction_count = 1u;
    for (uint32_t index = 0u; index < normal_owner_count; ++index)
        normal_instructions[normal_instruction_count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = normal_drop[index],
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
    normal_instructions[normal_instruction_count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_RETURN,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = normal_return,
        .operand_count = 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };

    XrCoreIrValueInput error_arguments[] = {
        {.key = edge_error, .type_id = XR_CORE_TYPE_ERROR},
        {.key = error_owner,
         .type_id = XR_PANIC_FIXTURE_AFFINE_TYPE,
         .ownership = XR_CORE_IR_OWNER},
    };
    XrCoreIrKey error_args[] = {edge_error, error_owner};
    XrCoreIrKey error_drop[] = {error_owner};
    XrCoreIrKey error_publish[] = {edge_error};
    XrCoreIrInstructionInput error_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = error_args,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = error_drop,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_ERROR_PUBLISH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = error_publish,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };

    XrCoreIrValueInput panic_arguments[] = {
        {.key = edge_panic,
         .type_id = mutation == XR_PANIC_FIXTURE_WRONG_CHANNEL_TYPE ? XR_CORE_TYPE_ERROR
                                                                    : XR_CORE_TYPE_PANIC_INFO,
         .ownership = mutation == XR_PANIC_FIXTURE_WRONG_CHANNEL_TYPE ? XR_CORE_IR_NON_OWNER
                                                                      : XR_CORE_IR_OWNER},
        {.key = panic_owner,
         .type_id = XR_PANIC_FIXTURE_AFFINE_TYPE,
         .ownership = XR_CORE_IR_OWNER},
        {.key = panic_owner_2,
         .type_id = XR_PANIC_FIXTURE_AFFINE_TYPE,
         .ownership = XR_CORE_IR_OWNER},
    };
    XrCoreIrKey panic_args[] = {edge_panic, panic_owner, panic_owner_2};
    XrCoreIrKey panic_drop[][1] = {{panic_owner}, {panic_owner_2}};
    XrCoreIrInstructionInput panic_instructions[4] = {{
        .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = panic_args,
        .operand_count = 1u + panic_owner_count,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    }};
    uint32_t panic_instruction_count = 1u;
    for (uint32_t index = 0u; index < panic_owner_count; ++index)
        panic_instructions[panic_instruction_count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = panic_drop[index],
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
    panic_instructions[panic_instruction_count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_PANIC_PUBLISH,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = panic_args,
        .operand_count = 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    XrCoreIrBlockInput entry_blocks[] = {
        {.key = entry_block_key,
         .arguments = entry_arguments,
         .argument_count = 3u,
         .instructions = entry_instructions,
         .instruction_count = entry_instruction_count},
        {.key = normal_key,
         .arguments = normal_arguments,
         .argument_count = 1u + normal_owner_count,
         .instructions = normal_instructions,
         .instruction_count = normal_instruction_count},
        {.key = error_key,
         .arguments = error_arguments,
         .argument_count = 2u,
         .instructions = error_instructions,
         .instruction_count = 3u},
        {.key = panic_key,
         .arguments = panic_arguments,
         .argument_count = 1u + panic_owner_count,
         .instructions = panic_instructions,
         .instruction_count = panic_instruction_count},
    };
    uint16_t entry_parameters[] = {XR_CORE_TYPE_BOOL, XR_CORE_TYPE_PANIC_INFO, XR_CORE_TYPE_I64};
    XrParamMode entry_modes[] = {XR_PARAM_READ, XR_PARAM_MOVE, XR_PARAM_READ};
    XrCoreIrFunctionInput entry_function = {
        .key = entry_key,
        .parameter_types = entry_parameters,
        .parameter_modes = entry_modes,
        .parameter_count = 3u,
        .result_type_id = XR_CORE_TYPE_I64,
        .error_type_id = XR_CORE_TYPE_ERROR,
        .panic_type_id = XR_CORE_TYPE_PANIC_INFO,
        .effect_mask = XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_ERROR | XR_CORE_EFFECT_PANIC,
        .entry_block = entry_block_key,
        .blocks = entry_blocks,
        .block_count = 4u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrFunctionInput functions[] = {callee_function, entry_function};
    XrCoreIrModuleInput module = {
        .key = xr_panic_fixture_key("panic:module"),
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
        .types = &affine_type,
        .type_count = 1u,
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

static XrProgramBuildStatus xr_program_panic_fixture_write(XrProgramArtifact *artifact,
                                                           char *diagnostic,
                                                           size_t diagnostic_size) {
    return xr_program_panic_fixture_write_mutated(XR_PANIC_FIXTURE_VALID, artifact, diagnostic,
                                                  diagnostic_size);
}

#endif /* XR_PROGRAM_PANIC_FIXTURE_H */
