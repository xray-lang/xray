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
/* The type a comparison answers. It is not an integer with a restricted range:
 * the plan lays a `bool` value out as a one-byte I1 slot, which is exactly why
 * the executable family needs a truth slot beside its signed i64 one. */
static XrType stub_bool = {.kind = XR_KIND_BOOL,
                           .id = 2,
                           .frozen = true,
                           .scalar_rep = XR_SCALAR_REP_NONE};

static void test_generated_instruction_contract(void) {
    enum {
        XR_TEST_VM_DISPATCH_COUNT = 0
#define XR_VM_OP(symbol, handler, kind, argument) +1
#include "../../../src/vm/xr_vm_ops.def"
#undef XR_VM_OP
    };
    REQUIRE(XR_TARGET_INSTRUCTION_CONST_I64 == 1);
    REQUIRE(XR_TARGET_INSTRUCTION_BRANCH_IF_TRUE_BOOL == 25);
    REQUIRE(XR_TARGET_INSTRUCTION_CONTRACT_COUNT == 25u);
    REQUIRE(XR_TEST_VM_DISPATCH_COUNT ==
            XR_TARGET_INSTRUCTION_CONTRACT_COUNT);
    uint32_t semantic_bindings = 0;
    for (uint16_t opcode = 1; opcode < XR_TARGET_INSTRUCTION_COUNT; opcode++) {
        const XrTargetInstructionContract *contract =
            xr_target_instruction_contract(opcode);
        REQUIRE(contract != NULL && contract->name != NULL);
        REQUIRE(contract->arity <= 2u);
        REQUIRE(contract->terminator ==
                (contract->control_kind != XR_TARGET_INSTRUCTION_CONTROL_NONE));
        REQUIRE(contract->terminator ==
                (contract->result_rep == XR_TARGET_INSTRUCTION_REP_NONE));
        REQUIRE((contract->error_kind != XR_TARGET_INSTRUCTION_ERROR_NONE) ==
                ((contract->effects &
                  XR_TARGET_INSTRUCTION_EFFECT_MAY_ERROR) != 0));
        REQUIRE(contract->may_suspend ==
                ((contract->effects &
                  XR_TARGET_INSTRUCTION_EFFECT_MAY_SUSPEND) != 0));
        semantic_bindings += contract->semantic_name != NULL;
        for (uint32_t other = 1; other < opcode; other++)
            REQUIRE(strcmp(contract->name,
                           xr_target_instruction_opcode_name(other)) != 0);
    }
    REQUIRE(semantic_bindings == 21u);
    REQUIRE(xr_target_instruction_contract(XR_TARGET_INSTRUCTION_INVALID) ==
            NULL);
    REQUIRE(xr_target_instruction_contract(XR_TARGET_INSTRUCTION_COUNT) ==
            NULL);
}

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

/* fn(selector, first, second) -> selector != 0 ? first - second : second - first.
 * The arms compute opposite differences, so taking the wrong edge negates the
 * answer and no constant could track either. Both arms read values the entry
 * block defined, which is the cross-block use the verifier has to prove. */
static XrSemanticPlan *build_branch_semantic(void) {
    XiFunc *function = xi_func_new("typed_dispatch_branch", &stub_int);
    REQUIRE(function != NULL);
    function->nparams = 3;
    function->min_params = 3;
    function->params = (XiValue **) xr_calloc(3, sizeof(XiValue *));
    REQUIRE(function->params != NULL);
    XiBlock *entry = xi_block_new(function);
    XiBlock *taken = xi_block_new(function);
    XiBlock *untaken = xi_block_new(function);
    REQUIRE(entry && taken && untaken);
    entry->sealed = true;
    for (uint16_t ordinal = 0; ordinal < 3; ordinal++) {
        function->params[ordinal] = xi_param(function, entry, ordinal, &stub_int);
        REQUIRE(function->params[ordinal] != NULL);
    }
    XiValue *forward = xi_value_new(function, taken, XI_SUB, &stub_int, 2);
    XiValue *reverse = xi_value_new(function, untaken, XI_SUB, &stub_int, 2);
    REQUIRE(forward && reverse);
    forward->args[0] = function->params[1];
    forward->args[1] = function->params[2];
    reverse->args[0] = function->params[2];
    reverse->args[1] = function->params[1];
    xi_block_set_if(entry, function->params[0], taken, untaken);
    xi_block_set_return(taken, forward);
    xi_block_set_return(untaken, reverse);
    taken->sealed = true;
    untaken->sealed = true;
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error, sizeof(error)));
    xi_func_free(function);
    return semantic;
}

