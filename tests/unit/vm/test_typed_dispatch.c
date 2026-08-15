/*
 * test_typed_dispatch.c - Closed typed TargetPlan scalar program execution
 */

#include "../../../src/base/xmalloc.h"
#include "../../../src/ir/xi.h"
#include "../../../src/plan/format/xr_xtp_internal.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_instruction_verify.h"
#include "../../../src/plan/target/xr_target_plan_internal.h"
#include "../../../src/plan/target/xr_target_verify.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/vm/debug/xr_vm_debug_control.h"
#include "../../../src/vm/debug/xr_vm_debug_control_internal.h"
#include "../../../src/vm/debug/xr_vm_materialize.h"
#include "../../../src/vm/debug/xr_vm_profile.h"
#include "../../../src/vm/xr_typed_dispatch.h"
#include "../../../src/vm/xr_typed_frame.h"
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
static XrType stub_function = {
    .kind = XR_KIND_FUNCTION,
    .id = 3,
    .frozen = true,
    .function = {.return_type = &stub_int,
                 .throw_effect = XR_FN_EFFECT_NO_THROW},
};
/* The type a comparison answers. It is not an integer with a restricted range:
 * the plan lays a `bool` value out as a one-byte I1 slot, which is exactly why
 * the executable family needs a truth slot beside its signed i64 one. */
static XrType stub_bool = {.kind = XR_KIND_BOOL,
                           .id = 2,
                           .frozen = true,
                           .scalar_rep = XR_SCALAR_REP_NONE};

static XrTypedDispatchStatus execute_request_i64(
    const XrTargetPlan *plan, const XrFingerprint *fingerprint,
    uint32_t function, const int64_t *arguments, uint32_t argument_count,
    int64_t *result) {
    REQUIRE(result != NULL);
    int64_t switch_result = *result;
    int64_t table_result = *result;
    XrTypedDispatchI64Request switch_request = {
        .verified_plan = plan,
        .required_plan_fingerprint = fingerprint,
        .arguments = arguments,
        .result = &switch_result,
        .provider = XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        .function = function,
        .argument_count = argument_count,
    };
    XrTypedDispatchI64Request table_request = switch_request;
    table_request.result = &table_result;
    table_request.provider =
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE;
    XrTypedDispatchStatus switch_status =
        xr_typed_dispatch_execute_i64(&switch_request);
    XrTypedDispatchStatus table_status =
        xr_typed_dispatch_execute_i64(&table_request);
    REQUIRE(table_status == switch_status);
    REQUIRE(table_result == switch_result);
    *result = switch_result;
    return switch_status;
}

static XrTypedDispatchStatus execute_debug_request_i64(
    const XrTargetPlan *plan, const XrFingerprint *fingerprint,
    uint32_t function, const int64_t *arguments, uint32_t argument_count,
    const XrModuleGenerationIdentity *generation_identity,
    const XrVmDebugSession *debug_session, XrTypedDispatchProvider provider,
    int64_t *result) {
    XrTypedDispatchI64Request request = {
        .verified_plan = plan,
        .required_plan_fingerprint = fingerprint,
        .arguments = arguments,
        .result = result,
        .debug_session = debug_session,
        .generation_identity = generation_identity,
        .provider = provider,
        .function = function,
        .argument_count = argument_count,
    };
    return xr_typed_dispatch_execute_i64(&request);
}

static XrModuleGenerationIdentity debug_generation_identity(
    const XrTargetPlan *plan, uint8_t generation_byte) {
    XrModuleGenerationIdentity identity = {
        .schema_version = XR_RUNTIME_GENERATION_SCHEMA_VERSION,
        .target_plan_schema_version = XR_TYPED_FRAME_SUPPORTED_PLAN_SCHEMA_VERSION,
        .generation_number = 7,
        .completed_family_mask = XR_TARGET_REQUIRED_FAMILIES,
    };
    XrFingerprint semantic = xr_target_plan_semantic_fingerprint(plan);
    XrFingerprint profile =
        xr_target_profile_fingerprint(xr_target_plan_profile(plan));
    XrFingerprint target = xr_target_plan_fingerprint(plan);
    memcpy(identity.semantic_fingerprint, semantic.bytes,
           sizeof(identity.semantic_fingerprint));
    memcpy(identity.target_profile_fingerprint, profile.bytes,
           sizeof(identity.target_profile_fingerprint));
    memcpy(identity.target_plan_fingerprint, target.bytes,
           sizeof(identity.target_plan_fingerprint));
    memset(identity.generation_fingerprint, generation_byte,
           sizeof(identity.generation_fingerprint));
    return identity;
}

static void test_generated_instruction_contract(void) {
    enum {
        XR_TEST_VM_DISPATCH_COUNT = 0
#define XR_VM_OP(symbol, handler, kind, argument) +1
#include "../../../src/vm/xr_vm_ops.def"
#undef XR_VM_OP
    };
    REQUIRE(XR_TARGET_INSTRUCTION_CONST_I64 == 1);
    REQUIRE(XR_TARGET_INSTRUCTION_BRANCH_IF_TRUE_BOOL == 25);
    REQUIRE(XR_TARGET_INSTRUCTION_CALL_DIRECT_I64 == 26);
    REQUIRE(XR_TARGET_INSTRUCTION_CALL_ENTRY_I64 == 27);
    REQUIRE(XR_TARGET_INSTRUCTION_CONTRACT_COUNT == 27u);
    REQUIRE(XR_TEST_VM_DISPATCH_COUNT ==
            XR_TARGET_INSTRUCTION_CONTRACT_COUNT);
    static const XrTypedDispatchProvider providers[] = {
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
    };
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
        for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
            REQUIRE(xr_typed_dispatch_provider_contract_is_exact(
                providers[i], opcode, contract));
            XrTargetInstructionContract mismatch = *contract;
            mismatch.dispatch_argument = UINT8_MAX;
            REQUIRE(!xr_typed_dispatch_provider_contract_is_exact(
                providers[i], opcode, &mismatch));
            mismatch = *contract;
            mismatch.name = "mismatched.contract";
            REQUIRE(!xr_typed_dispatch_provider_contract_is_exact(
                providers[i], opcode, &mismatch));
        }
        semantic_bindings += contract->semantic_name != NULL;
        for (uint32_t other = 1; other < opcode; other++)
            REQUIRE(strcmp(contract->name,
                           xr_target_instruction_opcode_name(other)) != 0);
    }
    REQUIRE(semantic_bindings == 23u);
    REQUIRE(xr_target_instruction_contract(XR_TARGET_INSTRUCTION_INVALID) ==
            NULL);
    REQUIRE(xr_target_instruction_contract(XR_TARGET_INSTRUCTION_COUNT) ==
            NULL);
    const XrTargetInstructionContract *first =
        xr_target_instruction_contract(XR_TARGET_INSTRUCTION_CONST_I64);
    REQUIRE(!xr_typed_dispatch_provider_contract_is_exact(
        XR_TYPED_DISPATCH_PROVIDER_INVALID, XR_TARGET_INSTRUCTION_CONST_I64,
        first));
    REQUIRE(!xr_typed_dispatch_provider_contract_is_exact(
        XR_TYPED_DISPATCH_PROVIDER_COUNT, XR_TARGET_INSTRUCTION_CONST_I64,
        first));
    REQUIRE(!xr_typed_dispatch_provider_contract_is_exact(
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        XR_TARGET_INSTRUCTION_INVALID, first));
    REQUIRE(!xr_typed_dispatch_provider_contract_is_exact(
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
        XR_TARGET_INSTRUCTION_COUNT, first));
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
    function->source_file = "typed_dispatch_probe.xr";
    XiValue *values[] = {left, right, copy, add, sub, mul};
    for (uint32_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        values[i]->source_span = (XiSourceSpan) {
            .start_line = i + 1u,
            .start_column = 1u,
            .end_line = i + 1u,
            .end_column = 8u,
        };
    }
    xi_block_set_return(entry, add);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error, sizeof(error)));
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_direct_i64_call_semantic(bool divide_by_zero) {
    XiFunc *root = xi_func_new("typed_dispatch_call_root", &stub_int);
    XiFunc *child = xi_func_new("typed_dispatch_call_child", &stub_int);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);
    child->nparams = child->min_params = 1;
    child->params = (XiValue **) xr_calloc(1, sizeof(*child->params));
    REQUIRE(child->params != NULL);
    child->params[0] = xi_param(child, child_entry, 0, &stub_int);
    REQUIRE(child->params[0] != NULL);
    XiValue *child_result = NULL;
    if (divide_by_zero) {
        XiValue *zero = xi_const_int(child, child_entry, 0, &stub_int);
        child_result = xi_value_new(child, child_entry, XI_DIV, &stub_int, 2);
        REQUIRE(zero != NULL && child_result != NULL);
        child_result->args[0] = child->params[0];
        child_result->args[1] = zero;
    } else {
        child_result = xi_value_new(child, child_entry, XI_ADD, &stub_int, 2);
        REQUIRE(child_result != NULL);
        child_result->args[0] = child->params[0];
        child_result->args[1] = child->params[0];
    }
    xi_block_set_return(child_entry, child_result);

    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;
    XiValue *closure =
        xi_value_new(root, root_entry, XI_STACK_ALLOC, &stub_function, 0);
    XiValue *alias = xi_value_new(root, root_entry, XI_COPY, &stub_function, 1);
    XiValue *argument =
        xi_const_int(root, root_entry, divide_by_zero ? 7 : 21, &stub_int);
    XiValue *call = xi_value_new(root, root_entry, XI_CALL, &stub_int, 2);
    REQUIRE(closure != NULL && alias != NULL && argument != NULL && call != NULL);
    closure->aux_int = XI_CLOSURE_NEW;
    closure->aux = child;
    alias->args[0] = closure;
    alias->aux_int = XI_COPY_KIND_IDENTITY;
    call->args[0] = alias;
    call->args[1] = argument;
    xi_block_set_return(root_entry, call);
    root->stage = child->stage = XI_STAGE_OPTIMIZED;

    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(root, &semantic, error, sizeof(error)));
    xi_func_free(root);
    return semantic;
}

