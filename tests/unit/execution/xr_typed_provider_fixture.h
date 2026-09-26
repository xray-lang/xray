/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_provider_fixture.h - Declared optional resource factory and reader
 */
#ifndef XR_TYPED_PROVIDER_FIXTURE_H
#define XR_TYPED_PROVIDER_FIXTURE_H

#include "../plan/target_profile_test_fixture.h"
#include "../program/xr_program_provider_fixture.h"
#include "program/xr_program.h"
#include "program/xr_validated_program_internal.h"
#include "core/xr_core_spec_gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const XrStableId typed_resource_id = {{1u, 2u, 3u, 4u}};
static const XrStableId typed_contract_id = {{254u}};

static XrProviderLogicalContract typed_logical_contract(bool reader) {
    XrProviderLogicalContract c = xr_program_fixture_provider_contract();
    c.effects = XR_PROVIDER_EFFECT_IO;
    c.parameter_count = reader ? 1u : 2u;
    c.parameter_modes[0] = XR_PROVIDER_MODE_IN;
    c.parameter_modes[1] = reader ? 0u : XR_PROVIDER_MODE_IN;
    c.parameter_owners[0] = reader ? XR_PROVIDER_OWNER_BORROWED : XR_PROVIDER_OWNER_TRIVIAL;
    c.parameter_owners[1] = reader ? 0u : XR_PROVIDER_OWNER_TRIVIAL;
    c.result_owner = reader ? XR_PROVIDER_OWNER_TRIVIAL : XR_PROVIDER_OWNER_OWNED;
    uint8_t n = 0u;
    if (!reader) {
        c.types[n++] = XR_PROVIDER_TYPE_I64;
        c.types[n++] = XR_PROVIDER_TYPE_BOOL;
        c.types[n++] = XR_PROVIDER_TYPE_OPTIONAL;
    }
    c.types[n++] = XR_PROVIDER_TYPE_RESOURCE;
    memcpy(c.types + n, typed_resource_id.bytes, XR_STABLE_ID_BYTES);
    n += XR_STABLE_ID_BYTES;
    if (reader) c.types[n++] = XR_PROVIDER_TYPE_I64;
    c.types[n++] = XR_PROVIDER_TYPE_UNIT;
    c.type_byte_count = n;
    if (!reader) {
        c.resource_count = 1u;
        c.resources[0] = (XrProviderLogicalResourceTransition) {
            .resource_id = typed_resource_id, .source = XR_PROVIDER_RESOURCE_RESULT,
            .action = XR_PROVIDER_RESOURCE_ACQUIRE, .timing = XR_PROVIDER_RESOURCE_RESULT_PRESENT,
            .path_count = 1u, .path = {0u},
        };
    }
    return c;
}

static int typed_provider_compare(const void *left, const void *right) {
    const XrTargetProviderContract *a = left, *b = right;
    return memcmp(a->contract_id.bytes, b->contract_id.bytes, XR_STABLE_ID_BYTES);
}

static inline XrProviderLogicalContract typed_byte_contract(void) {
    XrProviderLogicalContract c = xr_program_fixture_provider_contract();
    c.effects = XR_PROVIDER_EFFECT_IO;
    c.parameter_count = 1u;
    c.parameter_modes[0] = XR_PROVIDER_MODE_REF;
    c.parameter_owners[0] = XR_PROVIDER_OWNER_BORROWED;
    c.reentry = XR_PROVIDER_REENTRY_FORBIDDEN;
    c.type_byte_count = 3u;
    c.types[0] = XR_PROVIDER_TYPE_U8_ARRAY;
    c.types[1] = XR_PROVIDER_TYPE_UNIT;
    c.types[2] = XR_PROVIDER_TYPE_UNIT;
    return c;
}

