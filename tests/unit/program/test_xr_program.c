/*
 * Task 296: canonical CoreIR -> XrProgram writer and bounded structural decode.
 */

#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"
#include "program/xr_program_decode.h"
#include "program/xr_program_verify.h"
#include "program/xr_program_internal.h"
#include "program/xr_validated_program_internal.h"
#include "xr_program_allocation_probe.h"
#include "xr_program_provider_fixture.h"
#include "xr_program_construct_fixture.h"
#include "xr_program_coroutine_trap_fixture.h"
#include "xr_program_text_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static struct {
    bool enabled;
    size_t fail_at;
    size_t attempts;
    size_t allocated;
    size_t released;
    void *live[1024];
} allocation_probe;

static bool allocation_probe_reject(void) {
    return allocation_probe.enabled && ++allocation_probe.attempts == allocation_probe.fail_at;
}

static void *allocation_probe_record(void *pointer) {
    if (!pointer || !allocation_probe.enabled)
        return pointer;
    for (size_t index = 0u; index < XR_COUNTOF(allocation_probe.live); ++index) {
        if (allocation_probe.live[index])
            continue;
        allocation_probe.live[index] = pointer;
        ++allocation_probe.allocated;
        return pointer;
    }
    fprintf(stderr, "Program allocation probe capacity exceeded\n");
    abort();
}

void *xr_program_test_malloc(size_t size) {
    return allocation_probe_reject() ? NULL
                                     : allocation_probe_record(xr_program_test_system_malloc(size));
}

void *xr_program_test_calloc(size_t count, size_t size) {
    return allocation_probe_reject()
               ? NULL
               : allocation_probe_record(xr_program_test_system_calloc(count, size));
}

static size_t allocation_probe_slot(void *pointer) {
    for (size_t index = 0u; index < XR_COUNTOF(allocation_probe.live); ++index) {
        if (allocation_probe.live[index] == pointer)
            return index;
    }
    fprintf(stderr, "Program used unowned allocation at failure %zu\n", allocation_probe.fail_at);
    abort();
}

void *xr_program_test_realloc(void *pointer, size_t size) {
    if (!allocation_probe.enabled)
        return xr_program_test_system_realloc(pointer, size);
    /* The writer only grows buffers. Keep failed realloc ownership unchanged. */
    if (size == 0u)
        abort();
    if (allocation_probe_reject())
        return NULL;
    bool replacing = pointer != NULL;
    size_t slot = replacing ? allocation_probe_slot(pointer) : 0u;
    void *grown = xr_program_test_system_realloc(pointer, size);
    if (!grown)
        return NULL;
    if (!replacing)
        return allocation_probe_record(grown);
    allocation_probe.live[slot] = grown;
    ++allocation_probe.released;
    ++allocation_probe.allocated;
    return grown;
}