static XrSemanticPlan *build_deep_direct_i64_call_semantic(void) {
    const uint32_t function_count = XR_TYPED_DISPATCH_MAX_CALL_DEPTH + 1u;
    XiFunc **functions =
        (XiFunc **) xr_calloc(function_count, sizeof(*functions));
    XiBlock **blocks =
        (XiBlock **) xr_calloc(function_count, sizeof(*blocks));
    REQUIRE(functions != NULL && blocks != NULL);
    for (uint32_t i = 0; i < function_count; i++) {
        functions[i] = xi_func_new("typed_dispatch_deep_call", &stub_int);
        REQUIRE(functions[i] != NULL);
        blocks[i] = xi_block_new(functions[i]);
        REQUIRE(blocks[i] != NULL);
        if (i != 0) {
            functions[i]->nparams = functions[i]->min_params = 1;
            functions[i]->params =
                (XiValue **) xr_calloc(1, sizeof(*functions[i]->params));
            REQUIRE(functions[i]->params != NULL);
            functions[i]->params[0] =
                xi_param(functions[i], blocks[i], 0, &stub_int);
            REQUIRE(functions[i]->params[0] != NULL);
        }
        functions[i]->stage = XI_STAGE_OPTIMIZED;
    }
    for (uint32_t i = 0; i + 1u < function_count; i++) {
        functions[i]->children =
            (XiFunc **) xr_calloc(1, sizeof(*functions[i]->children));
        REQUIRE(functions[i]->children != NULL);
        functions[i]->children[0] = functions[i + 1u];
        functions[i]->nchildren = functions[i]->children_cap = 1;
        functions[i + 1u]->parent_func = functions[i];
        XiValue *closure = xi_value_new(functions[i], blocks[i],
                                        XI_STACK_ALLOC, &stub_function, 0);
        XiValue *argument = i == 0
                                ? xi_const_int(functions[i], blocks[i], 1,
                                               &stub_int)
                                : functions[i]->params[0];
        XiValue *call =
            xi_value_new(functions[i], blocks[i], XI_CALL, &stub_int, 2);
        REQUIRE(closure != NULL && argument != NULL && call != NULL);
        closure->aux_int = XI_CLOSURE_NEW;
        closure->aux = functions[i + 1u];
        call->args[0] = closure;
        call->args[1] = argument;
        xi_block_set_return(blocks[i], call);
    }
    xi_block_set_return(blocks[function_count - 1u],
                        functions[function_count - 1u]->params[0]);
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(functions[0], &semantic, error,
                                   sizeof(error)));
    xi_func_free(functions[0]);
    xr_free(blocks);
    xr_free(functions);
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

static XrSemanticPlan *build_scalar_with_unused_child_semantic(void) {
    XiFunc *root = xi_func_new("typed_dispatch_partition_root", &stub_int);
    XiFunc *child = xi_func_new("typed_dispatch_partition_child", &stub_int);
    REQUIRE(root && child);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry && child_entry);
    XiValue *root_result = xi_const_int(root, root_entry, 42, &stub_int);
    XiValue *left = xi_const_int(child, child_entry, 8, &stub_int);
    XiValue *right = xi_const_int(child, child_entry, 2, &stub_int);
    XiValue *rotate = xi_value_new(child, child_entry, XI_BIT_ROTL,
                                   &stub_int, 2);
    REQUIRE(root_result && left && right && rotate);
    rotate->args[0] = left;
    rotate->args[1] = right;
    xi_block_set_return(root_entry, root_result);
    xi_block_set_return(child_entry, rotate);
    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;
    root->stage = child->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(root, &semantic, error, sizeof(error)));
    xi_func_free(root);
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
    COPY_TABLE(entry_expectations);
    COPY_TABLE(debug_facts);
