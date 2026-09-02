#ifndef XR_PROGRAM_INVOKE_FIXTURE_H
#define XR_PROGRAM_INVOKE_FIXTURE_H

#include "../../../src/core/xr_core_spec_gen.h"
#include "../../../src/program/xr_program.h"

#include <string.h>

#define XR_INVOKE_FIXTURE_AFFINE_TYPE UINT16_C(16)

typedef enum XrProgramInvokeFixtureMutation {
    XR_INVOKE_FIXTURE_VALID = 0,
    XR_INVOKE_FIXTURE_INVOKE_INFALLIBLE,
    XR_INVOKE_FIXTURE_WRONG_NORMAL_RESULT_TYPE,
    XR_INVOKE_FIXTURE_WRONG_ERROR_TYPE,
    XR_INVOKE_FIXTURE_MISSING_NORMAL_OWNER,
    XR_INVOKE_FIXTURE_DUPLICATE_NORMAL_OWNER,
    XR_INVOKE_FIXTURE_MISSING_ERROR_OWNER,
    XR_INVOKE_FIXTURE_DUPLICATE_ERROR_OWNER,
} XrProgramInvokeFixtureMutation;

static XrCoreIrKey xr_invoke_fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

/* Builds a fallible callee and an entry function whose affine local is
 * explicitly dropped on both the invoke normal and error continuations. */
