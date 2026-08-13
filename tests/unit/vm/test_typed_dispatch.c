/*
 * test_typed_dispatch.c - Closed typed TargetPlan scalar program execution
 */

#include "../../../src/base/xmalloc.h"
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

/* A two-operand integer operation that is deliberately outside the executable
 * family: a rotate is an exact-width bit intrinsic keyed on a receiver width
 * this family has no authority over, so it must leave the whole function
 * unavailable rather than emit a partial group. */
static XrSemanticPlan *build_unsupported_semantic(void) {
    XiFunc *function = xi_func_new("typed_dispatch_unsupported", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *left = xi_const_int(function, entry, 8, &stub_int);
    XiValue *right = xi_const_int(function, entry, 2, &stub_int);
    XiValue *rotate = xi_value_new(function, entry, XI_BIT_ROTL, &stub_int, 2);
    REQUIRE(left && right && rotate);
    rotate->args[0] = left;
    rotate->args[1] = right;
    xi_block_set_return(entry, rotate);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error, sizeof(error)));
    xi_func_free(function);
    return semantic;
}

/* fn(first, second) -> (first - second) * first: the operand order and the
 * argument order are both asymmetric, so a swapped or zero-filled argument
 * vector cannot reach the expected result by accident. */
static XrSemanticPlan *build_parameter_semantic(void) {
    XiFunc *function = xi_func_new("typed_dispatch_parameters", &stub_int);
    REQUIRE(function != NULL);
    function->nparams = 2;
    function->min_params = 2;
    function->params = (XiValue **) xr_calloc(2, sizeof(XiValue *));
    REQUIRE(function->params != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    entry->sealed = true;
    function->params[0] = xi_param(function, entry, 0, &stub_int);
    function->params[1] = xi_param(function, entry, 1, &stub_int);
    XiValue *difference = xi_value_new(function, entry, XI_SUB, &stub_int, 2);
    XiValue *product = xi_value_new(function, entry, XI_MUL, &stub_int, 2);
    REQUIRE(function->params[0] && function->params[1] && difference && product);
    difference->args[0] = function->params[0];
    difference->args[1] = function->params[1];
    product->args[0] = difference;
    product->args[1] = function->params[0];
    xi_block_set_return(entry, product);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error, sizeof(error)));
    xi_func_free(function);
    return semantic;
}

/* fn(value, count) -> (value << count) - (value >> count): the two shifts read
 * the same pair in the same order, so the difference can only come out right
 * if each shift ran with its own kind, and swapping the arguments changes it. */
static XrSemanticPlan *build_shift_semantic(void) {
    XiFunc *function = xi_func_new("typed_dispatch_shifts", &stub_int);
    REQUIRE(function != NULL);
    function->nparams = 2;
    function->min_params = 2;
    function->params = (XiValue **) xr_calloc(2, sizeof(XiValue *));
    REQUIRE(function->params != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    entry->sealed = true;
    function->params[0] = xi_param(function, entry, 0, &stub_int);
    function->params[1] = xi_param(function, entry, 1, &stub_int);
    XiValue *left = xi_value_new(function, entry, XI_SHL, &stub_int, 2);
    XiValue *right = xi_value_new(function, entry, XI_SHR, &stub_int, 2);
    XiValue *difference = xi_value_new(function, entry, XI_SUB, &stub_int, 2);
    REQUIRE(function->params[0] && function->params[1] && left && right &&
            difference);
    left->args[0] = function->params[0];
    left->args[1] = function->params[1];
    right->args[0] = function->params[0];
    right->args[1] = function->params[1];
    difference->args[0] = left;
    difference->args[1] = right;
    xi_block_set_return(entry, difference);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error, sizeof(error)));
    xi_func_free(function);
    return semantic;
}

/* fn(first, second) -> (first OP second) - (second OP first) for an operation
 * that is not commutative, so swapping the argument pair negates the answer
 * and a folded constant could not track it. Both orders also reach the
 * operation with the other argument as divisor, so either position can carry
 * the zero that stops the program. */