#undef COPY_TABLE
    XrTargetDebugFactRecord debug_facts[16];
    REQUIRE(row_count <= sizeof(debug_facts) / sizeof(debug_facts[0]));
    draft.debug_facts_count = row_count;
    if (row_count) {
        for (uint32_t i = 0; i < row_count; i++) {
            REQUIRE(rows[i].id < fixture->program_plan->debug_facts_count);
            debug_facts[i] = fixture->program_plan->debug_facts[rows[i].id];
        }
        for (uint32_t i = 0; i < row_count; i++) {
            if (rows && rows[i].opcode == XR_TARGET_INSTRUCTION_JUMP &&
                fixture->rows[i].opcode != XR_TARGET_INSTRUCTION_JUMP) {
                debug_facts[i] = (XrTargetDebugFactRecord) {
                    .id = i,
                    .instruction = i,
                    .function = rows[i].function,
                    .semantic_operation = XR_SEMANTIC_INDEX_NONE,
                    .coroutine_state = XR_SEMANTIC_INDEX_NONE,
                };
            }
        }
        draft.debug_facts = debug_facts;
    }
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
    REQUIRE(execute_request_i64(fixture.base_plan, &base_fingerprint,
                                          0, NULL, 0, &result) ==
            XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE);
    REQUIRE(result == 0);

    XrFingerprint fingerprint =
        xr_target_plan_fingerprint(fixture.program_plan);
    REQUIRE(xr_target_plan_function_execution_family_mask(fixture.program_plan, 0) ==
            XR_TARGET_EXECUTION_SCALAR_I64_CLOSED);
    result = 7;
    XrTypedDispatchI64Request invalid_provider = {
        .verified_plan = fixture.program_plan,
        .required_plan_fingerprint = &fingerprint,
        .result = &result,
        .provider = XR_TYPED_DISPATCH_PROVIDER_INVALID,
    };
    REQUIRE(xr_typed_dispatch_execute_i64(&invalid_provider) ==
            XR_TYPED_DISPATCH_INVALID_ARGUMENT);
    REQUIRE(result == 0);
    REQUIRE(execute_request_i64(fixture.program_plan, &fingerprint,
                                0, NULL, 0, &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == INT64_MIN);

    REQUIRE(fixture.program_plan->root_maps_count == 0 &&
            fixture.program_plan->root_slots_count == 0);
    fixture.program_plan->root_maps =
        (XrTargetRootMapRecord *) xr_calloc(
            1, sizeof(*fixture.program_plan->root_maps));
    fixture.program_plan->root_slots =
        (uint32_t *) xr_calloc(1, sizeof(*fixture.program_plan->root_slots));
    REQUIRE(fixture.program_plan->root_maps &&
            fixture.program_plan->root_slots);
    fixture.program_plan->root_maps_count = 1;
    fixture.program_plan->root_slots_count = 1;
    fixture.program_plan->functions[0].root_begin = 0;
    fixture.program_plan->functions[0].root_count = 1;
    fixture.program_plan->root_maps[0] = (XrTargetRootMapRecord) {
        .id = 0,
        .function = 0,
        .semantic_operation = 0,
        .slot_begin = 0,
        .slot_count = 1,
        .flags = XR_TARGET_ROOT_SUSPEND | XR_TARGET_ROOT_CANCEL |
                 XR_TARGET_ROOT_EXIT,
    };
    fixture.program_plan->root_slots[0] =
        fixture.program_plan->functions[0].slot_begin;
    xr_target_plan_compute_fingerprint(fixture.program_plan,
                                       &fixture.program_plan->fingerprint);
    fingerprint = xr_target_plan_fingerprint(fixture.program_plan);
    result = 7;
    REQUIRE(execute_request_i64(fixture.program_plan, &fingerprint,
                                0, NULL, 0, &result) ==
            XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE);
    REQUIRE(result == 0);
    xr_free(fixture.program_plan->root_maps);
    xr_free(fixture.program_plan->root_slots);
    fixture.program_plan->root_maps = NULL;
    fixture.program_plan->root_slots = NULL;
    fixture.program_plan->root_maps_count = 0;
    fixture.program_plan->root_slots_count = 0;
    fixture.program_plan->functions[0].root_begin = 0;
    fixture.program_plan->functions[0].root_count = 0;
    xr_target_plan_compute_fingerprint(fixture.program_plan,
                                       &fixture.program_plan->fingerprint);
    fingerprint = xr_target_plan_fingerprint(fixture.program_plan);

    XrSemanticPlan *retained_semantic = fixture.program_plan->semantic_plan;
    fixture.program_plan->semantic_plan = NULL;
    REQUIRE(execute_request_i64(fixture.program_plan, &fingerprint,
                                          0, NULL, 0,
                                          &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == INT64_MIN);
    fixture.program_plan->semantic_plan = retained_semantic;

    fixture.program_plan->instructions[0].immediate_bits ^= UINT64_C(1);
    REQUIRE(execute_request_i64(fixture.program_plan, &fingerprint,
                                          0, NULL, 0, &result) ==
            XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED);
    fixture.program_plan->instructions[0].immediate_bits ^= UINT64_C(1);

    fingerprint.bytes[0] ^= 1;
    REQUIRE(execute_request_i64(fixture.program_plan, &fingerprint,
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
    REQUIRE(execute_request_i64(plan, &fingerprint, 0, NULL, 0,
                                          &result) ==
            XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE);
    REQUIRE(result == 0);
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_other_function_lifecycle_partitions_do_not_scan_selected_scalar(void) {
    enum { OTHER_LIFECYCLE_ROWS = 8192 };
    XrSemanticPlan *semantic = build_scalar_with_unused_child_semantic();
    XrTargetProfile *profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error,
                                 sizeof(error)));
    REQUIRE(plan->functions_count == 2 && plan->root_maps_count == 0 &&
            plan->cleanups_count == 0 && plan->coroutines_count == 0 &&
            xr_target_plan_function_execution_family_mask(plan, 0) ==
                XR_TARGET_EXECUTION_SCALAR_I64_CLOSED &&
            xr_target_plan_function_execution_family_mask(plan, 1) == 0);
    plan->root_maps = (XrTargetRootMapRecord *) xr_calloc(
        OTHER_LIFECYCLE_ROWS, sizeof(*plan->root_maps));
    plan->cleanups = (XrTargetCleanupRecord *) xr_calloc(
        OTHER_LIFECYCLE_ROWS, sizeof(*plan->cleanups));
    plan->coroutines = (XrTargetCoroutineStateRecord *) xr_calloc(
        OTHER_LIFECYCLE_ROWS, sizeof(*plan->coroutines));
    REQUIRE(plan->root_maps && plan->cleanups && plan->coroutines);
    plan->root_maps_count = OTHER_LIFECYCLE_ROWS;
    plan->cleanups_count = OTHER_LIFECYCLE_ROWS;
    plan->coroutines_count = OTHER_LIFECYCLE_ROWS;
    plan->functions[1].root_begin = 0;
    plan->functions[1].root_count = OTHER_LIFECYCLE_ROWS;
    plan->functions[1].cleanup_begin = 0;
    plan->functions[1].cleanup_count = OTHER_LIFECYCLE_ROWS;
    plan->functions[1].coroutine_begin = 0;
    plan->functions[1].coroutine_count = OTHER_LIFECYCLE_ROWS;
    for (uint32_t i = 0; i < OTHER_LIFECYCLE_ROWS; i++) {
        plan->root_maps[i].id = i;
        plan->root_maps[i].function = 1;
        plan->cleanups[i].id = i;
        plan->cleanups[i].function = 1;
        plan->coroutines[i].id = i;
        plan->coroutines[i].function = 1;
    }
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    int64_t result = 0;
    REQUIRE(execute_request_i64(plan, &fingerprint, 0, NULL, 0, &result) ==
                XR_TYPED_DISPATCH_OK &&
            result == 42);
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
    REQUIRE(execute_request_i64(plan, &fingerprint, 0, arguments, 2,
                                          &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == (arguments[0] - arguments[1]) * arguments[0]);

    /* A different argument vector must produce a different result, so the
     * value cannot be coming from a constant folded into the plan. */
    const int64_t swapped[2] = {arguments[1], arguments[0]};
    REQUIRE(execute_request_i64(plan, &fingerprint, 0, swapped, 2,
                                          &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == (swapped[0] - swapped[1]) * swapped[0]);

    /* Argument-count disagreement is refused instead of truncated or zero
     * filled, and the result stays untouched. */
    result = 11;
    REQUIRE(execute_request_i64(plan, &fingerprint, 0, arguments, 1,
                                          &result) ==
            XR_TYPED_DISPATCH_ARGUMENT_MISMATCH);
    REQUIRE(result == 0);
    REQUIRE(execute_request_i64(plan, &fingerprint, 0, arguments, 3,
                                          &result) ==
            XR_TYPED_DISPATCH_ARGUMENT_MISMATCH);
    REQUIRE(execute_request_i64(plan, &fingerprint, 0, NULL, 0,
                                          &result) ==
            XR_TYPED_DISPATCH_ARGUMENT_MISMATCH);
    REQUIRE(execute_request_i64(plan, &fingerprint, 0, NULL, 2,
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
        uint16_t opcode;
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
    REQUIRE(execute_request_i64(decoded, &fingerprint, 0, NULL, 0,
                                          &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == INT64_MIN);
    xr_target_plan_free(decoded);
    xr_xtp_candidate_release(candidate);
    xr_xtp_encoded_free(bytes);
    dispose_fixture(&fixture);
}

static void expect_rows_rejected_with_code(TypedDispatchFixture *fixture,
                                           XrTargetInstructionRecord *rows,
                                           const char *code) {
    XrTargetPlan *plan = (XrTargetPlan *) (uintptr_t) 1;
    char error[512] = {0};
    REQUIRE(!freeze_with_rows(fixture, rows, fixture->row_count, &plan, error,
                              sizeof(error)));
    REQUIRE(plan == NULL);
    REQUIRE(strncmp(error, code, strlen(code)) == 0);
}

static void expect_rows_rejected(TypedDispatchFixture *fixture,
                                 XrTargetInstructionRecord *rows) {
    expect_rows_rejected_with_code(fixture, rows, "XR_TARGET_1005");
}

static void test_instruction_mutations_fail_closed(void) {
    TypedDispatchFixture fixture = make_fixture();
    XrTargetInstructionRecord rows[16];
    memcpy(rows, fixture.rows, sizeof(rows));
    rows[0].opcode = UINT16_MAX;
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
        REQUIRE(execute_request_i64(plan, &fingerprint, 0, arguments,
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
        uint16_t opcode;
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
            REQUIRE(execute_request_i64(plan, &fingerprint, 0,
                                                  arguments, 2, &result) ==
                    XR_TYPED_DISPATCH_OK);
            REQUIRE(result == families[family].cases[i].expected);
        }

        /* The verifier admits the row, so the executor is what stops a zero
         * divisor, in either operand position, and it leaves no result. */
        const int64_t divisor_zero[2] = {17, 0};
        const int64_t dividend_zero[2] = {0, 17};
        int64_t result = 11;
        REQUIRE(execute_request_i64(plan, &fingerprint, 0,
                                              divisor_zero, 2, &result) ==
                families[family].zero_status);
        REQUIRE(result == 0);
        result = 11;
        REQUIRE(execute_request_i64(plan, &fingerprint, 0,
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
                              uint32_t row_count, uint16_t opcode,
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
        REQUIRE(execute_request_i64(plan, &fingerprint, 0, arguments,
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
    REQUIRE(execute_request_i64(plan, &fingerprint, 0, arguments, 2,
                                          &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == ((arguments[0] - arguments[1]) * arguments[0]) - arguments[1]);
    const int64_t swapped[2] = {arguments[1], arguments[0]};
    REQUIRE(execute_request_i64(plan, &fingerprint, 0, swapped, 2,
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
        uint16_t opcode;
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
            REQUIRE(execute_request_i64(plan, &fingerprint, 0, pairs[i],
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

static void test_direct_local_i64_call_executes_and_rejects_drift(void) {
    XrSemanticPlan *semantic = build_direct_i64_call_semantic(false);
    XrTargetProfile *profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error,
                                 sizeof(error)));
    uint32_t root_count = 0;
    uint32_t child_count = 0;
    const XrTargetInstructionRecord *root =
        xr_target_plan_function_instructions(plan, 0, &root_count);
    const XrTargetInstructionRecord *child =
        xr_target_plan_function_instructions(plan, 1, &child_count);
    REQUIRE(root != NULL && root_count == 3u);
    REQUIRE(child != NULL && child_count == 3u);
    REQUIRE(root[0].opcode == XR_TARGET_INSTRUCTION_CONST_I64);
    REQUIRE(root[1].opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_I64 &&
            root[1].immediate_bits == 0);
    REQUIRE(root[2].opcode == XR_TARGET_INSTRUCTION_RETURN_I64);
    REQUIRE(child[0].opcode == XR_TARGET_INSTRUCTION_PARAM_I64 &&
            child[1].opcode == XR_TARGET_INSTRUCTION_ADD_WRAP_I64 &&
            child[2].opcode == XR_TARGET_INSTRUCTION_RETURN_I64);

    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    int64_t result = 0;
    REQUIRE(execute_request_i64(plan, &fingerprint, 0, NULL, 0,
                                          &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == 42);

    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *all =
        xr_target_plan_instructions(plan, &instruction_count);
    REQUIRE(all != NULL && instruction_count == root_count + child_count);
    TypedDispatchFixture source = {.semantic = semantic,
                                   .profile = profile,
                                   .program_plan = plan,
                                   .row_count = instruction_count};
    XrTargetInstructionRecord mutated[16];
    REQUIRE(instruction_count <= sizeof(mutated) / sizeof(mutated[0]));
    memcpy(mutated, all, (size_t) instruction_count * sizeof(*all));
    mutated[1].immediate_bits = UINT32_MAX;
    expect_rows_rejected_with_code(&source, mutated, "XR_TARGET_1003");

    memcpy(mutated, all, (size_t) instruction_count * sizeof(*all));
    mutated[1].result_slot = mutated[0].result_slot;
    expect_rows_rejected_with_code(&source, mutated, "XR_TARGET_1003");

    /* Removing the callee group cannot leave an executable call whose target
     * is merely present in the function table. */
    REQUIRE(!freeze_with_rows(&source, all, root_count, NULL, error,
                              sizeof(error)));

    XrTargetCallRecord saved_call = plan->calls[0];
    XrTargetCallArgumentRecord saved_argument = plan->call_arguments[0];
    plan->calls[0].flags = XR_TARGET_CALL_SUSPEND;
    REQUIRE(!xr_target_instruction_program_verify(plan, error, sizeof(error)));
    plan->calls[0] = saved_call;
    plan->call_arguments[0].ownership = XR_TARGET_CALL_MOVE;
    REQUIRE(!xr_target_instruction_program_verify(plan, error, sizeof(error)));
    plan->call_arguments[0] = saved_argument;
    plan->call_arguments[0].caller_slot = plan->calls[0].result_slot;
    REQUIRE(!xr_target_instruction_program_verify(plan, error, sizeof(error)));
    plan->call_arguments[0] = saved_argument;
    REQUIRE(xr_target_instruction_program_verify(plan, error, sizeof(error)));

    /* A loop inside the child consumes the same entry-call budget as its
     * caller. If recursion accidentally reset the budget, this execution would
     * never return. */
    memcpy(mutated, all, (size_t) instruction_count * sizeof(*all));
    uint32_t child_last = instruction_count - 1u;
    REQUIRE(mutated[child_last].function == 1 &&
            mutated[child_last].opcode == XR_TARGET_INSTRUCTION_RETURN_I64);
    mutated[child_last] = (XrTargetInstructionRecord) {
        .id = child_last,
        .function = 1,
        .result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE,
        .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE,
                          XR_TARGET_INSTRUCTION_SLOT_NONE},
        .immediate_bits = XR_TARGET_INSTRUCTION_TARGET_PACK(0, 0),
        .opcode = XR_TARGET_INSTRUCTION_JUMP,
    };
    XrTargetPlan *nested_loop = NULL;
    REQUIRE(freeze_with_rows(&source, mutated, instruction_count, &nested_loop,
                             error, sizeof(error)));
    XrFingerprint loop_fingerprint =
        xr_target_plan_fingerprint(nested_loop);
    result = 17;
    REQUIRE(execute_request_i64(nested_loop, &loop_fingerprint, 0,
                                          NULL, 0, &result) ==
            XR_TYPED_DISPATCH_STEP_LIMIT_EXCEEDED);
    REQUIRE(result == 0);
    xr_target_plan_free(nested_loop);

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);

    semantic = build_direct_i64_call_semantic(true);
    profile = xr_test_target_profile_build(false,
                                           XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    plan = NULL;
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error,
                                 sizeof(error)));
    fingerprint = xr_target_plan_fingerprint(plan);
    for (uint32_t attempt = 0; attempt < 2u; attempt++) {
        result = 99;
        REQUIRE(execute_request_i64(plan, &fingerprint, 0, NULL, 0,
                                              &result) ==
                XR_TYPED_DISPATCH_DIVIDE_BY_ZERO);
        REQUIRE(result == 0);
    }
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_runtime_only_trace_profile_and_materialization(void) {
    XrSemanticPlan *semantic = build_direct_i64_call_semantic(false);
    XrTargetProfile *profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error,
                                 sizeof(error)));
    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    XrModuleGenerationIdentity generation = debug_generation_identity(plan, UINT8_C(0xa5));

    XrVmTraceEvent first_storage[32];
    XrVmTraceEvent second_storage[32];
    XrVmTraceBuffer first;
    XrVmTraceBuffer second;
    XrVmProfile first_profile;
    XrVmProfile second_profile;
    REQUIRE(xr_typed_trace_buffer_init(&first, first_storage, 32));
    REQUIRE(xr_typed_trace_buffer_init(&second, second_storage, 32));
    REQUIRE(xr_typed_profile_init(&first_profile));
    REQUIRE(xr_typed_profile_init(&second_profile));
    XrVmTraceSink first_sink = xr_typed_trace_buffer_sink(&first);
    XrVmTraceSink second_sink = xr_typed_trace_buffer_sink(&second);
    XrVmDebugSession *first_session = NULL;
    XrVmDebugSession *second_session = NULL;
    REQUIRE(xr_typed_debug_session_create(&fingerprint, &generation, &first_sink, &first_profile,
                                          NULL, &first_session) == XR_VM_DEBUG_SESSION_OK);
    REQUIRE(xr_typed_debug_session_create(&fingerprint, &generation, &second_sink, &second_profile,
                                          NULL, &second_session) == XR_VM_DEBUG_SESSION_OK);
    int64_t first_result = 0;
    int64_t second_result = 0;
    REQUIRE(execute_debug_request_i64(plan, &fingerprint, 0, NULL, 0, &generation, first_session,
                                      XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
                                      &first_result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(execute_debug_request_i64(plan, &fingerprint, 0, NULL, 0, &generation, second_session,
                                      XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
                                      &second_result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(first_result == 42 && second_result == 42);
    REQUIRE(first.count == 14 && second.count == first.count);
    REQUIRE(memcmp(first.events, second.events, first.count * sizeof(*first.events)) == 0);
    for (size_t i = 0; i < first.count; i++) {
        REQUIRE(first.events[i].ordinal == i);
        REQUIRE(first.events[i].generation_identity_present == 1);
        REQUIRE(first.events[i].generation_number == 7);
        REQUIRE(xr_fingerprint_equal(first.events[i].target_plan_fingerprint, fingerprint));
    }
    REQUIRE(first.events[0].kind == XR_VM_TRACE_FRAME_ENTER && first.events[0].frame == 0 &&
            first.events[0].parent_frame == XR_VM_TRACE_ID_NONE);
    REQUIRE(first.events[4].kind == XR_VM_TRACE_CALL_ENTER && first.events[4].frame == 0 &&
            first.events[4].related_frame == 1);
    REQUIRE(first.events[5].kind == XR_VM_TRACE_FRAME_ENTER && first.events[5].frame == 1 &&
            first.events[5].parent_frame == 0);
    REQUIRE(first.events[11].kind == XR_VM_TRACE_CALL_RETURN &&
            first.events[11].status == XR_TYPED_DISPATCH_OK);
    REQUIRE(first.events[13].kind == XR_VM_TRACE_FRAME_EXIT &&
            first.events[13].status == XR_TYPED_DISPATCH_OK);

    XrVmProfileSnapshot snapshot;
    XrVmProfileSnapshot second_snapshot;
    REQUIRE(xr_typed_profile_snapshot(&first_profile, &snapshot));
    REQUIRE(xr_typed_profile_snapshot(&second_profile, &second_snapshot));
    REQUIRE(memcmp(&snapshot, &second_snapshot, sizeof(snapshot)) == 0);
    REQUIRE(snapshot.saturated == 0 && snapshot.max_frame_depth == 1);
    REQUIRE(snapshot.event_counts[XR_VM_TRACE_FRAME_ENTER] == 2);
    REQUIRE(snapshot.event_counts[XR_VM_TRACE_INSTRUCTION] == 6);
    REQUIRE(snapshot.event_counts[XR_VM_TRACE_CALL_ENTER] == 1);
    REQUIRE(snapshot.event_counts[XR_VM_TRACE_CALL_RETURN] == 1);
    REQUIRE(snapshot.opcode_counts[XR_TARGET_INSTRUCTION_CALL_DIRECT_I64] == 1);

    XrVmMaterializedEvent materialized;
    REQUIRE(xr_typed_materialize_event(plan, &fingerprint, &first.events[4], &materialized) ==
            XR_VM_MATERIALIZE_OK);
    REQUIRE(materialized.function_identity == XR_VM_DEBUG_FACT_AVAILABLE);
    REQUIRE(materialized.generation_identity == XR_VM_DEBUG_FACT_AVAILABLE);
    REQUIRE(materialized.call_identity == XR_VM_DEBUG_FACT_AVAILABLE);
    REQUIRE(materialized.result.availability == XR_VM_DEBUG_FACT_AVAILABLE);
    REQUIRE(materialized.source_span == XR_VM_DEBUG_FACT_CONTEXT_UNAVAILABLE);
    REQUIRE(materialized.owner_identity == XR_VM_DEBUG_FACT_NOT_APPLICABLE);
    REQUIRE(materialized.layout_identity == XR_VM_DEBUG_FACT_AVAILABLE);

    int64_t unobserved_result = 0;
    REQUIRE(execute_request_i64(plan, &fingerprint, 0, NULL, 0, &unobserved_result) ==
            XR_TYPED_DISPATCH_OK);
    REQUIRE(unobserved_result == first_result);

    XrVmTraceEvent short_storage[4];
    XrVmTraceBuffer short_buffer;
    XrVmProfile short_profile;
    REQUIRE(xr_typed_trace_buffer_init(&short_buffer, short_storage, 4));
    REQUIRE(xr_typed_profile_init(&short_profile));
    XrVmTraceSink short_sink = xr_typed_trace_buffer_sink(&short_buffer);
    XrVmDebugSession *short_session = NULL;
    REQUIRE(xr_typed_debug_session_create(&fingerprint, &generation, &short_sink, &short_profile,
                                          NULL, &short_session) == XR_VM_DEBUG_SESSION_OK);
    int64_t rejected_result = 99;
    REQUIRE(execute_debug_request_i64(plan, &fingerprint, 0, NULL, 0, &generation, short_session,
                                      XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
                                      &rejected_result) == XR_TYPED_DISPATCH_TRACE_REJECTED);
    REQUIRE(rejected_result == 0 && short_buffer.count == 4 && short_buffer.capacity_exceeded);
    REQUIRE(xr_typed_profile_snapshot(&short_profile, &snapshot));
    uint64_t accepted_events = 0;
    for (uint32_t i = 0; i < XR_VM_TRACE_EVENT_KIND_COUNT; i++)
        accepted_events += snapshot.event_counts[i];
    REQUIRE(accepted_events == 4);

    xr_typed_debug_session_free(&short_session);
    xr_typed_debug_session_free(&second_session);
    xr_typed_debug_session_free(&first_session);
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);

    semantic = build_direct_i64_call_semantic(true);
    profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    plan = NULL;
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    fingerprint = xr_target_plan_fingerprint(plan);
    generation = debug_generation_identity(plan, UINT8_C(0x5a));
    XrVmTraceEvent error_storage[32];
    XrVmTraceBuffer error_buffer;
    REQUIRE(xr_typed_trace_buffer_init(&error_buffer, error_storage, 32));
    XrVmTraceSink error_sink = xr_typed_trace_buffer_sink(&error_buffer);
    XrVmDebugSession *error_session = NULL;
    REQUIRE(xr_typed_debug_session_create(&fingerprint, &generation, &error_sink, NULL, NULL,
                                          &error_session) == XR_VM_DEBUG_SESSION_OK);
    int64_t error_result = 88;
    REQUIRE(execute_debug_request_i64(plan, &fingerprint, 0, NULL, 0, &generation, error_session,
                                      XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
                                      &error_result) == XR_TYPED_DISPATCH_DIVIDE_BY_ZERO);
    REQUIRE(error_result == 0);
    size_t call_enter = SIZE_MAX;
    size_t child_error = SIZE_MAX;
    size_t call_return = SIZE_MAX;
    size_t root_error = SIZE_MAX;
    for (size_t i = 0; i < error_buffer.count; i++) {
        const XrVmTraceEvent *event = &error_buffer.events[i];
        if (event->kind == XR_VM_TRACE_CALL_ENTER)
            call_enter = i;
        else if (event->kind == XR_VM_TRACE_ERROR && event->frame == 1)
            child_error = i;
        else if (event->kind == XR_VM_TRACE_CALL_RETURN)
            call_return = i;
        else if (event->kind == XR_VM_TRACE_ERROR && event->frame == 0)
            root_error = i;
    }
    REQUIRE(call_enter < child_error && child_error < call_return && call_return < root_error);
    REQUIRE(error_buffer.events[call_return].status == XR_TYPED_DISPATCH_DIVIDE_BY_ZERO);
    REQUIRE(error_buffer.events[error_buffer.count - 1u].kind == XR_VM_TRACE_FRAME_EXIT &&
            error_buffer.events[error_buffer.count - 1u].status ==
                XR_TYPED_DISPATCH_DIVIDE_BY_ZERO);

    xr_typed_debug_session_free(&error_session);
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_trace_debug_facts_are_source_backed_and_mutation_closed(void) {
    TypedDispatchFixture fixture = make_fixture();
    XrFingerprint fingerprint = xr_target_plan_fingerprint(fixture.program_plan);
    XrModuleGenerationIdentity generation =
        debug_generation_identity(fixture.program_plan, UINT8_C(0x17));
    XrVmTraceEvent storage[32];
    XrVmTraceBuffer buffer;
    REQUIRE(xr_typed_trace_buffer_init(&buffer, storage, 32));
    XrVmTraceSink sink = xr_typed_trace_buffer_sink(&buffer);
    XrVmDebugSession *session = NULL;
    REQUIRE(xr_typed_debug_session_create(&fingerprint, &generation, &sink, NULL, NULL, &session) ==
            XR_VM_DEBUG_SESSION_OK);
    int64_t result = 0;
    REQUIRE(execute_debug_request_i64(fixture.program_plan, &fingerprint, 0, NULL, 0, &generation,
                                      session, XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
                                      &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == INT64_MIN && buffer.count > 0);

    bool saw_source_backed_instruction = false;
    for (uint32_t i = 0; i < buffer.count; i++) {
        const XrVmTraceEvent *event = &buffer.events[i];
        REQUIRE(event->schema_version == XR_VM_TRACE_SCHEMA_VERSION);
        if (event->instruction == XR_VM_TRACE_ID_NONE) {
            REQUIRE(event->debug_fact == XR_VM_TRACE_ID_NONE);
            continue;
        }
        REQUIRE(event->debug_fact == event->instruction);
        REQUIRE(event->semantic_operation != XR_VM_TRACE_ID_NONE);
        REQUIRE(event->source_span_availability == XR_VM_DEBUG_FACT_AVAILABLE);
        REQUIRE(memcmp(event->semantic_operation_identity.bytes,
                       (uint8_t[sizeof(event->semantic_operation_identity.bytes)]){0},
                       sizeof(event->semantic_operation_identity.bytes)) != 0);
        REQUIRE(memcmp(event->source_span_identity.bytes,
                       (uint8_t[sizeof(event->source_span_identity.bytes)]){0},
                       sizeof(event->source_span_identity.bytes)) != 0);
        REQUIRE(event->source_start_line > 0 && event->source_end_line > 0);
        XrVmMaterializedEvent materialized;
        REQUIRE(xr_typed_materialize_event(fixture.program_plan, &fingerprint,
                                           event, &materialized) ==
                XR_VM_MATERIALIZE_OK);
        REQUIRE(materialized.source_span == XR_VM_DEBUG_FACT_AVAILABLE);
        REQUIRE(materialized.semantic_operation == event->semantic_operation);
        XrVmTraceEvent mutated = *event;
        mutated.source_start_column++;
        REQUIRE(xr_typed_materialize_event(fixture.program_plan, &fingerprint,
                                           &mutated, &materialized) ==
                XR_VM_MATERIALIZE_EVENT_INVALID);
        saw_source_backed_instruction = true;
    }
    REQUIRE(saw_source_backed_instruction);

    XrTargetDebugFactRecord saved = fixture.program_plan->debug_facts[0];
    fixture.program_plan->debug_facts[0].source_end_column++;
    char error[512] = {0};
    REQUIRE(!xr_target_plan_verify(fixture.program_plan, error, sizeof(error)));
    REQUIRE(strncmp(error, "XR_TARGET_1005", strlen("XR_TARGET_1005")) == 0);
    fixture.program_plan->debug_facts[0] = saved;
    xr_typed_debug_session_free(&session);
    dispose_fixture(&fixture);
}

typedef struct DebugStopCopy {
    uint8_t reason;
    XrVmTraceEvent instruction;
    uint32_t frame_count;
    uint32_t frame_ids[4];
    uint32_t frame_instructions[4];
    uint32_t local_count;
    uint32_t local_frames[16];
    uint32_t local_slots[16];
    uint32_t local_sizes[16];
    XrStableId local_identities[16];
    uint64_t local_bits[16];
} DebugStopCopy;

typedef struct DebugControlProbe {
    XrVmDebugResumeCommand commands[8];
    uint32_t command_count;
    uint32_t stop_count;
    bool accept;
    DebugStopCopy stops[8];
} DebugControlProbe;

static bool copy_debug_stop(void *context, const XrVmDebugStop *stop,
                            XrVmDebugResumeCommand *resume) {
    DebugControlProbe *probe = (DebugControlProbe *) context;
    REQUIRE(probe != NULL && stop != NULL && resume != NULL);
    REQUIRE(stop->schema_version == XR_VM_DEBUG_CONTROL_SCHEMA_VERSION);
    REQUIRE(probe->stop_count < probe->command_count &&
            probe->stop_count < sizeof(probe->stops) / sizeof(probe->stops[0]));
    REQUIRE(stop->frame_count <= 4u && stop->local_count <= 16u);
    DebugStopCopy *copy = &probe->stops[probe->stop_count];
    memset(copy, 0, sizeof(*copy));
    copy->reason = stop->reason;
    copy->instruction = stop->instruction;
    copy->frame_count = stop->frame_count;
    copy->local_count = stop->local_count;
    for (uint32_t i = 0; i < stop->frame_count; i++) {
        copy->frame_ids[i] = stop->frames[i].instruction.frame;
        copy->frame_instructions[i] =
            stop->frames[i].instruction.instruction;
    }
    for (uint32_t i = 0; i < stop->local_count; i++) {
        const XrVmDebugLocalSnapshot *local = &stop->locals[i];
        REQUIRE(local->value_size > 0 && local->value_size <= sizeof(uint64_t));
        REQUIRE(local->value_offset <= stop->local_bytes_size &&
                local->value_size <=
                    stop->local_bytes_size - local->value_offset);
        copy->local_frames[i] = local->frame;
        copy->local_slots[i] = local->slot;
        copy->local_sizes[i] = local->value_size;
        copy->local_identities[i] = local->identity;
        memcpy(&copy->local_bits[i],
               stop->local_bytes + local->value_offset, local->value_size);
    }
    *resume = probe->commands[probe->stop_count];
    probe->stop_count++;
    return probe->accept;
}

static bool stable_id_is_nonzero(XrStableId identity) {
    uint8_t combined = 0;
    for (uint32_t i = 0; i < sizeof(identity.bytes); i++)
        combined |= identity.bytes[i];
    return combined != 0;
}

static void reset_debug_probe(DebugControlProbe *probe,
                              const XrVmDebugResumeCommand *commands,
                              uint32_t command_count, bool accept) {
    REQUIRE(probe != NULL && commands != NULL && command_count > 0 &&
            command_count <= sizeof(probe->commands) / sizeof(probe->commands[0]));
    memset(probe, 0, sizeof(*probe));
    memcpy(probe->commands, commands, (size_t) command_count * sizeof(*commands));
    probe->command_count = command_count;
    probe->accept = accept;
}

static void test_debug_control_breakpoints_and_initialized_locals(void) {
    TypedDispatchFixture fixture = make_fixture();
    XrTargetPlan *plan = fixture.program_plan;
    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    uint32_t instruction_count = 0;
    uint32_t fact_count = 0;
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_instructions(plan, &instruction_count);
    const XrTargetDebugFactRecord *facts = xr_target_plan_debug_facts(plan, &fact_count);
    REQUIRE(instructions != NULL && facts != NULL && instruction_count == fact_count);
    uint32_t copy_row =
        row_of_opcode(instructions, instruction_count, XR_TARGET_INSTRUCTION_COPY_I64, 0);
    REQUIRE(copy_row != UINT32_MAX && stable_id_is_nonzero(facts[copy_row].source_span_identity));
    XrVmDebugBreakpointRequest breakpoint = {
        .kind = XR_VM_DEBUG_BREAKPOINT_SOURCE_SPAN,
        .identity = facts[copy_row].source_span_identity,
    };
    XrVmDebugPlan *debug_plan = NULL;
    REQUIRE(xr_typed_debug_plan_create(plan, &fingerprint, &breakpoint, 1, &debug_plan) ==
            XR_VM_DEBUG_CONTROL_OK);
    XrVmDebugBreakpointRequest duplicate[2] = {breakpoint, breakpoint};
    XrVmDebugPlan *rejected = NULL;
    REQUIRE(xr_typed_debug_plan_create(plan, &fingerprint, duplicate, 2, &rejected) ==
            XR_VM_DEBUG_CONTROL_BREAKPOINT_DUPLICATE);
    REQUIRE(rejected == NULL);
    XrVmDebugBreakpointRequest missing = breakpoint;
    missing.identity.bytes[0] ^= UINT8_C(0x80);
    REQUIRE(xr_typed_debug_plan_create(plan, &fingerprint, &missing, 1, &rejected) ==
            XR_VM_DEBUG_CONTROL_BREAKPOINT_NOT_FOUND);
    REQUIRE(xr_typed_debug_plan_create(plan, &fingerprint, &breakpoint,
                                       XR_VM_DEBUG_MAX_BREAKPOINTS + UINT32_C(1),
                                       &rejected) == XR_VM_DEBUG_CONTROL_RESOURCE_LIMIT);
    uint32_t budget_locals = 0;
    size_t budget_bytes = 0;
    REQUIRE(xr_typed_debug_snapshot_budget_reserve(&budget_locals, &budget_bytes,
                                                   XR_VM_DEBUG_MAX_SNAPSHOT_BYTES));
    REQUIRE(budget_locals == 1u && budget_bytes == XR_VM_DEBUG_MAX_SNAPSHOT_BYTES);
    REQUIRE(!xr_typed_debug_snapshot_budget_reserve(&budget_locals, &budget_bytes, 1u));
    REQUIRE(budget_locals == 1u && budget_bytes == XR_VM_DEBUG_MAX_SNAPSHOT_BYTES);
    budget_locals = XR_VM_DEBUG_MAX_TRACKED_SLOTS;
    budget_bytes = 0;
    REQUIRE(!xr_typed_debug_snapshot_budget_reserve(&budget_locals, &budget_bytes, 1u));

    DebugControlProbe probe;
    XrVmDebugControl *control = NULL;
    REQUIRE(xr_typed_debug_control_create(debug_plan, copy_debug_stop, &probe, &control) ==
            XR_VM_DEBUG_CONTROL_OK);
    xr_typed_debug_plan_free(&debug_plan);
    REQUIRE(debug_plan == NULL && control != NULL);
    XrModuleGenerationIdentity generation = debug_generation_identity(plan, UINT8_C(0x31));
    XrVmDebugSession *session = NULL;
    XrFingerprint wrong_fingerprint = fingerprint;
    wrong_fingerprint.bytes[0] ^= UINT8_C(1);
    REQUIRE(xr_typed_debug_session_create(&wrong_fingerprint, NULL, NULL, NULL, control,
                                          &session) == XR_VM_DEBUG_SESSION_PLAN_IDENTITY_MISMATCH);
    REQUIRE(session == NULL);
    REQUIRE(xr_typed_debug_session_create(&fingerprint, &generation, NULL, NULL, control,
                                          &session) == XR_VM_DEBUG_SESSION_OK);
    static const XrVmDebugResumeCommand run_commands[] = {
        XR_VM_DEBUG_RESUME_STEP_INTO,
        XR_VM_DEBUG_RESUME_CONTINUE,
    };
    const XrTypedDispatchProvider providers[] = {
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
    };
    for (uint32_t provider = 0; provider < sizeof(providers) / sizeof(providers[0]); provider++) {
        reset_debug_probe(&probe, run_commands, 2, true);
        REQUIRE(xr_typed_debug_control_arm(control, XR_VM_DEBUG_RESUME_CONTINUE) ==
                XR_VM_DEBUG_CONTROL_OK);
        int64_t result = 9;
        REQUIRE(execute_debug_request_i64(plan, &fingerprint, 0, NULL, 0, &generation, session,
                                          providers[provider], &result) == XR_TYPED_DISPATCH_OK);
        REQUIRE(result == INT64_MIN && probe.stop_count == 2);
        REQUIRE(probe.stops[0].reason == XR_VM_DEBUG_STOP_BREAKPOINT &&
                probe.stops[0].instruction.instruction == copy_row &&
                probe.stops[0].frame_count == 1 && probe.stops[0].local_count == 2);
        REQUIRE(probe.stops[1].reason == XR_VM_DEBUG_STOP_STEP &&
                probe.stops[1].instruction.ordinal > probe.stops[0].instruction.ordinal &&
                probe.stops[1].frame_count == 1 && probe.stops[1].local_count == 3);
        bool saw_max = false;
        bool saw_one = false;
        for (uint32_t i = 0; i < probe.stops[0].local_count; i++) {
            REQUIRE(stable_id_is_nonzero(probe.stops[0].local_identities[i]));
            saw_max |= probe.stops[0].local_bits[i] == (uint64_t) INT64_MAX;
            saw_one |= probe.stops[0].local_bits[i] == UINT64_C(1);
        }
        REQUIRE(saw_max && saw_one);
    }

    static const XrVmDebugResumeCommand terminate[] = {
        XR_VM_DEBUG_RESUME_TERMINATE,
    };
    reset_debug_probe(&probe, terminate, 1, true);
    REQUIRE(xr_typed_debug_control_arm(control, XR_VM_DEBUG_RESUME_CONTINUE) ==
            XR_VM_DEBUG_CONTROL_OK);
    int64_t result = 7;
    REQUIRE(execute_debug_request_i64(plan, &fingerprint, 0, NULL, 0, &generation, session,
                                      XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
                                      &result) == XR_TYPED_DISPATCH_DEBUG_TERMINATED);
    REQUIRE(result == 0 && probe.stop_count == 1);

    reset_debug_probe(&probe, terminate, 1, false);
    REQUIRE(xr_typed_debug_control_arm(control, XR_VM_DEBUG_RESUME_CONTINUE) ==
            XR_VM_DEBUG_CONTROL_OK);
    result = 7;
    REQUIRE(execute_debug_request_i64(plan, &fingerprint, 0, NULL, 0, &generation, session,
                                      XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
                                      &result) == XR_TYPED_DISPATCH_DEBUG_STOP_REJECTED);
    REQUIRE(result == 0 && probe.stop_count == 1);

    XrVmDebugSession *mismatched_session = NULL;
    REQUIRE(xr_typed_debug_session_create(&wrong_fingerprint, NULL, NULL, NULL, NULL,
                                          &mismatched_session) == XR_VM_DEBUG_SESSION_OK);
    result = 7;
    REQUIRE(execute_debug_request_i64(plan, &fingerprint, 0, NULL, 0, &generation,
                                      mismatched_session,
                                      XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
                                      &result) == XR_TYPED_DISPATCH_DEBUG_IDENTITY_MISMATCH);
    REQUIRE(result == 0);
    xr_typed_debug_session_free(&mismatched_session);

    static const XrVmDebugResumeCommand continue_after_owner_release[] = {
        XR_VM_DEBUG_RESUME_CONTINUE,
    };
    reset_debug_probe(&probe, continue_after_owner_release, 1, true);
    REQUIRE(xr_typed_debug_control_arm(control, XR_VM_DEBUG_RESUME_CONTINUE) ==
            XR_VM_DEBUG_CONTROL_OK);
    REQUIRE(xr_typed_debug_control_free(&control) == XR_VM_DEBUG_CONTROL_OK);
    REQUIRE(control == NULL);
    result = 0;
    REQUIRE(execute_debug_request_i64(plan, &fingerprint, 0, NULL, 0, &generation, session,
                                      XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
                                      &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == INT64_MIN && probe.stop_count == 1);
    xr_typed_debug_session_free(&session);
    dispose_fixture(&fixture);
}

static void test_debug_control_steps_across_direct_call_stack(void) {
    XrSemanticPlan *semantic = build_direct_i64_call_semantic(false);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    XrModuleGenerationIdentity generation = debug_generation_identity(plan, UINT8_C(0x41));
    XrVmDebugPlan *debug_plan = NULL;
    REQUIRE(xr_typed_debug_plan_create(plan, &fingerprint, NULL, 0, &debug_plan) ==
            XR_VM_DEBUG_CONTROL_OK);
    DebugControlProbe probe;
    XrVmDebugControl *control = NULL;
    REQUIRE(xr_typed_debug_control_create(debug_plan, copy_debug_stop, &probe, &control) ==
            XR_VM_DEBUG_CONTROL_OK);
    XrVmDebugSession *session = NULL;
    REQUIRE(xr_typed_debug_session_create(&fingerprint, &generation, NULL, NULL, control,
                                          &session) == XR_VM_DEBUG_SESSION_OK);
    static const XrVmDebugResumeCommand commands[] = {
        XR_VM_DEBUG_RESUME_STEP_INTO,
        XR_VM_DEBUG_RESUME_STEP_INTO,
        XR_VM_DEBUG_RESUME_STEP_OUT,
        XR_VM_DEBUG_RESUME_CONTINUE,
    };
    reset_debug_probe(&probe, commands, 4, true);
    REQUIRE(xr_typed_debug_control_arm(control, XR_VM_DEBUG_RESUME_STEP_INTO) ==
            XR_VM_DEBUG_CONTROL_OK);
    int64_t result = 0;
    REQUIRE(execute_debug_request_i64(plan, &fingerprint, 0, NULL, 0, &generation, session,
                                      XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
                                      &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == 42 && probe.stop_count == 4);
    REQUIRE(probe.stops[0].instruction.frame_depth == 0 && probe.stops[0].frame_count == 1 &&
            probe.stops[0].local_count == 0);
    REQUIRE(probe.stops[1].instruction.opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_I64 &&
            probe.stops[1].instruction.frame_depth == 0 && probe.stops[1].local_count == 1);
    REQUIRE(probe.stops[2].instruction.opcode == XR_TARGET_INSTRUCTION_PARAM_I64 &&
            probe.stops[2].instruction.frame_depth == 1 && probe.stops[2].frame_count == 2 &&
            probe.stops[2].frame_ids[0] == 0 && probe.stops[2].frame_ids[1] == 1 &&
            probe.stops[2].local_count == 2 && probe.stops[2].local_frames[0] == 0 &&
            probe.stops[2].local_frames[1] == 1);
    REQUIRE(probe.stops[3].instruction.opcode == XR_TARGET_INSTRUCTION_RETURN_I64 &&
            probe.stops[3].instruction.frame_depth == 0 && probe.stops[3].frame_count == 1 &&
            probe.stops[3].local_count == 2);

    static const XrVmDebugResumeCommand step_over_commands[] = {
        XR_VM_DEBUG_RESUME_STEP_INTO,
        XR_VM_DEBUG_RESUME_STEP_OVER,
        XR_VM_DEBUG_RESUME_CONTINUE,
    };
    reset_debug_probe(&probe, step_over_commands, 3, true);
    REQUIRE(xr_typed_debug_control_arm(control, XR_VM_DEBUG_RESUME_STEP_INTO) ==
            XR_VM_DEBUG_CONTROL_OK);
    result = 0;
    REQUIRE(execute_debug_request_i64(plan, &fingerprint, 0, NULL, 0, &generation, session,
                                      XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
                                      &result) == XR_TYPED_DISPATCH_OK);
    REQUIRE(result == 42 && probe.stop_count == 3);
    REQUIRE(probe.stops[1].reason == XR_VM_DEBUG_STOP_STEP &&
            probe.stops[1].instruction.opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_I64);
    REQUIRE(probe.stops[2].reason == XR_VM_DEBUG_STOP_STEP &&
            probe.stops[2].instruction.opcode == XR_TARGET_INSTRUCTION_RETURN_I64 &&
            probe.stops[2].instruction.frame_depth == 0);

    static const XrVmDebugResumeCommand root_step_out[] = {
        XR_VM_DEBUG_RESUME_STEP_OUT,
    };
    reset_debug_probe(&probe, root_step_out, 1, true);
    REQUIRE(xr_typed_debug_control_arm(control, XR_VM_DEBUG_RESUME_STEP_INTO) ==
            XR_VM_DEBUG_CONTROL_OK);
    result = 7;
    REQUIRE(execute_debug_request_i64(plan, &fingerprint, 0, NULL, 0, &generation, session,
                                      XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
                                      &result) == XR_TYPED_DISPATCH_DEBUG_STOP_REJECTED);
    REQUIRE(result == 0 && probe.stop_count == 1);

    xr_typed_debug_session_free(&session);
    REQUIRE(xr_typed_debug_control_free(&control) == XR_VM_DEBUG_CONTROL_OK);
    xr_typed_debug_plan_free(&debug_plan);
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_direct_local_call_depth_is_globally_bounded(void) {
    XrSemanticPlan *semantic = build_deep_direct_i64_call_semantic();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error,
                                 sizeof(error)));
    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    for (uint32_t attempt = 0; attempt < 2u; attempt++) {
        int64_t result = 91;
        REQUIRE(execute_request_i64(plan, &fingerprint, 0, NULL, 0,
                                              &result) ==
                XR_TYPED_DISPATCH_CALL_DEPTH_EXCEEDED);
        REQUIRE(result == 0);
    }
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
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
    REQUIRE(execute_request_i64(looping, &fingerprint, 0, NULL, 0,
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
    test_other_function_lifecycle_partitions_do_not_scan_selected_scalar();
    test_parameters_reach_the_executed_program();
    test_xtp_exact_roundtrip_executes_same_program();
    test_instruction_mutations_fail_closed();
    test_shift_rows_execute_with_masked_counts();
    test_division_rows_take_the_zero_divisor_edge();
    test_conditional_branch_selects_its_edge();
    test_unconditional_jumps_chain_blocks();
    test_comparison_rows_drive_the_branch();
    test_direct_local_i64_call_executes_and_rejects_drift();
    test_runtime_only_trace_profile_and_materialization();
    test_trace_debug_facts_are_source_backed_and_mutation_closed();
    test_debug_control_breakpoints_and_initialized_locals();
    test_debug_control_steps_across_direct_call_stack();
    test_direct_local_call_depth_is_globally_bounded();
    test_backward_jump_stops_at_the_step_budget();
    puts("typed dispatch tests passed");
    return 0;
}
