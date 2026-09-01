/*
 * Task 296: canonical CoreIR -> XrProgram writer and bounded structural decode.
 */

#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"
#include "program/xr_program_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

typedef enum FixtureMutation {
    FIXTURE_VALID = 0,
    FIXTURE_UNKNOWN_OPERATION,
    FIXTURE_UNRESOLVED_OPERAND,
    FIXTURE_DUPLICATE_RESULT,
} FixtureMutation;

static XrCoreIrKey key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static bool contains_bytes(const uint8_t *haystack, size_t haystack_size, const char *needle) {
    size_t needle_size = strlen(needle);
    if (needle_size == 0 || needle_size > haystack_size)
        return false;
    for (size_t index = 0; index <= haystack_size - needle_size; ++index) {
        if (memcmp(haystack + index, needle, needle_size) == 0)
            return true;
    }
    return false;
}

static XrProgramBuildStatus build_fixture(bool reverse_modules, bool alternate_roots,
                                          FixtureMutation mutation, const char *profile_name,
                                          XrProgramArtifact *artifact, char *diagnostic,
                                          size_t diagnostic_size) {
    XrCoreIrConstantInput constants[] = {
        {.key = key("constant:40"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 40},
        {.key = key("constant:2"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 2},
    };
    XrCoreIrKey helper_block_key = key("helper:block:entry");
    XrCoreIrKey main_block_key = key("main:block:entry");
    XrCoreIrKey value_40 = key("helper:value:40");
    XrCoreIrKey value_2 = key("helper:value:2");
    XrCoreIrKey value_sum = key("helper:value:sum");
    XrCoreIrKey value_call = key("main:value:call");
    XrCoreIrKey helper_key = key("function:helper:i64");
    XrCoreIrKey main_key = key("function:main:i64");
    XrCoreIrKey add_operands[] = {value_40, value_2};
    XrCoreIrKey helper_return_operands[] = {value_sum};
    XrCoreIrKey main_return_operands[] = {value_call};
    XrCoreIrInstructionInput helper_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = value_40,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = value_2,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_ADD_I64,
         .result = value_sum,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = add_operands,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = helper_return_operands,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrInstructionInput main_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CALL_SEALED_DIRECT,
         .result = value_call,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = helper_key},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = main_return_operands,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    if (mutation == FIXTURE_UNKNOWN_OPERATION)
        helper_instructions[2].operation_id = UINT16_MAX;
    if (mutation == FIXTURE_UNRESOLVED_OPERAND)
        add_operands[1] = key("missing:value");
    if (mutation == FIXTURE_DUPLICATE_RESULT)
        helper_instructions[1].result = value_40;

    XrCoreIrBlockInput helper_blocks[] = {
        {.key = helper_block_key,
         .instructions = helper_instructions,
         .instruction_count = sizeof(helper_instructions) / sizeof(helper_instructions[0])},
    };
    XrCoreIrBlockInput main_blocks[] = {
        {.key = main_block_key,
         .instructions = main_instructions,
         .instruction_count = sizeof(main_instructions) / sizeof(main_instructions[0])},
    };
    XrCoreIrFunctionInput helper = {
        .key = helper_key,
        .result_type_id = XR_CORE_TYPE_I64,
        .entry_block = helper_block_key,
        .blocks = helper_blocks,
        .block_count = 1,
    };
    XrCoreIrFunctionInput main = {
        .key = main_key,
        .result_type_id = XR_CORE_TYPE_I64,
        .entry_block = main_block_key,
        .blocks = main_blocks,
        .block_count = 1,
        .flags = 1,
    };
    XrCoreIrModuleInput modules[] = {
        {.key = key(alternate_roots ? "root-b:module-helper" : "root-a:module-helper"),
         .constants = constants,
         .constant_count = sizeof(constants) / sizeof(constants[0]),
         .functions = &helper,
         .function_count = 1},
        {.key = key(alternate_roots ? "root-b:module-main" : "root-a:module-main"),
         .functions = &main,
         .function_count = 1},
    };
    XrCoreIrModuleInput ordered[2] = {modules[0], modules[1]};
    if (reverse_modules) {
        ordered[0] = modules[1];
        ordered[1] = modules[0];
    }
    XrCoreIrKey profile = key(profile_name);
    uint16_t features[] = {XR_CORE_FEATURE_CORE_BASE};
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = profile.bytes,
        .required_features = features,
        .required_feature_count = 1,
        .modules = ordered,
        .module_count = 2,
    };
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&input, &program, diagnostic, diagnostic_size);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, diagnostic_size);
    xr_core_ir_program_free(program);
    return status;
}