static XrSemanticPlan *build_opposed_pair_semantic(const char *name, XiOp op) {
    XiFunc *function = xi_func_new(name, &stub_int);
    REQUIRE(function != NULL);
    function->nparams = 2;
    function->min_params = 2;
    function->params = (XiValue **) xr_calloc(2, sizeof(XiValue *));
    REQUIRE(function->params != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    entry->sealed = true;
    function->params[0] = xi_param(function, entry, 0, &stub_int);
    function->params[1] = xi_param(function, entry, 1, &stub_int);
    XiValue *forward = xi_value_new(function, entry, op, &stub_int, 2);
    XiValue *reverse = xi_value_new(function, entry, op, &stub_int, 2);
    XiValue *difference = xi_value_new(function, entry, XI_SUB, &stub_int, 2);
    REQUIRE(function->params[0] && function->params[1] && forward && reverse &&
            difference);
    forward->args[0] = function->params[0];
    forward->args[1] = function->params[1];
    reverse->args[0] = function->params[1];
    reverse->args[1] = function->params[0];
    difference->args[0] = forward;
    difference->args[1] = reverse;
    xi_block_set_return(entry, difference);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error, sizeof(error)));
    xi_func_free(function);
    return semantic;
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
#define COPY_TABLE(name)                                                                            \
    draft.name = xr_target_plan_##name(fixture->program_plan, &draft.name##_count)
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
                                 &fixture.program_plan, error, sizeof(error)));
    const XrTargetInstructionRecord *rows =
        xr_target_plan_instructions(fixture.program_plan, &fixture.row_count);
    REQUIRE(rows != NULL && fixture.row_count ==
                                xr_semantic_plan_operation_count(fixture.semantic) + 1u);
    REQUIRE(fixture.row_count <= sizeof(fixture.rows) / sizeof(fixture.rows[0]));
    memcpy(fixture.rows, rows,
           (size_t) fixture.row_count * sizeof(*fixture.rows));
    uint32_t seen = 0;
    for (uint32_t i = 0; i + 1u < fixture.row_count; i++)
        seen |= UINT32_C(1) << fixture.rows[i].opcode;
    uint32_t required = (UINT32_C(1) << XR_TARGET_INSTRUCTION_CONST_I64) |
                        (UINT32_C(1) << XR_TARGET_INSTRUCTION_COPY_I64) |
                        (UINT32_C(1) << XR_TARGET_INSTRUCTION_ADD_WRAP_I64) |
                        (UINT32_C(1) << XR_TARGET_INSTRUCTION_SUB_WRAP_I64) |
                        (UINT32_C(1) << XR_TARGET_INSTRUCTION_MUL_WRAP_I64);
    REQUIRE((seen & required) == required);
    REQUIRE(freeze_with_rows(&fixture, NULL, 0, &fixture.base_plan, error,
                             sizeof(error)));
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
                                          0, NULL, 0, &result) ==
            XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE);
    REQUIRE(result == 0);

    XrFingerprint fingerprint =
        xr_target_plan_fingerprint(fixture.program_plan);
    REQUIRE(xr_target_plan_function_execution_family_mask(fixture.program_plan, 0) ==
            XR_TARGET_EXECUTION_SCALAR_I64_STRAIGHT_LINE);
    REQUIRE(xr_typed_dispatch_execute_i64(fixture.program_plan, &fingerprint,
                                          0, NULL, 0,
                                          &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == INT64_MIN);

    XrSemanticPlan *retained_semantic = fixture.program_plan->semantic_plan;
    fixture.program_plan->semantic_plan = NULL;
    REQUIRE(xr_typed_dispatch_execute_i64(fixture.program_plan, &fingerprint,
                                          0, NULL, 0,
                                          &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == INT64_MIN);
    fixture.program_plan->semantic_plan = retained_semantic;

    fixture.program_plan->instructions[0].immediate_bits ^= UINT64_C(1);
    REQUIRE(xr_typed_dispatch_execute_i64(fixture.program_plan, &fingerprint,
                                          0, NULL, 0, &result) ==
            XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED);
    fixture.program_plan->instructions[0].immediate_bits ^= UINT64_C(1);

    fingerprint.bytes[0] ^= 1;
    REQUIRE(xr_typed_dispatch_execute_i64(fixture.program_plan, &fingerprint,
                                          0, NULL, 0, &result) ==
            XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH);
    dispose_fixture(&fixture);
}

static void test_production_builder_keeps_unsupported_function_unavailable(void) {
    XrSemanticPlan *semantic = build_unsupported_semantic();
    XrTargetProfile *profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error,
                                 sizeof(error)));
    uint32_t instruction_count = UINT32_MAX;
    REQUIRE(xr_target_plan_instructions(plan, &instruction_count) == NULL);
    REQUIRE(instruction_count == 0);
    REQUIRE(xr_target_plan_function_execution_family_mask(plan, 0) == 0);
    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    int64_t result = 7;
    REQUIRE(xr_typed_dispatch_execute_i64(plan, &fingerprint, 0, NULL, 0,
                                          &result) ==
            XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE);
    REQUIRE(result == 0);
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_parameters_reach_the_executed_program(void) {
    XrSemanticPlan *semantic = build_parameter_semantic();
    XrTargetProfile *profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error,
                                 sizeof(error)));
    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *rows =
        xr_target_plan_instructions(plan, &instruction_count);
    REQUIRE(rows != NULL && instruction_count ==
                                xr_semantic_plan_operation_count(semantic) + 1u);
    uint32_t parameter_rows = 0;
    uint64_t ordinals = 0;
    for (uint32_t i = 0; i < instruction_count; i++) {
        if (rows[i].opcode != XR_TARGET_INSTRUCTION_PARAM_I64)
            continue;
        ordinals |= UINT64_C(1) << rows[i].immediate_bits;
        parameter_rows++;
    }
    REQUIRE(parameter_rows == 2 && ordinals == 3);
    REQUIRE(xr_target_plan_function_execution_family_mask(plan, 0) ==
            XR_TARGET_EXECUTION_SCALAR_I64_STRAIGHT_LINE);

    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    const int64_t arguments[2] = {7, 5};
    int64_t result = 0;
    REQUIRE(xr_typed_dispatch_execute_i64(plan, &fingerprint, 0, arguments, 2,
                                          &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == (arguments[0] - arguments[1]) * arguments[0]);

    /* A different argument vector must produce a different result, so the
     * value cannot be coming from a constant folded into the plan. */
    const int64_t swapped[2] = {arguments[1], arguments[0]};
    REQUIRE(xr_typed_dispatch_execute_i64(plan, &fingerprint, 0, swapped, 2,
                                          &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == (swapped[0] - swapped[1]) * swapped[0]);

    /* Argument-count disagreement is refused instead of truncated or zero
     * filled, and the result stays untouched. */
    result = 11;
    REQUIRE(xr_typed_dispatch_execute_i64(plan, &fingerprint, 0, arguments, 1,
                                          &result) ==
            XR_TYPED_DISPATCH_ARGUMENT_MISMATCH);
    REQUIRE(result == 0);
    REQUIRE(xr_typed_dispatch_execute_i64(plan, &fingerprint, 0, arguments, 3,
                                          &result) ==
            XR_TYPED_DISPATCH_ARGUMENT_MISMATCH);
    REQUIRE(xr_typed_dispatch_execute_i64(plan, &fingerprint, 0, NULL, 0,
                                          &result) ==
            XR_TYPED_DISPATCH_ARGUMENT_MISMATCH);
    REQUIRE(xr_typed_dispatch_execute_i64(plan, &fingerprint, 0, NULL, 2,
                                          &result) ==
            XR_TYPED_DISPATCH_INVALID_ARGUMENT);

    /* The independent verifier, not the executor, is what keeps a mutated
     * argument binding out of an executable plan. */
    TypedDispatchFixture mutable_source = {
        .semantic = semantic, .profile = profile, .program_plan = plan};
    XrTargetInstructionRecord mutated[16];
    REQUIRE(instruction_count <= sizeof(mutated) / sizeof(mutated[0]));
    uint32_t first = UINT32_MAX;
    uint32_t second = UINT32_MAX;
    for (uint32_t i = 0; i < instruction_count; i++) {
        if (rows[i].opcode != XR_TARGET_INSTRUCTION_PARAM_I64)
            continue;
        if (first == UINT32_MAX)
            first = i;
        else if (second == UINT32_MAX)
            second = i;
    }
    REQUIRE(first != UINT32_MAX && second != UINT32_MAX);
    size_t row_bytes = (size_t) instruction_count * sizeof(*rows);
    /* Positive control: the unmutated rows must still freeze, so each
     * rejection below is attributable to its mutation. */
    memcpy(mutated, rows, row_bytes);
    XrTargetPlan *refrozen = NULL;
    REQUIRE(freeze_with_rows(&mutable_source, mutated, instruction_count,
                             &refrozen, error, sizeof(error)));
    xr_target_plan_free(refrozen);
    const struct {
        uint32_t row;
        uint8_t opcode;
        uint64_t immediate;
    } mutations[] = {
        /* Two rows claiming the same argument leave one parameter unbound. */
        {second, XR_TARGET_INSTRUCTION_PARAM_I64, rows[first].immediate_bits},
        /* A sparse ordinal disagrees with the declared argument count. */
        {second, XR_TARGET_INSTRUCTION_PARAM_I64, 5},
        /* A constant may not define a parameter slot the caller must fill. */
        {first, XR_TARGET_INSTRUCTION_CONST_I64, 3},
    };
    for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        memcpy(mutated, rows, row_bytes);
        mutated[mutations[i].row].opcode = mutations[i].opcode;
        mutated[mutations[i].row].immediate_bits = mutations[i].immediate;
        XrTargetPlan *rejected = (XrTargetPlan *) (uintptr_t) 1;
        REQUIRE(!freeze_with_rows(&mutable_source, mutated, instruction_count,
                                  &rejected, error, sizeof(error)));
        REQUIRE(rejected == NULL);
        REQUIRE(strncmp(error, "XR_TARGET_1005", strlen("XR_TARGET_1005")) == 0);
    }
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
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
    REQUIRE(xr_typed_dispatch_execute_i64(decoded, &fingerprint, 0, NULL, 0,
                                          &result) == XR_TYPED_DISPATCH_OK);
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

/* Expected values are written out rather than recomputed from the shift helper
 * the executor uses, so the assertions are an independent oracle for the
 * language rule instead of a restatement of the implementation. */
static void test_shift_rows_execute_with_masked_counts(void) {
    XrSemanticPlan *semantic = build_shift_semantic();
    XrTargetProfile *profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error,
                                 sizeof(error)));
    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *rows =
        xr_target_plan_instructions(plan, &instruction_count);
    REQUIRE(rows != NULL && instruction_count ==
                                xr_semantic_plan_operation_count(semantic) + 1u);
    uint32_t shl = UINT32_MAX;
    uint32_t shr = UINT32_MAX;
    for (uint32_t i = 0; i < instruction_count; i++) {
        if (rows[i].opcode == XR_TARGET_INSTRUCTION_SHL_MASKED_I64)
            shl = i;
        else if (rows[i].opcode == XR_TARGET_INSTRUCTION_SHR_ARITH_MASKED_I64)
            shr = i;
    }
    REQUIRE(shl != UINT32_MAX && shr != UINT32_MAX);
    REQUIRE(xr_target_plan_function_execution_family_mask(plan, 0) ==
            XR_TARGET_EXECUTION_SCALAR_I64_STRAIGHT_LINE);

    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    const struct {
        int64_t value;
        int64_t count;
        int64_t expected;
    } cases[] = {
        {1, 4, 16},
        /* A negative left operand proves the right shift is arithmetic: a
         * logical shift would make the subtrahend a huge positive number. */
        {-256, 3, -2016},
        /* 67 and 3 select the same shift, which is the modulo-64 count rule. */
        {-256, 67, -2016},
        /* A count of 64 is a zero shift, not a wipe and not undefined. */
        {1, 64, 0},
        /* Shifting into the sign bit wraps instead of trapping. */
        {-1, 63, INT64_MIN + 1},
        /* Swapping the pair changes the answer, so the value is executed
         * rather than folded into the plan. */
        {4, 1, 6},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const int64_t arguments[2] = {cases[i].value, cases[i].count};
        int64_t result = 0;
        REQUIRE(xr_typed_dispatch_execute_i64(plan, &fingerprint, 0, arguments,
                                              2, &result) ==
                XR_TYPED_DISPATCH_OK);
        REQUIRE(result == cases[i].expected);
    }

    TypedDispatchFixture mutable_source = {.semantic = semantic,
                                           .profile = profile,
                                           .program_plan = plan,
                                           .row_count = instruction_count};
    XrTargetInstructionRecord mutated[16];
    REQUIRE(instruction_count <= sizeof(mutated) / sizeof(mutated[0]));
    size_t row_bytes = (size_t) instruction_count * sizeof(*rows);
    /* Positive control: the unmutated rows still freeze, so every rejection
     * below is attributable to its own mutation. */
    memcpy(mutated, rows, row_bytes);
    XrTargetPlan *refrozen = NULL;
    REQUIRE(freeze_with_rows(&mutable_source, mutated, instruction_count,
                             &refrozen, error, sizeof(error)));
    xr_target_plan_free(refrozen);

    /* There is no immediate shift form: a count folded into the row would
     * bypass the slot the executor masks. */
    memcpy(mutated, rows, row_bytes);
    mutated[shl].immediate_bits = 3;
    expect_rows_rejected(&mutable_source, mutated);

    /* A shift is binary; dropping the count operand may not leave a row the
     * executor would read an unwritten second slot for. */
    memcpy(mutated, rows, row_bytes);
    mutated[shr].operand_count = 1;
    expect_rows_rejected(&mutable_source, mutated);

    memcpy(mutated, rows, row_bytes);
    mutated[shr].operand_slots[1] = XR_TARGET_INSTRUCTION_SLOT_NONE;
    expect_rows_rejected(&mutable_source, mutated);

    /* The count must be defined before the shift reads it. */
    memcpy(mutated, rows, row_bytes);
    mutated[shl].operand_slots[1] = mutated[instruction_count - 2u].result_slot;
    expect_rows_rejected(&mutable_source, mutated);

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

