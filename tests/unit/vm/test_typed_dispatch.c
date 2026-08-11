/*
 * test_typed_dispatch.c - Closed typed TargetPlan scalar program execution
 */

#include "../../../src/ir/xi.h"
#include "../../../src/plan/format/xr_xtp_internal.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_plan_internal.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/vm/xr_typed_dispatch.h"
#include "../plan/target_profile_test_fixture.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

typedef struct TypedDispatchFixture {
    XrSemanticPlan *semantic;
    XrTargetProfile *profile;
    XrTargetPlan *base_plan;
    XrTargetPlan *program_plan;
    XrTargetInstructionRecord rows[16];
    uint32_t row_count;
} TypedDispatchFixture;

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static XrSemanticPlan *build_semantic(void) {
    XiFunc *function = xi_func_new("typed_dispatch_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *left = xi_const_int(function, entry, INT64_MAX, &stub_int);
    XiValue *right = xi_const_int(function, entry, 1, &stub_int);
    XiValue *copy = xi_value_new(function, entry, XI_COPY, &stub_int, 1);
    XiValue *add = xi_value_new(function, entry, XI_ADD, &stub_int, 2);
    XiValue *sub = xi_value_new(function, entry, XI_SUB, &stub_int, 2);
    XiValue *mul = xi_value_new(function, entry, XI_MUL, &stub_int, 2);
    REQUIRE(left && right && copy && add && sub && mul);
    copy->args[0] = left;
    add->args[0] = copy;
    add->args[1] = right;
    sub->args[0] = add;
    sub->args[1] = right;
    mul->args[0] = sub;
    mul->args[1] = right;
    xi_block_set_return(entry, add);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error, sizeof(error)));
    xi_func_free(function);
    return semantic;
}

static uint32_t slot_for_value(const XrTargetPlan *plan, uint32_t value) {
    const XrTargetValueRepRecord *record = xr_target_plan_value_rep(plan, value);
    REQUIRE(record != NULL && record->slot != XR_SEMANTIC_INDEX_NONE);
    return record->slot;
}

static uint8_t target_opcode(uint16_t semantic_opcode) {
    switch (semantic_opcode) {
        case XI_CONST: return XR_TARGET_INSTRUCTION_CONST_I64;
        case XI_COPY: return XR_TARGET_INSTRUCTION_COPY_I64;
        case XI_ADD: return XR_TARGET_INSTRUCTION_ADD_WRAP_I64;
        case XI_SUB: return XR_TARGET_INSTRUCTION_SUB_WRAP_I64;
        case XI_MUL: return XR_TARGET_INSTRUCTION_MUL_WRAP_I64;
        default: return XR_TARGET_INSTRUCTION_INVALID;
    }
}

static void assemble_rows(TypedDispatchFixture *fixture) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(fixture->semantic, &operand_count);
    uint32_t operations =
        (uint32_t) xr_semantic_plan_operation_count(fixture->semantic);
    REQUIRE(operations + 1u <= sizeof(fixture->rows) / sizeof(fixture->rows[0]));
    uint32_t seen = 0;
    for (uint32_t i = 0; i < operations; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(fixture->semantic, i);
        REQUIRE(operation != NULL && operation->function == 0 &&
                operation->operand_begin <= operand_count &&
                operation->operand_count <= operand_count - operation->operand_begin);
        uint8_t opcode = target_opcode(operation->opcode);
        REQUIRE(opcode != XR_TARGET_INSTRUCTION_INVALID && operation->operand_count <= 2);
        XrTargetInstructionRecord *row = &fixture->rows[fixture->row_count];
        *row = (XrTargetInstructionRecord) {
            .id = fixture->row_count,
            .function = 0,
            .result_slot = slot_for_value(fixture->base_plan,
                                          operation->result_value),
            .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE,
                              XR_TARGET_INSTRUCTION_SLOT_NONE},
            .immediate_bits = opcode == XR_TARGET_INSTRUCTION_CONST_I64
                                  ? (uint64_t) operation->semantic_immediate
                                  : 0,
            .opcode = opcode,
            .operand_count = (uint8_t) operation->operand_count,
        };
        for (uint16_t operand = 0; operand < operation->operand_count; operand++)
            row->operand_slots[operand] = slot_for_value(
                fixture->base_plan,
                operands[operation->operand_begin + operand].value);
        seen |= UINT32_C(1) << opcode;
        fixture->row_count++;
    }
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(fixture->semantic, 0);
    REQUIRE(function != NULL && function->block_count == 1);
    const XrSemanticBlockRecord *block =
        xr_semantic_plan_block(fixture->semantic, function->block_begin);
    REQUIRE(block != NULL && block->control_value != XR_SEMANTIC_INDEX_NONE);
    fixture->rows[fixture->row_count] = (XrTargetInstructionRecord) {
        .id = fixture->row_count,
        .function = 0,
        .result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE,
        .operand_slots = {slot_for_value(fixture->base_plan, block->control_value),
                          XR_TARGET_INSTRUCTION_SLOT_NONE},
        .opcode = XR_TARGET_INSTRUCTION_RETURN_I64,
        .operand_count = 1,
    };
    fixture->row_count++;
    uint32_t required = (UINT32_C(1) << XR_TARGET_INSTRUCTION_CONST_I64) |
                        (UINT32_C(1) << XR_TARGET_INSTRUCTION_COPY_I64) |
                        (UINT32_C(1) << XR_TARGET_INSTRUCTION_ADD_WRAP_I64) |
                        (UINT32_C(1) << XR_TARGET_INSTRUCTION_SUB_WRAP_I64) |
                        (UINT32_C(1) << XR_TARGET_INSTRUCTION_MUL_WRAP_I64);
    REQUIRE((seen & required) == required);
}

