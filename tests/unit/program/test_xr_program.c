/*
 * Task 296: canonical CoreIR -> XrProgram writer and bounded structural decode.
 */

#include "xr_program_module_fixture.h"
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
#include "xr_program_coroutine_outcome_fixture.h"
#include "xr_program_text_fixture.h"
#include "xr_program_output_trap_fixture.h"
#include "xr_program_assert_fixture.h"
#include "xr_program_callable_fixture.h"
#include "xr_program_array_fixture.h"

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
    /* Preserve all old identities from their fixed legacy wire. The new identity
     * is independently derived by appending the f64 builtin row and updating offsets. */
    CHECK(first.size == 501u);
    uint8_t previous_vector[497] = {
        0x58, 0x52, 0x50, 0x52, 0x4f, 0x47, 0x0d, 0x0a, 0x03, 0x00, 0x06, 0x00, 0x01, 0x32, 0x36, 0xb7,
        0x16, 0xd3, 0xb8, 0xca, 0x42, 0xa7, 0x24, 0x09, 0x5a, 0x7a, 0x6c, 0x10, 0x11, 0x62, 0x20, 0x2f,
        0x83, 0x37, 0xb2, 0x9b, 0x7a, 0x1e, 0xcd, 0xb2, 0x59, 0xc8, 0x45, 0x81, 0x45, 0x38, 0x93, 0xb8,
        0xb1, 0x11, 0xc5, 0xbd, 0xb0, 0xa0, 0xf1, 0x41, 0x3f, 0x48, 0xab, 0x5f, 0x06, 0x22, 0x1d, 0xf2,
        0x7d, 0x57, 0x7c, 0xe8, 0x66, 0x03, 0x3b, 0xfe, 0xaf, 0x69, 0xd7, 0xd9, 0x19, 0x01, 0x01, 0x07,
        0x01, 0x00, 0x49, 0x02, 0x49, 0x09, 0x03, 0x52, 0x19, 0x04, 0x6b, 0x67, 0x05, 0xd2, 0x01, 0xa7,
        0x01, 0x06, 0xf9, 0x02, 0x01, 0x07, 0xfa, 0x02, 0x0e, 0x12, 0x00, 0x00, 0x00, 0x02, 0x01, 0x01,
        0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x03, 0x03, 0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0x05, 0x05,
        0x01, 0x02, 0x06, 0x06, 0x00, 0x00, 0x07, 0x07, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00, 0x09, 0x09,
        0x00, 0x00, 0x0a, 0x0a, 0x00, 0x00, 0x0c, 0x0c, 0x01, 0x01, 0x0d, 0x0d, 0x00, 0x00, 0x0e, 0x0e,
        0x00, 0x00, 0x0f, 0x0f, 0x00, 0x00, 0x10, 0x10, 0x00, 0x00, 0x11, 0x11, 0x00, 0x00, 0x12, 0x12,
        0x00, 0x00, 0x02, 0x00, 0x02, 0x01, 0x04, 0x01, 0x02, 0x01, 0x50, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x45, 0x20, 0x02, 0x00, 0x00, 0x00, 0x01, 0x06, 0x00, 0x01, 0x00,
        0x00, 0x01, 0x01, 0x01, 0x02, 0x00, 0x01, 0x00, 0x00, 0x07, 0x01, 0x01, 0x02, 0x00, 0x00, 0x00,
        0x04, 0x01, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x10, 0x03, 0x02, 0x00,
        0x00, 0x02, 0x00, 0x01, 0x02, 0x00, 0x00, 0x88, 0x01, 0x04, 0x02, 0x00, 0x00, 0x01, 0x02, 0x0a,
        0x01, 0x00, 0x00, 0x88, 0x01, 0x05, 0x02, 0x00, 0x00, 0x01, 0x03, 0x0a, 0x01, 0x01, 0x00, 0x88,
        0x01, 0x06, 0x02, 0x00, 0x00, 0x01, 0x04, 0x0a, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x05, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x02, 0x24, 0x01, 0x02, 0x00, 0x00, 0x00, 0x05,
        0x00, 0x00, 0x23, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x25, 0xd8, 0x84, 0x50,
        0x2b, 0x30, 0x9c, 0x2c, 0x68, 0x34, 0x8d, 0x28, 0xdf, 0x75, 0x55, 0x0c, 0x01, 0x34, 0xe6, 0xa0,
        0x90, 0xba, 0x1e, 0x0d, 0x32, 0xd3, 0xa5, 0xcd, 0x82, 0x9f, 0x45, 0x41, 0x30, 0x1b, 0x01, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x01, 0x01, 0x03, 0x00, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x03, 0x03, 0x01, 0x7b, 0xd3, 0x57, 0x22, 0x64, 0x2c, 0xbb,
        0x62, 0x53, 0x2c, 0x6c, 0xc8, 0x21, 0xff, 0x2e, 0xbd, 0x02, 0x34, 0xe6, 0xa0, 0x90, 0xba, 0x1e,
        0x0d, 0x32, 0xd3, 0xa5, 0xcd, 0x82, 0x9f, 0x45, 0x41, 0x30, 0x1b, 0x01, 0x00, 0x00, 0x00, 0x08,
        0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x01, 0x01, 0x03, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x03, 0x03, 0x01, 0x4d, 0x05, 0xd6, 0xd0, 0xb6, 0x5f, 0xd0, 0x6a, 0x2a, 0xd8,
        0x33, 0xab, 0xcb, 0xa1, 0xd7, 0xd0, 0x1b, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x1f,
        0x00, 0x00, 0x00, 0x01, 0x01, 0x03, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x03,
        0x03, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00,
    };
    CHECK(first.bytes[10] == 9u && first.bytes[11] == 0u);
    previous_vector[10] = 4u;
    static const uint8_t previous_core_spec[32] = {
        0x0a, 0xcc, 0x9c, 0x76, 0x49, 0x58, 0xb4, 0xd0,
        0x0e, 0xeb, 0x05, 0x24, 0xe4, 0xc5, 0xa9, 0xa7,
        0x6e, 0xc5, 0x20, 0x94, 0xa3, 0xa6, 0xe8, 0x79,
        0x1d, 0xc8, 0x45, 0xb2, 0x89, 0x50, 0x3d, 0x38,
    };
    CHECK(first.bytes[12] == XR_CORE_SPEC_EPOCH);
    memcpy(previous_vector + 13u, previous_core_spec, sizeof(previous_core_spec));
    XrProgramId previous_id;
    xr_program_compute_id(previous_vector, sizeof(previous_vector), &previous_id);
    char previous_hex[65];
    xr_program_id_hex(previous_id, previous_hex);
    CHECK(strcmp(previous_hex, "7afaa251d147b3e0ddc127d441f2b9ac11333e44558a757434b755f80952b3ad") == 0);
    previous_vector[10] = 5u;
    xr_program_compute_id(previous_vector, sizeof(previous_vector), &previous_id);
    xr_program_id_hex(previous_id, previous_hex);
    CHECK(strcmp(previous_hex, "301cfcc1ada4b8382b88184c3ea725368b320212b3edfd74a8dc6048c6294d6d") == 0);
    static const uint8_t sequence_core_spec[32] = {
        0x60, 0x1e, 0x57, 0xfe, 0x0e, 0xf5, 0x7a, 0xfe, 0xfc, 0x3d, 0x0b, 0xd6, 0x66, 0xd4, 0xc8, 0x71, 0x0b, 0x6f, 0xcb, 0xae, 0xff, 0x9a, 0x6d, 0xd4, 0xba, 0xc6, 0x2c, 0xe3, 0x98, 0x6c, 0x9d, 0x20
    };
    memcpy(previous_vector + 13u, sequence_core_spec, sizeof(sequence_core_spec));
    xr_program_compute_id(previous_vector, sizeof(previous_vector), &previous_id);
    xr_program_id_hex(previous_id, previous_hex);
    CHECK(strcmp(previous_hex, "7ad5d4ff936713308fdf21da7155c686b9cfb4fe5f007dd518b002a96f7b2790") == 0);
    previous_vector[10] = 6u;
    xr_program_compute_id(previous_vector, sizeof(previous_vector), &previous_id);
    xr_program_id_hex(previous_id, previous_hex);
    CHECK(strcmp(previous_hex, "b927a4891131472a526c33e3e2d90fe1e393bab5bb8bf7ee18a9d86e24582c15") == 0);
    static const uint8_t atomic_core_spec[32] = {
        0x7c, 0x6e, 0xfe, 0x1e, 0x2c, 0xdd, 0x12, 0xc1, 0xc6, 0x36, 0x2d, 0x52, 0x3c, 0xf1, 0xc9, 0x08, 0x63, 0x58, 0x1a, 0xe0, 0x6b, 0x3f, 0x0b, 0x1b, 0xab, 0xb3, 0x75, 0x6c, 0x41, 0x1d, 0xae, 0x2f
    };
    memcpy(previous_vector + 13u, atomic_core_spec, sizeof(atomic_core_spec));
    xr_program_compute_id(previous_vector, sizeof(previous_vector), &previous_id);
    xr_program_id_hex(previous_id, previous_hex);
    CHECK(strcmp(previous_hex, "1d74adb599932d56e448d5f8ccc4c357b903a0499cfe242bca181b0f7f475942") == 0);
    static const uint8_t cas_core_spec[32] = {
        0xfe, 0xbb, 0xc0, 0xcf, 0x89, 0xac, 0xde, 0x9b, 0x05, 0xa0, 0x05, 0xc0, 0x5a, 0x1f, 0x4a, 0xa4, 0x7a, 0x3f, 0x19, 0x63, 0x13, 0x76, 0x40, 0x99, 0xe6, 0x9f, 0x17, 0x09, 0xf4, 0x10, 0x39, 0xb4
    };
    memcpy(previous_vector + 13u, cas_core_spec, sizeof(cas_core_spec));
    xr_program_compute_id(previous_vector, sizeof(previous_vector), &previous_id);
    xr_program_id_hex(previous_id, previous_hex);
    CHECK(strcmp(previous_hex, "735d1517863c53bedc836812b58b6a96079fbadd07602ec882dd1c4f07666ab8") == 0);
    static const uint8_t update_core_spec[32] = {0x32,0x36,0xb7,0x16,0xd3,0xb8,0xca,0x42,0xa7,0x24,0x09,0x5a,0x7a,0x6c,0x10,0x11,0x62,0x20,0x2f,0x83,0x37,0xb2,0x9b,0x7a,0x1e,0xcd,0xb2,0x59,0xc8,0x45,0x81,0x45};
    memcpy(previous_vector + 13u, update_core_spec, sizeof(update_core_spec));
    xr_program_compute_id(previous_vector, sizeof(previous_vector), &previous_id);
    xr_program_id_hex(previous_id, previous_hex);
    CHECK(strcmp(previous_hex, "428c18b3995c3443992cc374d7af1e5d57b0022d236b448af8b2ecdbf616c488") == 0);
    uint8_t previous_f64_vector[501];
    memcpy(previous_f64_vector, first.bytes, sizeof(previous_f64_vector));
    previous_f64_vector[10] = 7u;
    static const uint8_t previous_f64_core[32] = {0x9b,0xc4,0xf9,0x33,0xe0,0x94,0xa9,0xd6,0x78,0x33,0x4d,0x12,0x42,0x24,0x8a,0x66,0xfd,0x92,0x8a,0x7e,0xce,0xf5,0xf9,0x33,0x9b,0x55,0x03,0xcc,0xd4,0x65,0x1c,0xf9};
    memcpy(previous_f64_vector + 13u, previous_f64_core, sizeof(previous_f64_core));
    xr_program_compute_id(previous_f64_vector, sizeof(previous_f64_vector), &previous_id);
    xr_program_id_hex(previous_id, previous_hex);
    CHECK(strcmp(previous_hex, "e2fc903e3ea6b895a0e59a53cbc48aff6ee4529f9b5c90d4240a4ae593250c3e") == 0);
    static const uint8_t scalar_core_spec[32] = {0xae,0x5b,0x7d,0x6d,0xa8,0x68,0x0a,0x79,0xec,0x24,0x0a,0x1a,0xe7,0xa4,0xa9,0x83,0x9a,0x11,0x37,0x6b,0x5e,0x9c,0x85,0xdd,0x33,0xf9,0x65,0xb4,0x6f,0xdb,0x96,0x6f};
    memcpy(previous_f64_vector + 13u, scalar_core_spec, sizeof(scalar_core_spec));
    xr_program_compute_id(previous_f64_vector, sizeof(previous_f64_vector), &previous_id);
    xr_program_id_hex(previous_id, previous_hex);
    CHECK(strcmp(previous_hex, "f56d74fae27c57d60dff03bf70cf0330dba85659a7c091633eae80eed66f1ccb") == 0);
    static const uint8_t bitcast_core_spec[32] = {0x78,0x53,0xa4,0x50,0xcd,0x62,0x66,0x7a,0xb0,0xb5,0x8e,0x41,0x5a,0xcd,0x50,0xcb,0xdd,0x62,0x66,0xb0,0x00,0x1f,0xa1,0x88,0x5d,0x02,0x5b,0xe0,0xab,0x5b,0x75,0x4b};
    memcpy(previous_f64_vector + 13u, bitcast_core_spec, sizeof(bitcast_core_spec));
    xr_program_compute_id(previous_f64_vector, sizeof(previous_f64_vector), &previous_id);
    xr_program_id_hex(previous_id, previous_hex);
    CHECK(strcmp(previous_hex, "9416a7926c2449c9f688f9795297f86ac52e45444c114b1719dcc64323995aa1") == 0);
    static const uint8_t output_core_spec[32] = {0x75,0x2e,0xa7,0xaa,0x3e,0x13,0x42,0xbf,0x7b,0x10,0xd0,0x24,0xda,0xe4,0xb5,0xaa,0xc6,0xd1,0xec,0x59,0x44,0x80,0xf4,0x34,0xd3,0x00,0xfd,0x97,0x7a,0x75,0xda,0xf9};
    memcpy(previous_f64_vector + 13u, output_core_spec, sizeof(output_core_spec));
    xr_program_compute_id(previous_f64_vector, sizeof(previous_f64_vector), &previous_id);
    xr_program_id_hex(previous_id, previous_hex);
    CHECK(strcmp(previous_hex, "1e6c03fd8b42b19a8868464dd8478f54ccd14909033927f1756970d47b18224f") == 0);
    static const uint8_t record_type_core[32] = {0x66,0x0c,0x96,0x64,0x04,0xfd,0x71,0x34,0x88,0x45,0xd3,0xc0,0xf5,0xb6,0x27,0x11,0x60,0x26,0x88,0xe0,0xba,0xbe,0xd0,0xf4,0x13,0x1e,0x08,0x2c,0x46,0x09,0x91,0xf8};
    memcpy(previous_f64_vector + 13u, record_type_core, 32u);
    xr_program_compute_id(previous_f64_vector, sizeof(previous_f64_vector), &previous_id);
    xr_program_id_hex(previous_id, previous_hex);
    CHECK(strcmp(previous_hex, "8f8aa69d4b9ed9ec9f947b5f567b58cccd58c26c37cf6bad8531c6016a4b6b91") == 0);
    previous_f64_vector[10] = 8u;
    xr_program_compute_id(previous_f64_vector, sizeof(previous_f64_vector), &previous_id);
    xr_program_id_hex(previous_id, previous_hex);
    CHECK(strcmp(previous_hex, "e5ed1fd020f8792f2a8fa3c6b4aaa930f5339911683d30f41ee499cd4b388906") == 0);
    /* The fixed pre-message wire stays independent of the current encoder. */
    uint8_t message_vector[501] = {
        0x58, 0x52, 0x50, 0x52, 0x4f, 0x47, 0x0d, 0x0a, 0x03, 0x00, 0x09, 0x00, 0x01, 0x80, 0xe7, 0xef,
        0xb9, 0x72, 0xdd, 0xfe, 0xde, 0x0f, 0xf4, 0x05, 0xc0, 0xce, 0x17, 0xf4, 0x26, 0x7a, 0x6b, 0x81,
        0x94, 0x6c, 0xac, 0xde, 0xad, 0x75, 0x22, 0x04, 0x25, 0xe1, 0xeb, 0x1c, 0x89, 0x38, 0x93, 0xb8,
        0xb1, 0x11, 0xc5, 0xbd, 0xb0, 0xa0, 0xf1, 0x41, 0x3f, 0x48, 0xab, 0x5f, 0x06, 0x22, 0x1d, 0xf2,
        0x7d, 0x57, 0x7c, 0xe8, 0x66, 0x03, 0x3b, 0xfe, 0xaf, 0x69, 0xd7, 0xd9, 0x19, 0x01, 0x01, 0x07,
        0x01, 0x00, 0x4d, 0x02, 0x4d, 0x09, 0x03, 0x56, 0x19, 0x04, 0x6f, 0x67, 0x05, 0xd6, 0x01, 0xa7,
        0x01, 0x06, 0xfd, 0x02, 0x01, 0x07, 0xfe, 0x02, 0x0e, 0x13, 0x00, 0x00, 0x00, 0x02, 0x01, 0x01,
        0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x03, 0x03, 0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0x05, 0x05,
        0x01, 0x02, 0x06, 0x06, 0x00, 0x00, 0x07, 0x07, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00, 0x09, 0x09,
        0x00, 0x00, 0x0a, 0x0a, 0x00, 0x00, 0x0c, 0x0c, 0x01, 0x01, 0x0d, 0x0d, 0x00, 0x00, 0x0e, 0x0e,
        0x00, 0x00, 0x0f, 0x0f, 0x00, 0x00, 0x10, 0x10, 0x00, 0x00, 0x11, 0x11, 0x00, 0x00, 0x12, 0x12,
        0x00, 0x00, 0x13, 0x13, 0x00, 0x00, 0x02, 0x00, 0x02, 0x01, 0x04, 0x01, 0x02, 0x01, 0x50, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x45, 0x20, 0x02, 0x00, 0x00, 0x00, 0x01,
        0x06, 0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x01, 0x02, 0x00, 0x01, 0x00, 0x00, 0x07, 0x01, 0x01,
        0x02, 0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
        0x10, 0x03, 0x02, 0x00, 0x00, 0x02, 0x00, 0x01, 0x02, 0x00, 0x00, 0x88, 0x01, 0x04, 0x02, 0x00,
        0x00, 0x01, 0x02, 0x0a, 0x01, 0x00, 0x00, 0x88, 0x01, 0x05, 0x02, 0x00, 0x00, 0x01, 0x03, 0x0a,
        0x01, 0x01, 0x00, 0x88, 0x01, 0x06, 0x02, 0x00, 0x00, 0x01, 0x04, 0x0a, 0x00, 0x00, 0x00, 0x23,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x05, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x02, 0x24, 0x01, 0x02,
        0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x23, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
        0x25, 0xd8, 0x84, 0x50, 0x2b, 0x30, 0x9c, 0x2c, 0x68, 0x34, 0x8d, 0x28, 0xdf, 0x75, 0x55, 0x0c,
        0x01, 0x34, 0xe6, 0xa0, 0x90, 0xba, 0x1e, 0x0d, 0x32, 0xd3, 0xa5, 0xcd, 0x82, 0x9f, 0x45, 0x41,
        0x30, 0x1b, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x01, 0x01,
        0x03, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x03, 0x03, 0x01, 0x7b, 0xd3, 0x57,
        0x22, 0x64, 0x2c, 0xbb, 0x62, 0x53, 0x2c, 0x6c, 0xc8, 0x21, 0xff, 0x2e, 0xbd, 0x02, 0x34, 0xe6,
        0xa0, 0x90, 0xba, 0x1e, 0x0d, 0x32, 0xd3, 0xa5, 0xcd, 0x82, 0x9f, 0x45, 0x41, 0x30, 0x1b, 0x01,
        0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x01, 0x01, 0x03, 0x00, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x03, 0x03, 0x01, 0x4d, 0x05, 0xd6, 0xd0, 0xb6, 0x5f,
        0xd0, 0x6a, 0x2a, 0xd8, 0x33, 0xab, 0xcb, 0xa1, 0xd7, 0xd0, 0x1b, 0x01, 0x00, 0x00, 0x00, 0x08,
        0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x01, 0x01, 0x03, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x03, 0x03, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00,
    };
    memcpy(previous_f64_vector + 13u, message_vector + 13u, 32u);
    xr_program_compute_id(previous_f64_vector, sizeof(previous_f64_vector), &previous_id);
    xr_program_id_hex(previous_id, previous_hex);
    CHECK(strcmp(previous_hex, "1bb4200bb526389bbc2ede0160828177556b4ec7348616ce519e0c5198b30c4c") == 0);
    xr_program_compute_id(message_vector, sizeof(message_vector), &previous_id);
    xr_program_id_hex(previous_id, previous_hex);
    CHECK(strcmp(previous_hex, "a0d1f933ab3323f3d95634a084180c021c6b2d6ba997534d254820659452f9f0") == 0);
    static const uint8_t message_core_spec[32] = {0xa1,0x50,0xee,0xd8,0xc0,0x55,0xfd,0xc2,0xc8,0x39,0x63,0x8d,0x82,0xde,0xb9,0xf2,0x55,0x3d,0x17,0xb0,0xca,0xc5,0x0c,0x21,0x15,0x1c,0xff,0xbd,0xb0,0x50,0x12,0xff};
    memcpy(message_vector + 13u, message_core_spec, sizeof(message_core_spec));
    CHECK(memcmp(first.bytes, message_vector, sizeof(message_vector)) == 0);
    CHECK(strcmp(id_hex, "456a1708865174e260191ed7f1654a4efa8f98cba831b31461f858f2f995b354") == 0);
    printf("Canonical ProgramId: %s (%zu bytes)\n", id_hex, first.size);


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
    /* Every added builtin row has exact identity, kind and trivial ownership.
     * Changing any field must not reinterpret a scalar as a dynamic type. */
    for (size_t row = 13u; row < 18u; ++row) {
        size_t row_offset =
            (size_t) valid_view.sections[XR_PROGRAM_SECTION_TYPES - 1u].offset + 1u + row * 4u;
        for (size_t field = 0u; field < 4u; ++field) {
            memcpy(mutated, artifact.bytes, artifact.size);
            mutated[row_offset + field] ^= 1u;
            expect_decode_status(mutated, artifact.size, NULL, XR_PROGRAM_DECODE_NONCANONICAL);
        }
    }
    memcpy(mutated, artifact.bytes, artifact.size);
    mutated[XR_PROGRAM_MAGIC_SIZE + 2u] = 2u;
    expect_decode_status(mutated, artifact.size, NULL, XR_PROGRAM_DECODE_UNSUPPORTED_VERSION);
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

    memcpy(mutated, artifact.bytes, artifact.size);
    mutated[XR_PROGRAM_MAGIC_SIZE + 2u] = 1u;
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
static XrProgramBuildStatus write_module_slots_fixture(XrProgramArtifact *artifact,
                                                       char *diagnostic, size_t diagnostic_size);

