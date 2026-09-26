/* A slice result outlives its source; the panic edge releases that source. */
#ifndef XR_PROGRAM_STRING_SLICE_FIXTURE_H
#define XR_PROGRAM_STRING_SLICE_FIXTURE_H

#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"
#include <string.h>

static XrProgramBuildStatus xr_program_string_slice_fixture_write(unsigned mutation,
                                                                  XrProgramArtifact *artifact) {
#define SLICE_KEY(text) xr_core_ir_key(text, sizeof(text) - 1u)
    static const uint8_t text[] = {0x41, 0xc3, 0xa9, 0xe4, 0xb8, 0xad, 0xf0, 0x9f, 0x99, 0x82};
    XrCoreIrConstantInput constant = {
        .key = SLICE_KEY("slice:text"), .type_id = XR_CORE_TYPE_STRING,
        .kind = XR_CORE_IR_CONSTANT_STRING, .value.string = {text, sizeof(text)},
    };
    XrCoreIrKey entry = SLICE_KEY("slice:entry"), handler = SLICE_KEY("slice:handler");
    XrCoreIrKey values[] = {SLICE_KEY("slice:source"), SLICE_KEY("slice:start"),
        SLICE_KEY("slice:end"), SLICE_KEY("slice:result")};
    XrCoreIrKey panic_values[] = {SLICE_KEY("slice:panic"), SLICE_KEY("slice:cleanup")};
    uint16_t parameters[] = {XR_CORE_TYPE_I64, mutation == 1u ? XR_CORE_TYPE_BOOL : XR_CORE_TYPE_I64};
    XrCoreIrValueInput arguments[] = {
        {.key = values[1], .type_id = parameters[0]},
        {.key = values[2], .type_id = parameters[1]},
    };
    XrCoreIrKey slice_operands[] = {values[0], values[1], values[2], values[0]};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT, .operands = &values[1], .operand_count = 2u},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_STRING, .result = values[0],
         .result_type_id = XR_CORE_TYPE_STRING, .result_ownership = XR_CORE_IR_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = constant.key},
        {.operation_id = XR_CORE_OP_CORE_STRING_SLICE, .result = values[3],
         .result_type_id = XR_CORE_TYPE_STRING,
         .result_ownership = mutation == 2u ? XR_CORE_IR_NON_OWNER : XR_CORE_IR_OWNER,
         .operands = slice_operands, .operand_count = mutation == 3u ? 3u : 4u,
         .successors = &handler, .successor_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = values, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN, .operands = &values[3], .operand_count = 1u},
    };
    XrCoreIrValueInput panic_arguments[] = {
        {.key = panic_values[0], .type_id = XR_CORE_TYPE_PANIC_INFO, .ownership = XR_CORE_IR_OWNER},
        {.key = panic_values[1], .type_id = XR_CORE_TYPE_STRING, .ownership = XR_CORE_IR_OWNER},
    };
    XrCoreIrInstructionInput cleanup[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT, .operands = panic_values, .operand_count = 2u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &panic_values[1], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_PANIC_PUBLISH, .operands = panic_values, .operand_count = 1u},
    };
    if (mutation == 4u) cleanup[1] = cleanup[2];
    XrCoreIrBlockInput blocks[] = {
        {.key = entry, .arguments = arguments, .argument_count = 2u,
         .instructions = instructions, .instruction_count = 5u},
        {.key = handler, .arguments = panic_arguments, .argument_count = 2u,
         .instructions = cleanup, .instruction_count = mutation == 4u ? 2u : 3u},
    };
    XrCoreIrFunctionInput function = {
        .key = SLICE_KEY("slice:function"), .parameter_types = parameters, .parameter_count = 2u,
        .result_type_id = XR_CORE_TYPE_STRING, .result_ownership = XR_CORE_IR_OWNER,
        .panic_type_id = XR_CORE_TYPE_PANIC_INFO, .effect_mask = XR_CORE_EFFECT_PANIC,
        .flags = XR_PROGRAM_FUNCTION_ENTRY, .entry_block = entry, .blocks = blocks, .block_count = 2u,
    };
    XrCoreIrModuleInput module = {.key = SLICE_KEY("slice:module"), .constants = &constant,
        .constant_count = 1u, .functions = &function, .function_count = 1u};
    XrCoreIrKey semantic = SLICE_KEY("slice:semantics");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {.semantic_profile_fingerprint = semantic.bytes,
        .required_features = &feature, .required_feature_count = 1u,
        .modules = &module, .module_count = 1u};
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status = xr_core_ir_program_build(&input, &program, NULL, 0u);
    if (status == XR_PROGRAM_BUILD_OK) status = xr_program_write(program, artifact, NULL, 0u);
    xr_core_ir_program_free(program);
    return status;
#undef SLICE_KEY
}

#endif
