/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_f64_fixture.h - Independent binary64 comparison observations
 */
#ifndef XR_PROGRAM_F64_FIXTURE_H
#define XR_PROGRAM_F64_FIXTURE_H
#include "xr_program_module_fixture.h"

static XrProgramBuildStatus xr_program_f64_fixture_write(unsigned mutation,
                                                         XrProgramArtifact *artifact) {
    static const uint64_t bits[] = {UINT64_C(0), UINT64_C(0x8000000000000000),
        UINT64_C(0x3ff0000000000000), UINT64_C(0x4000000000000000),
        UINT64_C(0xfff0000000000000), UINT64_C(0x7ff0000000000000),
        UINT64_C(0x7ff8000000000001), UINT64_C(0x7ff0000000000001)};
    static const uint8_t pairs[6][2] = {{0,1},{2,3},{4,5},{6,2},{2,6},{7,7}};
    static const bool expected[6][6] = {
        {true,false,false,true,false,true}, {false,true,true,true,false,false},
        {false,true,true,true,false,false}, {false,true,false,false,false,false},
        {false,true,false,false,false,false}, {false,true,false,false,false,false},
    };
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    XrCoreIrConstantInput constants[16];
    XrCoreIrInstructionInput instructions[256] = {0};
    XrCoreIrKey operands[256][3] = {0};
    uint32_t count = 0u;
    for (uint8_t index = 0u; index < 8u; ++index) {
        constants[index] = (XrCoreIrConstantInput) {
            .key = xr_core_ir_key(&index, 1u), .type_id = XR_CORE_TYPE_F64,
            .kind = XR_CORE_IR_CONSTANT_F64, .value.f64_bits = bits[index],
        };
        instructions[count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_CONSTANT_F64, .result = constants[index].key,
            .result_type_id = XR_CORE_TYPE_F64, .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
            .immediate.key = constants[index].key,
        };
    }
    XrCoreIrKey accumulated = {0};
    for (uint32_t pair = 0u; pair < 6u; ++pair) {
        for (uint32_t predicate = 0u; predicate < 6u; ++predicate) {
            uint8_t id = (uint8_t) count;
            operands[count][0] = constants[pairs[pair][0]].key;
            operands[count][1] = constants[pairs[pair][1]].key;
            XrCoreIrKey comparison = xr_core_ir_key(&id, 1u);
            instructions[count] = (XrCoreIrInstructionInput) {
                .operation_id = XR_CORE_OP_CORE_COMPARE_F64, .result = comparison,
                .result_type_id = XR_CORE_TYPE_BOOL, .operands = operands[count], .operand_count = 2u,
                .immediate_kind = XR_CORE_IR_IMMEDIATE_U32, .immediate.u32 = predicate,
            };
            ++count;
            if (!expected[pair][predicate]) {
                id = (uint8_t) count;
                operands[count][0] = comparison;
                comparison = xr_core_ir_key(&id, 1u);
                instructions[count] = (XrCoreIrInstructionInput) {
                    .operation_id = XR_CORE_OP_CORE_LOGICAL_NOT, .result = comparison,
                    .result_type_id = XR_CORE_TYPE_BOOL, .operands = operands[count], .operand_count = 1u,
                };
                ++count;
            }
            if (pair || predicate) {
                id = (uint8_t) count;
                operands[count][0] = accumulated;
                operands[count][1] = comparison;
                comparison = xr_core_ir_key(&id, 1u);
                instructions[count] = (XrCoreIrInstructionInput) {
                    .operation_id = XR_CORE_OP_CORE_LOGICAL_AND, .result = comparison,
                    .result_type_id = XR_CORE_TYPE_BOOL, .operands = operands[count], .operand_count = 2u,
                };
                ++count;
            }
            accumulated = comparison;
        }
    }
    uint32_t first_bitcast = count + 1u;
    for (uint8_t index = 0u; index < 8u; ++index) {
        uint8_t id = (uint8_t) count;
        int64_t expected_bits;
        memcpy(&expected_bits, &bits[index], sizeof(expected_bits));
        constants[8u + index] = (XrCoreIrConstantInput) {
            .key = xr_core_ir_key(&id, 1u), .type_id = XR_CORE_TYPE_I64,
            .kind = XR_CORE_IR_CONSTANT_I64, .value.i64 = expected_bits,
        };
        instructions[count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_CONSTANT_I64, .result = constants[8u + index].key,
            .result_type_id = XR_CORE_TYPE_I64, .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
            .immediate.key = constants[8u + index].key,
        };
        XrCoreIrKey previous = constants[index].key;
        const uint16_t targets[] = {XR_CORE_TYPE_I64, XR_CORE_TYPE_U64, XR_CORE_TYPE_F64, XR_CORE_TYPE_I64};
        for (uint32_t step = 0u; step < 4u; ++step) {
            id = (uint8_t) count;
            operands[count][0] = previous;
            previous = xr_core_ir_key(&id, 1u);
            instructions[count] = (XrCoreIrInstructionInput) {
                .operation_id = XR_CORE_OP_CORE_SCALAR_BITCAST64, .result = previous,
                .result_type_id = targets[step], .operands = operands[count], .operand_count = 1u,
            };
            ++count;
        }
        id = (uint8_t) count;
        operands[count][0] = previous;
        operands[count][1] = constants[8u + index].key;
        XrCoreIrKey comparison = xr_core_ir_key(&id, 1u);
        instructions[count] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_COMPARE_I64, .result = comparison,
            .result_type_id = XR_CORE_TYPE_BOOL, .operands = operands[count], .operand_count = 2u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_U32, .immediate.u32 = 0u,
        };
        ++count;
        id = (uint8_t) count;
        operands[count][0] = accumulated;
        operands[count][1] = comparison;
        accumulated = xr_core_ir_key(&id, 1u);
        instructions[count] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_LOGICAL_AND, .result = accumulated,
            .result_type_id = XR_CORE_TYPE_BOOL, .operands = operands[count], .operand_count = 2u,
        };
        ++count;
    }
    XrCoreIrTypeInput atomic_type = {
        .key = xr_core_ir_key("float-atomic", 12u), .local_id = 90u,
        .kind = XR_CORE_IR_TYPE_ATOMIC, .atomic_element_type = XR_CORE_TYPE_F64,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE, .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
    };
    fixture.input.types = &atomic_type;
    fixture.input.type_count = 1u;
    static const uint8_t cas_cases[6][3] = {{6,6,2}, {6,7,6}, {7,7,2}, {1,0,1}, {0,1,0}, {1,1,2}};
    for (uint32_t test = 0u; test < 6u; ++test) {
        uint8_t id = (uint8_t) count;
        XrCoreIrKey handle = xr_core_ir_key(&id, 1u);
        operands[count][0] = constants[cas_cases[test][0]].key;
        instructions[count] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_ATOMIC_CONSTRUCT, .result = handle,
            .result_type_id = 90u, .result_ownership = XR_CORE_IR_OWNER,
            .operands = operands[count], .operand_count = 1u,
        };
        ++count;
        for (uint32_t observation = 0u; observation < 2u; ++observation) {
            id = (uint8_t) count;
            XrCoreIrKey returned = xr_core_ir_key(&id, 1u);
            operands[count][0] = handle;
            operands[count][1] = constants[cas_cases[test][1]].key;
            operands[count][2] = constants[2].key;
            instructions[count] = (XrCoreIrInstructionInput) {
                .operation_id = observation ? XR_CORE_OP_CORE_ATOMIC_LOAD : XR_CORE_OP_CORE_ATOMIC_COMPARE_EXCHANGE,
                .result = returned, .result_type_id = XR_CORE_TYPE_F64,
                .operands = operands[count], .operand_count = observation ? 1u : 3u,
                .immediate_kind = XR_CORE_IR_IMMEDIATE_U32, .immediate.u32 = 4u,
            };
            ++count;
            id = (uint8_t) count;
            XrCoreIrKey observed = xr_core_ir_key(&id, 1u);
            operands[count][0] = returned;
            instructions[count] = (XrCoreIrInstructionInput) {
                .operation_id = XR_CORE_OP_CORE_SCALAR_BITCAST64, .result = observed,
                .result_type_id = XR_CORE_TYPE_I64, .operands = operands[count], .operand_count = 1u,
            };
            ++count;
            id = (uint8_t) count;
            XrCoreIrKey comparison = xr_core_ir_key(&id, 1u);
            operands[count][0] = observed;
            operands[count][1] = constants[8u + cas_cases[test][observation ? 2u : 0u]].key;
            instructions[count] = (XrCoreIrInstructionInput) {
                .operation_id = XR_CORE_OP_CORE_COMPARE_I64, .result = comparison,
                .result_type_id = XR_CORE_TYPE_BOOL, .operands = operands[count], .operand_count = 2u,
                .immediate_kind = XR_CORE_IR_IMMEDIATE_U32, .immediate.u32 = 0u,
            };
            ++count;
            id = (uint8_t) count;
            operands[count][0] = accumulated;
            operands[count][1] = comparison;
            accumulated = xr_core_ir_key(&id, 1u);
            instructions[count] = (XrCoreIrInstructionInput) {
                .operation_id = XR_CORE_OP_CORE_LOGICAL_AND, .result = accumulated,
                .result_type_id = XR_CORE_TYPE_BOOL, .operands = operands[count], .operand_count = 2u,
            };
            ++count;
        }
        operands[count][0] = handle;
        instructions[count] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = operands[count], .operand_count = 1u,
        };
        ++count;
    }
    operands[count][0] = accumulated;
    instructions[count] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_RETURN, .operands = operands[count], .operand_count = 1u,
    };
    ++count;
    if (mutation == 1u) instructions[8].immediate.u32 = 6u;
    if (mutation == 2u) instructions[8].result_type_id = XR_CORE_TYPE_F64;
    if (mutation == 3u) instructions[0].operation_id = XR_CORE_OP_CORE_CONSTANT_I64;
    if (mutation == 4u) instructions[first_bitcast].result_type_id = XR_CORE_TYPE_I32;
    if (mutation == 5u) instructions[first_bitcast].immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
    fixture.functions[3].result_type_id = XR_CORE_TYPE_BOOL;
    fixture.modules[3].initializer = (XrCoreIrKey){0};
    fixture.modules[3].dependencies = NULL;
    fixture.modules[3].dependency_count = 0u;
    fixture.input.modules = &fixture.modules[3];
    fixture.input.module_count = 1u;
    fixture.modules[3].constants = constants;
    fixture.modules[3].constant_count = 16u;
    fixture.blocks[3].instructions = instructions;
    fixture.blocks[3].instruction_count = count;
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status = xr_core_ir_program_build(&fixture.input, &program, NULL, 0u);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, NULL, 0u);
    xr_core_ir_program_free(program);
    return status;
}
#endif
