/*
 * test_semantic_plan.c - Immutable SemanticPlan and exact-version XSM contract
 */

#include "../../../src/base/xmalloc.h"
#include "../../../src/base/xsha256.h"
#include "../../../src/base/xstorage.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_arc.h"
#include "../../../src/ir/xi_coro_analyze.h"
#include "../../../src/ir/xi_effect.h"
#include "../../../src/ir/xi_module.h"
#include "../../../src/ir/xi_own.h"
#include "../../../src/plan/format/xr_xsm_schema.h"
#include "../../../src/plan/ownership/xr_ownership_certificate.h"
#include "../../../src/plan/ownership/xr_ownership_certificate_internal.h"
#include "../../../src/plan/ownership/xr_ownership_check.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/semantic/xr_semantic_graph.h"
#include "../../../src/plan/semantic/xr_semantic_ops.h"
#include "../../../src/plan/semantic/xr_semantic_plan_internal.h"
#include "../../../src/plan/semantic/xr_semantic_verify.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/shared/xr_semantic_owner_ids_gen.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 2, .frozen = true};
static XrType stub_string = {.kind = XR_KIND_STRING, .id = 3, .frozen = true};
static XrType stub_unit = {.kind = XR_KIND_UNIT, .id = 4, .frozen = true};
static XrType stub_null = {.kind = XR_KIND_NULL, .id = 6, .frozen = true};
static XrType stub_string_builder = {
    .kind = XR_KIND_INSTANCE,
    .id = 9,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance = {.class_name = "StringBuilder"},
};
static XrType stub_shadow_string_builder = {
    .kind = XR_KIND_INSTANCE,
    .id = 10,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance = {
        .class_name = "StringBuilder",
        .class_ref = (XrClassInfo *) (uintptr_t) 1,
    },
};
static XrType stub_semaphore = {
    .kind = XR_KIND_INSTANCE,
    .id = 11,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance = {.class_name = "Semaphore"},
};
static XrType stub_shadow_semaphore = {
    .kind = XR_KIND_INSTANCE,
    .id = 12,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance = {
        .class_name = "Semaphore",
        .class_ref = (XrClassInfo *) (uintptr_t) 1,
    },
};
static XrType stub_function = {
    .kind = XR_KIND_FUNCTION,
    .id = 7,
    .frozen = true,
    .function = {.return_type = &stub_int, .throw_effect = XR_FN_EFFECT_NO_THROW},
};
static XrFunctionParam stub_sleep_params[] = {
    {.type = &stub_int, .mode = XR_PARAM_READ},
};
static XrType stub_sleep_function = {
    .kind = XR_KIND_FUNCTION,
    .id = 8,
    .frozen = true,
    .function = {
        .params = stub_sleep_params,
        .param_count = 1,
        .min_params = 1,
        .return_type = &stub_unit,
        .throw_effect = XR_FN_EFFECT_NO_THROW,
    },
};
static XrType stub_array = {
    .kind = XR_KIND_ARRAY,
    .id = 5,
    .frozen = true,
    .container = {.element_type = &stub_int},
};
static const char *stub_shape_field_names[] = {"count", "label"};
static XrType *stub_shape_field_types[] = {&stub_int, &stub_string};
static XrType stub_shape = {
    .kind = XR_KIND_STRUCT_OBJECT,
    .id = 7,
    .frozen = true,
    .object = {
        .field_names = stub_shape_field_names,
        .field_types = stub_shape_field_types,
        .field_count = 2,
    },
};

static void set_source_span(XiValue *value, uint32_t start_line, uint32_t start_column,
                            uint32_t end_line, uint32_t end_column) {
    REQUIRE(value != NULL);
    value->source_span =
        (XiSourceSpan) {start_line, start_column, end_line, end_column};
    REQUIRE(xi_source_span_is_complete(value->source_span));
}