/* fn(first, second) -> ((first - second) * first) - second, split across three
 * blocks chained by unconditional jumps. Each block reads a value the previous
 * one defined, so the answer is only reachable if control really moved and the
 * values survived the move. */
static XrSemanticPlan *build_jump_semantic(void) {
    XiFunc *function = xi_func_new("typed_dispatch_jump", &stub_int);
    REQUIRE(function != NULL);
    function->nparams = 2;
    function->min_params = 2;
    function->params = (XiValue **) xr_calloc(2, sizeof(XiValue *));
    REQUIRE(function->params != NULL);
    XiBlock *entry = xi_block_new(function);
    XiBlock *middle = xi_block_new(function);
    XiBlock *exit = xi_block_new(function);
    REQUIRE(entry && middle && exit);
    entry->sealed = true;
    function->params[0] = xi_param(function, entry, 0, &stub_int);
    function->params[1] = xi_param(function, entry, 1, &stub_int);
    XiValue *difference = xi_value_new(function, entry, XI_SUB, &stub_int, 2);
    XiValue *product = xi_value_new(function, middle, XI_MUL, &stub_int, 2);
    XiValue *answer = xi_value_new(function, exit, XI_SUB, &stub_int, 2);
    REQUIRE(function->params[0] && function->params[1] && difference && product &&
            answer);
    difference->args[0] = function->params[0];
    difference->args[1] = function->params[1];
    product->args[0] = difference;
    product->args[1] = function->params[0];
    answer->args[0] = product;
    answer->args[1] = function->params[1];
    xi_block_set_jump(entry, middle);
    xi_block_set_jump(middle, exit);
    xi_block_set_return(exit, answer);
    middle->sealed = true;
    exit->sealed = true;
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error, sizeof(error)));
    xi_func_free(function);
    return semantic;
}

/* fn(first, second) -> if (first REL second) { return first + second }
 *                      return first - second
 *
 * This is the shape a real `if (a > b)` has: the condition is the bool a
 * comparison produced, not an integer the caller handed in. The two arms are
 * different functions of the same pair, so the answer names which edge ran, and
 * the greater/less/equal argument pairs the test drives it with separate all
 * six relations from one another. */
static XrSemanticPlan *build_compare_semantic(const char *name, XiOp op) {
    XiFunc *function = xi_func_new(name, &stub_int);
    REQUIRE(function != NULL);
    function->nparams = 2;
    function->min_params = 2;
    function->params = (XiValue **) xr_calloc(2, sizeof(XiValue *));
    REQUIRE(function->params != NULL);
    XiBlock *entry = xi_block_new(function);
    XiBlock *taken = xi_block_new(function);
    XiBlock *untaken = xi_block_new(function);
    REQUIRE(entry && taken && untaken);
    entry->sealed = true;
    function->params[0] = xi_param(function, entry, 0, &stub_int);
    function->params[1] = xi_param(function, entry, 1, &stub_int);
    XiValue *relation = xi_value_new(function, entry, op, &stub_bool, 2);
    XiValue *sum = xi_value_new(function, taken, XI_ADD, &stub_int, 2);
    XiValue *difference = xi_value_new(function, untaken, XI_SUB, &stub_int, 2);
    REQUIRE(function->params[0] && function->params[1] && relation && sum &&
            difference);
    relation->args[0] = function->params[0];
    relation->args[1] = function->params[1];
    sum->args[0] = function->params[0];
    sum->args[1] = function->params[1];
    difference->args[0] = function->params[0];
    difference->args[1] = function->params[1];
    xi_block_set_if(entry, relation, taken, untaken);
    xi_block_set_return(taken, sum);
    xi_block_set_return(untaken, difference);
    taken->sealed = true;
    untaken->sealed = true;
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
            XR_TARGET_EXECUTION_SCALAR_I64_CLOSED);
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
            XR_TARGET_EXECUTION_SCALAR_I64_CLOSED);

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
            XR_TARGET_EXECUTION_SCALAR_I64_CLOSED);

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
                XR_TARGET_EXECUTION_SCALAR_I64_CLOSED);

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

/* The row group is the only description of the control flow, so the tests find
 * the block boundaries the same way the verifier does: a block begins at the
 * first row and after every terminator. */