static XrTargetProfile *typed_profile_build(unsigned mutation) {
    XrTestTargetProfileFixture base;
    if (!xr_test_target_profile_fixture_init(&base, false, XR_TARGET_RUNTIME_PROFILE_HOSTED))
        return NULL;
    XrTargetProviderContract providers[3] = {0};
    memcpy(providers, base.providers, sizeof(base.providers));
    XrTargetProviderContract *host = &providers[2];
    *host = (XrTargetProviderContract) {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .abi_schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .contract_id = typed_contract_id, .flags = XR_TARGET_PROVIDER_AVAILABLE_HOSTED,
        .operation_count = 2u, .runtime_profile = XR_TARGET_RUNTIME_PROFILE_HOSTED,
        .provider_role = XR_TARGET_PROVIDER_ROLE_OPERATIONS,
    };
    for (uint8_t i = 0u; i < 2u; ++i) {
        XrTargetProviderOperationContract *op = &host->operations[i];
        op->stable_id.bytes[0] = i + 1u;
        op->logical_contract = typed_logical_contract(i != 0u);
        op->effect_flags = XR_TARGET_PROVIDER_EFFECT_IO;
        op->failure_flags = XR_TARGET_PROVIDER_FAILURE_RETURNS_STATUS;
        op->lifetime_flags = XR_TARGET_PROVIDER_LIFETIME_BORROWS;
        op->call_abi = (XrTargetProviderCallAbiContract) {
            .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION, .parameter_count = 3u,
            .calling_convention = XR_TARGET_PROVIDER_CALLING_CONVENTION_C,
            .target_endian = (uint8_t) base.input.machine.data_layout.endian,
            .pointer_width = (uint8_t) base.input.machine.data_layout.pointer.size,
            .pointer_alignment = (uint8_t) base.input.machine.data_layout.pointer.align,
            .result = {.value_kind = XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER,
                .width = 4u, .alignment = 4u, .ownership = XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE},
        };
        for (uint8_t p = 0u; p < 3u; ++p) {
            op->call_abi.parameters[p] = (XrTargetProviderCallSlotAbi) {
                .value_kind = XR_TARGET_PROVIDER_CALL_VALUE_DATA_ADDRESS,
                .width = op->call_abi.pointer_width, .alignment = op->call_abi.pointer_alignment,
                .ownership = XR_TARGET_PROVIDER_CALL_OWNERSHIP_BORROWED,
                .flags = p == 0u ? XR_TARGET_PROVIDER_CALL_SLOT_NULLABLE :
                         p == 1u ? XR_TARGET_PROVIDER_CALL_SLOT_CONST_POINTEE : 0u,
            };
        }
    }
    if (mutation == 1u) host->operations[0].call_abi.result.width = 8u;
    if (mutation == 2u) host->operations[0].call_abi.parameters[1].flags = 0u;
    if (mutation == 3u) host->operations[0].call_abi.parameters[2].flags = XR_TARGET_PROVIDER_CALL_SLOT_NULLABLE;
    if (mutation == 4u) {
        host->operation_count = 1u;
        host->operations[0].logical_contract = typed_byte_contract();
        memset(&host->operations[1], 0, sizeof(host->operations[1]));
    }
    qsort(providers, 3u, sizeof(providers[0]), typed_provider_compare);
    base.input.providers = providers;
    base.input.provider_count = 3u;
    XrTargetProfile *profile = NULL;
    char diagnostic[256] = {0};
    if (!xr_target_profile_build(&base.input, &profile, diagnostic, sizeof(diagnostic)))
        fprintf(stderr, "typed profile: %s\n", diagnostic);
    return profile;
}

/* Both operations are reachable declarations. No unused provider requirement
 * or backend-generated signature is accepted as the fixture's authority. */
