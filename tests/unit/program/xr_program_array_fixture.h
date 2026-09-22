#ifndef XR_PROGRAM_ARRAY_FIXTURE_H
#define XR_PROGRAM_ARRAY_FIXTURE_H

#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"
#include <stdio.h>
#include <string.h>

/* Four independent layouts: owned strings, empty strings, repeated trivial
 * integers, and nested owned arrays. The copy outlives the original owner. */
static XrProgramBuildStatus xr_program_array_fixture_write(
    unsigned scenario, unsigned mutation, XrProgramArtifact *artifact) {
    uint16_t element = scenario == 2u || scenario == 4u ? XR_CORE_TYPE_I64 : XR_CORE_TYPE_STRING;
    XrCoreIrKey keys[10];
    for (unsigned i = 0u; i < 10u; ++i) {
        uint8_t material[2] = {0xa7, (uint8_t)i};
        keys[i] = xr_core_ir_key(material, sizeof(material));
    }
    XrCoreIrTypeInput types[] = {
        {.key = keys[7], .local_id = XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE,
         .kind = XR_CORE_IR_TYPE_ARRAY, .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
         .copy_contract = XR_CORE_IR_COPY_EXPLICIT, .array_element_type = element},
        {.key = keys[8], .local_id = XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + 1u,
         .kind = XR_CORE_IR_TYPE_ARRAY, .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
         .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
         .array_element_type = XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE},
    };
    static const uint8_t text[] = {'a', 0xe4, 0xb8, 0x96};
    XrCoreIrConstantInput constants[] = {
        {.key = keys[0], .type_id = element,
         .kind = element == XR_CORE_TYPE_STRING ? XR_CORE_IR_CONSTANT_STRING : XR_CORE_IR_CONSTANT_I64},
        {.key = keys[1], .type_id = element,
         .kind = element == XR_CORE_TYPE_STRING ? XR_CORE_IR_CONSTANT_STRING : XR_CORE_IR_CONSTANT_I64},
    };
    for (unsigned i = 0u; i < 2u; ++i) {
        if (element == XR_CORE_TYPE_STRING) {
            constants[i].value.string.bytes = text;
            constants[i].value.string.size = sizeof(text);
        } else {
            constants[i].value.i64 = 42;
        }
    }
    XrCoreIrInstructionInput instructions[12] = {0};
    uint32_t count = 0u;
    uint32_t elements = scenario == 1u ? 0u : 2u;
    for (unsigned i = 0u; i < elements; ++i) {
        instructions[count++] = (XrCoreIrInstructionInput){
            .operation_id = element == XR_CORE_TYPE_STRING ? XR_CORE_OP_CORE_CONSTANT_STRING
                                                           : XR_CORE_OP_CORE_CONSTANT_I64,
            .result = keys[i], .result_type_id = element,
            .result_ownership = element == XR_CORE_TYPE_STRING ? XR_CORE_IR_OWNER : XR_CORE_IR_NON_OWNER,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = keys[i]};
    }
    XrCoreIrKey operands[] = {keys[0], mutation == 2u || scenario == 2u ? keys[0] : keys[1]};
    uint32_t construct = count;
    instructions[count++] = (XrCoreIrInstructionInput){
        .operation_id = XR_CORE_OP_CORE_ARRAY_CONSTRUCT, .result = keys[2],
        .result_type_id = types[0].local_id, .result_ownership = XR_CORE_IR_OWNER,
        .operands = elements ? operands : NULL, .operand_count = elements};
    uint16_t array_type = types[0].local_id;
    XrCoreIrKey original = keys[2];
    if (scenario == 3u) {
        instructions[count++] = (XrCoreIrInstructionInput){
            .operation_id = XR_CORE_OP_CORE_ARRAY_CONSTRUCT, .result = keys[6],
            .result_type_id = types[1].local_id, .result_ownership = XR_CORE_IR_OWNER,
            .operands = &keys[2], .operand_count = 1u};
        array_type = types[1].local_id;
        original = keys[6];
    }
    XrCoreIrKey alias_elements[] = {keys[2], keys[9], keys[3]};
    if (scenario == 4u) {
        instructions[count++] = (XrCoreIrInstructionInput){
            .operation_id = XR_CORE_OP_CORE_OWNER_ALIAS, .result = keys[9],
            .result_type_id = array_type, .result_ownership = XR_CORE_IR_OWNER,
            .operands = &original, .operand_count = 1u};
    }
    uint32_t copy = count;
    instructions[count++] = (XrCoreIrInstructionInput){
        .operation_id = XR_CORE_OP_CORE_OWNER_COPY, .result = keys[3],
        .result_type_id = array_type, .result_ownership = XR_CORE_IR_OWNER,
        .operands = &original, .operand_count = 1u};
    uint32_t drop = count;
    XrCoreIrKey observed = mutation == 4u ? keys[0] : mutation == 5u ? original : keys[3];
    if (scenario == 4u) {
        instructions[count++] = (XrCoreIrInstructionInput){
            .operation_id = XR_CORE_OP_CORE_ARRAY_CONSTRUCT, .result = keys[6],
            .result_type_id = types[1].local_id, .result_ownership = XR_CORE_IR_OWNER,
            .operands = alias_elements, .operand_count = 3u};
        instructions[count++] = (XrCoreIrInstructionInput){
            .operation_id = XR_CORE_OP_CORE_RETURN, .operands = &keys[6], .operand_count = 1u};
    } else {
    instructions[count++] = (XrCoreIrInstructionInput){
        .operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &original, .operand_count = 1u};
    instructions[count++] = (XrCoreIrInstructionInput){
        .operation_id = XR_CORE_OP_CORE_SEQUENCE_LENGTH, .result = keys[4],
        .result_type_id = XR_CORE_TYPE_I64, .operands = &observed, .operand_count = 1u};
    instructions[count++] = (XrCoreIrInstructionInput){
        .operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &keys[3], .operand_count = 1u};
    instructions[count++] = (XrCoreIrInstructionInput){
        .operation_id = XR_CORE_OP_CORE_RETURN, .operands = &keys[4], .operand_count = 1u};
    }
    if (mutation == 1u)
        types[0].array_element_type = XR_CORE_TYPE_I64;
    if (mutation == 3u)
        instructions[construct].result_ownership = XR_CORE_IR_NON_OWNER;
    if (mutation == 6u) {
        XrCoreIrInstructionInput temporary = instructions[copy];
        instructions[copy] = instructions[drop];
        instructions[drop] = temporary;
    }
    if (mutation == 7u)
        instructions[construct].immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
    if (scenario == 4u) {
        if (mutation == 8u) instructions[3].result_ownership = XR_CORE_IR_NON_OWNER;
        if (mutation == 9u) instructions[3].immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
        if (mutation == 10u) instructions[3].operands = &keys[0];
        if (mutation == 11u) {
            memmove(&instructions[4], &instructions[3], (count - 3u) * sizeof(*instructions));
            instructions[3] = (XrCoreIrInstructionInput){
                .operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &keys[2], .operand_count = 1u};
            ++count;
        }
    }
    XrCoreIrBlockInput block = {.key = keys[5], .instructions = instructions, .instruction_count = count};
    XrCoreIrFunctionInput function = {.key = keys[5], .result_type_id = scenario == 4u ? types[1].local_id : XR_CORE_TYPE_I64,
        .result_ownership = scenario == 4u ? XR_CORE_IR_OWNER : XR_CORE_IR_NON_OWNER,
        .entry_block = keys[5], .blocks = &block, .block_count = 1u, .flags = XR_PROGRAM_FUNCTION_ENTRY};
    XrCoreIrModuleInput module = {.key = keys[5],
        .constants = elements ? constants : NULL, .constant_count = elements, .functions = &function, .function_count = 1u};
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {.semantic_profile_fingerprint = keys[5].bytes,
        .required_features = &feature, .required_feature_count = 1u, .modules = &module, .module_count = 1u,
        .types = types, .type_count = scenario >= 3u ? 2u : 1u};
    XrCoreIrProgram *program = NULL;
    char diagnostic[256] = {0};
    XrProgramBuildStatus status = xr_core_ir_program_build(&input, &program, diagnostic, sizeof(diagnostic));
    if (status != XR_PROGRAM_BUILD_OK)
        fprintf(stderr, "array fixture %u/%u: %s: %s\n", scenario, mutation, xr_program_build_status_name(status), diagnostic);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, NULL, 0u);
    xr_core_ir_program_free(program);
    return status;
}


