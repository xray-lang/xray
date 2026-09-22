/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_output_trap_fixture.h - Typed output owner-transfer wire fixture
 */

#ifndef XR_PROGRAM_OUTPUT_TRAP_FIXTURE_H
#define XR_PROGRAM_OUTPUT_TRAP_FIXTURE_H

#include "xr_program_text_fixture.h"

typedef enum XrProgramOutputTrapMutation {
    XR_OUTPUT_TRAP_VALID,
    XR_OUTPUT_TRAP_MISSING_OWNER,
    XR_OUTPUT_TRAP_DUPLICATE_OWNER,
    XR_OUTPUT_TRAP_WRONG_EDGE_TYPE,
    XR_OUTPUT_TRAP_EXTRA_SUCCESSOR,
    XR_OUTPUT_TRAP_WRONG_REASON,
    XR_OUTPUT_TRAP_RETURNS,
    XR_OUTPUT_TRAP_IMPLICIT_EXIT,
} XrProgramOutputTrapMutation;

static XrProgramBuildStatus write_output_trap_blocks(XrCoreIrBlockInput *blocks,
                                                     uint32_t block_count,
                                                     const XrCoreIrInstructionInput *output,
                                                     XrProgramArtifact *artifact, char *diagnostic,
                                                     size_t diagnostic_size) {
    XrCoreIrKey constant_key = xr_program_text_fixture_key("output-trap:constant");
    XrStableId contract = output->immediate.provider_operation.contract_id;
    XrStableId operation = output->immediate.provider_operation.operation_id;
    uint16_t field = XR_CORE_TYPE_STRING;
    XrCoreIrTypeInput type = {.key = xr_program_text_fixture_key("output-trap:class"),
                              .local_id = 100u,
                              .kind = XR_CORE_IR_TYPE_CLASS_REFERENCE,
                              .nominal_kind = XR_CORE_IR_NOMINAL_CLASS,
                              .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
                              .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
                              .field_types = &field,
                              .field_count = 1u};
    XrCoreIrConstantInput constant = {.key = constant_key,
                                      .type_id = XR_CORE_TYPE_STRING,
                                      .kind = XR_CORE_IR_CONSTANT_STRING,
                                      .value.string = {(const uint8_t *) "ready", 5u}};
    XrCoreIrFunctionInput function = {.key = xr_program_text_fixture_key("output-trap:function"),
                                      .result_type_id = XR_CORE_TYPE_VOID,
                                      .entry_block = blocks[0].key,
                                      .blocks = blocks,
                                      .block_count = block_count,
                                      .effect_mask = XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_CALL |
                                                     XR_CORE_EFFECT_PROVIDER_CALL,
                                      .capability_mask = XR_CORE_CAPABILITY_PROVIDER_BINDING,
                                      .flags = XR_PROGRAM_FUNCTION_ENTRY};
    XrCoreIrModuleInput module = {.key = xr_program_text_fixture_key("output-trap:module"),
                                  .constants = &constant,
                                  .constant_count = 1u,
                                  .functions = &function,
                                  .function_count = 1u};
    XrProgramProviderOperationRequirement operation_requirement = {
        .operation_id = operation,
        .logical_contract = xr_builtin_provider_byte_sink_logical_contract()};
    XrCoreIrProviderRequirementInput requirement = {
        .contract_id = contract, .operations = &operation_requirement, .operation_count = 1u};
    XrCoreIrKey semantic = xr_program_text_fixture_key("output-trap:semantic");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {.types = &type,
                                  .type_count = 1u,
                                  .semantic_profile_fingerprint = semantic.bytes,
                                  .required_features = &feature,
                                  .required_feature_count = 1u,
                                  .provider_requirements = &requirement,
                                  .provider_requirement_count = 1u,
                                  .modules = &module,
                                  .module_count = 1u};
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&input, &program, diagnostic, diagnostic_size);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, diagnostic_size);
    xr_core_ir_program_free(program);
    return status;
}

/* The display prefix borrows A, while the refusal suffix transfers A and B.
 * Successful output must leave both owners available to their ordinary drops. */
