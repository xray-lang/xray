/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_provider_bytes_fixture.h - Mutable byte loans through validated Program
 */
#ifndef XR_TYPED_PROVIDER_BYTES_FIXTURE_H
#define XR_TYPED_PROVIDER_BYTES_FIXTURE_H
#include "xr_typed_provider_fixture.h"

static XrValidatedProgram *typed_byte_validate(const XrCoreIrProgramInput *input, unsigned mutation) {
    XrCoreIrProgram *core = NULL;
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    char diagnostic[256] = {0};
    XrProgramBuildStatus status = xr_core_ir_program_build(input, &core, diagnostic, sizeof(diagnostic));
    if (status == XR_PROGRAM_BUILD_OK) status = xr_program_write(core, &artifact, diagnostic, sizeof(diagnostic));
    if (status == XR_PROGRAM_BUILD_OK) {
        XrProgramDiagnostic verify = {0};
        XrProgramVerifyStatus checked = xr_program_validate(artifact.bytes, artifact.size, NULL, &program, &verify);
        if (mutation && (checked == XR_PROGRAM_VERIFY_OK || program ||
                         verify.kind != XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE)) {
            fprintf(stderr, "byte mutation %u rejected at wrong boundary: status=%d kind=%u\n",
                    mutation, checked, (unsigned)verify.kind);
            abort();
        }
        if (!mutation && checked != XR_PROGRAM_VERIFY_OK)
            fprintf(stderr, "byte verify: %d kind=%u block=%u instruction=%u\n", checked,
                    (unsigned)verify.kind, verify.location.block_id, verify.location.instruction_id);
    } else {
        fprintf(stderr, "byte build: %s\n", diagnostic);
        if (mutation) abort();
    }
    xr_program_artifact_free(&artifact);
    xr_core_ir_program_free(core);
    return program;
}