static XrProgramBuildStatus xr_program_invoke_fixture_write_mutated(
    XrProgramInvokeFixtureMutation mutation, XrProgramArtifact *artifact, char *diagnostic,
    size_t diagnostic_size) {
    uint16_t affine_fields[] = {XR_CORE_TYPE_I64};
    XrCoreIrTypeInput affine_type = {
        .key = xr_invoke_fixture_key("invoke:type:affine"),
        .local_id = XR_INVOKE_FIXTURE_AFFINE_TYPE,
        .kind = XR_CORE_IR_TYPE_AGGREGATE,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .field_types = affine_fields,
        .field_count = 1u,
    };

    XrCoreIrKey callee_key = xr_invoke_fixture_key("invoke:callee");
    XrCoreIrKey callee_entry_key = xr_invoke_fixture_key("invoke:callee:entry");
    XrCoreIrKey callee_normal_key = xr_invoke_fixture_key("invoke:callee:normal");
    XrCoreIrKey callee_error_key = xr_invoke_fixture_key("invoke:callee:error");
    XrCoreIrKey callee_condition = xr_invoke_fixture_key("invoke:callee:condition");
    XrCoreIrKey callee_error = xr_invoke_fixture_key("invoke:callee:error-value");
    XrCoreIrKey callee_edge_error = xr_invoke_fixture_key("invoke:callee:edge-error");
    XrCoreIrKey callee_result = xr_invoke_fixture_key("invoke:callee:result");
    XrCoreIrKey callee_error_result =
        xr_invoke_fixture_key("invoke:callee:infallible-result");
    XrCoreIrValueInput callee_entry_arguments[] = {
        {.key = callee_condition, .type_id = XR_CORE_TYPE_BOOL},
        {.key = callee_error, .type_id = XR_CORE_TYPE_ERROR},
    };
    XrCoreIrKey callee_entry_values[] = {callee_condition, callee_error};
    XrCoreIrKey callee_branch_operands[] = {callee_condition, callee_error};
    XrCoreIrKey callee_successors[] = {callee_normal_key, callee_error_key};
    XrCoreIrInstructionInput callee_entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = callee_entry_values,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONDITIONAL_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = callee_branch_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = callee_successors,
         .successor_count = 2u},
    };
    XrCoreIrConstantInput constant = {
        .key = xr_invoke_fixture_key("invoke:constant:42"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 42,
    };
    XrCoreIrKey callee_return_operands[] = {callee_result};
    XrCoreIrInstructionInput callee_normal_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = callee_result,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant.key},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = callee_return_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrValueInput callee_error_argument = {
        .key = callee_edge_error,
        .type_id = XR_CORE_TYPE_ERROR,
    };
    XrCoreIrKey callee_error_values[] = {callee_edge_error};
    XrCoreIrKey callee_error_return[] = {callee_error_result};
    XrCoreIrInstructionInput callee_fallible_error_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = callee_error_values,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_ERROR_PUBLISH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = callee_error_values,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrInstructionInput callee_infallible_error_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = callee_error_values,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = callee_error_result,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant.key},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = callee_error_return,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    bool infallible_callee = mutation == XR_INVOKE_FIXTURE_INVOKE_INFALLIBLE;
    XrCoreIrBlockInput callee_blocks[] = {
        {.key = callee_entry_key,
         .arguments = callee_entry_arguments,
         .argument_count = 2u,
         .instructions = callee_entry_instructions,
         .instruction_count = 2u},
        {.key = callee_normal_key,
         .instructions = callee_normal_instructions,
         .instruction_count = 2u},
        {.key = callee_error_key,
         .arguments = &callee_error_argument,
         .argument_count = 1u,
         .instructions = infallible_callee ? callee_infallible_error_instructions
                                            : callee_fallible_error_instructions,
         .instruction_count = infallible_callee ? 3u : 2u},
    };
    uint16_t callee_parameters[] = {XR_CORE_TYPE_BOOL, XR_CORE_TYPE_ERROR};
    XrCoreIrFunctionInput callee_function = {
        .key = callee_key,
        .parameter_types = callee_parameters,
        .parameter_count = 2u,
        .result_type_id = XR_CORE_TYPE_I64,
        .error_type_id = infallible_callee ? XR_CORE_TYPE_VOID : XR_CORE_TYPE_ERROR,
        .effect_mask = infallible_callee ? 0u : XR_CORE_EFFECT_ERROR,
        .entry_block = callee_entry_key,
        .blocks = callee_blocks,
        .block_count = 3u,
    };

    XrCoreIrKey entry_key = xr_invoke_fixture_key("invoke:entry");
    XrCoreIrKey entry_block_key = xr_invoke_fixture_key("invoke:entry:block");
    XrCoreIrKey normal_key = xr_invoke_fixture_key("invoke:entry:normal");
    XrCoreIrKey error_key = xr_invoke_fixture_key("invoke:entry:error");
    XrCoreIrKey condition = xr_invoke_fixture_key("invoke:entry:condition");
    XrCoreIrKey error_value = xr_invoke_fixture_key("invoke:entry:error-value");
    XrCoreIrKey seed = xr_invoke_fixture_key("invoke:entry:seed");
    XrCoreIrKey owner = xr_invoke_fixture_key("invoke:entry:owner");
    XrCoreIrKey normal_result = xr_invoke_fixture_key("invoke:entry:normal-result");
    XrCoreIrKey normal_owner = xr_invoke_fixture_key("invoke:entry:normal-owner");
    XrCoreIrKey normal_owner_duplicate =
        xr_invoke_fixture_key("invoke:entry:normal-owner-duplicate");
    XrCoreIrKey normal_fallback = xr_invoke_fixture_key("invoke:entry:normal-fallback");
    XrCoreIrKey edge_error = xr_invoke_fixture_key("invoke:entry:edge-error");
    XrCoreIrKey error_owner = xr_invoke_fixture_key("invoke:entry:error-owner");
    XrCoreIrKey error_owner_duplicate =
        xr_invoke_fixture_key("invoke:entry:error-owner-duplicate");
    XrCoreIrKey error_fallback = xr_invoke_fixture_key("invoke:entry:error-fallback");
    XrCoreIrValueInput entry_arguments[] = {
        {.key = condition, .type_id = XR_CORE_TYPE_BOOL},
        {.key = error_value, .type_id = XR_CORE_TYPE_ERROR},
        {.key = seed, .type_id = XR_CORE_TYPE_I64},
    };
    XrCoreIrKey entry_values[] = {condition, error_value, seed};
    XrCoreIrKey construct_operands[] = {seed};
    uint32_t normal_owner_argument_count =
        mutation == XR_INVOKE_FIXTURE_DUPLICATE_NORMAL_OWNER ? 2u : 1u;
    uint32_t error_owner_argument_count =
        mutation == XR_INVOKE_FIXTURE_DUPLICATE_ERROR_OWNER ? 2u : 1u;
    uint32_t normal_owner_transfer_count =
        mutation == XR_INVOKE_FIXTURE_MISSING_NORMAL_OWNER ? 0u : normal_owner_argument_count;
    uint32_t error_owner_transfer_count =
        mutation == XR_INVOKE_FIXTURE_MISSING_ERROR_OWNER ? 0u : error_owner_argument_count;
    XrCoreIrKey invoke_operands[6] = {condition, error_value};
    uint32_t invoke_operand_count = 2u;
    for (uint32_t index = 0u; index < normal_owner_transfer_count; ++index)
        invoke_operands[invoke_operand_count++] = owner;
    for (uint32_t index = 0u; index < error_owner_transfer_count; ++index)
        invoke_operands[invoke_operand_count++] = owner;
    XrCoreIrKey invoke_successors[] = {normal_key, error_key};
    XrCoreIrInstructionInput entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = entry_values,
         .operand_count = 3u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
         .result = owner,
         .result_type_id = XR_INVOKE_FIXTURE_AFFINE_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = construct_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CALL_SEALED_INVOKE,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = invoke_operands,
         .operand_count = invoke_operand_count,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = callee_key,
         .successors = invoke_successors,
         .successor_count = 2u},
    };
    XrCoreIrValueInput normal_arguments[3] = {
        {.key = normal_result,
         .type_id = mutation == XR_INVOKE_FIXTURE_WRONG_NORMAL_RESULT_TYPE
                        ? XR_CORE_TYPE_BOOL
                        : XR_CORE_TYPE_I64},
        {.key = normal_owner,
         .type_id = XR_INVOKE_FIXTURE_AFFINE_TYPE,
         .ownership = XR_CORE_IR_OWNER},
        {.key = normal_owner_duplicate,
         .type_id = XR_INVOKE_FIXTURE_AFFINE_TYPE,
         .ownership = XR_CORE_IR_OWNER},
    };
    XrCoreIrKey normal_block_arguments[] = {normal_result, normal_owner, normal_owner_duplicate};
    XrCoreIrKey normal_drop_operands[][1] = {{normal_owner}, {normal_owner_duplicate}};
    XrCoreIrKey normal_return[] = {
        mutation == XR_INVOKE_FIXTURE_WRONG_NORMAL_RESULT_TYPE ? normal_fallback : normal_result};
    XrCoreIrInstructionInput normal_instructions[5] = {
        (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = normal_block_arguments,
            .operand_count = 1u + normal_owner_argument_count,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        },
    };
    uint32_t normal_instruction_count = 1u;
    for (uint32_t index = 0u; index < normal_owner_argument_count; ++index) {
        normal_instructions[normal_instruction_count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = normal_drop_operands[index],
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
    }
    if (mutation == XR_INVOKE_FIXTURE_WRONG_NORMAL_RESULT_TYPE) {
        normal_instructions[normal_instruction_count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
            .result = normal_fallback,
            .result_type_id = XR_CORE_TYPE_I64,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
            .immediate.key = constant.key,
        };
    }
    normal_instructions[normal_instruction_count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_RETURN,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = normal_return,
        .operand_count = 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };

    XrCoreIrValueInput error_arguments[3] = {
        {.key = edge_error,
         .type_id = mutation == XR_INVOKE_FIXTURE_WRONG_ERROR_TYPE ? XR_CORE_TYPE_BOOL
                                                                   : XR_CORE_TYPE_ERROR},
        {.key = error_owner,
         .type_id = XR_INVOKE_FIXTURE_AFFINE_TYPE,
         .ownership = XR_CORE_IR_OWNER},
        {.key = error_owner_duplicate,
         .type_id = XR_INVOKE_FIXTURE_AFFINE_TYPE,
         .ownership = XR_CORE_IR_OWNER},
    };
    XrCoreIrKey error_block_arguments[] = {edge_error, error_owner, error_owner_duplicate};
    XrCoreIrKey error_drop_operands[][1] = {{error_owner}, {error_owner_duplicate}};
    XrCoreIrKey error_terminal_operand[] = {
        mutation == XR_INVOKE_FIXTURE_WRONG_ERROR_TYPE ? error_fallback : edge_error};
    XrCoreIrInstructionInput error_instructions[5] = {
        (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = error_block_arguments,
            .operand_count = 1u + error_owner_argument_count,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        },
    };
    uint32_t error_instruction_count = 1u;
    for (uint32_t index = 0u; index < error_owner_argument_count; ++index) {
        error_instructions[error_instruction_count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = error_drop_operands[index],
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
    }
    if (mutation == XR_INVOKE_FIXTURE_WRONG_ERROR_TYPE) {
        error_instructions[error_instruction_count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
            .result = error_fallback,
            .result_type_id = XR_CORE_TYPE_I64,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
            .immediate.key = constant.key,
        };
    }
    error_instructions[error_instruction_count++] = (XrCoreIrInstructionInput) {
        .operation_id = mutation == XR_INVOKE_FIXTURE_WRONG_ERROR_TYPE
                            ? XR_CORE_OP_CORE_RETURN
                            : XR_CORE_OP_CORE_ERROR_PUBLISH,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = error_terminal_operand,
        .operand_count = 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    XrCoreIrBlockInput entry_blocks[] = {
        {.key = entry_block_key,
         .arguments = entry_arguments,
         .argument_count = 3u,
         .instructions = entry_instructions,
         .instruction_count = 3u},
        {.key = normal_key,
         .arguments = normal_arguments,
         .argument_count = 1u + normal_owner_argument_count,
         .instructions = normal_instructions,
         .instruction_count = normal_instruction_count},
        {.key = error_key,
         .arguments = error_arguments,
         .argument_count = 1u + error_owner_argument_count,
         .instructions = error_instructions,
         .instruction_count = error_instruction_count},
    };
    uint16_t entry_parameters[] = {XR_CORE_TYPE_BOOL, XR_CORE_TYPE_ERROR, XR_CORE_TYPE_I64};
    XrCoreIrFunctionInput entry_function = {
        .key = entry_key,
        .parameter_types = entry_parameters,
        .parameter_count = 3u,
        .result_type_id = XR_CORE_TYPE_I64,
        .error_type_id = XR_CORE_TYPE_ERROR,
        .effect_mask = XR_CORE_EFFECT_ERROR | XR_CORE_EFFECT_CALL,
        .entry_block = entry_block_key,
        .blocks = entry_blocks,
        .block_count = 3u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrFunctionInput functions[] = {callee_function, entry_function};
    XrCoreIrModuleInput module = {
        .key = xr_invoke_fixture_key("invoke:module"),
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

static XrProgramBuildStatus xr_program_invoke_fixture_write(XrProgramArtifact *artifact,
                                                            char *diagnostic,
                                                            size_t diagnostic_size) {
    return xr_program_invoke_fixture_write_mutated(XR_INVOKE_FIXTURE_VALID, artifact, diagnostic,
                                                   diagnostic_size);
}

#endif /* XR_PROGRAM_INVOKE_FIXTURE_H */
