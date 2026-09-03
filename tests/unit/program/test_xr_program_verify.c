/*
 * Task 297: XrProgram semantic verifier and independent reference evaluator.
 */

#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"
#include "program/xr_program_verify.h"
#include "program/xr_reference_evaluator.h"
#include "xr_program_existential_fixture.h"
#include "xr_program_callable_fixture.h"
#include "xr_program_invoke_fixture.h"
#include "xr_program_panic_fixture.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(XR_CORE_OP_CORE_CALL_SEALED_INVOKE == 37, "sealed invoke stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CALL_WITNESS_DIRECT == 40, "witness direct stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CALL_WITNESS_INVOKE == 41, "witness invoke stable id drifted");
_Static_assert(XR_CORE_OP_CORE_PANIC_PUBLISH == 50, "panic publish stable id drifted");
_Static_assert(XR_CORE_OP_CORE_EXISTENTIAL_PACK == 86, "existential pack stable id drifted");
_Static_assert(XR_CORE_OP_CORE_EXISTENTIAL_TEST == 87, "existential test stable id drifted");
_Static_assert(XR_CORE_OP_CORE_EXISTENTIAL_PROJECT == 88, "existential project stable id drifted");

static int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static XrCoreIrKey key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrProgramBuildStatus write_typed_modules(const XrCoreIrTypeInput *types, uint32_t type_count,
                                                const XrCoreIrModuleInput *modules,
                                                uint32_t module_count,
                                                XrProgramArtifact *artifact) {
    XrCoreIrKey profile = key("task-297:reference-profile");
    uint16_t features[] = {XR_CORE_FEATURE_CORE_BASE};
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = profile.bytes,
        .required_features = features,
        .required_feature_count = 1,
        .types = types,
        .type_count = type_count,
        .modules = modules,
        .module_count = module_count,
    };
    XrCoreIrProgram *program = NULL;
    char diagnostic[256] = {0};
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&input, &program, diagnostic, sizeof(diagnostic));
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, sizeof(diagnostic));
    if (status != XR_PROGRAM_BUILD_OK)
        fprintf(stderr, "fixture build failed: %s\n", diagnostic);
    xr_core_ir_program_free(program);
    return status;
}

static XrProgramBuildStatus write_modules(const XrCoreIrModuleInput *modules, uint32_t module_count,
                                          XrProgramArtifact *artifact) {
    return write_typed_modules(NULL, 0u, modules, module_count, artifact);
}

static XrProgramBuildStatus write_one_function(const XrCoreIrConstantInput *constants,
                                               uint32_t constant_count,
                                               const XrCoreIrFunctionInput *function,
                                               XrProgramArtifact *artifact) {
    XrCoreIrModuleInput module = {
        .key = key("task-297:module"),
        .constants = constants,
        .constant_count = constant_count,
        .functions = function,
        .function_count = 1,
    };
    return write_modules(&module, 1, artifact);
}

static XrValidatedProgram *validate_ok(const XrProgramArtifact *artifact) {
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic diagnostic;
    XrProgramVerifyStatus status =
        xr_program_validate(artifact->bytes, artifact->size, NULL, &program, &diagnostic);
    if (status != XR_PROGRAM_VERIFY_OK)
        fprintf(stderr, "unexpected verify reject: %s/%s decode=%s at f=%u b=%u i=%u v=%u\n",
                xr_program_verify_status_name(status),
                xr_program_diagnostic_kind_name(diagnostic.kind),
                xr_program_decode_status_name(diagnostic.decode_status),
                diagnostic.location.function_id, diagnostic.location.block_id,
                diagnostic.location.instruction_id, diagnostic.location.value_id);
    CHECK(status == XR_PROGRAM_VERIFY_OK);
    CHECK(program != NULL);
    return program;
}

static void expect_semantic_reject_at(const XrProgramArtifact *artifact,
                                      XrProgramDiagnosticKind expected, int caller_line) {
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic first;
    XrProgramDiagnostic second;
    XrProgramVerifyStatus first_status =
        xr_program_validate(artifact->bytes, artifact->size, NULL, &program, &first);
    if (first_status != XR_PROGRAM_VERIFY_SEMANTIC_REJECTED || first.kind != expected)
        fprintf(
            stderr,
            "unexpected reject from line %d: status=%s diagnostic=%s expected=%s f=%u b=%u i=%u\n",
            caller_line, xr_program_verify_status_name(first_status),
            xr_program_diagnostic_kind_name(first.kind), xr_program_diagnostic_kind_name(expected),
            first.location.function_id, first.location.block_id, first.location.instruction_id);
    CHECK(first_status == XR_PROGRAM_VERIFY_SEMANTIC_REJECTED);
    CHECK(program == NULL);
    CHECK(first.kind == expected);
    XrProgramVerifyStatus second_status =
        xr_program_validate(artifact->bytes, artifact->size, NULL, &program, &second);
    CHECK(second_status == first_status);
    CHECK(memcmp(&first, &second, sizeof(first)) == 0);
    CHECK(program == NULL);
}

#define expect_semantic_reject(artifact, expected)                                                 \
    expect_semantic_reject_at((artifact), (expected), __LINE__)

static uint64_t test_take_uvar(const uint8_t *bytes, size_t size, size_t *offset) {
    uint64_t value = 0u;
    unsigned shift = 0u;
    for (unsigned count = 0u; count < 10u && *offset < size; ++count) {
        uint8_t byte = bytes[(*offset)++];
        value |= (uint64_t) (byte & UINT8_C(0x7f)) << shift;
        if ((byte & UINT8_C(0x80)) == 0u)
            return value;
        shift += 7u;
    }
    CHECK(false);
    return UINT64_MAX;
}

static void test_skip_instruction(const uint8_t *bytes, size_t size, size_t *offset) {
    (void) test_take_uvar(bytes, size, offset); /* operation */
    (void) test_take_uvar(bytes, size, offset); /* result + 1 */
    (void) test_take_uvar(bytes, size, offset); /* result type */
    (void) test_take_uvar(bytes, size, offset); /* result category */
    (void) test_take_uvar(bytes, size, offset); /* result ownership */
    uint64_t operands = test_take_uvar(bytes, size, offset);
    for (uint64_t operand = 0u; operand < operands; ++operand)
        (void) test_take_uvar(bytes, size, offset);
    uint64_t immediate = test_take_uvar(bytes, size, offset);
    if (immediate != XR_CORE_IR_IMMEDIATE_NONE) {
        (void) test_take_uvar(bytes, size, offset);
        if (immediate == XR_CORE_IR_IMMEDIATE_VARIANT_FIELD)
            (void) test_take_uvar(bytes, size, offset);
    }
    uint64_t successors = test_take_uvar(bytes, size, offset);
    for (uint64_t successor = 0u; successor < successors; ++successor)
        (void) test_take_uvar(bytes, size, offset);
}

static XrProgramBuildStatus build_mode_artifact(XrParamMode mode, XrCoreIrValueCategory category,
                                                bool branch_to_second_block,
                                                XrProgramArtifact *artifact) {
    XrCoreIrKey entry_key = key("mode:block:entry");
    XrCoreIrKey second_key = key("mode:block:second");
    XrCoreIrKey entry_argument = key("mode:value:entry");
    XrCoreIrKey second_argument = key("mode:value:second");
    XrCoreIrValueInput entry_arguments[] = {
        {.key = entry_argument, .type_id = XR_CORE_TYPE_I64, .category = category},
    };
    XrCoreIrValueInput second_arguments[] = {
        {.key = second_argument, .type_id = XR_CORE_TYPE_I64, .category = category},
    };
    XrCoreIrKey entry_argument_operand[] = {entry_argument};
    XrCoreIrKey second_argument_operand[] = {second_argument};
    XrCoreIrKey second_successor[] = {second_key};
    XrCoreIrInstructionInput entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = entry_argument_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = branch_to_second_block ? XR_CORE_OP_CORE_BRANCH : XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = branch_to_second_block ? entry_argument_operand : NULL,
         .operand_count = branch_to_second_block ? 1u : 0u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = branch_to_second_block ? second_successor : NULL,
         .successor_count = branch_to_second_block ? 1u : 0u},
    };
    XrCoreIrInstructionInput second_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = second_argument_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput blocks[] = {
        {.key = entry_key,
         .arguments = entry_arguments,
         .argument_count = 1u,
         .instructions = entry_instructions,
         .instruction_count = 2u},
        {.key = second_key,
         .arguments = second_arguments,
         .argument_count = 1u,
         .instructions = second_instructions,
         .instruction_count = 2u},
    };
    uint16_t parameter_type = XR_CORE_TYPE_I64;
    XrCoreIrFunctionInput function = {
        .key = key("mode:function"),
        .parameter_types = &parameter_type,
        .parameter_modes = &mode,
        .parameter_count = 1u,
        .result_type_id = XR_CORE_TYPE_VOID,
        .entry_block = entry_key,
        .blocks = blocks,
        .block_count = branch_to_second_block ? 2u : 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return write_one_function(NULL, 0u, &function, artifact);
}

