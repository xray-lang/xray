/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_module_fixture.h - Dependency-ordered diamond initialization fixture
 */

#ifndef XR_PROGRAM_MODULE_FIXTURE_H
#define XR_PROGRAM_MODULE_FIXTURE_H

#include "../../../src/core/xr_core_spec_gen.h"
#include "../../../src/program/xr_program.h"
#include "../../../src/runtime/abi/xr_builtin_provider_contract.h"
#include "../../../src/plan/semantic/xr_semantic_ids.h"
#include "../../../src/shared/xr_assertion_plan.h"
#include <string.h>

typedef struct XrProgramModuleFixture {
    XrCoreIrKey profile;
    uint16_t feature;
    XrCoreIrInstructionInput returns[4];
    XrCoreIrBlockInput blocks[4];
    XrCoreIrFunctionInput functions[4];
    XrCoreIrKey dependencies[4][2];
    XrCoreIrModuleInput modules[4];
    XrCoreIrModuleSlotInput slots[4][2];
    XrCoreIrProgramInput input;
} XrProgramModuleFixture;

static inline void xr_program_module_fixture_init(XrProgramModuleFixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    const char *module_names[] = {"base", "left", "right", "main"};
    const char *function_names[] = {"base:init", "left:init", "right:init", "main:init"};
    for (uint32_t index = 0u; index < 4u; ++index) {
        fixture->returns[index].operation_id = XR_CORE_OP_CORE_RETURN;
        fixture->blocks[index] = (XrCoreIrBlockInput) {
            .key = xr_core_ir_key(function_names[index], strlen(function_names[index])),
            .instructions = &fixture->returns[index],
            .instruction_count = 1u,
        };
        fixture->functions[index] = (XrCoreIrFunctionInput) {
            .key = xr_core_ir_key(function_names[index], strlen(function_names[index])),
            .entry_block = fixture->blocks[index].key,
            .blocks = &fixture->blocks[index], .block_count = 1u,
            .flags = index == 3u ? XR_PROGRAM_FUNCTION_ENTRY : 0u,
        };
        fixture->modules[index] = (XrCoreIrModuleInput) {
            .key = xr_core_ir_key(module_names[index], strlen(module_names[index])),
            .functions = &fixture->functions[index], .function_count = 1u,
            .initializer = fixture->functions[index].key,
            .dependencies = index ? fixture->dependencies[index] : NULL,
            .dependency_count = index == 3u ? 2u : index ? 1u : 0u,
        };
    }
    fixture->dependencies[1][0] = fixture->modules[0].key;
    fixture->dependencies[2][0] = fixture->modules[0].key;
    fixture->dependencies[3][0] = fixture->modules[1].key;
    fixture->dependencies[3][1] = fixture->modules[2].key;
    static const char profile[] = "module-initialization-profile";
    fixture->profile = xr_core_ir_key(profile, sizeof(profile) - 1u);
    fixture->feature = XR_CORE_FEATURE_CORE_BASE;
    fixture->input = (XrCoreIrProgramInput) {
        .semantic_profile_fingerprint = fixture->profile.bytes,
        .required_features = &fixture->feature, .required_feature_count = 1u,
        .modules = fixture->modules, .module_count = 4u,
    };
}

static inline void xr_program_module_fixture_add_slots(XrProgramModuleFixture *fixture) {
    for (uint32_t module = 0u; module < 4u; ++module) {
        fixture->slots[module][0] = (XrCoreIrModuleSlotInput) {
            .key = xr_core_ir_key("counter:1", 9u), .type_id = XR_CORE_TYPE_I64,
        };
        fixture->slots[module][1] = (XrCoreIrModuleSlotInput) {
            .key = xr_core_ir_key("label:2", 7u), .type_id = XR_CORE_TYPE_STRING,
            .flags = XR_PROGRAM_MODULE_SLOT_CONST,
        };
        fixture->modules[module].slots = fixture->slots[module];
        fixture->modules[module].slot_count = 2u;
    }
}

static inline XrProgramBuildStatus xr_program_module_initializer_fixture_write(
    bool string_value, XrProgramArtifact *artifact, char *diagnostic, size_t diagnostic_size) {
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    xr_program_module_fixture_add_slots(&fixture);
    XrCoreIrKey operands[] = {xr_core_ir_key("place", 5u), xr_core_ir_key("initial", 7u)};
    XrCoreIrConstantInput constant = {
        .key = xr_core_ir_key("forty", 5u), .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64, .value.i64 = 40,
    };
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_PLACE_MODULE,
         .result = operands[0], .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_PLACE,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_MODULE_SLOT,
         .immediate.module_slot = {fixture.modules[0].key, fixture.slots[0][0].key}},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = operands[1], .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = constant.key},
        {.operation_id = XR_CORE_OP_CORE_PLACE_INITIALIZE,
         .operands = operands, .operand_count = 2u},
        {.operation_id = XR_CORE_OP_CORE_RETURN},
    };
    if (string_value) {
        fixture.slots[0][0].type_id = XR_CORE_TYPE_STRING;
        constant.type_id = XR_CORE_TYPE_STRING;
        constant.kind = XR_CORE_IR_CONSTANT_STRING;
        constant.value.string.bytes = (const uint8_t *) "module-owned-text";
        constant.value.string.size = 17u;
        instructions[0].result_type_id = XR_CORE_TYPE_STRING;
        instructions[1].operation_id = XR_CORE_OP_CORE_CONSTANT_STRING;
        instructions[1].result_type_id = XR_CORE_TYPE_STRING;
        instructions[1].result_ownership = XR_CORE_IR_OWNER;
    }
    fixture.modules[0].constants = &constant;
    fixture.modules[0].constant_count = 1u;
    fixture.blocks[0].instructions = instructions;
    fixture.blocks[0].instruction_count = 4u;
    fixture.functions[0].effect_mask = XR_CORE_EFFECT_TRAP;
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&fixture.input, &program, diagnostic, diagnostic_size);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, diagnostic_size);
    xr_core_ir_program_free(program);
    return status;
}

