/* Shared owned-array comparison fixture; both executors consume the same wire. */
#ifndef XR_PROGRAM_BYTE_COMPARE_FIXTURE_H
#define XR_PROGRAM_BYTE_COMPARE_FIXTURE_H
#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"
#include <stdio.h>
static XrProgramBuildStatus xr_program_byte_compare_fixture_write(
    unsigned scenario, unsigned mutation, XrProgramArtifact *artifact) {
    XrCoreIrKey k[20];
    for (unsigned i = 0; i < 20u; ++i) {
        uint8_t raw[] = {0xb2, (uint8_t)i};
        k[i] = xr_core_ir_key(raw, sizeof(raw));
    }
    uint16_t element = mutation == 1u ? XR_CORE_TYPE_I8 : XR_CORE_TYPE_U8;
    XrCoreIrTypeInput type = {.key = k[19], .local_id = 32u,
        .kind = XR_CORE_IR_TYPE_ARRAY, .array_element_type = element,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE, .copy_contract = XR_CORE_IR_COPY_EXPLICIT};
    XrCoreIrConstantInput constants[6] = {0};
    XrCoreIrInstructionInput ops[18] = {0};
    XrCoreIrKey bytes[6];
    const int64_t input[] = {0, 128, 255};
    for (unsigned i = 0; i < 6u; ++i) {
        int64_t value = input[i % 3u];
        if (scenario >= 2u && scenario <= 4u && i == scenario + 1u) value ^= 1;
        constants[i] = (XrCoreIrConstantInput){.key = k[i], .type_id = XR_CORE_TYPE_I64,
            .kind = XR_CORE_IR_CONSTANT_I64, .value.i64 = value};
        ops[i * 2u] = (XrCoreIrInstructionInput){.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
            .result = k[i], .result_type_id = XR_CORE_TYPE_I64,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = k[i]};
        bytes[i] = k[6u + i];
        ops[i * 2u + 1u] = (XrCoreIrInstructionInput){.operation_id = XR_CORE_OP_CORE_INTEGER_CONVERT,
            .result = bytes[i], .result_type_id = element, .operands = &k[i], .operand_count = 1u};
    }
    uint32_t left_count = scenario == 0u || scenario == 7u ? 0u : 3u;
    uint32_t right_count = scenario == 0u || scenario == 8u ? 0u : scenario == 5u ? 2u : 3u;
    ops[12] = (XrCoreIrInstructionInput){.operation_id = XR_CORE_OP_CORE_ARRAY_CONSTRUCT,
        .result = k[12], .result_type_id = 32u, .result_ownership = XR_CORE_IR_OWNER,
        .operands = left_count ? bytes : NULL, .operand_count = left_count};
    ops[13] = (XrCoreIrInstructionInput){.operation_id = XR_CORE_OP_CORE_ARRAY_CONSTRUCT,
        .result = k[13], .result_type_id = 32u, .result_ownership = XR_CORE_IR_OWNER,
        .operands = right_count ? bytes + 3u : NULL, .operand_count = right_count};
    XrCoreIrKey compared[] = {k[12], scenario == 6u ? k[12] : k[13]};
    if (mutation == 2u) compared[1] = k[0];
    ops[14] = (XrCoreIrInstructionInput){.operation_id = XR_CORE_OP_CORE_BYTES_TIMING_SAFE_EQUAL,
        .result = k[14], .result_type_id = XR_CORE_TYPE_BOOL,
        .result_ownership = mutation == 3u ? XR_CORE_IR_OWNER : XR_CORE_IR_NON_OWNER,
        .operands = compared, .operand_count = mutation == 4u ? 1u : 2u};
    if (mutation == 5u) ops[14].immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
    ops[15] = (XrCoreIrInstructionInput){.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
        .operands = &k[13], .operand_count = 1u};
    ops[16] = (XrCoreIrInstructionInput){.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
        .operands = &k[12], .operand_count = 1u};
    if (mutation == 6u) {
        XrCoreIrInstructionInput temporary = ops[14]; ops[14] = ops[15]; ops[15] = temporary;
    }
    ops[17] = (XrCoreIrInstructionInput){.operation_id = XR_CORE_OP_CORE_RETURN,
        .operands = &k[14], .operand_count = 1u};
    XrCoreIrBlockInput block = {.key = k[15], .instructions = ops, .instruction_count = 18u};
    XrCoreIrFunctionInput function = {.key = k[16], .result_type_id = XR_CORE_TYPE_BOOL,
        .entry_block = k[15], .blocks = &block, .block_count = 1u, .flags = XR_PROGRAM_FUNCTION_ENTRY};
    XrCoreIrModuleInput module = {.key = k[17], .constants = constants, .constant_count = 6u,
        .functions = &function, .function_count = 1u};
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input_program = {.semantic_profile_fingerprint = k[18].bytes,
        .required_features = &feature, .required_feature_count = 1u, .types = &type, .type_count = 1u,
        .modules = &module, .module_count = 1u};
    char diagnostic[256] = {0};
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status = xr_core_ir_program_build(&input_program, &program, diagnostic, sizeof(diagnostic));
    if (status == XR_PROGRAM_BUILD_OK) status = xr_program_write(program, artifact, diagnostic, sizeof(diagnostic));
    if (status != XR_PROGRAM_BUILD_OK) fprintf(stderr, "byte compare scenario=%u mutation=%u status=%u: %s\n", scenario, mutation, (unsigned)status, diagnostic);
    xr_core_ir_program_free(program);
    return status;
}
#endif  // XR_PROGRAM_BYTE_COMPARE_FIXTURE_H