#include "xr_program_display_name_checks.inc.c"

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
        case 5u:
            return write_module_slots_fixture(artifact, diagnostic, diagnostic_size);
        case 6u:
        case 7u:
            return xr_program_module_initializer_fixture_write(fixture == 7u, artifact, diagnostic,
                                                               diagnostic_size);
        case 8u:
            return xr_program_output_trap_fixture_write(XR_OUTPUT_TRAP_VALID, artifact, diagnostic,
                                                        diagnostic_size);
        case 9u:
        case 10u:
            return xr_program_panic_point_fixture_write(
                fixture == 9u ? XR_CORE_OP_CORE_ASSERT_CONDITION : XR_CORE_OP_CORE_INTEGER_DIVMOD,
                XR_ASSERT_FIXTURE_CHAINED_CLEANUP, artifact, diagnostic, diagnostic_size);
        case 11u:
            return xr_program_callable_fixture_write_mutated(XR_CALLABLE_FIXTURE_DIRECT_PANIC,
                                                             artifact, diagnostic, diagnostic_size);
        case 12u:
        case 13u:
        case 14u:
        case 15u:
            return xr_program_coroutine_outcome_fixture_write(
                (fixture & 1u) != 0u, true,
                fixture >= 14u ? XR_CORO_OUTCOME_HANDLED : XR_CORO_OUTCOME_VALID, artifact,
                diagnostic, diagnostic_size);
        case 16u:
            return write_named_type_fixture(artifact, diagnostic, diagnostic_size);
        case 17u:
            return xr_program_array_place_fixture_write(0u, artifact);
        case 18u:
            return xr_program_array_loan_fixture_write(0u, artifact);
        case 19u:
            return xr_program_array_loan_fixture_write(2u, artifact);
        case 20u:
            return xr_program_array_loan_fixture_write(3u, artifact);
        case 21u:
            return xr_program_array_loan_fixture_write(5u, artifact);
        case 22u:
            return xr_program_array_loan_fixture_write(8u, artifact);
        case 23u:
            return xr_program_array_loan_fixture_write(10u, artifact);
        case 24u:
            return xr_program_module_array_borrow_fixture_write(0u, artifact);
        case 25u:
            return xr_program_module_array_borrow_fixture_write(3u, artifact);
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

