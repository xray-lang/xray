#ifndef XR_PROGRAM_INTEGER_FIXTURE_H
#define XR_PROGRAM_INTEGER_FIXTURE_H

#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"
#include "program/xr_validated_program_internal.h"

#include <string.h>
#include <stdio.h>

/* Exact integer identity functions isolate the value ABI from arithmetic. */
static XrProgramBuildStatus xr_program_integer_fixture_write(bool convert, unsigned mutation,
                                                             XrProgramArtifact *artifact) {
    static const uint16_t types[] = {XR_CORE_TYPE_I8,  XR_CORE_TYPE_U8,  XR_CORE_TYPE_I16,
                                     XR_CORE_TYPE_U16, XR_CORE_TYPE_I32, XR_CORE_TYPE_U32,
                                     XR_CORE_TYPE_I64, XR_CORE_TYPE_U64};
    static const char *names[] = {"integer:i8",  "integer:u8",  "integer:i16", "integer:u16",
                                  "integer:i32", "integer:u32", "integer:i64", "integer:u64"};
    XrCoreIrFunctionInput functions[64] = {0};
    XrCoreIrBlockInput blocks[64] = {0};
    XrCoreIrValueInput arguments[64] = {0};
    XrCoreIrInstructionInput instructions[64][3] = {0};
    XrCoreIrKey values[64][2] = {0};
    uint16_t parameters[64] = {0};
    uint32_t count = convert ? 64u : 8u;
    for (uint32_t index = 0u; index < count; ++index) {
        unsigned source = convert ? index / 8u : index;
        unsigned target = convert ? index % 8u : index;
        char name[64];
        (void) snprintf(name, sizeof(name), "%s:%s", names[source], names[target]);
        XrCoreIrKey key = xr_core_ir_key(name, strlen(name));
        (void) snprintf(name, sizeof(name), "%s:%s:converted", names[source], names[target]);
        values[index][0] = key;
        values[index][1] = xr_core_ir_key(name, strlen(name));
        parameters[index] = index == 0u && mutation == 1u ? XR_CORE_TYPE_BOOL : types[source];
        uint16_t result_type = index == 0u && mutation == 2u ? XR_CORE_TYPE_BOOL : types[target];
        arguments[index] = (XrCoreIrValueInput) {.key = key, .type_id = parameters[index]};
        instructions[index][0] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
            .operands = &values[index][0],
            .operand_count = 1u,
        };
        if (convert)
            instructions[index][1] = (XrCoreIrInstructionInput) {
                .operation_id = XR_CORE_OP_CORE_INTEGER_CONVERT,
                .operands = &values[index][0],
                .operand_count = 1u,
                .result = values[index][1],
                .result_type_id = result_type,
                .result_ownership =
                    index == 0u && mutation == 3u ? XR_CORE_IR_OWNER : XR_CORE_IR_NON_OWNER,
                .immediate_kind = index == 0u && mutation == 4u ? XR_CORE_IR_IMMEDIATE_U32
                                                                : XR_CORE_IR_IMMEDIATE_NONE,
            };
        instructions[index][convert ? 2u : 1u] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_RETURN,
            .operands = &values[index][convert ? 1u : 0u],
            .operand_count = 1u,
        };
        blocks[index] = (XrCoreIrBlockInput) {
            .key = key,
            .arguments = &arguments[index],
            .argument_count = 1u,
            .instructions = instructions[index],
            .instruction_count = convert ? 3u : 2u,
        };
        functions[index] = (XrCoreIrFunctionInput) {
            .key = key,
            .parameter_types = &parameters[index],
            .parameter_count = 1u,
            .result_type_id = result_type,
            .entry_block = key,
            .blocks = &blocks[index],
            .block_count = 1u,
            .flags = index == 0u ? XR_PROGRAM_FUNCTION_ENTRY : 0u,
        };
    }
    XrCoreIrKey identity = xr_core_ir_key("exact-integers", 14u);
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrModuleInput module = {.key = identity, .functions = functions, .function_count = count};
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = identity.bytes,
        .required_features = &feature,
        .required_feature_count = 1u,
        .modules = &module,
        .module_count = 1u,
    };
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status = xr_core_ir_program_build(&input, &program, NULL, 0u);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, NULL, 0u);
    xr_core_ir_program_free(program);
    return status;
}