static XrProgramBuildStatus
xr_program_output_trap_fixture_write(XrProgramOutputTrapMutation mutation,
                                     XrProgramArtifact *artifact, char *diagnostic,
                                     size_t diagnostic_size) {
    XrStableId contract = {{0}}, operation = {{0}};
    XrFingerprint digest;
    if (!xr_stable_id_from_key(XR_PROVIDER_IO_CONTRACT_KEY, &contract, &digest) ||
        !xr_stable_id_from_key(XR_PROVIDER_IO_OUTPUT_WRITE_OPERATION_KEY, &operation, &digest))
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    XrCoreIrKey constant_key = xr_program_text_fixture_key("output-trap:constant");
    XrCoreIrKey a = xr_program_text_fixture_key("output-trap:a");
    XrCoreIrKey b = xr_program_text_fixture_key("output-trap:b");
    XrCoreIrKey b_text = xr_program_text_fixture_key("output-trap:b-text");
    XrCoreIrKey edge_a = xr_program_text_fixture_key("output-trap:edge-a");
    XrCoreIrKey edge_b = xr_program_text_fixture_key("output-trap:edge-b");
    XrCoreIrKey entry_key = xr_program_text_fixture_key("output-trap:entry");
    XrCoreIrKey trap_key = xr_program_text_fixture_key("output-trap:cleanup");
    XrCoreIrKey operands[] = {a, a, mutation == XR_OUTPUT_TRAP_DUPLICATE_OWNER ? a : b};
    XrCoreIrKey successors[] = {trap_key, trap_key};
    XrCoreIrKey edge_operands[] = {edge_a, edge_b};
    XrCoreIrValueInput arguments[] = {
        {.key = edge_a,
         .type_id = XR_CORE_TYPE_STRING,
         .category = XR_CORE_IR_VALUE,
         .ownership = XR_CORE_IR_OWNER},
        {.key = edge_b,
         .type_id = 100u,
         .category = XR_CORE_IR_VALUE,
         .ownership = XR_CORE_IR_OWNER},
    };
    if (mutation == XR_OUTPUT_TRAP_DUPLICATE_OWNER)
        arguments[1].type_id = XR_CORE_TYPE_STRING;
    if (mutation == XR_OUTPUT_TRAP_WRONG_EDGE_TYPE) {
        arguments[1].type_id = XR_CORE_TYPE_I64;
        arguments[1].ownership = XR_CORE_IR_NON_OWNER;
    }
    XrCoreIrInstructionInput entry[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_STRING,
         .result = a,
         .result_type_id = XR_CORE_TYPE_STRING,
         .result_ownership = XR_CORE_IR_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant_key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_STRING,
         .result = b_text,
         .result_type_id = XR_CORE_TYPE_STRING,
         .result_ownership = XR_CORE_IR_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant_key},
        {.operation_id = XR_CORE_OP_CORE_CLASS_CONSTRUCT,
         .result = b,
         .result_type_id = 100u,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = &b_text,
         .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OUTPUT_GROUP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = operands,
         .operand_count = 3u,
         .successors = successors,
         .successor_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
         .immediate.provider_operation = {.contract_id = contract, .operation_id = operation}},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = &a,
         .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = &b,
         .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN, .result_type_id = XR_CORE_TYPE_VOID},
    };
    XrCoreIrInstructionInput cleanup[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = edge_operands,
         .operand_count = 2u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = &edge_a,
         .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = &edge_b,
         .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_TRAP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = mutation == XR_OUTPUT_TRAP_WRONG_REASON ? 4u : 7u},
    };
    XrCoreIrBlockInput blocks[] = {
        {.key = entry_key, .instructions = entry, .instruction_count = 7u},
        {.key = trap_key,
         .arguments = arguments,
         .argument_count = 2u,
         .instructions = cleanup,
         .instruction_count = 4u},
    };
    if (mutation == XR_OUTPUT_TRAP_MISSING_OWNER) {
        entry[3].operand_count = 2u;
        blocks[1].argument_count = 1u;
        cleanup[0].operand_count = 1u;
        cleanup[2] = cleanup[3];
        blocks[1].instruction_count = 3u;
    }
    if (mutation == XR_OUTPUT_TRAP_EXTRA_SUCCESSOR)
        entry[3].successor_count = 2u;
    if (mutation == XR_OUTPUT_TRAP_RETURNS)
        cleanup[3] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_RETURN,
                                                 .result_type_id = XR_CORE_TYPE_VOID};
    if (mutation == XR_OUTPUT_TRAP_IMPLICIT_EXIT) {
        entry[3].operand_count = 1u;
        entry[3].successor_count = 0u;
        entry[3].successors = NULL;
    }
    return write_output_trap_blocks(blocks, mutation == XR_OUTPUT_TRAP_IMPLICIT_EXIT ? 1u : 2u,
                                    &entry[3], artifact, diagnostic, diagnostic_size);
}

#endif