static bool freeze_with_rows(const TypedDispatchFixture *fixture,
                             const XrTargetInstructionRecord *rows,
                             uint32_t row_count, XrTargetPlan **out,
                             char *error, size_t error_size) {
    XrTargetPlanDraft draft = {
        .semantic_plan = fixture->semantic,
        .profile = fixture->profile,
        .completed_family_mask = XR_TARGET_REQUIRED_FAMILIES,
    };
#define COPY_TABLE(name) draft.name = xr_target_plan_##name(fixture->base_plan, &draft.name##_count)
    COPY_TABLE(machine_reps);
    COPY_TABLE(value_reps);
    COPY_TABLE(extents);
    COPY_TABLE(layouts);
    COPY_TABLE(fields);
    COPY_TABLE(storage);
    COPY_TABLE(allocations);
    COPY_TABLE(extent_operands);
    COPY_TABLE(functions);
    COPY_TABLE(slots);
    COPY_TABLE(calls);
    COPY_TABLE(call_arguments);
    COPY_TABLE(root_maps);
    COPY_TABLE(root_slots);
    COPY_TABLE(cleanups);
    COPY_TABLE(adapters);
    COPY_TABLE(capabilities);
    COPY_TABLE(coroutines);
#undef COPY_TABLE
    draft.instructions = rows;
    draft.instructions_count = row_count;
    return xr_target_plan_freeze(&draft, out, error, error_size);
}

static TypedDispatchFixture make_fixture(void) {
    TypedDispatchFixture fixture = {0};
    fixture.semantic = build_semantic();
    fixture.profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(fixture.profile != NULL);
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(fixture.semantic, fixture.profile,
                                 &fixture.base_plan, error, sizeof(error)));
    assemble_rows(&fixture);
    REQUIRE(freeze_with_rows(&fixture, fixture.rows, fixture.row_count,
                             &fixture.program_plan, error, sizeof(error)));
    return fixture;
}

static void dispose_fixture(TypedDispatchFixture *fixture) {
    xr_target_plan_free(fixture->program_plan);
    xr_target_plan_free(fixture->base_plan);
    xr_target_profile_free(fixture->profile);
    xr_semantic_plan_free(fixture->semantic);
    memset(fixture, 0, sizeof(*fixture));
}