static uint32_t row_of_opcode(const XrTargetInstructionRecord *rows,
                              uint32_t row_count, uint8_t opcode,
                              uint32_t occurrence) {
    for (uint32_t i = 0; i < row_count; i++) {
        if (rows[i].opcode != opcode)
            continue;
        if (occurrence == 0)
            return i;
        occurrence--;
    }
    return UINT32_MAX;
}

static void test_conditional_branch_selects_its_edge(void) {
    XrSemanticPlan *semantic = build_branch_semantic();
    XrTargetProfile *profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *rows =
        xr_target_plan_instructions(plan, &instruction_count);
    /* Three blocks, so three terminator rows on top of every operation. */
    REQUIRE(rows != NULL &&
            instruction_count ==
                xr_semantic_plan_operation_count(semantic) + 3u);
    uint32_t branch = row_of_opcode(rows, instruction_count,
                                    XR_TARGET_INSTRUCTION_BRANCH_IF_NONZERO_I64, 0);
    uint32_t forward = row_of_opcode(rows, instruction_count,
                                     XR_TARGET_INSTRUCTION_SUB_WRAP_I64, 0);
    uint32_t reverse = row_of_opcode(rows, instruction_count,
                                     XR_TARGET_INSTRUCTION_SUB_WRAP_I64, 1);
    uint32_t last_return = row_of_opcode(rows, instruction_count,
                                         XR_TARGET_INSTRUCTION_RETURN_I64, 1);
    REQUIRE(branch != UINT32_MAX && forward != UINT32_MAX &&
            reverse != UINT32_MAX && last_return == instruction_count - 1u);
    /* Both edges are carried by the branch row itself, and each names the first
     * row of an arm rather than the row that happens to follow. */
    uint32_t nonzero_target =
        XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(rows[branch].immediate_bits);
    uint32_t zero_target =
        XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(rows[branch].immediate_bits);
    REQUIRE(nonzero_target == forward && zero_target == reverse);
    REQUIRE(xr_target_plan_function_execution_family_mask(plan, 0) ==
            XR_TARGET_EXECUTION_SCALAR_I64_CLOSED);

    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    const struct {
        int64_t selector;
        int64_t first;
        int64_t second;
        int64_t expected;
    } cases[] = {
        {1, 9, 4, 5},
        /* Zero is the only value that takes the other edge, and it negates the
         * answer, so a branch that ignored the condition could not produce
         * both results from the same pair. */
        {0, 9, 4, -5},
        /* A negative condition is still nonzero: the test is against zero, not
         * against a sign or a low bit. */
        {-1, 9, 4, 5},
        /* A value whose low 32 bits are zero must not read as zero. */
        {INT64_C(1) << 32, 9, 4, 5},
        {0, 4, 9, 5},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const int64_t arguments[3] = {cases[i].selector, cases[i].first,
                                      cases[i].second};
        int64_t result = 0;
        REQUIRE(xr_typed_dispatch_execute_i64(plan, &fingerprint, 0, arguments,
                                              3, &result) ==
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

    /* An edge past the end of the group. */
    memcpy(mutated, rows, row_bytes);
    mutated[branch].immediate_bits =
        XR_TARGET_INSTRUCTION_TARGET_PACK(instruction_count, zero_target);
    expect_rows_rejected(&mutable_source, mutated);

    /* An edge back into the middle of the entry block. */
    memcpy(mutated, rows, row_bytes);
    mutated[branch].immediate_bits =
        XR_TARGET_INSTRUCTION_TARGET_PACK(1, zero_target);
    expect_rows_rejected(&mutable_source, mutated);

    memcpy(mutated, rows, row_bytes);
    mutated[branch].immediate_bits =
        XR_TARGET_INSTRUCTION_TARGET_PACK(nonzero_target, branch);
    expect_rows_rejected(&mutable_source, mutated);

    /* Both edges to one arm leaves the other arm unreachable, and rows no path
     * can reach are refused rather than carried. */
    memcpy(mutated, rows, row_bytes);
    mutated[branch].immediate_bits =
        XR_TARGET_INSTRUCTION_TARGET_PACK(nonzero_target, nonzero_target);
    expect_rows_rejected(&mutable_source, mutated);

    /* A jump may not smuggle a second edge in the half of the immediate it
     * does not use. */
    memcpy(mutated, rows, row_bytes);
    mutated[branch].opcode = XR_TARGET_INSTRUCTION_JUMP;
    mutated[branch].operand_count = 0;
    mutated[branch].operand_slots[0] = XR_TARGET_INSTRUCTION_SLOT_NONE;
    expect_rows_rejected(&mutable_source, mutated);

    /* The condition is read like any other operand, so it must be defined. */
    memcpy(mutated, rows, row_bytes);
    mutated[branch].operand_slots[0] = rows[forward].result_slot;
    expect_rows_rejected(&mutable_source, mutated);

    /* The cross-block proof itself: one arm reading the value the other arm
     * defines is undefined on the path that reaches it, even though the row
     * that defines it exists in the group. */
    memcpy(mutated, rows, row_bytes);
    mutated[forward].operand_slots[1] = rows[reverse].result_slot;
    expect_rows_rejected(&mutable_source, mutated);

    /* A group whose last row is not a terminator could run off the end of the
     * table. */
    memcpy(mutated, rows, row_bytes);
    mutated[last_return].opcode = XR_TARGET_INSTRUCTION_COPY_I64;
    mutated[last_return].result_slot = rows[reverse].result_slot;
    expect_rows_rejected(&mutable_source, mutated);

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_unconditional_jumps_chain_blocks(void) {
    XrSemanticPlan *semantic = build_jump_semantic();
    XrTargetProfile *profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *rows =
        xr_target_plan_instructions(plan, &instruction_count);
    REQUIRE(rows != NULL &&
            instruction_count ==
                xr_semantic_plan_operation_count(semantic) + 3u);
    uint32_t first_jump =
        row_of_opcode(rows, instruction_count, XR_TARGET_INSTRUCTION_JUMP, 0);
    uint32_t second_jump =
        row_of_opcode(rows, instruction_count, XR_TARGET_INSTRUCTION_JUMP, 1);
    REQUIRE(first_jump != UINT32_MAX && second_jump != UINT32_MAX);
    REQUIRE(XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(rows[first_jump].immediate_bits) == 0);
    REQUIRE(XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(rows[first_jump].immediate_bits) ==
            first_jump + 1u);
    REQUIRE(xr_target_plan_function_execution_family_mask(plan, 0) ==
            XR_TARGET_EXECUTION_SCALAR_I64_CLOSED);

    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    const int64_t arguments[2] = {7, 5};
    int64_t result = 0;
    REQUIRE(xr_typed_dispatch_execute_i64(plan, &fingerprint, 0, arguments, 2,
                                          &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == ((arguments[0] - arguments[1]) * arguments[0]) - arguments[1]);
    const int64_t swapped[2] = {arguments[1], arguments[0]};
    REQUIRE(xr_typed_dispatch_execute_i64(plan, &fingerprint, 0, swapped, 2,
                                          &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == ((swapped[0] - swapped[1]) * swapped[0]) - swapped[1]);

    TypedDispatchFixture mutable_source = {.semantic = semantic,
                                           .profile = profile,
                                           .program_plan = plan,
                                           .row_count = instruction_count};
    XrTargetInstructionRecord mutated[16];
    REQUIRE(instruction_count <= sizeof(mutated) / sizeof(mutated[0]));
    size_t row_bytes = (size_t) instruction_count * sizeof(*rows);
    memcpy(mutated, rows, row_bytes);
    XrTargetPlan *refrozen = NULL;
    REQUIRE(freeze_with_rows(&mutable_source, mutated, instruction_count,
                             &refrozen, error, sizeof(error)));
    xr_target_plan_free(refrozen);

    /* A jump carries one edge, so the unused half of its immediate must stay
     * zero. */
    memcpy(mutated, rows, row_bytes);
    mutated[first_jump].immediate_bits = XR_TARGET_INSTRUCTION_TARGET_PACK(
        XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(rows[first_jump].immediate_bits),
        second_jump + 1u);
    expect_rows_rejected(&mutable_source, mutated);

    /* Jumping over the middle block leaves the value it defines undefined on
     * the only path that reaches the block reading it. */
    memcpy(mutated, rows, row_bytes);
    mutated[first_jump].immediate_bits =
        XR_TARGET_INSTRUCTION_TARGET_PACK(second_jump + 1u, 0);
    expect_rows_rejected(&mutable_source, mutated);

    /* An edge to the last row of the final block: every block stays reachable
     * and every value is still assigned before the row that reads it, so the
     * only thing wrong is that the target is not where a block begins. That
     * makes this the case the block-entry rule alone has to refuse. */
    memcpy(mutated, rows, row_bytes);
    mutated[second_jump].immediate_bits =
        XR_TARGET_INSTRUCTION_TARGET_PACK(instruction_count - 1u, 0);
    expect_rows_rejected(&mutable_source, mutated);

    /* A jump takes no operand: an operand slot would be a value the executor
     * never reads and the def-use proof would never see. */
    memcpy(mutated, rows, row_bytes);
    mutated[second_jump].operand_count = 1;
    mutated[second_jump].operand_slots[0] = rows[first_jump - 1u].result_slot;
    expect_rows_rejected(&mutable_source, mutated);

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

/* Expected values are written out rather than recomputed from the comparison
 * owner the executor consumes, so the assertions are an independent oracle for
 * each relation instead of a restatement of the implementation. */
static void test_comparison_rows_drive_the_branch(void) {
    const struct {
        const char *name;
        XiOp op;
        uint8_t opcode;
        /* greater, less, equal: the taken arm answers first + second and the
         * other answers first - second, so these three triples are pairwise
         * distinct across all six relations and no relation can pass with
         * another relation's edges. */
        int64_t expected[3];
    } relations[] = {
        {"typed_dispatch_eq", XI_EQ, XR_TARGET_INSTRUCTION_CMP_EQ_I64, {2, -2, 10}},
        {"typed_dispatch_ne", XI_NE, XR_TARGET_INSTRUCTION_CMP_NE_I64, {12, 12, 0}},
        {"typed_dispatch_lt", XI_LT, XR_TARGET_INSTRUCTION_CMP_LT_I64, {2, 12, 0}},
        {"typed_dispatch_le", XI_LE, XR_TARGET_INSTRUCTION_CMP_LE_I64, {2, 12, 10}},
        {"typed_dispatch_gt", XI_GT, XR_TARGET_INSTRUCTION_CMP_GT_I64, {12, -2, 0}},
        {"typed_dispatch_ge", XI_GE, XR_TARGET_INSTRUCTION_CMP_GE_I64, {12, -2, 10}},
    };
    const int64_t pairs[3][2] = {{7, 5}, {5, 7}, {5, 5}};
    for (size_t relation = 0; relation < sizeof(relations) / sizeof(relations[0]);
         relation++) {
        XrSemanticPlan *semantic =
            build_compare_semantic(relations[relation].name,
                                   relations[relation].op);
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
                    xr_semantic_plan_operation_count(semantic) + 3u);
        uint32_t compare = row_of_opcode(rows, instruction_count,
                                         relations[relation].opcode, 0);
        uint32_t branch = row_of_opcode(rows, instruction_count,
                                        XR_TARGET_INSTRUCTION_BRANCH_IF_TRUE_BOOL, 0);
        uint32_t sum = row_of_opcode(rows, instruction_count,
                                     XR_TARGET_INSTRUCTION_ADD_WRAP_I64, 0);
        uint32_t difference = row_of_opcode(rows, instruction_count,
                                            XR_TARGET_INSTRUCTION_SUB_WRAP_I64, 0);
        REQUIRE(compare != UINT32_MAX && branch != UINT32_MAX &&
                sum != UINT32_MAX && difference != UINT32_MAX);
        /* The branch reads exactly what the comparison wrote, and each edge
         * names the first row of an arm rather than the row that follows. */
        REQUIRE(rows[branch].operand_slots[0] == rows[compare].result_slot);
        REQUIRE(XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(rows[branch].immediate_bits) ==
                sum);
        REQUIRE(XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(rows[branch].immediate_bits) ==
                difference);
        /* The relation's answer really did land in a truth slot: one byte with
         * an I1 representation, not the eight-byte signed slot every arithmetic
         * row writes. */
        uint32_t slot_count = 0;
        const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
        REQUIRE(slots != NULL && rows[compare].result_slot < slot_count);
        const XrTargetSlotRecord *truth = &slots[rows[compare].result_slot];
        REQUIRE(truth->size == 1 && truth->align == 1);
        REQUIRE(xr_target_plan_machine_rep(plan, truth->memory_rep)->kind ==
                XR_MACHINE_REP_I1);
        REQUIRE(slots[rows[sum].result_slot].size == 8);
        REQUIRE(xr_target_plan_function_execution_family_mask(plan, 0) ==
                XR_TARGET_EXECUTION_SCALAR_I64_CLOSED);

        XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
        for (size_t i = 0; i < 3; i++) {
            int64_t result = 0;
            REQUIRE(xr_typed_dispatch_execute_i64(plan, &fingerprint, 0, pairs[i],
                                                  2, &result) ==
                    XR_TYPED_DISPATCH_OK);
            REQUIRE(result == relations[relation].expected[i]);
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

        /* There is no immediate comparison form: an operand folded into the row
         * would be a value the def-use proof never saw. */
        memcpy(mutated, rows, row_bytes);
        mutated[compare].immediate_bits = 3;
        expect_rows_rejected(&mutable_source, mutated);

        /* A relation is binary; dropping the second operand may not leave a row
         * the executor would read an unwritten slot for. */
        memcpy(mutated, rows, row_bytes);
        mutated[compare].operand_count = 1;
        expect_rows_rejected(&mutable_source, mutated);

        memcpy(mutated, rows, row_bytes);
        mutated[compare].operand_slots[1] = XR_TARGET_INSTRUCTION_SLOT_NONE;
        expect_rows_rejected(&mutable_source, mutated);

        /* Both operands must be defined where the relation reads them, and the
         * arm's value is not defined on the path through the entry block. */
        memcpy(mutated, rows, row_bytes);
        mutated[compare].operand_slots[0] = rows[sum].result_slot;
        expect_rows_rejected(&mutable_source, mutated);

        /* The three rules the truth slot adds, each mutated on its own.
         * An arithmetic row may not write a truth slot. */
        memcpy(mutated, rows, row_bytes);
        mutated[compare].opcode = XR_TARGET_INSTRUCTION_ADD_WRAP_I64;
        expect_rows_rejected(&mutable_source, mutated);

        /* A relation may not write a signed i64 slot. */
        memcpy(mutated, rows, row_bytes);
        mutated[sum].opcode = XR_TARGET_INSTRUCTION_CMP_EQ_I64;
        expect_rows_rejected(&mutable_source, mutated);

        /* The branch that reads eight bytes may not take a truth slot as its
         * condition; which branch row it is fixes the width it reads. */
        memcpy(mutated, rows, row_bytes);
        mutated[branch].opcode = XR_TARGET_INSTRUCTION_BRANCH_IF_NONZERO_I64;
        expect_rows_rejected(&mutable_source, mutated);

        xr_target_plan_free(plan);
        xr_target_profile_free(profile);
        xr_semantic_plan_free(semantic);
    }
}

/* A verified program may loop forever, and no static proof could forbid that
 * without also forbidding ordinary loops. The executor's step budget is what
 * keeps such a program from hanging its caller. */
static void test_backward_jump_stops_at_the_step_budget(void) {
    TypedDispatchFixture fixture = make_fixture();
    XrTargetInstructionRecord rows[16];
    memcpy(rows, fixture.rows, sizeof(rows));
    uint32_t last = fixture.row_count - 1u;
    REQUIRE(rows[last].opcode == XR_TARGET_INSTRUCTION_RETURN_I64);
    rows[last] = (XrTargetInstructionRecord) {
        .id = fixture.rows[last].id,
        .function = fixture.rows[last].function,
        .result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE,
        .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE,
                          XR_TARGET_INSTRUCTION_SLOT_NONE},
        .immediate_bits = XR_TARGET_INSTRUCTION_TARGET_PACK(0, 0),
        .opcode = XR_TARGET_INSTRUCTION_JUMP,
        .operand_count = 0,
    };
    /* The verifier admits it: the single block is entered at its own first
     * row, every operand is defined before it is read, and a program with no
     * return is still a closed program. */
    XrTargetPlan *looping = NULL;
    char error[512] = {0};
    REQUIRE(freeze_with_rows(&fixture, rows, fixture.row_count, &looping, error,
                             sizeof(error)));
    XrFingerprint fingerprint = xr_target_plan_fingerprint(looping);
    int64_t result = 11;
    REQUIRE(xr_typed_dispatch_execute_i64(looping, &fingerprint, 0, NULL, 0,
                                          &result) ==
            XR_TYPED_DISPATCH_STEP_LIMIT_EXCEEDED);
    REQUIRE(result == 0);
    xr_target_plan_free(looping);
    dispose_fixture(&fixture);
}

int main(void) {
    test_generated_instruction_contract();
    test_closed_program_and_unavailable_boundary();
    test_production_builder_keeps_unsupported_function_unavailable();
    test_parameters_reach_the_executed_program();
    test_xtp_exact_roundtrip_executes_same_program();
    test_instruction_mutations_fail_closed();
    test_shift_rows_execute_with_masked_counts();
    test_division_rows_take_the_zero_divisor_edge();
    test_conditional_branch_selects_its_edge();
    test_unconditional_jumps_chain_blocks();
    test_comparison_rows_drive_the_branch();
    test_backward_jump_stops_at_the_step_budget();
    puts("typed dispatch tests passed");
    return 0;
}
