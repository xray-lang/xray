#ifndef XR_PROGRAM_ARRAY_DEFAULT_FIXTURE_H
#define XR_PROGRAM_ARRAY_DEFAULT_FIXTURE_H
#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"

static XrProgramBuildStatus xr_program_array_default_fixture_write(unsigned mutation, XrProgramArtifact *artifact) {
    XrCoreIrKey keys[8];
    for (unsigned i = 0; i < 8; ++i) {
        uint8_t bytes[] = {0xd2, (uint8_t)i};
        keys[i] = xr_core_ir_key(bytes, sizeof(bytes));
    }
    uint16_t parameter = mutation == 1u ? XR_CORE_TYPE_BOOL : XR_CORE_TYPE_I64;
    XrCoreIrTypeInput type = {.key = keys[7], .local_id = XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE,
        .kind = XR_CORE_IR_TYPE_ARRAY, .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .array_element_type = mutation == 2u ? XR_CORE_TYPE_STRING : XR_CORE_TYPE_U8};
    XrCoreIrValueInput argument = {.key = keys[0], .type_id = parameter};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT, .operands = &keys[0], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_ARRAY_ALLOCATE_DEFAULT, .result = keys[1],
         .result_type_id = type.local_id, .result_ownership = mutation == 3u ? XR_CORE_IR_NON_OWNER : XR_CORE_IR_OWNER,
         .operands = &keys[0], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_SEQUENCE_LENGTH, .result = keys[2], .result_type_id = XR_CORE_TYPE_I64,
         .operands = &keys[1], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &keys[1], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN, .operands = &keys[2], .operand_count = 1u}
    };
    XrCoreIrBlockInput block = {.key = keys[3], .arguments = &argument, .argument_count = 1u,
        .instructions = instructions, .instruction_count = 5u};
    XrCoreIrFunctionInput function = {.key = keys[4], .parameter_types = &parameter, .parameter_count = 1u,
        .result_type_id = XR_CORE_TYPE_I64, .panic_type_id = XR_CORE_TYPE_PANIC_INFO,
        .effect_mask = XR_CORE_EFFECT_PANIC, .flags = XR_PROGRAM_FUNCTION_ENTRY,
        .entry_block = keys[3], .blocks = &block, .block_count = 1u};
    XrCoreIrModuleInput module = {.key = keys[5],
        .functions = &function, .function_count = 1u};
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
