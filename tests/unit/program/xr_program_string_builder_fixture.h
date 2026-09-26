/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_string_builder_fixture.h - Mutable text and independent snapshot
 */
#ifndef XR_PROGRAM_STRING_BUILDER_FIXTURE_H
#define XR_PROGRAM_STRING_BUILDER_FIXTURE_H
#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"
#include <string.h>

static XrProgramBuildStatus xr_program_string_builder_fixture_write(
    unsigned mutation, XrProgramArtifact *artifact) {
    XrCoreIrKey identity = xr_core_ir_key("builder", 7u);
    XrCoreIrKey builder = xr_core_ir_key("owner", 5u);
    XrCoreIrKey place = xr_core_ir_key("place", 5u);
    XrCoreIrKey text = xr_core_ir_key("text", 4u);
    XrCoreIrKey borrow = xr_core_ir_key("borrow", 6u);
    XrCoreIrKey snapshot = xr_core_ir_key("snapshot", 8u);
    XrCoreIrKey length = xr_core_ir_key("length", 6u);
    XrCoreIrKey append[] = {place, text};
    const uint8_t bytes[] = {'A', 0, 0xc3, 0xa9, 0xf0, 0x9f, 0x98, 0x80};
    XrCoreIrConstantInput constant = {.key = text, .type_id = XR_CORE_TYPE_STRING,
        .kind = XR_CORE_IR_CONSTANT_STRING, .value.string = {bytes, sizeof(bytes)}};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_STRING_BUILDER_CONSTRUCT, .result = builder,
         .result_type_id = XR_CORE_TYPE_STRING_BUILDER, .result_ownership = XR_CORE_IR_OWNER},
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOCAL, .result = place,
         .result_type_id = XR_CORE_TYPE_STRING_BUILDER, .result_category = XR_CORE_IR_PLACE,
         .operands = &builder, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_STRING, .result = text,
         .result_type_id = XR_CORE_TYPE_STRING, .result_ownership = XR_CORE_IR_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = text},
        {.operation_id = XR_CORE_OP_CORE_STRING_BUILDER_APPEND,
         .operands = append, .operand_count = 2u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &text, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOAD, .result = borrow,
         .result_type_id = XR_CORE_TYPE_STRING_BUILDER, .operands = &place, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_STRING_BUILDER_SNAPSHOT, .result = snapshot,
         .result_type_id = XR_CORE_TYPE_STRING, .result_ownership = XR_CORE_IR_OWNER,
         .operands = &borrow, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_STRING_BUILDER_CLEAR, .operands = &place, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_STRING_BUILDER_LENGTH, .result = length,
         .result_type_id = XR_CORE_TYPE_I64, .operands = &builder, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &builder, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN, .operands = &snapshot, .operand_count = 1u},
    };
    if (mutation == 1u) append[0] = builder;
    if (mutation == 2u) append[1] = builder;
    if (mutation == 3u) {
        instructions[7] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_COPY, .result = length,
            .result_type_id = XR_CORE_TYPE_STRING_BUILDER, .result_ownership = XR_CORE_IR_OWNER,
            .operands = &builder, .operand_count = 1u};
        instructions[8] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &length, .operand_count = 1u};
    }
    if (mutation == 4u) {
        XrCoreIrInstructionInput temporary = instructions[5];
        instructions[5] = instructions[9];
        instructions[9] = temporary;
    }
    if (mutation == 5u) {
        XrCoreIrInstructionInput temporary = instructions[6];
        instructions[6] = instructions[7];
        instructions[7] = temporary;
    }
    if (mutation == 6u) instructions[6].result_ownership = XR_CORE_IR_NON_OWNER;
    if (mutation == 7u) instructions[0].result_ownership = XR_CORE_IR_NON_OWNER;
    if (mutation == 8u) instructions[7].immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
    if (mutation == 9u) instructions[3].result_ownership = XR_CORE_IR_OWNER;
    if (mutation == 10u) instructions[6].operands = &text;
    if (mutation == 101u) constant.value.string.size = 0u;
    XrCoreIrInstructionInput expanded[19];
    if (mutation == 100u) {
        memcpy(expanded, instructions, 4u * sizeof(*instructions));
        for (size_t repeat = 0u; repeat < 8u; ++repeat) expanded[4u + repeat] = instructions[3];
        memcpy(expanded + 12u, instructions + 4u, 7u * sizeof(*instructions));
    }
    XrCoreIrBlockInput block = {.key = identity, .instructions = mutation == 100u ? expanded : instructions,
        .instruction_count = mutation == 100u ? 19u : 11u};
    XrCoreIrFunctionInput function = {.key = identity, .result_type_id = XR_CORE_TYPE_STRING,
        .result_ownership = XR_CORE_IR_OWNER,
        .entry_block = identity, .blocks = &block, .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY};
    XrCoreIrModuleInput module = {.key = identity, .constants = &constant, .constant_count = 1u,
        .functions = &function, .function_count = 1u};
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {.semantic_profile_fingerprint = identity.bytes,
        .required_features = &feature, .required_feature_count = 1u,
        .modules = &module, .module_count = 1u};
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status = xr_core_ir_program_build(&input, &program, NULL, 0u);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, NULL, 0u);
    xr_core_ir_program_free(program);
    return status;
}
static XrProgramBuildStatus xr_program_string_builder_call_fixture_write(
    unsigned mutation, XrProgramArtifact *artifact) {
    XrCoreIrKey keys[8];
    for (unsigned index = 0u; index < 8u; ++index)
        keys[index] = xr_core_ir_key(&index, sizeof(index));
    uint16_t parameters[] = {XR_CORE_TYPE_STRING_BUILDER, XR_CORE_TYPE_STRING_BUILDER};
    XrParamMode modes[] = {XR_PARAM_REF, XR_PARAM_READ};
    XrCoreIrValueInput args[] = {
        {.key = keys[0], .type_id = XR_CORE_TYPE_STRING_BUILDER, .category = XR_CORE_IR_PLACE},
        {.key = keys[1], .type_id = XR_CORE_TYPE_STRING_BUILDER},
    };
    if (mutation == 2u) { modes[0] = XR_PARAM_READ; args[0].category = XR_CORE_IR_VALUE; }
    XrCoreIrInstructionInput helper_ops[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT, .operands = keys, .operand_count = 2u},
        {.operation_id = XR_CORE_OP_CORE_STRING_BUILDER_CLEAR, .operands = keys, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_STRING_BUILDER_LENGTH, .result = keys[2],
         .result_type_id = XR_CORE_TYPE_I64, .operands = &keys[1], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN, .operands = &keys[2], .operand_count = 1u},
    };
    XrCoreIrBlockInput helper_block = {.key = keys[3], .arguments = args, .argument_count = 2u,
        .instructions = helper_ops, .instruction_count = 4u};
    XrCoreIrKey call_args[] = {mutation == 2u ? keys[0] : keys[4], mutation == 1u ? keys[0] : keys[1]};
    XrCoreIrInstructionInput main_ops[] = {
        {.operation_id = XR_CORE_OP_CORE_STRING_BUILDER_CONSTRUCT, .result = keys[0],
         .result_type_id = XR_CORE_TYPE_STRING_BUILDER, .result_ownership = XR_CORE_IR_OWNER},
        {.operation_id = XR_CORE_OP_CORE_STRING_BUILDER_CONSTRUCT, .result = keys[1],
         .result_type_id = XR_CORE_TYPE_STRING_BUILDER, .result_ownership = XR_CORE_IR_OWNER},
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOCAL, .result = keys[4],
         .result_type_id = XR_CORE_TYPE_STRING_BUILDER, .result_category = XR_CORE_IR_PLACE,
         .operands = &keys[0], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_CALL_SEALED_DIRECT, .result = keys[2],
         .result_type_id = XR_CORE_TYPE_I64, .operands = call_args, .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION, .immediate.key = keys[6]},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &keys[1], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &keys[0], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN, .operands = &keys[2], .operand_count = 1u},
    };
    XrCoreIrBlockInput main_block = {.key = keys[5], .instructions = main_ops, .instruction_count = 7u};
    XrCoreIrFunctionInput functions[] = {
        {.key = keys[6], .parameter_types = parameters, .parameter_modes = modes, .parameter_count = 2u,
         .result_type_id = XR_CORE_TYPE_I64, .entry_block = keys[3], .blocks = &helper_block, .block_count = 1u},
        {.key = keys[7], .result_type_id = XR_CORE_TYPE_I64, .effect_mask = XR_CORE_EFFECT_CALL,
         .entry_block = keys[5], .blocks = &main_block, .block_count = 1u, .flags = XR_PROGRAM_FUNCTION_ENTRY},
    };
    XrCoreIrModuleInput module = {.key = keys[7], .functions = functions, .function_count = 2u};
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {.semantic_profile_fingerprint = keys[7].bytes,
        .required_features = &feature, .required_feature_count = 1u, .modules = &module, .module_count = 1u};
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status = xr_core_ir_program_build(&input, &program, NULL, 0u);
    if (status == XR_PROGRAM_BUILD_OK) status = xr_program_write(program, artifact, NULL, 0u);
    xr_core_ir_program_free(program);
    return status;
}
#endif // XR_PROGRAM_STRING_BUILDER_FIXTURE_H
