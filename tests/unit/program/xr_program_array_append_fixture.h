#ifndef XR_PROGRAM_ARRAY_APPEND_FIXTURE_H
#define XR_PROGRAM_ARRAY_APPEND_FIXTURE_H
#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"

static XrProgramBuildStatus xr_program_array_append_fixture_write(unsigned mutation, XrProgramArtifact *artifact) {
    XrCoreIrKey keys[18];
    for (unsigned i = 0u; i < 18u; ++i) {
        uint8_t bytes[] = {0xd3, (uint8_t)i};
        keys[i] = xr_core_ir_key(bytes, sizeof(bytes));
    }
    uint16_t parameter = (mutation == 4u || mutation == 5u) ? XR_CORE_TYPE_STRING : XR_CORE_TYPE_I64;
    XrParamMode mode = mutation == 5u ? XR_PARAM_MOVE : XR_PARAM_READ;
    XrCoreIrTypeInput type = {.key = keys[7], .local_id = XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE,
        .kind = XR_CORE_IR_TYPE_ARRAY, .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .array_element_type = mutation == 1u ? XR_CORE_TYPE_BOOL :
            mutation == 100u ? XR_CORE_TYPE_STRING : parameter};
    XrCoreIrValueInput argument = {.key = keys[0], .type_id = parameter,
        .ownership = mutation == 5u ? XR_CORE_IR_OWNER : XR_CORE_IR_NON_OWNER};
    XrCoreIrKey append_operands[] = {mutation == 2u ? keys[1] : keys[2], keys[0]};
    XrCoreIrInstructionInput instructions[15] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT, .operands = &keys[0], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_ARRAY_CONSTRUCT, .result = keys[1],
         .result_type_id = type.local_id, .result_ownership = XR_CORE_IR_OWNER},
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOCAL, .result = keys[2],
         .result_type_id = type.local_id, .result_category = XR_CORE_IR_PLACE,
         .operands = &keys[1], .operand_count = 1u},
    };
    for (unsigned i = 3u; i < 12u; ++i)
        instructions[i] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_ARRAY_APPEND,
            .result_type_id = mutation == 3u ? XR_CORE_TYPE_I64 : XR_CORE_TYPE_VOID,
            .operands = append_operands, .operand_count = 2u};
    instructions[12] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_SEQUENCE_LENGTH, .result = keys[3],
        .result_type_id = XR_CORE_TYPE_I64, .operands = &keys[1], .operand_count = 1u};
    instructions[13] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &keys[1], .operand_count = 1u};
    instructions[14] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_RETURN, .operands = &keys[3], .operand_count = 1u};
    const uint8_t text[] = {'a', 0, 'b'};
    XrCoreIrConstantInput constant = {.key = keys[8], .type_id = XR_CORE_TYPE_STRING,
        .kind = XR_CORE_IR_CONSTANT_STRING, .value.string = {text, sizeof(text)}};
    XrCoreIrInstructionInput managed[26] = {0};
    XrCoreIrKey managed_arguments[9][2];
    if (mutation == 100u) {
        for (unsigned i = 0u; i < 3u; ++i) managed[i] = instructions[i];
        managed[3] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_CONSTANT_STRING, .result = keys[8],
            .result_type_id = XR_CORE_TYPE_STRING, .result_ownership = XR_CORE_IR_OWNER,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = keys[8]};
        for (unsigned i = 0u; i < 9u; ++i) {
            managed[4u + i * 2u] = (XrCoreIrInstructionInput) {
                .operation_id = XR_CORE_OP_CORE_OWNER_COPY, .result = keys[9u + i],
                .result_type_id = XR_CORE_TYPE_STRING, .result_ownership = XR_CORE_IR_OWNER,
                .operands = &keys[8], .operand_count = 1u};
            managed_arguments[i][0] = keys[2];
            managed_arguments[i][1] = keys[9u + i];
            managed[5u + i * 2u] = (XrCoreIrInstructionInput) {
                .operation_id = XR_CORE_OP_CORE_ARRAY_APPEND,
                .operands = managed_arguments[i], .operand_count = 2u};
        }
        managed[22] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &keys[8], .operand_count = 1u};
        for (unsigned i = 12u; i < 15u; ++i) managed[i + 11u] = instructions[i];
    }
    XrCoreIrBlockInput block = {.key = keys[4], .arguments = &argument, .argument_count = 1u,
        .instructions = mutation == 100u ? managed : instructions,
        .instruction_count = mutation == 100u ? 26u : 15u};
    XrCoreIrFunctionInput function = {.key = keys[5], .parameter_types = &parameter, .parameter_modes = &mode, .parameter_count = 1u,
        .result_type_id = XR_CORE_TYPE_I64, .flags = XR_PROGRAM_FUNCTION_ENTRY,
        .entry_block = keys[4], .blocks = &block, .block_count = 1u};
    XrCoreIrModuleInput module = {.key = keys[6], .functions = &function, .function_count = 1u,
        .constants = mutation == 100u ? &constant : NULL, .constant_count = mutation == 100u ? 1u : 0u};
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {.semantic_profile_fingerprint = keys[6].bytes,
        .required_features = &feature, .required_feature_count = 1u, .modules = &module, .module_count = 1u,
        .types = &type, .type_count = 1u};
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status = xr_core_ir_program_build(&input, &program, NULL, 0u);
    if (status == XR_PROGRAM_BUILD_OK) status = xr_program_write(program, artifact, NULL, 0u);
    xr_core_ir_program_free(program);
    return status;
}
#endif