static XrSemanticPlan *build_probe_plan(void) {
    XiFunc *function = xi_func_new("artifact_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    REQUIRE(xi_const_bool(function, entry, true, &stub_bool) != NULL);
    REQUIRE(xi_const_str(function, entry, "owned-by-plan", &stub_string) != NULL);
    XiValue *result = xi_const_int(function, entry, 42, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "SemanticPlan build failed: %s\n", error);
    REQUIRE(built);
    REQUIRE(plan != NULL);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_string_builder_constructor_plan(void) {
    XiFunc *function = xi_func_new("string_builder_constructor_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *builder =
        xi_value_new(function, entry, XI_CALL_BUILTIN, &stub_string_builder, 0);
    REQUIRE(builder != NULL);
    builder->aux = (void *) "StringBuilder";
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = builder;
    XiValue *result = xi_const_int(function, entry, 0, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "StringBuilder constructor plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(function);
    return plan;
}

static XrSemanticOperationRecord *find_operation(XrSemanticPlan *plan, uint16_t opcode) {
    for (uint32_t index = 0; plan && index < plan->operation_count; index++)
        if (plan->operations[index].opcode == opcode)
            return &plan->operations[index];
    return NULL;
}

static XrSemanticPlan *build_owned_parameter_plan(void) {
    XiFunc *function = xi_func_new("owned_parameter_probe", &stub_int);
    REQUIRE(function != NULL);
    function->source_file = "pkg/owned_parameter_probe.xr";
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *parameter = xi_param(function, entry, 0, &stub_string);
    REQUIRE(parameter != NULL);
    set_source_span(parameter, 1, 27, 1, 32);
    function->nparams = 1;
    function->params = (XiValue **) xr_malloc(sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = parameter;
    XiValue *result = xi_const_int(function, entry, 7, &stub_int);
    REQUIRE(result != NULL);
    set_source_span(result, 2, 12, 2, 13);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "owned-parameter plan build failed: %s\n", error);
    REQUIRE(built);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_panic_edge_plan(void) {
    XiFunc *function = xi_func_new("panic_edge_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *registration = xi_block_new(function);
    XiBlock *body = xi_block_new(function);
    XiBlock *handler = xi_block_new(function);
    REQUIRE(registration != NULL && body != NULL && handler != NULL);

    XiValue *try_operation = xi_value_new(function, registration, XI_TRY, &stub_unit, 0);
    REQUIRE(try_operation != NULL);
    try_operation->aux = handler;
    try_operation->aux_int = -1;
    try_operation->flags = XI_FLAG_SIDE_EFFECT;
    xi_block_set_jump(registration, body);

    XiValue *normal = xi_const_int(function, body, 1, &stub_int);
    XiValue *caught = xi_const_int(function, handler, 2, &stub_int);
    REQUIRE(normal != NULL && caught != NULL);
    xi_block_set_return(body, normal);
    xi_block_set_return(handler, caught);
    /* This is the same SSA-only predecessor shape produced by panic lowering:
     * it is deliberately not the block containing XI_TRY. */
    xi_block_add_pred(handler, body);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "panic-edge plan build failed: %s\n", error);
    REQUIRE(built);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_error_edge_plan(void) {
    XiFunc *function = xi_func_new("error_edge_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *check_block = xi_block_new(function);
    XiBlock *error_block = xi_block_new(function);
    XiBlock *normal_block = xi_block_new(function);
    REQUIRE(check_block != NULL && error_block != NULL && normal_block != NULL);

    XiValue *check = xi_value_new(function, check_block, XI_ERR_CHECK, &stub_bool, 0);
    REQUIRE(check != NULL);
    check->flags = XI_FLAG_SIDE_EFFECT;
    xi_block_set_if(check_block, check, error_block, normal_block);
    XiValue *error_result = xi_const_int(function, error_block, -1, &stub_int);
    XiValue *normal_result = xi_const_int(function, normal_block, 1, &stub_int);
    REQUIRE(error_result != NULL && normal_result != NULL);
    xi_block_set_return(error_block, error_result);
    xi_block_set_return(normal_block, normal_result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "error-edge plan build failed: %s\n", error);
    REQUIRE(built);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_phi_dominance_plan(void) {
    XiFunc *function = xi_func_new("phi_dominance_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    XiBlock *left = xi_block_new(function);
    XiBlock *right = xi_block_new(function);
    XiBlock *merge = xi_block_new(function);
    REQUIRE(entry != NULL && left != NULL && right != NULL && merge != NULL);

    XiValue *condition = xi_const_bool(function, entry, true, &stub_bool);
    REQUIRE(condition != NULL);
    xi_block_set_if(entry, condition, left, right);
    XiValue *left_value = xi_const_int(function, left, 10, &stub_int);
    XiValue *right_value = xi_const_int(function, right, 20, &stub_int);
    REQUIRE(left_value != NULL && right_value != NULL);
    xi_block_set_jump(left, merge);
    xi_block_set_jump(right, merge);
    XiPhi *phi = xi_phi_new(function, merge, &stub_int, merge->npreds);
    REQUIRE(phi != NULL && merge->npreds == 2);
    phi->value.args[0] = left_value;
    phi->value.args[1] = right_value;
    xi_block_set_return(merge, &phi->value);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "PHI dominance plan build failed: %s\n", error);
    REQUIRE(built);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_typed_call_operand_plan(void) {
    XiFunc *function = xi_func_new("typed_call_operand_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiImportRef import_ref = {
        .module_path = "./operand_contract",
        .member_name = "target",
        .resolved_mod_index = -1,
        .resolved_shared_slot = -1,
        .resolved_export_slot = -1,
    };
    XiValue *callee = xi_value_new(function, entry, XI_IMPORT_REF, &stub_function, 0);
    XiValue *first = xi_const_int(function, entry, 11, &stub_int);
    XiValue *second = xi_const_int(function, entry, 22, &stub_int);
    REQUIRE(callee != NULL && first != NULL && second != NULL);
    callee->aux = &import_ref;

    XiCallArgPlan arguments[2] = {0};
    arguments[0].param_mode = XR_PARAM_REF;
    arguments[0].access = XR_CALL_ARG_REF;
    arguments[0].origin = XI_PLACE_ORIGIN_STACK_LOCAL;
    arguments[0].lifetime = XI_PLACE_LIFETIME_CALL_BOUND;
    arguments[0].addressable = true;
    arguments[0].origin_var_id = 0;
    arguments[0].place = first;
    arguments[1].param_mode = XR_PARAM_MOVE;
    arguments[1].access = XR_CALL_ARG_MOVE;
    arguments[1].origin_var_id = XI_NO_VAR_ID;
    XiCallPlan call_plan = {.args = arguments, .nargs = 2, .verified = true};

    XiValue *call = xi_value_new(function, entry, XI_CALL, &stub_int, 3);
    REQUIRE(call != NULL);
    call->args[0] = callee;
    call->args[1] = first;
    call->args[2] = second;
    call->call_plan = &call_plan;
    xi_block_set_return(entry, call);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "typed-call-operand plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_indirect_callable_plan(uint16_t opcode,
                                                    bool publish_state) {
    XiFunc *function = xi_func_new("indirect_callable_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *callee = xi_param(function, entry, 0, &stub_function);
    REQUIRE(callee != NULL);
    function->nparams = 1;
    function->params = (XiValue **) xr_malloc(sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = callee;
    XiValue *argument = xi_const_int(function, entry, 9, &stub_int);
    XiValue *call = xi_value_new(function, entry, opcode, &stub_int, 2);
    REQUIRE(argument != NULL && call != NULL);
    call->args[0] = callee;
    call->args[1] = argument;
    xi_block_set_return(entry, call);
    XiCoroSuspendPoint point = {
        .state_id = 1,
        .op = call,
        .kind = XI_CORO_SUSP_CALL,
    };
    XiCoroPlan coroutine = {
        .is_coroutine = true,
        .nstates = 1,
        .points = &point,
    };
    function->coro_plan = publish_state ? &coroutine : NULL;
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "indirect-callable plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_native_yieldable_call_target_plan(const char *module,
                                                               const char *member,
                                                               uint16_t opcode,
                                                               uint16_t argument_count,
                                                               bool identity_copy,
                                                               bool publish_state) {
    XiFunc *function = xi_func_new("native_yieldable_probe", &stub_unit);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiImportRef import_ref = {
        .module_path = module,
        .member_name = member,
        .resolved_mod_index = -1,
        .resolved_shared_slot = -1,
        .resolved_export_slot = -1,
        .resolution_attempted = true,
    };
    XiValue *callee = xi_value_new(function, entry, XI_IMPORT_REF,
                                   &stub_sleep_function, 0);
    REQUIRE(callee != NULL);
    callee->aux = &import_ref;
    XiValue *call_callee = callee;
    if (identity_copy) {
        call_callee = xi_value_new(function, entry, XI_COPY,
                                   &stub_sleep_function, 1);
        REQUIRE(call_callee != NULL);
        call_callee->args[0] = callee;
        call_callee->aux_int = XI_COPY_KIND_IDENTITY;
    }
    XiValue *arguments[4] = {0};
    REQUIRE(argument_count <= 4);
    for (uint16_t index = 0; index < argument_count; index++) {
        arguments[index] = xi_const_int(function, entry, 1 + index, &stub_int);
        REQUIRE(arguments[index] != NULL);
    }
    XiValue *call = xi_value_new(function, entry, opcode, &stub_unit,
                                 (uint16_t) (argument_count + 1u));
    REQUIRE(call != NULL);
    call->args[0] = call_callee;
    for (uint16_t index = 0; index < argument_count; index++)
        call->args[index + 1] = arguments[index];
    xi_block_set_return(entry, call);
    XiCoroSuspendPoint point = {
        .state_id = 1,
        .op = call,
        .kind = XI_CORO_SUSP_CALL,
    };
    XiCoroPlan coroutine = {
        .is_coroutine = true,
        .nstates = 1,
        .points = &point,
    };
    function->coro_plan = publish_state ? &coroutine : NULL;
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "native-yieldable plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_native_namespace_yieldable_plan(
    const char *module, const char *selector, bool resolution_attempted,
    bool source_resolved, bool publish_state) {
    XiFunc *root = xi_func_new("native_namespace_root", &stub_unit);
    XiFunc *caller = xi_func_new("native_namespace_caller", &stub_unit);
    REQUIRE(root != NULL && caller != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *caller_entry = xi_block_new(caller);
    REQUIRE(root_entry != NULL && caller_entry != NULL);
    root->children = (XiFunc **) xr_malloc(sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = caller;
    root->nchildren = root->children_cap = 1;
    caller->parent_func = root;
    XiModule fake_source = {0};
    XiImportRef import_ref = {
        .module_path = module,
        .member_name = NULL,
        .resolved_mod_index = source_resolved ? 0 : -1,
        .resolved_shared_slot = -1,
        .resolved_export_slot = -1,
        .resolved_module = source_resolved ? &fake_source : NULL,
        .resolution_attempted = resolution_attempted,
    };
    XiValue *import =
        xi_value_new(root, root_entry, XI_IMPORT_REF, &stub_shape, 0);
    XiValue *store =
        xi_value_new(root, root_entry, XI_SET_SHARED, &stub_unit, 1);
    REQUIRE(import != NULL && store != NULL);
    import->aux = &import_ref;
    store->args[0] = import;
    store->aux_int = 0;
    root->nshared = 1;
    xi_block_set_return(root_entry, NULL);
    XiValue *receiver =
        xi_value_new(caller, caller_entry, XI_GET_SHARED, &stub_shape, 0);
    XiValue *argument = xi_const_int(caller, caller_entry, 1, &stub_int);
    XiValue *call =
        xi_value_new(caller, caller_entry, XI_CALL_METHOD, &stub_unit, 2);
    REQUIRE(receiver != NULL && argument != NULL && call != NULL);
    receiver->aux_int = 0;
    call->args[0] = receiver;
    call->args[1] = argument;
    call->aux = (void *) selector;
    call->aux_int = 0;
    xi_block_set_return(caller_entry, call);
    XiCoroSuspendPoint point = {
        .state_id = 1, .op = call, .kind = XI_CORO_SUSP_CALL,
    };
    XiCoroPlan coroutine = {
        .is_coroutine = true, .nstates = 1, .points = &point,
    };
    caller->coro_plan = publish_state ? &coroutine : NULL;
    root->stage = caller->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "native-namespace plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(root);
    return plan;
}

static XrSemanticPlan *build_builtin_instance_yieldable_plan(
    XrType *receiver_type, const char *selector, uint16_t argument_count,
    bool publish_state, bool expect_success) {
    XiFunc *function = xi_func_new("builtin_instance_yieldable_probe", &stub_unit);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *receiver = xi_param(function, entry, 0, receiver_type);
    REQUIRE(receiver != NULL);
    function->nparams = 1;
    function->params = (XiValue **) xr_malloc(sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = receiver;
    XiValue *arguments[4] = {0};
    REQUIRE(argument_count <= 4);
    for (uint16_t index = 0; index < argument_count; index++) {
        arguments[index] = xi_const_int(function, entry, index + 1, &stub_int);
        REQUIRE(arguments[index] != NULL);
    }
    XiValue *call = xi_value_new(function, entry, XI_CALL_METHOD, &stub_bool,
                                 (uint16_t) (argument_count + 1u));
    REQUIRE(call != NULL);
    call->args[0] = receiver;
    for (uint16_t index = 0; index < argument_count; index++)
        call->args[index + 1u] = arguments[index];
    call->aux = (void *) selector;
    call->aux_int = 0;
    xi_block_set_return(entry, call);
    XiCoroSuspendPoint point = {
        .state_id = 1, .op = call, .kind = XI_CORO_SUSP_CALL,
    };
    XiCoroPlan coroutine = {
        .is_coroutine = true, .nstates = 1, .points = &point,
    };
    function->coro_plan = publish_state ? &coroutine : NULL;
    function->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (built != expect_success)
        fprintf(stderr, "builtin-instance plan build=%u expected=%u: %s\n",
                built ? 1u : 0u, expect_success ? 1u : 0u, error);
    REQUIRE(built == expect_success);
    if (expect_success)
        REQUIRE(plan != NULL);
    else
        REQUIRE(plan == NULL);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_source_export_call_target_plan(
    XrSemanticPlan **dependency_out) {
    XiFunc *dependency_root = xi_func_new("net_init", &stub_unit);
    XiFunc *write_bytes = xi_func_new("writeBytes", &stub_unit);
    REQUIRE(dependency_root != NULL && write_bytes != NULL);
    XiBlock *dependency_entry = xi_block_new(dependency_root);
    XiBlock *write_entry = xi_block_new(write_bytes);
    REQUIRE(dependency_entry != NULL && write_entry != NULL);
    dependency_root->children = (XiFunc **) xr_malloc(sizeof(*dependency_root->children));
    REQUIRE(dependency_root->children != NULL);
    dependency_root->children[0] = write_bytes;
    dependency_root->nchildren = dependency_root->children_cap = 1;
    write_bytes->parent_func = dependency_root;
    XiValue *closure = xi_value_new(dependency_root, dependency_entry, XI_CLOSURE_NEW,
                                    &stub_function, 0);
    XiValue *store = xi_value_new(dependency_root, dependency_entry, XI_SET_SHARED,
                                  &stub_unit, 1);
    REQUIRE(closure != NULL && store != NULL);
    closure->aux = write_bytes;
    store->args[0] = closure;
    store->aux_int = 0;
    dependency_root->nshared = 1;
    xi_block_set_return(dependency_entry, NULL);
    XiValue *yield = xi_value_new(write_bytes, write_entry, XI_YIELD, &stub_unit, 0);
    REQUIRE(yield != NULL);
    xi_block_set_return(write_entry, yield);
    XiCoroSuspendPoint write_point = {
        .state_id = 1, .op = yield, .kind = XI_CORO_SUSP_YIELD,
    };
    XiCoroPlan write_coroutine = {
        .is_coroutine = true, .nstates = 1, .points = &write_point,
    };
    write_bytes->coro_plan = &write_coroutine;
    dependency_root->stage = write_bytes->stage = XI_STAGE_OPTIMIZED;
    XiModule *dependency_module =
        xi_module_new("stdlib/net/net.xr", "net", dependency_root);
    REQUIRE(dependency_module != NULL);
    dependency_root->module = dependency_module;
    dependency_module->nslots = 1;
    dependency_module->nexports = 1;
    dependency_module->exports =
        (XiModuleExport *) xr_calloc(1, sizeof(*dependency_module->exports));
    REQUIRE(dependency_module->exports != NULL);
    dependency_module->exports[0].name = "writeBytes";
    dependency_module->exports[0].shared_slot = 0;
    dependency_module->exports[0].function = write_bytes;

    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(dependency_root, error, sizeof(error)));
    XrSemanticPlan *dependency = xr_semantic_plan_retain(dependency_root->semantic_plan);
    REQUIRE(dependency != NULL && dependency->source_export_count == 1);

    XiFunc *caller_root = xi_func_new("http_init", &stub_unit);
    XiFunc *caller = xi_func_new("_serverWriteAll", &stub_unit);
    REQUIRE(caller_root != NULL && caller != NULL);
    XiBlock *root_entry = xi_block_new(caller_root);
    XiBlock *caller_entry = xi_block_new(caller);
    REQUIRE(root_entry != NULL && caller_entry != NULL);
    caller_root->children = (XiFunc **) xr_malloc(sizeof(*caller_root->children));
    REQUIRE(caller_root->children != NULL);
    caller_root->children[0] = caller;
    caller_root->nchildren = caller_root->children_cap = 1;
    caller->parent_func = caller_root;
    XiImportRef import_ref = {
        .module_path = "stdlib/net/net.xr",
        .member_name = NULL,
        .resolved_mod_index = 0,
        .resolved_shared_slot = -1,
        .resolved_export_slot = -1,
        .resolved_module = dependency_module,
    };
    XiValue *namespace_ref = xi_value_new(caller_root, root_entry, XI_IMPORT_REF,
                                          &stub_shape, 0);
    XiValue *namespace_store = xi_value_new(caller_root, root_entry, XI_SET_SHARED,
                                            &stub_unit, 1);
    REQUIRE(namespace_ref != NULL && namespace_store != NULL);
    namespace_ref->aux = &import_ref;
    namespace_store->args[0] = namespace_ref;
    namespace_store->aux_int = 0;
    caller_root->nshared = 1;
    xi_block_set_return(root_entry, NULL);
    XiValue *receiver = xi_value_new(caller, caller_entry, XI_GET_SHARED, &stub_shape, 0);
    XiValue *method = xi_value_new(caller, caller_entry, XI_CALL_METHOD, &stub_unit, 1);
    REQUIRE(receiver != NULL && method != NULL);
    receiver->aux_int = 0;
    method->args[0] = receiver;
    method->aux = "writeBytes";
    method->aux_int = 0;
    xi_block_set_return(caller_entry, method);
    XiCoroSuspendPoint caller_point = {
        .state_id = 1, .op = method, .kind = XI_CORO_SUSP_CALL,
    };
    XiCoroPlan caller_coroutine = {
        .is_coroutine = true, .nstates = 1, .points = &caller_point,
    };
    caller->coro_plan = &caller_coroutine;
    caller_root->stage = caller->stage = XI_STAGE_OPTIMIZED;
    XiModule *caller_module = xi_module_new("stdlib/http/http.xr", "http", caller_root);
    REQUIRE(caller_module != NULL);
    caller_root->module = caller_module;
    caller_module->nslots = 1;
    XiModule *dependency_modules[] = {dependency_module};
    REQUIRE(xr_semantic_plan_build_and_attach_module_set(
        caller_root, dependency_modules, 1, error, sizeof(error)));
    XrSemanticPlan *result = xr_semantic_plan_retain(caller_root->semantic_plan);
    REQUIRE(result != NULL);
    xi_func_free(caller_root);
    xi_func_free(dependency_root);
    *dependency_out = dependency;
    return result;
}

static XrSemanticPlan *build_direct_local_call_target_plan(void) {
    XiFunc *root = xi_func_new("direct_call_target_root", &stub_int);
    XiFunc *child = xi_func_new("direct_call_target_child", &stub_int);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);
    XiValue *yield = xi_value_new(child, child_entry, XI_YIELD, &stub_int, 0);
    XiValue *child_result = xi_const_int(child, child_entry, 73, &stub_int);
    REQUIRE(yield != NULL && child_result != NULL);
    xi_block_set_return(child_entry, child_result);

    root->children = (XiFunc **) xr_malloc(sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;

    XiValue *closure = xi_value_new(root, root_entry, XI_STACK_ALLOC, &stub_function, 0);
    REQUIRE(closure != NULL);
    closure->aux_int = XI_CLOSURE_NEW;
    closure->aux = child;
    XiValue *alias = xi_value_new(root, root_entry, XI_COPY, &stub_function, 1);
    REQUIRE(alias != NULL);
    alias->args[0] = closure;
    alias->aux_int = XI_COPY_KIND_IDENTITY;
    XiValue *call = xi_value_new(root, root_entry, XI_CALL, &stub_int, 1);
    REQUIRE(call != NULL);
    call->args[0] = alias;
    xi_block_set_return(root_entry, call);

    XiCoroSuspendPoint root_point = {
        .state_id = 1,
        .op = call,
        .kind = XI_CORO_SUSP_CALL,
        .resolved_callee = child,
    };
    XiCoroPlan root_coro = {
        .is_coroutine = true,
        .nstates = 1,
        .points = &root_point,
    };
    XiCoroSuspendPoint child_point = {
        .state_id = 1,
        .op = yield,
        .kind = XI_CORO_SUSP_YIELD,
    };
    XiCoroPlan child_coro = {
        .is_coroutine = true,
        .nstates = 1,
        .points = &child_point,
    };
    root->coro_plan = &root_coro;
    child->coro_plan = &child_coro;
    root->stage = child->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "direct-call-target plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(root);
    return plan;
}

static XrSemanticPlan *build_shared_direct_call_target_plan(void) {
    XiFunc *root = xi_func_new("shared_direct_root", &stub_int);
    XiFunc *target = xi_func_new("shared_direct_target", &stub_int);
    XiFunc *caller = xi_func_new("shared_direct_caller", &stub_int);
    REQUIRE(root != NULL && target != NULL && caller != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *target_entry = xi_block_new(target);
    XiBlock *caller_entry = xi_block_new(caller);
    REQUIRE(root_entry != NULL && target_entry != NULL && caller_entry != NULL);

    XiValue *yield = xi_value_new(target, target_entry, XI_YIELD, &stub_int, 0);
    XiValue *target_result = xi_const_int(target, target_entry, 73, &stub_int);
    REQUIRE(yield != NULL && target_result != NULL);
    xi_block_set_return(target_entry, target_result);

    root->children = (XiFunc **) xr_malloc(2 * sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = target;
    root->children[1] = caller;
    root->nchildren = root->children_cap = 2;
    target->parent_func = root;
    caller->parent_func = root;

    XiValue *closure = xi_value_new(root, root_entry, XI_CLOSURE_NEW, &stub_function, 0);
    XiValue *alias = xi_value_new(root, root_entry, XI_COPY, &stub_function, 1);
    XiValue *store = xi_value_new(root, root_entry, XI_SET_SHARED, &stub_unit, 1);
    XiValue *root_result = xi_const_int(root, root_entry, 1, &stub_int);
    REQUIRE(closure != NULL && alias != NULL && store != NULL && root_result != NULL);
    closure->aux = target;
    alias->args[0] = closure;
    alias->aux_int = XI_COPY_KIND_IDENTITY;
    store->args[0] = alias;
    store->aux_int = 7;
    xi_block_set_return(root_entry, root_result);

    XiValue *load = xi_value_new(caller, caller_entry, XI_GET_SHARED, &stub_function, 0);
    XiValue *call = xi_value_new(caller, caller_entry, XI_CALL, &stub_int, 1);
    REQUIRE(load != NULL && call != NULL);
    load->aux_int = 7;
    call->args[0] = load;
    xi_block_set_return(caller_entry, call);

    XiCoroSuspendPoint caller_point = {
        .state_id = 1,
        .op = call,
        .kind = XI_CORO_SUSP_CALL,
        .resolved_callee = target,
    };
    XiCoroPlan caller_coro = {
        .is_coroutine = true,
        .nstates = 1,
        .points = &caller_point,
    };
    XiCoroSuspendPoint target_point = {
        .state_id = 1,
        .op = yield,
        .kind = XI_CORO_SUSP_YIELD,
    };
    XiCoroPlan target_coro = {
        .is_coroutine = true,
        .nstates = 1,
        .points = &target_point,
    };
    caller->coro_plan = &caller_coro;
    target->coro_plan = &target_coro;
    root->stage = target->stage = caller->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "shared direct plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(root);
    return plan;
}

static XrSemanticPlan *build_ambiguous_shared_call_target_plan(bool sibling_store) {
    XiFunc *root = xi_func_new("ambiguous_shared_root", &stub_int);
    XiFunc *caller = xi_func_new("ambiguous_shared_caller", &stub_int);
    XiFunc *owner = sibling_store ? xi_func_new("ambiguous_shared_sibling", &stub_int) : root;
    XiFunc *target = xi_func_new("ambiguous_shared_target", &stub_int);
    REQUIRE(root != NULL && caller != NULL && owner != NULL && target != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *caller_entry = xi_block_new(caller);
    XiBlock *owner_entry = sibling_store ? xi_block_new(owner) : root_entry;
    XiBlock *target_entry = xi_block_new(target);
    REQUIRE(root_entry != NULL && caller_entry != NULL && owner_entry != NULL &&
            target_entry != NULL);

    root->children = (XiFunc **) xr_malloc(2 * sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = caller;
    root->children[1] = sibling_store ? owner : target;
    root->nchildren = root->children_cap = 2;
    caller->parent_func = root;
    if (sibling_store) {
        owner->parent_func = root;
        owner->children = (XiFunc **) xr_malloc(sizeof(*owner->children));
        REQUIRE(owner->children != NULL);
        owner->children[0] = target;
        owner->nchildren = owner->children_cap = 1;
        target->parent_func = owner;
    } else {
        target->parent_func = root;
    }

    XiValue *closure = xi_value_new(owner, owner_entry, XI_CLOSURE_NEW, &stub_function, 0);
    XiValue *store = xi_value_new(owner, owner_entry, XI_SET_SHARED, &stub_unit, 1);
    REQUIRE(closure != NULL && store != NULL);
    closure->aux = target;
    store->args[0] = closure;
    store->aux_int = 9;
    if (!sibling_store) {
        XiValue *duplicate_closure =
            xi_value_new(root, root_entry, XI_CLOSURE_NEW, &stub_function, 0);
        XiValue *duplicate = xi_value_new(root, root_entry, XI_SET_SHARED, &stub_unit, 1);
        REQUIRE(duplicate_closure != NULL && duplicate != NULL);
        duplicate_closure->aux = target;
        duplicate->args[0] = duplicate_closure;
        duplicate->aux_int = 9;
    }

    XiValue *load = xi_value_new(caller, caller_entry, XI_GET_SHARED, &stub_function, 0);
    XiValue *call = xi_value_new(caller, caller_entry, XI_CALL, &stub_int, 1);
    XiValue *root_result = xi_const_int(root, root_entry, 1, &stub_int);
    XiValue *owner_result = sibling_store ? xi_const_int(owner, owner_entry, 2, &stub_int) : NULL;
    XiValue *target_result = xi_const_int(target, target_entry, 3, &stub_int);
    REQUIRE(load != NULL && call != NULL && root_result != NULL && target_result != NULL &&
            (!sibling_store || owner_result != NULL));
    load->aux_int = 9;
    call->args[0] = load;
    xi_block_set_return(caller_entry, call);
    xi_block_set_return(root_entry, root_result);
    if (sibling_store)
        xi_block_set_return(owner_entry, owner_result);
    xi_block_set_return(target_entry, target_result);
    root->stage = caller->stage = owner->stage = target->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "ambiguous shared plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(root);
    return plan;
}

static XrSemanticPlan *build_unordered_shared_call_target_plan(bool conditional_store) {
    XiFunc *root = xi_func_new(conditional_store ? "conditional_shared_root"
                                                   : "late_shared_root",
                               &stub_int);
    XiFunc *target = xi_func_new("unordered_shared_target", &stub_int);
    REQUIRE(root != NULL && target != NULL);
    XiBlock *entry = xi_block_new(root);
    XiBlock *target_entry = xi_block_new(target);
    REQUIRE(entry != NULL && target_entry != NULL);
    root->children = (XiFunc **) xr_malloc(sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = target;
    root->nchildren = root->children_cap = 1;
    target->parent_func = root;

    XiValue *load = NULL;
    XiValue *call = NULL;
    if (!conditional_store) {
        load = xi_value_new(root, entry, XI_GET_SHARED, &stub_function, 0);
        XiValue *closure = xi_value_new(root, entry, XI_CLOSURE_NEW, &stub_function, 0);
        XiValue *store = xi_value_new(root, entry, XI_SET_SHARED, &stub_unit, 1);
        call = xi_value_new(root, entry, XI_CALL, &stub_int, 1);
        REQUIRE(load != NULL && closure != NULL && store != NULL && call != NULL);
        load->aux_int = 11;
        closure->aux = target;
        store->args[0] = closure;
        store->aux_int = 11;
        call->args[0] = load;
        xi_block_set_return(entry, call);
    } else {
        XiBlock *store_block = xi_block_new(root);
        XiBlock *skip_block = xi_block_new(root);
        XiBlock *merge = xi_block_new(root);
        REQUIRE(store_block != NULL && skip_block != NULL && merge != NULL);
        XiValue *condition = xi_const_bool(root, entry, true, &stub_bool);
        REQUIRE(condition != NULL);
        xi_block_set_if(entry, condition, store_block, skip_block);
        XiValue *closure =
            xi_value_new(root, store_block, XI_CLOSURE_NEW, &stub_function, 0);
        XiValue *store = xi_value_new(root, store_block, XI_SET_SHARED, &stub_unit, 1);
        REQUIRE(closure != NULL && store != NULL);
        closure->aux = target;
        store->args[0] = closure;
        store->aux_int = 11;
        xi_block_set_jump(store_block, merge);
        xi_block_set_jump(skip_block, merge);
        load = xi_value_new(root, merge, XI_GET_SHARED, &stub_function, 0);
        call = xi_value_new(root, merge, XI_CALL, &stub_int, 1);
        REQUIRE(load != NULL && call != NULL);
        load->aux_int = 11;
        call->args[0] = load;
        xi_block_set_return(merge, call);
        store_block->idom = entry;
        skip_block->idom = entry;
        merge->idom = entry;
    }
    XiValue *target_result = xi_const_int(target, target_entry, 3, &stub_int);
    REQUIRE(target_result != NULL);
    xi_block_set_return(target_entry, target_result);
    root->stage = target->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "unordered shared plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(root);
    return plan;
}

static XrSemanticPlan *build_nonroot_shared_call_target_plan(void) {
    XiFunc *root = xi_func_new("nonroot_shared_root", &stub_int);
    XiFunc *owner = xi_func_new("nonroot_shared_owner", &stub_int);
    XiFunc *target = xi_func_new("nonroot_shared_target", &stub_int);
    XiFunc *caller = xi_func_new("nonroot_shared_caller", &stub_int);
    REQUIRE(root != NULL && owner != NULL && target != NULL && caller != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *owner_entry = xi_block_new(owner);
    XiBlock *target_entry = xi_block_new(target);
    XiBlock *caller_entry = xi_block_new(caller);
    REQUIRE(root_entry != NULL && owner_entry != NULL && target_entry != NULL &&
            caller_entry != NULL);
    root->children = (XiFunc **) xr_malloc(sizeof(*root->children));
    owner->children = (XiFunc **) xr_malloc(2 * sizeof(*owner->children));
    REQUIRE(root->children != NULL && owner->children != NULL);
    root->children[0] = owner;
    root->nchildren = root->children_cap = 1;
    owner->parent_func = root;
    owner->children[0] = target;
    owner->children[1] = caller;
    owner->nchildren = owner->children_cap = 2;
    target->parent_func = owner;
    caller->parent_func = owner;

    XiValue *closure = xi_value_new(owner, owner_entry, XI_CLOSURE_NEW, &stub_function, 0);
    XiValue *store = xi_value_new(owner, owner_entry, XI_SET_SHARED, &stub_unit, 1);
    XiValue *owner_result = xi_const_int(owner, owner_entry, 2, &stub_int);
    REQUIRE(closure != NULL && store != NULL && owner_result != NULL);
    closure->aux = target;
    store->args[0] = closure;
    store->aux_int = 13;
    xi_block_set_return(owner_entry, owner_result);
    XiValue *load = xi_value_new(caller, caller_entry, XI_GET_SHARED, &stub_function, 0);
    XiValue *call = xi_value_new(caller, caller_entry, XI_CALL, &stub_int, 1);
    REQUIRE(load != NULL && call != NULL);
    load->aux_int = 13;
    call->args[0] = load;
    xi_block_set_return(caller_entry, call);
    XiValue *root_result = xi_const_int(root, root_entry, 1, &stub_int);
    XiValue *target_result = xi_const_int(target, target_entry, 3, &stub_int);
    REQUIRE(root_result != NULL && target_result != NULL);
    xi_block_set_return(root_entry, root_result);
    xi_block_set_return(target_entry, target_result);
    root->stage = owner->stage = target->stage = caller->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "nonroot shared plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(root);
    return plan;
}

static XrSemanticPlan *build_unproven_root_parent_shared_plan(bool conditional_store) {
    XiFunc *root = xi_func_new(conditional_store ? "conditional_root_init"
                                                   : "late_root_init",
                               &stub_int);
    XiFunc *target = xi_func_new("root_init_target", &stub_int);
    XiFunc *caller = xi_func_new("root_init_caller", &stub_int);
    REQUIRE(root != NULL && target != NULL && caller != NULL);
    XiBlock *entry = xi_block_new(root);
    XiBlock *target_entry = xi_block_new(target);
    XiBlock *caller_entry = xi_block_new(caller);
    REQUIRE(entry != NULL && target_entry != NULL && caller_entry != NULL);
    root->children = (XiFunc **) xr_malloc(2 * sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = target;
    root->children[1] = caller;
    root->nchildren = root->children_cap = 2;
    target->parent_func = root;
    caller->parent_func = root;

    if (!conditional_store) {
        XiImportRef import_ref = {
            .module_path = "./root_init_barrier",
            .member_name = "barrier",
            .resolved_mod_index = -1,
            .resolved_shared_slot = -1,
            .resolved_export_slot = -1,
        };
        XiValue *callee = xi_value_new(root, entry, XI_IMPORT_REF, &stub_function, 0);
        XiValue *barrier = xi_value_new(root, entry, XI_CALL, &stub_int, 1);
        REQUIRE(callee != NULL && barrier != NULL);
        callee->aux = &import_ref;
        barrier->args[0] = callee;
        XiValue *closure = xi_value_new(root, entry, XI_CLOSURE_NEW, &stub_function, 0);
        XiValue *store = xi_value_new(root, entry, XI_SET_SHARED, &stub_unit, 1);
        REQUIRE(closure != NULL && store != NULL);
        closure->aux = target;
        store->args[0] = closure;
        store->aux_int = 17;
        xi_block_set_return(entry, barrier);
    } else {
        XiBlock *store_block = xi_block_new(root);
        XiBlock *skip_block = xi_block_new(root);
        XiBlock *merge = xi_block_new(root);
        REQUIRE(store_block != NULL && skip_block != NULL && merge != NULL);
        XiValue *condition = xi_const_bool(root, entry, true, &stub_bool);
        REQUIRE(condition != NULL);
        xi_block_set_if(entry, condition, store_block, skip_block);
        XiValue *closure =
            xi_value_new(root, store_block, XI_CLOSURE_NEW, &stub_function, 0);
        XiValue *store = xi_value_new(root, store_block, XI_SET_SHARED, &stub_unit, 1);
        REQUIRE(closure != NULL && store != NULL);
        closure->aux = target;
        store->args[0] = closure;
        store->aux_int = 17;
        xi_block_set_jump(store_block, merge);
        xi_block_set_jump(skip_block, merge);
        XiValue *root_result = xi_const_int(root, merge, 1, &stub_int);
        REQUIRE(root_result != NULL);
        xi_block_set_return(merge, root_result);
        store_block->idom = entry;
        skip_block->idom = entry;
        merge->idom = entry;
    }
    XiValue *load = xi_value_new(caller, caller_entry, XI_GET_SHARED, &stub_function, 0);
    XiValue *call = xi_value_new(caller, caller_entry, XI_CALL, &stub_int, 1);
    XiValue *target_result = xi_const_int(target, target_entry, 3, &stub_int);
    REQUIRE(load != NULL && call != NULL && target_result != NULL);
    load->aux_int = 17;
    call->args[0] = load;
    xi_block_set_return(caller_entry, call);
    xi_block_set_return(target_entry, target_result);
    root->stage = target->stage = caller->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "unproven root parent plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(root);
    return plan;
}

static XrSemanticPlan *build_long_suspend_call_chain(void) {
    enum { CHAIN_LENGTH = 128 };
    XiFunc **functions = (XiFunc **) xr_calloc(CHAIN_LENGTH, sizeof(*functions));
    XiCoroPlan *coroutines =
        (XiCoroPlan *) xr_calloc(CHAIN_LENGTH, sizeof(*coroutines));
    XiCoroSuspendPoint *points =
        (XiCoroSuspendPoint *) xr_calloc(CHAIN_LENGTH, sizeof(*points));
    REQUIRE(functions != NULL && coroutines != NULL && points != NULL);
    for (uint32_t index = 0; index < CHAIN_LENGTH; index++) {
        char name[48];
        REQUIRE(snprintf(name, sizeof(name), "suspend_chain_%u", index) > 0);
        functions[index] = xi_func_new(name, &stub_int);
        REQUIRE(functions[index] != NULL && xi_block_new(functions[index]) != NULL);
        functions[index]->stage = XI_STAGE_OPTIMIZED;
        if (index == 0)
            continue;
        functions[index - 1]->children =
            (XiFunc **) xr_malloc(sizeof(*functions[index - 1]->children));
        REQUIRE(functions[index - 1]->children != NULL);
        functions[index - 1]->children[0] = functions[index];
        functions[index - 1]->nchildren = functions[index - 1]->children_cap = 1;
        functions[index]->parent_func = functions[index - 1];
    }
    for (uint32_t index = 0; index + 1 < CHAIN_LENGTH; index++) {
        XiBlock *entry = functions[index]->entry;
        bool tail = index + 2 == CHAIN_LENGTH;
        XiValue *closure =
            xi_value_new(functions[index], entry,
                         tail ? XI_CLOSURE_NEW : XI_STACK_ALLOC, &stub_function, 0);
        XiValue *call = xi_value_new(functions[index], entry,
                                     tail ? XI_TAIL_CALL : XI_CALL, &stub_int, 1);
        REQUIRE(closure != NULL && call != NULL);
        if (!tail)
            closure->aux_int = XI_CLOSURE_NEW;
        closure->aux = functions[index + 1];
        call->args[0] = closure;
        xi_block_set_return(entry, call);
        if (tail)
            continue;
        points[index] = (XiCoroSuspendPoint) {
            .state_id = 1,
            .op = call,
            .kind = XI_CORO_SUSP_CALL,
            .resolved_callee = functions[index + 1],
        };
        coroutines[index] = (XiCoroPlan) {
            .is_coroutine = true,
            .nstates = 1,
            .points = &points[index],
        };
        functions[index]->coro_plan = &coroutines[index];
    }
    XiBlock *last_entry = functions[CHAIN_LENGTH - 1]->entry;
    XiValue *yield =
        xi_value_new(functions[CHAIN_LENGTH - 1], last_entry, XI_YIELD, &stub_int, 0);
    XiValue *result =
        xi_const_int(functions[CHAIN_LENGTH - 1], last_entry, 1, &stub_int);
    REQUIRE(yield != NULL && result != NULL);
    xi_block_set_return(last_entry, result);
    points[CHAIN_LENGTH - 1] = (XiCoroSuspendPoint) {
        .state_id = 1,
        .op = yield,
        .kind = XI_CORO_SUSP_YIELD,
    };
    coroutines[CHAIN_LENGTH - 1] = (XiCoroPlan) {
        .is_coroutine = true,
        .nstates = 1,
        .points = &points[CHAIN_LENGTH - 1],
    };
    functions[CHAIN_LENGTH - 1]->coro_plan = &coroutines[CHAIN_LENGTH - 1];

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(functions[0], &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "long suspend chain build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(functions[0]);
    xr_free(points);
    xr_free(coroutines);
    xr_free(functions);
    return plan;
}

static XrSemanticPlan *build_capture_contract_plan(void) {
    XiFunc *root = xi_func_new("capture_contract_root", &stub_int);
    XiFunc *child = xi_func_new("capture_contract_child", &stub_int);
    XiFunc *grandchild = xi_func_new("capture_contract_grandchild", &stub_int);
    REQUIRE(root != NULL && child != NULL && grandchild != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    XiBlock *grandchild_entry = xi_block_new(grandchild);
    REQUIRE(root_entry != NULL && child_entry != NULL && grandchild_entry != NULL);
    XiValue *captured = xi_const_str(root, root_entry, "captured", &stub_string);
    XiValue *root_result = xi_const_int(root, root_entry, 1, &stub_int);
    XiValue *child_result = xi_const_int(child, child_entry, 2, &stub_int);
    XiValue *grandchild_result = xi_const_int(grandchild, grandchild_entry, 3, &stub_int);
    REQUIRE(captured != NULL && root_result != NULL && child_result != NULL &&
            grandchild_result != NULL);
    xi_block_set_return(root_entry, root_result);
    xi_block_set_return(child_entry, child_result);
    xi_block_set_return(grandchild_entry, grandchild_result);

    root->children = (XiFunc **) xr_malloc(sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = 1;
    root->children_cap = 1;
    child->parent_func = root;
    child->children = (XiFunc **) xr_malloc(sizeof(*child->children));
    REQUIRE(child->children != NULL);
    child->children[0] = grandchild;
    child->nchildren = 1;
    child->children_cap = 1;
    grandchild->parent_func = child;
    child->ncaptures = 1;
    child->captures[0].source = XI_CAPTURE_SRC_REG;
    child->captures[0].capture_kind = XI_CAPTURE_BY_COPY;
    child->captures[0].name = "captured";
    child->captures[0].type = &stub_string;
    child->captures[0].value = captured;
    child->captures[0].storage_domain = XR_STORAGE_EXEC_LOCAL;
    child->captures[0].value_capability = XR_SEM_VALUE_CONST;
    grandchild->ncaptures = 1;
    grandchild->captures[0].source = XI_CAPTURE_SRC_UPVAL;
    grandchild->captures[0].index = 0;
    grandchild->captures[0].capture_kind = XI_CAPTURE_BY_COPY;
    grandchild->captures[0].name = "captured";
    grandchild->captures[0].type = &stub_string;
    grandchild->captures[0].storage_domain = XR_STORAGE_EXEC_LOCAL;
    grandchild->captures[0].value_capability = XR_SEM_VALUE_CONST;
    root->stage = XI_STAGE_OPTIMIZED;
    child->stage = XI_STAGE_OPTIMIZED;
    grandchild->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "capture-contract plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(root);
    return plan;
}

static XrSemanticPlan *build_entity_identity_plan_with_source(uintptr_t *source_address) {
    XiFunc *root = xi_func_new("entity_identity_root", &stub_int);
    XiFunc *native = xi_func_new("entity_identity_native", &stub_int);
    REQUIRE(root != NULL && native != NULL);
    if (source_address)
        *source_address = (uintptr_t) root;
    root->source_file = "pkg\\identity_probe.xr";
    native->source_file = "pkg/identity_probe.xr";
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *native_entry = xi_block_new(native);
    REQUIRE(root_entry != NULL && native_entry != NULL);
    XiValue *loan = xi_param(root, root_entry, 0, &stub_string);
    XiValue *shape = xi_param(root, root_entry, 1, &stub_shape);
    REQUIRE(loan != NULL && shape != NULL);
    root->nparams = 2;
    root->params = (XiValue **) xr_malloc(2 * sizeof(*root->params));
    REQUIRE(root->params != NULL);
    root->params[0] = loan;
    root->params[1] = shape;
    root->receiver_borrowed = true;
    XiValue *capacity = xi_const_int(root, root_entry, 1, &stub_int);
    XiValue *borrowed_result = xi_value_new(root, root_entry, XI_CODEGEN_OPAQUE, &stub_string, 1);
    XiValue *allocation = xi_value_new(root, root_entry, XI_ARRAY_NEW, &stub_array, 1);
    REQUIRE(capacity != NULL && borrowed_result != NULL && allocation != NULL);
    borrowed_result->args[0] = loan;
    borrowed_result->line = 15;
    set_source_span(borrowed_result, 15, 3, 15, 12);
    allocation->args[0] = capacity;
    allocation->flags = xi_op_default_effects(XI_ARRAY_NEW);
    allocation->line = 16;
    set_source_span(allocation, 15, 3, 15, 12);
    XiValue *suspend = xi_value_new(root, root_entry, XI_YIELD, &stub_int, 0);
    REQUIRE(suspend != NULL);
    suspend->line = 17;
    set_source_span(suspend, 17, 5, 17, 10);
    XiValue *result = xi_const_int(root, root_entry, 9, &stub_int);
    REQUIRE(result != NULL);
    result->line = 18;
    set_source_span(result, 18, 1, 18, 2);
    xi_block_set_return(root_entry, result);
    XiValue *native_result = xi_const_int(native, native_entry, 4, &stub_int);
    REQUIRE(native_result != NULL);
    native_result->line = 21;
    set_source_span(native_result, 21, 1, 21, 2);
    xi_block_set_return(native_entry, native_result);
    native->is_extern = true;
    native->extern_symbol = "identity_native";
    root->children = (XiFunc **) xr_malloc(sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = native;
    root->nchildren = root->children_cap = 1;
    native->parent_func = root;
    XiCoroSuspendPoint point = {
        .state_id = 1,
        .op = suspend,
        .kind = XI_CORO_SUSP_YIELD,
    };
    XiCoroPlan coroutine = {
        .is_coroutine = true,
        .nstates = 1,
        .points = &point,
    };
    root->coro_plan = &coroutine;
    root->stage = native->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "entity-identity plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(root);
    return plan;
}

static XrSemanticPlan *build_entity_identity_plan(void) {
    return build_entity_identity_plan_with_source(NULL);
}

static void test_incomplete_debug_span_fails_closed(void) {
    XiFunc *function = xi_func_new("incomplete_debug_span", &stub_int);
    REQUIRE(function != NULL);
    function->source_file = "pkg/incomplete.xr";
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *result = xi_const_int(function, entry, 1, &stub_int);
    REQUIRE(result != NULL);
    result->line = 7;
    result->source_span = (XiSourceSpan) {7, 2, 0, 0};
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    REQUIRE(!xr_semantic_plan_build(function, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL && strncmp(error, "XR_SEM_0019", strlen("XR_SEM_0019")) == 0);
    xi_func_free(function);
}

static XrSemanticPlan *build_signed_extreme_plan(void) {
    XiFunc *function = xi_func_new("signed_extreme_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *minimum = xi_const_int(function, entry, INT64_MIN, &stub_int);
    XiValue *maximum = xi_const_int(function, entry, INT64_MAX, &stub_int);
    REQUIRE(minimum != NULL && maximum != NULL);
    XiValue *result = xi_binary(function, entry, XI_ADD, &stub_int, minimum, maximum);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "signed-extreme plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_owning_phi_plan(void) {
    XiFunc *function = xi_func_new("owning_phi_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    XiBlock *left = xi_block_new(function);
    XiBlock *right = xi_block_new(function);
    XiBlock *merge = xi_block_new(function);
    REQUIRE(entry != NULL && left != NULL && right != NULL && merge != NULL);

    XiValue *condition = xi_const_bool(function, entry, true, &stub_bool);
    REQUIRE(condition != NULL);
    xi_block_set_if(entry, condition, left, right);
    XiValue *left_value = xi_const_str(function, left, "left", &stub_string);
    XiValue *right_value = xi_const_str(function, right, "right", &stub_string);
    REQUIRE(left_value != NULL && right_value != NULL);
    xi_block_set_jump(left, merge);
    xi_block_set_jump(right, merge);
    XiPhi *phi = xi_phi_new(function, merge, &stub_string, merge->npreds);
    REQUIRE(phi != NULL && merge->npreds == 2);
    phi->value.args[0] = left_value;
    phi->value.args[1] = right_value;
    XiValue *release = xi_value_new(function, merge, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = &phi->value;
    XiValue *result = xi_const_int(function, merge, 0, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(merge, result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "owning-PHI plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_loop_invariant_plan(void) {
    XiFunc *function = xi_func_new("loop_invariant_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    XiBlock *header = xi_block_new(function);
    XiBlock *dispatch = xi_block_new(function);
    XiBlock *left_latch = xi_block_new(function);
    XiBlock *right_latch = xi_block_new(function);
    XiBlock *exit = xi_block_new(function);
    REQUIRE(entry != NULL && header != NULL && dispatch != NULL && left_latch != NULL &&
            right_latch != NULL && exit != NULL);
    XiValue *parameter = xi_param(function, entry, 0, &stub_string);
    REQUIRE(parameter != NULL);
    function->nparams = 1;
    function->params = (XiValue **) xr_malloc(sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = parameter;
    xi_block_set_jump(entry, header);
    XiValue *continue_loop = xi_const_bool(function, header, true, &stub_bool);
    REQUIRE(continue_loop != NULL);
    xi_block_set_if(header, continue_loop, dispatch, exit);
    XiValue *choose_latch = xi_const_bool(function, dispatch, true, &stub_bool);
    REQUIRE(choose_latch != NULL);
    xi_block_set_if(dispatch, choose_latch, left_latch, right_latch);
    xi_block_set_jump(left_latch, header);
    xi_block_set_jump(right_latch, header);
    XiValue *result = xi_const_int(function, exit, 0, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(exit, result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "loop-invariant plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(function);
    return plan;
}

static uint8_t *copy_bytes(const uint8_t *bytes, size_t size) {
    uint8_t *copy = (uint8_t *) xr_malloc(size);
    REQUIRE(copy != NULL);
    memcpy(copy, bytes, size);
    return copy;
}

static void rewrite_payload_digest(uint8_t *bytes, size_t size) {
    REQUIRE(size >= XR_XSM_HEADER_SIZE);
    xr_sha256(bytes + XR_XSM_HEADER_SIZE, size - XR_XSM_HEADER_SIZE, bytes + 24);
}

static size_t find_bytes(const uint8_t *bytes, size_t size, const char *needle) {
    size_t length = strlen(needle);
    if (length == 0 || length > size)
        return SIZE_MAX;
    for (size_t i = 0; i <= size - length; i++) {
        if (memcmp(bytes + i, needle, length) == 0)
            return i;
    }
    return SIZE_MAX;
}

static size_t find_raw_bytes(const uint8_t *bytes, size_t size, const uint8_t *needle,
                             size_t needle_size) {
    if (needle_size == 0 || needle_size > size)
        return SIZE_MAX;
    for (size_t i = 0; i <= size - needle_size; i++) {
        if (memcmp(bytes + i, needle, needle_size) == 0)
            return i;
    }
    return SIZE_MAX;
}

static char *dump_plan(const XrSemanticPlan *plan, size_t *size) {
    FILE *stream = tmpfile();
    REQUIRE(stream != NULL);
    REQUIRE(xr_semantic_plan_dump(plan, stream));
    REQUIRE(fflush(stream) == 0);
    REQUIRE(fseek(stream, 0, SEEK_END) == 0);
    long end = ftell(stream);
    REQUIRE(end >= 0);
    REQUIRE(fseek(stream, 0, SEEK_SET) == 0);
    char *text = (char *) xr_malloc((size_t) end + 1u);
    REQUIRE(text != NULL);
    REQUIRE(fread(text, 1, (size_t) end, stream) == (size_t) end);
    text[end] = '\0';
    fclose(stream);
    if (size)
        *size = (size_t) end;
    return text;
}

static char *dump_entity(const XrSemanticPlan *plan, XrStableId id, size_t *size) {
    FILE *stream = tmpfile();
    REQUIRE(stream != NULL);
    REQUIRE(xr_semantic_plan_dump_entity(plan, id, stream));
    REQUIRE(fflush(stream) == 0);
    REQUIRE(fseek(stream, 0, SEEK_END) == 0);
    long end = ftell(stream);
    REQUIRE(end >= 0);
    REQUIRE(fseek(stream, 0, SEEK_SET) == 0);
    char *text = (char *) xr_malloc((size_t) end + 1u);
    REQUIRE(text != NULL);
    REQUIRE(fread(text, 1, (size_t) end, stream) == (size_t) end);
    text[end] = '\0';
    fclose(stream);
    if (size)
        *size = (size_t) end;
    return text;
}

static void expect_decode_failure(const uint8_t *bytes, size_t size, const char *code) {
    XrSemanticPlan *decoded = NULL;
    char error[512] = {0};
    REQUIRE(!xr_xsm_decode(bytes, size, &decoded, error, sizeof(error)));
    REQUIRE(decoded == NULL);
    if (strncmp(error, code, strlen(code)) != 0)
        fprintf(stderr, "expected decode failure %s, got %s\n", code, error);
    REQUIRE(strncmp(error, code, strlen(code)) == 0);
}

static void expect_verify_failure_at(XrSemanticPlan *plan, const char *code, int line);
#define expect_verify_failure(plan, code) expect_verify_failure_at((plan), (code), __LINE__)

static void forge_direct_call_target(XrSemanticPlan *plan, uint32_t caller,
                                     uint32_t function) {
    REQUIRE(plan != NULL && plan->call_target_count == 0 && plan->call_targets == NULL);
    uint32_t operation = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t index = 0; index < plan->operation_count; index++) {
        if (plan->operations[index].function == caller &&
            plan->operations[index].opcode == XI_CALL) {
            operation = index;
        }
    }
    REQUIRE(operation != XR_SEMANTIC_INDEX_NONE && function < plan->function_count);
    plan->call_targets =
        (XrSemanticCallTargetRecord *) xr_calloc(1, sizeof(*plan->call_targets));
    REQUIRE(plan->call_targets != NULL);
    plan->call_target_count = plan->call_target_capacity = 1;
    XrSemanticCallTargetRecord *target = &plan->call_targets[0];
    target->operation = operation;
    target->function = function;
    target->dependency = XR_SEMANTIC_INDEX_NONE;
    target->source_export = XR_SEMANTIC_INDEX_NONE;
    target->callable_type = XR_SEMANTIC_INDEX_NONE;
    target->kind = XR_SEM_CALL_TARGET_DIRECT_LOCAL;
    char operation_id[XR_STABLE_ID_BYTES * 2 + 1];
    char function_id[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    xr_stable_id_hex(plan->operations[operation].id, operation_id);
    xr_stable_id_hex(plan->functions[function].id, function_id);
    REQUIRE(snprintf(key, sizeof(key),
                     "call-target-v3:schema=%u:operation=%s:function=%s:kind=1",
                     XR_SEMANTIC_SCHEMA_VERSION, operation_id, function_id) > 0);
    bool frozen = plan->frozen;
    plan->frozen = false;
    target->canonical_key = xr_semantic_plan_copy_string(plan, key);
    plan->frozen = frozen;
    REQUIRE(target->canonical_key != NULL &&
            xr_stable_id_from_key(target->canonical_key, &target->id,
                                  &(XrFingerprint) {{0}}));
}

static void test_stable_ids(void) {
    XrStableId id;
    XrFingerprint digest;
    char hex[XR_STABLE_ID_BYTES * 2 + 1];
    REQUIRE(xr_stable_id_from_key("alpha", &id, &digest));
    xr_stable_id_hex(id, hex);
    REQUIRE(strcmp(hex, "9bf6af74f9708ff32171293971b35071") == 0);

    XrSemanticPlan *plan = build_probe_plan();
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(plan, 0);
    REQUIRE(function != NULL);
    xr_stable_id_hex(function->id, hex);
    REQUIRE(strcmp(hex, "0230e87274d99b234c25523c39611e2a") == 0);
    xr_semantic_plan_free(plan);
}

static void test_typed_entity_identity_table(void) {
    uintptr_t source_address = 0;
    XrSemanticPlan *first = build_entity_identity_plan_with_source(&source_address);
    XrSemanticPlan *second = build_entity_identity_plan();
    uint32_t kinds[XR_SEM_ENTITY_KIND_COUNT] = {0};
    for (uint32_t i = 0; i < first->entity_count; i++) {
        const XrSemanticEntityRecord *entity = xr_semantic_plan_entity(first, i);
        REQUIRE(entity != NULL && entity->kind < XR_SEM_ENTITY_KIND_COUNT);
        kinds[entity->kind]++;
        if (i > 0)
            REQUIRE(xr_stable_id_compare(first->entities[i - 1].id, entity->id) < 0);
    }
    for (uint16_t kind = 0; kind < XR_SEM_ENTITY_KIND_COUNT; kind++)
        REQUIRE(kinds[kind] > 0);
    REQUIRE(xr_semantic_plan_entity_count(first) == first->entity_count);
    REQUIRE(xr_fingerprint_equal(xr_semantic_plan_fingerprint(first),
                                 xr_semantic_plan_fingerprint(second)));

    uint8_t *first_bytes = NULL;
    uint8_t *second_bytes = NULL;
    size_t first_size = 0;
    size_t second_size = 0;
    char error[512] = {0};
    REQUIRE(xr_xsm_encode(first, &first_bytes, &first_size, error, sizeof(error)));
    REQUIRE(xr_xsm_encode(second, &second_bytes, &second_size, error, sizeof(error)));
    REQUIRE(first_size == second_size && memcmp(first_bytes, second_bytes, first_size) == 0);
    if (sizeof(source_address) >= 8)
        REQUIRE(find_raw_bytes(first_bytes, first_size, (const uint8_t *) &source_address,
                               sizeof(source_address)) == SIZE_MAX);
    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(first_bytes, first_size, &decoded, error, sizeof(error)));
    REQUIRE(decoded->entity_count == first->entity_count);
    for (uint32_t i = 0; i < first->entity_count; i++) {
        const XrSemanticEntityRecord *left = &first->entities[i];
        const XrSemanticEntityRecord *right = &decoded->entities[i];
        REQUIRE(xr_stable_id_equal(left->id, right->id));
        REQUIRE(left->kind == right->kind && left->subject_kind == right->subject_kind &&
                left->subject == right->subject && left->parent == right->parent &&
                left->ordinal == right->ordinal);
        REQUIRE(strcmp(left->canonical_key, right->canonical_key) == 0);
    }

    uint32_t module = XR_SEMANTIC_INDEX_NONE;
    uint32_t field = XR_SEMANTIC_INDEX_NONE;
    uint32_t operation_loan = XR_SEMANTIC_INDEX_NONE;
    uint32_t first_shared_span = XR_SEMANTIC_INDEX_NONE;
    uint32_t second_shared_span = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < first->entity_count; i++) {
        if (first->entities[i].kind == XR_SEM_ENTITY_MODULE)
            module = i;
        if (first->entities[i].kind == XR_SEM_ENTITY_FIELD)
            field = i;
        if (first->entities[i].kind == XR_SEM_ENTITY_LOAN &&
            first->entities[i].subject_kind == XR_SEM_ENTITY_SUBJECT_OPERATION)
            operation_loan = i;
        if (first->entities[i].kind == XR_SEM_ENTITY_DEBUG_SPAN) {
            const XrSemanticOperationRecord *operation =
                &first->operations[first->entities[i].subject];
            if (operation->source_start_line == 15 && operation->source_discriminator == 1)
                first_shared_span = i;
            if (operation->source_start_line == 15 && operation->source_discriminator == 2)
                second_shared_span = i;
        }
    }
    REQUIRE(module != XR_SEMANTIC_INDEX_NONE && field != XR_SEMANTIC_INDEX_NONE &&
            operation_loan != XR_SEMANTIC_INDEX_NONE &&
            first_shared_span != XR_SEMANTIC_INDEX_NONE &&
            second_shared_span != XR_SEMANTIC_INDEX_NONE);
    const XrSemanticEntityRecord *first_debug = &first->entities[first_shared_span];
    const XrSemanticEntityRecord *second_debug = &first->entities[second_shared_span];
    const XrSemanticOperationRecord *first_debug_operation =
        &first->operations[first_debug->subject];
    const XrSemanticOperationRecord *second_debug_operation =
        &first->operations[second_debug->subject];
    REQUIRE(strcmp(first_debug_operation->source_file, "pkg/identity_probe.xr") == 0);
    REQUIRE(strcmp(second_debug_operation->source_file, "pkg/identity_probe.xr") == 0);
    REQUIRE(first_debug_operation->source_start_column == 3 &&
            first_debug_operation->source_end_line == 15 &&
            first_debug_operation->source_end_column == 12 && first_debug->ordinal == 1);
    REQUIRE(second_debug_operation->source_start_column == 3 &&
            second_debug_operation->source_end_line == 15 &&
            second_debug_operation->source_end_column == 12 && second_debug->ordinal == 2);
    REQUIRE(strstr(first_debug->canonical_key,
                   ":file=21:pkg/identity_probe.xr:start=15:3:end=15:12:"
                   "discriminator=1:operation=") != NULL);
    REQUIRE(strstr(second_debug->canonical_key, "discriminator=2:operation=") != NULL);
    char first_debug_id_hex[XR_STABLE_ID_BYTES * 2 + 1];
    xr_stable_id_hex(first_debug->id, first_debug_id_hex);
    REQUIRE(strcmp(first_debug_id_hex, "45a9007a9c57c76a3f96279317a1ccc9") == 0);
    const XrSemanticOperationRecord *decoded_debug_operation =
        &decoded->operations[first_debug->subject];
    REQUIRE(decoded_debug_operation->source_file != NULL &&
            strcmp(decoded_debug_operation->source_file, first_debug_operation->source_file) == 0 &&
            decoded_debug_operation->source_start_line ==
                first_debug_operation->source_start_line &&
            decoded_debug_operation->source_start_column ==
                first_debug_operation->source_start_column &&
            decoded_debug_operation->source_end_line == first_debug_operation->source_end_line &&
            decoded_debug_operation->source_end_column ==
                first_debug_operation->source_end_column &&
            decoded_debug_operation->source_discriminator ==
                first_debug_operation->source_discriminator);
    size_t debug_dump_size = 0;
    char *debug_dump = dump_entity(first, first_debug->id, &debug_dump_size);
    REQUIRE(debug_dump_size != 0 && strstr(debug_dump, "source-file=21:") != NULL &&
            strstr(debug_dump, "source-span=15:3-15:12 discriminator=1") != NULL);
    xr_free(debug_dump);
    const XrSemanticEntityRecord *loan_entity = &first->entities[operation_loan];
    REQUIRE(strstr(loan_entity->canonical_key, ":declaration=") != NULL);
    REQUIRE(strstr(loan_entity->canonical_key, ":function=") != NULL);
    REQUIRE(strstr(loan_entity->canonical_key, ":operation=") != NULL);
    REQUIRE(strstr(loan_entity->canonical_key, ":ordinal=0:type=") != NULL);
    char loan_id_hex[XR_STABLE_ID_BYTES * 2 + 1];
    xr_stable_id_hex(loan_entity->id, loan_id_hex);
    REQUIRE(strcmp(loan_id_hex, "b739a7ab4d23429e84af3cb719c52b78") == 0);
    size_t entity_dump_size = 0;
    char *entity_dump = dump_entity(first, loan_entity->id, &entity_dump_size);
    REQUIRE(entity_dump_size != 0 && strstr(entity_dump, "kind=12") != NULL &&
            strstr(entity_dump, "source-line=15") != NULL);
    size_t decoded_entity_dump_size = 0;
    char *decoded_entity_dump = dump_entity(decoded, loan_entity->id, &decoded_entity_dump_size);
    REQUIRE(entity_dump_size == decoded_entity_dump_size &&
            memcmp(entity_dump, decoded_entity_dump, entity_dump_size) == 0);
    xr_free(decoded_entity_dump);
    xr_free(entity_dump);
    uint8_t saved_subject_kind = first->entities[module].subject_kind;
    first->entities[module].subject_kind = XR_SEM_ENTITY_SUBJECT_TYPE;
    expect_verify_failure(first, "XR_SEM_0019");
    first->entities[module].subject_kind = saved_subject_kind;
    uint32_t saved_ordinal = first->entities[field].ordinal;
    first->entities[field].ordinal = UINT32_MAX;
    expect_verify_failure(first, "XR_SEM_0019");
    first->entities[field].ordinal = saved_ordinal;
    saved_ordinal = first->entities[operation_loan].ordinal;
    first->entities[operation_loan].ordinal = 1;
    expect_verify_failure(first, "XR_SEM_0019");
    first->entities[operation_loan].ordinal = saved_ordinal;
    XrStableId saved_loan_id = first->entities[operation_loan].id;
    first->entities[operation_loan].id = first->entities[module].id;
    expect_verify_failure(first, "XR_SEM_0003");
    first->entities[operation_loan].id = saved_loan_id;

    xr_semantic_plan_free(decoded);
    xr_free(second_bytes);
    xr_free(first_bytes);
    xr_semantic_plan_free(second);
    xr_semantic_plan_free(first);
}

static void test_immutable_owned_snapshot(void) {
    XrSemanticPlan *plan = build_probe_plan();
    REQUIRE(xr_semantic_plan_is_frozen(plan));
    REQUIRE(xr_semantic_plan_is_verified(plan));
    REQUIRE(xr_semantic_plan_schema(plan) == XR_SEMANTIC_SCHEMA_VERSION);
    XrFingerprint registry_fingerprint;
    xr_semantic_op_registry_fingerprint(&registry_fingerprint);
    char registry_hex[XR_FINGERPRINT_BYTES * 2 + 1];
    char semantic_hex[XR_FINGERPRINT_BYTES * 2 + 1];
    xr_fingerprint_hex(registry_fingerprint, registry_hex);
    xr_fingerprint_hex(xr_semantic_plan_fingerprint(plan), semantic_hex);
    REQUIRE(strcmp(XR_SEMANTIC_OWNER_REGISTRY_FINGERPRINT,
                   "e525dbb47853aec742d8e66317883932200e5075cb56c118f8f70aa28f3026c8") == 0);
    REQUIRE(strcmp(registry_hex,
                   "bef011ef58ee22f1de78b0d6436d705723ec528cbc2fafe6119626fed46f9358") == 0);
    REQUIRE(strcmp(semantic_hex,
                   "b7bfbc07f24d199ae9c589d3c1f277b05742689d41ebf88b3bc8a879d627b190") == 0);
    REQUIRE(xr_fingerprint_equal(registry_fingerprint,
                                 xr_semantic_plan_operation_registry_fingerprint(plan)));
    REQUIRE(xr_semantic_plan_function_count(plan) == 1);
    REQUIRE(xr_semantic_plan_block_count(plan) == 1);
    REQUIRE(xr_semantic_plan_operation_count(plan) == 3);
    REQUIRE(xr_semantic_plan_constant_count(plan) == 3);

    const XrSemanticConstantRecord *string_constant = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_constant_count(plan); i++) {
        const XrSemanticConstantRecord *candidate = xr_semantic_plan_constant(plan, i);
        if (candidate && candidate->kind == XR_SEM_CONST_STRING)
            string_constant = candidate;
    }
    REQUIRE(string_constant != NULL);
    REQUIRE(strcmp(string_constant->string, "owned-by-plan") == 0);

    for (uint32_t i = 1; i < xr_semantic_plan_type_count(plan); i++) {
        const XrSemanticTypeRecord *previous = xr_semantic_plan_type(plan, i - 1);
        const XrSemanticTypeRecord *current = xr_semantic_plan_type(plan, i);
        REQUIRE(previous != NULL && current != NULL);
        REQUIRE(xr_stable_id_compare(previous->id, current->id) < 0);
    }
    const XrOwnershipCertificate *ownership = xr_semantic_plan_ownership(plan);
    REQUIRE(ownership != NULL);
    REQUIRE(xr_ownership_certificate_owner_count(ownership) == 1);
    xr_semantic_plan_free(plan);
}

static void test_string_builder_constructor_allocation_authority(void) {
    XrSemanticPlan *plan = build_string_builder_constructor_plan();
    XrSemanticOperationRecord *constructor = find_operation(plan, XI_CALL_BUILTIN);
    REQUIRE(constructor != NULL && constructor->operand_count == 0 &&
            constructor->metadata_count == 1);
    REQUIRE(strcmp(plan->metadata[constructor->metadata_begin], "StringBuilder") == 0);
    REQUIRE(constructor->result_type < plan->type_count);
    const XrSemanticTypeRecord *type = &plan->types[constructor->result_type];
    REQUIRE(strcmp(type->canonical_key,
                   "type-v3:11:0:20:0:0:0:0:0:0:255:0:;named:13:StringBuilder[0]") == 0);
    REQUIRE(type->flags ==
            (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT));
    REQUIRE(constructor->allocation_key != NULL);
    size_t operation_key_length = strlen(constructor->canonical_key);
    REQUIRE(strncmp(constructor->allocation_key, constructor->canonical_key,
                    operation_key_length) == 0);
    REQUIRE(strcmp(constructor->allocation_key + operation_key_length, "/allocation") == 0);
    REQUIRE(constructor->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
            constructor->return_provenance == XR_SEM_RETURN_OWNED &&
            constructor->return_complete == 1);

    uint8_t *bytes = NULL;
    size_t size = 0;
    char error[512] = {0};
    REQUIRE(xr_xsm_encode(plan, &bytes, &size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(bytes, size, &decoded, error, sizeof(error)));
    XrSemanticOperationRecord *decoded_constructor =
        find_operation(decoded, XI_CALL_BUILTIN);
    REQUIRE(decoded_constructor != NULL && decoded_constructor->allocation_key != NULL);
    REQUIRE(strcmp(decoded_constructor->allocation_key, constructor->allocation_key) == 0);
    REQUIRE(xr_stable_id_equal(decoded_constructor->allocation_id,
                               constructor->allocation_id));
    uint8_t *roundtrip = NULL;
    size_t roundtrip_size = 0;
    REQUIRE(xr_xsm_encode(decoded, &roundtrip, &roundtrip_size, error, sizeof(error)));
    REQUIRE(roundtrip_size == size && memcmp(roundtrip, bytes, size) == 0);
    xr_free(roundtrip);
    xr_semantic_plan_free(decoded);
    xr_free(bytes);

    const char *saved_metadata = plan->metadata[constructor->metadata_begin];
    plan->metadata[constructor->metadata_begin] = "StringBuilded";
    expect_verify_failure(plan, "XR_SEM_0019");
    plan->metadata[constructor->metadata_begin] = saved_metadata;

    uint16_t saved_operand_count = constructor->operand_count;
    constructor->operand_count = 1;
    expect_verify_failure(plan, "XR_SEM_0019");
    constructor->operand_count = saved_operand_count;

    uint32_t saved_result_type = constructor->result_type;
    REQUIRE(plan->type_count > 1);
    constructor->result_type = saved_result_type == 0 ? 1 : 0;
    expect_verify_failure(plan, "XR_SEM_0019");
    constructor->result_type = saved_result_type;

    uint8_t saved_result_ownership = constructor->result_ownership;
    constructor->result_ownership = XI_GEN_RESULT_OWNERSHIP_BORROWED;
    expect_verify_failure(plan, "XR_SEM_0019");
    constructor->result_ownership = saved_result_ownership;

    const char *saved_allocation_key = constructor->allocation_key;
    constructor->allocation_key = NULL;
    expect_verify_failure(plan, "XR_SEM_0019");
    constructor->allocation_key = saved_allocation_key;

    XrStableId saved_allocation_id = constructor->allocation_id;
    constructor->allocation_id.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_SEM_0002");
    constructor->allocation_id = saved_allocation_id;

    char forged_allocation_key[512];
    REQUIRE(snprintf(forged_allocation_key, sizeof(forged_allocation_key), "%s/allocatioN",
                     constructor->canonical_key) > 0);
    constructor->allocation_key = forged_allocation_key;
    REQUIRE(xr_stable_id_from_key(constructor->allocation_key, &constructor->allocation_id,
                                  &(XrFingerprint) {{0}}));
    expect_verify_failure(plan, "XR_SEM_0019");
    constructor->allocation_key = saved_allocation_key;
    constructor->allocation_id = saved_allocation_id;
    REQUIRE(xr_semantic_plan_verify(plan, error, sizeof(error)));
    xr_semantic_plan_free(plan);

    XiFunc *shadow = xi_func_new("shadow_string_builder_constructor_probe", &stub_int);
    REQUIRE(shadow != NULL);
    XiBlock *entry = xi_block_new(shadow);
    REQUIRE(entry != NULL);
    XiValue *call =
        xi_value_new(shadow, entry, XI_CALL_BUILTIN, &stub_shadow_string_builder, 0);
    REQUIRE(call != NULL);
    call->aux = (void *) "StringBuilder";
    XiValue *result = xi_const_int(shadow, entry, 0, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    shadow->stage = XI_STAGE_OPTIMIZED;
    plan = NULL;
    memset(error, 0, sizeof(error));
    REQUIRE(!xr_semantic_plan_build(shadow, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL && strncmp(error, "XR_SEM_0019", strlen("XR_SEM_0019")) == 0);
    xi_func_free(shadow);
}

static void test_operation_registry(void) {
    char error[512] = {0};
    REQUIRE(xr_semantic_op_registry_verify(error, sizeof(error)));
    REQUIRE(xr_semantic_op_contract_count() == XI_OP_COUNT);

    const XrSemanticOpContract *add = xr_semantic_op_contract(XI_ADD);
    const XrSemanticOpContract *decode = xr_semantic_op_contract(XI_JSON_DECODE);
    const XrSemanticOpContract *print = xr_semantic_op_contract(XI_PRINT);
    const XrSemanticOpContract *generated = xr_semantic_op_contract(XI_GEN_CALL);
    const XrSemanticOpContract *logical_not = xr_semantic_op_contract(XI_NOT);
    const XrSemanticOpContract *type_id = xr_semantic_op_contract(XI_TYPEID);
    const XiOp byte_slice_scalar_ops[] = {
        XI_BYTE_SLICE_LOAD_U16,  XI_BYTE_SLICE_LOAD_U32,  XI_BYTE_SLICE_LOAD_U64,
        XI_BYTE_SLICE_LOAD_F32,  XI_BYTE_SLICE_LOAD_F64,  XI_BYTE_SLICE_STORE_U16,
        XI_BYTE_SLICE_STORE_U32, XI_BYTE_SLICE_STORE_U64, XI_BYTE_SLICE_STORE_F32,
        XI_BYTE_SLICE_STORE_F64,
    };
    REQUIRE(add != NULL && strcmp(add->canonical_name, "xi.add") == 0);
    REQUIRE(strcmp(add->canonical_owner, "xi.add") == 0);
    REQUIRE(add->operation_id_hi == add->owner_id_hi);
    REQUIRE(add->operation_id_lo == add->owner_id_lo);
    REQUIRE(add->owner == XR_SEM_OWNER_DECLARATIVE_PRIMITIVE);
    REQUIRE(decode != NULL && decode->owner == XR_SEM_OWNER_SHARED_SEMANTIC_KERNEL);
    REQUIRE(print != NULL && print->owner == XR_SEM_OWNER_CAPABILITY_PROVIDER);
    REQUIRE(generated != NULL && generated->owner == XR_SEM_OWNER_GENERATED_SPECIALIZATION);
    REQUIRE(logical_not != NULL && logical_not->owner == XR_SEM_OWNER_SHARED_SEMANTIC_KERNEL);
    REQUIRE(strcmp(logical_not->canonical_owner, "shared.truthiness") == 0);
    REQUIRE(logical_not->owner_id_hi == XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI);
    REQUIRE(logical_not->owner_id_lo == XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO);
    REQUIRE(logical_not->operation_id_hi != logical_not->owner_id_hi ||
            logical_not->operation_id_lo != logical_not->owner_id_lo);
    REQUIRE(xr_semantic_owner_has_consumer(logical_not->owner_id_hi,
                                           logical_not->owner_id_lo,
                                           XR_SEM_CONSUMER_SEMANTIC_PLAN));
    REQUIRE(type_id != NULL && type_id->owner == XR_SEM_OWNER_DECLARATIVE_PRIMITIVE);
    REQUIRE(strcmp(type_id->canonical_owner, "primitive.type-identity") == 0);
    REQUIRE(type_id->owner_id_hi == XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_HI);
    REQUIRE(type_id->owner_id_lo == XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_LO);
    REQUIRE(type_id->operation_id_hi != type_id->owner_id_hi ||
            type_id->operation_id_lo != type_id->owner_id_lo);
    REQUIRE(xr_semantic_owner_consumer_bits(type_id->owner_id_hi, type_id->owner_id_lo) ==
            (XR_SEM_CONSUMER_SEMANTIC_PLAN | XR_SEM_CONSUMER_VM |
             XR_SEM_CONSUMER_AOT_HOSTED | XR_SEM_CONSUMER_AOT_FREESTANDING |
             XR_SEM_CONSUMER_CGEN | XR_SEM_CONSUMER_RUNTIME));
    for (size_t i = 0; i < sizeof(byte_slice_scalar_ops) / sizeof(byte_slice_scalar_ops[0]); i++) {
        const XrSemanticOpContract *scalar =
            xr_semantic_op_contract(byte_slice_scalar_ops[i]);
        REQUIRE(scalar != NULL);
        REQUIRE(scalar->owner == XR_SEM_OWNER_SHARED_SEMANTIC_KERNEL);
        REQUIRE(strcmp(scalar->canonical_owner, "shared.byte-slice-scalar") == 0);
        REQUIRE(scalar->owner_id_hi == XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_HI);
        REQUIRE(scalar->owner_id_lo == XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_LO);
        REQUIRE(scalar->operation_id_hi != scalar->owner_id_hi ||
                scalar->operation_id_lo != scalar->owner_id_lo);
    }
    REQUIRE(xr_semantic_owner_consumer_bits(
                XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_HI,
                XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_LO) ==
            (XR_SEM_CONSUMER_SEMANTIC_PLAN | XR_SEM_CONSUMER_VM |
             XR_SEM_CONSUMER_AOT_HOSTED | XR_SEM_CONSUMER_AOT_FREESTANDING |
             XR_SEM_CONSUMER_CGEN));
    REQUIRE(strcmp(xr_semantic_owner_cgen_adapter(
                       XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_HI,
                       XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_LO),
                   "xrt_byte_slice_scalar_eval") == 0);
    REQUIRE(xr_semantic_owner_consumer_bits(0, 0) == 0);
    REQUIRE(xr_semantic_owner_cgen_adapter(0, 0) == NULL);
    REQUIRE(xr_semantic_op_contract(XI_OP_COUNT) == NULL);
}

static void test_xsm_roundtrip_and_determinism(void) {
    XrSemanticPlan *first = build_probe_plan();
    XrSemanticPlan *second = build_probe_plan();
    REQUIRE(xr_fingerprint_equal(xr_semantic_plan_fingerprint(first),
                                 xr_semantic_plan_fingerprint(second)));

    uint8_t *first_bytes = NULL;
    uint8_t *second_bytes = NULL;
    size_t first_size = 0;
    size_t second_size = 0;
    char error[512] = {0};
    REQUIRE(xr_xsm_encode(first, &first_bytes, &first_size, error, sizeof(error)));
    REQUIRE(xr_xsm_encode(second, &second_bytes, &second_size, error, sizeof(error)));
    REQUIRE(first_size == second_size);
    REQUIRE(memcmp(first_bytes, second_bytes, first_size) == 0);
    REQUIRE(first_size > XR_XSM_HEADER_SIZE);

    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(first_bytes, first_size, &decoded, error, sizeof(error)));
    REQUIRE(decoded != NULL);
    REQUIRE(xr_semantic_plan_is_verified(decoded));
    REQUIRE(xr_fingerprint_equal(xr_semantic_plan_fingerprint(first),
                                 xr_semantic_plan_fingerprint(decoded)));

    size_t first_dump_size = 0;
    size_t decoded_dump_size = 0;
    char *first_dump = dump_plan(first, &first_dump_size);
    char *decoded_dump = dump_plan(decoded, &decoded_dump_size);
    REQUIRE(first_dump_size == decoded_dump_size);
    REQUIRE(memcmp(first_dump, decoded_dump, first_dump_size) == 0);

    uint8_t *roundtrip = NULL;
    size_t roundtrip_size = 0;
    REQUIRE(xr_xsm_encode(decoded, &roundtrip, &roundtrip_size, error, sizeof(error)));
    REQUIRE(roundtrip_size == first_size);
    REQUIRE(memcmp(roundtrip, first_bytes, first_size) == 0);

    xr_free(decoded_dump);
    xr_free(first_dump);
    xr_free(roundtrip);
    xr_semantic_plan_free(decoded);
    xr_free(second_bytes);
    xr_free(first_bytes);
    xr_semantic_plan_free(second);
    xr_semantic_plan_free(first);
}

static void test_fingerprint_separates_metadata_boundaries(void) {
    XrSemanticPlan *plan = build_typed_call_operand_plan();
    REQUIRE(plan->metadata_count >= 2);
    const char *saved_first = plan->metadata[0];
    const char *saved_second = plan->metadata[1];
    XrFingerprint first;
    XrFingerprint second;
    plan->metadata[0] = "a";
    plan->metadata[1] = "bc";
    xr_semantic_plan_compute_fingerprint(plan, &first);
    plan->metadata[0] = "ab";
    plan->metadata[1] = "c";
    xr_semantic_plan_compute_fingerprint(plan, &second);
    REQUIRE(!xr_fingerprint_equal(first, second));
    plan->metadata[0] = saved_first;
    plan->metadata[1] = saved_second;
    xr_semantic_plan_free(plan);
}

static void test_xsm_signed_extreme_roundtrip(void) {
    XrSemanticPlan *plan = build_signed_extreme_plan();
    uint8_t *bytes = NULL;
    size_t size = 0;
    char error[512] = {0};
    REQUIRE(xr_xsm_encode(plan, &bytes, &size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(bytes, size, &decoded, error, sizeof(error)));
    REQUIRE(decoded->functions[0].return_parameter == -1);
    bool saw_minimum_immediate = false;
    bool saw_maximum_immediate = false;
    bool saw_negative_operation_index = false;
    for (uint32_t i = 0; i < decoded->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &decoded->operations[i];
        saw_minimum_immediate |= operation->semantic_immediate == INT64_MIN;
        saw_maximum_immediate |= operation->semantic_immediate == INT64_MAX;
        saw_negative_operation_index |=
            operation->result_alias_operand == -1 && operation->return_parameter == -1;
    }
    bool saw_minimum_constant = false;
    bool saw_maximum_constant = false;
    for (uint32_t i = 0; i < decoded->constant_count; i++) {
        saw_minimum_constant |= decoded->constants[i].integer == INT64_MIN;
        saw_maximum_constant |= decoded->constants[i].integer == INT64_MAX;
    }
    bool saw_negative_operand_parameter = false;
    for (uint32_t i = 0; i < decoded->operand_count; i++)
        saw_negative_operand_parameter |= decoded->operands[i].parameter == -1;
    REQUIRE(saw_minimum_immediate && saw_maximum_immediate && saw_negative_operation_index);
    REQUIRE(saw_minimum_constant && saw_maximum_constant && saw_negative_operand_parameter);

    const uint8_t encoded_minimum[8] = {0, 0, 0, 0, 0, 0, 0, 0x80};
    size_t minimum_offset =
        find_raw_bytes(bytes + XR_XSM_HEADER_SIZE, size - XR_XSM_HEADER_SIZE, encoded_minimum,
                       sizeof(encoded_minimum));
    REQUIRE(minimum_offset != SIZE_MAX);
    uint8_t *mutation = copy_bytes(bytes, size);
    uint8_t *field = mutation + XR_XSM_HEADER_SIZE + minimum_offset;
    memset(field, 0xff, sizeof(encoded_minimum));
    field[sizeof(encoded_minimum) - 1u] = 0x7f;
    rewrite_payload_digest(mutation, size);
    expect_decode_failure(mutation, size, "XR_ARTIFACT_2002");
    xr_free(mutation);

    xr_semantic_plan_free(decoded);
    xr_free(bytes);
    xr_semantic_plan_free(plan);
}

static void test_explicit_panic_edge_and_roundtrip(void) {
    XrSemanticPlan *plan = build_panic_edge_plan();
    REQUIRE(xr_semantic_plan_edge_count(plan) == 2);
    const XrSemanticEdgeRecord *normal = NULL;
    const XrSemanticEdgeRecord *panic = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_edge_count(plan); i++) {
        const XrSemanticEdgeRecord *edge = xr_semantic_plan_edge(plan, i);
        REQUIRE(edge != NULL);
        if (edge->kind == XR_SEM_EDGE_NORMAL)
            normal = edge;
        if (edge->kind == XR_SEM_EDGE_PANIC)
            panic = edge;
    }
    REQUIRE(normal != NULL && panic != NULL);
    REQUIRE(normal->from_block == 0 && normal->to_block == 1);
    REQUIRE(normal->operation == XR_SEMANTIC_INDEX_NONE);
    REQUIRE(panic->from_block == 0 && panic->to_block == 2);
    REQUIRE(panic->operation < xr_semantic_plan_operation_count(plan));
    REQUIRE(xr_semantic_plan_operation(plan, panic->operation)->opcode == XI_TRY);

    uint8_t *bytes = NULL;
    size_t size = 0;
    char error[512] = {0};
    REQUIRE(xr_xsm_encode(plan, &bytes, &size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(bytes, size, &decoded, error, sizeof(error)));
    REQUIRE(xr_semantic_plan_edge_count(decoded) == 2);
    REQUIRE(xr_fingerprint_equal(xr_semantic_plan_fingerprint(plan),
                                 xr_semantic_plan_fingerprint(decoded)));
    xr_semantic_plan_free(decoded);
    xr_free(bytes);
    xr_semantic_plan_free(plan);
}

static void test_explicit_error_edge(void) {
    XrSemanticPlan *plan = build_error_edge_plan();
    REQUIRE(xr_semantic_plan_edge_count(plan) == 2);
    const XrSemanticEdgeRecord *error_edge = NULL;
    const XrSemanticEdgeRecord *normal_edge = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_edge_count(plan); i++) {
        const XrSemanticEdgeRecord *edge = xr_semantic_plan_edge(plan, i);
        REQUIRE(edge != NULL);
        if (edge->kind == XR_SEM_EDGE_ERROR)
            error_edge = edge;
        if (edge->kind == XR_SEM_EDGE_NORMAL)
            normal_edge = edge;
    }
    REQUIRE(error_edge != NULL && normal_edge != NULL);
    REQUIRE(error_edge->from_block == 0 && error_edge->to_block == 1);
    REQUIRE(error_edge->operation < xr_semantic_plan_operation_count(plan));
    REQUIRE(xr_semantic_plan_operation(plan, error_edge->operation)->opcode == XI_ERR_CHECK);
    REQUIRE(normal_edge->from_block == 0 && normal_edge->to_block == 2);
    xr_semantic_plan_free(plan);
}

static void test_typed_call_operand_contract(void) {
    XrSemanticPlan *plan = build_typed_call_operand_plan();
    const XrSemanticOperationRecord *call = NULL;
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        if (plan->operations[i].opcode == XI_CALL) {
            call = &plan->operations[i];
            break;
        }
    }
    REQUIRE(call != NULL && call->operand_count == 3);
    XrSemanticOperandRecord *callee = &plan->operands[call->operand_begin];
    XrSemanticOperandRecord *first = &plan->operands[call->operand_begin + 1];
    XrSemanticOperandRecord *second = &plan->operands[call->operand_begin + 2];
    REQUIRE(callee->role == XR_SEM_OPERAND_CALLEE && callee->parameter == -1);
    REQUIRE((callee->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0);
    REQUIRE(first->role == XR_SEM_OPERAND_ARGUMENT && first->parameter == 0);
    REQUIRE(first->parameter_mode == XR_PARAM_REF && first->access == XR_CALL_ARG_REF);
    REQUIRE(first->origin == XI_PLACE_ORIGIN_STACK_LOCAL);
    REQUIRE(first->lifetime == XI_PLACE_LIFETIME_CALL_BOUND);
    REQUIRE((first->flags & (XR_SEM_OPERAND_CALL_CONTRACT | XR_SEM_OPERAND_ADDRESSABLE)) ==
            (XR_SEM_OPERAND_CALL_CONTRACT | XR_SEM_OPERAND_ADDRESSABLE));
    REQUIRE(second->role == XR_SEM_OPERAND_ARGUMENT && second->parameter == 1);
    REQUIRE(second->parameter_mode == XR_PARAM_MOVE && second->access == XR_CALL_ARG_MOVE);

    uint32_t saved_type = first->type;
    first->type = callee->type;
    expect_verify_failure(plan, "XR_SEM_0015");
    first->type = saved_type;
    uint8_t saved_role = first->role;
    first->role = XR_SEM_OPERAND_RECEIVER;
    expect_verify_failure(plan, "XR_SEM_0018");
    first->role = saved_role;

    uint8_t *bytes = NULL;
    size_t size = 0;
    char error[512] = {0};
    REQUIRE(xr_xsm_encode(plan, &bytes, &size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(bytes, size, &decoded, error, sizeof(error)));
    const XrSemanticOperationRecord *decoded_call = NULL;
    for (uint32_t i = 0; i < decoded->operation_count; i++) {
        if (decoded->operations[i].opcode == XI_CALL) {
            decoded_call = &decoded->operations[i];
            break;
        }
    }
    REQUIRE(decoded_call != NULL);
    const XrSemanticOperandRecord *decoded_first =
        &decoded->operands[decoded_call->operand_begin + 1];
    REQUIRE(decoded_first->type == saved_type && decoded_first->parameter == 0);
    REQUIRE(decoded_first->parameter_mode == XR_PARAM_REF &&
            decoded_first->access == XR_CALL_ARG_REF);
    xr_semantic_plan_free(decoded);
    xr_free(bytes);
    xr_semantic_plan_free(plan);
}

static void test_direct_local_call_target_authority(void) {
    XrSemanticPlan *plan = build_direct_local_call_target_plan();
    REQUIRE(xr_semantic_plan_call_target_count(plan) == 1);
    const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(plan, 0);
    REQUIRE(target != NULL && target->operation < plan->operation_count);
    REQUIRE(target->function == 1 && target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL);
    REQUIRE(plan->operations[target->operation].opcode == XI_CALL);
    REQUIRE(plan->operations[target->operation].effects == xi_generated_op_effects(XI_CALL));
    REQUIRE(strstr(target->canonical_key, "call-target-v3:schema=19:operation=") != NULL);
    REQUIRE(strstr(target->canonical_key, ":function=") != NULL);
    REQUIRE(strstr(target->canonical_key, ":kind=1") != NULL);
    char target_id_hex[XR_STABLE_ID_BYTES * 2 + 1];
    xr_stable_id_hex(target->id, target_id_hex);
    REQUIRE(strcmp(target_id_hex, "6eacef5db5440805816d3a95eb9a120a") == 0);

    uint32_t coroutine_states = 0;
    bool call_has_state = false;
    for (uint32_t i = 0; i < plan->entity_count; i++) {
        const XrSemanticEntityRecord *entity = &plan->entities[i];
        if (entity->kind != XR_SEM_ENTITY_COROUTINE_STATE)
            continue;
        coroutine_states++;
        call_has_state |= entity->subject == target->operation;
    }
    /* A frozen direct target takes priority and can independently ground a state. */
    REQUIRE(coroutine_states == 2 && call_has_state);

    bool saw_binding = false;
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        if (plan->operations[i].opcode == XI_STACK_ALLOC &&
            plan->operations[i].semantic_immediate == XI_CLOSURE_NEW) {
            REQUIRE(plan->operations[i].callable_function == target->function);
            saw_binding = true;
        }
    }
    REQUIRE(saw_binding);

    uint8_t *bytes = NULL;
    size_t size = 0;
    char error[512] = {0};
    REQUIRE(xr_xsm_encode(plan, &bytes, &size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(bytes, size, &decoded, error, sizeof(error)));
    const XrSemanticCallTargetRecord *decoded_target =
        xr_semantic_plan_call_target(decoded, 0);
    REQUIRE(decoded_target != NULL && decoded_target->operation == target->operation &&
            decoded_target->function == target->function &&
            strcmp(decoded_target->canonical_key, target->canonical_key) == 0 &&
            xr_stable_id_equal(decoded_target->id, target->id));
    uint8_t *roundtrip_bytes = NULL;
    size_t roundtrip_size = 0;
    REQUIRE(xr_xsm_encode(decoded, &roundtrip_bytes, &roundtrip_size, error, sizeof(error)));
    REQUIRE(roundtrip_size == size && memcmp(roundtrip_bytes, bytes, size) == 0);
    xr_free(roundtrip_bytes);
    xr_semantic_plan_free(decoded);

    uint8_t *mutation = copy_bytes(bytes, size);
    size_t key_offset = find_bytes(mutation + XR_XSM_HEADER_SIZE,
                                   size - XR_XSM_HEADER_SIZE, target->canonical_key);
    REQUIRE(key_offset != SIZE_MAX && strlen(target->canonical_key) > 8);
    mutation[XR_XSM_HEADER_SIZE + key_offset + 8] ^= 0x01;
    rewrite_payload_digest(mutation, size);
    expect_decode_failure(mutation, size, "XR_ARTIFACT_2002");
    xr_free(mutation);
    xr_free(bytes);

    uint32_t saved_function = plan->call_targets[0].function;
    plan->call_targets[0].function = 0;
    expect_verify_failure(plan, "XR_SEM_0019");
    plan->call_targets[0].function = saved_function;
    uint8_t saved_kind = plan->call_targets[0].kind;
    plan->call_targets[0].kind = 0;
    expect_verify_failure(plan, "XR_SEM_0019");
    plan->call_targets[0].kind = saved_kind;
    uint32_t saved_count = plan->call_target_count;
    plan->call_target_count = 0;
    expect_verify_failure(plan, "XR_SEM_0019");
    plan->call_target_count = saved_count;
    xr_semantic_plan_free(plan);

    XrSemanticPlan *unknown = build_typed_call_operand_plan();
    REQUIRE(xr_semantic_plan_call_target_count(unknown) == 0);
    xr_semantic_plan_free(unknown);
}

static void test_indirect_callable_state_authority(void) {
    XrSemanticPlan *plan = build_indirect_callable_plan(XI_CALL, true);
    REQUIRE(plan->call_target_count == 1);
    XrSemanticCallTargetRecord *target = &plan->call_targets[0];
    REQUIRE(target->kind == XR_SEM_CALL_TARGET_INDIRECT_CALLABLE);
    REQUIRE(target->function == XR_SEMANTIC_INDEX_NONE &&
            target->dependency == XR_SEMANTIC_INDEX_NONE &&
            target->source_export == XR_SEMANTIC_INDEX_NONE &&
            target->callable_type < plan->type_count &&
            plan->types[target->callable_type].kind == XR_KIND_FUNCTION);
    REQUIRE(plan->operations[target->operation].opcode == XI_CALL);
    REQUIRE(strstr(target->canonical_key,
                   "call-target-v3:schema=19:operation=") != NULL);
    REQUIRE(strstr(target->canonical_key, ":callable-type=") != NULL);
    REQUIRE(strstr(target->canonical_key, ":kind=4") != NULL);
    char target_id_hex[XR_STABLE_ID_BYTES * 2 + 1];
    xr_stable_id_hex(target->id, target_id_hex);
    REQUIRE(strcmp(target_id_hex, "3c38c0d5d6846d455f10cc5764f4ed3b") == 0);
    uint32_t state_count = 0;
    for (uint32_t index = 0; index < plan->entity_count; index++)
        state_count += plan->entities[index].kind == XR_SEM_ENTITY_COROUTINE_STATE &&
                       plan->entities[index].subject == target->operation;
    REQUIRE(state_count == 1);

    char error[512] = {0};
    uint8_t *bytes = NULL;
    size_t size = 0;
    REQUIRE(xr_xsm_encode(plan, &bytes, &size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(bytes, size, &decoded, error, sizeof(error)));
    REQUIRE(decoded->call_target_count == 1 &&
            decoded->call_targets[0].kind == XR_SEM_CALL_TARGET_INDIRECT_CALLABLE &&
            decoded->call_targets[0].callable_type == target->callable_type &&
            strcmp(decoded->call_targets[0].canonical_key,
                   target->canonical_key) == 0);
    uint8_t *roundtrip = NULL;
    size_t roundtrip_size = 0;
    REQUIRE(xr_xsm_encode(decoded, &roundtrip, &roundtrip_size, error,
                          sizeof(error)));
    REQUIRE(roundtrip_size == size && memcmp(roundtrip, bytes, size) == 0);
    xr_free(roundtrip);
    xr_semantic_plan_free(decoded);

    uint8_t *old_schema = copy_bytes(bytes, size);
    old_schema[8] = 18;
    old_schema[9] = old_schema[10] = old_schema[11] = 0;
    expect_decode_failure(old_schema, size, "XR_ARTIFACT_2000");
    xr_free(old_schema);
    xr_free(bytes);

    uint32_t saved_type = target->callable_type;
    target->callable_type = XR_SEMANTIC_INDEX_NONE;
    expect_verify_failure(plan, "XR_SEM_0019");
    target->callable_type = saved_type;
    uint32_t saved_function = target->function;
    target->function = 0;
    expect_verify_failure(plan, "XR_SEM_0019");
    target->function = saved_function;
    uint8_t saved_kind = target->kind;
    target->kind = XR_SEM_CALL_TARGET_NATIVE_YIELDABLE;
    expect_verify_failure(plan, "XR_SEM_0019");
    target->kind = saved_kind;
    uint32_t saved_target_count = plan->call_target_count;
    plan->call_target_count = 0;
    expect_verify_failure(plan, "XR_SEM_0019");
    plan->call_target_count = saved_target_count;

    uint32_t saved_entity_count = plan->entity_count;
    XrSemanticEntityRecord *saved_entities = plan->entities;
    XrSemanticEntityRecord compacted[64];
    REQUIRE(saved_entity_count <= (uint32_t) (sizeof(compacted) / sizeof(compacted[0])));
    uint32_t compacted_count = 0;
    for (uint32_t index = 0; index < saved_entity_count; index++)
        if (saved_entities[index].kind != XR_SEM_ENTITY_COROUTINE_STATE)
            compacted[compacted_count++] = saved_entities[index];
    plan->entities = compacted;
    plan->entity_count = compacted_count;
    expect_verify_failure(plan, "XR_SEM_0019");
    plan->entities = saved_entities;
    plan->entity_count = saved_entity_count;
    xr_semantic_plan_free(plan);

    XrSemanticPlan *tail = build_indirect_callable_plan(XI_TAIL_CALL, false);
    REQUIRE(tail->call_target_count == 0);
    xr_semantic_plan_free(tail);
    XrSemanticPlan *method = build_indirect_callable_plan(XI_CALL_METHOD, false);
    REQUIRE(method->call_target_count == 0);
    xr_semantic_plan_free(method);
    XrSemanticPlan *builtin = build_indirect_callable_plan(XI_CALL_BUILTIN, false);
    REQUIRE(builtin->call_target_count == 0);
    xr_semantic_plan_free(builtin);
}

static void test_source_export_call_target_authority(void) {
    XrSemanticPlan *dependency = NULL;
    XrSemanticPlan *plan = build_source_export_call_target_plan(&dependency);
    REQUIRE(plan != NULL && dependency != NULL);
    REQUIRE(xr_semantic_plan_is_verified(plan));
    REQUIRE(plan->dependency_count == 1 && dependency->source_export_count == 1);
    REQUIRE(plan->call_target_count == 1);
    XrSemanticCallTargetRecord *target = &plan->call_targets[0];
    REQUIRE(target->kind == XR_SEM_CALL_TARGET_SOURCE_EXPORT);
    REQUIRE(target->dependency == 0 && target->source_export == 0);
    REQUIRE(xr_stable_id_equal(target->export_identity, dependency->source_exports[0].id));
    REQUIRE(xr_stable_id_equal(target->callee_function,
                               dependency->functions[dependency->source_exports[0].function].id));
    REQUIRE(plan->operations[target->operation].opcode == XI_CALL_METHOD);
    REQUIRE(plan->operations[target->operation].metadata_count == 1);
    REQUIRE(strcmp(plan->metadata[plan->operations[target->operation].metadata_begin],
                   "writeBytes") == 0);
    char dependency_id[XR_STABLE_ID_BYTES * 2 + 1];
    char export_id[XR_STABLE_ID_BYTES * 2 + 1];
    char target_id[XR_STABLE_ID_BYTES * 2 + 1];
    xr_stable_id_hex(plan->dependencies[0].id, dependency_id);
    xr_stable_id_hex(dependency->source_exports[0].id, export_id);
    xr_stable_id_hex(target->id, target_id);
    REQUIRE(strcmp(dependency_id, "4053cf01b76a02be3410f81c48b46ed6") == 0);
    REQUIRE(strcmp(export_id, "b25c977fe6c825cd2589a7d44cdd8729") == 0);
    REQUIRE(strcmp(target_id, "b31e666444c28a8032c607995d44c558") == 0);
    const XrSemanticPlan *dependencies[] = {dependency};
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_verify_module_set(plan, dependencies, 1, error,
                                               sizeof(error)));

    uint8_t *bytes = NULL;
    size_t size = 0;
    REQUIRE(xr_xsm_encode(plan, &bytes, &size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    REQUIRE(!xr_xsm_decode(bytes, size, &decoded, error, sizeof(error)));
    REQUIRE(decoded == NULL && strncmp(error, "XR_ARTIFACT_2004", 16) == 0);
    memset(error, 0, sizeof(error));
    REQUIRE(xr_xsm_decode_module_set(bytes, size, dependencies, 1, &decoded, error,
                                     sizeof(error)));
    REQUIRE(decoded != NULL && xr_semantic_plan_is_verified(decoded));
    REQUIRE(decoded->dependency_count == 1 && decoded->call_target_count == 1);
    xr_semantic_plan_free(decoded);
    xr_free(bytes);

    XrStableId saved_export = target->export_identity;
    target->export_identity.bytes[0] ^= 1;
    REQUIRE(!xr_semantic_plan_verify_module_set(plan, dependencies, 1, error,
                                                sizeof(error)));
    target->export_identity = saved_export;
    uint32_t saved_dependency = target->dependency;
    target->dependency = XR_SEMANTIC_INDEX_NONE;
    expect_verify_failure(plan, "XR_SEM_0019");
    target->dependency = saved_dependency;
    uint32_t saved_source_export = target->source_export;
    target->source_export = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(!xr_semantic_plan_verify_module_set(plan, dependencies, 1, error,
                                                sizeof(error)));
    target->source_export = saved_source_export;
    XrStableId saved_callee = target->callee_function;
    target->callee_function.bytes[0] ^= 1;
    REQUIRE(!xr_semantic_plan_verify_module_set(plan, dependencies, 1, error,
                                                sizeof(error)));
    target->callee_function = saved_callee;
    XrSemanticOperationRecord *call = &plan->operations[target->operation];
    int64_t saved_immediate = call->semantic_immediate;
    call->semantic_immediate |= 1;
    expect_verify_failure(plan, "XR_SEM_0019");
    call->semantic_immediate = saved_immediate;
    const char *saved_selector = plan->metadata[call->metadata_begin];
    const XrSemanticOperationRecord *import = NULL;
    for (uint32_t operation = 0; operation < plan->operation_count; operation++)
        if (plan->operations[operation].opcode == XI_IMPORT_REF)
            import = &plan->operations[operation];
    REQUIRE(import != NULL && import->metadata_count == 2);
    plan->metadata[call->metadata_begin] =
        plan->metadata[import->metadata_begin + 1];
    expect_verify_failure(plan, "XR_SEM_0019");
    plan->metadata[call->metadata_begin] = saved_selector;
    XrFingerprint saved_fingerprint = plan->dependencies[0].semantic_fingerprint;
    plan->dependencies[0].semantic_fingerprint.bytes[0] ^= 1;
    REQUIRE(!xr_semantic_plan_verify_module_set(plan, dependencies, 1, error,
                                                sizeof(error)));
    plan->dependencies[0].semantic_fingerprint = saved_fingerprint;
    const XrSemanticPlan *wrong_dependencies[] = {plan};
    REQUIRE(!xr_semantic_plan_verify_module_set(plan, wrong_dependencies, 1, error,
                                                sizeof(error)));
    bool saved_verified = dependency->verified;
    dependency->verified = false;
    REQUIRE(!xr_semantic_plan_verify_module_set(plan, dependencies, 1, error,
                                                sizeof(error)));
    dependency->verified = saved_verified;
    REQUIRE(!xr_semantic_plan_verify_module_set(plan, NULL, 0, error, sizeof(error)));
    xr_semantic_plan_free(plan);
    xr_semantic_plan_free(dependency);
}

static void test_native_yieldable_call_target_authority(void) {
    XrSemanticPlan *plan = build_native_yieldable_call_target_plan(
        "os", "__sleep", XI_CALL, 1, true, true);
    REQUIRE(xr_semantic_plan_call_target_count(plan) == 1);
    XrSemanticCallTargetRecord *target = &plan->call_targets[0];
    REQUIRE(target->kind == XR_SEM_CALL_TARGET_NATIVE_YIELDABLE &&
            target->function == XR_SEMANTIC_INDEX_NONE &&
            target->operation < plan->operation_count &&
            plan->operations[target->operation].opcode == XI_CALL);
    REQUIRE(strstr(target->canonical_key,
                   "call-target-v3:schema=19:operation=") != NULL);
    REQUIRE(strstr(target->canonical_key, ":native=os.__sleep:kind=2") != NULL);
    uint32_t state_count = 0;
    for (uint32_t index = 0; index < plan->entity_count; index++)
        state_count += plan->entities[index].kind == XR_SEM_ENTITY_COROUTINE_STATE;
    REQUIRE(state_count == 1);

    char error[512] = {0};
    uint8_t *bytes = NULL;
    size_t size = 0;
    REQUIRE(xr_xsm_encode(plan, &bytes, &size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(bytes, size, &decoded, error, sizeof(error)));
    REQUIRE(decoded->call_target_count == 1 &&
            decoded->call_targets[0].kind == XR_SEM_CALL_TARGET_NATIVE_YIELDABLE &&
            decoded->call_targets[0].function == XR_SEMANTIC_INDEX_NONE &&
            xr_fingerprint_equal(decoded->stdlib_registry_fingerprint,
                                 plan->stdlib_registry_fingerprint));
    xr_semantic_plan_free(decoded);
    xr_free(bytes);

    uint32_t saved_kind = target->kind;
    target->kind = XR_SEM_CALL_TARGET_DIRECT_LOCAL;
    expect_verify_failure(plan, "XR_SEM_0019");
    target->kind = saved_kind;
    uint32_t saved_function = target->function;
    target->function = 0;
    expect_verify_failure(plan, "XR_SEM_0019");
    target->function = saved_function;
    char *mutable_key = (char *) target->canonical_key;
    char saved_key_byte = mutable_key[0];
    mutable_key[0] ^= 1;
    expect_verify_failure(plan, "XR_SEM_0019");
    mutable_key[0] = saved_key_byte;
    XrFingerprint saved_registry = plan->stdlib_registry_fingerprint;
    plan->stdlib_registry_fingerprint.bytes[0] ^= 1;
    expect_verify_failure(plan, "XR_SEM_0017");
    plan->stdlib_registry_fingerprint = saved_registry;
    uint32_t saved_entity_count = plan->entity_count;
    XrSemanticEntityRecord *saved_entities = plan->entities;
    XrSemanticEntityRecord compacted[64];
    REQUIRE(saved_entity_count <= (uint32_t) (sizeof(compacted) / sizeof(compacted[0])));
    uint32_t compacted_count = 0;
    for (uint32_t index = 0; index < saved_entity_count; index++)
        if (saved_entities[index].kind != XR_SEM_ENTITY_COROUTINE_STATE)
            compacted[compacted_count++] = saved_entities[index];
    plan->entities = compacted;
    plan->entity_count = compacted_count;
    expect_verify_failure(plan, "XR_SEM_0019");
    plan->entities = saved_entities;
    plan->entity_count = saved_entity_count;
    xr_semantic_plan_free(plan);

    XrSemanticPlan *unknown = build_native_yieldable_call_target_plan(
        "os", "__not_a_native", XI_CALL, 1, false, false);
    REQUIRE(unknown->call_target_count == 0);
    xr_semantic_plan_free(unknown);
    XrSemanticPlan *non_yieldable = build_native_yieldable_call_target_plan(
        "os", "__getpid", XI_CALL, 0, false, false);
    REQUIRE(non_yieldable->call_target_count == 0);
    xr_semantic_plan_free(non_yieldable);
    XrSemanticPlan *wrong_arity = build_native_yieldable_call_target_plan(
        "os", "__sleep", XI_CALL, 0, false, false);
    REQUIRE(wrong_arity->call_target_count == 0);
    xr_semantic_plan_free(wrong_arity);
    XrSemanticPlan *relative = build_native_yieldable_call_target_plan(
        "./os", "__sleep", XI_CALL, 1, false, false);
    REQUIRE(relative->call_target_count == 0);
    xr_semantic_plan_free(relative);
    XrSemanticPlan *tail = build_native_yieldable_call_target_plan(
        "os", "__sleep", XI_TAIL_CALL, 1, false, false);
    REQUIRE(tail->call_target_count == 0);
    xr_semantic_plan_free(tail);
    XrSemanticPlan *method = build_native_yieldable_call_target_plan(
        "os", "__sleep", XI_CALL_METHOD, 1, false, false);
    REQUIRE(method->call_target_count == 0);
    xr_semantic_plan_free(method);
}

static void test_native_namespace_yieldable_authority(void) {
    XrSemanticPlan *plan = build_native_namespace_yieldable_plan(
        "time", "sleep", true, false, true);
    REQUIRE(plan->call_target_count == 1);
    XrSemanticCallTargetRecord *target = &plan->call_targets[0];
    REQUIRE(target->kind ==
            XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE);
    REQUIRE(target->function == XR_SEMANTIC_INDEX_NONE &&
            target->dependency == XR_SEMANTIC_INDEX_NONE &&
            target->source_export == XR_SEMANTIC_INDEX_NONE &&
            target->callable_type == XR_SEMANTIC_INDEX_NONE);
    REQUIRE(strstr(target->canonical_key,
                   "call-target-v5:schema=19:operation=") != NULL);
    REQUIRE(strstr(target->canonical_key,
                   ":native-namespace=time.sleep:kind=5") != NULL);
    char target_id_hex[XR_STABLE_ID_BYTES * 2 + 1];
    xr_stable_id_hex(target->id, target_id_hex);
    REQUIRE(strcmp(target_id_hex, "98b2e28b0bac714a98cb5d93cf447111") == 0);
    const XrSemanticOperationRecord *import = NULL;
    for (uint32_t i = 0; i < plan->operation_count; i++)
        if (plan->operations[i].opcode == XI_IMPORT_REF)
            import = &plan->operations[i];
    REQUIRE(import != NULL && import->import_resolution ==
                                  XR_SEM_IMPORT_RESOLUTION_NATIVE_STDLIB);
    uint32_t states = 0;
    for (uint32_t i = 0; i < plan->entity_count; i++)
        states += plan->entities[i].kind == XR_SEM_ENTITY_COROUTINE_STATE &&
                  plan->entities[i].subject == target->operation;
    REQUIRE(states == 1);

    char error[512] = {0};
    uint8_t *bytes = NULL;
    size_t size = 0;
    REQUIRE(xr_xsm_encode(plan, &bytes, &size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(bytes, size, &decoded, error, sizeof(error)));
    REQUIRE(decoded->call_target_count == 1 &&
            decoded->call_targets[0].kind ==
                XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE);
    const XrSemanticOperationRecord *decoded_import = NULL;
    for (uint32_t i = 0; i < decoded->operation_count; i++)
        if (decoded->operations[i].opcode == XI_IMPORT_REF)
            decoded_import = &decoded->operations[i];
    REQUIRE(decoded_import != NULL &&
            decoded_import->import_resolution ==
                XR_SEM_IMPORT_RESOLUTION_NATIVE_STDLIB);
    uint8_t *roundtrip = NULL;
    size_t roundtrip_size = 0;
    REQUIRE(xr_xsm_encode(decoded, &roundtrip, &roundtrip_size, error,
                          sizeof(error)));
    REQUIRE(roundtrip_size == size && memcmp(roundtrip, bytes, size) == 0);
    xr_free(roundtrip);
    xr_semantic_plan_free(decoded);
    uint8_t *old_schema = copy_bytes(bytes, size);
    old_schema[8] = 18;
    old_schema[9] = old_schema[10] = old_schema[11] = 0;
    expect_decode_failure(old_schema, size, "XR_ARTIFACT_2000");
    xr_free(old_schema);
    xr_free(bytes);

    uint8_t saved_resolution =
        ((XrSemanticOperationRecord *) import)->import_resolution;
    ((XrSemanticOperationRecord *) import)->import_resolution =
        XR_SEM_IMPORT_RESOLUTION_UNRESOLVED;
    expect_verify_failure(plan, "XR_SEM_0019");
    ((XrSemanticOperationRecord *) import)->import_resolution = saved_resolution;
    uint8_t saved_kind = target->kind;
    target->kind = XR_SEM_CALL_TARGET_NATIVE_YIELDABLE;
    expect_verify_failure(plan, "XR_SEM_0019");
    target->kind = saved_kind;
    uint32_t saved_operation = target->operation;
    target->operation = target->operation > 0 ? target->operation - 1 : target->operation + 1;
    expect_verify_failure(plan, "XR_SEM_0019");
    target->operation = saved_operation;
    uint32_t saved_function = target->function;
    target->function = 0;
    expect_verify_failure(plan, "XR_SEM_0019");
    target->function = saved_function;
    char *mutable_key = (char *) target->canonical_key;
    char saved_key_byte = mutable_key[0];
    mutable_key[0] ^= 1;
    expect_verify_failure(plan, "XR_SEM_0019");
    mutable_key[0] = saved_key_byte;
    uint32_t saved_count = plan->call_target_count;
    plan->call_target_count = 0;
    expect_verify_failure(plan, "XR_SEM_0019");
    plan->call_target_count = saved_count;
    uint32_t saved_entity_count = plan->entity_count;
    XrSemanticEntityRecord *saved_entities = plan->entities;
    XrSemanticEntityRecord compacted[64];
    REQUIRE(saved_entity_count <= (uint32_t) (sizeof(compacted) / sizeof(compacted[0])));
    uint32_t compacted_count = 0;
    for (uint32_t index = 0; index < saved_entity_count; index++)
        if (saved_entities[index].kind != XR_SEM_ENTITY_COROUTINE_STATE)
            compacted[compacted_count++] = saved_entities[index];
    plan->entities = compacted;
    plan->entity_count = compacted_count;
    expect_verify_failure(plan, "XR_SEM_0019");
    plan->entities = saved_entities;
    plan->entity_count = saved_entity_count;
    uint16_t saved_operand_count =
        plan->operations[target->operation].operand_count;
    plan->operations[target->operation].operand_count = 1;
    expect_verify_failure(plan, "XR_SEM_0015");
    plan->operations[target->operation].operand_count = saved_operand_count;
    xr_semantic_plan_free(plan);

    XrSemanticPlan *unresolved = build_native_namespace_yieldable_plan(
        "time", "sleep", false, false, false);
    REQUIRE(unresolved->call_target_count == 0);
    xr_semantic_plan_free(unresolved);
    XrSemanticPlan *shadowed = build_native_namespace_yieldable_plan(
        "time", "sleep", true, true, false);
    REQUIRE(shadowed->call_target_count == 0);
    xr_semantic_plan_free(shadowed);
    XrSemanticPlan *non_yieldable = build_native_namespace_yieldable_plan(
        "time", "monotonic", true, false, false);
    REQUIRE(non_yieldable->call_target_count == 0);
    xr_semantic_plan_free(non_yieldable);
}

static void test_builtin_instance_yieldable_authority(void) {
    XrSemanticPlan *plan = build_builtin_instance_yieldable_plan(
        &stub_semaphore, "acquire", 0, true, true);
    REQUIRE(plan->call_target_count == 1);
    XrSemanticCallTargetRecord *target = &plan->call_targets[0];
    REQUIRE(target->kind == XR_SEM_CALL_TARGET_BUILTIN_INSTANCE_YIELDABLE);
    REQUIRE(target->function == XR_SEMANTIC_INDEX_NONE &&
            target->dependency == XR_SEMANTIC_INDEX_NONE &&
            target->source_export == XR_SEMANTIC_INDEX_NONE &&
            target->callable_type < plan->type_count &&
            plan->types[target->callable_type].builtin_type == XR_TID_SEMAPHORE);
    REQUIRE(strstr(target->canonical_key,
                   "call-target-v6:schema=19:operation=") != NULL);
    REQUIRE(strstr(target->canonical_key,
                   ":builtin-instance=Semaphore.acquire:type=") != NULL);
    char target_id_hex[XR_STABLE_ID_BYTES * 2 + 1];
    xr_stable_id_hex(target->id, target_id_hex);
    REQUIRE(strcmp(target_id_hex, "0d48360a277742d68fd6e958bc9b0a8d") == 0);
    uint32_t states = 0;
    for (uint32_t index = 0; index < plan->entity_count; index++)
        states += plan->entities[index].kind == XR_SEM_ENTITY_COROUTINE_STATE &&
                  plan->entities[index].subject == target->operation;
    REQUIRE(states == 1);

    char error[512] = {0};
    uint8_t *bytes = NULL;
    size_t size = 0;
    REQUIRE(xr_xsm_encode(plan, &bytes, &size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(bytes, size, &decoded, error, sizeof(error)));
    REQUIRE(decoded->call_target_count == 1 &&
            decoded->call_targets[0].kind ==
                XR_SEM_CALL_TARGET_BUILTIN_INSTANCE_YIELDABLE &&
            decoded->types[decoded->call_targets[0].callable_type].builtin_type ==
                XR_TID_SEMAPHORE);
    uint8_t *roundtrip = NULL;
    size_t roundtrip_size = 0;
    REQUIRE(xr_xsm_encode(decoded, &roundtrip, &roundtrip_size, error,
                          sizeof(error)));
    REQUIRE(roundtrip_size == size && memcmp(roundtrip, bytes, size) == 0);
    xr_free(roundtrip);
    xr_semantic_plan_free(decoded);
    uint8_t *old_schema = copy_bytes(bytes, size);
    old_schema[8] = 18;
    old_schema[9] = old_schema[10] = old_schema[11] = 0;
    expect_decode_failure(old_schema, size, "XR_ARTIFACT_2000");
    xr_free(old_schema);
    xr_free(bytes);

    uint32_t saved_builtin = plan->types[target->callable_type].builtin_type;
    plan->types[target->callable_type].builtin_type = XR_TID_NULL;
    expect_verify_failure(plan, "XR_SEM_0002");
    plan->types[target->callable_type].builtin_type = saved_builtin;
    uint8_t saved_kind = target->kind;
    target->kind = XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE;
    expect_verify_failure(plan, "XR_SEM_0019");
    target->kind = saved_kind;
    uint32_t saved_type = target->callable_type;
    target->callable_type = XR_SEMANTIC_INDEX_NONE;
    expect_verify_failure(plan, "XR_SEM_0019");
    target->callable_type = saved_type;
    uint32_t saved_count = plan->call_target_count;
    plan->call_target_count = 0;
    expect_verify_failure(plan, "XR_SEM_0019");
    plan->call_target_count = saved_count;
    uint32_t saved_entity_count = plan->entity_count;
    XrSemanticEntityRecord *saved_entities = plan->entities;
    XrSemanticEntityRecord compacted[64];
    REQUIRE(saved_entity_count <= (uint32_t) (sizeof(compacted) / sizeof(compacted[0])));
    uint32_t compacted_count = 0;
    for (uint32_t index = 0; index < saved_entity_count; index++)
        if (saved_entities[index].kind != XR_SEM_ENTITY_COROUTINE_STATE)
            compacted[compacted_count++] = saved_entities[index];
    plan->entities = compacted;
    plan->entity_count = compacted_count;
    expect_verify_failure(plan, "XR_SEM_0019");
    plan->entities = saved_entities;
    plan->entity_count = saved_entity_count;
    xr_semantic_plan_free(plan);

    XrSemanticPlan *shadow = build_builtin_instance_yieldable_plan(
        &stub_shadow_semaphore, "acquire", 0, false, true);
    REQUIRE(shadow->call_target_count == 0);
    xr_semantic_plan_free(shadow);
    XrSemanticPlan *wrong_selector = build_builtin_instance_yieldable_plan(
        &stub_semaphore, "release", 0, false, true);
    REQUIRE(wrong_selector->call_target_count == 0);
    xr_semantic_plan_free(wrong_selector);
    XrSemanticPlan *wrong_arity = build_builtin_instance_yieldable_plan(
        &stub_semaphore, "acquire", 1, false, true);
    REQUIRE(wrong_arity->call_target_count == 0);
    xr_semantic_plan_free(wrong_arity);
    (void) build_builtin_instance_yieldable_plan(
        &stub_semaphore, "acquire", 0, false, false);
}

static void test_shared_direct_call_target_authority(void) {
    XrSemanticPlan *plan = build_shared_direct_call_target_plan();
    REQUIRE(xr_semantic_plan_call_target_count(plan) == 1);
    XrSemanticCallTargetRecord *target = &plan->call_targets[0];
    const XrSemanticOperationRecord *call = &plan->operations[target->operation];
    REQUIRE(target->function == 1 && target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL);
    REQUIRE(call->function == 2 && call->opcode == XI_CALL);
    REQUIRE((call->effects & XI_EFFECT_MAY_SUSPEND) == 0);
    REQUIRE((call->flags & XI_FLAG_MAY_SUSPEND) == 0);
    REQUIRE(strstr(target->canonical_key, "call-target-v3:schema=19:operation=") != NULL);
    REQUIRE(strstr(target->canonical_key, ":kind=1") != NULL);
    char target_id_hex[XR_STABLE_ID_BYTES * 2 + 1];
    xr_stable_id_hex(target->id, target_id_hex);
    REQUIRE(strcmp(target_id_hex, "69ce0977ee4bf41f7e315cf56b9ac423") == 0);

    uint32_t coroutine_states = 0;
    bool call_has_state = false;
    bool static_seed_has_state = false;
    for (uint32_t i = 0; i < plan->entity_count; i++) {
        const XrSemanticEntityRecord *entity = &plan->entities[i];
        if (entity->kind != XR_SEM_ENTITY_COROUTINE_STATE)
            continue;
        coroutine_states++;
        call_has_state |= entity->subject == target->operation;
        static_seed_has_state |= plan->operations[entity->subject].opcode == XI_YIELD;
    }
    REQUIRE(coroutine_states == 2 && call_has_state && static_seed_has_state);

    uint8_t *bytes = NULL;
    size_t size = 0;
    char error[512] = {0};
    REQUIRE(xr_xsm_encode(plan, &bytes, &size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(bytes, size, &decoded, error, sizeof(error)));
    const XrSemanticCallTargetRecord *decoded_target =
        xr_semantic_plan_call_target(decoded, 0);
    REQUIRE(decoded_target != NULL &&
            decoded_target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
            decoded_target->operation == target->operation &&
            decoded_target->function == target->function &&
            strcmp(decoded_target->canonical_key, target->canonical_key) == 0 &&
            xr_stable_id_equal(decoded_target->id, target->id));
    uint8_t *roundtrip = NULL;
    size_t roundtrip_size = 0;
    REQUIRE(xr_xsm_encode(decoded, &roundtrip, &roundtrip_size, error, sizeof(error)));
    REQUIRE(roundtrip_size == size && memcmp(roundtrip, bytes, size) == 0);
    xr_free(roundtrip);
    xr_semantic_plan_free(decoded);
    xr_free(bytes);

    uint32_t saved_function = target->function;
    target->function = 0;
    expect_verify_failure(plan, "XR_SEM_0019");
    target->function = saved_function;
    uint32_t saved_count = plan->call_target_count;
    plan->call_target_count = 0;
    expect_verify_failure(plan, "XR_SEM_0019");
    plan->call_target_count = saved_count;
    xr_semantic_plan_free(plan);

    XrSemanticPlan *duplicate = build_ambiguous_shared_call_target_plan(false);
    REQUIRE(xr_semantic_plan_call_target_count(duplicate) == 0);
    forge_direct_call_target(duplicate, 1, 2);
    expect_verify_failure(duplicate, "XR_SEM_0019");
    xr_semantic_plan_free(duplicate);
    XrSemanticPlan *sibling = build_ambiguous_shared_call_target_plan(true);
    REQUIRE(xr_semantic_plan_call_target_count(sibling) == 0);
    forge_direct_call_target(sibling, 1, 3);
    expect_verify_failure(sibling, "XR_SEM_0019");
    xr_semantic_plan_free(sibling);
    XrSemanticPlan *late = build_unordered_shared_call_target_plan(false);
    REQUIRE(xr_semantic_plan_call_target_count(late) == 0);
    forge_direct_call_target(late, 0, 1);
    expect_verify_failure(late, "XR_SEM_0019");
    xr_semantic_plan_free(late);
    XrSemanticPlan *conditional = build_unordered_shared_call_target_plan(true);
    REQUIRE(xr_semantic_plan_call_target_count(conditional) == 0);
    forge_direct_call_target(conditional, 0, 1);
    expect_verify_failure(conditional, "XR_SEM_0019");
    xr_semantic_plan_free(conditional);
    XrSemanticPlan *nonroot = build_nonroot_shared_call_target_plan();
    REQUIRE(xr_semantic_plan_call_target_count(nonroot) == 0);
    forge_direct_call_target(nonroot, 3, 2);
    expect_verify_failure(nonroot, "XR_SEM_0019");
    xr_semantic_plan_free(nonroot);
    XrSemanticPlan *late_root = build_unproven_root_parent_shared_plan(false);
    REQUIRE(xr_semantic_plan_call_target_count(late_root) == 0);
    forge_direct_call_target(late_root, 2, 1);
    expect_verify_failure(late_root, "XR_SEM_0019");
    xr_semantic_plan_free(late_root);
    XrSemanticPlan *conditional_root = build_unproven_root_parent_shared_plan(true);
    REQUIRE(xr_semantic_plan_call_target_count(conditional_root) == 0);
    forge_direct_call_target(conditional_root, 2, 1);
    expect_verify_failure(conditional_root, "XR_SEM_0019");
    xr_semantic_plan_free(conditional_root);
    XrSemanticPlan *chain = build_long_suspend_call_chain();
    REQUIRE(xr_semantic_plan_call_target_count(chain) == 127);
    uint32_t chain_states = 0;
    for (uint32_t i = 0; i < chain->entity_count; i++)
        chain_states += chain->entities[i].kind == XR_SEM_ENTITY_COROUTINE_STATE;
    REQUIRE(chain_states == 127);
    xr_semantic_plan_free(chain);
}

static void test_parameter_and_capture_contracts(void) {
    XrSemanticPlan *parameters = build_owned_parameter_plan();
    REQUIRE(xr_semantic_plan_parameter_count(parameters) == 1);
    const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(parameters, 0);
    REQUIRE(parameter != NULL && parameter->function == 0 && parameter->ordinal == 0);
    REQUIRE(parameter->type < xr_semantic_plan_type_count(parameters));
    REQUIRE(parameter->value < parameters->functions[0].value_count);
    REQUIRE(parameter->canonical_key != NULL);
    uint16_t saved_ordinal = parameters->parameters[0].ordinal;
    parameters->parameters[0].ordinal = 1;
    expect_verify_failure(parameters, "XR_SEM_0013");
    parameters->parameters[0].ordinal = saved_ordinal;
    xr_semantic_plan_free(parameters);

    XrSemanticPlan *captures = build_capture_contract_plan();
    REQUIRE(xr_semantic_plan_function_count(captures) == 3);
    REQUIRE(xr_semantic_plan_capture_count(captures) == 2);
    const XrSemanticFunctionRecord *child = xr_semantic_plan_function(captures, 1);
    const XrSemanticCaptureRecord *capture = xr_semantic_plan_capture(captures, 0);
    REQUIRE(child != NULL && child->parent == 0 && child->capture_begin == 0 &&
            child->capture_count == 1);
    REQUIRE(capture != NULL && capture->function == 1 && capture->source_function == 0);
    REQUIRE(capture->source == XR_SEM_CAPTURE_LOCAL_VALUE &&
            capture->source_capture == XR_SEMANTIC_INDEX_NONE);
    REQUIRE(capture->source_value == capture->source_index);
    REQUIRE(capture->kind == XR_SEM_CAPTURE_BY_COPY && capture->source_type == capture->type &&
            capture->storage_domain == XR_STORAGE_EXEC_LOCAL &&
            capture->value_capability == XR_SEM_VALUE_CONST);
    const XrSemanticCaptureRecord *transitive = xr_semantic_plan_capture(captures, 1);
    REQUIRE(transitive != NULL && transitive->function == 2 && transitive->source_function == 1);
    REQUIRE(transitive->source == XR_SEM_CAPTURE_PARENT_CAPTURE &&
            transitive->source_value == XR_SEMANTIC_INDEX_NONE && transitive->source_capture == 0 &&
            transitive->source_index == 0 && transitive->source_type == capture->source_type);

    uint8_t *bytes = NULL;
    size_t size = 0;
    char error[512] = {0};
    REQUIRE(xr_xsm_encode(captures, &bytes, &size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(bytes, size, &decoded, error, sizeof(error)));
    REQUIRE(xr_semantic_plan_capture_count(decoded) == 2);
    REQUIRE(strcmp(xr_semantic_plan_capture(decoded, 0)->name, "captured") == 0);
    xr_semantic_plan_free(decoded);
    xr_free(bytes);

    XrSemanticCaptureRecord *mutable_capture = &captures->captures[0];
    uint32_t saved_source = mutable_capture->source_value;
    mutable_capture->source_value = XR_SEMANTIC_INDEX_NONE;
    expect_verify_failure(captures, "XR_SEM_0018");
    mutable_capture->source_value = saved_source;
    xr_semantic_plan_free(captures);
}

static void test_attachment_freezes_exact_function_identity(void) {
    XiFunc *root = xi_func_new("attachment_root", &stub_int);
    XiFunc *child = xi_func_new("attachment_child", &stub_int);
    XiFunc *grandchild = xi_func_new("attachment_grandchild", &stub_int);
    REQUIRE(root != NULL && child != NULL && grandchild != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    XiBlock *grandchild_entry = xi_block_new(grandchild);
    REQUIRE(root_entry != NULL && child_entry != NULL && grandchild_entry != NULL);
    xi_block_set_return(root_entry, xi_const_int(root, root_entry, 1, &stub_int));
    xi_block_set_return(child_entry, xi_const_int(child, child_entry, 2, &stub_int));
    xi_block_set_return(grandchild_entry, xi_const_int(grandchild, grandchild_entry, 3, &stub_int));
    root->children = (XiFunc **) xr_malloc(sizeof(*root->children));
    child->children = (XiFunc **) xr_malloc(sizeof(*child->children));
    REQUIRE(root->children != NULL && child->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->children[0] = grandchild;
    child->nchildren = child->children_cap = 1;
    child->parent_func = root;
    grandchild->parent_func = child;
    root->stage = child->stage = grandchild->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(root, error, sizeof(error)));
    REQUIRE(root->semantic_plan == child->semantic_plan &&
            child->semantic_plan == grandchild->semantic_plan);
    REQUIRE(root->semantic_plan_function_index == 0);
    REQUIRE(child->semantic_plan_function_index == 1);
    REQUIRE(grandchild->semantic_plan_function_index == 2);
    REQUIRE(strcmp(xr_semantic_plan_function(root->semantic_plan, 0)->name, "attachment_root") ==
            0);
    REQUIRE(strcmp(xr_semantic_plan_function(child->semantic_plan, 1)->name, "attachment_child") ==
            0);
    REQUIRE(strcmp(xr_semantic_plan_function(grandchild->semantic_plan, 2)->name,
                   "attachment_grandchild") == 0);
    xi_func_free(root);
}

static void test_unknown_capture_contract_fails_closed(void) {
    XiFunc *root = xi_func_new("unknown_capture_root", &stub_int);
    XiFunc *child = xi_func_new("unknown_capture_child", &stub_int);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);
    XiValue *captured = xi_const_str(root, root_entry, "unknown", &stub_string);
    XiValue *root_result = xi_const_int(root, root_entry, 1, &stub_int);
    XiValue *child_result = xi_const_int(child, child_entry, 2, &stub_int);
    REQUIRE(captured != NULL && root_result != NULL && child_result != NULL);
    xi_block_set_return(root_entry, root_result);
    xi_block_set_return(child_entry, child_result);
    root->children = (XiFunc **) xr_malloc(sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = 1;
    root->children_cap = 1;
    child->parent_func = root;
    child->ncaptures = 1;
    child->captures[0].source = XI_CAPTURE_SRC_REG;
    child->captures[0].capture_kind = XI_CAPTURE_BY_COPY;
    child->captures[0].name = "unknown";
    child->captures[0].type = &stub_string;
    child->captures[0].value = captured;
    child->captures[0].storage_domain = XR_STORAGE_DOMAIN_UNKNOWN;
    child->captures[0].value_capability = XR_SEM_VALUE_CAPABILITY_UNKNOWN;
    root->stage = XI_STAGE_OPTIMIZED;
    child->stage = XI_STAGE_OPTIMIZED;

    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(!xr_semantic_plan_build(root, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL);
    REQUIRE(strncmp(error, "XR_SEM_0018", strlen("XR_SEM_0018")) == 0);
    xi_func_free(root);
}

static void test_xsm_fail_closed_mutations(void) {
    XrSemanticPlan *plan = build_probe_plan();
    uint8_t *bytes = NULL;
    size_t size = 0;
    char error[512] = {0};
    REQUIRE(xr_xsm_encode(plan, &bytes, &size, error, sizeof(error)));

    expect_decode_failure(bytes, XR_XSM_HEADER_SIZE - 1, "XR_ARTIFACT_2001");

    uint8_t *mutation = copy_bytes(bytes, size);
    mutation[0] ^= 0x80;
    expect_decode_failure(mutation, size, "XR_ARTIFACT_2000");
    xr_free(mutation);

    mutation = copy_bytes(bytes, size);
    mutation[8] ^= 0x01;
    expect_decode_failure(mutation, size, "XR_ARTIFACT_2000");
    xr_free(mutation);

    mutation = copy_bytes(bytes, size);
    mutation[XR_XSM_HEADER_SIZE] ^= 0x01;
    expect_decode_failure(mutation, size, "XR_ARTIFACT_2002");
    xr_free(mutation);

    mutation = copy_bytes(bytes, size);
    mutation[56] ^= 0x01;
    expect_decode_failure(mutation, size, "XR_ARTIFACT_2003");
    xr_free(mutation);

    mutation = copy_bytes(bytes, size);
    mutation[88] ^= 0x01;
    expect_decode_failure(mutation, size, "XR_ARTIFACT_2003");
    xr_free(mutation);

    mutation = copy_bytes(bytes, size);
    mutation[120] ^= 0x01;
    expect_decode_failure(mutation, size, "XR_ARTIFACT_2002");
    xr_free(mutation);

    mutation = copy_bytes(bytes, size);
    size_t key_offset =
        find_bytes(mutation + XR_XSM_HEADER_SIZE, size - XR_XSM_HEADER_SIZE, "type-v3");
    REQUIRE(key_offset != SIZE_MAX);
    mutation[XR_XSM_HEADER_SIZE + key_offset + 4] = '\0';
    rewrite_payload_digest(mutation, size);
    expect_decode_failure(mutation, size, "XR_ARTIFACT_2001");
    xr_free(mutation);

    mutation = copy_bytes(bytes, size);
    size_t operation_count_offset = XR_XSM_HEADER_SIZE + 3u * sizeof(uint32_t);
    uint32_t excessive_operations = 10000000u;
    for (unsigned i = 0; i < sizeof(uint32_t); i++)
        mutation[operation_count_offset + i] = (uint8_t) (excessive_operations >> (i * 8));
    rewrite_payload_digest(mutation, size);
    expect_decode_failure(mutation, size, "XR_EXEC_5003");
    xr_free(mutation);

    mutation = copy_bytes(bytes, size);
    size_t call_target_count_offset = XR_XSM_HEADER_SIZE + 4u * sizeof(uint32_t);
    uint32_t excessive_call_targets = 10000001u;
    for (unsigned i = 0; i < sizeof(uint32_t); i++)
        mutation[call_target_count_offset + i] =
            (uint8_t) (excessive_call_targets >> (i * 8));
    rewrite_payload_digest(mutation, size);
    expect_decode_failure(mutation, size, "XR_EXEC_5003");
    xr_free(mutation);

    mutation = copy_bytes(bytes, size);
    size_t entity_count_offset = XR_XSM_HEADER_SIZE + 7u * sizeof(uint32_t);
    uint32_t excessive_entities = 80000001u;
    for (unsigned i = 0; i < sizeof(uint32_t); i++)
        mutation[entity_count_offset + i] = (uint8_t) (excessive_entities >> (i * 8));
    rewrite_payload_digest(mutation, size);
    expect_decode_failure(mutation, size, "XR_EXEC_5003");
    xr_free(mutation);

    mutation = copy_bytes(bytes, size);
    size_t metadata_count_offset = XR_XSM_HEADER_SIZE + 13u * sizeof(uint32_t);
    uint32_t unrepresentable_metadata = 16000000u;
    for (unsigned i = 0; i < sizeof(uint32_t); i++)
        mutation[metadata_count_offset + i] = (uint8_t) (unrepresentable_metadata >> (i * 8));
    rewrite_payload_digest(mutation, size);
    expect_decode_failure(mutation, size, "XR_EXEC_5003");
    xr_free(mutation);

    uint8_t truncated = 0;
    expect_decode_failure(&truncated, XR_XSM_MAX_ARTIFACT_SIZE + 1u, "XR_EXEC_5003");

    xr_free(bytes);
    xr_semantic_plan_free(plan);
}

static void expect_verify_failure_at(XrSemanticPlan *plan, const char *code, int line) {
    char error[512] = {0};
    REQUIRE(!xr_semantic_plan_verify(plan, error, sizeof(error)));
    if (strncmp(error, code, strlen(code)) != 0)
        fprintf(stderr, "mutation at test_semantic_plan.c:%d expected verifier code %s, got %s\n",
                line, code, error);
    REQUIRE(strncmp(error, code, strlen(code)) == 0);
}

static void expect_ownership_failure_at(XrSemanticPlan *plan, const char *code, int line) {
    char error[512] = {0};
    XrSemanticGraph graph = {0};
    REQUIRE(xr_semantic_graph_build(plan, &graph, error, sizeof(error)));
    REQUIRE(!xr_ownership_certificate_check(plan, &graph, error, sizeof(error)));
    if (strncmp(error, code, strlen(code)) != 0)
        fprintf(stderr, "mutation at test_semantic_plan.c:%d expected ownership code %s, got %s\n",
                line, code, error);
    REQUIRE(strncmp(error, code, strlen(code)) == 0);
    xr_semantic_graph_dispose(&graph);
}

#define expect_ownership_failure(plan, code)                                                       \
    expect_ownership_failure_at((plan), (code), __LINE__)

static void test_semantic_side_table_partitions(void) {
    XrSemanticPlan *plan = build_entity_identity_plan();
    uint32_t child_type = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->type_count; i++) {
        if (plan->types[i].child_count != 0 &&
            plan->types[i].kind != XR_KIND_STRUCT_OBJECT) {
            child_type = i;
            break;
        }
    }
    REQUIRE(child_type != XR_SEMANTIC_INDEX_NONE);
    uint16_t saved_child_count = plan->types[child_type].child_count;
    plan->types[child_type].child_count--;
    expect_verify_failure(plan, "XR_SEM_0012");
    plan->types[child_type].child_count = saved_child_count;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_verify(plan, error, sizeof(error)));
    xr_semantic_plan_free(plan);

    plan = build_typed_call_operand_plan();
    uint32_t operand_operation = XR_SEMANTIC_INDEX_NONE;
    uint32_t metadata_operation = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        if (operand_operation == XR_SEMANTIC_INDEX_NONE && plan->operations[i].operand_count != 0)
            operand_operation = i;
        if (metadata_operation == XR_SEMANTIC_INDEX_NONE && plan->operations[i].metadata_count != 0)
            metadata_operation = i;
    }
    REQUIRE(operand_operation != XR_SEMANTIC_INDEX_NONE);
    REQUIRE(metadata_operation != XR_SEMANTIC_INDEX_NONE);

    uint32_t saved_operand_begin = plan->operations[operand_operation].operand_begin;
    plan->operations[operand_operation].operand_begin++;
    expect_verify_failure(plan, "XR_SEM_0015");
    plan->operations[operand_operation].operand_begin = saved_operand_begin;

    uint32_t saved_metadata_begin = plan->operations[metadata_operation].metadata_begin;
    plan->operations[metadata_operation].metadata_begin++;
    expect_verify_failure(plan, "XR_SEM_0015");
    plan->operations[metadata_operation].metadata_begin = saved_metadata_begin;

    memset(error, 0, sizeof(error));
    REQUIRE(xr_semantic_plan_verify(plan, error, sizeof(error)));
    xr_semantic_plan_free(plan);
}

static void test_explicit_loop_invariant_certificate(void) {
    XrSemanticPlan *first = build_loop_invariant_plan();
    XrSemanticPlan *second = build_loop_invariant_plan();
    const XrOwnershipCertificate *certificate = xr_semantic_plan_ownership(first);
    REQUIRE(certificate != NULL);
    REQUIRE(xr_ownership_certificate_owner_count(certificate) == 1);
    REQUIRE(xr_ownership_certificate_loop_invariant_count(certificate) == 2);
    REQUIRE(xr_ownership_certificate_edge_state_count(certificate) >= 2);
    const XrOwnershipLoopInvariantRecord *left =
        xr_ownership_certificate_loop_invariant(certificate, 0);
    const XrOwnershipLoopInvariantRecord *right =
        xr_ownership_certificate_loop_invariant(certificate, 1);
    REQUIRE(left != NULL && right != NULL);
    REQUIRE(xr_stable_id_compare(left->id, right->id) < 0);
    REQUIRE(left->owner == right->owner && left->header == right->header &&
            left->backedge != right->backedge);
    REQUIRE(left->balance == 1 && right->balance == 1);
    REQUIRE(left->state == XR_OWN_OWNED_LOCAL && right->state == XR_OWN_OWNED_LOCAL);
    REQUIRE(strncmp(left->canonical_key, "ownership-loop-v1:",
                    strlen("ownership-loop-v1:")) == 0);

    uint8_t *first_bytes = NULL;
    uint8_t *second_bytes = NULL;
    size_t first_size = 0;
    size_t second_size = 0;
    char error[512] = {0};
    REQUIRE(xr_xsm_encode(first, &first_bytes, &first_size, error, sizeof(error)));
    REQUIRE(xr_xsm_encode(second, &second_bytes, &second_size, error, sizeof(error)));
    REQUIRE(first_size == second_size && memcmp(first_bytes, second_bytes, first_size) == 0);
    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(first_bytes, first_size, &decoded, error, sizeof(error)));
    const XrOwnershipCertificate *decoded_certificate = xr_semantic_plan_ownership(decoded);
    REQUIRE(xr_ownership_certificate_loop_invariant_count(decoded_certificate) == 2);
    for (uint32_t i = 0; i < 2; i++) {
        const XrOwnershipLoopInvariantRecord *expected =
            xr_ownership_certificate_loop_invariant(certificate, i);
        const XrOwnershipLoopInvariantRecord *actual =
            xr_ownership_certificate_loop_invariant(decoded_certificate, i);
        REQUIRE(expected != NULL && actual != NULL);
        REQUIRE(xr_stable_id_equal(expected->id, actual->id));
        REQUIRE(strcmp(expected->canonical_key, actual->canonical_key) == 0);
        REQUIRE(expected->owner == actual->owner && expected->header == actual->header &&
                expected->backedge == actual->backedge && expected->balance == actual->balance &&
                expected->state == actual->state);
    }
    size_t first_dump_size = 0;
    size_t decoded_dump_size = 0;
    char *first_dump = dump_plan(first, &first_dump_size);
    char *decoded_dump = dump_plan(decoded, &decoded_dump_size);
    REQUIRE(first_dump_size == decoded_dump_size);
    REQUIRE(memcmp(first_dump, decoded_dump, first_dump_size) == 0);
    xr_free(decoded_dump);
    xr_free(first_dump);

    uint8_t *oversized = copy_bytes(first_bytes, first_size);
    size_t loop_count_offset = XR_XSM_HEADER_SIZE + 16u * sizeof(uint32_t);
    uint32_t excessive_loop_count = 40000001u;
    for (unsigned i = 0; i < sizeof(uint32_t); i++)
        oversized[loop_count_offset + i] = (uint8_t) (excessive_loop_count >> (i * 8));
    rewrite_payload_digest(oversized, first_size);
    expect_decode_failure(oversized, first_size, "XR_EXEC_5003");
    xr_free(oversized);

    XrOwnershipLoopInvariantRecord *mutable = &first->ownership->loop_invariants[0];
    int32_t saved_balance = mutable->balance;
    mutable->balance++;
    expect_verify_failure(first, "XR_OWN_3006");
    mutable->balance = saved_balance;
    uint8_t saved_state = mutable->state;
    mutable->state = XR_OWN_RELEASED;
    expect_verify_failure(first, "XR_OWN_3006");
    mutable->state = saved_state;
    mutable->reserved[0] = 1;
    expect_verify_failure(first, "XR_OWN_3006");
    mutable->reserved[0] = 0;
    uint32_t saved_backedge = mutable->backedge;
    mutable->backedge = mutable->header;
    expect_verify_failure(first, "XR_OWN_3006");
    mutable->backedge = saved_backedge;
    const char *saved_key = mutable->canonical_key;
    mutable->canonical_key = "ownership-loop-v1:corrupt";
    expect_verify_failure(first, "XR_OWN_3006");
    mutable->canonical_key = saved_key;
    mutable->id.bytes[0] ^= 1;
    expect_verify_failure(first, "XR_OWN_3006");
    mutable->id.bytes[0] ^= 1;
    XrOwnershipEdgeStateRecord *loop_edge = NULL;
    for (uint32_t i = 0; i < first->ownership->edge_state_count; i++) {
        XrOwnershipEdgeStateRecord *candidate = &first->ownership->edge_states[i];
        if (candidate->owner == mutable->owner && candidate->block == mutable->backedge &&
            candidate->successor == mutable->header) {
            loop_edge = candidate;
            break;
        }
    }
    REQUIRE(loop_edge != NULL && loop_edge->flags == 0);
    XrOwnershipEdgeStateRecord saved_loop_edge = *loop_edge;
    uint32_t saved_count = first->ownership->loop_invariant_count;
    loop_edge->entry_balance = 0;
    loop_edge->exit_balance = 0;
    loop_edge->entry_state = XR_OWN_UNINITIALIZED;
    loop_edge->exit_state = XR_OWN_UNINITIALIZED;
    loop_edge->flags = 1;
    first->ownership->loop_invariant_count--;
    expect_verify_failure(first, "XR_OWN_3001");
    first->ownership->loop_invariant_count = saved_count;
    *loop_edge = saved_loop_edge;
    XrOwnershipEdgeStateRecord saved_first_edge = first->ownership->edge_states[0];
    XrOwnershipEdgeStateRecord saved_second_edge = first->ownership->edge_states[1];
    first->ownership->edge_states[0] = saved_second_edge;
    first->ownership->edge_states[1] = saved_first_edge;
    expect_verify_failure(first, "XR_OWN_3002");
    first->ownership->edge_states[0] = saved_first_edge;
    first->ownership->edge_states[1] = saved_second_edge;
    first->ownership->edge_states[1] = saved_first_edge;
    expect_verify_failure(first, "XR_OWN_3002");
    first->ownership->edge_states[1] = saved_second_edge;
    uint8_t saved_entry_state = first->ownership->edge_states[0].entry_state;
    first->ownership->edge_states[0].entry_state = XR_OWN_IMMORTAL;
    expect_verify_failure(first, "XR_OWN_3001");
    first->ownership->edge_states[0].entry_state = saved_entry_state;
    uint8_t saved_exit_state = first->ownership->edge_states[0].exit_state;
    first->ownership->edge_states[0].exit_state = XR_OWN_IMMORTAL;
    expect_verify_failure(first, "XR_OWN_3001");
    first->ownership->edge_states[0].exit_state = saved_exit_state;
    uint16_t saved_flags = first->ownership->edge_states[0].flags;
    first->ownership->edge_states[0].flags = UINT16_C(2);
    expect_verify_failure(first, "XR_OWN_3002");
    first->ownership->edge_states[0].flags = saved_flags;
    int32_t saved_entry_balance = first->ownership->edge_states[0].entry_balance;
    int32_t saved_exit_balance = first->ownership->edge_states[0].exit_balance;
    first->ownership->edge_states[0].entry_balance = INT32_MIN;
    first->ownership->edge_states[0].exit_balance = INT32_MAX;
    expect_verify_failure(first, "XR_OWN_3001");
    first->ownership->edge_states[0].entry_balance = saved_entry_balance;
    first->ownership->edge_states[0].exit_balance = saved_exit_balance;
    XrOwnershipEdgeStateRecord *second_branch_edge = NULL;
    for (uint32_t i = 1; i < first->ownership->edge_state_count; i++) {
        XrOwnershipEdgeStateRecord *previous = &first->ownership->edge_states[i - 1];
        XrOwnershipEdgeStateRecord *candidate = &first->ownership->edge_states[i];
        if (candidate->owner == previous->owner && candidate->block == previous->block &&
            candidate->successor != previous->successor && candidate->flags == 0) {
            second_branch_edge = candidate;
            break;
        }
    }
    REQUIRE(second_branch_edge != NULL);
    XrOwnershipEdgeStateRecord saved_second_branch_edge = *second_branch_edge;
    second_branch_edge->entry_balance++;
    second_branch_edge->exit_balance++;
    expect_verify_failure(first, "XR_OWN_3001");
    *second_branch_edge = saved_second_branch_edge;
    first->ownership->loop_invariant_count--;
    expect_verify_failure(first, "XR_OWN_3006");
    first->ownership->loop_invariant_count = saved_count;
    REQUIRE(xr_semantic_plan_verify(first, error, sizeof(error)));

    xr_semantic_plan_free(decoded);
    xr_free(second_bytes);
    xr_free(first_bytes);
    xr_semantic_plan_free(second);
    xr_semantic_plan_free(first);
}

static void test_semantic_and_ownership_mutations(void) {
    XrSemanticPlan *plan = build_owned_parameter_plan();
    REQUIRE(plan->operation_count >= 2);
    REQUIRE(plan->ownership != NULL);
    REQUIRE(plan->ownership->event_count >= 2);
    REQUIRE(plan->ownership->edge_state_count >= 1);

    uint32_t original_owner_count = plan->ownership->owner_count;
    REQUIRE(original_owner_count < plan->ownership->owner_capacity);
    plan->ownership->owners[original_owner_count] = plan->ownership->owners[0];
    plan->ownership->owners[original_owner_count].canonical_key = "ownership-owner-v4:extra";
    XrFingerprint extra_owner_digest = {{0}};
    REQUIRE(xr_stable_id_from_key(plan->ownership->owners[original_owner_count].canonical_key,
                                  &plan->ownership->owners[original_owner_count].id,
                                  &extra_owner_digest));
    plan->ownership->owner_count++;
    expect_ownership_failure(plan, "XR_OWN_3002");
    plan->ownership->owner_count = original_owner_count;

    uint32_t saved_origin = plan->ownership->owners[0].origin_value;
    plan->ownership->owners[0].origin_value++;
    expect_verify_failure(plan, "XR_OWN_3002");
    plan->ownership->owners[0].origin_value = saved_origin;
    uint8_t saved_initial_state = plan->ownership->owners[0].initial_state;
    plan->ownership->owners[0].initial_state = XR_OWN_BORROWED;
    expect_verify_failure(plan, "XR_OWN_3002");
    plan->ownership->owners[0].initial_state = saved_initial_state;

    uint32_t direct_event = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->ownership->event_count; i++) {
        if (plan->ownership->events[i].program_point != XR_OWN_POINT_BLOCK_EXIT) {
            direct_event = i;
            break;
        }
    }
    REQUIRE(direct_event != XR_SEMANTIC_INDEX_NONE);
    uint32_t original_event_count = plan->ownership->event_count;
    XrOwnershipEventRecord saved_direct_event = plan->ownership->events[direct_event];
    XrOwnershipEventRecord saved_last_event = plan->ownership->events[original_event_count - 1u];
    plan->ownership->events[direct_event] = saved_last_event;
    plan->ownership->event_count--;
    expect_verify_failure(plan, "XR_OWN_3002");
    plan->ownership->event_count = original_event_count;
    plan->ownership->events[direct_event] = saved_direct_event;
    plan->ownership->events[original_event_count - 1u] = saved_last_event;

    plan->operation_registry_fingerprint.bytes[0] ^= 0x01;
    expect_verify_failure(plan, "XR_SEM_0017");
    plan->operation_registry_fingerprint.bytes[0] ^= 0x01;

    uint16_t saved_opcode = plan->operations[0].opcode;
    plan->operations[0].opcode = XI_OP_COUNT;
    expect_verify_failure(plan, "XR_SEM_0015");
    plan->operations[0].opcode = saved_opcode;

    XrStableId saved_id = plan->operations[1].id;
    const char *saved_key = plan->operations[1].canonical_key;
    plan->operations[1].id = plan->operations[0].id;
    plan->operations[1].canonical_key = plan->operations[0].canonical_key;
    expect_verify_failure(plan, "XR_SEM_0003");
    plan->operations[1].id = saved_id;
    plan->operations[1].canonical_key = saved_key;

    XrOwnershipEventRecord *release_event = NULL;
    for (uint32_t i = 0; i < plan->ownership->event_count; i++) {
        if (plan->ownership->events[i].kind == XR_OWN_EVENT_RELEASE) {
            release_event = &plan->ownership->events[i];
            break;
        }
    }
    REQUIRE(release_event != NULL);
    int16_t saved_delta = release_event->logical_delta;
    release_event->logical_delta = 0;
    expect_verify_failure(plan, "XR_OWN_3001");
    release_event->logical_delta = saved_delta;

    uint8_t saved_reserved = release_event->reserved;
    release_event->reserved = 1;
    expect_verify_failure(plan, "XR_OWN_3002");
    release_event->reserved = saved_reserved;

    uint8_t saved_program_point = release_event->program_point;
    release_event->program_point = XR_OWN_POINT_EDGE;
    expect_verify_failure(plan, "XR_OWN_3002");
    release_event->program_point = saved_program_point;

    XrOwnershipEventRecord *opening_event = NULL;
    for (uint32_t i = 0; i < plan->ownership->event_count; i++) {
        XrOwnershipEventRecord *candidate = &plan->ownership->events[i];
        if (candidate->owner == release_event->owner && candidate->block == release_event->block &&
            candidate->successor == XR_SEMANTIC_INDEX_NONE && candidate->logical_delta > 0) {
            opening_event = candidate;
            break;
        }
    }
    REQUIRE(opening_event != NULL);
    REQUIRE(opening_event->program_point == XR_OWN_POINT_AFTER_OPERATION);
    uint8_t opening_state = opening_event->state_after;
    opening_event->state_after = XR_OWN_BORROWED;
    expect_verify_failure(plan, "XR_OWN_3002");
    opening_event->state_after = opening_state;
    uint8_t opening_program_point = opening_event->program_point;
    opening_event->program_point = XR_OWN_POINT_BLOCK_EXIT;
    expect_verify_failure(plan, "XR_OWN_3002");
    opening_event->program_point = opening_program_point;
    int16_t opening_delta = opening_event->logical_delta;
    opening_event->logical_delta = saved_delta;
    release_event->logical_delta = opening_delta;
    expect_verify_failure(plan, "XR_OWN_3002");
    opening_event->logical_delta = opening_delta;
    release_event->logical_delta = saved_delta;

    XrOwnershipEdgeStateRecord *edge = &plan->ownership->edge_states[0];
    int32_t saved_exit = edge->exit_balance;
    edge->exit_balance = saved_exit + 1;
    expect_verify_failure(plan, "XR_OWN_3001");
    edge->exit_balance = saved_exit;

    XrSemanticOperationRecord *debug_operation = NULL;
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        if (plan->operations[i].source_file) {
            debug_operation = &plan->operations[i];
            break;
        }
    }
    REQUIRE(debug_operation != NULL);
    uint32_t saved_start_line = debug_operation->source_start_line;
    debug_operation->source_start_line = saved_start_line + 1;
    expect_verify_failure(plan, "XR_SEM_0019");
    uint8_t *invalid_artifact = NULL;
    size_t invalid_artifact_size = 0;
    char encode_error[512] = {0};
    REQUIRE(!xr_xsm_encode(plan, &invalid_artifact, &invalid_artifact_size, encode_error,
                           sizeof(encode_error)));
    REQUIRE(invalid_artifact == NULL && invalid_artifact_size == 0);
    REQUIRE(strncmp(encode_error, "XR_SEM_0019", strlen("XR_SEM_0019")) == 0);
    debug_operation->source_start_line = saved_start_line;

    char error[512] = {0};
    REQUIRE(xr_semantic_plan_verify(plan, error, sizeof(error)));
    xr_semantic_plan_free(plan);

    plan = build_probe_plan();
    XrSemanticConstantRecord *boolean = NULL;
    for (uint32_t i = 0; i < plan->constant_count; i++) {
        if (plan->constants[i].kind == XR_SEM_CONST_BOOL)
            boolean = &plan->constants[i];
    }
    REQUIRE(boolean != NULL);
    boolean->integer = 2;
    expect_verify_failure(plan, "XR_SEM_0009");
    xr_semantic_plan_free(plan);

    plan = build_panic_edge_plan();
    XrSemanticEdgeRecord *panic = NULL;
    for (uint32_t i = 0; i < plan->edge_count; i++) {
        if (plan->edges[i].kind == XR_SEM_EDGE_PANIC)
            panic = &plan->edges[i];
    }
    REQUIRE(panic != NULL);
    panic->operation = XR_SEMANTIC_INDEX_NONE;
    expect_verify_failure(plan, "XR_SEM_0010");
    xr_semantic_plan_free(plan);

    plan = build_phi_dominance_plan();
    XrSemanticOperationRecord *phi = NULL;
    uint32_t left_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t right_value = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        XrSemanticOperationRecord *operation = &plan->operations[i];
        if (operation->opcode == XI_PHI)
            phi = operation;
        else if (operation->block == 1)
            left_value = operation->result_value;
        else if (operation->block == 2)
            right_value = operation->result_value;
    }
    REQUIRE(phi != NULL && left_value != XR_SEMANTIC_INDEX_NONE &&
            right_value != XR_SEMANTIC_INDEX_NONE && phi->operand_count == 2);
    uint32_t saved_operand = plan->operands[phi->operand_begin].value;
    REQUIRE(saved_operand == left_value);
    plan->operands[phi->operand_begin].value = right_value;
    expect_verify_failure(plan, "XR_SEM_0016");
    plan->operands[phi->operand_begin].value = saved_operand;
    char graph_error[512] = {0};
    REQUIRE(xr_semantic_plan_verify(plan, graph_error, sizeof(graph_error)));
    XrSemanticGraph graph = {0};
    REQUIRE(xr_semantic_graph_build(plan, &graph, graph_error, sizeof(graph_error)));
    REQUIRE(xr_semantic_graph_postdominates(&graph, phi->block, 0));
    REQUIRE(xr_semantic_graph_postdominates(&graph, phi->block, 1));
    REQUIRE(xr_semantic_graph_postdominates(&graph, phi->block, 2));
    REQUIRE(!xr_semantic_graph_postdominates(&graph, 1, 0));
    xr_semantic_graph_dispose(&graph);
    xr_semantic_plan_free(plan);
}

static void test_owning_phi_frontiers(void) {
    XrSemanticPlan *plan = build_owning_phi_plan();
    uint32_t phi_operation = XR_SEMANTIC_INDEX_NONE;
    uint32_t phi_owner = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        if (plan->operations[i].opcode == XI_PHI) {
            phi_operation = i;
            for (uint32_t owner = 0; owner < plan->ownership->owner_count; owner++) {
                if (plan->ownership->owners[owner].origin_value ==
                    plan->operations[i].result_value) {
                    phi_owner = owner;
                    break;
                }
            }
            break;
        }
    }
    REQUIRE(phi_operation != XR_SEMANTIC_INDEX_NONE && phi_owner != XR_SEMANTIC_INDEX_NONE);
    uint32_t frontier_count = 0;
    XrOwnershipEdgeStateRecord *frontier = NULL;
    for (uint32_t i = 0; i < plan->ownership->edge_state_count; i++) {
        XrOwnershipEdgeStateRecord *edge = &plan->ownership->edge_states[i];
        if (edge->owner == phi_owner && edge->flags == XR_OWN_EDGE_OWNER_FRONTIER) {
            REQUIRE(edge->successor == plan->operations[phi_operation].block);
            REQUIRE(edge->entry_balance == 0 && edge->exit_balance == 1);
            REQUIRE(edge->entry_state == XR_OWN_UNINITIALIZED &&
                    edge->exit_state == XR_OWN_OWNED_LOCAL);
            frontier = edge;
            frontier_count++;
        }
    }
    REQUIRE(frontier_count == 2 && frontier != NULL);
    uint16_t saved_flags = frontier->flags;
    frontier->flags = XR_OWN_EDGE_OUT_OF_SCOPE;
    expect_verify_failure(plan, "XR_OWN_3002");
    frontier->flags = saved_flags;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_verify(plan, error, sizeof(error)));
    xr_semantic_plan_free(plan);
}

static void test_borrowed_phi_loan_frontiers(void) {
    XiFunc *function = xi_func_new("borrowed_phi_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    XiBlock *header = xi_block_new(function);
    XiBlock *latch = xi_block_new(function);
    XiBlock *exit = xi_block_new(function);
    REQUIRE(entry != NULL && header != NULL && latch != NULL && exit != NULL);
    XiValue *first = xi_param(function, entry, 0, &stub_string);
    REQUIRE(first != NULL);
    function->nparams = 1;
    function->params = (XiValue **) xr_malloc(sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = first;
    function->arc_borrow_sig =
        (XiBorrowSig *) xi_func_arena_alloc(function, (uint32_t) sizeof(*function->arc_borrow_sig));
    REQUIRE(function->arc_borrow_sig != NULL);
    function->arc_borrow_sig->nparams = 1;
    function->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    function->arc_borrow_sig->valid = true;
    xi_block_set_jump(entry, header);
    XiValue *condition = xi_const_bool(function, header, true, &stub_bool);
    REQUIRE(condition != NULL);
    xi_block_set_if(header, condition, latch, exit);
    xi_block_set_jump(latch, header);
    XiPhi *phi = xi_phi_new(function, header, &stub_string, header->npreds);
    REQUIRE(phi != NULL && header->npreds == 2);
    phi->value.args[0] = first;
    phi->value.args[1] = &phi->value;
    XiValue *result = xi_const_int(function, exit, 0, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(exit, result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "borrowed-PHI plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    uint32_t loan_frontiers = 0;
    XrOwnershipEdgeStateRecord *loan_frontier = NULL;
    for (uint32_t i = 0; i < plan->ownership->edge_state_count; i++) {
        XrOwnershipEdgeStateRecord *edge = &plan->ownership->edge_states[i];
        if (edge->flags != XR_OWN_EDGE_OWNER_FRONTIER ||
            edge->exit_state != XR_OWN_BORROWED)
            continue;
        REQUIRE(edge->entry_balance == 0 && edge->exit_balance == 0);
        loan_frontier = edge;
        loan_frontiers++;
    }
    REQUIRE(loan_frontiers == 1 && loan_frontier != NULL);
    uint32_t loan_events = 0;
    for (uint32_t i = 0; i < plan->ownership->event_count; i++) {
        const XrOwnershipEventRecord *event = &plan->ownership->events[i];
        if (event->kind == XR_OWN_EVENT_BORROW && event->program_point == XR_OWN_POINT_EDGE &&
            event->state_after == XR_OWN_BORROWED && event->logical_delta == 0)
            loan_events++;
    }
    REQUIRE(loan_events == 2);
    uint8_t saved_loan_exit_state = loan_frontier->exit_state;
    loan_frontier->exit_state = XR_OWN_OWNED_LOCAL;
    expect_verify_failure(plan, "XR_OWN_3002");
    loan_frontier->exit_state = saved_loan_exit_state;
    xr_semantic_plan_free(plan);
    xi_func_free(function);

    function = xi_func_new("owned_loop_phi_source_probe", &stub_int);
    REQUIRE(function != NULL);
    entry = xi_block_new(function);
    XiBlock *loop_header = xi_block_new(function);
    XiBlock *loop_latch = xi_block_new(function);
    XiBlock *loop_choice = xi_block_new(function);
    XiBlock *loop_left = xi_block_new(function);
    XiBlock *loop_right = xi_block_new(function);
    XiBlock *loop_merge = xi_block_new(function);
    REQUIRE(entry != NULL && loop_header != NULL && loop_latch != NULL &&
            loop_choice != NULL && loop_left != NULL && loop_right != NULL &&
            loop_merge != NULL);
    XiValue *loop_seed = xi_const_str(function, entry, "seed", &stub_string);
    REQUIRE(loop_seed != NULL);
    xi_block_set_jump(entry, loop_header);
    condition = xi_const_bool(function, loop_header, true, &stub_bool);
    REQUIRE(condition != NULL);
    xi_block_set_if(loop_header, condition, loop_latch, loop_choice);
    xi_block_set_jump(loop_latch, loop_header);
    XiPhi *loop_phi = xi_phi_new(function, loop_header, &stub_string, loop_header->npreds);
    REQUIRE(loop_phi != NULL && loop_header->npreds == 2);
    loop_phi->value.args[0] = loop_seed;
    loop_phi->value.args[1] = &loop_phi->value;
    condition = xi_const_bool(function, loop_choice, true, &stub_bool);
    REQUIRE(condition != NULL);
    xi_block_set_if(loop_choice, condition, loop_left, loop_right);
    xi_block_set_jump(loop_left, loop_merge);
    xi_block_set_jump(loop_right, loop_merge);
    phi = xi_phi_new(function, loop_merge, &stub_string, loop_merge->npreds);
    REQUIRE(phi != NULL && loop_merge->npreds == 2);
    phi->value.args[0] = &loop_phi->value;
    phi->value.args[1] = &loop_phi->value;
    XiValue *merged_length = xi_value_new(function, loop_merge, XI_LEN, &stub_int, 1);
    REQUIRE(merged_length != NULL);
    merged_length->args[0] = &phi->value;
    xi_block_set_return(loop_merge, merged_length);
    function->stage = XI_STAGE_OPTIMIZED;
    plan = NULL;
    memset(error, 0, sizeof(error));
    built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "owned loop-PHI source plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    uint32_t owned_phi_count = 0;
    for (uint32_t owner = 0; owner < plan->ownership->owner_count; owner++) {
        const XrOwnershipOwnerRecord *record = &plan->ownership->owners[owner];
        for (uint32_t operation = 0; operation < plan->operation_count; operation++) {
            if (plan->operations[operation].opcode == XI_PHI &&
                plan->operations[operation].result_value == record->origin_value) {
                REQUIRE(record->initial_state == XR_OWN_OWNED_LOCAL);
                owned_phi_count++;
            }
        }
    }
    REQUIRE(owned_phi_count == 2);
    for (uint32_t operation = 0; operation < plan->operation_count; operation++)
        REQUIRE(plan->operations[operation].opcode != XI_RETAIN);
    xr_semantic_plan_free(plan);
    xi_func_free(function);

    function = xi_func_new("mixed_promoted_phi_probe", &stub_int);
    REQUIRE(function != NULL);
    entry = xi_block_new(function);
    XiBlock *owned_path = xi_block_new(function);
    XiBlock *borrowed_path = xi_block_new(function);
    XiBlock *promoted_merge = xi_block_new(function);
    REQUIRE(entry != NULL && owned_path != NULL && borrowed_path != NULL &&
            promoted_merge != NULL);
    first = xi_param(function, entry, 0, &stub_string);
    REQUIRE(first != NULL);
    function->nparams = 1;
    function->params = (XiValue **) xr_malloc(sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = first;
    function->arc_borrow_sig =
        (XiBorrowSig *) xi_func_arena_alloc(function, (uint32_t) sizeof(*function->arc_borrow_sig));
    REQUIRE(function->arc_borrow_sig != NULL);
    function->arc_borrow_sig->nparams = 1;
    function->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    function->arc_borrow_sig->valid = true;
    condition = xi_const_bool(function, entry, true, &stub_bool);
    REQUIRE(condition != NULL);
    xi_block_set_if(entry, condition, owned_path, borrowed_path);
    XiValue *left_text = xi_const_str(function, owned_path, "left", &stub_string);
    XiValue *suffix = xi_const_str(function, owned_path, "!", &stub_string);
    XiValue *owned_text = xi_value_new(function, owned_path, XI_STR_CONCAT, &stub_string, 2);
    REQUIRE(left_text != NULL && suffix != NULL && owned_text != NULL);
    owned_text->args[0] = left_text;
    owned_text->args[1] = suffix;
    owned_text->flags = xi_op_default_effects(XI_STR_CONCAT);
    xi_block_set_jump(owned_path, promoted_merge);
    XiValue *promotion = xi_value_new(function, borrowed_path, XI_RETAIN, &stub_unit, 1);
    REQUIRE(promotion != NULL);
    promotion->args[0] = first;
    promotion->flags = XI_FLAG_SIDE_EFFECT;
    xi_block_set_jump(borrowed_path, promoted_merge);
    phi = xi_phi_new(function, promoted_merge, &stub_string, promoted_merge->npreds);
    REQUIRE(phi != NULL && promoted_merge->npreds == 2);
    phi->value.args[0] = owned_text;
    phi->value.args[1] = first;
    XiValue *length = xi_value_new(function, promoted_merge, XI_LEN, &stub_int, 1);
    REQUIRE(length != NULL);
    length->args[0] = &phi->value;
    xi_block_set_return(promoted_merge, length);
    function->stage = XI_STAGE_OPTIMIZED;
    plan = NULL;
    memset(error, 0, sizeof(error));
    built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "mixed promoted PHI plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    XrOwnershipEventRecord *promotion_event = NULL;
    for (uint32_t i = 0; i < plan->ownership->event_count; i++) {
        XrOwnershipEventRecord *event = &plan->ownership->events[i];
        if (plan->operations[event->operation].opcode == XI_RETAIN) {
            promotion_event = event;
            break;
        }
    }
    REQUIRE(promotion_event != NULL && promotion_event->kind == XR_OWN_EVENT_RETAIN &&
            promotion_event->logical_delta == 1);
    int16_t saved_promotion_delta = promotion_event->logical_delta;
    promotion_event->logical_delta = 0;
    expect_verify_failure(plan, "XR_OWN_3002");
    promotion_event->logical_delta = saved_promotion_delta;
    xr_semantic_plan_free(plan);
    xi_func_free(function);

    function = xi_func_new("returned_mixed_borrowed_phi_probe", &stub_string);
    REQUIRE(function != NULL);
    entry = xi_block_new(function);
    XiBlock *left = xi_block_new(function);
    XiBlock *right = xi_block_new(function);
    XiBlock *merge = xi_block_new(function);
    REQUIRE(entry != NULL && left != NULL && right != NULL && merge != NULL);
    first = xi_param(function, entry, 0, &stub_string);
    XiValue *second = xi_param(function, entry, 1, &stub_string);
    REQUIRE(first != NULL && second != NULL);
    function->nparams = 2;
    function->params = (XiValue **) xr_malloc(2u * sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = first;
    function->params[1] = second;
    function->arc_borrow_sig =
        (XiBorrowSig *) xi_func_arena_alloc(function, (uint32_t) sizeof(*function->arc_borrow_sig));
    REQUIRE(function->arc_borrow_sig != NULL);
    function->arc_borrow_sig->nparams = 2;
    function->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    function->arc_borrow_sig->param_own[1] = XI_OWN_BORROWED;
    function->arc_borrow_sig->valid = true;
    condition = xi_const_bool(function, entry, true, &stub_bool);
    REQUIRE(condition != NULL);
    xi_block_set_if(entry, condition, left, right);
    xi_block_set_jump(left, merge);
    xi_block_set_jump(right, merge);
    phi = xi_phi_new(function, merge, &stub_string, merge->npreds);
    REQUIRE(phi != NULL && merge->npreds == 2);
    phi->value.args[0] = first;
    phi->value.args[1] = second;
    xi_block_set_return(merge, &phi->value);
    function->stage = XI_STAGE_OPTIMIZED;
    plan = NULL;
    memset(error, 0, sizeof(error));
    REQUIRE(!xr_semantic_plan_build(function, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL);
    REQUIRE(strncmp(error, "XR_OWN_3004", strlen("XR_OWN_3004")) == 0);
    xi_func_free(function);
}

static void test_stack_extent_is_logical_and_fail_closed(void) {
    XiFunc *function = xi_func_new("stack_extent_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *stack = xi_value_new(function, entry, XI_STACK_ALLOC, &stub_array, 0);
    REQUIRE(stack != NULL);
    stack->aux_int = XI_ARRAY_NEW;
    XiValue *result = xi_const_int(function, entry, 1, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "stack extent plan build failed: %s\n", error);
    REQUIRE(built);
    const XrOwnershipCertificate *ownership = xr_semantic_plan_ownership(plan);
    REQUIRE(ownership != NULL);
    bool saw_alloc = false;
    bool saw_destroy = false;
    bool saw_physical_release = false;
    for (uint32_t i = 0; i < xr_ownership_certificate_event_count(ownership); i++) {
        const XrOwnershipEventRecord *event = xr_ownership_certificate_event(ownership, i);
        REQUIRE(event != NULL);
        saw_alloc |= event->kind == XR_OWN_EVENT_ALLOC && event->logical_delta == 1;
        saw_destroy |= event->kind == XR_OWN_EVENT_DESTROY && event->logical_delta == -1;
        saw_physical_release |= event->kind == XR_OWN_EVENT_RELEASE;
    }
    REQUIRE(saw_alloc && saw_destroy && !saw_physical_release);
    xr_semantic_plan_free(plan);
    xi_func_free(function);

    function = xi_func_new("stack_extent_escape_probe", &stub_array);
    REQUIRE(function != NULL);
    entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    stack = xi_value_new(function, entry, XI_STACK_ALLOC, &stub_array, 0);
    REQUIRE(stack != NULL);
    stack->aux_int = XI_ARRAY_NEW;
    xi_block_set_return(entry, stack);
    function->stage = XI_STAGE_OPTIMIZED;
    plan = NULL;
    memset(error, 0, sizeof(error));
    REQUIRE(!xr_semantic_plan_build(function, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL);
    REQUIRE(strncmp(error, "XR_OWN_3001", strlen("XR_OWN_3001")) == 0);
    xi_func_free(function);
}

static void test_loop_redefinition_closes_previous_owner(void) {
    XiFunc *function = xi_func_new("loop_redefinition_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    XiBlock *header = xi_block_new(function);
    XiBlock *body = xi_block_new(function);
    XiBlock *latch = xi_block_new(function);
    XiBlock *exit = xi_block_new(function);
    REQUIRE(entry != NULL && header != NULL && body != NULL && latch != NULL && exit != NULL);

    xi_block_set_jump(entry, header);
    XiValue *condition = xi_const_bool(function, header, true, &stub_bool);
    REQUIRE(condition != NULL);
    xi_block_set_if(header, condition, body, exit);
    XiValue *capacity = xi_const_int(function, body, 1, &stub_int);
    REQUIRE(capacity != NULL);
    XiValue *array = xi_value_new(function, body, XI_ARRAY_NEW, &stub_array, 1);
    REQUIRE(array != NULL);
    array->args[0] = capacity;
    array->flags = xi_op_default_effects(XI_ARRAY_NEW);
    xi_block_set_jump(body, latch);
    XiValue *repeat = xi_const_bool(function, latch, true, &stub_bool);
    REQUIRE(repeat != NULL);
    xi_block_set_if(latch, repeat, header, header);
    XiValue *result = xi_const_int(function, exit, 0, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(exit, result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "loop-redefinition plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL && plan->ownership != NULL);

    uint32_t origin_block = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        if (plan->operations[i].opcode == XI_ARRAY_NEW) {
            origin_block = plan->operations[i].block;
            break;
        }
    }
    REQUIRE(origin_block != XR_SEMANTIC_INDEX_NONE);
    bool saw_redefinition_release = false;
    for (uint32_t i = 0; i < plan->ownership->event_count; i++) {
        const XrOwnershipEventRecord *event = &plan->ownership->events[i];
        saw_redefinition_release |= event->kind == XR_OWN_EVENT_RELEASE &&
                                    event->logical_delta == -1 && event->successor == origin_block;
    }
    REQUIRE(saw_redefinition_release);
    xr_semantic_plan_free(plan);
    xi_func_free(function);
}

static void test_owner_forward_creates_a_distinct_loop_owner(void) {
    XiFunc *function = xi_func_new("owner_forward_loop_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    XiBlock *header = xi_block_new(function);
    XiBlock *body = xi_block_new(function);
    XiBlock *exit = xi_block_new(function);
    REQUIRE(entry != NULL && header != NULL && body != NULL && exit != NULL);

    xi_block_set_jump(entry, header);
    XiValue *condition = xi_const_bool(function, header, true, &stub_bool);
    REQUIRE(condition != NULL);
    xi_block_set_if(header, condition, body, exit);
    XiValue *capacity = xi_const_int(function, body, 1, &stub_int);
    REQUIRE(capacity != NULL);
    XiValue *array = xi_value_new(function, body, XI_ARRAY_NEW, &stub_array, 1);
    REQUIRE(array != NULL);
    array->args[0] = capacity;
    array->flags = xi_op_default_effects(XI_ARRAY_NEW);
    XiValue *forward = xi_value_new(function, body, XI_OWNER_FORWARD, &stub_array, 1);
    REQUIRE(forward != NULL);
    forward->args[0] = array;
    XiValue *release = xi_value_new(function, body, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = forward;
    xi_block_set_jump(body, header);
    XiValue *result = xi_const_int(function, exit, 0, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(exit, result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "owner-forward loop plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL && plan->ownership != NULL);

    uint32_t array_owner = XR_SEMANTIC_INDEX_NONE;
    uint32_t forward_owner = XR_SEMANTIC_INDEX_NONE;
    bool saw_forward_alloc = false;
    bool saw_source_move = false;
    bool saw_forward_release = false;
    for (uint32_t i = 0; i < plan->ownership->owner_count; i++) {
        const XrOwnershipOwnerRecord *owner = &plan->ownership->owners[i];
        uint32_t origin = XR_SEMANTIC_INDEX_NONE;
        for (uint32_t op = 0; op < plan->operation_count; op++) {
            if (plan->operations[op].result_value == owner->origin_value) {
                origin = op;
                break;
            }
        }
        if (origin == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (plan->operations[origin].opcode == XI_ARRAY_NEW)
            array_owner = i;
        else if (plan->operations[origin].opcode == XI_OWNER_FORWARD)
            forward_owner = i;
    }
    REQUIRE(array_owner != XR_SEMANTIC_INDEX_NONE);
    REQUIRE(forward_owner != XR_SEMANTIC_INDEX_NONE);
    REQUIRE(array_owner != forward_owner);
    for (uint32_t i = 0; i < plan->ownership->event_count; i++) {
        const XrOwnershipEventRecord *event = &plan->ownership->events[i];
        saw_forward_alloc |= event->owner == forward_owner && event->kind == XR_OWN_EVENT_ALLOC &&
                             event->logical_delta == 1;
        saw_source_move |= event->owner == array_owner && event->kind == XR_OWN_EVENT_MOVE &&
                           event->logical_delta == -1;
        saw_forward_release |= event->owner == forward_owner &&
                               event->kind == XR_OWN_EVENT_RELEASE &&
                               event->logical_delta == -1;
    }
    REQUIRE(saw_forward_alloc && saw_source_move && saw_forward_release);
    xr_semantic_plan_free(plan);
    xi_func_free(function);
}

static void test_owner_origin_ignores_preceding_alias_storage_order(void) {
    XiFunc *function = xi_func_new("owner_origin_storage_order_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    XiBlock *alias_use = xi_block_new(function);
    XiBlock *definition = xi_block_new(function);
    XiBlock *exit = xi_block_new(function);
    REQUIRE(entry != NULL && alias_use != NULL && definition != NULL && exit != NULL);

    xi_block_set_jump(entry, definition);
    XiValue *capacity = xi_const_int(function, definition, 1, &stub_int);
    REQUIRE(capacity != NULL);
    XiValue *array = xi_value_new(function, definition, XI_ARRAY_NEW, &stub_array, 1);
    REQUIRE(array != NULL);
    array->args[0] = capacity;
    array->flags = xi_op_default_effects(XI_ARRAY_NEW);
    xi_block_set_jump(definition, alias_use);
    XiValue *alias = xi_value_new(function, alias_use, XI_COPY, &stub_array, 1);
    REQUIRE(alias != NULL);
    alias->args[0] = array;
    XiValue *release = xi_value_new(function, alias_use, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = alias;
    xi_block_set_jump(alias_use, exit);
    XiValue *result = xi_const_int(function, exit, 0, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(exit, result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "owner-origin storage-order plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL && plan->ownership != NULL);

    bool saw_array_origin = false;
    bool saw_copy_origin = false;
    for (uint32_t i = 0; i < plan->ownership->owner_count; i++) {
        const XrOwnershipOwnerRecord *owner = &plan->ownership->owners[i];
        for (uint32_t op = 0; op < plan->operation_count; op++) {
            if (plan->operations[op].result_value != owner->origin_value)
                continue;
            saw_array_origin |= plan->operations[op].opcode == XI_ARRAY_NEW;
            saw_copy_origin |= plan->operations[op].opcode == XI_COPY;
        }
    }
    REQUIRE(saw_array_origin && !saw_copy_origin);
    xr_semantic_plan_free(plan);
    xi_func_free(function);
}

static void test_nullable_borrowed_parameter_keeps_sealed_provenance(void) {
    XiFunc *function = xi_func_new("nullable_borrowed_parameter_probe", &stub_array);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    XiBlock *null_return = xi_block_new(function);
    XiBlock *parameter_return = xi_block_new(function);
    REQUIRE(entry != NULL && null_return != NULL && parameter_return != NULL);
    XiValue *parameter = xi_param(function, entry, 0, &stub_array);
    REQUIRE(parameter != NULL);
    function->nparams = 1;
    function->params = (XiValue **) xr_malloc(sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = parameter;
    function->arc_return_ownership.kind = XI_RETURN_OWNERSHIP_BORROWED_PARAM;
    function->arc_return_ownership.param_index = 0;
    function->arc_return_ownership.complete = true;

    XiValue *condition = xi_const_bool(function, entry, true, &stub_bool);
    REQUIRE(condition != NULL);
    xi_block_set_if(entry, condition, null_return, parameter_return);
    XiValue *null_value = xi_const_null(function, null_return, &stub_null);
    REQUIRE(null_value != NULL);
    xi_block_set_return(null_return, null_value);
    xi_block_set_return(parameter_return, parameter);
    xi_arc_analyze_contracts(function);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "nullable-borrow plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    REQUIRE(plan->functions[0].return_provenance == XR_SEM_RETURN_BORROWED_PARAM);
    REQUIRE(plan->functions[0].return_parameter == 0);
    xr_semantic_plan_free(plan);
    xi_func_free(function);
}

int main(void) {
    test_stable_ids();
    test_typed_entity_identity_table();
    test_incomplete_debug_span_fails_closed();
    test_operation_registry();
    test_immutable_owned_snapshot();
    test_string_builder_constructor_allocation_authority();
    test_xsm_roundtrip_and_determinism();
    test_fingerprint_separates_metadata_boundaries();
    test_xsm_signed_extreme_roundtrip();
    test_explicit_panic_edge_and_roundtrip();
    test_explicit_error_edge();
    test_typed_call_operand_contract();
    test_direct_local_call_target_authority();
    test_indirect_callable_state_authority();
    test_native_yieldable_call_target_authority();
    test_native_namespace_yieldable_authority();
    test_builtin_instance_yieldable_authority();
    test_source_export_call_target_authority();
    test_shared_direct_call_target_authority();
    test_parameter_and_capture_contracts();
    test_attachment_freezes_exact_function_identity();
    test_unknown_capture_contract_fails_closed();
    test_xsm_fail_closed_mutations();
    test_semantic_side_table_partitions();
    test_explicit_loop_invariant_certificate();
    test_semantic_and_ownership_mutations();
    test_owning_phi_frontiers();
    test_borrowed_phi_loan_frontiers();
    test_stack_extent_is_logical_and_fail_closed();
    test_loop_redefinition_closes_previous_owner();
    test_owner_forward_creates_a_distinct_loop_owner();
    test_owner_origin_ignores_preceding_alias_storage_order();
    test_nullable_borrowed_parameter_keeps_sealed_provenance();
    printf("SemanticPlan/XSM tests passed\n");
    return 0;
}
