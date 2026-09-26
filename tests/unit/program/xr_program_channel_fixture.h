/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_channel_fixture.h - Shared channel construction and lifetime fixture
 */
#ifndef XR_PROGRAM_CHANNEL_FIXTURE_H
#define XR_PROGRAM_CHANNEL_FIXTURE_H
#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"
static XrProgramBuildStatus xr_program_channel_fixture_write(
    int64_t capacity, unsigned mutation, XrProgramArtifact *artifact) {
    XrCoreIrKey identity = xr_core_ir_key("channel", 7u);
    XrCoreIrKey initial = xr_core_ir_key("capacity", 8u);
    XrCoreIrKey source = xr_core_ir_key("source", 6u);
    XrCoreIrKey alias = xr_core_ir_key("alias", 5u);
    XrCoreIrKey closed = xr_core_ir_key("closed", 6u);
    XrCoreIrTypeInput type = {.key = identity, .local_id = 90u,
        .kind = XR_CORE_IR_TYPE_CHANNEL, .channel_element_type = XR_CORE_TYPE_STRING,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE, .copy_contract = XR_CORE_IR_COPY_EXPLICIT};
    XrCoreIrConstantInput constant = {.key = initial, .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64, .value.i64 = capacity};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64, .result = initial,
         .result_type_id = XR_CORE_TYPE_I64, .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = initial},
        {.operation_id = XR_CORE_OP_CORE_CHANNEL_CONSTRUCT, .result = source,
         .result_type_id = 90u, .result_ownership = XR_CORE_IR_OWNER,
         .operands = &initial, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_COPY, .result = alias,
         .result_type_id = 90u, .result_ownership = XR_CORE_IR_OWNER,
         .operands = &source, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &source, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_CHANNEL_IS_CLOSED, .result = closed,
         .result_type_id = XR_CORE_TYPE_BOOL, .operands = &alias, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &alias, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN, .operands = &closed, .operand_count = 1u},
    };
    if (mutation == 1u) instructions[1].operands = &alias;
    if (mutation == 2u) instructions[4].operands = &initial;
    if (mutation == 3u) instructions[4].operands = &source;
    XrCoreIrBlockInput block = {.key = identity, .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0])};
    XrCoreIrFunctionInput function = {.key = identity, .result_type_id = XR_CORE_TYPE_BOOL,
        .entry_block = identity, .blocks = &block, .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY};
    XrCoreIrModuleInput module = {.key = identity, .constants = &constant, .constant_count = 1u,
        .functions = &function, .function_count = 1u};
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {.semantic_profile_fingerprint = identity.bytes,
        .required_features = &feature, .required_feature_count = 1u,
        .types = &type, .type_count = 1u, .modules = &module, .module_count = 1u};
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status = xr_core_ir_program_build(&input, &program, NULL, 0u);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, NULL, 0u);
    xr_core_ir_program_free(program);
    return status;
}
#endif
