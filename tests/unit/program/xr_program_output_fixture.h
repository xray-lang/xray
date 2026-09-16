#ifndef XR_PROGRAM_OUTPUT_FIXTURE_H
#define XR_PROGRAM_OUTPUT_FIXTURE_H

#include "core/xr_core_spec_gen.h"
#include "plan/semantic/xr_semantic_ids.h"
#include "program/xr_program.h"
#include "xr_program_provider_fixture.h"
#include "runtime/abi/xr_builtin_provider_contract.h"

#include <string.h>

typedef enum XrProgramOutputFixtureMutation {
    XR_PROGRAM_OUTPUT_FIXTURE_VALID = 0,
    XR_PROGRAM_OUTPUT_FIXTURE_BOOL_OPERAND,
} XrProgramOutputFixtureMutation;

static XrCoreIrKey xr_program_output_fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrProgramBuildStatus xr_program_output_fixture_write_mutated(
    int64_t value, XrProgramOutputFixtureMutation mutation, XrProgramArtifact *artifact,
    char *diagnostic, size_t diagnostic_size) {
    XrStableId contract_id = {{0}};
    XrStableId operation_id = {{0}};
    XrFingerprint key_digest;
    if (!xr_stable_id_from_key(XR_PROVIDER_IO_CONTRACT_KEY, &contract_id, &key_digest) ||
        !xr_stable_id_from_key(XR_PROVIDER_IO_OUTPUT_WRITE_OPERATION_KEY, &operation_id,
                               &key_digest))
        return XR_PROGRAM_BUILD_INVALID_INPUT;

    XrCoreIrConstantInput constants[] = {
        {.key = xr_program_output_fixture_key("output:value"),
         .type_id = mutation == XR_PROGRAM_OUTPUT_FIXTURE_BOOL_OPERAND ? XR_CORE_TYPE_BOOL
                                                                       : XR_CORE_TYPE_I64,
         .kind = mutation == XR_PROGRAM_OUTPUT_FIXTURE_BOOL_OPERAND
                     ? XR_CORE_IR_CONSTANT_BOOL
                     : XR_CORE_IR_CONSTANT_I64,
         .value.i64 = value},
        {.key = xr_program_output_fixture_key("output:zero"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 0},
    };
    if (mutation == XR_PROGRAM_OUTPUT_FIXTURE_BOOL_OPERAND)
        constants[0].value.boolean = value != 0;
    XrCoreIrKey output_value = xr_program_output_fixture_key("output:value:loaded");
    XrCoreIrKey zero_value = xr_program_output_fixture_key("output:zero:loaded");
    XrCoreIrKey output_operands[] = {output_value};
    XrCoreIrKey return_operands[] = {zero_value};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = mutation == XR_PROGRAM_OUTPUT_FIXTURE_BOOL_OPERAND
                             ? XR_CORE_OP_CORE_CONSTANT_BOOL
                             : XR_CORE_OP_CORE_CONSTANT_I64,
         .result = output_value,
         .result_type_id = constants[0].type_id,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_OUTPUT_GROUP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = output_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
         .immediate.provider_operation = {.contract_id = contract_id,
                                          .operation_id = operation_id}},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = zero_value,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = return_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = xr_program_output_fixture_key("output:block:entry");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = xr_program_output_fixture_key("output:function:entry"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_PROVIDER_CALL,
        .capability_mask = XR_CORE_CAPABILITY_PROVIDER_BINDING,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = xr_program_output_fixture_key("output:module"),
        .constants = constants,
        .constant_count = sizeof(constants) / sizeof(constants[0]),
        .functions = &function,
        .function_count = 1u,
    };
    XrProgramProviderOperationRequirement operation_requirement = {
        .operation_id = operation_id,
        .logical_contract = xr_builtin_provider_byte_sink_logical_contract(),
    };
    XrCoreIrProviderRequirementInput requirement = {
        .contract_id = contract_id,
        .operations = &operation_requirement,
        .operation_count = 1u,
    };
    XrCoreIrKey semantic = xr_program_output_fixture_key("output:semantic");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = semantic.bytes,
        .required_features = &feature,
        .required_feature_count = 1u,
        .provider_requirements = &requirement,
        .provider_requirement_count = 1u,
        .modules = &module,
        .module_count = 1u,
    };
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&input, &program, diagnostic, diagnostic_size);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, diagnostic_size);
    xr_core_ir_program_free(program);
    return status;
}

static XrProgramBuildStatus xr_program_output_fixture_write(
    int64_t value, XrProgramArtifact *artifact, char *diagnostic, size_t diagnostic_size) {
    return xr_program_output_fixture_write_mutated(
        value, XR_PROGRAM_OUTPUT_FIXTURE_VALID, artifact, diagnostic, diagnostic_size);
}

#endif /* XR_PROGRAM_OUTPUT_FIXTURE_H */