static XrProgramBuildStatus write_module_initialization_fixture(XrProgramArtifact *artifact,
                                                                char *diagnostic,
                                                                size_t diagnostic_size) {
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&fixture.input, &program, diagnostic, diagnostic_size);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, diagnostic_size);
    xr_core_ir_program_free(program);
    return status;
}

static void test_module_initialization_hostile_wire(void) {
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
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
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
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
                CHECK(
                    xr_core_ir_key_equal(module->dependencies[dependency],
                                         fixture.modules[1].key) ||
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
        xr_program_module_fixture_init(&fixture);
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
    xr_program_module_fixture_init(&fixture);
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

#include "xr_program_module_slot_checks.inc.c"
#include "xr_program_array_type_checks.inc.c"
#include "xr_program_atomic_type_checks.inc.c"
#include "xr_program_record_type_checks.inc.c"
#include "xr_program_resource_type_checks.inc.c"

static void test_retained_function_identity_publication(void) {
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    XrCoreIrProgram *program = NULL;
    char diagnostic[256] = {0};
    CHECK(xr_core_ir_program_build(&fixture.input, &program, diagnostic, sizeof(diagnostic)) ==
          XR_PROGRAM_BUILD_OK);
    if (!program)
        return;
    XrCoreIrKey keys[4];
    uint32_t ids[4] = {0u};
    for (uint32_t index = 0u; index < 4u; ++index)
        keys[index] = fixture.functions[3u - index].key;
    CHECK(xr_program_function_ids_from_keys(program, keys, 4u, ids) == XR_PROGRAM_BUILD_OK);
    for (uint32_t index = 0u; index < 4u; ++index) {
        uint32_t expected = 0u;
        for (uint32_t other = 0u; other < 4u; ++other)
            expected += memcmp(keys[other].bytes, keys[index].bytes, XR_CORE_IR_KEY_SIZE) < 0;
        CHECK(ids[index] == expected);
    }
    XrCoreIrKey saved = keys[3];
    keys[3] = key("absent-retained-function");
    CHECK(xr_program_function_ids_from_keys(program, keys, 4u, ids) ==
          XR_PROGRAM_BUILD_INVALID_INPUT);
    for (uint32_t index = 0u; index < 4u; ++index)
        CHECK(ids[index] == UINT32_MAX);
    keys[3] = saved;
    allocation_probe_begin(1u);
    CHECK(xr_program_function_ids_from_keys(program, keys, 4u, ids) ==
          XR_PROGRAM_BUILD_OUT_OF_MEMORY);
    CHECK(allocation_probe.attempts == 1u);
    allocation_probe_end();
    for (uint32_t index = 0u; index < 4u; ++index)
        CHECK(ids[index] == UINT32_MAX);
    allocation_probe_begin(2u);
    CHECK(xr_program_function_ids_from_keys(program, keys, 4u, ids) == XR_PROGRAM_BUILD_OK);
    CHECK(allocation_probe.attempts == 1u);
    allocation_probe_end();
    xr_core_ir_program_free(program);
}

static void test_array_element_place_structure(void) {
    XrProgramArtifact artifact = {0};
    CHECK(xr_program_array_place_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    if (!artifact.bytes) return;
    expect_decode_status(artifact.bytes, artifact.size, NULL, XR_PROGRAM_DECODE_OK);
    for (size_t size = 0u; size < artifact.size; ++size) {
        XrProgramView view;
        CHECK(xr_program_decode_structure(artifact.bytes, size, NULL, &view, NULL, 0u) !=
              XR_PROGRAM_DECODE_OK);
    }
    XrProgramDecodeBudget budget = xr_program_decode_default_budget();
    budget.max_records = 1u;
    expect_decode_status(artifact.bytes, artifact.size, &budget, XR_PROGRAM_DECODE_RESOURCE_LIMIT);
    xr_program_artifact_free(&artifact);
    size_t allocations = 0u;
    for (size_t fail_at = 1u; fail_at < 1024u; ++fail_at) {
        allocation_probe_begin(fail_at);
        XrProgramBuildStatus status = xr_program_array_place_fixture_write(0u, &artifact);
        bool complete = status == XR_PROGRAM_BUILD_OK;
        if (!complete) {
            CHECK(status == XR_PROGRAM_BUILD_OUT_OF_MEMORY);
            CHECK(artifact.bytes == NULL && artifact.size == 0u);
        }
        allocations = allocation_probe.attempts;
        xr_program_artifact_free(&artifact);
        allocation_probe_end();
        if (complete) break;
    }
    CHECK(allocations > 0u && allocations < 1023u);
    printf("Array element-place writer allocation points: %zu\n", allocations);
}

static const uint64_t f64_constant_bits[] = {
    UINT64_C(0), UINT64_C(1), UINT64_C(0x3ff0000000000000),
    UINT64_C(0x7fefffffffffffff), UINT64_C(0x7ff0000000000000),
    UINT64_C(0x7ff0000000000001), UINT64_C(0x7ff8000000000001),
    UINT64_C(0x8000000000000000), UINT64_C(0xfff0000000000000),
    UINT64_C(0xfff8000000000001),
};

static XrProgramBuildStatus write_f64_constants(bool reverse, bool wrong_type,
                                                XrProgramArtifact *artifact) {
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    XrCoreIrConstantInput constants[XR_COUNTOF(f64_constant_bits)];
    for (uint32_t index = 0u; index < XR_COUNTOF(constants); ++index) {
        uint32_t selected = reverse ? (uint32_t) XR_COUNTOF(constants) - index - 1u : index;
        constants[index] = (XrCoreIrConstantInput) {
            .key = xr_core_ir_key(&selected, sizeof(selected)),
            .type_id = wrong_type ? XR_CORE_TYPE_I64 : XR_CORE_TYPE_F64,
            .kind = XR_CORE_IR_CONSTANT_F64,
            .value.f64_bits = f64_constant_bits[selected],
        };
    }
    fixture.modules[0].constants = constants;
    fixture.modules[0].constant_count = XR_COUNTOF(constants);
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status = xr_core_ir_program_build(&fixture.input, &program, NULL, 0u);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, NULL, 0u);
    xr_core_ir_program_free(program);
    return status;
}

static void test_f64_constant_bits_and_wire(void) {
    XrProgramArtifact first = {0}, reversed = {0}, invalid = {0};
    CHECK(write_f64_constants(false, false, &first) == XR_PROGRAM_BUILD_OK);
    CHECK(write_f64_constants(true, false, &reversed) == XR_PROGRAM_BUILD_OK);
    CHECK(write_f64_constants(false, true, &invalid) == XR_PROGRAM_BUILD_INVALID_INPUT);
    CHECK(invalid.bytes == NULL);
    CHECK(first.bytes && reversed.bytes && first.size == reversed.size);
    if (!first.bytes || !reversed.bytes)
        goto cleanup;
    CHECK(memcmp(first.bytes, reversed.bytes, first.size) == 0);
    XrValidatedProgram *validated = NULL;
    CHECK(xr_program_validate(first.bytes, first.size, NULL, &validated, NULL) == XR_PROGRAM_VERIFY_OK);
    if (validated) {
        CHECK(validated->constant_count == XR_COUNTOF(f64_constant_bits));
        for (uint32_t index = 0u; index < validated->constant_count; ++index) {
            CHECK(validated->constants[index].kind == XR_CORE_IR_CONSTANT_F64);
            CHECK(validated->constants[index].type_id == XR_CORE_TYPE_F64);
            CHECK(validated->constants[index].value.f64_bits == f64_constant_bits[index]);
        }
    }
    xr_validated_program_free(validated);
    XrProgramView view;
    CHECK(xr_program_decode_structure(first.bytes, first.size, NULL, &view, NULL, 0u) == XR_PROGRAM_DECODE_OK);
    size_t offset = (size_t) view.sections[XR_PROGRAM_SECTION_CONSTANTS - 1u].offset;
    const uint8_t prefix[] = {10u, 0u, 19u, 5u, 0u, 1u, 19u, 5u, 1u};
    CHECK(memcmp(first.bytes + offset, prefix, sizeof(prefix)) == 0);
    uint8_t *bytes = xr_malloc(first.size);
    CHECK(bytes != NULL);
    if (bytes) {
        for (uint32_t mutation = 0u; mutation < 4u; ++mutation) {
            memcpy(bytes, first.bytes, first.size);
            switch (mutation) {
                case 0u: bytes[offset + 2u] = 2u; break;
                case 1u: bytes[offset + 3u] = 6u; break;
                case 2u: bytes[offset + 8u] = 0u; break; /* Duplicate positive zero. */
                case 3u: bytes[offset + 4u] = 2u; break; /* Descending unsigned bit order. */
            }
            CHECK(xr_program_decode_structure(bytes, first.size, NULL, &view, NULL, 0u) ==
                  XR_PROGRAM_DECODE_NONCANONICAL);
            validated = NULL;
            CHECK(xr_program_validate(bytes, first.size, NULL, &validated, NULL) != XR_PROGRAM_VERIFY_OK);
            CHECK(validated == NULL);
            xr_validated_program_free(validated);
        }
        xr_free(bytes);
    }
    size_t verify_allocations = 0u;
    for (size_t failure = 1u; failure < 256u; ++failure) {
        XrProgramDiagnostic diagnostic = {0};
        validated = NULL;
        allocation_probe_begin(failure);
        XrProgramVerifyStatus status =
            xr_program_validate(first.bytes, first.size, NULL, &validated, &diagnostic);
        if (status != XR_PROGRAM_VERIFY_OK) {
            CHECK(diagnostic.kind == XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY);
            CHECK(validated == NULL);
        }
        xr_validated_program_free(validated);
        verify_allocations = allocation_probe.attempts;
        allocation_probe_end();
        if (status == XR_PROGRAM_VERIFY_OK) {
            CHECK(verify_allocations + 1u == failure);
            break;
        }
    }
    CHECK(verify_allocations > 0u && verify_allocations < 255u);
    printf("f64 constant verifier allocation points: %zu\n", verify_allocations);
cleanup:
    xr_program_artifact_free(&first);
    xr_program_artifact_free(&reversed);
    xr_program_artifact_free(&invalid);
    size_t allocations = 0u;
    for (size_t failure = 1u; failure < 256u; ++failure) {
        XrProgramArtifact artifact = {0};
        allocation_probe_begin(failure);
        XrProgramBuildStatus status = write_f64_constants(false, false, &artifact);
        if (status != XR_PROGRAM_BUILD_OK)
            CHECK(status == XR_PROGRAM_BUILD_OUT_OF_MEMORY && artifact.bytes == NULL);
        xr_program_artifact_free(&artifact);
        allocations = allocation_probe.attempts;
        allocation_probe_end();
        if (status == XR_PROGRAM_BUILD_OK) {
            CHECK(allocations + 1u == failure);
            break;
        }
    }
    CHECK(allocations > 0u && allocations < 255u);
    printf("f64 constant constructor/writer allocation points: %zu\n", allocations);
}

int main(void) {
    CHECK(xr_core_spec_operation_by_id(150u) == NULL);
    CHECK(xr_core_spec_operation_by_id(167u) != NULL);
    test_f64_constant_bits_and_wire();
    test_array_element_place_structure();
    test_display_names_roundtrip_and_wire();
    test_display_names_invalid_construction();
    test_display_names_are_not_type_identity();
    test_retained_function_identity_publication();
    test_atomic_type_roundtrip();
    test_atomic_type_invalid_construction();
    test_atomic_type_hostile_wire();
    test_atomic_type_allocation_failures();
    test_resource_type_admission();
    test_resource_type_allocation_failures();
    test_record_type_roundtrip();
    test_record_type_invalid_and_hostile();
    test_record_type_allocation_failures();
    test_array_type_roundtrip();
    test_array_type_invalid_construction();
    test_array_type_hostile_wire();
    test_array_type_allocation_failures();
    test_module_slots_construction();
    test_module_slots_hostile_wire();
    test_module_initialization_construction();
    test_module_initialization_hostile_wire();
    for (uint32_t fixture = 0u; fixture < 26u; ++fixture)
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