static inline XrProgramBuildStatus
xr_program_module_state_fixture_write_mode(bool text_value, uint32_t fault, bool print_value,
                                           XrProgramArtifact *artifact, char *diagnostic,
                                           size_t diagnostic_size) {
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    xr_program_module_fixture_add_slots(&fixture);
    uint16_t type = text_value ? XR_CORE_TYPE_STRING : XR_CORE_TYPE_I64;
    fixture.slots[0][0].type_id = type;
    fixture.modules[0].slot_count = 1u;
    for (uint32_t index = 1u; index < 4u; ++index) {
        fixture.modules[index].slot_count = 0u;
        fixture.modules[index].slots = NULL;
    }
    XrCoreIrKey place = xr_core_ir_key("state-place", 11u);
    XrCoreIrKey initial = xr_core_ir_key("state-initial", 13u);
    XrCoreIrKey loaded = xr_core_ir_key("state-loaded", 12u);
    XrCoreIrKey copied = xr_core_ir_key("state-copied", 12u);
    XrCoreIrConstantInput constant = {
        .key = initial,
        .type_id = type,
        .kind = text_value ? XR_CORE_IR_CONSTANT_STRING : XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 40,
    };
    if (text_value) {
        constant.value.string.bytes = (const uint8_t *) "module-owned-text";
        constant.value.string.size = 17u;
    }
    fixture.modules[0].constants = &constant;
    fixture.modules[0].constant_count = 1u;
    XrCoreIrKey publication[] = {place, initial};
    XrCoreIrKey duplicate_publication[] = {place, loaded};
    XrCoreIrInstructionInput initialize[] = {
        {.operation_id = XR_CORE_OP_CORE_PLACE_MODULE,
         .result = place,
         .result_type_id = type,
         .result_category = XR_CORE_IR_PLACE,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_MODULE_SLOT,
         .immediate.module_slot = {fixture.modules[0].key, fixture.slots[0][0].key}},
        {.operation_id =
             text_value ? XR_CORE_OP_CORE_CONSTANT_STRING : XR_CORE_OP_CORE_CONSTANT_I64,
         .result = initial,
         .result_type_id = type,
         .result_ownership = text_value ? XR_CORE_IR_OWNER : XR_CORE_IR_NON_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = initial},
        {.operation_id = XR_CORE_OP_CORE_PLACE_INITIALIZE,
         .operands = publication,
         .operand_count = 2u},
        {.operation_id = XR_CORE_OP_CORE_RETURN},
        {.operation_id = XR_CORE_OP_CORE_RETURN},
        {.operation_id = XR_CORE_OP_CORE_RETURN},
    };
    fixture.blocks[0].instructions = initialize;
    fixture.blocks[0].instruction_count = 4u;
    if (fault == 1u) {
        initialize[2] = initialize[3];
        fixture.blocks[0].instruction_count = 3u;
    } else if (fault == 2u) {
        initialize[3] = initialize[1];
        initialize[3].result = loaded;
        initialize[4] = initialize[2];
        initialize[4].operands = duplicate_publication;
        fixture.blocks[0].instruction_count = 6u;
    } else if (fault == 3u) {
        initialize[3] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_TRAP,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
            .immediate.u32 = 4u,
        };
    }
    fixture.functions[0].effect_mask = XR_CORE_EFFECT_TRAP;
    XrCoreIrKey replacement = xr_core_ir_key("state-replacement", 17u);
    XrCoreIrKey old = xr_core_ir_key("state-old", 9u);
    XrCoreIrConstantInput next_constant = {
        .key = replacement,
        .type_id = type,
        .kind = text_value ? XR_CORE_IR_CONSTANT_STRING : XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 1,
    };
    if (text_value) {
        next_constant.value.string.bytes = (const uint8_t *) "changed";
        next_constant.value.string.size = 7u;
    }
    fixture.modules[3].constants = &next_constant;
    fixture.modules[3].constant_count = 1u;
    XrCoreIrKey addition[] = {loaded, replacement};
    XrCoreIrKey update[] = {place, text_value ? replacement : copied};
    XrCoreIrInstructionInput entry[9] = {
        initialize[0],
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOAD,
         .operands = &place,
         .operand_count = 1u,
         .result = loaded,
         .result_type_id = type},
        {.operation_id =
             text_value ? XR_CORE_OP_CORE_CONSTANT_STRING : XR_CORE_OP_CORE_CONSTANT_I64,
         .result = replacement,
         .result_type_id = type,
         .result_ownership = text_value ? XR_CORE_IR_OWNER : XR_CORE_IR_NON_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = replacement},
        {.operation_id = text_value ? XR_CORE_OP_CORE_OWNER_COPY : XR_CORE_OP_CORE_ADD_I64,
         .operands = text_value ? &loaded : addition,
         .operand_count = text_value ? 1u : 2u,
         .result = copied,
         .result_type_id = type,
         .immediate_kind = text_value ? XR_CORE_IR_IMMEDIATE_NONE : XR_CORE_IR_IMMEDIATE_U32,
         .result_ownership = text_value ? XR_CORE_IR_OWNER : XR_CORE_IR_NON_OWNER},
        {.operation_id = text_value ? XR_CORE_OP_CORE_PLACE_EXCHANGE : XR_CORE_OP_CORE_PLACE_STORE,
         .operands = update,
         .operand_count = 2u,
         .result = text_value ? old : (XrCoreIrKey) {{0}},
         .result_type_id = text_value ? type : XR_CORE_TYPE_VOID,
         .result_ownership = text_value ? XR_CORE_IR_OWNER : XR_CORE_IR_NON_OWNER},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &old, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN, .operands = &copied, .operand_count = 1u},
    };
    if (!text_value) {
        entry[5] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_PLACE_LOAD,
            .operands = &place,
            .operand_count = 1u,
            .result = old,
            .result_type_id = type,
        };
        entry[6].operands = &old;
    }
    XrStableId output_contract = {{0}}, output_operation = {{0}};
    XrFingerprint digest;
    XrProgramProviderOperationRequirement operation_row = {
        .logical_contract = xr_builtin_provider_byte_sink_logical_contract(),
    };
    XrCoreIrProviderRequirementInput provider = {
        .operations = &operation_row,
        .operation_count = 1u,
    };
    if (print_value) {
        if (!text_value ||
            !xr_stable_id_from_key(XR_PROVIDER_IO_CONTRACT_KEY, &output_contract, &digest) ||
            !xr_stable_id_from_key(XR_PROVIDER_IO_OUTPUT_WRITE_OPERATION_KEY, &output_operation,
                                   &digest))
            return XR_PROGRAM_BUILD_INVALID_INPUT;
        operation_row.operation_id = output_operation;
        provider.contract_id = output_contract;
        fixture.input.provider_requirements = &provider;
        fixture.input.provider_requirement_count = 1u;
        entry[6] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OUTPUT_GROUP,
            .operands = &copied,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
            .immediate.provider_operation = {output_contract, output_operation},
        };
        entry[7] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
            .operands = &copied,
            .operand_count = 1u,
        };
        entry[8].operation_id = XR_CORE_OP_CORE_RETURN;
    }
    XrCoreIrBlockInput entry_block = {
        .key = xr_core_ir_key("state-entry-block", 17u),
        .instructions = entry,
        .instruction_count = print_value ? 9u : 7u,
    };
    fixture.functions[3].flags = 0u;
    XrCoreIrFunctionInput functions[] = {
        fixture.functions[3],
        {.key = xr_core_ir_key("state-entry", 11u),
         .result_type_id = print_value ? XR_CORE_TYPE_VOID : type,
         .result_ownership = text_value && !print_value ? XR_CORE_IR_OWNER : XR_CORE_IR_NON_OWNER,
         .entry_block = entry_block.key,
         .blocks = &entry_block,
         .block_count = 1u,
         .effect_mask = XR_CORE_EFFECT_TRAP |
                        (print_value ? XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_PROVIDER_CALL : 0u),
         .capability_mask = print_value ? XR_CORE_CAPABILITY_PROVIDER_BINDING : 0u,
         .flags = XR_PROGRAM_FUNCTION_ENTRY},
    };
    fixture.modules[3].functions = functions;
    fixture.modules[3].function_count = 2u;
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&fixture.input, &program, diagnostic, diagnostic_size);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, diagnostic_size);
    xr_core_ir_program_free(program);
    return status;
}

