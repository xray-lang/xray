#ifndef XR_PROGRAM_TEXT_FIXTURE_H
#define XR_PROGRAM_TEXT_FIXTURE_H

#include "core/xr_core_spec_gen.h"
#include "plan/semantic/xr_semantic_ids.h"
#include "program/xr_program.h"
#include "runtime/abi/xr_builtin_provider_contract.h"

#include <string.h>

/* One canonical program exercising the string/rune family end to end:
 * constants, concatenation, i64 rendering, ordered comparison, explicit copy,
 * typed output groups and exactly-once owner release.  The valid shape
 * writes XR_PROGRAM_TEXT_FIXTURE_STDOUT and returns 0; every mutation breaks
 * exactly one contract so verifiers can be probed one rule at a time. */
typedef enum XrProgramTextFixtureMutation {
    XR_PROGRAM_TEXT_FIXTURE_VALID = 0,
    /* concat's right operand is the i64 constant instead of a string */
    XR_PROGRAM_TEXT_FIXTURE_CONCAT_I64_OPERAND,
    /* constant.string names the i64 constant row */
    XR_PROGRAM_TEXT_FIXTURE_CONSTANT_KIND_MISMATCH,
    /* compare.rune reads a string operand */
    XR_PROGRAM_TEXT_FIXTURE_RUNE_COMPARE_STRING_OPERAND,
    /* the concatenation result is dropped twice */
    XR_PROGRAM_TEXT_FIXTURE_DOUBLE_DROP,
    /* the concatenation result is produced as a non-owner */
    XR_PROGRAM_TEXT_FIXTURE_CONCAT_NON_OWNER,
    XR_PROGRAM_TEXT_FIXTURE_SCALAR_STRING_OPERAND,
} XrProgramTextFixtureMutation;

#define XR_PROGRAM_TEXT_FIXTURE_STDOUT "-5 true abcd \xF0\x9F\x98\x80 true abcd\nabcd\n\n"

