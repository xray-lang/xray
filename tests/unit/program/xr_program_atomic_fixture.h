/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_atomic_fixture.h - Shared atomic identity and ordered-access fixture
 */
#ifndef XR_PROGRAM_ATOMIC_FIXTURE_H
#define XR_PROGRAM_ATOMIC_FIXTURE_H
#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"

static XrProgramBuildStatus xr_program_atomic_fixture_write_scalar(
    uint16_t scalar, unsigned mutation, unsigned mode, XrProgramArtifact *artifact) {
    XrCoreIrKey identity = xr_core_ir_key("atomic-identity", 15u);
    XrCoreIrKey initial = xr_core_ir_key("initial", 7u);
    XrCoreIrKey replacement = xr_core_ir_key("replacement", 11u);
    XrCoreIrKey source = xr_core_ir_key("source", 6u);
    XrCoreIrKey alias = xr_core_ir_key("alias", 5u);
    XrCoreIrKey previous = xr_core_ir_key("previous", 8u);
    XrCoreIrKey loaded = xr_core_ir_key("loaded", 6u);
    bool boolean = scalar == XR_CORE_TYPE_BOOL;
    bool floating = scalar == XR_CORE_TYPE_F64;
    XrCoreIrTypeInput type = {.key = identity, .local_id = 90u,
        .kind = XR_CORE_IR_TYPE_ATOMIC, .atomic_element_type = scalar,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE, .copy_contract = XR_CORE_IR_COPY_EXPLICIT};
    if (mutation == 1u)
        type.atomic_element_type = boolean ? XR_CORE_TYPE_I64 : XR_CORE_TYPE_BOOL;
    XrCoreIrConstantInput constants[2] = {
        {.key = initial, .type_id = scalar,
         .kind = boolean ? XR_CORE_IR_CONSTANT_BOOL : XR_CORE_IR_CONSTANT_I64},
        {.key = replacement, .type_id = scalar,
         .kind = boolean ? XR_CORE_IR_CONSTANT_BOOL : XR_CORE_IR_CONSTANT_I64},
    };
    if (boolean) {
        constants[0].value.boolean = false;
        constants[1].value.boolean = true;
    } else {
        constants[0].value.i64 = 40;
        constants[1].value.i64 = 42;
    }
    XrCoreIrKey exchange_operands[2] = {source, replacement};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = boolean ? XR_CORE_OP_CORE_CONSTANT_BOOL : XR_CORE_OP_CORE_CONSTANT_I64,
         .result = initial, .result_type_id = scalar,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = initial},
        {.operation_id = boolean ? XR_CORE_OP_CORE_CONSTANT_BOOL : XR_CORE_OP_CORE_CONSTANT_I64,
         .result = replacement, .result_type_id = scalar,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = replacement},
        {.operation_id = XR_CORE_OP_CORE_ATOMIC_CONSTRUCT, .result = source,
         .result_type_id = 90u, .result_ownership = XR_CORE_IR_OWNER,
         .operands = &initial, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_COPY, .result = alias,
         .result_type_id = 90u, .result_ownership = XR_CORE_IR_OWNER,
         .operands = &source, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_ATOMIC_EXCHANGE, .result = previous,
         .result_type_id = scalar, .operands = exchange_operands, .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32, .immediate.u32 = 4u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &source, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_ATOMIC_LOAD, .result = loaded, .result_type_id = scalar,
         .operands = &alias, .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32, .immediate.u32 = 4u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &alias, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN, .operands = &loaded, .operand_count = 1u},
    };
    XrCoreIrKey compare_operands[3] = {source, mode == 2u || mode == 4u ? replacement : initial, replacement};
    if (mode && mode <= 4u) {
        instructions[4].operation_id = XR_CORE_OP_CORE_ATOMIC_COMPARE_EXCHANGE;
        instructions[4].operands = compare_operands;
        instructions[4].operand_count = 3u;
        if (mode >= 3u) instructions[8].operands = &previous;
    }
    if (mode >= 5u) {
        if (!boolean) constants[1].value.i64 = 2;
        instructions[4].operation_id = XR_CORE_OP_CORE_ATOMIC_UPDATE;
        instructions[4].immediate.u32 = ((mode - 5u) % 3u) * 8u + 4u;
        if (mode >= 8u) instructions[8].operands = &previous;
    }
    if (floating) {
        for (unsigned index = 0u; index < 2u; ++index) {
            constants[index].kind = XR_CORE_IR_CONSTANT_F64;
            constants[index].value.f64_bits = index == 0u ? UINT64_C(0x4044000000000000) :
                mode >= 5u ? UINT64_C(0x4000000000000000) : UINT64_C(0x4045000000000000);
            instructions[index].operation_id = XR_CORE_OP_CORE_CONSTANT_F64;
        }
    }
    if (mutation == 2u) instructions[6].immediate.u32 = 5u;
    if (mutation == 3u) instructions[6].result_ownership = XR_CORE_IR_OWNER;
    if (mutation == 4u) {
        XrCoreIrInstructionInput temp = instructions[6];
        instructions[6] = instructions[7];
        instructions[7] = temp;
    }
    if (mutation == 5u) instructions[4].immediate.u32 = UINT32_MAX;
    if (mutation == 6u) instructions[2].result_category = XR_CORE_IR_PLACE;
    if (mutation == 7u) instructions[4].immediate.u32 = floating ? 20u : boolean ? 4u : 28u;
    XrCoreIrBlockInput block = {.key = identity, .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0])};
    XrCoreIrFunctionInput function = {.key = identity, .result_type_id = scalar,
        .entry_block = identity, .blocks = &block, .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY};
    XrCoreIrModuleInput module = {.key = identity, .constants = constants, .constant_count = 2u,
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
static XrProgramBuildStatus xr_program_atomic_fixture_write_mode(
    bool boolean, unsigned mutation, unsigned mode, XrProgramArtifact *artifact) {
    return xr_program_atomic_fixture_write_scalar(boolean ? XR_CORE_TYPE_BOOL : XR_CORE_TYPE_I64,
                                                   mutation, mode, artifact);
}
static XrProgramBuildStatus xr_program_atomic_fixture_write(
    bool boolean, unsigned mutation, XrProgramArtifact *artifact) {
    return xr_program_atomic_fixture_write_mode(boolean, mutation, 0u, artifact);
}
#endif // XR_PROGRAM_ATOMIC_FIXTURE_H