static inline XrProgramBuildStatus
xr_program_module_state_fixture_write(bool text_value, uint32_t fault, XrProgramArtifact *artifact,
                                      char *diagnostic, size_t diagnostic_size) {
    return xr_program_module_state_fixture_write_mode(text_value, fault, false, artifact,
                                                      diagnostic, diagnostic_size);
}

static inline XrProgramBuildStatus
xr_program_module_cleanup_fixture_write(uint32_t finish, XrProgramArtifact *artifact,
                                        char *diagnostic, size_t diagnostic_size) {
    bool message = finish == 8u;
    if (message) finish = 5u;
    bool delayed = finish == 6u || finish == 7u;
    if (delayed)
        finish -= 2u;
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    uint16_t field = XR_CORE_TYPE_STRING;
    XrCoreIrTypeInput type = {
        .key = xr_core_ir_key("module-class", 12u),
        .local_id = 100u,
        .kind = XR_CORE_IR_TYPE_CLASS_REFERENCE,
        .nominal_kind = XR_CORE_IR_NOMINAL_CLASS,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .field_types = &field,
        .field_count = 1u,
    };
    fixture.input.types = &type;
    fixture.input.type_count = 1u;
    XrCoreIrConstantInput constant = {
        .key = xr_core_ir_key("module-field", 12u),
        .type_id = XR_CORE_TYPE_STRING,
        .kind = XR_CORE_IR_CONSTANT_STRING,
        .value.string = {.bytes = (const uint8_t *) "x", .size = 1u},
    };
    fixture.modules[0].constants = &constant;
    fixture.modules[0].constant_count = 1u;
    XrCoreIrModuleSlotInput slots[4] = {0};
    XrCoreIrKey strings[4], instances[4], places[4], shared[4], fields[4], replacements[4], old[4],
        publications[4][2], exchanges[4][2];
    XrCoreIrInstructionInput rows[41] = {0};
    const uint32_t order[] = {1u, 0u, 3u, 2u};
    for (uint32_t index = 0u; index < 4u; ++index) {
        uint8_t identity[] = {0u, (uint8_t) index};
        slots[index].key = xr_core_ir_key(identity, sizeof(identity));
        slots[index].type_id = 100u;
        identity[0] = 1u;
        strings[index] = xr_core_ir_key(identity, sizeof(identity));
        identity[0] = 2u;
        instances[index] = xr_core_ir_key(identity, sizeof(identity));
        identity[0] = 3u;
        places[index] = xr_core_ir_key(identity, sizeof(identity));
        identity[0] = 4u;
        shared[index] = xr_core_ir_key(identity, sizeof(identity));
        identity[0] = 5u;
        fields[index] = xr_core_ir_key(identity, sizeof(identity));
        identity[0] = 6u;
        replacements[index] = xr_core_ir_key(identity, sizeof(identity));
        identity[0] = 7u;
        old[index] = xr_core_ir_key(identity, sizeof(identity));
    }
    for (uint32_t index = 0u; index < 4u; ++index) {
        publications[index][0] = places[index];
        publications[index][1] = shared[index];
        exchanges[index][0] = fields[index];
        exchanges[index][1] = replacements[index];
        rows[index * 10u] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_CONSTANT_STRING,
            .result = strings[index],
            .result_type_id = field,
            .result_ownership = XR_CORE_IR_OWNER,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
            .immediate.key = constant.key,
        };
        rows[index * 10u + 1u] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_CLASS_CONSTRUCT,
            .result = instances[index],
            .result_type_id = 100u,
            .result_ownership = XR_CORE_IR_OWNER,
            .operands = &strings[index],
            .operand_count = 1u,
        };
        rows[index * 10u + 2u] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_ALIAS,
            .result = shared[index],
            .result_type_id = 100u,
            .result_ownership = XR_CORE_IR_OWNER,
            .operands = &instances[index],
            .operand_count = 1u,
        };
        rows[index * 10u + 3u] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_CLASS_FIELD_PLACE,
            .result = fields[index],
            .result_type_id = field,
            .result_category = XR_CORE_IR_PLACE,
            .operands = &instances[index],
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
            .immediate.field_ordinal = 0u,
        };
        rows[index * 10u + 4u] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_PLACE_MODULE,
            .result = places[index],
            .result_type_id = 100u,
            .result_category = XR_CORE_IR_PLACE,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_MODULE_SLOT,
            .immediate.module_slot = {fixture.modules[0].key, slots[order[index]].key},
        };
        rows[index * 10u + 5u] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_PLACE_INITIALIZE,
            .operands = publications[index],
            .operand_count = 2u,
        };
        rows[index * 10u + 6u] = rows[index * 10u];
        rows[index * 10u + 6u].result = replacements[index];
        rows[index * 10u + 7u] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_PLACE_EXCHANGE,
            .result = old[index],
            .result_type_id = field,
            .result_ownership = XR_CORE_IR_OWNER,
            .operands = exchanges[index],
            .operand_count = 2u,
        };
        rows[index * 10u + 8u] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
            .operands = &old[index],
            .operand_count = 1u,
        };
        rows[index * 10u + 9u] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
            .operands = &instances[index],
            .operand_count = 1u,
        };
    }
    uint32_t completed = finish == 0u || finish == 3u ? 4u : 3u;
    rows[completed * 10u] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_RETURN};
    if (finish == 1u) {
        rows[completed * 10u].operation_id = XR_CORE_OP_CORE_TRAP;
        rows[completed * 10u].immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
        rows[completed * 10u].immediate.u32 = 4u;
    }
    fixture.modules[0].slots = slots;
    fixture.modules[0].slot_count = 4u;
    fixture.blocks[0].instructions = rows;
    fixture.blocks[0].instruction_count = completed * 10u + 1u;
    fixture.functions[0].effect_mask = XR_CORE_EFFECT_TRAP;
    if (finish == 4u) {
        /* Publish the local owner as the error while the module slot still
         * owns a separate reference to the same object. */
        rows[completed * 10u - 1u] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_ERROR_PUBLISH,
            .operands = &instances[completed - 1u],
            .operand_count = 1u,
        };
        fixture.blocks[0].instruction_count = completed * 10u;
        fixture.functions[0].error_type_id = 100u;
        fixture.functions[0].effect_mask |= XR_CORE_EFFECT_ERROR;
    }
    XrCoreIrKey helper_key = xr_core_ir_key("module:consume", 14u);
    XrCoreIrKey helper_parameter = xr_core_ir_key("module:parameter", 16u);
    XrCoreIrKey helper_text = xr_core_ir_key("module:temporary", 16u);
    uint16_t class_type = 100u;
    XrParamMode mode = XR_PARAM_MOVE;
    XrCoreIrInstructionInput helper_rows[] = {
        rows[0],
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &helper_text, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .operands = &helper_parameter,
         .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN},
    };
    helper_rows[0].result = helper_text;
    XrCoreIrValueInput argument = {
        .key = helper_parameter,
        .type_id = class_type,
        .ownership = XR_CORE_IR_OWNER,
    };
    XrCoreIrBlockInput helper_block = {
        .key = helper_key,
        .arguments = &argument,
        .argument_count = 1u,
        .instructions = helper_rows,
        .instruction_count = XR_COUNTOF(helper_rows),
    };
    XrCoreIrFunctionInput module_functions[] = {
        fixture.functions[0],
        {.key = helper_key,
         .parameter_types = &class_type,
         .parameter_modes = &mode,
         .parameter_count = 1u,
         .entry_block = helper_key,
         .blocks = &helper_block,
         .block_count = 1u,
         .effect_mask = XR_CORE_EFFECT_TRAP},
    };
    if (finish == 3u) {
        for (uint32_t index = 0u; index < completed; ++index)
            rows[index * 10u + 9u] = (XrCoreIrInstructionInput) {
                .operation_id = XR_CORE_OP_CORE_CALL_SEALED_DIRECT,
                .operands = &instances[index],
                .operand_count = 1u,
                .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
                .immediate.key = helper_key,
            };
        module_functions[0].effect_mask |= XR_CORE_EFFECT_CALL;
        fixture.modules[0].functions = module_functions;
        fixture.modules[0].function_count = XR_COUNTOF(module_functions);
    }
    XrCoreIrKey successors[] = {xr_core_ir_key("module:resume", 13u),
                                xr_core_ir_key("module:cancel", 13u)};
    XrCoreIrInstructionInput resume = {.operation_id = XR_CORE_OP_CORE_RETURN};
    XrCoreIrInstructionInput cancel = {.operation_id = XR_CORE_OP_CORE_CANCEL_PUBLISH};
    XrCoreIrBlockInput blocks[4] = {
        fixture.blocks[0],
        {.key = successors[0], .instructions = &resume, .instruction_count = 1u},
        {.key = successors[1], .instructions = &cancel, .instruction_count = 1u},
    };
    XrCoreIrCoroutineStateInput states[] = {
        {.state_id = 0u, .continuation_block = blocks[0].key},
        {.state_id = 1u, .continuation_block = successors[0]},
    };
    XrCoreIrCoroutineSafepointInput safepoint = {.resume_state_id = 1u};
    if (finish == 2u) {
        rows[completed * 10u] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_COROUTINE_YIELD,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
            .successors = successors,
            .successor_count = 2u,
        };
    }
    XrCoreIrConstantInput panic_constants[] = {
        constant,
        {.key = xr_core_ir_key("module:false", 12u),
         .type_id = XR_CORE_TYPE_BOOL,
         .kind = XR_CORE_IR_CONSTANT_BOOL,
         .value.boolean = false},
    };
    XrCoreIrKey panic_key = xr_core_ir_key("module:panic", 12u);
    XrCoreIrKey panic_values[] = {panic_key, instances[3], xr_core_ir_key("module:panic-message-owner", 26u)};
    XrCoreIrKey assert_values[] = {panic_constants[1].key, instances[2], instances[2], strings[3]};
    if (message) assert_values[1] = strings[3];
    XrCoreIrValueInput panic_arguments[] = {
        {.key = panic_values[0], .type_id = XR_CORE_TYPE_PANIC_INFO, .ownership = XR_CORE_IR_OWNER},
        {.key = panic_values[1], .type_id = 100u, .ownership = XR_CORE_IR_OWNER},
        {.key = panic_values[2], .type_id = XR_CORE_TYPE_STRING, .ownership = XR_CORE_IR_OWNER},
    };
    XrCoreIrInstructionInput panic_rows[4] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .operands = panic_values,
         .operand_count = 2u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .operands = &panic_values[1],
         .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_PANIC_PUBLISH,
         .operands = panic_values,
         .operand_count = 1u},
    };
    if (message) {
        panic_rows[3] = panic_rows[2];
        panic_rows[2] = panic_rows[1];
        panic_rows[1].operands = &panic_values[2];
        panic_rows[0].operand_count = 3u;
    }
    XrCoreIrBlockInput panic_blocks[] = {
        fixture.blocks[0],
        {.key = panic_key,
         .arguments = panic_arguments,
         .argument_count = message ? 3u : 2u,
         .instructions = panic_rows,
         .instruction_count = message ? 4u : 3u},
    };
    if (finish == 5u) {
        /* Close the local alias before publishing PanicInfo; module owners are
         * reclaimed
         * at the boundary. */
        rows[29] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_CONSTANT_BOOL,
            .result = assert_values[0],
            .result_type_id = XR_CORE_TYPE_BOOL,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
            .immediate.key = assert_values[0],
        };
        rows[30] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_ASSERT_CONDITION,
            .operands = assert_values,
            .operand_count = 2u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
            .immediate.u32 = XR_ASSERTION_FAILURE_CONDITION_FALSE,
            .successors = &panic_key,
            .successor_count = 1u,
        };
        rows[31] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
            .operands = &instances[2],
            .operand_count = 1u,
        };
        rows[32] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_RETURN};
        if (message) {
            rows[34] = rows[32];
            rows[33] = rows[31];
            rows[32] = (XrCoreIrInstructionInput){.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
                .operands = &strings[3], .operand_count = 1u};
            rows[31] = rows[30];
            rows[31].operand_count = 4u;
            rows[31].immediate.u32 |= XR_CORE_ASSERT_MESSAGE_PRESENT;
            rows[30] = (XrCoreIrInstructionInput){.operation_id = XR_CORE_OP_CORE_CONSTANT_STRING,
                .result = strings[3], .result_type_id = XR_CORE_TYPE_STRING,
                .result_ownership = XR_CORE_IR_OWNER,
                .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = constant.key};
        }
        panic_blocks[0].instruction_count = message ? 35u : 33u;
        fixture.functions[0].blocks = panic_blocks;
        fixture.functions[0].block_count = XR_COUNTOF(panic_blocks);
        fixture.functions[0].panic_type_id = XR_CORE_TYPE_PANIC_INFO;
        fixture.functions[0].effect_mask |= XR_CORE_EFFECT_PANIC;
        fixture.modules[0].constants = panic_constants;
        fixture.modules[0].constant_count = XR_COUNTOF(panic_constants);
    }
    XrCoreIrInstructionInput yield = {
        .operation_id = XR_CORE_OP_CORE_COROUTINE_YIELD,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
        .successors = successors,
        .successor_count = 2u,
    };
    if (delayed) {
        uint32_t count = fixture.functions[0].block_count;
        blocks[count + 1u] = blocks[2];
        memmove(&blocks[1], fixture.functions[0].blocks, count * sizeof(*blocks));
        successors[0] = fixture.functions[0].entry_block;
        fixture.functions[0].entry_block = xr_core_ir_key("module:yield", 12u);
        blocks[0] = (XrCoreIrBlockInput) {.key = fixture.functions[0].entry_block,
                                          .instructions = &yield,
                                          .instruction_count = 1u};
        states[0].continuation_block = blocks[0].key;
        states[1].continuation_block = successors[0];
    }
    if (finish == 2u || delayed) {
        fixture.functions[0].effect_mask |= XR_CORE_EFFECT_CANCEL | XR_CORE_EFFECT_SUSPEND;
        fixture.functions[0].capability_mask = XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION;
        fixture.functions[0].blocks = blocks;
        fixture.functions[0].block_count = delayed ? fixture.functions[0].block_count + 2u : 3u;
        fixture.functions[0].coroutine_states = states;
        fixture.functions[0].coroutine_state_count = 2u;
        fixture.functions[0].coroutine_safepoints = &safepoint;
        fixture.functions[0].coroutine_safepoint_count = 1u;
    }
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&fixture.input, &program, diagnostic, diagnostic_size);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, diagnostic_size);
    xr_core_ir_program_free(program);
    return status;
}