static XrValidatedProgram *typed_program_build(unsigned mutation) {
    XrCoreIrKey keys[20] = {0};
    for (uint8_t i = 0u; i < 20u; ++i) keys[i].bytes[0] = i + 1u;
    uint16_t resource = 32u;
    XrCoreIrVariantInput variants[] = {{0}, {.payload_types = &resource, .payload_count = 1u}};
    XrCoreIrTypeInput types[] = {
        {.local_id = 32u, .key = keys[0], .kind = XR_CORE_IR_TYPE_PROVIDER_RESOURCE,
         .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE, .copy_contract = XR_CORE_IR_COPY_FORBIDDEN,
         .resource_id = typed_resource_id},
        {.local_id = 33u, .key = keys[1], .kind = XR_CORE_IR_TYPE_VARIANT,
         .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE, .copy_contract = XR_CORE_IR_COPY_FORBIDDEN,
         .variants = variants, .variant_count = 2u},
    };
    XrCoreIrConstantInput constants[] = {
        {.key = keys[2], .type_id = XR_CORE_TYPE_I64, .kind = XR_CORE_IR_CONSTANT_I64, .value.i64 = 42},
        {.key = keys[3], .type_id = XR_CORE_TYPE_BOOL, .kind = XR_CORE_IR_CONSTANT_BOOL, .value.boolean = true},
    };
    XrCoreIrInstructionInput make[7] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64, .result = keys[2], .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = keys[2]},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_BOOL, .result = keys[3], .result_type_id = XR_CORE_TYPE_BOOL,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = keys[3]},
        {.operation_id = XR_CORE_OP_CORE_PROVIDER_CALL, .result = keys[4], .result_type_id = 33u,
         .result_ownership = XR_CORE_IR_OWNER, .operands = &keys[2], .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
         .immediate.provider_operation = {.contract_id = typed_contract_id, .operation_id = {{1u}}}},
        {.operation_id = XR_CORE_OP_CORE_RETURN, .result_type_id = XR_CORE_TYPE_VOID,
         .operands = &keys[4], .operand_count = 1u},
    };
    XrCoreIrInstructionInput read[] = {
        {.operation_id = XR_CORE_OP_CORE_PROVIDER_CALL, .result = keys[6], .result_type_id = XR_CORE_TYPE_I64,
         .operands = &keys[5], .operand_count = 1u, .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
         .immediate.provider_operation = {.contract_id = typed_contract_id, .operation_id = {{2u}}}},
        {.operation_id = XR_CORE_OP_CORE_RETURN, .result_type_id = XR_CORE_TYPE_VOID,
         .operands = &keys[6], .operand_count = 1u},
    };
    XrCoreIrValueInput parameter = {.key = keys[5], .type_id = resource};
    XrCoreIrValueInput cleanup_owner = {.key = keys[9], .type_id = 33u, .ownership = XR_CORE_IR_OWNER};
    XrCoreIrKey read_operands[] = {keys[5], keys[4]};
    XrCoreIrInstructionInput cleanup[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT, .result_type_id = XR_CORE_TYPE_VOID,
         .operands = &keys[9], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .result_type_id = XR_CORE_TYPE_VOID,
         .operands = &keys[9], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_TRAP, .result_type_id = XR_CORE_TYPE_VOID,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32, .immediate.u32 = 7u},
    };
    XrCoreIrInstructionInput initialize[6] = {0};
    XrCoreIrBlockInput blocks[4] = {
        {.key = keys[7], .instructions = make, .instruction_count = 4u},
        {.key = keys[10], .arguments = &cleanup_owner, .argument_count = 1u,
         .instructions = cleanup, .instruction_count = 3u},
        {.key = keys[8], .arguments = &parameter, .argument_count = 1u,
         .instructions = read, .instruction_count = 2u},
    };
    XrParamMode read_mode = XR_PARAM_READ;
    XrCoreIrFunctionInput functions[3] = {
        {.key = keys[0], .flags = XR_PROGRAM_FUNCTION_ENTRY, .result_type_id = 33u,
         .result_ownership = XR_CORE_IR_OWNER, .entry_block = keys[7], .blocks = &blocks[0], .block_count = 1u},
        {.key = keys[1], .parameter_types = &resource, .parameter_modes = &read_mode, .parameter_count = 1u,
         .result_type_id = XR_CORE_TYPE_I64, .entry_block = keys[8], .blocks = &blocks[2], .block_count = 1u},
    };
    XrProgramProviderOperationRequirement operations[2] = {0};
    for (uint8_t i = 0u; i < 2u; ++i) {
        functions[i].effect_mask = XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_PROVIDER_CALL;
        functions[i].capability_mask = XR_CORE_CAPABILITY_PROVIDER_BINDING;
        operations[i].operation_id.bytes[0] = i + 1u;
        operations[i].logical_contract = typed_logical_contract(i != 0u);
    }
    if (mutation == 1u) types[0].resource_id.bytes[0] ^= 1u;
    if (mutation == 2u) make[2].result_ownership = XR_CORE_IR_NON_OWNER;
    XrCoreIrKey wrong_parameters[] = {keys[3], keys[2]};
    if (mutation == 3u) make[2].operands = wrong_parameters;
    if (mutation == 4u) {
        make[3] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_VARIANT_PROJECT,
            .result = keys[5], .result_type_id = resource, .operands = &keys[4], .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT_FIELD,
            .immediate.variant_field = {.variant_ordinal = 1u, .field_ordinal = 0u}};
        make[4] = read[0];
        make[4].operands = read_operands;
        make[4].operand_count = 2u;
        make[4].successors = &keys[10];
        make[4].successor_count = 1u;
        functions[0].block_count = 2u;
        make[5] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
            .result_type_id = XR_CORE_TYPE_VOID, .operands = &keys[4], .operand_count = 1u};
        make[6] = read[1];
        blocks[0].instruction_count = 7u;
        functions[0].result_type_id = XR_CORE_TYPE_I64;
        functions[0].result_ownership = XR_CORE_IR_NON_OWNER;
    }
    XrCoreIrProviderRequirementInput requirement = {
        .contract_id = typed_contract_id, .operations = operations, .operation_count = 2u};
    XrCoreIrModuleInput module = {.key = keys[11], .constants = constants, .constant_count = 2u,
        .functions = functions, .function_count = 2u};
    XrCoreIrModuleSlotInput slot = {.key = keys[15], .type_id = 33u};
    XrCoreIrKey publication[] = {keys[14], keys[4]};
    if (mutation == 5u) {
        memcpy(initialize, make, 3u * sizeof(make[0]));
        initialize[3] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_PLACE_MODULE,
            .result = keys[14], .result_type_id = 33u, .result_category = XR_CORE_IR_PLACE,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_MODULE_SLOT,
            .immediate.module_slot = {module.key, slot.key}};
        initialize[4] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_PLACE_INITIALIZE,
            .operands = publication, .operand_count = 2u};
        initialize[5] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_RETURN};
        blocks[3] = (XrCoreIrBlockInput) {.key = keys[13], .instructions = initialize, .instruction_count = 6u};
        functions[2] = (XrCoreIrFunctionInput) {.key = keys[12], .entry_block = keys[13],
            .blocks = &blocks[3], .block_count = 1u, .effect_mask = functions[0].effect_mask,
            .capability_mask = functions[0].capability_mask};
        module.function_count = 3u;
        module.initializer = keys[12];
        module.slots = &slot;
        module.slot_count = 1u;
        make[0] = initialize[3];
        make[1] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_PLACE_LOAD,
            .result = keys[3], .result_type_id = 33u, .operands = &keys[14], .operand_count = 1u};
        make[2] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_VARIANT_PROJECT,
            .result = keys[5], .result_type_id = resource, .operands = &keys[3], .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT_FIELD,
            .immediate.variant_field = {.variant_ordinal = 1u, .field_ordinal = 0u}};
        make[3] = read[0];
        make[4] = read[1];
        blocks[0].instruction_count = 5u;
        functions[0].result_type_id = XR_CORE_TYPE_I64;
        functions[0].result_ownership = XR_CORE_IR_NON_OWNER;
    }
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {.semantic_profile_fingerprint = keys[11].bytes,
        .types = types, .type_count = 2u, .required_features = &feature, .required_feature_count = 1u,
        .provider_requirements = &requirement, .provider_requirement_count = 1u,
        .modules = &module, .module_count = 1u};
    XrCoreIrProgram *core = NULL;
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *validated = NULL;
    char diagnostic[256] = {0};
    XrProgramBuildStatus status = xr_core_ir_program_build(&input, &core, diagnostic, sizeof(diagnostic));
    if (status == XR_PROGRAM_BUILD_OK) status = xr_program_write(core, &artifact, diagnostic, sizeof(diagnostic));
    if (status == XR_PROGRAM_BUILD_OK) {
        XrProgramDiagnostic verify = {0};
        XrProgramVerifyStatus v = xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, &verify);
        if ((mutation == 0u || mutation >= 4u) && v != XR_PROGRAM_VERIFY_OK) fprintf(stderr, "typed verify status: %d kind=%u function=%u block=%u instruction=%u value=%u\n", v, (unsigned)verify.kind, verify.location.function_id, verify.location.block_id, verify.location.instruction_id, verify.location.value_id);
    } else if (mutation == 0u || mutation >= 4u) fprintf(stderr, "typed build: %s\n", diagnostic);
    xr_program_artifact_free(&artifact);
    xr_core_ir_program_free(core);
    return validated;
}
#endif
