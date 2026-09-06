#ifndef XR_PROGRAM_ASSERT_FIXTURE_H
#define XR_PROGRAM_ASSERT_FIXTURE_H

#include "../../../src/core/xr_core_spec_gen.h"
#include "../../../src/program/xr_program.h"
#include "../../../src/shared/xr_assertion_plan.h"

#include <string.h>

#define XR_ASSERT_FIXTURE_AFFINE_TYPE UINT16_C(16)

typedef enum XrProgramAssertFixtureMutation {
    XR_ASSERT_FIXTURE_VALID = 0,
    XR_ASSERT_FIXTURE_NON_BOOL_CONDITION,
    XR_ASSERT_FIXTURE_WRONG_PANIC_TYPE,
    XR_ASSERT_FIXTURE_MISSING_OWNER_TRANSFER,
} XrProgramAssertFixtureMutation;

static XrCoreIrKey xr_assert_fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrProgramBuildStatus
xr_program_assert_fixture_write_mutated(XrProgramAssertFixtureMutation mutation,
                                        XrProgramArtifact *artifact, char *diagnostic,
                                        size_t diagnostic_size) {
    uint16_t affine_fields[] = {XR_CORE_TYPE_I64};
    XrCoreIrTypeInput affine_type = {
        .key = xr_assert_fixture_key("assert:type:affine"),
        .local_id = XR_ASSERT_FIXTURE_AFFINE_TYPE,
        .kind = XR_CORE_IR_TYPE_AGGREGATE,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .field_types = affine_fields,
        .field_count = 1u,
    };
    XrCoreIrConstantInput constant = {
        .key = xr_assert_fixture_key("assert:constant:42"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 42,
    };

    XrCoreIrKey entry_key = xr_assert_fixture_key("assert:function");
    XrCoreIrKey entry_block_key = xr_assert_fixture_key("assert:block:entry");
    XrCoreIrKey panic_block_key = xr_assert_fixture_key("assert:block:panic");
    XrCoreIrKey condition = xr_assert_fixture_key("assert:value:condition");
    XrCoreIrKey seed = xr_assert_fixture_key("assert:value:seed");
    XrCoreIrKey owner = xr_assert_fixture_key("assert:value:owner");
    XrCoreIrKey result = xr_assert_fixture_key("assert:value:result");
    XrCoreIrKey panic = xr_assert_fixture_key("assert:value:panic");
    XrCoreIrKey panic_owner = xr_assert_fixture_key("assert:value:panic-owner");

    XrCoreIrValueInput entry_arguments[] = {
        {.key = condition,
         .type_id = mutation == XR_ASSERT_FIXTURE_NON_BOOL_CONDITION ? XR_CORE_TYPE_I64
                                                                     : XR_CORE_TYPE_BOOL},
        {.key = seed, .type_id = XR_CORE_TYPE_I64},
    };
    XrCoreIrKey entry_argument_values[] = {condition, seed};
    XrCoreIrKey construct_operands[] = {seed};
    XrCoreIrKey assert_operands[] = {condition, owner};
    XrCoreIrKey assert_successor[] = {panic_block_key};
    XrCoreIrKey drop_owner[] = {owner};
    XrCoreIrKey return_result[] = {result};
    XrCoreIrInstructionInput entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = entry_argument_values,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
         .result = owner,
         .result_type_id = XR_ASSERT_FIXTURE_AFFINE_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = construct_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_ASSERT_CONDITION,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = assert_operands,
         .operand_count = mutation == XR_ASSERT_FIXTURE_MISSING_OWNER_TRANSFER ? 1u : 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = XR_ASSERTION_FAILURE_CONDITION_FALSE,
         .successors = assert_successor,
         .successor_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = drop_owner,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = result,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant.key},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = return_result,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };

    XrCoreIrValueInput panic_arguments[] = {
        {.key = panic,
         .type_id = mutation == XR_ASSERT_FIXTURE_WRONG_PANIC_TYPE ? XR_CORE_TYPE_ERROR
                                                                   : XR_CORE_TYPE_PANIC_INFO,
         .ownership = mutation == XR_ASSERT_FIXTURE_WRONG_PANIC_TYPE ? XR_CORE_IR_NON_OWNER
                                                                     : XR_CORE_IR_OWNER},
        {.key = panic_owner,
         .type_id = XR_ASSERT_FIXTURE_AFFINE_TYPE,
         .ownership = XR_CORE_IR_OWNER},
    };
    XrCoreIrKey panic_argument_values[] = {panic, panic_owner};
    XrCoreIrKey drop_panic_owner[] = {panic_owner};
    XrCoreIrKey publish_panic[] = {panic};
    XrCoreIrInstructionInput panic_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = panic_argument_values,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = drop_panic_owner,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_PANIC_PUBLISH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = publish_panic,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput blocks[] = {
        {.key = entry_block_key,
         .arguments = entry_arguments,
         .argument_count = 2u,
         .instructions = entry_instructions,
         .instruction_count = 6u},
        {.key = panic_block_key,
         .arguments = panic_arguments,
         .argument_count = 2u,
         .instructions = panic_instructions,
         .instruction_count = 3u},
    };
    uint16_t parameters[] = {
        mutation == XR_ASSERT_FIXTURE_NON_BOOL_CONDITION ? XR_CORE_TYPE_I64 : XR_CORE_TYPE_BOOL,
        XR_CORE_TYPE_I64,
    };
    XrParamMode modes[] = {XR_PARAM_READ, XR_PARAM_READ};
    XrCoreIrFunctionInput function = {
        .key = entry_key,
        .parameter_types = parameters,
        .parameter_modes = modes,
        .parameter_count = 2u,
        .result_type_id = XR_CORE_TYPE_I64,
        .panic_type_id = XR_CORE_TYPE_PANIC_INFO,
        .effect_mask = XR_CORE_EFFECT_PANIC,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
        .entry_block = entry_block_key,
        .blocks = blocks,
        .block_count = 2u,
    };
    XrCoreIrModuleInput module = {
        .key = xr_assert_fixture_key("assert:module"),
        .constants = &constant,
        .constant_count = 1u,
        .functions = &function,
        .function_count = 1u,
    };
    XrCoreIrKey semantic = xr_assert_fixture_key("assert:semantic-profile");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = semantic.bytes,
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

static XrProgramBuildStatus xr_program_assert_fixture_write(XrProgramArtifact *artifact,
                                                            char *diagnostic,
                                                            size_t diagnostic_size) {
    return xr_program_assert_fixture_write_mutated(XR_ASSERT_FIXTURE_VALID, artifact, diagnostic,
                                                   diagnostic_size);
}

#endif