static inline XrProgramBuildStatus xr_program_module_output_fixture_write_with_suspension(
    uint32_t failing_module, uint32_t suspending_module, XrProgramArtifact *artifact,
    char *diagnostic, size_t diagnostic_size) {
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    XrStableId contract = {{0}}, operation = {{0}};
    XrFingerprint digest;
    if (!xr_stable_id_from_key(XR_PROVIDER_IO_CONTRACT_KEY, &contract, &digest) ||
        !xr_stable_id_from_key(XR_PROVIDER_IO_OUTPUT_WRITE_OPERATION_KEY, &operation, &digest))
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    XrProgramProviderOperationRequirement operation_row = {
        .operation_id = operation,
        .logical_contract = xr_builtin_provider_byte_sink_logical_contract(),
    };
    XrCoreIrProviderRequirementInput provider = {
        .contract_id = contract,
        .operations = &operation_row,
        .operation_count = 1u,
    };
    fixture.input.provider_requirements = &provider;
    fixture.input.provider_requirement_count = 1u;
    XrCoreIrConstantInput constants[4] = {0};
    XrCoreIrInstructionInput instructions[4][3] = {0};
    XrCoreIrBlockInput suspended_blocks[3] = {0};
    XrCoreIrCoroutineStateInput states[2] = {0};
    XrCoreIrCoroutineSafepointInput safepoint = {.safepoint_id = 0u, .resume_state_id = 1u};
    XrCoreIrKey successors[2] = {xr_core_ir_key("module-resume", 13u),
                                 xr_core_ir_key("module-cancel", 13u)};
    XrCoreIrInstructionInput resumed = {0};
    XrCoreIrKey cancel_value = xr_core_ir_key("module-cancel-value", 19u);
    XrCoreIrInstructionInput cancelled[3] = {0};
    XrCoreIrKey values[4];
    for (uint32_t module = 0u; module < 4u; ++module) {
        values[module] = fixture.functions[module].key;
        constants[module] = (XrCoreIrConstantInput) {
            .key = fixture.modules[module].key,
            .type_id = XR_CORE_TYPE_I64,
            .kind = XR_CORE_IR_CONSTANT_I64,
            .value.i64 = (int64_t) module + 1,
        };
        fixture.modules[module].constants = &constants[module];
        fixture.modules[module].constant_count = 1u;
        instructions[module][0] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
            .result = values[module],
            .result_type_id = XR_CORE_TYPE_I64,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
            .immediate.key = constants[module].key,
        };
        instructions[module][1] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OUTPUT_GROUP,
            .operands = &values[module],
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
            .immediate.provider_operation = {contract, operation},
        };
        instructions[module][2] = fixture.returns[module];
        if (module == failing_module) {
            instructions[module][2].operation_id = XR_CORE_OP_CORE_TRAP;
            instructions[module][2].immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
            instructions[module][2].immediate.u32 = 4u;
        }
        fixture.blocks[module].instructions = instructions[module];
        fixture.blocks[module].instruction_count = 3u;
        fixture.functions[module].effect_mask =
            XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_PROVIDER_CALL;
        fixture.functions[module].capability_mask = XR_CORE_CAPABILITY_PROVIDER_BINDING;
        if (module == suspending_module) {
            cancelled[0] = instructions[module][0];
            cancelled[0].result = cancel_value;
            cancelled[1] = instructions[module][1];
            cancelled[1].operands = &cancel_value;
            cancelled[2].operation_id = XR_CORE_OP_CORE_CANCEL_PUBLISH;
            resumed = instructions[module][2];
            instructions[module][2] = (XrCoreIrInstructionInput) {
                .operation_id = XR_CORE_OP_CORE_COROUTINE_YIELD,
                .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
                .immediate.u32 = 0u,
                .successors = successors,
                .successor_count = 2u,
            };
            suspended_blocks[0] = fixture.blocks[module];
            suspended_blocks[1] = (XrCoreIrBlockInput) {
                .key = successors[0],
                .instructions = &resumed,
                .instruction_count = 1u,
            };
            suspended_blocks[2] = (XrCoreIrBlockInput) {
                .key = successors[1],
                .instructions = cancelled,
                .instruction_count = 3u,
            };
            states[0] = (XrCoreIrCoroutineStateInput) {
                .state_id = 0u,
                .continuation_block = suspended_blocks[0].key,
            };
            states[1] = (XrCoreIrCoroutineStateInput) {
                .state_id = 1u,
                .continuation_block = successors[0],
            };
            fixture.functions[module].blocks = suspended_blocks;
            fixture.functions[module].block_count = 3u;
            fixture.functions[module].coroutine_states = states;
            fixture.functions[module].coroutine_state_count = 2u;
            fixture.functions[module].coroutine_safepoints = &safepoint;
            fixture.functions[module].coroutine_safepoint_count = 1u;
            fixture.functions[module].effect_mask |= XR_CORE_EFFECT_SUSPEND | XR_CORE_EFFECT_CANCEL;
            fixture.functions[module].capability_mask |=
                XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION;
        }
    }
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&fixture.input, &program, diagnostic, diagnostic_size);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, diagnostic_size);
    xr_core_ir_program_free(program);
    return status;
}