/* A checked element place shares the array root and its panic cleanup edge. */
static inline XrProgramBuildStatus xr_program_array_place_fixture_write(
    unsigned mutation, XrProgramArtifact *artifact) {
    XrCoreIrKey key[12];
    for (uint8_t i = 0u; i < 12u; ++i) {
        uint8_t material[] = {0xa8, i};
        key[i] = xr_core_ir_key(material, sizeof(material));
    }
    XrCoreIrTypeInput type = {
        .key = key[11], .local_id = XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE,
        .kind = XR_CORE_IR_TYPE_ARRAY, .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT, .array_element_type = XR_CORE_TYPE_I64,
    };
    XrCoreIrConstantInput constants[] = {
        {.key = key[0], .type_id = XR_CORE_TYPE_I64, .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 42},
        {.key = key[1], .type_id = XR_CORE_TYPE_I64, .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 0},
    };
    XrCoreIrKey place_operands[] = {key[2], key[1], key[2]};
    XrCoreIrKey store_operands[] = {key[3], key[0]};
    XrCoreIrInstructionInput entry[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64, .result = key[0],
         .result_type_id = XR_CORE_TYPE_I64, .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = key[0]},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64, .result = key[1],
         .result_type_id = XR_CORE_TYPE_I64, .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = key[1]},
        {.operation_id = XR_CORE_OP_CORE_ARRAY_CONSTRUCT, .result = key[2],
         .result_type_id = type.local_id, .result_ownership = XR_CORE_IR_OWNER,
         .operands = &key[0], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_SEQUENCE_ELEMENT_PLACE, .result = key[3],
         .result_type_id = XR_CORE_TYPE_I64, .result_category = XR_CORE_IR_PLACE,
         .operands = place_operands, .operand_count = 3u,
         .successors = &key[9], .successor_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_PLACE_STORE,
         .operands = store_operands, .operand_count = 2u},
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOAD, .result = key[4],
         .result_type_id = XR_CORE_TYPE_I64, .operands = &key[3], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &key[2], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN, .operands = &key[4], .operand_count = 1u},
    };
    XrCoreIrInstructionInput cleanup[] = {
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &key[6], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_PANIC_PUBLISH, .operands = &key[5], .operand_count = 1u},
    };
    XrCoreIrValueInput cleanup_args[] = {
        {.key = key[5], .type_id = XR_CORE_TYPE_PANIC_INFO, .ownership = XR_CORE_IR_OWNER},
        {.key = key[6], .type_id = type.local_id, .ownership = XR_CORE_IR_OWNER},
    };
    XrCoreIrBlockInput blocks[] = {
        {.key = key[8], .instructions = entry, .instruction_count = 8u},
        {.key = key[9], .arguments = cleanup_args,
         .argument_count = 2u, .instructions = cleanup, .instruction_count = 2u},
    };
    if (mutation == 1u) entry[3].result_type_id = XR_CORE_TYPE_U64;
    if (mutation == 2u) place_operands[0] = key[0];
    if (mutation == 3u) place_operands[1] = key[2];
    if (mutation == 4u) {
        entry[3].successor_count = 0u;
        entry[3].successors = NULL;
        entry[3].operand_count = 2u;
    }
    if (mutation == 5u) {
        XrCoreIrInstructionInput temporary = entry[6];
        entry[6] = entry[5];
        entry[5] = temporary;
    }
    if (mutation == 6u) entry[3].immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
    if (mutation == 7u) cleanup[0].operands = &key[5];
    if (mutation == 10u) constants[1].value.i64 = 1;
    if (mutation == 11u) constants[1].value.i64 = -1;
    if (mutation == 12u) constants[1].value.i64 = INT64_MAX;
    if (mutation == 13u) constants[1].value.i64 = INT64_MIN;
    if (mutation == 14u) {
        entry[2].operands = NULL;
        entry[2].operand_count = 0u;
    }
    XrCoreIrFunctionInput function = {
        .key = key[10], .flags = XR_PROGRAM_FUNCTION_ENTRY,
        .result_type_id = XR_CORE_TYPE_I64, .panic_type_id = XR_CORE_TYPE_PANIC_INFO,
        .effect_mask = XR_CORE_EFFECT_PANIC, .entry_block = key[8],
        .blocks = blocks, .block_count = 2u,
    };
    XrParamMode read_mode = XR_PARAM_READ;
    XrCoreIrValueInput parameter = {.key = key[2], .type_id = type.local_id};
    if (mutation == 8u || mutation == 9u) {
        function.parameter_types = &type.local_id;
        function.parameter_modes = &read_mode;
        function.parameter_count = 1u;
        function.block_count = 1u;
        blocks[0].arguments = &parameter;
        blocks[0].argument_count = 1u;
        entry[3].successors = NULL;
        entry[3].successor_count = 0u;
        entry[3].operand_count = 2u;
        entry[6] = entry[7];
        memmove(&entry[2], &entry[3], 4u * sizeof(entry[0]));
        blocks[0].instruction_count = 6u;
        if (mutation == 8u) {
            memmove(&entry[3], &entry[4], 2u * sizeof(entry[0]));
            blocks[0].instruction_count = 5u;
        }
    }
    XrCoreIrModuleInput module = {.key = key[10], .constants = constants, .constant_count = 2u,
        .functions = &function, .function_count = 1u};
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {.semantic_profile_fingerprint = key[10].bytes,
        .types = &type, .type_count = 1u, .required_features = &feature,
        .required_feature_count = 1u, .modules = &module, .module_count = 1u};
    XrCoreIrProgram *program = NULL;
    char diagnostic[256] = {0};
    XrProgramBuildStatus status = xr_core_ir_program_build(&input, &program, diagnostic, sizeof(diagnostic));
    if (status == XR_PROGRAM_BUILD_OK) status = xr_program_write(program, artifact, NULL, 0u);
    else fprintf(stderr, "array place fixture %u: %s\n", mutation, diagnostic);
    xr_core_ir_program_free(program);
    return status;
}