static bool mode_fixture_offsets(const XrProgramArtifact *artifact, size_t *mode_offset,
                                 size_t category_offsets[2], uint32_t *entry_block_out,
                                 uint32_t *block_count_out) {
    XrProgramView view;
    char diagnostic[256] = {0};
    if (xr_program_decode_structure(artifact->bytes, artifact->size, NULL, &view, diagnostic,
                                    sizeof(diagnostic)) != XR_PROGRAM_DECODE_OK)
        return false;

    const XrProgramSectionView *functions = &view.sections[XR_PROGRAM_SECTION_FUNCTIONS - 1u];
    size_t cursor = (size_t) functions->offset;
    uint64_t signature_count = test_take_uvar(artifact->bytes, artifact->size, &cursor);
    if (signature_count != 1u)
        return false;
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* signature id */
    uint64_t parameter_count = test_take_uvar(artifact->bytes, artifact->size, &cursor);
    if (parameter_count != 1u)
        return false;
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* parameter type */
    *mode_offset = cursor;
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor);
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* has receiver */
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* receiver mode */
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* result type */
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* result ownership */
    uint64_t origins = test_take_uvar(artifact->bytes, artifact->size, &cursor);
    for (uint64_t origin = 0u; origin < origins; ++origin) {
        uint64_t kind = test_take_uvar(artifact->bytes, artifact->size, &cursor);
        if (kind == XR_VIEW_ORIGIN_PARAM)
            (void) test_take_uvar(artifact->bytes, artifact->size, &cursor);
    }
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* error type */
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* panic type */
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* effects */
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* capabilities */
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* function count */
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* function id */
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* signature id */
    uint32_t entry_block = (uint32_t) test_take_uvar(artifact->bytes, artifact->size, &cursor);

    const XrProgramSectionView *code = &view.sections[XR_PROGRAM_SECTION_CODE - 1u];
    cursor = (size_t) code->offset;
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* function count */
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* function id */
    uint32_t block_count = (uint32_t) test_take_uvar(artifact->bytes, artifact->size, &cursor);
    if (block_count == 0u || block_count > 2u)
        return false;
    for (uint32_t block = 0u; block < block_count; ++block) {
        uint32_t block_id = (uint32_t) test_take_uvar(artifact->bytes, artifact->size, &cursor);
        uint64_t argument_count = test_take_uvar(artifact->bytes, artifact->size, &cursor);
        if (block_id >= 2u || argument_count != 1u)
            return false;
        (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* value id */
        (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* type id */
        category_offsets[block_id] = cursor;
        (void) test_take_uvar(artifact->bytes, artifact->size, &cursor);
        (void) test_take_uvar(artifact->bytes, artifact->size, &cursor); /* ownership */
        uint64_t instruction_count = test_take_uvar(artifact->bytes, artifact->size, &cursor);
        for (uint64_t instruction = 0u; instruction < instruction_count; ++instruction)
            test_skip_instruction(artifact->bytes, artifact->size, &cursor);
    }
    *entry_block_out = entry_block;
    *block_count_out = block_count;
    return true;
}

static void expect_mutated_verify(const XrProgramArtifact *artifact, size_t offset, uint8_t value,
                                  XrProgramVerifyStatus expected_status,
                                  XrProgramDiagnosticKind expected_diagnostic) {
    uint8_t *bytes = malloc(artifact->size);
    CHECK(bytes != NULL);
    if (!bytes)
        return;
    memcpy(bytes, artifact->bytes, artifact->size);
    bytes[offset] = value;
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic diagnostic;
    XrProgramVerifyStatus status =
        xr_program_validate(bytes, artifact->size, NULL, &program, &diagnostic);
    if (status != expected_status || diagnostic.kind != expected_diagnostic) {
        XrProgramView view;
        XrProgramDecodeStatus decode =
            xr_program_decode_structure(bytes, artifact->size, NULL, &view, NULL, 0u);
        fprintf(stderr,
                "mutated verify mismatch at %zu: status=%s diagnostic=%s decode=%s old=%u new=%u\n",
                offset, xr_program_verify_status_name(status),
                xr_program_diagnostic_kind_name(diagnostic.kind),
                xr_program_decode_status_name(decode), artifact->bytes[offset], value);
    }
    CHECK(status == expected_status);
    CHECK(diagnostic.kind == expected_diagnostic);
    CHECK(program == NULL);
    free(bytes);
}

static void test_parameter_modes_and_value_categories(void) {
    XrProgramArtifact ref = {0};
    CHECK(build_mode_artifact(XR_PARAM_REF, XR_CORE_IR_PLACE, true, &ref) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = validate_ok(&ref);
    xr_validated_program_free(program);

    size_t mode_offset = 0u;
    size_t category_offsets[2] = {0u, 0u};
    uint32_t entry_block = 0u;
    uint32_t block_count = 0u;
    CHECK(mode_fixture_offsets(&ref, &mode_offset, category_offsets, &entry_block, &block_count));
    CHECK(block_count == 2u);
    expect_mutated_verify(&ref, mode_offset, XR_PARAM_MOVE, XR_PROGRAM_VERIFY_SEMANTIC_REJECTED,
                          XR_PROGRAM_DIAGNOSTIC_TYPE);
    expect_mutated_verify(&ref, mode_offset, UINT8_C(3), XR_PROGRAM_VERIFY_STRUCTURAL_REJECTED,
                          XR_PROGRAM_DIAGNOSTIC_STRUCTURAL);
    uint32_t non_entry = entry_block == 0u ? 1u : 0u;
    expect_mutated_verify(&ref, category_offsets[non_entry], XR_CORE_IR_VALUE,
                          XR_PROGRAM_VERIFY_SEMANTIC_REJECTED,
                          XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE);

    XrProgramArtifact read = {0};
    CHECK(build_mode_artifact(XR_PARAM_READ, XR_CORE_IR_VALUE, false, &read) ==
          XR_PROGRAM_BUILD_OK);
    CHECK(mode_fixture_offsets(&read, &mode_offset, category_offsets, &entry_block, &block_count));
    expect_mutated_verify(&read, category_offsets[entry_block], XR_CORE_IR_PLACE,
                          XR_PROGRAM_VERIFY_SEMANTIC_REJECTED, XR_PROGRAM_DIAGNOSTIC_TYPE);
    CHECK(!xr_program_id_equal(ref.id, read.id));

    XrProgramArtifact invalid = {0};
    CHECK(build_mode_artifact((XrParamMode) 3, XR_CORE_IR_VALUE, false, &invalid) ==
          XR_PROGRAM_BUILD_INVALID_INPUT);
    CHECK(build_mode_artifact(XR_PARAM_REF, XR_CORE_IR_VALUE, false, &invalid) ==
          XR_PROGRAM_BUILD_INVALID_INPUT);

    xr_program_artifact_free(&read);
    xr_program_artifact_free(&ref);
}

static XrProgramArtifact build_aggregate_variant_artifact(bool wrong_variant,
                                                          bool reverse_type_inputs) {
    enum {
        AGGREGATE_TYPE = 101,
        VARIANT_TYPE = 77,
    };
    uint16_t aggregate_fields[] = {XR_CORE_TYPE_I64};
    uint16_t variant_payload[] = {AGGREGATE_TYPE};
    XrCoreIrVariantInput variants[] = {
        {0},
        {.payload_types = variant_payload,
         .payload_count = sizeof(variant_payload) / sizeof(variant_payload[0])},
    };
    XrCoreIrTypeInput types[] = {
        {.key = key("aggregate:type:variant"),
         .local_id = VARIANT_TYPE,
         .kind = XR_CORE_IR_TYPE_VARIANT,
         .variants = variants,
         .variant_count = sizeof(variants) / sizeof(variants[0])},
        {.key = key("aggregate:type:record"),
         .local_id = AGGREGATE_TYPE,
         .kind = XR_CORE_IR_TYPE_AGGREGATE,
         .field_types = aggregate_fields,
         .field_count = sizeof(aggregate_fields) / sizeof(aggregate_fields[0])},
    };
    XrCoreIrTypeInput ordered_types[] = {types[0], types[1]};
    if (reverse_type_inputs) {
        ordered_types[0] = types[1];
        ordered_types[1] = types[0];
    }
    XrCoreIrConstantInput constants[] = {
        {.key = key("aggregate:constant:40"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 40},
        {.key = key("aggregate:constant:2"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 2},
    };
    XrCoreIrKey v40 = key("aggregate:value:40");
    XrCoreIrKey v2 = key("aggregate:value:2");
    XrCoreIrKey aggregate = key("aggregate:value:record");
    XrCoreIrKey projected_40 = key("aggregate:value:projected-40");
    XrCoreIrKey updated = key("aggregate:value:updated");
    XrCoreIrKey variant = key("aggregate:value:variant");
    XrCoreIrKey tested = key("aggregate:value:tested");
    XrCoreIrKey projected_aggregate = key("aggregate:value:projected-record");
    XrCoreIrKey projected_2 = key("aggregate:value:projected-2");
    XrCoreIrKey construct_operands[] = {v40};
    XrCoreIrKey aggregate_operand[] = {aggregate};
    XrCoreIrKey update_operands[] = {aggregate, v2};
    XrCoreIrKey variant_operands[] = {updated};
    XrCoreIrKey variant_operand[] = {variant};
    XrCoreIrKey projected_operand[] = {projected_aggregate};
    XrCoreIrKey returned[] = {projected_2};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v40,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v2,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
         .result = aggregate,
         .result_type_id = AGGREGATE_TYPE,
         .operands = construct_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT,
         .result = projected_40,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = aggregate_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_UPDATE,
         .result = updated,
         .result_type_id = AGGREGATE_TYPE,
         .operands = update_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_VARIANT_CONSTRUCT,
         .result = variant,
         .result_type_id = VARIANT_TYPE,
         .operands = wrong_variant ? NULL : variant_operands,
         .operand_count = wrong_variant ? 0u : 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT,
         .immediate.variant_ordinal = wrong_variant ? 0u : 1u},
        {.operation_id = XR_CORE_OP_CORE_VARIANT_TEST,
         .result = tested,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = variant_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT,
         .immediate.variant_ordinal = 1u},
        {.operation_id = XR_CORE_OP_CORE_VARIANT_PROJECT,
         .result = projected_aggregate,
         .result_type_id = AGGREGATE_TYPE,
         .operands = variant_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT_FIELD,
         .immediate.variant_field = {.variant_ordinal = 1u, .field_ordinal = 0u}},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT,
         .result = projected_2,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = projected_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returned,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = key("aggregate:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = key("aggregate:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = key("aggregate:module"),
        .constants = constants,
        .constant_count = sizeof(constants) / sizeof(constants[0]),
        .functions = &function,
        .function_count = 1u,
    };
    XrProgramArtifact artifact = {0};
    CHECK(write_typed_modules(ordered_types, sizeof(ordered_types) / sizeof(ordered_types[0]),
                              &module, 1u, &artifact) == XR_PROGRAM_BUILD_OK);
    return artifact;
}

static void test_aggregate_variant_operations(void) {
    XrProgramArtifact artifact = build_aggregate_variant_artifact(false, false);
    XrProgramArtifact reordered = build_aggregate_variant_artifact(false, true);
    CHECK(artifact.size == reordered.size);
    CHECK(artifact.size != 0u && memcmp(artifact.bytes, reordered.bytes, artifact.size) == 0);
    CHECK(xr_program_id_equal(artifact.id, reordered.id));
    xr_program_artifact_free(&reordered);
    XrValidatedProgram *program = validate_ok(&artifact);
    if (program) {
        XrReferenceOutcome result = xr_reference_evaluate(
            program, xr_validated_program_entry_function(program), NULL, 0u, NULL, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_RETURN);
        CHECK(result.value.kind == XR_REFERENCE_VALUE_I64);
        CHECK(result.value.as.i64 == 2);
        XrReferenceBudget cell_budget = xr_reference_default_budget();
        cell_budget.max_value_cells = 1u;
        result = xr_reference_evaluate(program, xr_validated_program_entry_function(program), NULL,
                                       0u, NULL, &cell_budget);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_RESOURCE_LIMIT);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);

    artifact = build_aggregate_variant_artifact(true, false);
    program = validate_ok(&artifact);
    if (program) {
        XrReferenceOutcome result = xr_reference_evaluate(
            program, xr_validated_program_entry_function(program), NULL, 0u, NULL, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_TRAP);
        CHECK(result.trap == XR_REFERENCE_TRAP_VARIANT_TAG_MISMATCH);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);
}

static XrProgramBuildStatus build_with_type_graph(const XrCoreIrTypeInput *types,
                                                  uint32_t type_count) {
    XrCoreIrInstructionInput instruction = {
        .operation_id = XR_CORE_OP_CORE_TRAP,
        .result_type_id = XR_CORE_TYPE_VOID,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
        .immediate.u32 = XR_REFERENCE_TRAP_EXPLICIT,
    };
    XrCoreIrKey block_key = key("type-graph:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = &instruction,
        .instruction_count = 1u,
    };
    XrCoreIrFunctionInput function = {
        .key = key("type-graph:function"),
        .result_type_id = XR_CORE_TYPE_VOID,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = key("type-graph:module"),
        .functions = &function,
        .function_count = 1u,
    };
    XrCoreIrKey profile = key("type-graph:profile");
    uint16_t features[] = {XR_CORE_FEATURE_CORE_BASE};
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = profile.bytes,
        .required_features = features,
        .required_feature_count = 1u,
        .types = types,
        .type_count = type_count,
        .modules = &module,
        .module_count = 1u,
    };
    XrCoreIrProgram *program = NULL;
    char diagnostic[256] = {0};
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&input, &program, diagnostic, sizeof(diagnostic));
    xr_core_ir_program_free(program);
    return status;
}

static void test_dynamic_type_graph_rejection(void) {
    enum {
        SELF_TYPE = 41,
        MISSING_TYPE = 99
    };
    uint16_t self_field[] = {SELF_TYPE};
    XrCoreIrTypeInput recursive = {
        .key = key("type-graph:recursive"),
        .local_id = SELF_TYPE,
        .kind = XR_CORE_IR_TYPE_AGGREGATE,
        .field_types = self_field,
        .field_count = 1u,
    };
    CHECK(build_with_type_graph(&recursive, 1u) == XR_PROGRAM_BUILD_INVALID_INPUT);

    uint16_t missing_field[] = {MISSING_TYPE};
    XrCoreIrTypeInput unresolved = {
        .key = key("type-graph:unresolved"),
        .local_id = SELF_TYPE,
        .kind = XR_CORE_IR_TYPE_AGGREGATE,
        .field_types = missing_field,
        .field_count = 1u,
    };
    CHECK(build_with_type_graph(&unresolved, 1u) == XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE);
}