static inline XrProgramBuildStatus
xr_program_module_output_fixture_write(uint32_t failing_module, XrProgramArtifact *artifact,
                                       char *diagnostic, size_t diagnostic_size) {
    return xr_program_module_output_fixture_write_with_suspension(
        failing_module, UINT32_MAX, artifact, diagnostic, diagnostic_size);
}

static inline XrProgramBuildStatus
xr_program_module_native_fixture_write(uint32_t scenario, XrProgramArtifact *artifact,
                                       char *diagnostic, size_t diagnostic_size) {
    if (scenario >= 5u && scenario <= 13u)
        return xr_program_module_cleanup_fixture_write(scenario - 5u, artifact, diagnostic,
                                                       diagnostic_size);
    return xr_program_module_state_fixture_write_mode(
        scenario == 1u || scenario == 3u || scenario == 4u, scenario >= 2u ? scenario - 1u : 0u,
        scenario == 1u || scenario == 3u || scenario == 4u, artifact, diagnostic, diagnostic_size);
}

static inline XrProgramBuildStatus xr_program_module_array_borrow_fixture_write(
    uint32_t variant, XrProgramArtifact *artifact) {
    bool mutated = variant == 1u || variant == 2u;
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    xr_program_module_fixture_add_slots(&fixture);
    XrCoreIrTypeInput type = {.key = xr_core_ir_key("module-array-type", sizeof("module-array-type") - 1u), .local_id = 100u,
        .kind = XR_CORE_IR_TYPE_ARRAY, .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT, .array_element_type = XR_CORE_TYPE_I64};
    fixture.input.types = &type; fixture.input.type_count = 1u;
    fixture.slots[0][0].type_id = type.local_id;
    XrCoreIrKey array = xr_core_ir_key("module-array-owner", sizeof("module-array-owner") - 1u), place = xr_core_ir_key("module-array-place", sizeof("module-array-place") - 1u);
    XrCoreIrKey borrow = xr_core_ir_key("module-array-borrow", sizeof("module-array-borrow") - 1u), arg = xr_core_ir_key("module-array-arg", sizeof("module-array-arg") - 1u);
    XrCoreIrKey target = xr_core_ir_key("module-array-next", sizeof("module-array-next") - 1u), replacement = xr_core_ir_key("module-array-new", sizeof("module-array-new") - 1u);
    XrCoreIrKey old = xr_core_ir_key("module-array-old", sizeof("module-array-old") - 1u), length = xr_core_ir_key("module-array-length", sizeof("module-array-length") - 1u);
    XrCoreIrKey second = xr_core_ir_key("module-array-second-place", sizeof("module-array-second-place") - 1u);
    XrCoreIrKey initialize[] = {place, array}, exchange[] = {second, replacement};
    XrCoreIrInstructionInput first[] = {
        {.operation_id = XR_CORE_OP_CORE_ARRAY_CONSTRUCT, .result = array,
         .result_type_id = type.local_id, .result_ownership = XR_CORE_IR_OWNER},
        {.operation_id = XR_CORE_OP_CORE_PLACE_MODULE, .result = place,
         .result_type_id = type.local_id, .result_category = XR_CORE_IR_PLACE,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_MODULE_SLOT,
         .immediate.module_slot = {fixture.modules[0].key, fixture.slots[0][0].key}},
        {.operation_id = XR_CORE_OP_CORE_PLACE_INITIALIZE,
         .operands = initialize, .operand_count = 2u},
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOAD, .result = borrow,
         .result_type_id = type.local_id, .operands = &place, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_BRANCH, .operands = &borrow, .operand_count = 1u,
         .successors = &target, .successor_count = 1u},
    };
    XrCoreIrInstructionInput next[] = {
        {.operation_id = XR_CORE_OP_CORE_PLACE_MODULE, .result = second,
         .result_type_id = type.local_id, .result_category = XR_CORE_IR_PLACE,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_MODULE_SLOT,
         .immediate.module_slot = {fixture.modules[0].key, fixture.slots[0][0].key}},
        {.operation_id = XR_CORE_OP_CORE_ARRAY_CONSTRUCT, .result = replacement,
         .result_type_id = type.local_id, .result_ownership = XR_CORE_IR_OWNER},
        {.operation_id = XR_CORE_OP_CORE_PLACE_EXCHANGE, .result = old,
         .result_type_id = type.local_id, .result_ownership = XR_CORE_IR_OWNER,
         .operands = exchange, .operand_count = 2u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &old, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_SEQUENCE_LENGTH, .result = length,
         .result_type_id = XR_CORE_TYPE_I64, .operands = &arg, .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN},
    };
    XrCoreIrValueInput argument = {.key = arg, .type_id = type.local_id};
    XrCoreIrBlockInput blocks[] = {
        {.key = fixture.blocks[0].key, .instructions = first, .instruction_count = 5u},
        {.key = target, .arguments = &argument, .argument_count = 1u,
         .instructions = next + (mutated ? 0u : 4u), .instruction_count = mutated ? 6u : 2u},
    };
    if (variant >= 2u) {
        XrCoreIrInstructionInput read = next[4];
        if (mutated) memmove(&next[1], &next[0], 4u * sizeof(next[0]));
        next[0] = read;
        next[mutated ? 5u : 1u] = (XrCoreIrInstructionInput){
            .operation_id = XR_CORE_OP_CORE_BRANCH, .operands = &arg, .operand_count = 1u,
            .successors = &target, .successor_count = 1u};
        blocks[1].instructions = next;
    }
    fixture.functions[0].blocks = blocks; fixture.functions[0].block_count = 2u;
    fixture.functions[0].effect_mask = XR_CORE_EFFECT_TRAP;
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status = xr_core_ir_program_build(&fixture.input, &program, NULL, 0u);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, NULL, 0u);
    xr_core_ir_program_free(program);
    return status;
}

#endif  // XR_PROGRAM_MODULE_FIXTURE_H
