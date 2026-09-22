#ifndef XR_PROGRAM_ASSERT_FIXTURE_H
#define XR_PROGRAM_ASSERT_FIXTURE_H

#include "../../../src/core/xr_core_spec_gen.h"
#include "../../../src/program/xr_program.h"
#include "../../../src/shared/xr_assertion_plan.h"

#include <string.h>

#define XR_ASSERT_FIXTURE_AFFINE_TYPE XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE

typedef enum XrProgramAssertFixtureMutation {
    XR_ASSERT_FIXTURE_VALID = 0,
    XR_ASSERT_FIXTURE_NON_BOOL_CONDITION,
    XR_ASSERT_FIXTURE_WRONG_PANIC_TYPE,
    XR_ASSERT_FIXTURE_MISSING_OWNER_TRANSFER,
    XR_ASSERT_FIXTURE_CHAINED_CLEANUP,
    XR_ASSERT_FIXTURE_NO_EDGE_WITH_LIVE_OWNER,
    XR_ASSERT_FIXTURE_RESULT_ON_PANIC_EDGE,
    XR_ASSERT_FIXTURE_CHAINED_OWNER_LEAK,
    XR_ASSERT_FIXTURE_CHAINED_RECOVERY,
} XrProgramAssertFixtureMutation;

static XrCoreIrKey xr_assert_fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrProgramBuildStatus
xr_program_panic_point_fixture_write(uint16_t operation_id, XrProgramAssertFixtureMutation mutation,
                                     XrProgramArtifact *artifact, char *diagnostic,
                                     size_t diagnostic_size) {
    bool division = operation_id == XR_CORE_OP_CORE_INTEGER_DIVMOD;
    uint32_t prefix = division ? 2u : 1u;
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
    XrCoreIrKey division_result = xr_assert_fixture_key("assert:value:division-result");
    XrCoreIrKey forwarded_panic = xr_assert_fixture_key("assert:value:forwarded-panic");
    XrCoreIrKey forwarded_owner = xr_assert_fixture_key("assert:value:forwarded-owner");
    XrCoreIrKey cleanup_key = xr_assert_fixture_key("assert:block:cleanup");
    XrCoreIrKey recovered_value = xr_assert_fixture_key("assert:value:recovered");
    uint16_t condition_type = division || mutation == XR_ASSERT_FIXTURE_NON_BOOL_CONDITION
                                  ? XR_CORE_TYPE_I64 : XR_CORE_TYPE_BOOL;

    XrCoreIrValueInput entry_arguments[] = {
        {.key = condition,
         .type_id = condition_type},
        {.key = seed, .type_id = XR_CORE_TYPE_I64},
    };
    XrCoreIrKey entry_argument_values[] = {condition, seed};
    XrCoreIrKey construct_operands[] = {seed};
    XrCoreIrKey assert_operands[3] = {condition, owner};
    if (division) {
        assert_operands[0] = seed;
        assert_operands[1] = condition;
        assert_operands[2] = owner;
    }
    if (mutation == XR_ASSERT_FIXTURE_RESULT_ON_PANIC_EDGE)
        assert_operands[prefix] = division_result;
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
        {.operation_id = operation_id,
         .result = division ? division_result : (XrCoreIrKey) {{0}},
         .result_type_id = division ? XR_CORE_TYPE_I64 : XR_CORE_TYPE_VOID,
         .operands = assert_operands,
         .operand_count = (mutation == XR_ASSERT_FIXTURE_MISSING_OWNER_TRANSFER ||
                           mutation == XR_ASSERT_FIXTURE_NO_EDGE_WITH_LIVE_OWNER) ? prefix : prefix + 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = division ? 0u : XR_ASSERTION_FAILURE_CONDITION_FALSE,
         .successors = mutation == XR_ASSERT_FIXTURE_NO_EDGE_WITH_LIVE_OWNER ? NULL : assert_successor,
         .successor_count = mutation == XR_ASSERT_FIXTURE_NO_EDGE_WITH_LIVE_OWNER ? 0u : 1u},
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
    XrCoreIrValueInput forwarded_arguments[] = {
        {.key = forwarded_panic, .type_id = XR_CORE_TYPE_PANIC_INFO, .ownership = XR_CORE_IR_OWNER},
        {.key = forwarded_owner, .type_id = XR_ASSERT_FIXTURE_AFFINE_TYPE, .ownership = XR_CORE_IR_OWNER},
    };
    XrCoreIrKey forwarded_values[] = {forwarded_panic, forwarded_owner};
    XrCoreIrInstructionInput forwarded_instructions[5] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT, .operands = forwarded_values,
         .operand_count = 2u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &forwarded_values[1],
         .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_PANIC_PUBLISH, .operands = &forwarded_values[0],
         .operand_count = 1u},
    };
    bool chained = mutation == XR_ASSERT_FIXTURE_CHAINED_CLEANUP ||
                   mutation == XR_ASSERT_FIXTURE_CHAINED_OWNER_LEAK ||
                   mutation == XR_ASSERT_FIXTURE_CHAINED_RECOVERY;
    if (mutation == XR_ASSERT_FIXTURE_CHAINED_OWNER_LEAK)
        forwarded_instructions[1] = forwarded_instructions[2];
    if (mutation == XR_ASSERT_FIXTURE_CHAINED_RECOVERY) {
        forwarded_instructions[2].operation_id = XR_CORE_OP_CORE_OWNER_DROP;
        forwarded_instructions[3] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_CONSTANT_I64, .result = recovered_value,
            .result_type_id = XR_CORE_TYPE_I64,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = constant.key,
        };
        forwarded_instructions[4] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_RETURN, .operands = &recovered_value,
            .operand_count = 1u,
        };
    }
    if (chained)
        panic_instructions[1] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_BRANCH,
            .operands = panic_argument_values, .operand_count = 2u,
            .successors = &cleanup_key, .successor_count = 1u,
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
         .instruction_count = chained ? 2u : 3u},
        {.key = cleanup_key, .arguments = forwarded_arguments, .argument_count = 2u,
         .instructions = forwarded_instructions,
         .instruction_count = mutation == XR_ASSERT_FIXTURE_CHAINED_RECOVERY ? 5u :
                              mutation == XR_ASSERT_FIXTURE_CHAINED_OWNER_LEAK ? 2u : 3u},
    };
    uint16_t parameters[] = {
        condition_type,
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
        .block_count = chained ? 3u : mutation == XR_ASSERT_FIXTURE_NO_EDGE_WITH_LIVE_OWNER ? 1u : 2u,
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

static XrProgramBuildStatus
xr_program_assert_fixture_write_mutated(XrProgramAssertFixtureMutation mutation,
                                        XrProgramArtifact *artifact, char *diagnostic,
                                        size_t diagnostic_size) {
    return xr_program_panic_point_fixture_write(XR_CORE_OP_CORE_ASSERT_CONDITION, mutation,
                                                artifact, diagnostic, diagnostic_size);
}

static XrProgramBuildStatus xr_program_assert_fixture_write(XrProgramArtifact *artifact,
                                                            char *diagnostic,
                                                            size_t diagnostic_size) {
    return xr_program_assert_fixture_write_mutated(XR_ASSERT_FIXTURE_VALID, artifact, diagnostic,
                                                   diagnostic_size);
}

/* A message is borrowed by assert and forwarded separately as a live owner.
 * Mutations independently violate its type, control, or cleanup obligation. */
static inline XrProgramBuildStatus xr_program_assert_message_fixture_write(
    unsigned mutation, XrProgramArtifact *artifact) {
    XrCoreIrKey keys[9];
    for (unsigned i = 0u; i < XR_COUNTOF(keys); ++i) {
        uint8_t bytes[] = {0xa5, 0x19, (uint8_t)i};
        keys[i] = xr_core_ir_key(bytes, sizeof(bytes));
    }
    static const uint8_t message[] = {'a', 0, 0xe4, 0xb8, 0xad};
    XrCoreIrConstantInput constant = {.key = keys[0], .type_id = XR_CORE_TYPE_STRING,
        .kind = XR_CORE_IR_CONSTANT_STRING,
        .value.string = {.bytes = message, .size = sizeof(message)}};
    XrCoreIrValueInput parameter = {.key = keys[1], .type_id = XR_CORE_TYPE_BOOL};
    XrCoreIrKey operands[] = {keys[1], mutation == 1u ? keys[1] : keys[2], keys[2]};
    XrCoreIrInstructionInput entry[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT, .operands = &keys[1], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_STRING, .result = keys[2],
         .result_type_id = XR_CORE_TYPE_STRING, .result_ownership = XR_CORE_IR_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = keys[0]},
        {.operation_id = XR_CORE_OP_CORE_ASSERT_CONDITION, .operands = operands,
         .operand_count = mutation == 3u ? 2u : 3u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 1u | XR_CORE_ASSERT_MESSAGE_PRESENT | (mutation == 2u ? 512u : 0u),
         .successors = &keys[4], .successor_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &keys[2], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN},
    };
    XrCoreIrValueInput arguments[] = {
        {.key = keys[5], .type_id = XR_CORE_TYPE_PANIC_INFO, .ownership = XR_CORE_IR_OWNER},
        {.key = keys[6], .type_id = XR_CORE_TYPE_STRING, .ownership = XR_CORE_IR_OWNER},
    };
    XrCoreIrKey argument_values[] = {keys[5], keys[6]};
    XrCoreIrInstructionInput failure[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT, .operands = argument_values, .operand_count = 2u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &keys[6], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_PANIC_PUBLISH, .operands = &keys[5], .operand_count = 1u},
    };
    XrCoreIrBlockInput blocks[] = {
        {.key = keys[3], .arguments = &parameter, .argument_count = 1u,
         .instructions = entry, .instruction_count = XR_COUNTOF(entry)},
        {.key = keys[4], .arguments = arguments, .argument_count = 2u,
         .instructions = failure, .instruction_count = XR_COUNTOF(failure)},
    };
    uint16_t parameter_type = XR_CORE_TYPE_BOOL;
    XrParamMode mode = XR_PARAM_READ;
    XrCoreIrFunctionInput function = {.key = keys[7], .parameter_types = &parameter_type,
        .parameter_modes = &mode, .parameter_count = 1u,
        .panic_type_id = XR_CORE_TYPE_PANIC_INFO, .effect_mask = XR_CORE_EFFECT_PANIC,
        .flags = XR_PROGRAM_FUNCTION_ENTRY, .entry_block = keys[3],
        .blocks = blocks, .block_count = XR_COUNTOF(blocks)};
    XrCoreIrModuleInput module = {.key = keys[8], .constants = &constant, .constant_count = 1u,
        .functions = &function, .function_count = 1u};
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {.semantic_profile_fingerprint = keys[8].bytes,
        .required_features = &feature, .required_feature_count = 1u,
        .modules = &module, .module_count = 1u};
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status = xr_core_ir_program_build(&input, &program, NULL, 0u);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, NULL, 0u);
    xr_core_ir_program_free(program);
    return status;
}

#endif