/* Expected values are written out rather than recomputed from the division
 * helpers the executor uses, so the assertions independently pin truncation
 * toward zero, the sign of a remainder, and the two edges C leaves undefined. */
static void test_division_rows_take_the_zero_divisor_edge(void) {
    const struct {
        const char *name;
        XiOp op;
        uint8_t opcode;
        XrTypedDispatchStatus zero_status;
        struct {
            int64_t first;
            int64_t second;
            int64_t expected;
        } cases[4];
    } families[] = {
        {"typed_dispatch_division", XI_DIV, XR_TARGET_INSTRUCTION_DIV_TRAP_I64,
         XR_TYPED_DISPATCH_DIVIDE_BY_ZERO,
         {/* 17/5 - 5/17 */
          {17, 5, 3},
          /* Swapping the pair negates the answer, so it is executed rather
           * than folded into the plan. */
          {5, 17, -3},
          /* Truncation toward zero: a flooring division would give -4. */
          {-17, 5, -3},
          /* INT64_MIN / -1 wraps to INT64_MIN instead of trapping, and
           * -1 / INT64_MIN is zero. */
          {INT64_MIN, -1, INT64_MIN}}},
        {"typed_dispatch_modulo", XI_MOD, XR_TARGET_INSTRUCTION_MOD_TRAP_I64,
         XR_TYPED_DISPATCH_MODULO_BY_ZERO,
         {/* 17%5 - 5%17 */
          {17, 5, -3},
          {5, 17, 3},
          /* The remainder takes the dividend's sign: a floored modulo would
           * make the first term 3 and the answer 8. */
          {-17, 5, -7},
          /* INT64_MIN % -1 is zero, and -1 % INT64_MIN is -1. */
          {INT64_MIN, -1, 1}}},
    };
    for (size_t family = 0; family < sizeof(families) / sizeof(families[0]);
         family++) {
        XrSemanticPlan *semantic =
            build_opposed_pair_semantic(families[family].name,
                                        families[family].op);
        XrTargetProfile *profile = xr_test_target_profile_build(
            false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        REQUIRE(profile != NULL);
        XrTargetPlan *plan = NULL;
        char error[512] = {0};
        REQUIRE(xr_target_plan_build(semantic, profile, &plan, error,
                                     sizeof(error)));
        uint32_t instruction_count = 0;
        const XrTargetInstructionRecord *rows =
            xr_target_plan_instructions(plan, &instruction_count);
        REQUIRE(rows != NULL &&
                instruction_count ==
                    xr_semantic_plan_operation_count(semantic) + 1u);
        uint32_t first = UINT32_MAX;
        uint32_t second = UINT32_MAX;
        for (uint32_t i = 0; i < instruction_count; i++) {
            if (rows[i].opcode != families[family].opcode)
                continue;
            if (first == UINT32_MAX)
                first = i;
            else if (second == UINT32_MAX)
                second = i;
        }
        REQUIRE(first != UINT32_MAX && second != UINT32_MAX);
        REQUIRE(xr_target_plan_function_execution_family_mask(plan, 0) ==
                XR_TARGET_EXECUTION_SCALAR_I64_STRAIGHT_LINE);

        XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
        for (size_t i = 0; i < 4; i++) {
            const int64_t arguments[2] = {families[family].cases[i].first,
                                          families[family].cases[i].second};
            int64_t result = 0;
            REQUIRE(xr_typed_dispatch_execute_i64(plan, &fingerprint, 0,
                                                  arguments, 2, &result) ==
                    XR_TYPED_DISPATCH_OK);
            REQUIRE(result == families[family].cases[i].expected);
        }

        /* The verifier admits the row, so the executor is what stops a zero
         * divisor, in either operand position, and it leaves no result. */
        const int64_t divisor_zero[2] = {17, 0};
        const int64_t dividend_zero[2] = {0, 17};
        int64_t result = 11;
        REQUIRE(xr_typed_dispatch_execute_i64(plan, &fingerprint, 0,
                                              divisor_zero, 2, &result) ==
                families[family].zero_status);
        REQUIRE(result == 0);
        result = 11;
        REQUIRE(xr_typed_dispatch_execute_i64(plan, &fingerprint, 0,
                                              dividend_zero, 2, &result) ==
                families[family].zero_status);
        REQUIRE(result == 0);

        TypedDispatchFixture mutable_source = {.semantic = semantic,
                                               .profile = profile,
                                               .program_plan = plan,
                                               .row_count = instruction_count};
        XrTargetInstructionRecord mutated[16];
        REQUIRE(instruction_count <= sizeof(mutated) / sizeof(mutated[0]));
        size_t row_bytes = (size_t) instruction_count * sizeof(*rows);
        /* Positive control: the unmutated rows still freeze, so every
         * rejection below is attributable to its own mutation. */
        memcpy(mutated, rows, row_bytes);
        XrTargetPlan *refrozen = NULL;
        REQUIRE(freeze_with_rows(&mutable_source, mutated, instruction_count,
                                 &refrozen, error, sizeof(error)));
        xr_target_plan_free(refrozen);

        /* There is no immediate divisor form, so no row can carry a divisor
         * the executor never inspects for zero. */
        memcpy(mutated, rows, row_bytes);
        mutated[first].immediate_bits = 2;
        expect_rows_rejected(&mutable_source, mutated);

        memcpy(mutated, rows, row_bytes);
        mutated[second].operand_count = 1;
        expect_rows_rejected(&mutable_source, mutated);

        memcpy(mutated, rows, row_bytes);
        mutated[second].operand_slots[1] = XR_TARGET_INSTRUCTION_SLOT_NONE;
        expect_rows_rejected(&mutable_source, mutated);

        /* The divisor must be defined before the division reads it. */
        memcpy(mutated, rows, row_bytes);
        mutated[first].operand_slots[1] =
            mutated[instruction_count - 2u].result_slot;
        expect_rows_rejected(&mutable_source, mutated);

        xr_target_plan_free(plan);
        xr_target_profile_free(profile);
        xr_semantic_plan_free(semantic);
    }
}

int main(void) {
    test_closed_program_and_unavailable_boundary();
    test_production_builder_keeps_unsupported_function_unavailable();
    test_parameters_reach_the_executed_program();
    test_xtp_exact_roundtrip_executes_same_program();
    test_instruction_mutations_fail_closed();
    test_shift_rows_execute_with_masked_counts();
    test_division_rows_take_the_zero_divisor_edge();
    puts("typed dispatch tests passed");
    return 0;
}