static void test_determinism_roundtrip_and_identity(void) {
    char diagnostic[256] = {0};
    XrProgramArtifact first = {0};
    XrProgramArtifact reordered = {0};
    XrProgramArtifact rerooted = {0};
    XrProgramArtifact reencoded = {0};
    XrProgramView view;
    CHECK(build_fixture(false, false, FIXTURE_VALID, "profile:checked", &first, diagnostic,
                        sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
    CHECK(build_fixture(true, false, FIXTURE_VALID, "profile:checked", &reordered, diagnostic,
                        sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
    CHECK(build_fixture(true, true, FIXTURE_VALID, "profile:checked", &rerooted, diagnostic,
                        sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
    CHECK(first.size != 0);
    CHECK(first.size == reordered.size && memcmp(first.bytes, reordered.bytes, first.size) == 0);
    CHECK(first.size == rerooted.size && memcmp(first.bytes, rerooted.bytes, first.size) == 0);
    CHECK(xr_program_id_equal(first.id, reordered.id));
    CHECK(xr_program_id_equal(first.id, rerooted.id));
    CHECK(!contains_bytes(first.bytes, first.size, "root-a"));
    CHECK(!contains_bytes(first.bytes, first.size, "module-helper"));
    CHECK(xr_program_decode_structure(first.bytes, first.size, NULL, &view, diagnostic,
                                      sizeof(diagnostic)) == XR_PROGRAM_DECODE_OK);
    CHECK(view.format_major == XR_PROGRAM_FORMAT_MAJOR);
    CHECK(view.required_feature_count == 1);
    CHECK(view.section_count == XR_PROGRAM_REQUIRED_SECTION_COUNT);
    CHECK(xr_program_id_equal(view.id, first.id));
    CHECK(xr_program_reencode(&view, &reencoded, diagnostic, sizeof(diagnostic)) ==
          XR_PROGRAM_DECODE_OK);
    CHECK(first.size == reencoded.size && memcmp(first.bytes, reencoded.bytes, first.size) == 0);
    CHECK(xr_program_id_equal(first.id, reencoded.id));

    char id_hex[XR_PROGRAM_DIGEST_SIZE * 2u + 1u];
    xr_program_id_hex(first.id, id_hex);
    CHECK(first.size == 208u);
    CHECK(strcmp(id_hex, "b42660098853abe00a90a2958553d0534a5d9fed496580a25f423b4b2b8c4c42") == 0);
    printf("Task 296 walking-skeleton ProgramId: %s (%zu bytes)\n", id_hex, first.size);

    xr_program_artifact_free(&reencoded);
    xr_program_artifact_free(&rerooted);
    xr_program_artifact_free(&reordered);
    xr_program_artifact_free(&first);
}

static void expect_decode_status(const uint8_t *bytes, size_t size,
                                 const XrProgramDecodeBudget *budget,
                                 XrProgramDecodeStatus expected) {
    XrProgramView view;
    char diagnostic[256] = {0};
    XrProgramDecodeStatus status =
        xr_program_decode_structure(bytes, size, budget, &view, diagnostic, sizeof(diagnostic));
    if (status != expected)
        fprintf(stderr, "decode status mismatch: expected=%s actual=%s diagnostic=%s\n",
                xr_program_decode_status_name(expected), xr_program_decode_status_name(status),
                diagnostic);
    CHECK(status == expected);
}

static void test_hostile_structure_and_budget(void) {
    char diagnostic[256] = {0};
    XrProgramArtifact artifact = {0};
    CHECK(build_fixture(false, false, FIXTURE_VALID, "profile:checked", &artifact, diagnostic,
                        sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
    uint8_t *mutated = malloc(artifact.size);
    CHECK(mutated != NULL);
    if (!mutated) {
        xr_program_artifact_free(&artifact);
        return;
    }
    memcpy(mutated, artifact.bytes, artifact.size);
    mutated[0] ^= UINT8_C(0xff);
    expect_decode_status(mutated, artifact.size, NULL, XR_PROGRAM_DECODE_BAD_MAGIC);

    memcpy(mutated, artifact.bytes, artifact.size);
    mutated[XR_PROGRAM_MAGIC_SIZE] = 2u;
    expect_decode_status(mutated, artifact.size, NULL, XR_PROGRAM_DECODE_UNSUPPORTED_VERSION);

    expect_decode_status(artifact.bytes, artifact.size - 1u, NULL,
                         XR_PROGRAM_DECODE_INVALID_SECTION);

    /* 8-byte magic + two u16 versions + one-byte epoch + two 32-byte digests. */
    size_t feature_id_offset = XR_PROGRAM_MAGIC_SIZE + 4u + 1u + 64u + 1u;
    memcpy(mutated, artifact.bytes, artifact.size);
    mutated[feature_id_offset] = 2u;
    expect_decode_status(mutated, artifact.size, NULL, XR_PROGRAM_DECODE_UNSUPPORTED_FEATURE);

    /* The sole feature and section count are one byte each; the first directory
     * entry begins immediately afterward. Its relative offset must be zero. */
    size_t first_relative_offset = feature_id_offset + 1u + 1u + 1u;
    memcpy(mutated, artifact.bytes, artifact.size);
    mutated[first_relative_offset] = 1u;
    expect_decode_status(mutated, artifact.size, NULL, XR_PROGRAM_DECODE_NONCANONICAL);

    XrProgramDecodeBudget budget = xr_program_decode_default_budget();
    budget.max_bytes = artifact.size - 1u;
    expect_decode_status(artifact.bytes, artifact.size, &budget, XR_PROGRAM_DECODE_RESOURCE_LIMIT);

    free(mutated);
    xr_program_artifact_free(&artifact);
}

static void test_semantic_profile_and_invalid_core_ir(void) {
    char diagnostic[256] = {0};
    XrProgramArtifact checked = {0};
    XrProgramArtifact wrapping = {0};
    CHECK(build_fixture(false, false, FIXTURE_VALID, "profile:checked", &checked, diagnostic,
                        sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
    CHECK(build_fixture(false, false, FIXTURE_VALID, "profile:wrapping", &wrapping, diagnostic,
                        sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
    CHECK(!xr_program_id_equal(checked.id, wrapping.id));
    CHECK(checked.size == wrapping.size &&
          memcmp(checked.bytes, wrapping.bytes, checked.size) != 0);
    xr_program_artifact_free(&wrapping);
    xr_program_artifact_free(&checked);

    XrProgramArtifact unused = {0};
    CHECK(build_fixture(false, false, FIXTURE_UNKNOWN_OPERATION, "profile:checked", &unused,
                        diagnostic, sizeof(diagnostic)) == XR_PROGRAM_BUILD_INVALID_INPUT);
    CHECK(build_fixture(false, false, FIXTURE_UNRESOLVED_OPERAND, "profile:checked", &unused,
                        diagnostic, sizeof(diagnostic)) == XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE);
    CHECK(build_fixture(false, false, FIXTURE_DUPLICATE_RESULT, "profile:checked", &unused,
                        diagnostic, sizeof(diagnostic)) == XR_PROGRAM_BUILD_DUPLICATE_IDENTITY);
}

int main(void) {
    test_determinism_roundtrip_and_identity();
    test_hostile_structure_and_budget();
    test_semantic_profile_and_invalid_core_ir();
    if (failures != 0) {
        fprintf(stderr, "XrProgram tests failed: %d\n", failures);
        return 1;
    }
    puts("XrProgram canonical writer and structural decoder tests passed");
    return 0;
}