static uint32_t xr_program_integer_fixture_function(const XrValidatedProgram *program,
                                                    uint16_t source_type, uint16_t target_type) {
    for (uint32_t index = 0u; index < program->function_count; ++index)
        if (program->functions[index].result_type_id == target_type &&
            program->functions[index].parameter_types[0] == source_type)
            return index;
    return UINT32_MAX;
}

/* Each integer type has a quotient and remainder entry with an explicit panic ABI. */
static XrProgramBuildStatus xr_program_integer_divmod_fixture_write(unsigned mutation,
                                                                    XrProgramArtifact *artifact) {
    static const uint16_t types[] = {XR_CORE_TYPE_I8, XR_CORE_TYPE_U8, XR_CORE_TYPE_I16,
                                     XR_CORE_TYPE_U16, XR_CORE_TYPE_I32, XR_CORE_TYPE_U32,
                                     XR_CORE_TYPE_I64, XR_CORE_TYPE_U64};
    XrCoreIrFunctionInput functions[16] = {0};
    XrCoreIrBlockInput blocks[16] = {0};
    XrCoreIrValueInput arguments[16][2] = {0};
    XrCoreIrInstructionInput instructions[16][3] = {0};
    XrCoreIrKey values[16][3] = {0};
    uint16_t parameters[16][2] = {0};
    for (uint32_t index = 0u; index < 16u; ++index) {
        uint16_t type = types[index / 2u];
        char name[64];
        (void) snprintf(name, sizeof(name), "integer-divmod:%u", index);
        XrCoreIrKey key = xr_core_ir_key(name, strlen(name));
        for (unsigned value = 0u; value < 3u; ++value) {
            (void) snprintf(name, sizeof(name), "integer-divmod:%u:value:%u", index, value);
            values[index][value] = xr_core_ir_key(name, strlen(name));
        }
        for (unsigned argument = 0u; argument < 2u; ++argument) {
            parameters[index][argument] =
                index == 0u && mutation == argument + 1u ? XR_CORE_TYPE_BOOL : type;
            arguments[index][argument] = (XrCoreIrValueInput) {
                .key = values[index][argument], .type_id = parameters[index][argument],
            };
        }
        instructions[index][0] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
            .operands = values[index], .operand_count = 2u,
        };
        uint16_t result_type = index == 0u && mutation == 3u ? XR_CORE_TYPE_BOOL : type;
        instructions[index][1] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_INTEGER_DIVMOD,
            .operands = values[index],
            .operand_count = index == 0u && mutation == 6u ? 1u : 2u,
            .result = values[index][2], .result_type_id = result_type,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
            .immediate.u32 = index == 0u && mutation == 4u ? 2u : index % 2u,
        };
        instructions[index][2] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_RETURN,
            .operands = &values[index][2], .operand_count = 1u,
        };
        blocks[index] = (XrCoreIrBlockInput) {
            .key = key, .arguments = arguments[index], .argument_count = 2u,
            .instructions = instructions[index], .instruction_count = 3u,
        };
        functions[index] = (XrCoreIrFunctionInput) {
            .key = key, .parameter_types = parameters[index], .parameter_count = 2u,
            .result_type_id = result_type,
            .panic_type_id = index == 0u && mutation == 5u ? XR_CORE_TYPE_VOID
                                                           : XR_CORE_TYPE_PANIC_INFO,
            .effect_mask = XR_CORE_EFFECT_PANIC,
            .entry_block = key, .blocks = &blocks[index], .block_count = 1u,
            .flags = index == 0u ? XR_PROGRAM_FUNCTION_ENTRY : 0u,
        };
    }
    XrCoreIrKey identity = xr_core_ir_key("exact-integer-divmod", 20u);
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrModuleInput module = {.key = identity, .functions = functions, .function_count = 16u};
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = identity.bytes,
        .required_features = &feature, .required_feature_count = 1u,
        .modules = &module, .module_count = 1u,
    };
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status = xr_core_ir_program_build(&input, &program, NULL, 0u);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, NULL, 0u);
    xr_core_ir_program_free(program);
    return status;
}