/* A borrowed managed element is distinct from both its slot and a copied owner. */
static inline XrProgramBuildStatus xr_program_array_loan_fixture_write(
    unsigned scenario, XrProgramArtifact *artifact) {
    XrCoreIrKey k[32];
    for (uint8_t n = 0u; n < 32u; ++n) {
        uint8_t material[] = {0xa9, n};
        k[n] = xr_core_ir_key(material, sizeof(material));
    }
    XrCoreIrTypeInput type = {.key = k[15], .local_id = XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE,
        .kind = XR_CORE_IR_TYPE_ARRAY, .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT, .array_element_type = XR_CORE_TYPE_STRING};
    XrCoreIrTypeInput types[] = {type, {.key = k[21],
        .local_id = XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + 1u,
        .kind = XR_CORE_IR_TYPE_ARRAY, .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT, .array_element_type = type.local_id}};
    bool nested = scenario >= 8u;
    static const uint8_t original[] = {'o', 'l', 'd'};
    static const uint8_t replacement[] = {'n', 'e', 'w', 'e', 'r'};
    XrCoreIrConstantInput constants[] = {
        {.key = k[0], .type_id = XR_CORE_TYPE_STRING, .kind = XR_CORE_IR_CONSTANT_STRING,
         .value.string = {.bytes = original, .size = sizeof(original)}},
        {.key = k[1], .type_id = XR_CORE_TYPE_I64, .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 0},
        {.key = k[6], .type_id = XR_CORE_TYPE_STRING, .kind = XR_CORE_IR_CONSTANT_STRING,
         .value.string = {.bytes = replacement, .size = sizeof(replacement)}},
    };
    XrCoreIrKey place[] = {k[2], k[1], k[2]}, exchange[] = {k[3], k[6]};
    XrCoreIrInstructionInput instructions[24] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_STRING, .result = k[0],
         .result_type_id = XR_CORE_TYPE_STRING, .result_ownership = XR_CORE_IR_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = k[0]},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64, .result = k[1],
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = k[1]},
        {.operation_id = XR_CORE_OP_CORE_ARRAY_CONSTRUCT, .result = k[2],
         .result_type_id = type.local_id, .result_ownership = XR_CORE_IR_OWNER,
         .operands = &k[0], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_SEQUENCE_ELEMENT_PLACE, .result = k[3],
         .result_type_id = XR_CORE_TYPE_STRING, .result_category = XR_CORE_IR_PLACE,
         .operands = place, .operand_count = 3u, .successors = &k[12], .successor_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOAD, .result = k[4],
         .result_type_id = XR_CORE_TYPE_STRING, .operands = &k[3], .operand_count = 1u},
    };
    XrCoreIrKey alternate_place[] = {k[2], k[16], k[2]};
    XrCoreIrKey dynamic_index[] = {k[1], k[1]};
    uint32_t count = 5u;
    XrCoreIrKey inner_place[] = {k[4], k[1], k[18]};
    if (nested) {
        memmove(&instructions[4], &instructions[3], 2u * sizeof(instructions[0]));
        instructions[3] = (XrCoreIrInstructionInput){
            .operation_id = XR_CORE_OP_CORE_ARRAY_CONSTRUCT, .result = k[18],
            .result_type_id = types[1].local_id, .result_ownership = XR_CORE_IR_OWNER,
            .operands = &k[2], .operand_count = 1u};
        place[0] = k[18]; place[2] = k[18];
        instructions[4].result_type_id = type.local_id;
        instructions[5].result_type_id = type.local_id;
        count = 6u;
        instructions[count++] = (XrCoreIrInstructionInput){
            .operation_id = XR_CORE_OP_CORE_SEQUENCE_ELEMENT_PLACE, .result = k[17],
            .result_type_id = XR_CORE_TYPE_STRING, .result_category = XR_CORE_IR_PLACE,
            .operands = inner_place, .operand_count = 3u,
            .successors = &k[12], .successor_count = 1u};
        instructions[count++] = (XrCoreIrInstructionInput){
            .operation_id = XR_CORE_OP_CORE_PLACE_LOAD, .result = k[19],
            .result_type_id = XR_CORE_TYPE_STRING, .operands = &k[17], .operand_count = 1u};
    }
    if (scenario >= 4u && !nested) {
        instructions[count++] = scenario == 6u ? (XrCoreIrInstructionInput){
            .operation_id = XR_CORE_OP_CORE_ADD_I64, .result = k[16],
            .result_type_id = XR_CORE_TYPE_I64, .operands = dynamic_index, .operand_count = 2u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_U32, .immediate.u32 = 1u}
            : (XrCoreIrInstructionInput){
            .operation_id = XR_CORE_OP_CORE_CONSTANT_I64, .result = k[16],
            .result_type_id = XR_CORE_TYPE_I64, .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
            .immediate.key = k[1]};
        instructions[count++] = (XrCoreIrInstructionInput){
            .operation_id = XR_CORE_OP_CORE_SEQUENCE_ELEMENT_PLACE, .result = k[17],
            .result_type_id = XR_CORE_TYPE_STRING, .result_category = XR_CORE_IR_PLACE,
            .operands = alternate_place, .operand_count = 3u,
            .successors = &k[12], .successor_count = 1u};
        exchange[0] = k[17];
    }
    if (scenario == 2u)
        instructions[count++] = (XrCoreIrInstructionInput){
            .operation_id = XR_CORE_OP_CORE_OWNER_COPY, .result = k[5],
            .result_type_id = XR_CORE_TYPE_STRING, .result_ownership = XR_CORE_IR_OWNER,
            .operands = &k[4], .operand_count = 1u};
    XrCoreIrInstructionInput length = {.operation_id = XR_CORE_OP_CORE_SEQUENCE_LENGTH,
        .result = k[8], .result_type_id = XR_CORE_TYPE_I64,
        .operands = &k[nested ? 19u : scenario == 2u ? 5u : 4u], .operand_count = 1u};
    if (scenario == 0u) instructions[count++] = length;
    instructions[count++] = (XrCoreIrInstructionInput){
        .operation_id = XR_CORE_OP_CORE_CONSTANT_STRING, .result = k[6],
        .result_type_id = XR_CORE_TYPE_STRING, .result_ownership = XR_CORE_IR_OWNER,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = k[6]};
    if (nested) {
        instructions[count++] = (XrCoreIrInstructionInput){
            .operation_id = XR_CORE_OP_CORE_ARRAY_CONSTRUCT, .result = k[20],
            .result_type_id = type.local_id, .result_ownership = XR_CORE_IR_OWNER,
            .operands = &k[6], .operand_count = 1u};
        exchange[1] = k[20];
    }
    instructions[count++] = (XrCoreIrInstructionInput){
        .operation_id = XR_CORE_OP_CORE_PLACE_EXCHANGE, .result = k[7],
        .result_type_id = nested ? type.local_id : XR_CORE_TYPE_STRING, .result_ownership = XR_CORE_IR_OWNER,
        .operands = exchange, .operand_count = 2u};
    if (scenario == 3u || scenario == 5u || scenario == 8u) instructions[count++] = length;
    instructions[count++] = (XrCoreIrInstructionInput){
        .operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &k[7], .operand_count = 1u};
    if (scenario == 1u || scenario == 2u || scenario == 4u || scenario == 6u || scenario == 9u) instructions[count++] = length;
    instructions[count++] = (XrCoreIrInstructionInput){
        .operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &k[nested ? 18u : 2u], .operand_count = 1u};
    if (scenario == 2u)
        instructions[count++] = (XrCoreIrInstructionInput){
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &k[5], .operand_count = 1u};
    instructions[count++] = (XrCoreIrInstructionInput){
        .operation_id = XR_CORE_OP_CORE_RETURN, .operands = &k[8], .operand_count = 1u};
    XrCoreIrValueInput arguments[] = {
        {.key = k[9], .type_id = XR_CORE_TYPE_PANIC_INFO, .ownership = XR_CORE_IR_OWNER},
        {.key = k[10], .type_id = nested ? types[1].local_id : type.local_id, .ownership = XR_CORE_IR_OWNER},
    };
    XrCoreIrInstructionInput cleanup[] = {
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &k[10], .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_PANIC_PUBLISH, .operands = &k[9], .operand_count = 1u},
    };
    XrCoreIrBlockInput blocks[3] = {
        {.key = k[11], .instructions = instructions, .instruction_count = count},
        {.key = k[12], .arguments = arguments, .argument_count = 2u,
         .instructions = cleanup, .instruction_count = 2u},
    };
    XrCoreIrFunctionInput function = {.key = k[13], .flags = XR_PROGRAM_FUNCTION_ENTRY,
        .result_type_id = XR_CORE_TYPE_I64, .panic_type_id = XR_CORE_TYPE_PANIC_INFO,
        .effect_mask = XR_CORE_EFFECT_PANIC, .entry_block = k[11], .blocks = blocks, .block_count = 2u};
    XrCoreIrKey crossing[] = {k[4], k[18], k[1]};
    XrCoreIrValueInput continuation_args[] = {
        {.key = k[24], .type_id = type.local_id},
        {.key = k[25], .type_id = types[1].local_id, .ownership = XR_CORE_IR_OWNER},
        {.key = k[26], .type_id = XR_CORE_TYPE_I64},
    };
    XrCoreIrInstructionInput continuation[12] = {0};
    XrCoreIrKey outer_place[] = {k[25], k[26], k[25]};
    XrCoreIrKey replace_outer[] = {k[28], k[20]};
    if (scenario == 10u || scenario == 11u || scenario == 12u) {
        uint32_t next = 0u;
        if (scenario == 11u)
            continuation[next++] = (XrCoreIrInstructionInput){
                .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
                .operands = &k[25], .operand_count = 1u};
        if (scenario == 12u) {
            continuation[next++] = (XrCoreIrInstructionInput){
                .operation_id = XR_CORE_OP_CORE_SEQUENCE_ELEMENT_PLACE, .result = k[28],
                .result_type_id = type.local_id, .result_category = XR_CORE_IR_PLACE,
                .operands = outer_place, .operand_count = 3u,
                .successors = &k[12], .successor_count = 1u};
            continuation[next++] = (XrCoreIrInstructionInput){
                .operation_id = XR_CORE_OP_CORE_CONSTANT_STRING, .result = k[6],
                .result_type_id = XR_CORE_TYPE_STRING, .result_ownership = XR_CORE_IR_OWNER,
                .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = k[6]};
            continuation[next++] = (XrCoreIrInstructionInput){
                .operation_id = XR_CORE_OP_CORE_ARRAY_CONSTRUCT, .result = k[20],
                .result_type_id = type.local_id, .result_ownership = XR_CORE_IR_OWNER,
                .operands = &k[6], .operand_count = 1u};
            continuation[next++] = (XrCoreIrInstructionInput){
                .operation_id = XR_CORE_OP_CORE_PLACE_EXCHANGE, .result = k[7],
                .result_type_id = type.local_id, .result_ownership = XR_CORE_IR_OWNER,
                .operands = replace_outer, .operand_count = 2u};
            continuation[next++] = (XrCoreIrInstructionInput){
                .operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &k[7], .operand_count = 1u};
        }
        inner_place[0] = k[24]; inner_place[1] = k[26]; inner_place[2] = k[25];
        continuation[next++] = instructions[6];
        continuation[next++] = instructions[7];
        continuation[next++] = length;
        if (scenario == 10u || scenario == 12u)
            continuation[next++] = (XrCoreIrInstructionInput){
                .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
                .operands = &k[25], .operand_count = 1u};
        continuation[next++] = (XrCoreIrInstructionInput){
            .operation_id = XR_CORE_OP_CORE_RETURN, .operands = &k[8], .operand_count = 1u};
        instructions[6] = (XrCoreIrInstructionInput){
            .operation_id = XR_CORE_OP_CORE_BRANCH, .operands = crossing, .operand_count = 3u,
            .successors = &k[27], .successor_count = 1u};
        blocks[0].instruction_count = 7u;
        blocks[2] = (XrCoreIrBlockInput){.key = k[27], .arguments = continuation_args,
            .argument_count = 3u, .instructions = continuation, .instruction_count = next};
        function.block_count = 3u;
    }
    XrCoreIrModuleInput module = {.key = k[14], .functions = &function, .function_count = 1u,
        .constants = constants, .constant_count = 3u};
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {.semantic_profile_fingerprint = k[14].bytes,
        .required_features = &feature, .required_feature_count = 1u,
        .types = types, .type_count = nested ? 2u : 1u, .modules = &module, .module_count = 1u};
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status = xr_core_ir_program_build(&input, &program, NULL, 0u);
    if (status == XR_PROGRAM_BUILD_OK) status = xr_program_write(program, artifact, NULL, 0u);
    xr_core_ir_program_free(program);
    return status;
}

#endif // XR_PROGRAM_ARRAY_FIXTURE_H
