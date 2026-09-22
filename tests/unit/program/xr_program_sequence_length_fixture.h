#ifndef XR_PROGRAM_SEQUENCE_LENGTH_FIXTURE_H
#define XR_PROGRAM_SEQUENCE_LENGTH_FIXTURE_H

#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"

/* The copy remains live after its source is dropped. Its six Unicode scalars
 * include a supplementary character and a separate combining character. */
static XrProgramBuildStatus xr_program_sequence_length_fixture_write(
    unsigned mutation, XrProgramArtifact *artifact) {
    static const uint8_t text[] = {
        'A', 0xc3, 0xa9, 0xe4, 0xb8, 0x96, 0xf0, 0x9f, 0x98, 0x80, 'e', 0xcc, 0x81};
    XrCoreIrKey identity = xr_core_ir_key("sequence-length", 15u);
    XrCoreIrKey source = xr_core_ir_key("source", 6u);
    XrCoreIrKey copy = xr_core_ir_key("copy", 4u);
    XrCoreIrKey length = xr_core_ir_key("length", 6u);
    XrCoreIrKey number = xr_core_ir_key("number", 6u);
    XrCoreIrConstantInput constants[] = {
        {.key = source, .type_id = XR_CORE_TYPE_STRING, .kind = XR_CORE_IR_CONSTANT_STRING,
         .value.string = {text, sizeof(text)}},
        {.key = number, .type_id = XR_CORE_TYPE_I64, .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 1},
    };
    XrCoreIrKey operand = mutation == 1u ? number : mutation == 5u ? source : copy;
    uint16_t result_type = mutation == 2u ? XR_CORE_TYPE_BOOL : XR_CORE_TYPE_I64;
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64, .result = number,
         .result_type_id = XR_CORE_TYPE_I64, .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = number},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_STRING, .result = source,
         .result_type_id = XR_CORE_TYPE_STRING, .result_ownership = XR_CORE_IR_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = source},
        {.operation_id = XR_CORE_OP_CORE_OWNER_COPY, .result = copy,
         .result_type_id = XR_CORE_TYPE_STRING, .result_ownership = XR_CORE_IR_OWNER,
         .operands = &source, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &source, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_SEQUENCE_LENGTH, .result = length,
         .result_type_id = result_type,
         .result_ownership = mutation == 3u ? XR_CORE_IR_OWNER : XR_CORE_IR_NON_OWNER,
         .immediate_kind = mutation == 4u ? XR_CORE_IR_IMMEDIATE_U32 : XR_CORE_IR_IMMEDIATE_NONE,
         .operands = &operand, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &copy, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN, .operands = &length, .operand_count = 1u},
    };
    if (mutation == 6u) {
        XrCoreIrInstructionInput temporary = instructions[4];
        instructions[4] = instructions[5];
        instructions[5] = temporary;
    }
    XrCoreIrBlockInput block = {.key = identity, .instructions = instructions,
                                .instruction_count = sizeof(instructions) / sizeof(instructions[0])};
    XrCoreIrFunctionInput function = {.key = identity, .result_type_id = result_type,
        .entry_block = identity, .blocks = &block, .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY};
    XrCoreIrModuleInput module = {.key = identity, .constants = constants, .constant_count = 2u,
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

#endif // XR_PROGRAM_SEQUENCE_LENGTH_FIXTURE_H