static XrCoreIrKey xr_program_text_fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrProgramBuildStatus
xr_program_text_fixture_write_mutated(XrProgramTextFixtureMutation mutation,
                                      XrProgramArtifact *artifact, char *diagnostic,
                                      size_t diagnostic_size) {
    XrStableId contract_id = {{0}};
    XrStableId operation_id = {{0}};
    XrFingerprint key_digest;
    if (!xr_stable_id_from_key(XR_PROVIDER_IO_CONTRACT_KEY, &contract_id, &key_digest) ||
        !xr_stable_id_from_key(XR_PROVIDER_IO_OUTPUT_WRITE_OPERATION_KEY, &operation_id,
                               &key_digest))
        return XR_PROGRAM_BUILD_INVALID_INPUT;

    static const uint8_t text_ab[] = {'a', 'b'};
    static const uint8_t text_cd[] = {'c', 'd'};
    static const uint8_t text_abcd[] = {'a', 'b', 'c', 'd'};
    enum {
        CONSTANT_AB = 0,
        CONSTANT_CD,
        CONSTANT_ABCD,
        CONSTANT_EMPTY,
        CONSTANT_MINUS_FIVE,
        CONSTANT_ZERO,
        CONSTANT_RUNE_A,
        CONSTANT_RUNE_SMILE,
    };
    XrCoreIrConstantInput constants[] = {
        {.key = xr_program_text_fixture_key("text:ab"),
         .type_id = XR_CORE_TYPE_STRING,
         .kind = XR_CORE_IR_CONSTANT_STRING,
         .value.string = {text_ab, 2u}},
        {.key = xr_program_text_fixture_key("text:cd"),
         .type_id = XR_CORE_TYPE_STRING,
         .kind = XR_CORE_IR_CONSTANT_STRING,
         .value.string = {text_cd, 2u}},
        {.key = xr_program_text_fixture_key("text:abcd"),
         .type_id = XR_CORE_TYPE_STRING,
         .kind = XR_CORE_IR_CONSTANT_STRING,
         .value.string = {text_abcd, 4u}},
        {.key = xr_program_text_fixture_key("text:empty"),
         .type_id = XR_CORE_TYPE_STRING,
         .kind = XR_CORE_IR_CONSTANT_STRING,
         .value.string = {NULL, 0u}},
        {.key = xr_program_text_fixture_key("text:minus-five"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = -5},
        {.key = xr_program_text_fixture_key("text:zero"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 0},
        {.key = xr_program_text_fixture_key("text:rune-a"),
         .type_id = XR_CORE_TYPE_RUNE,
         .kind = XR_CORE_IR_CONSTANT_RUNE,
         .value.rune = 0x41u},
        {.key = xr_program_text_fixture_key("text:rune-smile"),
         .type_id = XR_CORE_TYPE_RUNE,
         .kind = XR_CORE_IR_CONSTANT_RUNE,
         .value.rune = 0x1F600u},
    };

    XrCoreIrKey ab = xr_program_text_fixture_key("text:v:ab");
    XrCoreIrKey cd = xr_program_text_fixture_key("text:v:cd");
    XrCoreIrKey joined = xr_program_text_fixture_key("text:v:joined");
    XrCoreIrKey minus_five = xr_program_text_fixture_key("text:v:minus-five");
    XrCoreIrKey rendered = xr_program_text_fixture_key("text:v:rendered");
    XrCoreIrKey abcd = xr_program_text_fixture_key("text:v:abcd");
    XrCoreIrKey equal = xr_program_text_fixture_key("text:v:equal");
    XrCoreIrKey less = xr_program_text_fixture_key("text:v:less");
    XrCoreIrKey rune_a = xr_program_text_fixture_key("text:v:rune-a");
    XrCoreIrKey rune_smile = xr_program_text_fixture_key("text:v:rune-smile");
    XrCoreIrKey rune_less = xr_program_text_fixture_key("text:v:rune-less");
    XrCoreIrKey copy = xr_program_text_fixture_key("text:v:copy");
    XrCoreIrKey empty = xr_program_text_fixture_key("text:v:empty");
    XrCoreIrKey zero = xr_program_text_fixture_key("text:v:zero");

    XrCoreIrKey concat_operands[] = {
        ab, mutation == XR_PROGRAM_TEXT_FIXTURE_CONCAT_I64_OPERAND ? minus_five : cd};
    XrCoreIrKey drop_ab_operands[] = {ab};
    XrCoreIrKey drop_cd_operands[] = {cd};
    XrCoreIrKey rendered_operands[] = {mutation == XR_PROGRAM_TEXT_FIXTURE_SCALAR_STRING_OPERAND ? joined : minus_five};
    XrCoreIrKey equal_operands[] = {joined, abcd};
    XrCoreIrKey less_operands[] = {rendered, abcd};
    XrCoreIrKey rune_less_operands[] = {
        mutation == XR_PROGRAM_TEXT_FIXTURE_RUNE_COMPARE_STRING_OPERAND ? abcd : rune_a,
        rune_smile};
    XrCoreIrKey group_operands[] = {rendered, equal, joined, rune_smile, rune_less, abcd};
    XrCoreIrKey copy_operands[] = {joined};
    XrCoreIrKey copy_group_operands[] = {copy};
    XrCoreIrKey drop_joined_operands[] = {joined};
    XrCoreIrKey drop_abcd_operands[] = {abcd};
    XrCoreIrKey drop_rendered_operands[] = {rendered};
    XrCoreIrKey drop_copy_operands[] = {copy};
    XrCoreIrKey empty_group_operands[] = {empty};
    XrCoreIrKey drop_empty_operands[] = {mutation == XR_PROGRAM_TEXT_FIXTURE_DOUBLE_DROP ? joined
                                                                                         : empty};
    XrCoreIrKey return_operands[] = {zero};

    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_STRING,
         .result = ab,
         .result_type_id = XR_CORE_TYPE_STRING,
         .result_ownership = XR_CORE_IR_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[CONSTANT_AB].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_STRING,
         .result = cd,
         .result_type_id = XR_CORE_TYPE_STRING,
         .result_ownership = XR_CORE_IR_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[mutation == XR_PROGRAM_TEXT_FIXTURE_CONSTANT_KIND_MISMATCH
                                        ? CONSTANT_MINUS_FIVE
                                        : CONSTANT_CD]
                              .key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = minus_five,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[CONSTANT_MINUS_FIVE].key},
        {.operation_id = XR_CORE_OP_CORE_STRING_CONCAT,
         .result = joined,
         .result_type_id = XR_CORE_TYPE_STRING,
         .result_ownership = mutation == XR_PROGRAM_TEXT_FIXTURE_CONCAT_NON_OWNER
                                 ? XR_CORE_IR_NON_OWNER
                                 : XR_CORE_IR_OWNER,
         .operands = concat_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = drop_ab_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = drop_cd_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_STRING_FROM_SCALAR,
         .result = rendered,
         .result_type_id = XR_CORE_TYPE_STRING,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = rendered_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_STRING,
         .result = abcd,
         .result_type_id = XR_CORE_TYPE_STRING,
         .result_ownership = XR_CORE_IR_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[CONSTANT_ABCD].key},
        {.operation_id = XR_CORE_OP_CORE_COMPARE_STRING,
         .result = equal,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = equal_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0u},
        {.operation_id = XR_CORE_OP_CORE_COMPARE_STRING,
         .result = less,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = less_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 2u},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_RUNE,
         .result = rune_a,
         .result_type_id = XR_CORE_TYPE_RUNE,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[CONSTANT_RUNE_A].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_RUNE,
         .result = rune_smile,
         .result_type_id = XR_CORE_TYPE_RUNE,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[CONSTANT_RUNE_SMILE].key},
        {.operation_id = XR_CORE_OP_CORE_COMPARE_RUNE,
         .result = rune_less,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = rune_less_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 2u},
        {.operation_id = XR_CORE_OP_CORE_OUTPUT_GROUP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = group_operands,
         .operand_count = 6u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
         .immediate.provider_operation = {.contract_id = contract_id,
                                          .operation_id = operation_id}},
        {.operation_id = XR_CORE_OP_CORE_OWNER_COPY,
         .result = copy,
         .result_type_id = XR_CORE_TYPE_STRING,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = copy_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OUTPUT_GROUP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = copy_group_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
         .immediate.provider_operation = {.contract_id = contract_id,
                                          .operation_id = operation_id}},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = drop_joined_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = drop_abcd_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = drop_rendered_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = drop_copy_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_STRING,
         .result = empty,
         .result_type_id = XR_CORE_TYPE_STRING,
         .result_ownership = XR_CORE_IR_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[CONSTANT_EMPTY].key},
        {.operation_id = XR_CORE_OP_CORE_OUTPUT_GROUP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = empty_group_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
         .immediate.provider_operation = {.contract_id = contract_id,
                                          .operation_id = operation_id}},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = drop_empty_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = zero,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[CONSTANT_ZERO].key},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = return_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = xr_program_text_fixture_key("text:block:entry");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = xr_program_text_fixture_key("text:function:entry"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_PROVIDER_CALL,
        .capability_mask = XR_CORE_CAPABILITY_PROVIDER_BINDING,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = xr_program_text_fixture_key("text:module"),
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
    XrCoreIrKey semantic = xr_program_text_fixture_key("text:semantic");
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

static XrProgramBuildStatus xr_program_text_fixture_write(XrProgramArtifact *artifact,
                                                          char *diagnostic,
                                                          size_t diagnostic_size) {
    return xr_program_text_fixture_write_mutated(XR_PROGRAM_TEXT_FIXTURE_VALID, artifact,
                                                 diagnostic, diagnostic_size);
}

#endif /* XR_PROGRAM_TEXT_FIXTURE_H */