static uint32_t xr_program_integer_divmod_fixture_function(const XrValidatedProgram *program,
                                                           uint16_t type, unsigned remainder) {
    for (uint32_t index = 0u; index < program->function_count; ++index) {
        const XrValidatedFunction *function = &program->functions[index];
        if (function->result_type_id == type &&
            function->blocks[0].instructions[1].immediate.u32 == remainder)
            return index;
    }
    return UINT32_MAX;
}

static XrProgramBuildStatus xr_program_integer_bitwise_fixture_write(unsigned mutation,
                                                                    XrProgramArtifact *artifact) {
    static const uint16_t types[] = {XR_CORE_TYPE_I8, XR_CORE_TYPE_U8, XR_CORE_TYPE_I16,
                                     XR_CORE_TYPE_U16, XR_CORE_TYPE_I32, XR_CORE_TYPE_U32,
                                     XR_CORE_TYPE_I64, XR_CORE_TYPE_U64};
    XrCoreIrFunctionInput functions[48] = {0};
    XrCoreIrBlockInput blocks[48] = {0};
    XrCoreIrValueInput arguments[48][2] = {0};
    XrCoreIrInstructionInput instructions[48][3] = {0};
    XrCoreIrKey values[48][3] = {0};
    uint16_t parameters[48][2] = {0};
    for (uint32_t index = 0u; index < 48u; ++index) {
        uint16_t type = types[index / 6u];
        char name[64];
        (void) snprintf(name, sizeof(name), "integer-bitwise:%u", index);
        XrCoreIrKey key = xr_core_ir_key(name, strlen(name));
        for (unsigned value = 0u; value < 3u; ++value) {
            (void) snprintf(name, sizeof(name), "integer-bitwise:%u:value:%u", index, value);
            values[index][value] = xr_core_ir_key(name, strlen(name));
        }
        for (unsigned argument = 0u; argument < 2u; ++argument) {
            parameters[index][argument] =
                index == 0u && mutation == argument + 1u ? XR_CORE_TYPE_BOOL : type;
            arguments[index][argument] = (XrCoreIrValueInput) {
                .key = values[index][argument], .type_id = parameters[index][argument],
            };
        }
        instructions[index][0] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
            .operands = values[index], .operand_count = 2u,
        };
        uint16_t result_type = index == 0u && mutation == 3u ? XR_CORE_TYPE_BOOL : type;
        instructions[index][1] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_INTEGER_BITWISE,
            .operands = values[index],
            .operand_count = index == 0u && mutation == 6u ? 1u : (index % 6u == 3u ? 1u : 2u),
            .result = values[index][2], .result_type_id = result_type,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
            .immediate.u32 = index == 0u && mutation == 4u ? 6u : index % 6u,
        };
        instructions[index][2] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_RETURN,
            .operands = &values[index][2], .operand_count = 1u,
        };
        blocks[index] = (XrCoreIrBlockInput) {
            .key = key, .arguments = arguments[index], .argument_count = 2u,
            .instructions = instructions[index], .instruction_count = 3u,
        };
        functions[index] = (XrCoreIrFunctionInput) {
            .key = key, .parameter_types = parameters[index], .parameter_count = 2u,
            .result_type_id = result_type,
            .entry_block = key, .blocks = &blocks[index], .block_count = 1u,
            .flags = index == 0u ? XR_PROGRAM_FUNCTION_ENTRY : 0u,
        };
    }
    XrCoreIrKey identity = xr_core_ir_key("exact-integer-bitwise", 21u);
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrModuleInput module = {.key = identity, .functions = functions, .function_count = 48u};
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = identity.bytes,
        .required_features = &feature, .required_feature_count = 1u,
        .modules = &module, .module_count = 1u,
    };
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status = xr_core_ir_program_build(&input, &program, NULL, 0u);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, NULL, 0u);
    xr_core_ir_program_free(program);
    return status;
}

static uint32_t xr_program_integer_bitwise_fixture_function(const XrValidatedProgram *program,
                                                           uint16_t type, unsigned remainder) {
    for (uint32_t index = 0u; index < program->function_count; ++index) {
        const XrValidatedFunction *function = &program->functions[index];
        if (function->result_type_id == type &&
            function->blocks[0].instructions[1].immediate.u32 == remainder)
            return index;
    }
    return UINT32_MAX;
}

#endif  // XR_PROGRAM_INTEGER_FIXTURE_H