static void test_closed_program_and_unavailable_boundary(void) {
    TypedDispatchFixture fixture = make_fixture();
    XrFingerprint base_fingerprint =
        xr_target_plan_fingerprint(fixture.base_plan);
    int64_t result = 7;
    REQUIRE(xr_target_plan_function_execution_family_mask(fixture.base_plan, 0) == 0);
    REQUIRE(xr_typed_dispatch_execute_i64(fixture.base_plan, &base_fingerprint,
                                          0, &result) ==
            XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE);
    REQUIRE(result == 0);

    XrFingerprint fingerprint =
        xr_target_plan_fingerprint(fixture.program_plan);
    REQUIRE(xr_target_plan_function_execution_family_mask(fixture.program_plan, 0) ==
            XR_TARGET_EXECUTION_SCALAR_I64_STRAIGHT_LINE);
    REQUIRE(xr_typed_dispatch_execute_i64(fixture.program_plan, &fingerprint,
                                          0, &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == INT64_MIN);

    XrSemanticPlan *retained_semantic = fixture.program_plan->semantic_plan;
    fixture.program_plan->semantic_plan = NULL;
    REQUIRE(xr_typed_dispatch_execute_i64(fixture.program_plan, &fingerprint,
                                          0, &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == INT64_MIN);
    fixture.program_plan->semantic_plan = retained_semantic;

    fixture.program_plan->instructions[0].immediate_bits ^= UINT64_C(1);
    REQUIRE(xr_typed_dispatch_execute_i64(fixture.program_plan, &fingerprint,
                                          0, &result) ==
            XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED);
    fixture.program_plan->instructions[0].immediate_bits ^= UINT64_C(1);

    fingerprint.bytes[0] ^= 1;
    REQUIRE(xr_typed_dispatch_execute_i64(fixture.program_plan, &fingerprint,
                                          0, &result) ==
            XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH);
    dispose_fixture(&fixture);
}

static void test_xtp_exact_roundtrip_executes_same_program(void) {
    TypedDispatchFixture fixture = make_fixture();
    uint8_t *bytes = NULL;
    size_t size = 0;
    char error[512] = {0};
    REQUIRE(xr_xtp_encode_plan(fixture.program_plan, &bytes, &size, error,
                               sizeof(error)));
    XrXtpCandidate *candidate = NULL;
    REQUIRE(xr_xtp_decode_candidate(bytes, size, &candidate, error,
                                    sizeof(error)));
    XrTargetPlan *decoded = NULL;
    REQUIRE(xr_xtp_materialize_target_plan(candidate, fixture.semantic,
                                           fixture.profile, &decoded, error,
                                           sizeof(error)));
    XrFingerprint fingerprint = xr_target_plan_fingerprint(decoded);
    REQUIRE(xr_fingerprint_equal(fingerprint,
                                 xr_target_plan_fingerprint(fixture.program_plan)));
    uint32_t instruction_count = 0;
    REQUIRE(xr_target_plan_instructions(decoded, &instruction_count) != NULL &&
            instruction_count == fixture.row_count);
    int64_t result = 0;
    REQUIRE(xr_typed_dispatch_execute_i64(decoded, &fingerprint, 0, &result) ==
            XR_TYPED_DISPATCH_OK);
    REQUIRE(result == INT64_MIN);
    xr_target_plan_free(decoded);
    xr_xtp_candidate_release(candidate);
    xr_xtp_encoded_free(bytes);
    dispose_fixture(&fixture);
}

static void expect_rows_rejected(TypedDispatchFixture *fixture,
                                 XrTargetInstructionRecord *rows) {
    XrTargetPlan *plan = (XrTargetPlan *) (uintptr_t) 1;
    char error[512] = {0};
    REQUIRE(!freeze_with_rows(fixture, rows, fixture->row_count, &plan, error,
                              sizeof(error)));
    REQUIRE(plan == NULL);
    REQUIRE(strncmp(error, "XR_TARGET_1005", strlen("XR_TARGET_1005")) == 0);
}

static void test_instruction_mutations_fail_closed(void) {
    TypedDispatchFixture fixture = make_fixture();
    XrTargetInstructionRecord rows[16];
    memcpy(rows, fixture.rows, sizeof(rows));
    rows[0].opcode = UINT8_MAX;
    expect_rows_rejected(&fixture, rows);

    memcpy(rows, fixture.rows, sizeof(rows));
    rows[0].opcode = XR_TARGET_INSTRUCTION_COPY_I64;
    rows[0].operand_count = 1;
    rows[0].immediate_bits = 0;
    rows[0].operand_slots[0] = rows[0].result_slot;
    expect_rows_rejected(&fixture, rows);

    memcpy(rows, fixture.rows, sizeof(rows));
    rows[1].result_slot = rows[0].result_slot;
    expect_rows_rejected(&fixture, rows);

    memcpy(rows, fixture.rows, sizeof(rows));
    rows[1].id = rows[0].id;
    expect_rows_rejected(&fixture, rows);

    memcpy(rows, fixture.rows, sizeof(rows));
    rows[fixture.row_count - 1u].function = UINT32_MAX;
    expect_rows_rejected(&fixture, rows);

    memcpy(rows, fixture.rows, sizeof(rows));
    rows[fixture.row_count - 2u].opcode = XR_TARGET_INSTRUCTION_RETURN_I64;
    rows[fixture.row_count - 2u].result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE;
    rows[fixture.row_count - 2u].operand_slots[1] = XR_TARGET_INSTRUCTION_SLOT_NONE;
    rows[fixture.row_count - 2u].operand_count = 1;
    expect_rows_rejected(&fixture, rows);
    dispose_fixture(&fixture);
}

int main(void) {
    test_closed_program_and_unavailable_boundary();
    test_xtp_exact_roundtrip_executes_same_program();
    test_instruction_mutations_fail_closed();
    puts("typed dispatch tests passed");
    return 0;
}