void xr_program_test_free(void *pointer) {
    if (pointer && allocation_probe.enabled) {
        allocation_probe.live[allocation_probe_slot(pointer)] = NULL;
        ++allocation_probe.released;
    }
    xr_program_test_system_free(pointer);
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static void allocation_probe_begin(size_t fail_at) {
    memset(&allocation_probe, 0, sizeof(allocation_probe));
    allocation_probe.enabled = true;
    allocation_probe.fail_at = fail_at;
}

static void allocation_probe_end(void) {
    if (allocation_probe.allocated != allocation_probe.released)
        fprintf(stderr, "Program failure %zu leaked %zu allocations\n", allocation_probe.fail_at,
                allocation_probe.allocated - allocation_probe.released);
    CHECK(allocation_probe.allocated == allocation_probe.released);
    allocation_probe.enabled = false;
}

typedef enum FixtureMutation {
    FIXTURE_VALID = 0,
    FIXTURE_UNKNOWN_OPERATION,
    FIXTURE_UNRESOLVED_OPERAND,
    FIXTURE_DUPLICATE_RESULT,
    FIXTURE_CONFLICTING_PROVIDER_CONTRACT,
    FIXTURE_CHANGED_PROVIDER_EFFECT,
    FIXTURE_INVALID_PROVIDER_CONTRACT,
} FixtureMutation;

static XrCoreIrKey key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrStableId stable_id(const char *text) {
    XrCoreIrKey value = key(text);
    XrStableId id;
    memcpy(id.bytes, value.bytes, sizeof(id.bytes));
    return id;
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
    XrCoreIrKey value_provider_a = key("helper:value:provider-a");
    XrCoreIrKey value_provider_b = key("helper:value:provider-b");
    XrCoreIrKey value_provider_c = key("helper:value:provider-c");
    XrCoreIrKey value_call = key("main:value:call");
    XrCoreIrKey helper_key = key("function:helper:i64");
    XrCoreIrKey main_key = key("function:main:i64");
    XrCoreIrKey add_operands[] = {value_40, value_2};
    XrCoreIrKey provider_a_operands[] = {value_sum};
    XrCoreIrKey provider_b_operands[] = {value_provider_a};
    XrCoreIrKey provider_c_operands[] = {value_provider_b};
    XrCoreIrKey helper_return_operands[] = {value_provider_c};
    XrCoreIrKey main_return_operands[] = {value_call};
    XrStableId contract_a = stable_id("provider-contract:a");
    XrStableId contract_b = stable_id("provider-contract:b");
    XrStableId operation_a = stable_id("provider-operation:a");
    XrStableId operation_b = stable_id("provider-operation:b");
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
        {.operation_id = XR_CORE_OP_CORE_PROVIDER_CALL,
         .result = value_provider_a,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = provider_a_operands,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
         .immediate.provider_operation = {.contract_id = contract_a, .operation_id = operation_a}},
        {.operation_id = XR_CORE_OP_CORE_PROVIDER_CALL,
         .result = value_provider_b,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = provider_b_operands,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
         .immediate.provider_operation = {.contract_id = contract_a, .operation_id = operation_b}},
        {.operation_id = XR_CORE_OP_CORE_PROVIDER_CALL,
         .result = value_provider_c,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = provider_c_operands,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
         .immediate.provider_operation = {.contract_id = contract_b, .operation_id = operation_a}},
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
        .effect_mask = XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_PROVIDER_CALL,
        .capability_mask = XR_CORE_CAPABILITY_PROVIDER_BINDING,
        .entry_block = helper_block_key,
        .blocks = helper_blocks,
        .block_count = 1,
    };
    XrCoreIrFunctionInput main = {
        .key = main_key,
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_PROVIDER_CALL,
        .capability_mask = XR_CORE_CAPABILITY_PROVIDER_BINDING,
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
    XrProgramProviderOperationRequirement requirement_a = {
        .operation_id = operation_a,
        .logical_contract = xr_program_fixture_scalar_contract(false),
    };
    if (mutation == FIXTURE_CHANGED_PROVIDER_EFFECT)
        requirement_a.logical_contract.effects = XR_PROVIDER_EFFECT_READS_PROCESS;
    if (mutation == FIXTURE_INVALID_PROVIDER_CONTRACT)
        requirement_a.logical_contract.refusal = 0u;
    XrProgramProviderOperationRequirement requirement_b = {
        .operation_id = operation_b,
        .logical_contract = xr_program_fixture_scalar_contract(false),
    };
    XrProgramProviderOperationRequirement operations_ab[] = {requirement_a, requirement_b};
    XrProgramProviderOperationRequirement operations_ba[] = {requirement_b, requirement_a};
    if (mutation == FIXTURE_CONFLICTING_PROVIDER_CONTRACT)
        requirement_a.logical_contract.effects = XR_PROVIDER_EFFECT_READS_PROCESS;
    XrCoreIrProviderRequirementInput forward_requirements[] = {
        {.contract_id = contract_b, .operations = &requirement_a, .operation_count = 1u},
        {.contract_id = contract_a, .operations = operations_ba, .operation_count = 2u},
        {.contract_id = contract_a, .operations = &requirement_a, .operation_count = 1u},
    };
    XrCoreIrProviderRequirementInput reverse_requirements[] = {
        {.contract_id = contract_a, .operations = &requirement_a, .operation_count = 1u},
        {.contract_id = contract_a, .operations = operations_ab, .operation_count = 2u},
        {.contract_id = contract_b, .operations = &requirement_a, .operation_count = 1u},
    };
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = profile.bytes,
        .required_features = features,
        .required_feature_count = 1,
        .provider_requirements = reverse_modules ? reverse_requirements : forward_requirements,
        .provider_requirement_count = 3u,
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
    CHECK(view.provider_requirement_count == 2u);
    CHECK(view.section_count == XR_PROGRAM_REQUIRED_SECTION_COUNT);
    CHECK(xr_program_id_equal(view.id, first.id));
    CHECK(xr_program_reencode(&view, &reencoded, diagnostic, sizeof(diagnostic)) ==
          XR_PROGRAM_DECODE_OK);
    CHECK(first.size == reencoded.size && memcmp(first.bytes, reencoded.bytes, first.size) == 0);
    CHECK(xr_program_id_equal(first.id, reencoded.id));

    char id_hex[XR_PROGRAM_DIGEST_SIZE * 2u + 1u];
    xr_program_id_hex(first.id, id_hex);
    /* Independently derived by setting format minor to one and appending a
     * zero module count to semantic metadata, including its directory length. */
    CHECK(first.size == 477u);
    printf("Task 296 walking-skeleton ProgramId: %s (%zu bytes)\n", id_hex, first.size);
    CHECK(strcmp(id_hex, "c1fd51fb97573aed653736925758d033c4a6ccd00d84f1e2ca9e6d21b9d64abb") == 0);

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

    XrProgramView valid_view;
    CHECK(xr_program_decode_structure(artifact.bytes, artifact.size, NULL, &valid_view, diagnostic,
                                      sizeof(diagnostic)) == XR_PROGRAM_DECODE_OK);
    memcpy(mutated, artifact.bytes, artifact.size);
    memset(mutated + valid_view.sections[XR_PROGRAM_SECTION_IMPORTS - 1u].offset + 1u, 0,
           XR_STABLE_ID_BYTES);
    expect_decode_status(mutated, artifact.size, NULL, XR_PROGRAM_DECODE_NONCANONICAL);

    memcpy(mutated, artifact.bytes, artifact.size);
    mutated[valid_view.sections[XR_PROGRAM_SECTION_IMPORTS - 1u].offset + 1u + XR_STABLE_ID_BYTES] =
        0u;
    expect_decode_status(mutated, artifact.size, NULL, XR_PROGRAM_DECODE_NONCANONICAL);

    memcpy(mutated, artifact.bytes, artifact.size);
    mutated[XR_PROGRAM_MAGIC_SIZE] = 1u;
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
    CHECK(build_fixture(false, false, FIXTURE_CONFLICTING_PROVIDER_CONTRACT, "profile:checked",
                        &unused, diagnostic, sizeof(diagnostic)) == XR_PROGRAM_BUILD_INVALID_INPUT);
    CHECK(build_fixture(false, false, FIXTURE_INVALID_PROVIDER_CONTRACT, "profile:checked", &unused,
                        diagnostic, sizeof(diagnostic)) == XR_PROGRAM_BUILD_INVALID_INPUT);
}

static void test_provider_semantics_are_program_identity(void) {
    char diagnostic[256] = {0};
    XrProgramArtifact clock = {0};
    XrProgramArtifact process = {0};
    CHECK(build_fixture(false, false, FIXTURE_VALID, "profile:checked", &clock, diagnostic,
                        sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
    CHECK(build_fixture(false, false, FIXTURE_CHANGED_PROVIDER_EFFECT, "profile:checked", &process,
                        diagnostic, sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
    CHECK(!xr_program_id_equal(clock.id, process.id));
    CHECK(clock.size == process.size && memcmp(clock.bytes, process.bytes, clock.size) != 0);
    xr_program_artifact_free(&process);
    xr_program_artifact_free(&clock);
}

static void test_partial_core_ir_construction(void) {
    uint8_t first_bytes[] = "first";
    uint8_t last_bytes[] = "last";
    XrCoreIrConstantInput constants[] = {
        {.key = key("string:first"),
         .type_id = XR_CORE_TYPE_STRING,
         .kind = XR_CORE_IR_CONSTANT_STRING,
         .value.string = {.bytes = first_bytes, .size = sizeof(first_bytes) - 1u}},
        {.key = key("string:missing"),
         .type_id = XR_CORE_TYPE_STRING,
         .kind = XR_CORE_IR_CONSTANT_STRING,
         .value.string = {.size = 1u}},
        {.key = key("string:last"),
         .type_id = XR_CORE_TYPE_STRING,
         .kind = XR_CORE_IR_CONSTANT_STRING,
         .value.string = {.bytes = last_bytes, .size = sizeof(last_bytes) - 1u}},
    };
    XrCoreIrInstructionInput instruction = {
        .operation_id = XR_CORE_OP_CORE_RETURN,
        .result_type_id = XR_CORE_TYPE_VOID,
    };
    XrCoreIrBlockInput block = {
        .key = key("partial:block"),
        .instructions = &instruction,
        .instruction_count = 1u,
    };
    XrCoreIrFunctionInput function = {
        .key = key("partial:function"),
        .entry_block = block.key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = key("partial:module"),
        .functions = &function,
        .function_count = 1u,
        .constants = constants,
        .constant_count = 3u,
    };
    XrCoreIrKey profile = key("partial:profile");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = profile.bytes,
        .required_features = &feature,
        .required_feature_count = 1u,
        .modules = &module,
        .module_count = 1u,
    };
    XrCoreIrProgram *program = NULL;
    char diagnostic[256];
    CHECK(xr_core_ir_program_build(&input, &program, diagnostic, sizeof(diagnostic)) ==
          XR_PROGRAM_BUILD_INVALID_INPUT);
    CHECK(program == NULL);
    CHECK(memcmp(first_bytes, "first", sizeof(first_bytes)) == 0);
    CHECK(memcmp(last_bytes, "last", sizeof(last_bytes)) == 0);

    constants[1].value.string.size = 0u;
    CHECK(xr_core_ir_program_build(&input, &program, diagnostic, sizeof(diagnostic)) ==
          XR_PROGRAM_BUILD_OK);
    CHECK(program != NULL);
    memset(first_bytes, 'x', sizeof(first_bytes) - 1u);
    memset(last_bytes, 'y', sizeof(last_bytes) - 1u);
    XrProgramArtifact artifact = {0};
    CHECK(xr_program_write(program, &artifact, diagnostic, sizeof(diagnostic)) ==
          XR_PROGRAM_BUILD_OK);
    CHECK(contains_bytes(artifact.bytes, artifact.size, "first"));
    CHECK(contains_bytes(artifact.bytes, artifact.size, "last"));
    xr_program_artifact_free(&artifact);
    xr_core_ir_program_free(program);

    uint16_t payload_type = XR_CORE_TYPE_I64;
    XrCoreIrVariantInput variants[] = {
        {.payload_types = &payload_type, .payload_count = 1u},
        {.payload_count = 1u},
    };
    XrCoreIrCallableSignatureInput signature = {
        .parameter_types = &payload_type,
        .parameter_count = 1u,
        .result_borrow_origin_count = 1u,
    };
    XrCoreIrTypeInput type = {
        .key = key("partial:type"),
        .local_id = XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE,
        .kind = XR_CORE_IR_TYPE_VARIANT,
        .variant_count = 2u,
    };
    XrCoreIrInterfaceInput interface = {
        .key = key("partial:interface"),
        .slot_count = 2u,
    };
    for (uint32_t mutation = 0u; mutation < 9u; ++mutation) {
        XrCoreIrModuleInput bad_module = module;
        XrCoreIrFunctionInput bad_function = function;
        XrCoreIrBlockInput bad_block = block;
        XrCoreIrProgramInput bad_input = input;
        bad_module.constants = NULL;
        bad_module.constant_count = 0u;
        bad_module.functions = &bad_function;
        bad_function.blocks = &bad_block;
        bad_input.modules = &bad_module;
        switch (mutation) {
            case 0u:  // Unallocated module function storage.
                bad_module.functions = NULL;
                bad_module.function_count = 2u;
                break;
            case 1u:  // Unallocated function block storage.
                bad_function.blocks = NULL;
                bad_function.block_count = 2u;
                break;
            case 2u:  // Unallocated instruction storage.
                bad_block.instructions = NULL;
                bad_block.instruction_count = 2u;
                break;
            case 3u:  // Unallocated value-root storage.
                bad_function.value_root_set_count = 2u;
                break;
            case 4u:  // Unallocated safepoint storage.
                bad_function.coroutine_safepoint_count = 2u;
                break;
            case 5u:  // Unallocated variant storage.
                type.variants = NULL;
                bad_input.types = &type;
                bad_input.type_count = 1u;
                break;
            case 6u:  // Failure after a preceding variant payload was copied.
                type.variants = variants;
                bad_input.types = &type;
                bad_input.type_count = 1u;
                break;
            case 7u:  // Unallocated interface slot storage.
                bad_input.interfaces = &interface;
                bad_input.interface_count = 1u;
                break;
            case 8u:  // Failure after signature parameters were copied.
                interface.slots = &signature;
                interface.slot_count = 1u;
                bad_input.interfaces = &interface;
                bad_input.interface_count = 1u;
                break;
        }
        program = NULL;
        CHECK(xr_core_ir_program_build(&bad_input, &program, diagnostic, sizeof(diagnostic)) !=
              XR_PROGRAM_BUILD_OK);
        CHECK(program == NULL);
        xr_core_ir_program_free(program);
    }
}

static XrProgramBuildStatus write_module_initialization_fixture(XrProgramArtifact *artifact,
                                                                char *diagnostic,
                                                                size_t diagnostic_size);

static XrProgramBuildStatus write_allocation_fixture(uint32_t fixture, XrProgramArtifact *artifact,
                                                     char *diagnostic, size_t diagnostic_size) {
    switch (fixture) {
        case 0u:
            return build_fixture(false, false, FIXTURE_VALID, "profile:checked", artifact,
                                 diagnostic, diagnostic_size);
        case 1u:
            return xr_program_text_fixture_write(artifact, diagnostic, diagnostic_size);
        case 2u:
            return xr_program_construct_fixture_write(XR_PROGRAM_CONSTRUCT_NESTED_TRANSFER,
                                                      artifact, diagnostic, diagnostic_size);
        case 3u:
            return xr_program_coroutine_trap_fixture_write(XR_PROGRAM_COROUTINE_TRAP_VALID,
                                                           artifact, diagnostic, diagnostic_size);
        case 4u:
            return write_module_initialization_fixture(artifact, diagnostic, diagnostic_size);
        default:
            return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
}

static void test_constructor_allocation_failures(uint32_t fixture) {
    char diagnostic[256];
    size_t builder_allocations = 0u;
    for (size_t fail_at = 1u; fail_at < 1024u; ++fail_at) {
        XrProgramArtifact artifact = {0};
        allocation_probe_begin(fail_at);
        XrProgramBuildStatus status =
            write_allocation_fixture(fixture, &artifact, diagnostic, sizeof(diagnostic));
        if (status != XR_PROGRAM_BUILD_OK) {
            CHECK(artifact.bytes == NULL);
            CHECK(artifact.size == 0u);
        }
        xr_program_artifact_free(&artifact);
        size_t attempts = allocation_probe.attempts;
        allocation_probe_end();
        if (status == XR_PROGRAM_BUILD_OK) {
            CHECK(attempts + 1u == fail_at);
            builder_allocations = attempts;
            break;
        }
        if (status != XR_PROGRAM_BUILD_OUT_OF_MEMORY)
            fprintf(stderr, "Program build fixture %u failure %zu returned %s: %s\n", fixture,
                    fail_at, xr_program_build_status_name(status), diagnostic);
        CHECK(status == XR_PROGRAM_BUILD_OUT_OF_MEMORY);
    }
    CHECK(builder_allocations != 0u);

    XrProgramArtifact artifact = {0};
    CHECK(write_allocation_fixture(fixture, &artifact, diagnostic, sizeof(diagnostic)) ==
          XR_PROGRAM_BUILD_OK);
    size_t verifier_allocations = 0u;
    for (size_t fail_at = 1u; fail_at < 1024u; ++fail_at) {
        XrValidatedProgram *validated = NULL;
        XrProgramDiagnostic rejection;
        allocation_probe_begin(fail_at);
        XrProgramVerifyStatus status =
            xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, &rejection);
        if (status != XR_PROGRAM_VERIFY_OK)
            CHECK(validated == NULL);
        xr_validated_program_free(validated);
        size_t attempts = allocation_probe.attempts;
        allocation_probe_end();
        if (status == XR_PROGRAM_VERIFY_OK) {
            CHECK(attempts + 1u == fail_at);
            verifier_allocations = attempts;
            break;
        }
        if (status != XR_PROGRAM_VERIFY_OUT_OF_MEMORY)
            fprintf(stderr, "Program verify fixture %u failure %zu returned %s: %s\n", fixture,
                    fail_at, xr_program_verify_status_name(status),
                    xr_program_diagnostic_kind_name(rejection.kind));
        CHECK(status == XR_PROGRAM_VERIFY_OUT_OF_MEMORY);
    }
    CHECK(verifier_allocations != 0u);
    xr_program_artifact_free(&artifact);
    printf("Program allocation failures: fixture=%u builder=%zu verifier=%zu\n", fixture,
           builder_allocations, verifier_allocations);
}

typedef struct ModuleInitializationFixture {
    XrCoreIrKey profile;
    uint16_t feature;
    XrCoreIrInstructionInput returns[4];
    XrCoreIrBlockInput blocks[4];
    XrCoreIrFunctionInput functions[4];
    XrCoreIrKey dependencies[4][2];
    XrCoreIrModuleInput modules[4];
    XrCoreIrProgramInput input;
} ModuleInitializationFixture;

static void init_module_initialization_fixture(ModuleInitializationFixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    const char *module_names[] = {"base", "left", "right", "main"};
    const char *function_names[] = {"base:init", "left:init", "right:init", "main:init"};
    for (uint32_t index = 0u; index < 4u; ++index) {
        fixture->returns[index].operation_id = XR_CORE_OP_CORE_RETURN;
        fixture->blocks[index] = (XrCoreIrBlockInput) {
            .key = key(function_names[index]),
            .instructions = &fixture->returns[index],
            .instruction_count = 1u,
        };
        fixture->functions[index] = (XrCoreIrFunctionInput) {
            .key = key(function_names[index]),
            .entry_block = fixture->blocks[index].key,
            .blocks = &fixture->blocks[index], .block_count = 1u,
            .flags = index == 3u ? XR_PROGRAM_FUNCTION_ENTRY : 0u,
        };
        fixture->modules[index] = (XrCoreIrModuleInput) {
            .key = key(module_names[index]),
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
    fixture->profile = key("module-initialization-profile");
    fixture->feature = XR_CORE_FEATURE_CORE_BASE;
    fixture->input = (XrCoreIrProgramInput) {
        .semantic_profile_fingerprint = fixture->profile.bytes,
        .required_features = &fixture->feature, .required_feature_count = 1u,
        .modules = fixture->modules, .module_count = 4u,
    };
}

static XrProgramBuildStatus write_module_initialization_fixture(XrProgramArtifact *artifact,
                                                                char *diagnostic,
                                                                size_t diagnostic_size) {
    ModuleInitializationFixture fixture;
    init_module_initialization_fixture(&fixture);
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&fixture.input, &program, diagnostic, diagnostic_size);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, diagnostic_size);
    xr_core_ir_program_free(program);
    return status;
}

static void test_module_initialization_hostile_wire(void) {
    ModuleInitializationFixture fixture;
    init_module_initialization_fixture(&fixture);
    char diagnostic[256] = {0};
    XrProgramArtifact artifact = {0};
    XrProgramBuildStatus status =
        write_module_initialization_fixture(&artifact, diagnostic, sizeof(diagnostic));
    CHECK(status == XR_PROGRAM_BUILD_OK);
    if (status != XR_PROGRAM_BUILD_OK)
        return;
    XrProgramView view = {0};
    XrProgramDecodeStatus decoded = xr_program_decode_structure(
        artifact.bytes, artifact.size, NULL, &view, diagnostic, sizeof(diagnostic));
    CHECK(decoded == XR_PROGRAM_DECODE_OK);
    if (decoded != XR_PROGRAM_DECODE_OK) {
        xr_program_artifact_free(&artifact);
        return;
    }
    const XrProgramSectionView *section = &view.sections[XR_PROGRAM_SECTION_SEMANTIC_METADATA - 1u];
    size_t rows[4] = {0};
    for (size_t module = 0u; module < XR_COUNTOF(rows); ++module) {
        unsigned matches = 0u;
        for (size_t offset = section->offset;
             offset + XR_CORE_IR_KEY_SIZE <= section->offset + section->size; ++offset) {
            if (memcmp(artifact.bytes + offset, fixture.modules[module].key.bytes,
                       XR_CORE_IR_KEY_SIZE) == 0) {
                rows[module] = offset;
                ++matches;
            }
        }
        CHECK(matches == 1u);
        if (matches != 1u) {
            xr_program_artifact_free(&artifact);
            return;
        }
    }
    uint8_t *mutated = xr_malloc(artifact.size);
    CHECK(mutated != NULL);
    if (!mutated) {
        xr_program_artifact_free(&artifact);
        return;
    }
    /* These fixture rows have one-byte IDs/counts: key, initializer, owned count,
     * owned function, dependency count, dependency IDs. */
    const size_t key_size = XR_CORE_IR_KEY_SIZE;
    for (unsigned mutation = 0u; mutation < 10u; ++mutation) {
        memcpy(mutated, artifact.bytes, artifact.size);
        switch (mutation) {
            case 0u:  // A zero module identity is structurally bounded but has no owner.
                memset(mutated + rows[1], 0, key_size);
                break;
            case 1u:  // Two initialization rows cannot claim the same module identity.
                memcpy(mutated + rows[1], mutated + rows[0], key_size);
                break;
            case 2u:  // An initializer must belong to the module that invokes it.
                mutated[rows[1] + key_size] = mutated[rows[0] + key_size];
                break;
            case 3u:  // A function cannot be owned by two modules.
                mutated[rows[1] + key_size + 2u] = mutated[rows[0] + key_size + 2u];
                break;
            case 4u:
                mutated[rows[1] + key_size] = 4u;
                break;
            case 5u:
                mutated[rows[1] + key_size + 2u] = 4u;
                break;
            case 6u:  // Empty function ownership is not a runtime module.
                mutated[rows[1] + key_size + 1u] = 0u;
                break;
            case 7u:  // Self-dependency.
                mutated[rows[1] + key_size + 4u] = 1u;
                break;
            case 8u:  // Forward dependency.
                mutated[rows[1] + key_size + 4u] = 3u;
                break;
            case 9u:  // Duplicate dependency in the diamond's root.
                mutated[rows[3] + key_size + 5u] = mutated[rows[3] + key_size + 4u];
                break;
        }
        expect_decode_status(mutated, artifact.size, NULL,
                             mutation < 4u ? XR_PROGRAM_DECODE_OK : XR_PROGRAM_DECODE_NONCANONICAL);
        XrValidatedProgram *validated = NULL;
        XrProgramDiagnostic rejection = {0};
        CHECK(xr_program_validate(mutated, artifact.size, NULL, &validated, &rejection) !=
              XR_PROGRAM_VERIFY_OK);
        CHECK(validated == NULL);
        if (mutation < 4u)
            CHECK(rejection.kind == XR_PROGRAM_DIAGNOSTIC_FUNCTION);
        xr_validated_program_free(validated);
    }
    xr_free(mutated);
    xr_program_artifact_free(&artifact);
}

static void test_module_initialization_construction(void) {
    ModuleInitializationFixture fixture;
    init_module_initialization_fixture(&fixture);
    char diagnostic[256] = {0};
    XrCoreIrProgram *program = NULL;
    CHECK(xr_core_ir_program_build(&fixture.input, &program, diagnostic, sizeof(diagnostic)) ==
          XR_PROGRAM_BUILD_OK);
    if (!program)
        return;
    fixture.dependencies[3][0] = key("mutated-producer-storage");
    for (uint32_t index = 0u; index < 4u; ++index) {
        const XrCoreIrModule *module = &program->modules[index];
        uint32_t order = module->initialization_order;
        CHECK(order < 4u && xr_core_ir_key_equal(module->key, fixture.modules[order].key));
        CHECK(xr_core_ir_key_equal(module->initializer, fixture.functions[order].key));
        if (order == 3u) {
            CHECK(module->dependency_count == 2u);
            for (uint32_t dependency = 0u; dependency < module->dependency_count; ++dependency)
                CHECK(xr_core_ir_key_equal(module->dependencies[dependency], fixture.modules[1].key) ||
                      xr_core_ir_key_equal(module->dependencies[dependency], fixture.modules[2].key));
        }
    }
    XrProgramArtifact artifact = {0};
    CHECK(xr_program_write(program, &artifact, diagnostic, sizeof(diagnostic)) ==
          XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *validated = NULL;
    XrProgramDiagnostic rejection = {0};
    CHECK(xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, &rejection) ==
          XR_PROGRAM_VERIFY_OK);
    if (validated) {
        CHECK(validated->module_count == 4u);
        for (uint32_t index = 0u; index < validated->module_count; ++index) {
            const XrValidatedModule *module = &validated->modules[index];
            CHECK(xr_core_ir_key_equal(module->key, fixture.modules[index].key));
            CHECK(module->initializer < validated->function_count);
            CHECK(validated->functions[module->initializer].module_index_plus_one == index + 1u);
            CHECK(module->dependency_count == fixture.modules[index].dependency_count);
            for (uint32_t dependency = 0u; dependency < module->dependency_count; ++dependency)
                CHECK(module->dependencies[dependency] == (index == 3u ? dependency + 1u : 0u));
        }
    }
    xr_validated_program_free(validated);
    xr_program_artifact_free(&artifact);
    xr_core_ir_program_free(program);
    for (uint32_t mutation = 0u; mutation < 7u; ++mutation) {
        init_module_initialization_fixture(&fixture);
        if (mutation == 0u)
            fixture.modules[1].initializer = fixture.functions[0].key;
        else if (mutation == 1u)
            fixture.dependencies[1][0] = key("missing-module");
        else if (mutation == 2u)
            fixture.dependencies[1][0] = fixture.modules[1].key;
        else if (mutation == 3u)
            fixture.dependencies[3][1] = fixture.dependencies[3][0];
        else if (mutation == 4u)
            fixture.dependencies[1][0] = fixture.modules[3].key;
        else if (mutation == 5u)
            memset(&fixture.modules[1].initializer, 0, sizeof(XrCoreIrKey));
        else
            fixture.modules[1].dependencies = NULL;
        program = NULL;
        CHECK(xr_core_ir_program_build(&fixture.input, &program, diagnostic, sizeof(diagnostic)) ==
              XR_PROGRAM_BUILD_INVALID_INPUT);
        CHECK(program == NULL);
        xr_core_ir_program_free(program);
    }
    init_module_initialization_fixture(&fixture);
    size_t allocations = 0u;
    for (size_t fail_at = 1u; fail_at < 128u; ++fail_at) {
        allocation_probe_begin(fail_at);
        XrProgramBuildStatus status =
            xr_core_ir_program_build(&fixture.input, &program, diagnostic, sizeof(diagnostic));
        if (status != XR_PROGRAM_BUILD_OK)
            CHECK(status == XR_PROGRAM_BUILD_OUT_OF_MEMORY && program == NULL);
        xr_core_ir_program_free(program);
        allocations = allocation_probe.attempts;
        allocation_probe_end();
        if (status == XR_PROGRAM_BUILD_OK) {
            CHECK(allocations + 1u == fail_at);
            break;
        }
    }
    CHECK(allocations != 0u && allocations < 127u);
    printf("Module initialization constructor allocation points: %zu\n", allocations);
}

int main(void) {
    test_module_initialization_construction();
    test_module_initialization_hostile_wire();
    for (uint32_t fixture = 0u; fixture < 5u; ++fixture)
        test_constructor_allocation_failures(fixture);
    test_partial_core_ir_construction();
    test_determinism_roundtrip_and_identity();
    test_hostile_structure_and_budget();
    test_semantic_profile_and_invalid_core_ir();
    test_provider_semantics_are_program_identity();
    if (failures != 0) {
        fprintf(stderr, "XrProgram tests failed: %d\n", failures);
        return 1;
    }
    puts("XrProgram canonical writer and structural decoder tests passed");
    return 0;
}