static void test_scalar_operations(void) {
    XrCoreIrConstantInput constants[] = {
        {.key = key("scalar:constant:6"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 6},
        {.key = key("scalar:constant:2"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 2},
        {.key = key("scalar:constant:true"),
         .type_id = XR_CORE_TYPE_BOOL,
         .kind = XR_CORE_IR_CONSTANT_BOOL,
         .value.boolean = true},
    };
    XrCoreIrKey v6 = key("scalar:value:6");
    XrCoreIrKey v2 = key("scalar:value:2");
    XrCoreIrKey v8 = key("scalar:value:8");
    XrCoreIrKey vsub = key("scalar:value:sub");
    XrCoreIrKey vmul = key("scalar:value:mul");
    XrCoreIrKey vdiv = key("scalar:value:div");
    XrCoreIrKey vcmp = key("scalar:value:compare");
    XrCoreIrKey vcopy = key("scalar:value:copy");
    XrCoreIrKey vbool = key("scalar:value:bool");
    XrCoreIrKey add_args[] = {v6, v2};
    XrCoreIrKey sub_args[] = {v8, v2};
    XrCoreIrKey mul_args[] = {vsub, v2};
    XrCoreIrKey div_args[] = {vmul, v2};
    XrCoreIrKey compare_args[] = {vdiv, v6};
    XrCoreIrKey copy_args[] = {vcmp};
    XrCoreIrKey return_args[] = {vcopy};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v6,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v2,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_BOOL,
         .result = vbool,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[2].key},
        {.operation_id = XR_CORE_OP_CORE_ADD_I64,
         .result = v8,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = add_args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_SUB_I64,
         .result = vsub,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = sub_args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_MUL_I64,
         .result = vmul,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = mul_args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_DIV_I64,
         .result = vdiv,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = div_args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_COMPARE_I64,
         .result = vcmp,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = compare_args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_OWNER_COPY,
         .result = vcopy,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = copy_args,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = return_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = key("scalar:block:entry");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = key("scalar:function"),
        .result_type_id = XR_CORE_TYPE_BOOL,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrProgramArtifact artifact = {0};
    CHECK(write_one_function(constants, sizeof(constants) / sizeof(constants[0]), &function,
                             &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = validate_ok(&artifact);
    if (program) {
        XrReferenceProfile profile = {.pointer_width = 64u};
        XrReferenceOutcome result = xr_reference_evaluate(
            program, xr_validated_program_entry_function(program), NULL, 0, &profile, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_RETURN);
        CHECK(result.value.kind == XR_REFERENCE_VALUE_BOOL);
        CHECK(result.value.as.boolean);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);
}

static void test_control_and_profile(void) {
    XrCoreIrConstantInput constants[] = {
        {.key = key("control:constant:1"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 1},
        {.key = key("control:constant:2"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 2},
        {.key = key("control:constant:true"),
         .type_id = XR_CORE_TYPE_BOOL,
         .kind = XR_CORE_IR_CONSTANT_BOOL,
         .value.boolean = true},
    };
    XrCoreIrKey entry_key = key("control:block:entry");
    XrCoreIrKey true_key = key("control:block:true");
    XrCoreIrKey false_key = key("control:block:false");
    XrCoreIrKey merge_key = key("control:block:merge");
    XrCoreIrKey v1 = key("control:value:1");
    XrCoreIrKey v2 = key("control:value:2");
    XrCoreIrKey vcmp = key("control:value:compare");
    XrCoreIrKey vcond = key("control:value:condition");
    XrCoreIrKey true_arg = key("control:value:true-arg");
    XrCoreIrKey false_arg = key("control:value:false-arg");
    XrCoreIrKey true_width = key("control:value:true-width");
    XrCoreIrKey false_width = key("control:value:false-width");
    XrCoreIrKey merge_arg = key("control:value:merge-arg");
    XrCoreIrKey compare_args[] = {v1, v2};
    XrCoreIrKey conditional_args[] = {vcond, v1, v2};
    XrCoreIrKey conditional_successors[] = {true_key, false_key};
    XrCoreIrKey true_block_args[] = {true_arg};
    XrCoreIrKey false_block_args[] = {false_arg};
    XrCoreIrKey true_branch_args[] = {true_width};
    XrCoreIrKey false_branch_args[] = {false_width};
    XrCoreIrKey merge_successor[] = {merge_key};
    XrCoreIrKey merge_block_args[] = {merge_arg};
    XrCoreIrKey return_args[] = {merge_arg};
    XrCoreIrInstructionInput entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v1,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v2,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_COMPARE_I64,
         .result = vcmp,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = compare_args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 2},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_BOOL,
         .result = vcond,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[2].key},
        {.operation_id = XR_CORE_OP_CORE_CONDITIONAL_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = conditional_args,
         .operand_count = 3,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = conditional_successors,
         .successor_count = 2},
    };
    XrCoreIrValueInput true_argument = {.key = true_arg, .type_id = XR_CORE_TYPE_I64};
    XrCoreIrInstructionInput true_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = true_block_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_TARGET_POINTER_WIDTH,
         .result = true_width,
         .result_type_id = XR_CORE_TYPE_U32,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = true_branch_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = merge_successor,
         .successor_count = 1},
    };
    XrCoreIrValueInput false_argument = {.key = false_arg, .type_id = XR_CORE_TYPE_I64};
    XrCoreIrInstructionInput false_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = false_block_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_TARGET_POINTER_WIDTH,
         .result = false_width,
         .result_type_id = XR_CORE_TYPE_U32,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = false_branch_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = merge_successor,
         .successor_count = 1},
    };
    XrCoreIrValueInput merge_argument = {.key = merge_arg, .type_id = XR_CORE_TYPE_U32};
    XrCoreIrInstructionInput merge_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = merge_block_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = return_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput blocks[] = {
        {.key = entry_key,
         .instructions = entry_instructions,
         .instruction_count = sizeof(entry_instructions) / sizeof(entry_instructions[0])},
        {.key = true_key,
         .arguments = &true_argument,
         .argument_count = 1,
         .instructions = true_instructions,
         .instruction_count = sizeof(true_instructions) / sizeof(true_instructions[0])},
        {.key = false_key,
         .arguments = &false_argument,
         .argument_count = 1,
         .instructions = false_instructions,
         .instruction_count = sizeof(false_instructions) / sizeof(false_instructions[0])},
        {.key = merge_key,
         .arguments = &merge_argument,
         .argument_count = 1,
         .instructions = merge_instructions,
         .instruction_count = sizeof(merge_instructions) / sizeof(merge_instructions[0])},
    };
    XrCoreIrFunctionInput function = {
        .key = key("control:function"),
        .result_type_id = XR_CORE_TYPE_U32,
        .effect_mask = 9u,
        .capability_mask = 1u,
        .entry_block = entry_key,
        .blocks = blocks,
        .block_count = sizeof(blocks) / sizeof(blocks[0]),
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrProgramArtifact artifact = {0};
    CHECK(write_one_function(constants, sizeof(constants) / sizeof(constants[0]), &function,
                             &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = validate_ok(&artifact);
    if (program) {
        XrReferenceProfile profile = {.pointer_width = 64u};
        XrReferenceOutcome result = xr_reference_evaluate(program, 0, NULL, 0, &profile, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_RETURN);
        CHECK(result.value.kind == XR_REFERENCE_VALUE_U32);
        CHECK(result.value.as.u32 == 64u);
        profile.pointer_width = 0;
        result = xr_reference_evaluate(program, 0, NULL, 0, &profile, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_TRAP);
        CHECK(result.trap == XR_REFERENCE_TRAP_PROFILE_UNAVAILABLE);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);
}

static void test_direct_call(void) {
    XrCoreIrConstantInput constants[] = {
        {.key = key("call:constant:40"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 40},
        {.key = key("call:constant:2"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 2},
    };
    XrCoreIrKey helper_key = key("call:function:helper");
    XrCoreIrKey main_key = key("call:function:main");
    XrCoreIrKey helper_block_key = key("call:block:helper");
    XrCoreIrKey main_block_key = key("call:block:main");
    XrCoreIrKey v40 = key("call:value:40");
    XrCoreIrKey v2 = key("call:value:2");
    XrCoreIrKey vsum = key("call:value:sum");
    XrCoreIrKey vcall = key("call:value:result");
    XrCoreIrKey add_args[] = {v40, v2};
    XrCoreIrKey helper_return[] = {vsum};
    XrCoreIrKey main_return[] = {vcall};
    XrCoreIrInstructionInput helper_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v40,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v2,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_ADD_I64,
         .result = vsum,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = add_args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = helper_return,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrInstructionInput main_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CALL_SEALED_DIRECT,
         .result = vcall,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = helper_key},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = main_return,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput helper_block = {
        .key = helper_block_key,
        .instructions = helper_instructions,
        .instruction_count = sizeof(helper_instructions) / sizeof(helper_instructions[0]),
    };
    XrCoreIrBlockInput main_block = {
        .key = main_block_key,
        .instructions = main_instructions,
        .instruction_count = sizeof(main_instructions) / sizeof(main_instructions[0]),
    };
    XrCoreIrFunctionInput functions[] = {
        {.key = helper_key,
         .result_type_id = XR_CORE_TYPE_I64,
         .effect_mask = 1u,
         .entry_block = helper_block_key,
         .blocks = &helper_block,
         .block_count = 1},
        {.key = main_key,
         .result_type_id = XR_CORE_TYPE_I64,
         .effect_mask = 5u,
         .entry_block = main_block_key,
         .blocks = &main_block,
         .block_count = 1,
         .flags = XR_PROGRAM_FUNCTION_ENTRY},
    };
    XrCoreIrModuleInput module = {
        .key = key("call:module"),
        .constants = constants,
        .constant_count = sizeof(constants) / sizeof(constants[0]),
        .functions = functions,
        .function_count = sizeof(functions) / sizeof(functions[0]),
    };
    XrProgramArtifact artifact = {0};
    CHECK(write_modules(&module, 1, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = validate_ok(&artifact);
    if (program) {
        XrReferenceProfile profile = {.pointer_width = 64u};
        XrReferenceOutcome result = xr_reference_evaluate(
            program, xr_validated_program_entry_function(program), NULL, 0, &profile, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_RETURN);
        CHECK(result.value.kind == XR_REFERENCE_VALUE_I64);
        CHECK(result.value.as.i64 == 42);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);

    functions[0].error_type_id = XR_CORE_TYPE_ERROR;
    functions[0].effect_mask |= XR_CORE_EFFECT_ERROR;
    CHECK(write_modules(&module, 1, &artifact) == XR_PROGRAM_BUILD_OK);
    expect_semantic_reject(&artifact, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE);
    xr_program_artifact_free(&artifact);
}

static void test_owner_and_local_place_operations(void) {
    XrCoreIrConstantInput constants[] = {
        {.key = key("place:constant:40"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 40},
        {.key = key("place:constant:42"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 42},
        {.key = key("place:constant:7"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 7},
    };
    XrCoreIrKey helper_key = key("place:function:helper");
    XrCoreIrKey main_key = key("place:function:main");
    XrCoreIrKey helper_block_key = key("place:block:helper");
    XrCoreIrKey main_block_key = key("place:block:main");
    XrCoreIrKey helper_argument = key("place:value:helper-argument");
    XrCoreIrKey v40 = key("place:value:40");
    XrCoreIrKey moved = key("place:value:moved");
    XrCoreIrKey local = key("place:value:local");
    XrCoreIrKey v42 = key("place:value:42");
    XrCoreIrKey loaded = key("place:value:loaded");
    XrCoreIrKey v7 = key("place:value:7");
    XrCoreIrValueInput helper_arguments[] = {
        {.key = helper_argument, .type_id = XR_CORE_TYPE_I64, .category = XR_CORE_IR_PLACE},
    };
    XrCoreIrKey helper_argument_operand[] = {helper_argument};
    XrCoreIrKey store_operands[] = {helper_argument, v42};
    XrCoreIrInstructionInput helper_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = helper_argument_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v42,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_PLACE_STORE,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = store_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey v40_operand[] = {v40};
    XrCoreIrKey moved_operand[] = {moved};
    XrCoreIrKey local_operand[] = {local};
    XrCoreIrKey v7_operand[] = {v7};
    XrCoreIrKey loaded_operand[] = {loaded};
    XrCoreIrInstructionInput main_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v40,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_OWNER_MOVE,
         .result = moved,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = v40_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOCAL,
         .result = local,
         .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_PLACE,
         .operands = moved_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CALL_SEALED_DIRECT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = local_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = helper_key},
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOAD,
         .result = loaded,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = local_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v7,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[2].key},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = v7_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = loaded_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput blocks[] = {
        {.key = helper_block_key,
         .arguments = helper_arguments,
         .argument_count = 1u,
         .instructions = helper_instructions,
         .instruction_count = sizeof(helper_instructions) / sizeof(helper_instructions[0])},
        {.key = main_block_key,
         .instructions = main_instructions,
         .instruction_count = sizeof(main_instructions) / sizeof(main_instructions[0])},
    };
    uint16_t parameter_type = XR_CORE_TYPE_I64;
    XrParamMode parameter_mode = XR_PARAM_REF;
    XrCoreIrFunctionInput functions[] = {
        {.key = helper_key,
         .parameter_types = &parameter_type,
         .parameter_modes = &parameter_mode,
         .parameter_count = 1u,
         .result_type_id = XR_CORE_TYPE_VOID,
         .entry_block = helper_block_key,
         .blocks = &blocks[0],
         .block_count = 1u},
        {.key = main_key,
         .result_type_id = XR_CORE_TYPE_I64,
         .effect_mask = 4u,
         .entry_block = main_block_key,
         .blocks = &blocks[1],
         .block_count = 1u,
         .flags = XR_PROGRAM_FUNCTION_ENTRY},
    };
    XrCoreIrModuleInput module = {
        .key = key("place:module"),
        .constants = constants,
        .constant_count = sizeof(constants) / sizeof(constants[0]),
        .functions = functions,
        .function_count = sizeof(functions) / sizeof(functions[0]),
    };
    XrProgramArtifact artifact = {0};
    CHECK(write_modules(&module, 1u, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = validate_ok(&artifact);
    if (program) {
        XrReferenceOutcome result = xr_reference_evaluate(
            program, xr_validated_program_entry_function(program), NULL, 0u, NULL, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_RETURN);
        CHECK(result.value.kind == XR_REFERENCE_VALUE_I64);
        CHECK(result.value.as.i64 == 42);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);

    XrCoreIrKey bad_result = key("place:value:bad-result");
    XrCoreIrKey bad_operands[] = {v40, moved};
    XrCoreIrKey bad_return[] = {bad_result};
    XrCoreIrInstructionInput bad_instructions[] = {
        main_instructions[0],
        main_instructions[1],
        {.operation_id = XR_CORE_OP_CORE_ADD_I64,
         .result = bad_result,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = bad_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = bad_return,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput bad_block = {
        .key = key("place:block:bad"),
        .instructions = bad_instructions,
        .instruction_count = sizeof(bad_instructions) / sizeof(bad_instructions[0]),
    };
    XrCoreIrFunctionInput bad_function = {
        .key = key("place:function:bad"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = 1u,
        .entry_block = bad_block.key,
        .blocks = &bad_block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    CHECK(write_one_function(constants, 1u, &bad_function, &artifact) == XR_PROGRAM_BUILD_OK);
    expect_semantic_reject(&artifact, XR_PROGRAM_DIAGNOSTIC_VALUE_USE);
    xr_program_artifact_free(&artifact);
}

typedef enum AffineCopyFixtureKind {
    AFFINE_COPY_VALID = 0,
    AFFINE_COPY_MISSING_DROP,
    AFFINE_COPY_USE_AFTER_DROP,
    AFFINE_COPY_FORBIDDEN,
} AffineCopyFixtureKind;

static XrProgramBuildStatus build_affine_copy_artifact(AffineCopyFixtureKind kind,
                                                       XrProgramArtifact *artifact) {
    enum {
        AFFINE_TYPE = 61
    };
    uint16_t fields[] = {XR_CORE_TYPE_I64};
    XrCoreIrTypeInput type = {
        .key = key("affine-copy:type"),
        .local_id = AFFINE_TYPE,
        .kind = XR_CORE_IR_TYPE_AGGREGATE,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract =
            kind == AFFINE_COPY_FORBIDDEN ? XR_CORE_IR_COPY_FORBIDDEN : XR_CORE_IR_COPY_EXPLICIT,
        .field_types = fields,
        .field_count = 1u,
    };
    XrCoreIrConstantInput constant = {
        .key = key("affine-copy:constant"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 42,
    };
    XrCoreIrKey scalar = key("affine-copy:scalar");
    XrCoreIrKey owner = key("affine-copy:owner");
    XrCoreIrKey projected = key("affine-copy:projected");
    XrCoreIrKey copied = key("affine-copy:copied");
    XrCoreIrKey construct_operands[] = {scalar};
    XrCoreIrKey owner_operand[] = {owner};
    XrCoreIrKey copied_operand[] = {copied};
    XrCoreIrKey return_operand[] = {projected};
    XrCoreIrInstructionInput instructions[7] = {0};
    uint32_t count = 0u;
    instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
        .result = scalar,
        .result_type_id = XR_CORE_TYPE_I64,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
        .immediate.key = constant.key,
    };
    instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
        .result = owner,
        .result_type_id = AFFINE_TYPE,
        .result_ownership = XR_CORE_IR_OWNER,
        .operands = construct_operands,
        .operand_count = 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    if (kind != AFFINE_COPY_USE_AFTER_DROP) {
        instructions[count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT,
            .result = projected,
            .result_type_id = XR_CORE_TYPE_I64,
            .operands = owner_operand,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
            .immediate.field_ordinal = 0u,
        };
    }
    instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_OWNER_COPY,
        .result = copied,
        .result_type_id = AFFINE_TYPE,
        .result_ownership = XR_CORE_IR_OWNER,
        .operands = owner_operand,
        .operand_count = 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = copied_operand,
        .operand_count = 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    if (kind != AFFINE_COPY_MISSING_DROP) {
        instructions[count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = owner_operand,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
    }
    if (kind == AFFINE_COPY_USE_AFTER_DROP) {
        instructions[count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT,
            .result = projected,
            .result_type_id = XR_CORE_TYPE_I64,
            .operands = owner_operand,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
            .immediate.field_ordinal = 0u,
        };
    }
    instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_RETURN,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = return_operand,
        .operand_count = 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    XrCoreIrKey block_key = key("affine-copy:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = count,
    };
    XrCoreIrFunctionInput function = {
        .key = key("affine-copy:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = key("affine-copy:module"),
        .constants = &constant,
        .constant_count = 1u,
        .functions = &function,
        .function_count = 1u,
    };
    return write_typed_modules(&type, 1u, &module, 1u, artifact);
}

static XrProgramBuildStatus build_affine_read_call_artifact(XrProgramArtifact *artifact) {
    enum {
        AFFINE_TYPE = 65
    };
    uint16_t fields[] = {XR_CORE_TYPE_I64};
    XrCoreIrTypeInput type = {
        .key = key("affine-read:type"),
        .local_id = AFFINE_TYPE,
        .kind = XR_CORE_IR_TYPE_AGGREGATE,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .field_types = fields,
        .field_count = 1u,
    };
    XrCoreIrConstantInput constant = {
        .key = key("affine-read:constant"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 42,
    };
    XrCoreIrKey helper_key = key("affine-read:function:helper");
    XrCoreIrKey main_key = key("affine-read:function:main");
    XrCoreIrKey helper_block_key = key("affine-read:block:helper");
    XrCoreIrKey main_block_key = key("affine-read:block:main");
    XrCoreIrKey borrowed = key("affine-read:value:borrowed");
    XrCoreIrKey projected = key("affine-read:value:projected");
    XrCoreIrValueInput helper_argument = {
        .key = borrowed,
        .type_id = AFFINE_TYPE,
        .ownership = XR_CORE_IR_NON_OWNER,
    };
    XrCoreIrKey borrowed_operand[] = {borrowed};
    XrCoreIrKey projected_operand[] = {projected};
    XrCoreIrInstructionInput helper_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = borrowed_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT,
         .result = projected,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = borrowed_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = projected_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput helper_block = {
        .key = helper_block_key,
        .arguments = &helper_argument,
        .argument_count = 1u,
        .instructions = helper_instructions,
        .instruction_count = sizeof(helper_instructions) / sizeof(helper_instructions[0]),
    };
    XrCoreIrKey scalar = key("affine-read:value:scalar");
    XrCoreIrKey owner = key("affine-read:value:owner");
    XrCoreIrKey result = key("affine-read:value:result");
    XrCoreIrKey construct_operands[] = {scalar};
    XrCoreIrKey owner_operand[] = {owner};
    XrCoreIrKey result_operand[] = {result};
    XrCoreIrInstructionInput main_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = scalar,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant.key},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
         .result = owner,
         .result_type_id = AFFINE_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = construct_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CALL_SEALED_DIRECT,
         .result = result,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = owner_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = helper_key},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = owner_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = result_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput main_block = {
        .key = main_block_key,
        .instructions = main_instructions,
        .instruction_count = sizeof(main_instructions) / sizeof(main_instructions[0]),
    };
    uint16_t parameter_type = AFFINE_TYPE;
    XrParamMode parameter_mode = XR_PARAM_READ;
    XrCoreIrFunctionInput functions[] = {
        {.key = helper_key,
         .parameter_types = &parameter_type,
         .parameter_modes = &parameter_mode,
         .parameter_count = 1u,
         .result_type_id = XR_CORE_TYPE_I64,
         .effect_mask = 1u,
         .entry_block = helper_block_key,
         .blocks = &helper_block,
         .block_count = 1u},
        {.key = main_key,
         .result_type_id = XR_CORE_TYPE_I64,
         .effect_mask = 5u,
         .entry_block = main_block_key,
         .blocks = &main_block,
         .block_count = 1u,
         .flags = XR_PROGRAM_FUNCTION_ENTRY},
    };
    XrCoreIrModuleInput module = {
        .key = key("affine-read:module"),
        .constants = &constant,
        .constant_count = 1u,
        .functions = functions,
        .function_count = sizeof(functions) / sizeof(functions[0]),
    };
    return write_typed_modules(&type, 1u, &module, 1u, artifact);
}

static XrProgramBuildStatus build_affine_branch_artifact(uint32_t owner_copies_per_edge,
                                                         XrProgramArtifact *artifact) {
    enum {
        AFFINE_TYPE = 62
    };
    uint16_t fields[] = {XR_CORE_TYPE_I64};
    XrCoreIrTypeInput type = {
        .key = key("affine-branch:type"),
        .local_id = AFFINE_TYPE,
        .kind = XR_CORE_IR_TYPE_AGGREGATE,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .field_types = fields,
        .field_count = 1u,
    };
    XrCoreIrConstantInput constants[] = {
        {.key = key("affine-branch:constant:42"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 42},
        {.key = key("affine-branch:constant:true"),
         .type_id = XR_CORE_TYPE_BOOL,
         .kind = XR_CORE_IR_CONSTANT_BOOL,
         .value.boolean = true},
    };
    XrCoreIrKey entry_key = key("affine-branch:block:entry");
    XrCoreIrKey true_key = key("affine-branch:block:true");
    XrCoreIrKey false_key = key("affine-branch:block:false");
    XrCoreIrKey scalar = key("affine-branch:value:scalar");
    XrCoreIrKey condition = key("affine-branch:value:condition");
    XrCoreIrKey owner = key("affine-branch:value:owner");
    XrCoreIrKey true_values[] = {key("affine-branch:value:true:0"),
                                 key("affine-branch:value:true:1")};
    XrCoreIrKey false_values[] = {key("affine-branch:value:false:0"),
                                  key("affine-branch:value:false:1")};
    XrCoreIrValueInput true_arguments[2] = {0};
    XrCoreIrValueInput false_arguments[2] = {0};
    XrCoreIrKey branch_operands[5] = {condition};
    for (uint32_t index = 0; index < owner_copies_per_edge; ++index) {
        true_arguments[index] = (XrCoreIrValueInput) {
            .key = true_values[index],
            .type_id = AFFINE_TYPE,
            .ownership = XR_CORE_IR_OWNER,
        };
        false_arguments[index] = (XrCoreIrValueInput) {
            .key = false_values[index],
            .type_id = AFFINE_TYPE,
            .ownership = XR_CORE_IR_OWNER,
        };
        branch_operands[1u + index] = owner;
        branch_operands[1u + owner_copies_per_edge + index] = owner;
    }
    XrCoreIrKey construct_operands[] = {scalar};
    XrCoreIrKey successors[] = {true_key, false_key};
    XrCoreIrInstructionInput entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = scalar,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_BOOL,
         .result = condition,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
         .result = owner,
         .result_type_id = AFFINE_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = construct_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONDITIONAL_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = branch_operands,
         .operand_count = 1u + 2u * owner_copies_per_edge,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = successors,
         .successor_count = 2u},
    };
    XrCoreIrInstructionInput true_instructions[6] = {0};
    XrCoreIrInstructionInput false_instructions[6] = {0};
    uint32_t true_count = 0u;
    uint32_t false_count = 0u;
    if (owner_copies_per_edge != 0u) {
        true_instructions[true_count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = true_values,
            .operand_count = owner_copies_per_edge,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
        false_instructions[false_count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = false_values,
            .operand_count = owner_copies_per_edge,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
    }
    for (uint32_t index = 0; index < owner_copies_per_edge; ++index) {
        true_instructions[true_count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = &true_values[index],
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
        false_instructions[false_count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = &false_values[index],
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
    }
    XrCoreIrKey true_result = key("affine-branch:value:true-result");
    XrCoreIrKey false_result = key("affine-branch:value:false-result");
    true_instructions[true_count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
        .result = true_result,
        .result_type_id = XR_CORE_TYPE_I64,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
        .immediate.key = constants[0].key,
    };
    true_instructions[true_count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_RETURN,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = &true_result,
        .operand_count = 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    false_instructions[false_count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
        .result = false_result,
        .result_type_id = XR_CORE_TYPE_I64,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
        .immediate.key = constants[0].key,
    };
    false_instructions[false_count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_RETURN,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = &false_result,
        .operand_count = 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    XrCoreIrBlockInput blocks[] = {
        {.key = entry_key,
         .instructions = entry_instructions,
         .instruction_count = sizeof(entry_instructions) / sizeof(entry_instructions[0])},
        {.key = true_key,
         .arguments = owner_copies_per_edge ? true_arguments : NULL,
         .argument_count = owner_copies_per_edge,
         .instructions = true_instructions,
         .instruction_count = true_count},
        {.key = false_key,
         .arguments = owner_copies_per_edge ? false_arguments : NULL,
         .argument_count = owner_copies_per_edge,
         .instructions = false_instructions,
         .instruction_count = false_count},
    };
    XrCoreIrFunctionInput function = {
        .key = key("affine-branch:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = 1u,
        .entry_block = entry_key,
        .blocks = blocks,
        .block_count = sizeof(blocks) / sizeof(blocks[0]),
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = key("affine-branch:module"),
        .constants = constants,
        .constant_count = sizeof(constants) / sizeof(constants[0]),
        .functions = &function,
        .function_count = 1u,
    };
    return write_typed_modules(&type, 1u, &module, 1u, artifact);
}

static void test_affine_owner_domain(void) {
    XrProgramArtifact artifact = {0};
    CHECK(build_affine_copy_artifact(AFFINE_COPY_VALID, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = validate_ok(&artifact);
    if (program) {
        XrReferenceOutcome result = xr_reference_evaluate(
            program, xr_validated_program_entry_function(program), NULL, 0u, NULL, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_RETURN);
        CHECK(result.value.kind == XR_REFERENCE_VALUE_I64);
        CHECK(result.value.as.i64 == 42);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);

    CHECK(build_affine_read_call_artifact(&artifact) == XR_PROGRAM_BUILD_OK);
    program = validate_ok(&artifact);
    if (program) {
        XrReferenceOutcome result = xr_reference_evaluate(
            program, xr_validated_program_entry_function(program), NULL, 0u, NULL, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_RETURN);
        CHECK(result.value.kind == XR_REFERENCE_VALUE_I64);
        CHECK(result.value.as.i64 == 42);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);

    CHECK(build_affine_copy_artifact(AFFINE_COPY_MISSING_DROP, &artifact) == XR_PROGRAM_BUILD_OK);
    expect_semantic_reject(&artifact, XR_PROGRAM_DIAGNOSTIC_VALUE_USE);
    xr_program_artifact_free(&artifact);

    CHECK(build_affine_copy_artifact(AFFINE_COPY_USE_AFTER_DROP, &artifact) == XR_PROGRAM_BUILD_OK);
    expect_semantic_reject(&artifact, XR_PROGRAM_DIAGNOSTIC_VALUE_USE);
    xr_program_artifact_free(&artifact);

    CHECK(build_affine_copy_artifact(AFFINE_COPY_FORBIDDEN, &artifact) == XR_PROGRAM_BUILD_OK);
    expect_semantic_reject(&artifact, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE);
    xr_program_artifact_free(&artifact);

    CHECK(build_affine_branch_artifact(1u, &artifact) == XR_PROGRAM_BUILD_OK);
    program = validate_ok(&artifact);
    xr_validated_program_free(program);
    xr_program_artifact_free(&artifact);

    CHECK(build_affine_branch_artifact(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    expect_semantic_reject(&artifact, XR_PROGRAM_DIAGNOSTIC_VALUE_USE);
    xr_program_artifact_free(&artifact);

    CHECK(build_affine_branch_artifact(2u, &artifact) == XR_PROGRAM_BUILD_OK);
    expect_semantic_reject(&artifact, XR_PROGRAM_DIAGNOSTIC_VALUE_USE);
    xr_program_artifact_free(&artifact);
}

static void test_terminal_operations(void) {
    XrCoreIrKey trap_block_key = key("trap:block");
    XrCoreIrInstructionInput trap_instruction = {
        .operation_id = XR_CORE_OP_CORE_TRAP,
        .result_type_id = XR_CORE_TYPE_VOID,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
        .immediate.u32 = XR_REFERENCE_TRAP_EXPLICIT,
    };
    XrCoreIrBlockInput trap_block = {
        .key = trap_block_key, .instructions = &trap_instruction, .instruction_count = 1};
    XrCoreIrFunctionInput trap_function = {
        .key = key("trap:function"),
        .result_type_id = XR_CORE_TYPE_VOID,
        .effect_mask = 1u,
        .entry_block = trap_block_key,
        .blocks = &trap_block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrProgramArtifact artifact = {0};
    CHECK(write_one_function(NULL, 0, &trap_function, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = validate_ok(&artifact);
    if (program) {
        XrReferenceOutcome result = xr_reference_evaluate(program, 0, NULL, 0, NULL, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_TRAP);
        CHECK(result.trap == XR_REFERENCE_TRAP_EXPLICIT);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);

    XrCoreIrKey error_block_key = key("error:block");
    XrCoreIrKey error_value = key("error:value");
    XrCoreIrValueInput error_argument = {.key = error_value, .type_id = XR_CORE_TYPE_ERROR};
    XrCoreIrKey block_args[] = {error_value};
    XrCoreIrInstructionInput error_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = block_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_ERROR_PUBLISH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = block_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput error_block = {
        .key = error_block_key,
        .arguments = &error_argument,
        .argument_count = 1,
        .instructions = error_instructions,
        .instruction_count = sizeof(error_instructions) / sizeof(error_instructions[0]),
    };
    uint16_t parameter_types[] = {XR_CORE_TYPE_ERROR};
    XrCoreIrFunctionInput error_function = {
        .key = key("error:function"),
        .parameter_types = parameter_types,
        .parameter_count = 1,
        .result_type_id = XR_CORE_TYPE_VOID,
        .error_type_id = XR_CORE_TYPE_ERROR,
        .effect_mask = XR_CORE_EFFECT_ERROR,
        .entry_block = error_block_key,
        .blocks = &error_block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    CHECK(write_one_function(NULL, 0, &error_function, &artifact) == XR_PROGRAM_BUILD_OK);
    program = validate_ok(&artifact);
    if (program) {
        XrReferenceValue argument = {.kind = XR_REFERENCE_VALUE_ERROR, .as.error = 73u};
        XrReferenceOutcome result = xr_reference_evaluate(program, 0, &argument, 1, NULL, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_ERROR);
        CHECK(result.error_value.kind == XR_REFERENCE_VALUE_ERROR);
        CHECK(result.error_value.as.error == 73u);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);
}

static void test_sealed_invoke_and_cleanup_cfg(void) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    XrProgramBuildStatus build_status =
        xr_program_invoke_fixture_write(&artifact, diagnostic, sizeof(diagnostic));
    if (build_status != XR_PROGRAM_BUILD_OK)
        fprintf(stderr, "invoke fixture build failed: %s\n", diagnostic);
    CHECK(build_status == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = validate_ok(&artifact);
    if (program) {
        uint32_t entry = xr_validated_program_entry_function(program);
        XrReferenceValue arguments[] = {
            {.kind = XR_REFERENCE_VALUE_BOOL, .as.boolean = true},
            {.kind = XR_REFERENCE_VALUE_ERROR, .as.error = 73u},
            {.kind = XR_REFERENCE_VALUE_I64, .as.i64 = 9},
        };
        XrReferenceOutcome normal =
            xr_reference_evaluate(program, entry, arguments, 3u, NULL, NULL);
        CHECK(normal.kind == XR_REFERENCE_OUTCOME_RETURN);
        CHECK(normal.value.kind == XR_REFERENCE_VALUE_I64);
        CHECK(normal.value.as.i64 == 42);
        arguments[0].as.boolean = false;
        XrReferenceOutcome error = xr_reference_evaluate(program, entry, arguments, 3u, NULL, NULL);
        CHECK(error.kind == XR_REFERENCE_OUTCOME_ERROR);
        CHECK(error.error_value.kind == XR_REFERENCE_VALUE_ERROR);
        CHECK(error.error_value.as.error == 73u);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);

    const struct {
        XrProgramInvokeFixtureMutation mutation;
        XrProgramDiagnosticKind diagnostic;
    } invalid[] = {
        {XR_INVOKE_FIXTURE_INVOKE_INFALLIBLE, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE},
        {XR_INVOKE_FIXTURE_WRONG_NORMAL_RESULT_TYPE, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE},
        {XR_INVOKE_FIXTURE_WRONG_ERROR_TYPE, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE},
        {XR_INVOKE_FIXTURE_MISSING_NORMAL_OWNER, XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY},
        {XR_INVOKE_FIXTURE_DUPLICATE_NORMAL_OWNER, XR_PROGRAM_DIAGNOSTIC_VALUE_USE},
        {XR_INVOKE_FIXTURE_MISSING_ERROR_OWNER, XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY},
        {XR_INVOKE_FIXTURE_DUPLICATE_ERROR_OWNER, XR_PROGRAM_DIAGNOSTIC_VALUE_USE},
    };
    for (size_t index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        memset(diagnostic, 0, sizeof(diagnostic));
        CHECK(xr_program_invoke_fixture_write_mutated(invalid[index].mutation, &artifact,
                                                      diagnostic,
                                                      sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
        expect_semantic_reject(&artifact, invalid[index].diagnostic);
        xr_program_artifact_free(&artifact);
    }
}

static void test_typed_panic_invoke_and_cleanup_cfg(void) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    CHECK(xr_program_panic_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
          XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = validate_ok(&artifact);
    if (program) {
        uint32_t entry = xr_validated_program_entry_function(program);
        XrReferenceValue arguments[] = {
            {.kind = XR_REFERENCE_VALUE_BOOL, .as.boolean = true},
            {.kind = XR_REFERENCE_VALUE_PANIC_INFO, .as.panic_info = 91u},
            {.kind = XR_REFERENCE_VALUE_I64, .as.i64 = 9},
        };
        XrReferenceOutcome normal =
            xr_reference_evaluate(program, entry, arguments, 3u, NULL, NULL);
        CHECK(normal.kind == XR_REFERENCE_OUTCOME_RETURN);
        CHECK(normal.value.kind == XR_REFERENCE_VALUE_I64);
        CHECK(normal.value.as.i64 == 42);
        arguments[0].as.boolean = false;
        XrReferenceOutcome panic = xr_reference_evaluate(program, entry, arguments, 3u, NULL, NULL);
        CHECK(panic.kind == XR_REFERENCE_OUTCOME_PANIC);
        CHECK(panic.panic_value.kind == XR_REFERENCE_VALUE_PANIC_INFO);
        CHECK(panic.panic_value.as.panic_info == 91u);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);

    const struct {
        XrProgramPanicFixtureMutation mutation;
        XrProgramDiagnosticKind diagnostic;
    } invalid[] = {
        {XR_PANIC_FIXTURE_DIRECT_PANICFUL, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE},
        {XR_PANIC_FIXTURE_COPY_PANIC, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE},
        {XR_PANIC_FIXTURE_PANIC_AS_ERROR, XR_PROGRAM_DIAGNOSTIC_FUNCTION},
        {XR_PANIC_FIXTURE_WRONG_CHANNEL_TYPE, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE},
        {XR_PANIC_FIXTURE_MISSING_NORMAL_OWNER, XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY},
        {XR_PANIC_FIXTURE_DUPLICATE_NORMAL_OWNER, XR_PROGRAM_DIAGNOSTIC_VALUE_USE},
        {XR_PANIC_FIXTURE_MISSING_PANIC_OWNER, XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY},
        {XR_PANIC_FIXTURE_DUPLICATE_PANIC_OWNER, XR_PROGRAM_DIAGNOSTIC_VALUE_USE},
    };
    for (size_t index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        memset(diagnostic, 0, sizeof(diagnostic));
        CHECK(xr_program_panic_fixture_write_mutated(invalid[index].mutation, &artifact, diagnostic,
                                                     sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
        expect_semantic_reject(&artifact, invalid[index].diagnostic);
        xr_program_artifact_free(&artifact);
    }
}

typedef enum InvalidFixture {
    INVALID_EFFECT = 0,
    INVALID_RESULT_TYPE,
    INVALID_MISSING_TERMINATOR,
} InvalidFixture;

static XrProgramArtifact build_invalid_fixture(InvalidFixture kind) {
    XrCoreIrConstantInput constants[] = {
        {.key = key("invalid:constant:1"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 1},
        {.key = key("invalid:constant:2"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 2},
    };
    XrCoreIrKey v1 = key("invalid:value:1");
    XrCoreIrKey v2 = key("invalid:value:2");
    XrCoreIrKey v3 = key("invalid:value:3");
    XrCoreIrKey add_args[] = {v1, v2};
    XrCoreIrKey return_args[] = {v3};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v1,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v2,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_ADD_I64,
         .result = v3,
         .result_type_id = kind == INVALID_RESULT_TYPE ? XR_CORE_TYPE_BOOL : XR_CORE_TYPE_I64,
         .operands = add_args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = return_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = key("invalid:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = kind == INVALID_MISSING_TERMINATOR ? 3u : 4u,
    };
    XrCoreIrFunctionInput function = {
        .key = key("invalid:function"),
        .result_type_id = kind == INVALID_RESULT_TYPE ? XR_CORE_TYPE_BOOL : XR_CORE_TYPE_I64,
        .effect_mask = kind == INVALID_EFFECT ? 0u : 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrProgramArtifact artifact = {0};
    CHECK(write_one_function(constants, sizeof(constants) / sizeof(constants[0]), &function,
                             &artifact) == XR_PROGRAM_BUILD_OK);
    return artifact;
}

static void test_negative_diagnostics_and_budget(void) {
    XrProgramArtifact artifact = build_invalid_fixture(INVALID_EFFECT);
    expect_semantic_reject(&artifact, XR_PROGRAM_DIAGNOSTIC_EFFECT);
    xr_program_artifact_free(&artifact);

    artifact = build_invalid_fixture(INVALID_RESULT_TYPE);
    expect_semantic_reject(&artifact, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE);
    xr_program_artifact_free(&artifact);

    artifact = build_invalid_fixture(INVALID_MISSING_TERMINATOR);
    expect_semantic_reject(&artifact, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW);
    xr_program_artifact_free(&artifact);

    test_scalar_operations();
    /* Build a known-valid fixture again for budget and identity rejection. */
    XrCoreIrKey block_key = key("budget:block");
    XrCoreIrInstructionInput instruction = {
        .operation_id = XR_CORE_OP_CORE_TRAP,
        .result_type_id = XR_CORE_TYPE_VOID,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
        .immediate.u32 = XR_REFERENCE_TRAP_EXPLICIT,
    };
    XrCoreIrBlockInput block = {
        .key = block_key, .instructions = &instruction, .instruction_count = 1};
    XrCoreIrFunctionInput function = {
        .key = key("budget:function"),
        .result_type_id = XR_CORE_TYPE_VOID,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    CHECK(write_one_function(NULL, 0, &function, &artifact) == XR_PROGRAM_BUILD_OK);
    XrProgramVerifyBudget budget = xr_program_verify_default_budget();
    budget.max_work = 1u;
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic diagnostic;
    CHECK(xr_program_validate(artifact.bytes, artifact.size, &budget, &program, &diagnostic) ==
          XR_PROGRAM_VERIFY_RESOURCE_LIMIT);
    CHECK(program == NULL);
    CHECK(diagnostic.kind == XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT);

    uint8_t *mutated = malloc(artifact.size);
    CHECK(mutated != NULL);
    if (mutated) {
        memcpy(mutated, artifact.bytes, artifact.size);
        size_t fingerprint_offset = XR_PROGRAM_MAGIC_SIZE + 4u + 1u;
        mutated[fingerprint_offset] ^= UINT8_C(1);
        CHECK(xr_program_validate(mutated, artifact.size, NULL, &program, &diagnostic) ==
              XR_PROGRAM_VERIFY_SEMANTIC_REJECTED);
        CHECK(diagnostic.kind == XR_PROGRAM_DIAGNOSTIC_CORE_SPEC_IDENTITY);
        CHECK(program == NULL);
        free(mutated);
    }
    xr_program_artifact_free(&artifact);
}

static void test_evaluator_traps_and_budget(void) {
    XrCoreIrConstantInput constants[] = {
        {.key = key("overflow:max"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = INT64_MAX},
        {.key = key("overflow:one"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 1},
    };
    XrCoreIrKey vmax = key("overflow:value:max");
    XrCoreIrKey vone = key("overflow:value:one");
    XrCoreIrKey vsum = key("overflow:value:sum");
    XrCoreIrKey args[] = {vmax, vone};
    XrCoreIrKey returns[] = {vsum};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = vmax,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = vone,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_ADD_I64,
         .result = vsum,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returns,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = key("overflow:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = key("overflow:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrProgramArtifact artifact = {0};
    CHECK(write_one_function(constants, sizeof(constants) / sizeof(constants[0]), &function,
                             &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = validate_ok(&artifact);
    if (program) {
        XrReferenceOutcome result = xr_reference_evaluate(program, 0, NULL, 0, NULL, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_TRAP);
        CHECK(result.trap == XR_REFERENCE_TRAP_INTEGER_OVERFLOW);
        XrReferenceBudget budget = {
            .max_steps = 2u,
            .max_value_cells = 8u,
            .max_call_depth = 8u,
        };
        result = xr_reference_evaluate(program, 0, NULL, 0, NULL, &budget);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_RESOURCE_LIMIT);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);
}

static int64_t i64_from_bits(uint64_t bits) {
    if (bits <= (uint64_t) INT64_MAX)
        return (int64_t) bits;
    return -(int64_t) (~bits) - 1;
}

static XrProgramArtifact build_typed_chain(uint32_t operation_count, uint64_t seed,
                                           const char *identity_prefix, int64_t *expected_out) {
    XrProgramArtifact artifact = {0};
    XrCoreIrConstantInput constants[2];
    XrCoreIrInstructionInput *instructions =
        calloc((size_t) operation_count + 3u, sizeof(XrCoreIrInstructionInput));
    XrCoreIrKey(*operands)[2] = calloc(operation_count ? operation_count : 1u, sizeof(*operands));
    XrCoreIrKey *results = calloc((size_t) operation_count + 2u, sizeof(XrCoreIrKey));
    CHECK(instructions != NULL && operands != NULL && results != NULL);
    if (!instructions || !operands || !results) {
        free(results);
        free(operands);
        free(instructions);
        return artifact;
    }
    int64_t left = i64_from_bits(seed * UINT64_C(0x9e3779b97f4a7c15));
    int64_t right = i64_from_bits((seed | 1u) * UINT64_C(0xd6e8feb86659fd93));
    char name[96];
    snprintf(name, sizeof(name), "%s:constant:left", identity_prefix);
    constants[0] = (XrCoreIrConstantInput) {
        .key = key(name),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = left,
    };
    snprintf(name, sizeof(name), "%s:constant:right", identity_prefix);
    constants[1] = (XrCoreIrConstantInput) {
        .key = key(name),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = right,
    };
    snprintf(name, sizeof(name), "%s:value:left", identity_prefix);
    results[0] = key(name);
    snprintf(name, sizeof(name), "%s:value:right", identity_prefix);
    results[1] = key(name);
    instructions[0] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
        .result = results[0],
        .result_type_id = XR_CORE_TYPE_I64,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
        .immediate.key = constants[0].key,
    };
    instructions[1] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
        .result = results[1],
        .result_type_id = XR_CORE_TYPE_I64,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
        .immediate.key = constants[1].key,
    };
    uint64_t expected_bits = (uint64_t) left;
    for (uint32_t index = 0; index < operation_count; ++index) {
        snprintf(name, sizeof(name), "%s:value:%u", identity_prefix, index);
        results[index + 2u] = key(name);
        operands[index][0] = index == 0 ? results[0] : results[index + 1u];
        operands[index][1] = results[1];
        uint16_t operation = index % 3u == 0u   ? XR_CORE_OP_CORE_ADD_I64
                             : index % 3u == 1u ? XR_CORE_OP_CORE_SUB_I64
                                                : XR_CORE_OP_CORE_MUL_I64;
        instructions[index + 2u] = (XrCoreIrInstructionInput) {
            .operation_id = operation,
            .result = results[index + 2u],
            .result_type_id = XR_CORE_TYPE_I64,
            .operands = operands[index],
            .operand_count = 2,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
            .immediate.u32 = 1u,
        };
        if (operation == XR_CORE_OP_CORE_ADD_I64)
            expected_bits += (uint64_t) right;
        else if (operation == XR_CORE_OP_CORE_SUB_I64)
            expected_bits -= (uint64_t) right;
        else
            expected_bits *= (uint64_t) right;
    }
    XrCoreIrKey return_operand[] = {operation_count == 0 ? results[0]
                                                         : results[operation_count + 1u]};
    instructions[operation_count + 2u] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_RETURN,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = return_operand,
        .operand_count = 1,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    snprintf(name, sizeof(name), "%s:block", identity_prefix);
    XrCoreIrKey block_key = key(name);
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = operation_count + 3u,
    };
    snprintf(name, sizeof(name), "%s:function", identity_prefix);
    XrCoreIrFunctionInput function = {
        .key = key(name),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    CHECK(write_one_function(constants, 2, &function, &artifact) == XR_PROGRAM_BUILD_OK);
    *expected_out = i64_from_bits(expected_bits);
    free(results);
    free(operands);
    free(instructions);
    return artifact;
}

static void test_typed_random_and_linear_work(void) {
    for (uint32_t sample = 1; sample <= 32u; ++sample) {
        char prefix[48];
        snprintf(prefix, sizeof(prefix), "random:%u", sample);
        int64_t expected = 0;
        XrProgramArtifact artifact =
            build_typed_chain(1u + sample % 31u, UINT64_C(0x29700000) + sample, prefix, &expected);
        XrValidatedProgram *program = validate_ok(&artifact);
        if (program) {
            XrReferenceOutcome result = xr_reference_evaluate(program, 0, NULL, 0, NULL, NULL);
            CHECK(result.kind == XR_REFERENCE_OUTCOME_RETURN);
            CHECK(result.value.kind == XR_REFERENCE_VALUE_I64);
            CHECK(result.value.as.i64 == expected);
            xr_validated_program_free(program);
        }
        xr_program_artifact_free(&artifact);
    }

    const uint32_t counts[] = {16u, 64u, 256u};
    uint64_t previous_work = 0;
    for (size_t index = 0; index < sizeof(counts) / sizeof(counts[0]); ++index) {
        int64_t expected = 0;
        XrProgramArtifact artifact =
            build_typed_chain(counts[index], 17u, "resource-ladder", &expected);
        XrValidatedProgram *program = validate_ok(&artifact);
        if (program) {
            uint64_t work = xr_validated_program_verifier_work(program);
            CHECK(work > previous_work);
            CHECK(work <= UINT64_C(20) * counts[index] + UINT64_C(128));
            previous_work = work;
            xr_validated_program_free(program);
        }
        xr_program_artifact_free(&artifact);
    }

    int64_t expected = 0;
    XrProgramArtifact first = build_typed_chain(8u, 99u, "alpha-a", &expected);
    XrProgramArtifact renamed = build_typed_chain(8u, 99u, "alpha-b", &expected);
    CHECK(first.size == renamed.size);
    CHECK(first.size != 0 && memcmp(first.bytes, renamed.bytes, first.size) == 0);
    CHECK(xr_program_id_equal(first.id, renamed.id));
    xr_program_artifact_free(&renamed);
    xr_program_artifact_free(&first);
}

static XrProgramArtifact build_view_root_artifact(bool reverse_rows, bool incomplete_call_roots) {
    enum {
        VIEW_TYPE = 91
    };
    XrCoreIrTypeInput view_type = {
        .key = key("view-root:type:const-slice-i64"),
        .local_id = VIEW_TYPE,
        .kind = XR_CORE_IR_TYPE_VIEW,
        .view_element_type = XR_CORE_TYPE_I64,
        .view_capability = XR_CORE_IR_VIEW_READ,
    };
    XrParamMode modes[] = {XR_PARAM_READ, XR_PARAM_READ};
    uint16_t parameter_types[] = {VIEW_TYPE, VIEW_TYPE};
    XrViewOrigin result_origins[] = {
        {.kind = XR_VIEW_ORIGIN_PARAM, .param_ordinal = 0},
        {.kind = XR_VIEW_ORIGIN_PARAM, .param_ordinal = 1},
    };

    XrCoreIrKey callee_key = key("view-root:function:callee");
    XrCoreIrKey callee_block_key = key("view-root:callee:block");
    XrCoreIrKey callee_a = key("view-root:callee:a");
    XrCoreIrKey callee_b = key("view-root:callee:b");
    XrCoreIrKey callee_r0 = key("view-root:callee:root-a");
    XrCoreIrKey callee_r1 = key("view-root:callee:root-b");
    XrCoreIrValueInput callee_arguments[] = {
        {.key = callee_a, .type_id = VIEW_TYPE},
        {.key = callee_b, .type_id = VIEW_TYPE},
    };
    XrCoreIrKey callee_block_operands[] = {callee_a, callee_b};
    XrCoreIrKey callee_return_operand[] = {callee_a};
    XrCoreIrInstructionInput callee_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = callee_block_operands,
         .operand_count = 2u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = callee_return_operand,
         .operand_count = 1u},
    };
    XrCoreIrBlockInput callee_block = {
        .key = callee_block_key,
        .arguments = callee_arguments,
        .argument_count = 2u,
        .instructions = callee_instructions,
        .instruction_count = 2u,
    };
    XrCoreIrRootInput callee_roots[] = {
        {.key = callee_r0, .kind = XR_CORE_IR_ROOT_PARAMETER, .parameter_ordinal = 0},
        {.key = callee_r1, .kind = XR_CORE_IR_ROOT_PARAMETER, .parameter_ordinal = 1},
    };
    XrCoreIrKey callee_a_roots[] = {callee_r0};
    XrCoreIrKey callee_b_roots[] = {callee_r1};
    XrCoreIrValueRootSetInput callee_sets[] = {
        {.value = callee_a, .roots = callee_a_roots, .root_count = 1u},
        {.value = callee_b, .roots = callee_b_roots, .root_count = 1u},
    };

    XrCoreIrKey caller_block_key = key("view-root:caller:block");
    XrCoreIrKey caller_x = key("view-root:caller:x");
    XrCoreIrKey caller_y = key("view-root:caller:y");
    XrCoreIrKey caller_result = key("view-root:caller:result");
    XrCoreIrKey caller_r0 = key("view-root:caller:root-x");
    XrCoreIrKey caller_r1 = key("view-root:caller:root-y");
    XrCoreIrValueInput caller_arguments[] = {
        {.key = caller_x, .type_id = VIEW_TYPE},
        {.key = caller_y, .type_id = VIEW_TYPE},
    };
    XrCoreIrKey caller_block_operands[] = {caller_x, caller_y};
    XrCoreIrKey caller_call_operands[] = {caller_x, caller_y};
    XrCoreIrKey caller_return_operand[] = {caller_result};
    XrCoreIrInstructionInput caller_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = caller_block_operands,
         .operand_count = 2u},
        {.operation_id = XR_CORE_OP_CORE_CALL_SEALED_DIRECT,
         .result = caller_result,
         .result_type_id = VIEW_TYPE,
         .operands = caller_call_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = callee_key},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = caller_return_operand,
         .operand_count = 1u},
    };
    XrCoreIrBlockInput caller_block = {
        .key = caller_block_key,
        .arguments = caller_arguments,
        .argument_count = 2u,
        .instructions = caller_instructions,
        .instruction_count = 3u,
    };
    XrCoreIrRootInput caller_roots[] = {
        {.key = caller_r0, .kind = XR_CORE_IR_ROOT_PARAMETER, .parameter_ordinal = 0},
        {.key = caller_r1, .kind = XR_CORE_IR_ROOT_PARAMETER, .parameter_ordinal = 1},
    };
    XrCoreIrKey caller_x_roots[] = {caller_r0};
    XrCoreIrKey caller_y_roots[] = {caller_r1};
    XrCoreIrKey caller_result_roots[] = {caller_r1, caller_r0};
    XrCoreIrValueRootSetInput caller_sets[] = {
        {.value = caller_x, .roots = caller_x_roots, .root_count = 1u},
        {.value = caller_y, .roots = caller_y_roots, .root_count = 1u},
        {.value = caller_result,
         .roots = caller_result_roots,
         .root_count = incomplete_call_roots ? 1u : 2u},
    };
    if (reverse_rows) {
        XrCoreIrRootInput root = callee_roots[0];
        callee_roots[0] = callee_roots[1];
        callee_roots[1] = root;
        root = caller_roots[0];
        caller_roots[0] = caller_roots[1];
        caller_roots[1] = root;
        XrCoreIrValueRootSetInput set = callee_sets[0];
        callee_sets[0] = callee_sets[1];
        callee_sets[1] = set;
        set = caller_sets[0];
        caller_sets[0] = caller_sets[2];
        caller_sets[2] = set;
    }
    XrCoreIrFunctionInput functions[] = {
        {.key = callee_key,
         .parameter_types = parameter_types,
         .parameter_modes = modes,
         .parameter_count = 2u,
         .result_type_id = VIEW_TYPE,
         .result_borrow_origins = result_origins,
         .result_borrow_origin_count = 2u,
         .effect_mask = 1u,
         .entry_block = callee_block_key,
         .blocks = &callee_block,
         .block_count = 1u,
         .roots = callee_roots,
         .root_count = 2u,
         .value_root_sets = callee_sets,
         .value_root_set_count = 2u},
        {.key = key("view-root:function:caller"),
         .parameter_types = parameter_types,
         .parameter_modes = modes,
         .parameter_count = 2u,
         .result_type_id = VIEW_TYPE,
         .result_borrow_origins = result_origins,
         .result_borrow_origin_count = 2u,
         .effect_mask = 5u,
         .entry_block = caller_block_key,
         .blocks = &caller_block,
         .block_count = 1u,
         .roots = caller_roots,
         .root_count = 2u,
         .value_root_sets = caller_sets,
         .value_root_set_count = 3u,
         .flags = XR_PROGRAM_FUNCTION_ENTRY},
    };
    XrCoreIrModuleInput module = {
        .key = key("view-root:module"),
        .functions = functions,
        .function_count = 2u,
    };
    XrProgramArtifact artifact = {0};
    CHECK(write_typed_modules(&view_type, 1u, &module, 1u, &artifact) == XR_PROGRAM_BUILD_OK);
    return artifact;
}

static void test_immutable_view_root_tables(void) {
    XrProgramArtifact first = build_view_root_artifact(false, false);
    XrProgramArtifact reordered = build_view_root_artifact(true, false);
    CHECK(first.size != 0u && first.size == reordered.size);
    CHECK(first.size != 0u && memcmp(first.bytes, reordered.bytes, first.size) == 0);
    CHECK(xr_program_id_equal(first.id, reordered.id));
    XrValidatedProgram *program = validate_ok(&first);
    xr_validated_program_free(program);

    XrProgramArtifact incomplete = build_view_root_artifact(false, true);
    expect_semantic_reject(&incomplete, XR_PROGRAM_DIAGNOSTIC_ROOT);

    xr_program_artifact_free(&incomplete);
    xr_program_artifact_free(&reordered);
    xr_program_artifact_free(&first);
}

static XrProgramBuildStatus build_program_table_artifact(bool reverse_inputs, bool bad_slot,
                                                         XrProgramArtifact *artifact) {
    enum {
        IMPLEMENTOR_TYPE = 70,
        EXISTENTIAL_A_TYPE = 71,
        EXISTENTIAL_B_TYPE = 72,
        CALLABLE_TYPE = 73,
    };
    XrCoreIrKey interface_a_key = key("program-table:interface:a");
    XrCoreIrKey interface_b_key = key("program-table:interface:b");
    XrParamMode receiver_mode = XR_PARAM_READ;
    uint16_t interface_a_receiver = EXISTENTIAL_A_TYPE;
    uint16_t interface_b_receiver = EXISTENTIAL_B_TYPE;
    XrCoreIrCallableSignatureInput slot_a = {
        .parameter_types = &interface_a_receiver,
        .parameter_modes = &receiver_mode,
        .parameter_count = 1u,
        .has_receiver = true,
        .receiver_mode = XR_PARAM_READ,
        .result_type_id = XR_CORE_TYPE_VOID,
    };
    XrCoreIrCallableSignatureInput slot_b = slot_a;
    slot_b.parameter_types = &interface_b_receiver;
    if (bad_slot)
        slot_b.result_type_id = XR_CORE_TYPE_I64;
    XrCoreIrCallableSignatureInput callable_signature = {
        .result_type_id = XR_CORE_TYPE_VOID,
    };
    uint16_t implementor_fields[] = {XR_CORE_TYPE_I64};
    XrCoreIrTypeInput types[] = {
        {.key = key("program-table:type:implementor"),
         .local_id = IMPLEMENTOR_TYPE,
         .kind = XR_CORE_IR_TYPE_AGGREGATE,
         .nominal_kind = XR_CORE_IR_NOMINAL_STRUCT,
         .field_types = implementor_fields,
         .field_count = 1u},
        {.key = key("program-table:type:existential-a"),
         .local_id = EXISTENTIAL_A_TYPE,
         .kind = XR_CORE_IR_TYPE_EXISTENTIAL,
         .existential_interface = interface_a_key,
         .interface_use_kind = XR_CORE_IR_INTERFACE_EXISTENTIAL_READ},
        {.key = key("program-table:type:existential-b"),
         .local_id = EXISTENTIAL_B_TYPE,
         .kind = XR_CORE_IR_TYPE_EXISTENTIAL,
         .existential_interface = interface_b_key,
         .interface_use_kind = XR_CORE_IR_INTERFACE_EXISTENTIAL_READ},
        {.key = key("program-table:type:callable"),
         .local_id = CALLABLE_TYPE,
         .kind = XR_CORE_IR_TYPE_CALLABLE,
         .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
         .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
         .callable_signature = &callable_signature},
    };
    XrCoreIrTypeInput ordered_types[4] = {types[0], types[1], types[2], types[3]};
    if (reverse_inputs) {
        for (uint32_t index = 0; index < 4u; ++index)
            ordered_types[index] = types[3u - index];
    }

    XrCoreIrKey function_key = key("program-table:function:implementation");
    XrCoreIrKey block_key = key("program-table:block:implementation");
    XrCoreIrKey receiver_value = key("program-table:value:receiver");
    XrCoreIrValueInput argument = {
        .key = receiver_value,
        .type_id = IMPLEMENTOR_TYPE,
    };
    XrCoreIrKey block_argument_operand[] = {receiver_value};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = block_argument_operand,
         .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN, .result_type_id = XR_CORE_TYPE_VOID},
    };
    XrCoreIrBlockInput block = {
        .key = block_key,
        .arguments = &argument,
        .argument_count = 1u,
        .instructions = instructions,
        .instruction_count = 2u,
    };
    uint16_t implementation_receiver = IMPLEMENTOR_TYPE;
    XrCoreIrFunctionInput function = {
        .key = function_key,
        .parameter_types = &implementation_receiver,
        .parameter_modes = &receiver_mode,
        .parameter_count = 1u,
        .has_receiver = true,
        .receiver_mode = XR_PARAM_READ,
        .result_type_id = XR_CORE_TYPE_VOID,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrKey unrelated_block_key = key("program-table:block:unrelated");
    XrCoreIrInstructionInput unrelated_return = {
        .operation_id = XR_CORE_OP_CORE_RETURN,
        .result_type_id = XR_CORE_TYPE_VOID,
    };
    XrCoreIrBlockInput unrelated_block = {
        .key = unrelated_block_key,
        .instructions = &unrelated_return,
        .instruction_count = 1u,
    };
    XrCoreIrFunctionInput unrelated_function = {
        .key = key("program-table:function:unrelated"),
        .result_type_id = XR_CORE_TYPE_VOID,
        .entry_block = unrelated_block_key,
        .blocks = &unrelated_block,
        .block_count = 1u,
    };
    XrCoreIrFunctionInput functions[] = {function, unrelated_function};
    XrCoreIrModuleInput module = {
        .key = key("program-table:module"),
        .functions = functions,
        .function_count = 2u,
    };
    XrCoreIrInterfaceInput interfaces[] = {
        {.key = interface_a_key, .slots = &slot_a, .slot_count = 1u},
        {.key = interface_b_key, .slots = &slot_b, .slot_count = 1u},
    };
    XrCoreIrInterfaceInput ordered_interfaces[2] = {interfaces[0], interfaces[1]};
    XrCoreIrKey slot_function[] = {function_key};
    XrCoreIrConformanceInput conformances[] = {
        {.key = key("program-table:conformance:a"),
         .implementor_type_id = IMPLEMENTOR_TYPE,
         .implementor_kind = XR_CORE_IR_NOMINAL_STRUCT,
         .interface_key = interface_a_key,
         .slot_functions = slot_function,
         .slot_count = 1u},
        {.key = key("program-table:conformance:b"),
         .implementor_type_id = IMPLEMENTOR_TYPE,
         .implementor_kind = XR_CORE_IR_NOMINAL_STRUCT,
         .interface_key = interface_b_key,
         .slot_functions = slot_function,
         .slot_count = 1u},
    };
    XrCoreIrConformanceInput ordered_conformances[2] = {conformances[0], conformances[1]};
    if (reverse_inputs) {
        ordered_interfaces[0] = interfaces[1];
        ordered_interfaces[1] = interfaces[0];
        ordered_conformances[0] = conformances[1];
        ordered_conformances[1] = conformances[0];
    }
    XrCoreIrKey profile = key("program-table:profile");
    uint16_t features[] = {XR_CORE_FEATURE_CORE_BASE};
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = profile.bytes,
        .required_features = features,
        .required_feature_count = 1u,
        .types = ordered_types,
        .type_count = 4u,
        .interfaces = ordered_interfaces,
        .interface_count = 2u,
        .conformances = ordered_conformances,
        .conformance_count = 2u,
        .modules = &module,
        .module_count = 1u,
    };
    XrCoreIrProgram *program = NULL;
    char diagnostic[256] = {0};
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&input, &program, diagnostic, sizeof(diagnostic));
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, sizeof(diagnostic));
    xr_core_ir_program_free(program);
    return status;
}

static size_t first_conformance_slot_offset(const XrProgramArtifact *artifact) {
    XrProgramView view;
    if (xr_program_decode_structure(artifact->bytes, artifact->size, NULL, &view, NULL, 0u) !=
        XR_PROGRAM_DECODE_OK)
        return SIZE_MAX;
    size_t cursor = (size_t) view.sections[XR_PROGRAM_SECTION_SEMANTIC_METADATA - 1u].offset;
    uint64_t interface_count = test_take_uvar(artifact->bytes, artifact->size, &cursor);
    for (uint64_t interface = 0; interface < interface_count; ++interface) {
        (void) test_take_uvar(artifact->bytes, artifact->size, &cursor);
        cursor += XR_CORE_IR_KEY_SIZE;
        uint64_t slot_count = test_take_uvar(artifact->bytes, artifact->size, &cursor);
        for (uint64_t slot = 0; slot < slot_count; ++slot)
            (void) test_take_uvar(artifact->bytes, artifact->size, &cursor);
    }
    if (test_take_uvar(artifact->bytes, artifact->size, &cursor) == 0u)
        return SIZE_MAX;
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor);
    cursor += XR_CORE_IR_KEY_SIZE;
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor);
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor);
    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor);
    if (test_take_uvar(artifact->bytes, artifact->size, &cursor) == 0u)
        return SIZE_MAX;
    return cursor;
}

static void test_callable_interface_conformance_tables(void) {
    XrProgramArtifact first = {0};
    XrProgramArtifact reordered = {0};
    XrProgramArtifact invalid = {0};
    CHECK(build_program_table_artifact(false, false, &first) == XR_PROGRAM_BUILD_OK);
    CHECK(build_program_table_artifact(true, false, &reordered) == XR_PROGRAM_BUILD_OK);
    CHECK(first.size != 0u && first.size == reordered.size);
    CHECK(first.size != 0u && memcmp(first.bytes, reordered.bytes, first.size) == 0);
    CHECK(xr_program_id_equal(first.id, reordered.id));
    XrValidatedProgram *program = validate_ok(&first);
    xr_validated_program_free(program);
    CHECK(build_program_table_artifact(false, true, &invalid) == XR_PROGRAM_BUILD_INVALID_INPUT);
    size_t slot_offset = first_conformance_slot_offset(&first);
    CHECK(slot_offset != SIZE_MAX && first.bytes[slot_offset] <= 1u);
    if (slot_offset != SIZE_MAX && first.bytes[slot_offset] <= 1u) {
        uint8_t saved = first.bytes[slot_offset];
        first.bytes[slot_offset] = saved == 0u ? 1u : 0u;
        expect_semantic_reject(&first, XR_PROGRAM_DIAGNOSTIC_FUNCTION);
        first.bytes[slot_offset] = saved;
    }
    xr_program_artifact_free(&reordered);
    xr_program_artifact_free(&first);
}

typedef struct ExistentialTypeOffsets {
    size_t read_ownership;
    size_t read_copy_contract;
    size_t read_use_kind;
    size_t aggregate_field;
    uint16_t ref_type_id;
} ExistentialTypeOffsets;

static bool find_existential_type_offsets(const XrProgramArtifact *artifact,
                                          ExistentialTypeOffsets *offsets) {
    memset(offsets, 0, sizeof(*offsets));
    offsets->read_ownership = SIZE_MAX;
    offsets->read_copy_contract = SIZE_MAX;
    offsets->read_use_kind = SIZE_MAX;
    offsets->aggregate_field = SIZE_MAX;
    XrProgramView view;
    if (xr_program_decode_structure(artifact->bytes, artifact->size, NULL, &view, NULL, 0u) !=
        XR_PROGRAM_DECODE_OK)
        return false;
    size_t cursor = (size_t) view.sections[XR_PROGRAM_SECTION_TYPES - 1u].offset;
    uint64_t total_count = test_take_uvar(artifact->bytes, artifact->size, &cursor);
    for (uint32_t builtin = 0u; builtin < 6u; ++builtin)
        for (uint32_t field = 0u; field < 4u; ++field)
            (void) test_take_uvar(artifact->bytes, artifact->size, &cursor);
    for (uint64_t index = 6u; index < total_count; ++index) {
        uint64_t type_id = test_take_uvar(artifact->bytes, artifact->size, &cursor);
        uint64_t kind = test_take_uvar(artifact->bytes, artifact->size, &cursor);
        size_t ownership = cursor;
        (void) test_take_uvar(artifact->bytes, artifact->size, &cursor);
        size_t copy_contract = cursor;
        (void) test_take_uvar(artifact->bytes, artifact->size, &cursor);
        if (cursor > artifact->size || XR_CORE_IR_KEY_SIZE > artifact->size - cursor)
            return false;
        cursor += XR_CORE_IR_KEY_SIZE;
        (void) test_take_uvar(artifact->bytes, artifact->size, &cursor);
        if (kind == XR_PROGRAM_TYPE_KIND_AGGREGATE) {
            uint64_t field_count = test_take_uvar(artifact->bytes, artifact->size, &cursor);
            for (uint64_t field = 0u; field < field_count; ++field) {
                if (offsets->aggregate_field == SIZE_MAX)
                    offsets->aggregate_field = cursor;
                (void) test_take_uvar(artifact->bytes, artifact->size, &cursor);
            }
        } else if (kind == XR_PROGRAM_TYPE_KIND_VARIANT) {
            uint64_t variant_count = test_take_uvar(artifact->bytes, artifact->size, &cursor);
            for (uint64_t variant = 0u; variant < variant_count; ++variant) {
                uint64_t field_count = test_take_uvar(artifact->bytes, artifact->size, &cursor);
                for (uint64_t field = 0u; field < field_count; ++field)
                    (void) test_take_uvar(artifact->bytes, artifact->size, &cursor);
            }
        } else if (kind == XR_PROGRAM_TYPE_KIND_VIEW) {
            (void) test_take_uvar(artifact->bytes, artifact->size, &cursor);
        } else if (kind == XR_PROGRAM_TYPE_KIND_EXISTENTIAL) {
            size_t use_kind_offset = cursor;
            uint64_t use_kind = test_take_uvar(artifact->bytes, artifact->size, &cursor);
            if (use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_READ) {
                offsets->read_ownership = ownership;
                offsets->read_copy_contract = copy_contract;
                offsets->read_use_kind = use_kind_offset;
            } else if (use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_REF && type_id <= UINT16_MAX) {
                offsets->ref_type_id = (uint16_t) type_id;
            }
        }
    }
    return offsets->read_ownership != SIZE_MAX && offsets->read_copy_contract != SIZE_MAX &&
           offsets->read_use_kind != SIZE_MAX && offsets->aggregate_field != SIZE_MAX &&
           offsets->ref_type_id != 0u;
}

static void test_existential_ref_non_escape_admission(const XrProgramArtifact *artifact) {
    ExistentialTypeOffsets offsets;
    bool found = find_existential_type_offsets(artifact, &offsets);
    CHECK(found);
    if (!found)
        return;
    uint8_t *bytes = malloc(artifact->size);
    CHECK(bytes != NULL);
    if (!bytes)
        return;
    XrProgramArtifact mutated = {.bytes = bytes, .size = artifact->size};

    memcpy(bytes, artifact->bytes, artifact->size);
    bytes[offsets.read_ownership] = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
    bytes[offsets.read_copy_contract] = XR_CORE_IR_COPY_FORBIDDEN;
    bytes[offsets.read_use_kind] = XR_CORE_IR_INTERFACE_EXISTENTIAL_REF;
    expect_semantic_reject(&mutated, XR_PROGRAM_DIAGNOSTIC_FUNCTION);

    CHECK(offsets.ref_type_id < UINT8_C(0x80));
    if (offsets.ref_type_id >= UINT8_C(0x80)) {
        free(bytes);
        return;
    }
    memcpy(bytes, artifact->bytes, artifact->size);
    bytes[offsets.aggregate_field] = (uint8_t) offsets.ref_type_id;
    expect_semantic_reject(&mutated, XR_PROGRAM_DIAGNOSTIC_TYPE);
    free(bytes);
}

static void test_existential_pack_test_project(void) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    CHECK(xr_program_existential_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
          XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = validate_ok(&artifact);
    XrReferenceProfile profile = {.pointer_width = 64u};
    XrReferenceOutcome result = xr_reference_evaluate(
        program, xr_validated_program_entry_function(program), NULL, 0u, &profile, NULL);
    CHECK(result.kind == XR_REFERENCE_OUTCOME_RETURN);
    CHECK(result.value.kind == XR_REFERENCE_VALUE_I64);
    CHECK(result.value.as.i64 == 42);
    test_existential_ref_non_escape_admission(&artifact);
    xr_validated_program_free(program);
    xr_program_artifact_free(&artifact);

    const XrProgramExistentialFixtureMutation rejected[] = {
        XR_EXISTENTIAL_FIXTURE_UNDOMINATED_PROJECT,
        XR_EXISTENTIAL_FIXTURE_FALSE_EDGE_PROJECT,
        XR_EXISTENTIAL_FIXTURE_NO_CONFORMANCE,
        XR_EXISTENTIAL_FIXTURE_WITNESS_DIRECT_FALLIBLE,
        XR_EXISTENTIAL_FIXTURE_WITNESS_INVOKE_INFALLIBLE,
    };
    for (size_t index = 0; index < sizeof(rejected) / sizeof(rejected[0]); ++index) {
        memset(&artifact, 0, sizeof(artifact));
        CHECK(xr_program_existential_fixture_write_mutated(rejected[index], &artifact, diagnostic,
                                                           sizeof(diagnostic)) ==
              XR_PROGRAM_BUILD_OK);
        expect_semantic_reject(&artifact, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE);
        xr_program_artifact_free(&artifact);
    }

    memset(&artifact, 0, sizeof(artifact));
    CHECK(xr_program_existential_fixture_write_mutated(
              XR_EXISTENTIAL_FIXTURE_WITNESS_SLOT_OUT_OF_RANGE, &artifact, diagnostic,
              sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
    expect_semantic_reject(&artifact, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE);
    xr_program_artifact_free(&artifact);

    const XrProgramExistentialFixtureMutation escaped_refs[] = {
        XR_EXISTENTIAL_FIXTURE_REF_RESULT_ESCAPE,
        XR_EXISTENTIAL_FIXTURE_REF_FIELD_ESCAPE,
    };
    for (size_t index = 0; index < sizeof(escaped_refs) / sizeof(escaped_refs[0]); ++index) {
        memset(&artifact, 0, sizeof(artifact));
        CHECK(xr_program_existential_fixture_write_mutated(escaped_refs[index], &artifact,
                                                           diagnostic, sizeof(diagnostic)) ==
              XR_PROGRAM_BUILD_INVALID_INPUT);
        xr_program_artifact_free(&artifact);
    }
}

static void test_callable_pack_and_indirect_calls(void) {
    _Static_assert(XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT == 38, "indirect direct stable id drifted");
    _Static_assert(XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE == 39, "indirect invoke stable id drifted");
    _Static_assert(XR_CORE_OP_CORE_CALLABLE_PACK == 89, "callable pack stable id drifted");
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    CHECK(xr_program_callable_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
          XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = validate_ok(&artifact);
    if (program) {
        XrReferenceProfile profile = {.pointer_width = 64u};
        XrReferenceOutcome result = xr_reference_evaluate(
            program, xr_validated_program_entry_function(program), NULL, 0u, &profile, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_RETURN);
        CHECK(result.value.kind == XR_REFERENCE_VALUE_I64);
        CHECK(result.value.as.i64 == 42);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);

    const XrProgramCallableFixtureMutation rejected[] = {
        XR_CALLABLE_FIXTURE_PACK_SIGNATURE_MISMATCH,
        XR_CALLABLE_FIXTURE_PACK_CAPABILITY_EXCESS,
        XR_CALLABLE_FIXTURE_DIRECT_FALLIBLE,
        XR_CALLABLE_FIXTURE_INVOKE_INFALLIBLE,
    };
    for (size_t index = 0; index < sizeof(rejected) / sizeof(rejected[0]); ++index) {
        memset(&artifact, 0, sizeof(artifact));
        CHECK(xr_program_callable_fixture_write_mutated(rejected[index], &artifact, diagnostic,
                                                        sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
        expect_semantic_reject(&artifact, rejected[index] == XR_CALLABLE_FIXTURE_INVOKE_INFALLIBLE
                                              ? XR_PROGRAM_DIAGNOSTIC_VALUE_USE
                                              : XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE);
        xr_program_artifact_free(&artifact);
    }
}

int main(void) {
    test_aggregate_variant_operations();
    test_dynamic_type_graph_rejection();
    test_scalar_operations();
    test_control_and_profile();
    test_direct_call();
    test_owner_and_local_place_operations();
    test_affine_owner_domain();
    test_parameter_modes_and_value_categories();
    test_terminal_operations();
    test_sealed_invoke_and_cleanup_cfg();
    test_typed_panic_invoke_and_cleanup_cfg();
    test_negative_diagnostics_and_budget();
    test_evaluator_traps_and_budget();
    test_typed_random_and_linear_work();
    test_immutable_view_root_tables();
    test_callable_interface_conformance_tables();
    test_existential_pack_test_project();
    test_callable_pack_and_indirect_calls();
    if (failures != 0) {
        fprintf(stderr, "XrProgram verifier tests failed: %d\n", failures);
        return 1;
    }
    puts("XrProgram verifier and reference evaluator tests passed");
    return 0;
}