static XrValidatedProgram *typed_byte_program_build(bool empty, unsigned mutation, bool scalar, bool persistent) {
    XrCoreIrKey k[16];
    for (uint8_t i = 0u; i < 16u; ++i) {
        uint8_t material[] = {0xdb, i};
        k[i] = xr_core_ir_key(material, sizeof(material));
    }
    XrCoreIrTypeInput type = {.key = k[0], .local_id = 32u, .kind = XR_CORE_IR_TYPE_ARRAY,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE, .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .array_element_type = XR_CORE_TYPE_U8};
    XrCoreIrValueInput seed = {.key = k[1], .type_id = XR_CORE_TYPE_U8};
    XrCoreIrValueInput cleanup_owner = {.key = k[5], .type_id = 32u, .ownership = XR_CORE_IR_OWNER};
    XrCoreIrKey elements[] = {k[1], k[1], k[1]};
    XrCoreIrKey call[] = {k[3], k[2], k[2]};
    XrCoreIrInstructionInput code[9] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT, .operands = &k[1], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_ARRAY_CONSTRUCT, .result = k[2], .result_type_id = 32u,
         .result_ownership = XR_CORE_IR_OWNER, .operands = empty ? NULL : elements, .operand_count = empty ? 0u : 3u},
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOCAL, .result = k[3], .result_type_id = 32u,
         .result_category = XR_CORE_IR_PLACE, .operands = &k[2], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_PROVIDER_CALL, .operands = call, .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
         .immediate.provider_operation = {.contract_id = typed_contract_id, .operation_id = {{1u}}},
         .successors = &k[7], .successor_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN, .operands = &k[2], .operand_count = 1u},
    };
    XrCoreIrInstructionInput cleanup[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT, .operands = &k[5], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &k[5], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_TRAP, .immediate_kind = XR_CORE_IR_IMMEDIATE_U32, .immediate.u32 = 7u},
    };
    XrCoreIrBlockInput blocks[] = {
        {.key = k[6], .arguments = &seed, .argument_count = 1u, .instructions = code, .instruction_count = 5u},
        {.key = k[7], .arguments = &cleanup_owner, .argument_count = 1u, .instructions = cleanup, .instruction_count = 3u},
    };
    uint16_t parameter = XR_CORE_TYPE_U8;
    XrParamMode mode = XR_PARAM_READ;
    XrCoreIrFunctionInput function = {.key = k[8], .flags = XR_PROGRAM_FUNCTION_ENTRY,
        .parameter_types = &parameter, .parameter_modes = &mode, .parameter_count = 1u,
        .result_type_id = 32u, .result_ownership = XR_CORE_IR_OWNER,
        .effect_mask = XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_PROVIDER_CALL,
        .capability_mask = XR_CORE_CAPABILITY_PROVIDER_BINDING, .entry_block = k[6],
        .blocks = blocks, .block_count = 2u};
    XrProgramProviderOperationRequirement operation = {.operation_id = {{1u}}, .logical_contract = typed_byte_contract()};
    if (mutation == 1u) operation.logical_contract.reentry = XR_PROVIDER_REENTRY_ALLOWED;
    if (mutation == 2u) call[0] = k[2];
    if (mutation == 3u) operation.logical_contract.parameter_modes[0] = XR_PROVIDER_MODE_IN;
    if (mutation == 4u || mutation == 6u) {
        operation.logical_contract.parameter_count = 2u;
        operation.logical_contract.parameter_modes[1] = XR_PROVIDER_MODE_REF;
        operation.logical_contract.parameter_owners[1] = XR_PROVIDER_OWNER_BORROWED;
        operation.logical_contract.types[1] = XR_PROVIDER_TYPE_U8_ARRAY;
        operation.logical_contract.types[3] = XR_PROVIDER_TYPE_UNIT;
        operation.logical_contract.type_byte_count = 4u;
        call[1] = k[3];
        code[3].operand_count = 3u;
        if (mutation == 6u) {
            code[5] = code[4];
            code[4] = code[3];
            code[3] = code[2];
            code[3].result = k[4];
            call[1] = k[4];
            blocks[0].instruction_count = 6u;
        }
    }
    XrCoreIrProviderRequirementInput requirement = {.contract_id = typed_contract_id,
        .operations = &operation, .operation_count = 1u};
    XrCoreIrModuleInput module = {.key = k[9], .functions = &function, .function_count = 1u};
    XrCoreIrConstantInput constants[] = {
        {.key = k[11], .type_id = XR_CORE_TYPE_I64, .kind = XR_CORE_IR_CONSTANT_I64, .value.i64 = 7},
        {.key = k[12], .type_id = XR_CORE_TYPE_I64, .kind = XR_CORE_IR_CONSTANT_I64, .value.i64 = 42},
    };
    if (scalar) {
        memmove(&code[2], &code[1], 3u * sizeof(code[0]));
        code[0] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
            .result = k[11], .result_type_id = XR_CORE_TYPE_I64,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = k[11]};
        code[1] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_INTEGER_CONVERT,
            .result = k[1], .result_type_id = XR_CORE_TYPE_U8, .operands = &k[11], .operand_count = 1u};
        code[5] = code[4];
        code[6] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
            .operands = &k[2], .operand_count = 1u};
        code[7] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
            .result = k[12], .result_type_id = XR_CORE_TYPE_I64,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = k[12]};
        code[8] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_RETURN,
            .operands = &k[12], .operand_count = 1u};
        blocks[0].arguments = NULL;
        blocks[0].argument_count = 0u;
        blocks[0].instruction_count = 9u;
        function.parameter_types = NULL;
        function.parameter_modes = NULL;
        function.parameter_count = 0u;
        function.result_type_id = XR_CORE_TYPE_I64;
        function.result_ownership = XR_CORE_IR_NON_OWNER;
        module.constants = constants;
        module.constant_count = 2u;
    }
    XrCoreIrModuleSlotInput slot = {.key = k[13], .type_id = 32u,
        .flags = mutation == 5u ? XR_PROGRAM_MODULE_SLOT_CONST : 0u};
    XrCoreIrInstructionInput initialize[6] = {0};
    XrCoreIrBlockInput initialize_block = {.key = k[15], .instructions = initialize, .instruction_count = 6u};
    XrCoreIrFunctionInput functions[2] = {0};
    XrCoreIrKey publication[] = {k[3], k[2]};
    if (persistent) {
        if (!scalar) return NULL;
        memcpy(initialize, code, 3u * sizeof(code[0]));
        initialize[3] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_PLACE_MODULE,
            .result = k[3], .result_type_id = 32u, .result_category = XR_CORE_IR_PLACE,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_MODULE_SLOT, .immediate.module_slot = {module.key, slot.key}};
        initialize[4] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_PLACE_INITIALIZE,
            .operands = publication, .operand_count = 2u};
        initialize[5] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_RETURN};
        code[0] = initialize[3];
        code[1] = code[4];
        code[1].operand_count = 1u;
        code[1].successors = NULL;
        code[1].successor_count = 0u;
        code[2] = code[1];
        code[3] = code[7];
        code[4] = code[8];
        blocks[0].instruction_count = 5u;
        function.block_count = 1u;
        functions[0] = function;
        functions[1] = (XrCoreIrFunctionInput) {.key = k[14], .entry_block = k[15],
            .effect_mask = XR_CORE_EFFECT_TRAP, .blocks = &initialize_block, .block_count = 1u};
        module.functions = functions;
        module.function_count = 2u;
        module.initializer = k[14];
        module.slots = &slot;
        module.slot_count = 1u;
    }
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {.semantic_profile_fingerprint = k[10].bytes,
        .types = &type, .type_count = 1u, .required_features = &feature, .required_feature_count = 1u,
        .provider_requirements = &requirement, .provider_requirement_count = 1u,
        .modules = &module, .module_count = 1u};
    return typed_byte_validate(&input, mutation);
}

static inline XrValidatedProgram *typed_byte_program(bool empty, unsigned mutation) {
    return typed_byte_program_build(empty, mutation, false, false);
}
#endif
