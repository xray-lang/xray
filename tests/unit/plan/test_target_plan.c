/*
 * test_target_plan.c - Immutable backend-neutral TargetPlan contract
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_ops_gen.h"
#include "../../../src/ir/xi_coro_lower.h"
#include "../../../src/ir/xi_stage.h"
#include "../../../src/ir/xi_module.h"
#include "../../../src/ir/xi_own.h"
#include "../../../src/frontend/analyzer/xa_intrinsic_registry.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/plan/format/xr_xtp_internal.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/semantic/xr_semantic_plan_internal.h"
#include "../../../src/plan/semantic/xr_semantic_verify.h"
#include "../../../src/plan/ownership/xr_ownership_certificate_internal.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/aot/emit_c/xr_c_emission_plan.h"
#include "../../../src/plan/target/xr_target_plan_internal.h"
#include "../../../src/plan/target/xr_target_profile_internal.h"
#include "../../../src/plan/target/xr_target_verify.h"
#include "../../../src/aot/emit_c/xr_c_emission_plan.h"
#include "../../../src/runtime/class/xclass_info.h"
#include "../../../src/runtime/value/xstruct_layout.h"
#include "../../../src/runtime/value/xenum_layout.h"
#include "../../../src/runtime/value/xtype.h"
#include "target_profile_test_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

typedef struct TargetFixture {
    XrTargetMachineRepRecord reps[7];
    XrTargetValueRepRecord value_reps[3];
    XrTargetExtentRecord extents[1];
    XrTargetLayoutRecord layouts[2];
    XrTargetFieldRecord fields[1];
    XrTargetStorageRecord storage[1];
    XrTargetAllocationRecord allocations[1];
    XrTargetFunctionRecord functions[1];
    XrTargetSlotRecord slots[3];
    XrTargetCallRecord calls[1];
    XrTargetCallArgumentRecord call_arguments[1];
    XrTargetRootMapRecord roots[1];
    uint32_t root_slots[1];
    XrTargetCleanupRecord cleanups[1];
    XrTargetAdapterRecord adapters[1];
    XrTargetCapabilityRecord capabilities[2];
    XrTargetCoroutineStateRecord coroutines[1];
    XrTargetPlanDraft draft;
} TargetFixture;

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_string = {.kind = XR_KIND_STRING, .id = 2, .frozen = true};
static XrType stub_exact_string = {
    .kind = XR_KIND_STRING,
    .id = 8,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_unit = {
    .kind = XR_KIND_UNIT,
    .id = 3,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_bool = {
    .kind = XR_KIND_BOOL,
    .id = 4,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_nullable_int = {
    .kind = XR_KIND_INT,
    .id = 5,
    .frozen = true,
    .is_nullable = true,
};
static XrType stub_function = {
    .kind = XR_KIND_FUNCTION,
    .id = 6,
    .frozen = true,
    .function = {.return_type = &stub_int, .throw_effect = XR_FN_EFFECT_NO_THROW},
};
static XrType stub_exact_function = {
    .kind = XR_KIND_FUNCTION,
    .id = 7,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .function = {.return_type = &stub_int, .throw_effect = XR_FN_EFFECT_NO_THROW},
};
static XrType stub_channel = {
    .kind = XR_KIND_CHANNEL,
    .id = 110,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .container = {.element_type = &stub_int},
};
static XrType stub_float = {
    .kind = XR_KIND_FLOAT,
    .id = 111,
    .frozen = true,
    .scalar_rep = XR_NATIVE_F64,
};
static XrType stub_float_channel = {
    .kind = XR_KIND_CHANNEL,
    .id = 112,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .container = {.element_type = &stub_float},
};
static XrType stub_module_namespace = {
    .kind = XR_KIND_STRUCT_OBJECT,
    .id = 111,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_string_builder = {
    .kind = XR_KIND_INSTANCE,
    .id = 113,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance = {.class_name = "StringBuilder"},
};
static XrType stub_rune = {
    .kind = XR_KIND_RUNE,
    .id = 119,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_u32 = {
    .kind = XR_KIND_INT,
    .id = 121,
    .frozen = true,
    .scalar_rep = XR_NATIVE_U32,
};
static XrType *stub_iterator_rune_args[] = {&stub_rune};
static XrType stub_iterator_rune = {
    .kind = XR_KIND_INSTANCE,
    .id = 120,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance = {
        .class_name = "Iterator",
        .type_args = stub_iterator_rune_args,
        .type_arg_count = 1,
    },
};
static XrType stub_raw_pointer = {
    .kind = XR_KIND_POINTER,
    .id = 121,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_raw_pointer_function = {
    .kind = XR_KIND_FUNCTION,
    .id = 122,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .function = {.return_type = &stub_raw_pointer,
                 .throw_effect = XR_FN_EFFECT_NO_THROW},
};
static XrType stub_target_u8 = {
    .kind = XR_KIND_INT,
    .id = 117,
    .frozen = true,
    .scalar_rep = XR_NATIVE_U8,
};
static XrType stub_target_u8_array = {
    .kind = XR_KIND_ARRAY,
    .id = 120,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .container = {.element_type = &stub_target_u8},
};
static XrType stub_target_u8_slice = {
    .kind = XR_KIND_SLICE,
    .id = 118,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .container = {.element_type = &stub_target_u8},
};
static XrClassInfo stub_target_source_class_info = {.name = "FinalTargetWorker"};
static XrClassInfo stub_target_open_source_class_info = {
    .name = "OpenTargetWorker",
    .xg_class_id = 142,
};
static XrClassInfo stub_target_imported_open_source_class_info = {
    .name = "OpenTargetWorker",
    .xg_class_id = 142,
};
static XrType stub_target_source_instance = {
    .kind = XR_KIND_INSTANCE,
    .id = 114,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance =
        {
            .class_name = "FinalTargetWorker",
            .class_ref = &stub_target_source_class_info,
        },
};
static XrType stub_target_open_source_instance = {
    .kind = XR_KIND_INSTANCE,
    .id = 115,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance =
        {
            .class_name = "OpenTargetWorker",
            .class_ref = &stub_target_open_source_class_info,
        },
};
static XrType stub_target_imported_open_source_instance = {
    .kind = XR_KIND_INSTANCE,
    .id = 116,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance =
        {
            .class_name = "OpenTargetWorker",
            .class_ref = &stub_target_imported_open_source_class_info,
        },
};

static XrSemanticPlan *build_source_instance_method_semantic(void) {
    XiFunc *root = xi_func_new("target_source_instance_root", &stub_unit);
    XiFunc *callee = xi_func_new("wait", &stub_unit);
    XiFunc *caller = xi_func_new("run", &stub_unit);
    REQUIRE(root != NULL && callee != NULL && caller != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *callee_entry = xi_block_new(callee);
    XiBlock *caller_entry = xi_block_new(caller);
    REQUIRE(root_entry != NULL && callee_entry != NULL && caller_entry != NULL);
    root->children = (XiFunc **) xr_malloc(2u * sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = callee;
    root->children[1] = caller;
    root->nchildren = root->children_cap = 2;
    callee->parent_func = caller->parent_func = root;
    XiValue *callee_this = xi_param(callee, callee_entry, 0, &stub_target_source_instance);
    XiValue *caller_this = xi_param(caller, caller_entry, 0, &stub_target_source_instance);
    REQUIRE(callee_this != NULL && caller_this != NULL);
    callee->params = (XiValue **) xr_malloc(sizeof(*callee->params));
    caller->params = (XiValue **) xr_malloc(sizeof(*caller->params));
    REQUIRE(callee->params != NULL && caller->params != NULL);
    callee->params[0] = callee_this;
    caller->params[0] = caller_this;
    callee->nparams = caller->nparams = 1;
    XiValue *yield = xi_value_new(callee, callee_entry, XI_YIELD, &stub_unit, 0);
    REQUIRE(yield != NULL);
    xi_block_set_return(callee_entry, yield);
    XiValue *call = xi_value_new(caller, caller_entry, XI_CALL_METHOD, &stub_unit, 1);
    REQUIRE(call != NULL);
    call->args[0] = caller_this;
    call->aux = "wait";
    xi_block_set_return(caller_entry, call);
    xi_block_set_return(root_entry, NULL);
    XiCoroSuspendPoint callee_point = {.state_id = 1, .op = yield, .kind = XI_CORO_SUSP_YIELD};
    XiCoroSuspendPoint caller_point = {.state_id = 1, .op = call, .kind = XI_CORO_SUSP_CALL};
    XiCoroPlan callee_coroutine = {.is_coroutine = true, .nstates = 1, .points = &callee_point};
    XiCoroPlan caller_coroutine = {.is_coroutine = true, .nstates = 1, .points = &caller_point};
    callee->coro_plan = &callee_coroutine;
    caller->coro_plan = &caller_coroutine;
    root->stage = callee->stage = caller->stage = XI_STAGE_OPTIMIZED;
    XiModule *module =
        xi_module_new("pkg/target_source_instance.xr", "target_source_instance", root);
    REQUIRE(module != NULL);
    root->module = module;
    XiClassMethod methods[2] = {{.name = "wait"}, {.name = "run"}};
    uint16_t child_indices[2] = {0, 1};
    XiClassData source_class = {
        .class_info = &stub_target_source_class_info,
        .class_name = "FinalTargetWorker",
        .methods = methods,
        .nmethod = 2,
        .child_idx = child_indices,
        .ninst = 2,
        .explicit_final = true,
        .needs_runtime_type = true,
    };
    module->classes = (XiClassData **) xr_malloc(sizeof(*module->classes));
    REQUIRE(module->classes != NULL);
    module->classes[0] = &source_class;
    module->nclasses = 1;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(root, &semantic, error, sizeof(error)));
    REQUIRE(semantic != NULL && semantic->call_target_count == 1 &&
            semantic->call_targets[0].kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL);
    root->module = NULL;
    xi_func_free(root);
    module->init = NULL;
    xi_module_free(module);
    return semantic;
}

static XrSemanticPlan *build_open_source_instance_method_semantic(XrSemanticPlan **dependency_out) {
    XiFunc *dependency_root = xi_func_new("open_target_root", &stub_unit);
    XiFunc *wait = xi_func_new("wait", &stub_unit);
    REQUIRE(dependency_root != NULL && wait != NULL);
    XiBlock *dependency_entry = xi_block_new(dependency_root);
    XiBlock *wait_entry = xi_block_new(wait);
    REQUIRE(dependency_entry != NULL && wait_entry != NULL);
    dependency_root->children = (XiFunc **) xr_malloc(sizeof(*dependency_root->children));
    REQUIRE(dependency_root->children != NULL);
    dependency_root->children[0] = wait;
    dependency_root->nchildren = dependency_root->children_cap = 1;
    wait->parent_func = dependency_root;
    XiValue *self = xi_param(wait, wait_entry, 0, &stub_target_open_source_instance);
    REQUIRE(self != NULL);
    wait->params = (XiValue **) xr_malloc(sizeof(*wait->params));
    REQUIRE(wait->params != NULL);
    wait->params[0] = self;
    wait->nparams = 1;
    XiValue *yield = xi_value_new(wait, wait_entry, XI_YIELD, &stub_unit, 0);
    REQUIRE(yield != NULL);
    xi_block_set_return(wait_entry, yield);
    xi_block_set_return(dependency_entry, NULL);
    XiCoroSuspendPoint wait_point = {
        .state_id = 1,
        .op = yield,
        .kind = XI_CORO_SUSP_YIELD,
    };
    XiCoroPlan wait_coroutine = {
        .is_coroutine = true,
        .nstates = 1,
        .points = &wait_point,
    };
    wait->coro_plan = &wait_coroutine;
    dependency_root->stage = wait->stage = XI_STAGE_OPTIMIZED;
    XiModule *dependency_module =
        xi_module_new("pkg/open_target.xr", "open_target", dependency_root);
    REQUIRE(dependency_module != NULL);
    dependency_root->module = dependency_module;
    XiClassMethod method = {.name = "wait"};
    uint16_t child = 0;
    XiClassData source_class = {
        .class_info = &stub_target_open_source_class_info,
        .xg_class_id = 142,
        .class_name = "OpenTargetWorker",
        .methods = &method,
        .nmethod = 1,
        .child_idx = &child,
        .ninst = 1,
        .explicit_final = false,
        .needs_runtime_type = true,
    };
    dependency_module->classes = (XiClassData **) xr_malloc(sizeof(*dependency_module->classes));
    REQUIRE(dependency_module->classes != NULL);
    dependency_module->classes[0] = &source_class;
    dependency_module->nclasses = 1;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(dependency_root, error, sizeof(error)));
    XrSemanticPlan *dependency = xr_semantic_plan_retain(dependency_root->semantic_plan);
    REQUIRE(dependency != NULL && dependency->source_method_count == 1);

    XiFunc *caller = xi_func_new("call_open_target", &stub_unit);
    REQUIRE(caller != NULL);
    XiBlock *caller_entry = xi_block_new(caller);
    REQUIRE(caller_entry != NULL);
    XiValue *receiver =
        xi_param(caller, caller_entry, 0, &stub_target_imported_open_source_instance);
    REQUIRE(receiver != NULL);
    caller->params = (XiValue **) xr_malloc(sizeof(*caller->params));
    REQUIRE(caller->params != NULL);
    caller->params[0] = receiver;
    caller->nparams = 1;
    XiValue *call = xi_value_new(caller, caller_entry, XI_CALL_METHOD, &stub_unit, 1);
    REQUIRE(call != NULL);
    call->args[0] = receiver;
    call->aux = "wait";
    xi_block_set_return(caller_entry, call);
    XiCoroSuspendPoint caller_point = {
        .state_id = 1,
        .op = call,
        .kind = XI_CORO_SUSP_CALL,
    };
    XiCoroPlan caller_coroutine = {
        .is_coroutine = true,
        .nstates = 1,
        .points = &caller_point,
    };
    caller->coro_plan = &caller_coroutine;
    caller->stage = XI_STAGE_OPTIMIZED;
    XiModule *caller_module = xi_module_new("pkg/open_target_user.xr", "open_target_user", caller);
    REQUIRE(caller_module != NULL);
    caller->module = caller_module;
    XiModule *dependency_modules[] = {dependency_module};
    REQUIRE(xr_semantic_plan_build_and_attach_module_set(caller, dependency_modules, 1, error,
                                                         sizeof(error)));
    XrSemanticPlan *semantic = xr_semantic_plan_retain(caller->semantic_plan);
    REQUIRE(semantic != NULL && semantic->call_target_count == 1 &&
            semantic->call_targets[0].kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_OPEN);

    caller->module = NULL;
    xi_func_free(caller);
    caller_module->init = NULL;
    xi_module_free(caller_module);
    dependency_root->module = NULL;
    xi_func_free(dependency_root);
    dependency_module->init = NULL;
    xi_module_free(dependency_module);
    *dependency_out = dependency;
    return semantic;
}

static XrSemanticPlan *build_stringbuilder_constructor_semantic(void) {
    XiFunc *function = xi_func_new("target_stringbuilder_constructor", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *builder = xi_value_new(function, entry, XI_CALL_BUILTIN, &stub_string_builder, 0);
    REQUIRE(builder != NULL);
    builder->aux = (void *) "StringBuilder";
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = builder;
    XiValue *result = xi_const_int(function, entry, 0, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error, sizeof(error)));
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_array_intrinsic_semantic(void) {
    XiFunc *function = xi_func_new("target_array_intrinsics", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *capacity = xi_const_int(function, entry, 2, &stub_int);
    XiValue *with_capacity =
        xi_value_new(function, entry, XI_CALL_BUILTIN,
                     &stub_target_u8_array, 1);
    REQUIRE(capacity != NULL && with_capacity != NULL);
    with_capacity->args[0] = capacity;
    with_capacity->aux = (void *) "array_with_capacity";
    with_capacity->array_intrinsic_kind = XI_ARRAY_INTRINSIC_WITH_CAPACITY;
    with_capacity->array_element_storage = XR_ELEM_U8;
    XiValue *release_capacity =
        xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release_capacity != NULL);
    release_capacity->args[0] = with_capacity;

    XiValue *count = xi_const_int(function, entry, 3, &stub_int);
    XiValue *fill = xi_const_int(function, entry, 1, &stub_int);
    XiValue *filled =
        xi_value_new(function, entry, XI_CALL_BUILTIN,
                     &stub_target_u8_array, 2);
    REQUIRE(count != NULL && fill != NULL && filled != NULL);
    filled->args[0] = count;
    filled->args[1] = fill;
    filled->aux = (void *) "array_filled_new";
    filled->array_intrinsic_kind = XI_ARRAY_INTRINSIC_FILLED_NEW;
    filled->array_element_storage = XR_ELEM_U8;
    XiValue *release_filled =
        xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release_filled != NULL);
    release_filled->args[0] = filled;

    XiValue *result = xi_const_int(function, entry, 0, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &semantic, error,
                                        sizeof(error));
    if (!built)
        fprintf(stderr, "Array intrinsic semantic failed: %s\n", error);
    REQUIRE(built && semantic != NULL);
    xi_func_free(function);
    return semantic;
}

/* The three StringBuilder method families are all stated against the same
 * exact constructor receiver, so every fixture starts from one. */
static XiValue *emit_exact_string_builder(XiFunc *function, XiBlock *entry) {
    XiValue *builder = xi_value_new(function, entry, XI_CALL_BUILTIN, &stub_string_builder, 0);
    REQUIRE(builder != NULL);
    builder->aux = (void *) "StringBuilder";
    return builder;
}

static XrSemanticPlan *finish_stringbuilder_semantic(XiFunc *function, XiBlock *entry,
                                                     const char *label) {
    XiValue *result = xi_const_int(function, entry, 0, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &semantic, error, sizeof(error));
    if (!built)
        fprintf(stderr, "%s Target semantic failed: %s\n", label, error);
    REQUIRE(built && semantic != NULL);
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_stringbuilder_append_rune_semantic(void) {
    XiFunc *function = xi_func_new("target_stringbuilder_append_rune", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *builder = emit_exact_string_builder(function, entry);
    XiValue *rune = xi_const_rune(function, entry, 'x', &stub_rune);
    REQUIRE(rune != NULL);
    XiValue *append = xi_value_new(function, entry, XI_CALL_METHOD, &stub_string_builder, 2);
    REQUIRE(append != NULL);
    append->args[0] = builder;
    append->args[1] = rune;
    append->aux = (void *) "append";
    append->aux_int = 2;
    append->result_alias_operand = 0;
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = append;
    return finish_stringbuilder_semantic(function, entry, "StringBuilder.append(rune)");
}

static XrSemanticPlan *build_stringbuilder_to_string_semantic(void) {
    XiFunc *function = xi_func_new("target_stringbuilder_to_string", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *builder = emit_exact_string_builder(function, entry);
    XiValue *to_string = xi_value_new(function, entry, XI_CALL_METHOD, &stub_exact_string, 1);
    REQUIRE(to_string != NULL);
    to_string->args[0] = builder;
    to_string->aux = (void *) "toString";
    to_string->aux_int = 2;
    XiValue *release_string = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    XiValue *release_builder = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release_string != NULL && release_builder != NULL);
    release_string->args[0] = to_string;
    release_builder->args[0] = builder;
    return finish_stringbuilder_semantic(function, entry, "StringBuilder.toString");
}

static XrSemanticPlan *build_stringbuilder_append_string_semantic(void) {
    XiFunc *function = xi_func_new("target_stringbuilder_append_string", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *builder = emit_exact_string_builder(function, entry);
    XiValue *text = xi_const_str(function, entry, "target-append", &stub_exact_string);
    REQUIRE(text != NULL);
    XiValue *append = xi_value_new(function, entry, XI_CALL_METHOD, &stub_string_builder, 2);
    REQUIRE(append != NULL);
    append->args[0] = builder;
    append->args[1] = text;
    append->aux = (void *) "append";
    append->aux_int = 2;
    append->result_alias_operand = 0;
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = append;
    return finish_stringbuilder_semantic(function, entry, "StringBuilder.append(string)");
}

static XrSemanticPlan *build_string_runes_semantic(void) {
    XiFunc *function = xi_func_new("target_string_runes", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(function, entry, "target-runes", &stub_exact_string);
    XiValue *runes = xi_value_new(function, entry, XI_CALL_METHOD,
                                  &stub_iterator_rune, 1);
    REQUIRE(source != NULL && runes != NULL);
    runes->args[0] = source;
    runes->aux = (void *) "runes";
    runes->aux_int = 470;
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = runes;
    return finish_stringbuilder_semantic(function, entry, "String.runes");
}

static XrSemanticPlan *build_iterator_rune_has_next_semantic(void) {
    XiFunc *function = xi_func_new("target_iterator_rune_has_next", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(function, entry, "target-has-next",
                                   &stub_exact_string);
    XiValue *runes = xi_value_new(function, entry, XI_CALL_METHOD,
                                  &stub_iterator_rune, 1);
    XiValue *has_next = xi_value_new(function, entry, XI_CALL_METHOD,
                                     &stub_bool, 1);
    REQUIRE(source && runes && has_next);
    runes->args[0] = source;
    runes->aux = (void *) "runes";
    runes->aux_int = 470;
    has_next->args[0] = runes;
    has_next->aux = (void *) "hasNext";
    has_next->aux_int = 112;
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = runes;
    return finish_stringbuilder_semantic(function, entry,
                                         "Iterator<rune>.hasNext");
}

static XrSemanticPlan *build_iterator_rune_next_semantic(void) {
    XiFunc *function = xi_func_new("target_iterator_rune_next", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(function, entry, "target-next",
                                   &stub_exact_string);
    XiValue *runes = xi_value_new(function, entry, XI_CALL_METHOD,
                                  &stub_iterator_rune, 1);
    XiValue *next = xi_value_new(function, entry, XI_CALL_METHOD,
                                 &stub_rune, 1);
    REQUIRE(source && runes && next);
    runes->args[0] = source;
    runes->aux = (void *) "runes";
    runes->aux_int = 470;
    next->args[0] = runes;
    next->aux = (void *) "next";
    next->aux_int = 114;
    next->call_return_ownership.kind = XI_RETURN_OWNERSHIP_OWNED;
    next->call_return_ownership.param_index = -1;
    next->call_return_ownership.complete = true;
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = runes;
    return finish_stringbuilder_semantic(function, entry,
                                         "Iterator<rune>.next");
}

static XrSemanticPlan *build_rune_to_uint32_semantic(void) {
    XiFunc *function = xi_func_new("target_rune_to_uint32", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(function, entry, "target-rune-u32",
                                   &stub_exact_string);
    XiValue *runes = xi_value_new(function, entry, XI_CALL_METHOD,
                                  &stub_iterator_rune, 1);
    XiValue *next = xi_value_new(function, entry, XI_CALL_METHOD,
                                 &stub_rune, 1);
    XiValue *to_u32 = xi_value_new(function, entry, XI_CALL_METHOD,
                                   &stub_u32, 1);
    REQUIRE(source && runes && next && to_u32);
    runes->args[0] = source;
    runes->aux = (void *) "runes";
    runes->aux_int = 470;
    next->args[0] = runes;
    next->aux = (void *) "next";
    next->aux_int = 114;
    next->call_return_ownership.kind = XI_RETURN_OWNERSHIP_OWNED;
    next->call_return_ownership.param_index = -1;
    next->call_return_ownership.complete = true;
    to_u32->args[0] = next;
    to_u32->aux = (void *) "toUInt32";
    to_u32->aux_int = 474;
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = runes;
    return finish_stringbuilder_semantic(function, entry, "rune.toUInt32");
}

static XrSemanticPlan *build_rune_is_whitespace_semantic(void) {
    XiFunc *function = xi_func_new("target_rune_is_whitespace", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(function, entry, "target-rune-whitespace",
                                   &stub_exact_string);
    XiValue *runes = xi_value_new(function, entry, XI_CALL_METHOD,
                                  &stub_iterator_rune, 1);
    XiValue *next = xi_value_new(function, entry, XI_CALL_METHOD,
                                 &stub_rune, 1);
    XiValue *is_whitespace = xi_value_new(function, entry, XI_CALL_METHOD,
                                          &stub_bool, 1);
    REQUIRE(source && runes && next && is_whitespace);
    runes->args[0] = source;
    runes->aux = (void *) "runes";
    runes->aux_int = 470;
    next->args[0] = runes;
    next->aux = (void *) "next";
    next->aux_int = 114;
    next->call_return_ownership.kind = XI_RETURN_OWNERSHIP_OWNED;
    next->call_return_ownership.param_index = -1;
    next->call_return_ownership.complete = true;
    is_whitespace->args[0] = next;
    is_whitespace->aux = (void *) "isWhitespace";
    is_whitespace->aux_int = 90;
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = runes;
    return finish_stringbuilder_semantic(function, entry,
                                         "rune.isWhitespace");
}

static XrSemanticPlan *build_string_byte_slice_view_semantic(void) {
    XiFunc *function = xi_func_new("target_string_byte_slice_view", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(function, entry, "target-view", &stub_exact_string);
    XiValue *view = xi_value_new(function, entry, XI_CALL_BUILTIN,
                                 &stub_target_u8_slice, 1);
    REQUIRE(source && view);
    view->args[0] = source;
    view->xa_intrinsic_id = XA_INTRINSIC_STRING_BYTE_SLICE_VIEW;
    view->view_evidence = (XiViewEvidence) {
        .root_value_id = source->id,
        .element_type_id = stub_target_u8.id,
        .source_operand = 0,
        .source_param = -1,
        .origin = XI_VIEW_ORIGIN_RECEIVER,
        .capability = 1,
        .lifetime = 1,
        .complete = 1,
    };
    XiValue *result = xi_const_int(function, entry, 0, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &semantic, error, sizeof(error));
    if (!built)
        fprintf(stderr, "string byte-slice Target semantic failed: %s\n", error);
    REQUIRE(built && semantic != NULL);
    xi_func_free(function);
    return semantic;
}

typedef struct SourceExportResolverFixture {
    const XiFunc *callee;
    int suspendability;
} SourceExportResolverFixture;

static int source_export_call_suspendability(void *ud, const XiFunc *current, const XiValue *call) {
    (void) current;
    const SourceExportResolverFixture *fixture = (const SourceExportResolverFixture *) ud;
    return fixture && call && call->op == XI_CALL_METHOD && call->aux &&
                   strcmp((const char *) call->aux, "writeBytes") == 0
               ? fixture->suspendability
               : -1;
}

static const XiFunc *source_export_resolve_method(void *ud, const XiFunc *current,
                                                  const XiValue *call) {
    (void) current;
    const SourceExportResolverFixture *fixture = (const SourceExportResolverFixture *) ud;
    return fixture && call && call->op == XI_CALL_METHOD && call->aux &&
                   strcmp((const char *) call->aux, "writeBytes") == 0
               ? fixture->callee
               : NULL;
}

static void fill_foundation_capabilities(XrTargetCapabilityRecord capabilities[2]) {
    capabilities[0] = (XrTargetCapabilityRecord) {
        .id = 0,
        .capability = XR_TARGET_PROVIDER_ALLOCATOR,
        .provider = XR_TARGET_PROVIDER_ALLOCATOR,
        .flags = XR_TARGET_CAPABILITY_REQUIRED,
    };
    capabilities[1] = (XrTargetCapabilityRecord) {
        .id = 1,
        .capability = XR_TARGET_PROVIDER_PANIC,
        .provider = XR_TARGET_PROVIDER_PANIC,
        .flags = XR_TARGET_CAPABILITY_REQUIRED,
    };
}

static XrSemanticPlan *build_semantic_plan_with_string_type(XrType *string_type) {
    XiFunc *function = xi_func_new("target_plan_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *result = xi_const_int(function, entry, 42, &stub_int);
    REQUIRE(result != NULL);
    XiValue *string = xi_const_str(function, entry, "target", string_type);
    REQUIRE(string != NULL);
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = string;
    REQUIRE(xi_const_int(function, entry, 7, &stub_int) != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    REQUIRE(xr_semantic_plan_function_count(plan) == 1);
    REQUIRE(xr_semantic_plan_type_count(plan) >= 2);
    REQUIRE(xr_semantic_plan_operation_count(plan) >= 1);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_semantic_plan(void) {
    return build_semantic_plan_with_string_type(&stub_string);
}

static XrSemanticPlan *build_exact_string_semantic_plan(void) {
    return build_semantic_plan_with_string_type(&stub_exact_string);
}

static XrTargetProfile *build_profile(uint64_t extra_atomic_width) {
    XrTestTargetProfileFixture fixture;
    REQUIRE(xr_test_target_profile_fixture_init(&fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    fixture.input.machine.atomic_width_mask |= extra_atomic_width;
    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    bool built = xr_target_profile_build(&fixture.input, &profile, error, sizeof(error));
    if (!built)
        fprintf(stderr, "target profile failed: %s\n", error);
    REQUIRE(built && profile != NULL);
    return profile;
}

static uint32_t operation_for_value(const XrSemanticPlan *semantic, uint32_t value) {
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (operation && operation->result_value == value)
            return i;
    }
    return XR_SEMANTIC_INDEX_NONE;
}

static const XrSemanticParameterRecord *parameter_for_value(const XrSemanticPlan *semantic,
                                                            uint32_t function, uint32_t value) {
    uint32_t parameter_count = (uint32_t) xr_semantic_plan_parameter_count(semantic);
    for (uint32_t i = 0; i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(semantic, i);
        if (parameter && parameter->function == function && parameter->value == value)
            return parameter;
    }
    return NULL;
}

static void fill_slot_source(const XrSemanticPlan *semantic, XrTargetSlotRecord *slot,
                             uint32_t function, uint32_t value) {
    uint32_t operation_index = operation_for_value(semantic, value);
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(semantic, operation_index);
    const XrSemanticParameterRecord *parameter = parameter_for_value(semantic, function, value);
    const XrSemanticFunctionRecord *semantic_function =
        xr_semantic_plan_function(semantic, function);
    REQUIRE(operation && semantic_function && operation->function == function);
    XrStableId source = operation->id;
    slot->role = operation->opcode == XI_PHI ? XR_TARGET_SLOT_PHI : XR_TARGET_SLOT_TEMPORARY;
    if (parameter) {
        REQUIRE(operation->opcode == XI_PARAM);
        source = parameter->id;
        slot->role = XR_TARGET_SLOT_PARAMETER;
    }
    slot->semantic_value = value;
    slot->semantic_operation = operation_index;
    slot->logical_slot = XR_SEMANTIC_INDEX_NONE;
    char function_id[XR_STABLE_ID_BYTES * 2 + 1];
    char source_id[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    xr_stable_id_hex(semantic_function->id, function_id);
    xr_stable_id_hex(source, source_id);
    int written =
        snprintf(key, sizeof(key), "xray-target-slot-v2:function=%s:role=%u:source=%s:logical=%u",
                 function_id, (unsigned) slot->role, source_id, XR_SEMANTIC_INDEX_NONE);
    XrFingerprint digest;
    REQUIRE(written > 0 && (size_t) written < sizeof(key));
    REQUIRE(xr_stable_id_from_key(key, &slot->identity, &digest));
}

static XrSemanticPlan *build_single_scalar_semantic(XrType *type) {
    XiFunc *function = xi_func_new("target_scalar_probe", type);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *result = type->kind == XR_KIND_BOOL ? xi_const_bool(function, entry, true, type)
                                                 : xi_const_int(function, entry, 1, type);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &plan, error, sizeof(error)));
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_heap_closure_semantic(bool captured) {
    XiFunc *root = xi_func_new(
        captured ? "target_capturing_closure_root" : "target_exact_closure_root", &stub_unit);
    XiFunc *child = xi_func_new(
        captured ? "target_capturing_closure_child" : "target_exact_closure_child", &stub_int);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);
    XiValue *child_result = xi_const_int(child, child_entry, 42, &stub_int);
    REQUIRE(child_result != NULL);
    xi_block_set_return(child_entry, child_result);

    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;

    XiValue *captured_value = NULL;
    if (captured) {
        captured_value = xi_const_int(root, root_entry, 7, &stub_int);
        REQUIRE(captured_value != NULL);
        child->ncaptures = 1;
        child->captures[0] = (XiCapture) {
            .source = XI_CAPTURE_SRC_REG,
            .capture_kind = XI_CAPTURE_BY_COPY,
            .type = &stub_int,
            .value = captured_value,
            .name = "captured",
            .storage_domain = XR_STORAGE_EXEC_LOCAL,
            .value_capability = XR_SEM_VALUE_CONST,
        };
    }
    XiValue *closure =
        xi_value_new(root, root_entry, XI_CLOSURE_NEW, &stub_exact_function, captured ? 1 : 0);
    REQUIRE(closure != NULL);
    closure->aux = child;
    if (captured)
        closure->args[0] = captured_value;
    XiValue *store = xi_value_new(root, root_entry, XI_SET_SHARED, &stub_unit, 1);
    REQUIRE(store != NULL);
    store->args[0] = closure;
    store->aux_int = 0;
    xi_block_set_return(root_entry, NULL);
    root->stage = child->stage = XI_STAGE_OPTIMIZED;

    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "heap closure semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(root);
    return plan;
}

static XrSemanticPlan *build_parameter_without_operation_semantic(void) {
    XiFunc *function = xi_func_new("target_parameter_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    function->nparams = 1;
    function->min_params = 1;
    function->params = (XiValue **) xr_calloc(1, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    XiValue *parameter = xi_param(function, entry, 0, &stub_int);
    REQUIRE(parameter != NULL && entry->nvalues == 1);
    function->params[0] = parameter;
    xi_block_set_return(entry, parameter);
    function->stage = XI_STAGE_OPTIMIZED;

    /* A verified SemanticPlan may carry parameter SSA authority without a
     * duplicate XI_PARAM operation record. Keep the Xi value alive for the
     * builder, but exclude it from operation collection for this fixture. */
    entry->nvalues = 0;
    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &plan, error, sizeof(error)));
    REQUIRE(xr_semantic_plan_parameter_count(plan) == 1);
    REQUIRE(xr_semantic_plan_operation_count(plan) == 0);
    entry->values[entry->nvalues++] = parameter;
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_scalar_and_effect_void_same_type_semantic(void) {
    XiFunc *function = xi_func_new("target_effect_void_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *result = xi_const_int(function, entry, 1, &stub_int);
    XiValue *released = xi_const_int(function, entry, 2, &stub_int);
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_int, 1);
    REQUIRE(result && released && release);
    release->args[0] = released;
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &plan, error, sizeof(error)));
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_nested_aggregate_semantic(void) {
    XrType fixed = {
        .kind = XR_KIND_FIXED_ARRAY,
        .id = 101,
        .frozen = true,
        .is_value_type = true,
        .scalar_rep = XR_SCALAR_REP_NONE,
    };
    fixed.fixed_array.element_type = &stub_int;
    fixed.fixed_array.length = 3;
    XrType *tuple_elements[2] = {&stub_bool, &fixed};
    XrType tuple = {
        .kind = XR_KIND_TUPLE,
        .id = 102,
        .frozen = true,
        .is_value_type = true,
        .scalar_rep = XR_SCALAR_REP_NONE,
    };
    tuple.tuple.element_types = tuple_elements;
    tuple.tuple.element_count = 2;

    XiFunc *function = xi_func_new("target_nested_aggregate_probe", &tuple);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *array = xi_value_new(function, entry, XI_FIXED_ARRAY_NEW, &fixed, 0);
    REQUIRE(array != NULL);
    array->aux_int = 3;
    XiValue *boolean = xi_const_bool(function, entry, true, &stub_bool);
    REQUIRE(boolean != NULL);
    XiValue *result = xi_value_new(function, entry, XI_TUPLE_NEW, &tuple, 2);
    REQUIRE(result != NULL);
    result->args[0] = boolean;
    result->args[1] = array;
    result->aux_int = xi_tuple_pack_aux(2, 0);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;

    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "nested aggregate semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_struct_and_named_aggregate_semantic(bool unknown_call) {
    const char *struct_names[2] = {"count", "ready"};
    XrType *struct_fields[2] = {&stub_int, &stub_bool};
    XrType structural = {
        .kind = XR_KIND_STRUCT_OBJECT,
        .id = 103,
        .frozen = true,
        .is_value_type = true,
        .scalar_rep = XR_SCALAR_REP_NONE,
        .object =
            {
                .field_names = struct_names,
                .field_types = struct_fields,
                .field_count = 2,
            },
    };
    const char *dynamic_names[2] = {"count", "label"};
    XrType *dynamic_fields[2] = {&stub_int, &stub_string};
    XrType dynamic_structural = {
        .kind = XR_KIND_STRUCT_OBJECT,
        .id = 104,
        .frozen = true,
        .is_value_type = true,
        .scalar_rep = XR_SCALAR_REP_NONE,
        .object =
            {
                .field_names = dynamic_names,
                .field_types = dynamic_fields,
                .field_count = 2,
            },
    };
    const char *named_fields[2] = {"x", "flag"};
    XrType *named_field_types[2] = {&stub_int, &stub_bool};
    XrAggregateLayout native_layout = {
        .field_count = 2,
        .kind = XR_AGG_LAYOUT_STRUCT,
        .explicit_align = 16,
        .nominal_name = "AlignedPair",
        .field_names = named_fields,
    };
    XrClassInfo class_info = {
        .name = "AlignedPair",
        .struct_layout = &native_layout,
    };
    XrType named = {
        .kind = XR_KIND_INSTANCE,
        .id = 105,
        .frozen = true,
        .is_value_type = true,
        .scalar_rep = XR_SCALAR_REP_NONE,
    };
    named.instance.class_name = "AlignedPair";
    named.instance.class_ref = &class_info;
    XiClassData declaration = {
        .class_info = &class_info,
        .class_name = "AlignedPair",
        .instance_field_names = named_fields,
        .instance_field_types = named_field_types,
        .instance_field_count = 2,
        .needs_runtime_type = false,
        .struct_layout = &native_layout,
    };
    XrType class_object = {
        .kind = XR_KIND_UNKNOWN,
        .id = 105,
        .frozen = true,
        .scalar_rep = XR_SCALAR_REP_NONE,
    };

    XiFunc *function = xi_func_new("target_struct_aggregate_probe", &named);
    REQUIRE(function != NULL);
    XiModule *module = xi_module_new("pkg/target_struct_aggregate.xr",
                                     "target_struct_aggregate", function);
    REQUIRE(module != NULL);
    function->module = module;
    module->classes = (XiClassData **) xr_malloc(sizeof(*module->classes));
    REQUIRE(module->classes != NULL);
    module->classes[0] = &declaration;
    module->nclasses = 1;
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *class_declaration =
        xi_value_new(function, entry, XI_CLASS_CREATE, &class_object, 0);
    REQUIRE(class_declaration != NULL);
    class_declaration->aux = &declaration;
    XiValue *class_store = xi_value_new(function, entry, XI_SET_SHARED, &stub_unit, 1);
    XiValue *class_load = xi_value_new(function, entry, XI_GET_SHARED, &class_object, 0);
    REQUIRE(class_store != NULL && class_load != NULL);
    class_store->args[0] = class_declaration;
    class_store->aux_int = 0;
    class_load->aux_int = 0;
    function->nshared = 1;
    XiValue *structural_value = xi_value_new(function, entry, XI_OBJECT_NEW, &structural, 0);
    REQUIRE(structural_value != NULL);
    structural_value->aux = (void *) struct_names;
    structural_value->aux_int = xi_object_pack_aux(2, 0);
    XiValue *dynamic_value = xi_value_new(function, entry, XI_OBJECT_NEW, &dynamic_structural, 0);
    REQUIRE(dynamic_value != NULL);
    dynamic_value->aux = (void *) dynamic_names;
    dynamic_value->aux_int = xi_object_pack_aux(2, 0);
    XiValue *named_value = xi_value_new(function, entry, XI_AGG_NEW, &named, 1);
    REQUIRE(named_value != NULL);
    named_value->args[0] = class_load;
    named_value->aux = &native_layout;
    if (unknown_call) {
        XiValue *deferred_call = xi_value_new(function, entry, XI_CALL, &named, 1);
        REQUIRE(deferred_call != NULL);
        deferred_call->args[0] = class_load;
    }
    xi_block_set_return(entry, named_value);
    function->stage = XI_STAGE_OPTIMIZED;

    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "struct aggregate semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    function->module = NULL;
    xi_func_free(function);
    module->init = NULL;
    xi_module_free(module);
    return plan;
}

static bool freeze_single_scalar(XrSemanticPlan *semantic, XrTargetProfile *profile,
                                 bool bind_value, bool boolean_rep, XrTargetPlan **out, char *error,
                                 size_t error_size) {
    const XrSemanticFunctionRecord *semantic_function = xr_semantic_plan_function(semantic, 0);
    REQUIRE(semantic_function != NULL && semantic_function->value_count == 1);
    uint32_t semantic_type = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_type_count(semantic); i++) {
        const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, i);
        if ((boolean_rep && type->kind == XR_KIND_BOOL) ||
            (!boolean_rep && type->kind == XR_KIND_INT))
            semantic_type = i;
    }
    REQUIRE(semantic_type != XR_SEMANTIC_INDEX_NONE);
    XrTargetMachineRepRecord reps[2] = {
        {.id = 0, .kind = XR_MACHINE_REP_VOID},
        {
            .id = 1,
            .kind = boolean_rep ? XR_MACHINE_REP_I1 : XR_MACHINE_REP_I64,
            .register_bits = boolean_rep ? 1u : 64u,
            .memory_size = boolean_rep ? 1u : 8u,
            .memory_align = boolean_rep ? 1u : 8u,
            .signedness = boolean_rep ? XR_TARGET_SIGN_NONE : XR_TARGET_SIGN_SIGNED,
            .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
        },
    };
    XrTargetValueRepRecord value = {
        .semantic_value = semantic_function->value_begin,
        .register_rep = 1,
        .memory_rep = 1,
        .slot = 0,
    };
    XrTargetExtentRecord extent = {
        .id = 0,
        .kind = XR_TARGET_EXTENT_FIXED,
        .element_layout = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetLayoutRecord layout = {
        .id = 0,
        .semantic_type = semantic_type,
        .kind = XR_TARGET_LAYOUT_SCALAR,
        .align = boolean_rep ? 1u : 8u,
        .fixed_prefix_size = boolean_rep ? 1u : 8u,
        .extent = 0,
    };
    XrTargetFunctionRecord function = {
        .id = 0,
        .semantic_function = 0,
        .slot_count = bind_value ? 1u : 0u,
        .frame_size = bind_value ? (boolean_rep ? 1u : 8u) : 0u,
        .frame_align = bind_value ? (boolean_rep ? 1u : 8u) : 1u,
    };
    XrTargetSlotRecord slot = {
        .id = 0,
        .function = 0,
        .size = boolean_rep ? 1u : 8u,
        .align = boolean_rep ? 1u : 8u,
        .register_rep = 1,
        .memory_rep = 1,
        .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetCapabilityRecord capabilities[2];
    fill_foundation_capabilities(capabilities);
    if (bind_value)
        fill_slot_source(semantic, &slot, 0, semantic_function->value_begin);
    XrTargetPlanDraft draft = {
        .semantic_plan = semantic,
        .profile = profile,
        .completed_family_mask = XR_TARGET_REQUIRED_FAMILIES,
        .machine_reps = reps,
        .machine_reps_count = 2,
        .value_reps = bind_value ? &value : NULL,
        .value_reps_count = bind_value ? 1u : 0u,
        .extents = bind_value ? &extent : NULL,
        .extents_count = bind_value ? 1u : 0u,
        .layouts = bind_value ? &layout : NULL,
        .layouts_count = bind_value ? 1u : 0u,
        .functions = &function,
        .functions_count = 1,
        .slots = bind_value ? &slot : NULL,
        .slots_count = bind_value ? 1u : 0u,
        .capabilities = capabilities,
        .capabilities_count = 2,
    };
    return xr_target_plan_freeze(&draft, out, error, error_size);
}

static void fill_representation_fixture(TargetFixture *fixture, XrSemanticPlan *semantic,
                                        uint32_t *out_int_layout, uint32_t *out_string_layout) {
    uint32_t int_type = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_type_count(semantic); i++) {
        const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, i);
        if (type->kind == XR_KIND_INT)
            int_type = i;
    }
    REQUIRE(int_type != XR_SEMANTIC_INDEX_NONE);
    fixture->reps[0] = (XrTargetMachineRepRecord) {
        .id = 0,
        .kind = XR_MACHINE_REP_VOID,
    };
    fixture->reps[1] = (XrTargetMachineRepRecord) {
        .id = 1,
        .kind = XR_MACHINE_REP_I64,
        .register_bits = 64,
        .memory_size = 8,
        .memory_align = 8,
        .signedness = XR_TARGET_SIGN_SIGNED,
        .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
        .legal_conversion_mask = {UINT64_C(1) << 2},
    };
    fixture->reps[2] = (XrTargetMachineRepRecord) {
        .id = 2,
        .kind = XR_MACHINE_REP_I64,
        .register_bits = 64,
        .memory_size = 8,
        .memory_align = 8,
        .signedness = XR_TARGET_SIGN_SIGNED,
        .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
        .legal_conversion_mask = {UINT64_C(1) << 1},
    };
    fixture->value_reps[0] = (XrTargetValueRepRecord) {
        .semantic_value = 0,
        .register_rep = 1,
        .memory_rep = 1,
        .slot = 0,
    };
    fixture->value_reps[1] = (XrTargetValueRepRecord) {
        .semantic_value = 2,
        .register_rep = 0,
        .memory_rep = 0,
        .slot = XR_SEMANTIC_INDEX_NONE,
    };
    fixture->value_reps[2] = (XrTargetValueRepRecord) {
        .semantic_value = 3,
        .register_rep = 1,
        .memory_rep = 1,
        .slot = 1,
    };
    fixture->extents[0] = (XrTargetExtentRecord) {
        .id = 0,
        .kind = XR_TARGET_EXTENT_FIXED,
        .element_layout = XR_SEMANTIC_INDEX_NONE,
    };
    fixture->layouts[0] = (XrTargetLayoutRecord) {
        .id = 0,
        .semantic_type = int_type,
        .kind = XR_TARGET_LAYOUT_SCALAR,
        .align = 8,
        .fixed_prefix_size = 8,
        .extent = 0,
        .field_begin = 0,
    };
    *out_int_layout = 0;
    *out_string_layout = XR_SEMANTIC_INDEX_NONE;
}

static void fill_execution_fixture(TargetFixture *fixture, XrSemanticPlan *semantic,
                                   uint32_t int_layout, uint32_t string_layout) {
    (void) int_layout;
    (void) string_layout;
    fixture->functions[0] = (XrTargetFunctionRecord) {
        .id = 0,
        .semantic_function = 0,
        .slot_begin = 0,
        .slot_count = 2,
        .frame_size = 16,
        .frame_align = 8,
        .root_begin = 0,
        .root_count = 0,
        .cleanup_begin = 0,
        .cleanup_count = 0,
    };
    fixture->slots[0] = (XrTargetSlotRecord) {
        .id = 0,
        .function = 0,
        .offset = 0,
        .size = 8,
        .align = 8,
        .register_rep = 1,
        .memory_rep = 1,
        .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    fill_slot_source(semantic, &fixture->slots[0], 0, fixture->value_reps[0].semantic_value);
    fixture->slots[1] = (XrTargetSlotRecord) {
        .id = 1,
        .function = 0,
        .offset = 8,
        .size = 8,
        .align = 8,
        .register_rep = 1,
        .memory_rep = 1,
        .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    fill_slot_source(semantic, &fixture->slots[1], 0, fixture->value_reps[2].semantic_value);
    if (xr_stable_id_compare(fixture->slots[0].identity, fixture->slots[1].identity) > 0) {
        XrTargetSlotRecord temporary = fixture->slots[0];
        fixture->slots[0] = fixture->slots[1];
        fixture->slots[1] = temporary;
    }
    for (uint32_t i = 0; i < 2; i++) {
        fixture->slots[i].id = i;
        fixture->slots[i].offset = i * 8u;
        for (uint32_t value = 0; value < 3; value++)
            if (fixture->value_reps[value].semantic_value == fixture->slots[i].semantic_value)
                fixture->value_reps[value].slot = i;
    }
}

static void fill_draft(TargetFixture *fixture, XrSemanticPlan *semantic, XrTargetProfile *profile) {
    fixture->draft = (XrTargetPlanDraft) {
        .semantic_plan = semantic,
        .profile = profile,
        .completed_family_mask = XR_TARGET_REQUIRED_FAMILIES,
        .machine_reps = fixture->reps,
        .machine_reps_count = 3,
        .value_reps = fixture->value_reps,
        .value_reps_count = 3,
        .extents = fixture->extents,
        .extents_count = 1,
        .layouts = fixture->layouts,
        .layouts_count = 1,
        .functions = fixture->functions,
        .functions_count = 1,
        .slots = fixture->slots,
        .slots_count = 2,
        .capabilities = fixture->capabilities,
        .capabilities_count = 2,
    };
}

static void fill_fixture(TargetFixture *fixture, XrSemanticPlan *semantic,
                         XrTargetProfile *profile) {
    memset(fixture, 0, sizeof(*fixture));
    uint32_t int_layout;
    uint32_t string_layout;
    fill_representation_fixture(fixture, semantic, &int_layout, &string_layout);
    fill_execution_fixture(fixture, semantic, int_layout, string_layout);
    fill_foundation_capabilities(fixture->capabilities);
    fill_draft(fixture, semantic, profile);
}

static XrTargetPlan *build_target_plan(XrSemanticPlan *semantic, XrTargetProfile *profile) {
    TargetFixture fixture;
    fill_fixture(&fixture, semantic, profile);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool frozen = xr_target_plan_freeze(&fixture.draft, &plan, error, sizeof(error));
    if (!frozen)
        fprintf(stderr, "target fixture failed: %s\n", error);
    REQUIRE(frozen && plan != NULL);
    return plan;
}

static void expect_verify_failure_raw_at(XrTargetPlan *plan, const char *code,
                                         uint32_t mutation_line) {
    char error[512] = {0};
    bool verified = xr_target_plan_verify(plan, error, sizeof(error));
    if (verified)
        fprintf(stderr, "mutation at test_target_plan.c:%u unexpectedly verified\n", mutation_line);
    REQUIRE(!verified);
    if (strncmp(error, code, strlen(code)) != 0)
        fprintf(stderr, "expected verifier code %s, got: %s\n", code, error);
    REQUIRE(strncmp(error, code, strlen(code)) == 0);
}

static void expect_verify_failure_at(XrTargetPlan *plan, const char *code, uint32_t mutation_line) {
    XrFingerprint frozen_fingerprint = plan->fingerprint;
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    expect_verify_failure_raw_at(plan, code, mutation_line);
    plan->fingerprint = frozen_fingerprint;
}

#define expect_verify_failure(plan, code) expect_verify_failure_at((plan), (code), __LINE__)
#define expect_verify_failure_raw(plan, code) expect_verify_failure_raw_at((plan), (code), __LINE__)

static XrSemanticPlan *build_unit_enum_semantic(XrEnumLayout **out_layout) {
    static const char *members[] = {"Standard", "UrlSafe"};
    XrEnumLayout *layout =
        xr_enum_layout_new("stdlib/base64", "Base64Alphabet", members, 2);
    REQUIRE(layout != NULL && layout->is_zero_payload && layout->layout_id != 0);
    XrType enum_type = {
        .kind = XR_KIND_ENUM,
        .id = 18,
        .frozen = true,
        .scalar_rep = XR_SCALAR_REP_NONE,
        .enum_type = {
            .enum_name = "Base64Alphabet",
            .layout_id = layout->layout_id,
            .layout = layout,
        },
    };
    XiFunc *function = xi_func_new("target_source_enum_probe", &stub_int);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(function != NULL && entry != NULL);
    XiValue *ordinal = xi_param(function, entry, 0, &enum_type);
    XiValue *result = xi_const_int(function, entry, 0, &stub_int);
    REQUIRE(ordinal != NULL && result != NULL);
    function->nparams = function->min_params = 1;
    function->params = (XiValue **) xr_calloc(1, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = ordinal;
    function->arc_borrow_sig =
        (XiBorrowSig *) xi_func_arena_alloc(function,
                                            (uint32_t) sizeof(*function->arc_borrow_sig));
    REQUIRE(function->arc_borrow_sig != NULL);
    function->arc_borrow_sig->nparams = 1;
    function->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    function->arc_borrow_sig->valid = true;
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "target source-enum semantic failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(function);
    *out_layout = layout;
    return plan;
}

static void test_unit_enum_target_rep_mutations(void) {
    XrEnumLayout *source_layout = NULL;
    XrSemanticPlan *semantic = build_unit_enum_semantic(&source_layout);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    const XrSemanticTypeRecord *enum_type = NULL;
    for (uint32_t i = 0; i < semantic->type_count; i++)
        if (semantic->types[i].kind == XR_KIND_ENUM)
            enum_type = &semantic->types[i];
    REQUIRE(enum_type != NULL);
    const XrTargetValueRepRecord *binding = NULL;
    uint32_t binding_count = 0;
    XrTargetValueRepRecord *bindings =
        (XrTargetValueRepRecord *) xr_target_plan_value_reps(plan, &binding_count);
    for (uint32_t i = 0; i < binding_count; i++) {
        const XrTargetMachineRepRecord *rep =
            xr_target_plan_machine_rep(plan, bindings[i].register_rep);
        if (rep && rep->kind == XR_MACHINE_REP_ENUM_ORDINAL)
            binding = &bindings[i];
    }
    REQUIRE(binding != NULL);
    XrTargetMachineRepRecord *rep = &plan->machine_reps[binding->register_rep];
    REQUIRE(rep->kind == XR_MACHINE_REP_ENUM_ORDINAL &&
            rep->detail < semantic->type_count &&
            &semantic->types[rep->detail] == enum_type &&
            rep->root_kind == XR_TARGET_ROOT_NONE &&
            rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL);
    uint16_t saved_kind = rep->kind;
    uint32_t saved_detail = rep->detail;
    uint8_t saved_root = rep->root_kind;
    rep->kind = XR_MACHINE_REP_I64;
    expect_verify_failure(plan, "XR_TARGET_1001");
    rep->kind = saved_kind;
    rep->detail = XR_SEMANTIC_INDEX_NONE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    rep->detail = saved_detail;
    rep->root_kind = XR_TARGET_ROOT_OBJECT;
    expect_verify_failure(plan, "XR_TARGET_1001");
    rep->root_kind = saved_root;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    xr_enum_layout_free(source_layout);
}

static void test_profile_freeze_and_determinism(void) {
    XrTargetProfile *first = build_profile(0);
    XrTargetProfile *same = build_profile(0);
    XrTargetProfile *different = build_profile(XR_TARGET_ATOMIC_WIDTH_128);
    REQUIRE(xr_target_profile_is_frozen(first));
    REQUIRE(xr_fingerprint_equal(xr_target_profile_fingerprint(first),
                                 xr_target_profile_fingerprint(same)));
    REQUIRE(!xr_fingerprint_equal(xr_target_profile_fingerprint(first),
                                  xr_target_profile_fingerprint(different)));

    first->facts.machine.data_layout.pointer.size = 4;
    char error[512] = {0};
    REQUIRE(!xr_target_profile_verify(first, error, sizeof(error)));
    REQUIRE(strncmp(error, "XR_TARGET_1000", strlen("XR_TARGET_1000")) == 0);
    first->facts.machine.data_layout.pointer.size = 8;
    REQUIRE(xr_target_profile_verify(first, error, sizeof(error)));
    first->facts.machine.native_abi = XR_TARGET_ABI_COUNT;
    REQUIRE(!xr_target_profile_verify(first, error, sizeof(error)));
    first->facts.machine.native_abi = XR_TARGET_ABI_WIN64_X86_64;
    XrFingerprint saved_profile_fingerprint = first->fingerprint;
    first->facts.machine.maximum_vector_bits = 256;
    xr_target_profile_compute_fingerprint(&first->facts, &first->fingerprint);
    REQUIRE(!xr_target_profile_verify(first, error, sizeof(error)));
    first->facts.machine.maximum_vector_bits = 128;
    first->facts.machine.vector_feature_mask = XR_TARGET_VECTOR_AVX2;
    first->facts.machine.maximum_vector_bits = 256;
    xr_target_profile_compute_fingerprint(&first->facts, &first->fingerprint);
    REQUIRE(!xr_target_profile_verify(first, error, sizeof(error)));
    first->facts.machine.vector_feature_mask = XR_TARGET_VECTOR_SSE2;
    first->facts.machine.maximum_vector_bits = 128;
    first->facts.machine.operating_system = XR_TARGET_OS_FREESTANDING;
    first->facts.machine.environment = XR_TARGET_ENV_FREESTANDING;
    first->facts.machine.runtime_profile = XR_TARGET_RUNTIME_PROFILE_FREESTANDING;
    xr_target_profile_compute_fingerprint(&first->facts, &first->fingerprint);
    REQUIRE(!xr_target_profile_verify(first, error, sizeof(error)));
    first->facts.machine.operating_system = XR_TARGET_OS_WINDOWS;
    first->facts.machine.environment = XR_TARGET_ENV_MSVC;
    first->facts.machine.runtime_profile = XR_TARGET_RUNTIME_PROFILE_HOSTED;
    first->fingerprint = saved_profile_fingerprint;
    first->facts.provider_mask |= UINT64_C(1) << 63;
    xr_target_profile_compute_fingerprint(&first->facts, &first->fingerprint);
    REQUIRE(!xr_target_profile_verify(first, error, sizeof(error)));
    first->facts.provider_mask &= ~(UINT64_C(1) << 63);
    first->fingerprint = saved_profile_fingerprint;
    first->fingerprint.bytes[0] ^= 1;
    REQUIRE(!xr_target_profile_verify(first, error, sizeof(error)));
    first->fingerprint.bytes[0] ^= 1;

    xr_target_profile_free(first);
    xr_target_profile_free(same);
    xr_target_profile_free(different);
}

static void test_plan_snapshot_and_determinism(void) {
    XrSemanticPlan *semantic = build_semantic_plan();
    XrTargetProfile *profile = build_profile(0);
    TargetFixture fixture;
    fill_fixture(&fixture, semantic, profile);
    XrTargetPlan *first = NULL;
    XrTargetPlan *second = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_freeze(&fixture.draft, &first, error, sizeof(error)));
    REQUIRE(xr_target_plan_freeze(&fixture.draft, &second, error, sizeof(error)));
    REQUIRE(xr_target_plan_is_frozen(first));
    REQUIRE(xr_target_plan_is_verified(first));
    REQUIRE(xr_target_plan_schema_version(first) == XR_TARGET_PLAN_SCHEMA_VERSION);
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(first),
                                 xr_target_plan_fingerprint(second)));
    REQUIRE(xr_fingerprint_equal(xr_target_plan_semantic_fingerprint(first),
                                 xr_semantic_plan_fingerprint(semantic)));
    char target_hex[XR_FINGERPRINT_BYTES * 2 + 1];
    xr_fingerprint_hex(xr_target_plan_fingerprint(first), target_hex);
    REQUIRE(strcmp(target_hex,
                   "86d918840eb01ae5e43625024ff4a712739f2d5a680b73b5bd420b7339b16683") == 0);

    fixture.slots[0].offset = 64;
    uint32_t count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(first, &count);
    REQUIRE(count == 2 && slots[0].offset == 0);
    REQUIRE(xr_target_plan_machine_reps(first, &count) != NULL && count == 3);
    const XrTargetValueRepRecord *value_reps = xr_target_plan_value_reps(first, &count);
    REQUIRE(value_reps != NULL && count == 3 && value_reps[0].register_rep == 1);
    REQUIRE(xr_target_plan_value_rep(first, 0) == &value_reps[0]);
    REQUIRE(xr_target_plan_value_rep(first, 1) == NULL);
    REQUIRE(xr_target_plan_value_rep(first, 2) == &value_reps[1]);
    REQUIRE(xr_target_plan_value_rep(first, 3) == &value_reps[2]);
    REQUIRE(xr_target_plan_machine_rep(first, 2) != NULL);
    REQUIRE(xr_target_plan_machine_rep(first, 3) == NULL);
    REQUIRE(xr_target_plan_profile(first) == profile);
    REQUIRE(xr_target_plan_semantic_plan(first) == semantic);

    XrTargetCallArgumentRecord fingerprint_argument = {
        .register_rep = 1,
        .memory_rep = 1,
    };
    XrTargetCallRecord fingerprint_call = {
        .semantic_operation = 0,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .result_register_rep = 1,
        .result_memory_rep = 1,
        .argument_count = 1,
    };
    first->calls = &fingerprint_call;
    first->calls_count = 1;
    first->call_arguments = &fingerprint_argument;
    first->call_arguments_count = 1;
    XrFingerprint call_fingerprint;
    XrFingerprint changed_call_fingerprint;
    xr_target_call_compute_fingerprint(first, 0, &call_fingerprint);
    fingerprint_argument.memory_rep = 2;
    xr_target_call_compute_fingerprint(first, 0, &changed_call_fingerprint);
    REQUIRE(!xr_fingerprint_equal(call_fingerprint, changed_call_fingerprint));
    fingerprint_argument.memory_rep = 1;
    REQUIRE(xr_semantic_plan_operation_count(semantic) > 1);
    fingerprint_call.semantic_operation = 1;
    xr_target_call_compute_fingerprint(first, 0, &changed_call_fingerprint);
    REQUIRE(!xr_fingerprint_equal(call_fingerprint, changed_call_fingerprint));
    first->calls = NULL;
    first->calls_count = 0;
    first->call_arguments = NULL;
    first->call_arguments_count = 0;

    xr_target_plan_free(first);
    xr_target_plan_free(second);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_builder_materializes_canonical_scalar_intents(void) {
    XrSemanticPlan *semantic = build_exact_string_semantic_plan();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *first = NULL;
    XrTargetPlan *second = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &first, error, sizeof(error)));
    REQUIRE(xr_target_plan_build(semantic, profile, &second, error, sizeof(error)));
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(first),
                                 xr_target_plan_fingerprint(second)));
    REQUIRE(first->completed_family_mask == XR_TARGET_REQUIRED_FAMILIES);
    REQUIRE(first->functions_count == 1 && first->slots_count == 3);
    REQUIRE(first->instructions_count == 0);
    REQUIRE(first->functions[0].slot_begin == 0 && first->functions[0].slot_count == 3);
    REQUIRE(first->functions[0].frame_size == 32 && first->functions[0].frame_align == 8);
    REQUIRE(xr_stable_id_compare(first->slots[0].identity, first->slots[1].identity) < 0);
    REQUIRE(xr_stable_id_compare(first->slots[1].identity, first->slots[2].identity) < 0);
    for (uint32_t i = 0; i < first->slots_count; i++) {
        const XrTargetSlotRecord *slot = &first->slots[i];
        REQUIRE(slot->id == i && slot->function == 0);
        REQUIRE(slot->logical_slot == XR_SEMANTIC_INDEX_NONE);
        REQUIRE(slot->role == XR_TARGET_SLOT_TEMPORARY);
        REQUIRE(slot->semantic_operation == operation_for_value(semantic, slot->semantic_value));
    }
    REQUIRE(first->value_reps_count == 4);
    for (uint32_t i = 1; i < first->value_reps_count; i++)
        REQUIRE(first->value_reps[i - 1u].semantic_value < first->value_reps[i].semantic_value);
    const XrSemanticOperationRecord *string_operation = NULL;
    uint32_t string_operation_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        const XrSemanticConstantRecord *constant =
            operation && operation->constant < xr_semantic_plan_constant_count(semantic)
                ? xr_semantic_plan_constant(semantic, operation->constant)
                : NULL;
        if (operation && constant && constant->kind == XR_SEM_CONST_STRING) {
            REQUIRE(string_operation == NULL);
            string_operation = operation;
            string_operation_index = i;
        }
    }
    REQUIRE(string_operation != NULL &&
            string_operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
            string_operation->return_complete == 1 && string_operation->allocation_key == NULL);
    const XrTargetValueRepRecord *string_binding =
        xr_target_plan_value_rep(first, string_operation->result_value);
    REQUIRE(string_binding != NULL && string_binding->slot < first->slots_count);
    const XrTargetMachineRepRecord *string_rep = &first->machine_reps[string_binding->memory_rep];
    REQUIRE(string_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
            string_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
            string_rep->ownership == XR_TARGET_OWNERSHIP_OWNED &&
            string_rep->null_encoding == XR_TARGET_NULL_TAGGED);
    REQUIRE(first->slots[string_binding->slot].semantic_operation == string_operation_index);
    uint32_t string_layout_count = 0;
    for (uint32_t i = 0; i < first->layouts_count; i++)
        string_layout_count += first->layouts[i].semantic_type == string_operation->result_type &&
                               first->layouts[i].kind == XR_TARGET_LAYOUT_DYNAMIC;
    REQUIRE(string_layout_count == 1);
    REQUIRE(first->allocations_count == 0 && first->root_maps_count == 0 &&
            first->root_slots_count == 0 && first->cleanups_count == 0);

    uint32_t string_binding_index = (uint32_t) (string_binding - first->value_reps);
    XrTargetValueRepRecord saved_string_rows[4];
    memcpy(saved_string_rows, first->value_reps, sizeof(saved_string_rows));
    memmove(&first->value_reps[string_binding_index], &first->value_reps[string_binding_index + 1u],
            (first->value_reps_count - string_binding_index - 1u) * sizeof(*first->value_reps));
    first->value_reps_count--;
    expect_verify_failure(first, "XR_TARGET_1001");
    first->value_reps_count++;
    memcpy(first->value_reps, saved_string_rows, sizeof(saved_string_rows));

    XrTargetValueRepRecord *original_rows = first->value_reps;
    XrTargetValueRepRecord *extra_rows =
        (XrTargetValueRepRecord *) xr_malloc(5u * sizeof(*extra_rows));
    REQUIRE(extra_rows != NULL);
    memcpy(extra_rows, original_rows, first->value_reps_count * sizeof(*extra_rows));
    extra_rows[4] = saved_string_rows[string_binding_index];
    first->value_reps = extra_rows;
    first->value_reps_count++;
    expect_verify_failure(first, "XR_TARGET_1001");
    first->value_reps_count--;
    first->value_reps = original_rows;
    xr_free(extra_rows);

    uint64_t saved_family_mask = first->completed_family_mask;
    first->completed_family_mask &= ~XR_TARGET_FAMILY_STRING_LITERAL_STORAGE;
    expect_verify_failure(first, "XR_TARGET_1001");
    first->completed_family_mask = saved_family_mask;
    uint32_t saved_string_operation = first->slots[string_binding->slot].semantic_operation;
    first->slots[string_binding->slot].semantic_operation = saved_string_operation + 1u;
    expect_verify_failure(first, "XR_TARGET_1001");
    first->slots[string_binding->slot].semantic_operation = saved_string_operation;
    REQUIRE(xr_target_plan_verify(first, error, sizeof(error)));
    uint32_t release_operation = XR_SEMANTIC_INDEX_NONE;
    uint32_t release_value = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (operation && operation->opcode == XI_RELEASE) {
            release_operation = i;
            release_value = operation->result_value;
        }
    }
    REQUIRE(release_operation != XR_SEMANTIC_INDEX_NONE && release_value != XR_SEMANTIC_INDEX_NONE);
    uint32_t release_binding = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < first->value_reps_count; i++)
        if (first->value_reps[i].semantic_value == release_value)
            release_binding = i;
    REQUIRE(release_binding != XR_SEMANTIC_INDEX_NONE);
    REQUIRE(first->machine_reps[first->value_reps[release_binding].register_rep].kind ==
            XR_MACHINE_REP_VOID);
    REQUIRE(first->machine_reps[first->value_reps[release_binding].memory_rep].kind ==
            XR_MACHINE_REP_VOID);
    REQUIRE(first->value_reps[release_binding].slot == XR_SEMANTIC_INDEX_NONE);

    XrTargetValueRepRecord saved_release = first->value_reps[release_binding];
    first->value_reps[release_binding].register_rep = 1;
    first->value_reps[release_binding].memory_rep = 1;
    expect_verify_failure(first, "XR_TARGET_1001");
    first->value_reps[release_binding] = saved_release;
    first->value_reps[release_binding].slot = 0;
    expect_verify_failure(first, "XR_TARGET_1001");
    first->value_reps[release_binding] = saved_release;
    XrTargetValueRepRecord saved_values[4];
    memcpy(saved_values, first->value_reps, sizeof(saved_values));
    for (uint32_t i = release_binding + 1; i < first->value_reps_count; i++)
        first->value_reps[i - 1] = first->value_reps[i];
    first->value_reps_count--;
    expect_verify_failure(first, "XR_TARGET_1001");
    memcpy(first->value_reps, saved_values, sizeof(saved_values));
    first->value_reps_count = 4;

    first->slots[0].identity.bytes[0] ^= 1;
    expect_verify_failure(first, "XR_TARGET_1001");
    first->slots[0].identity.bytes[0] ^= 1;
    first->slots[0].role = XR_TARGET_SLOT_ROLE_INVALID;
    expect_verify_failure(first, "XR_TARGET_1001");
    first->slots[0].role = XR_TARGET_SLOT_TEMPORARY;
    first->slots[0].logical_slot = 0;
    expect_verify_failure(first, "XR_TARGET_1001");
    first->slots[0].logical_slot = XR_SEMANTIC_INDEX_NONE;
    first->functions[0].frame_size += first->functions[0].frame_align;
    expect_verify_failure(first, "XR_TARGET_1002");
    first->functions[0].frame_size -= first->functions[0].frame_align;
    REQUIRE(xr_target_plan_verify(first, NULL, 0));

    xr_target_plan_free(first);
    xr_target_plan_free(second);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_builder_materializes_parameter_without_operation(void) {
    XrSemanticPlan *semantic = build_parameter_without_operation_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    REQUIRE(plan != NULL && plan->slots_count == 1 && plan->value_reps_count == 1);
    REQUIRE(plan->slots[0].role == XR_TARGET_SLOT_PARAMETER);
    REQUIRE(plan->slots[0].semantic_operation == XR_SEMANTIC_INDEX_NONE);
    REQUIRE(plan->slots[0].semantic_value == xr_semantic_plan_parameter(semantic, 0)->value);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    plan->slots[0].semantic_operation = 0;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[0].semantic_operation = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_builder_materializes_effect_void_independent_of_type(void) {
    XrSemanticPlan *semantic = build_scalar_and_effect_void_same_type_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    uint32_t release_value = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (operation && operation->opcode == XI_RELEASE)
            release_value = operation->result_value;
    }
    REQUIRE(release_value != XR_SEMANTIC_INDEX_NONE);
    const XrTargetValueRepRecord *binding = xr_target_plan_value_rep(plan, release_value);
    REQUIRE(binding != NULL && binding->slot == XR_SEMANTIC_INDEX_NONE);
    REQUIRE(plan->machine_reps[binding->register_rep].kind == XR_MACHINE_REP_VOID);
    REQUIRE(plan->machine_reps[binding->memory_rep].kind == XR_MACHINE_REP_VOID);
    REQUIRE(plan->slots_count == 2);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_builder_materializes_exact_heap_closure_storage(void) {
    XrSemanticPlan *semantic = build_heap_closure_semantic(false);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));

    uint32_t closure_operation = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *closure = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (operation && operation->opcode == XI_CLOSURE_NEW) {
            REQUIRE(closure == NULL);
            closure = operation;
            closure_operation = i;
        }
    }
    REQUIRE(closure != NULL && closure->operand_count == 0 &&
            closure->callable_function != XR_SEMANTIC_INDEX_NONE);
    const XrSemanticFunctionRecord *exact_callee =
        xr_semantic_plan_function(semantic, closure->callable_function);
    const XrSemanticTypeRecord *exact_type = xr_semantic_plan_type(semantic, closure->result_type);
    uint32_t exact_child_count = 0;
    const uint32_t *exact_children = xr_semantic_plan_type_children(semantic, &exact_child_count);
    REQUIRE(closure->opcode == XI_CLOSURE_NEW && closure->allocation_key &&
            closure->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED);
    REQUIRE(exact_callee && exact_type && exact_callee->parent == closure->function &&
            exact_callee->capture_count == 0 && exact_type->kind == XR_KIND_FUNCTION &&
            exact_type->scalar_rep == XR_SCALAR_REP_NONE && exact_type->aggregate_extent == 0 &&
            exact_type->aggregate_align == 0);
    REQUIRE((exact_type->flags & (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE |
                                  XR_SEM_TYPE_BORROW_VIEW | XR_SEM_TYPE_AGGREGATE_EXACT)) == 0);
    REQUIRE((exact_type->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT)) ==
            (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT));
    REQUIRE(exact_type->child_count == (uint32_t) exact_callee->parameter_count + 1u &&
            exact_type->child_begin <= exact_child_count &&
            exact_type->child_count <= exact_child_count - exact_type->child_begin &&
            exact_callee->parameter_begin <= xr_semantic_plan_parameter_count(semantic) &&
            exact_callee->parameter_count <=
                xr_semantic_plan_parameter_count(semantic) - exact_callee->parameter_begin &&
            exact_children[exact_type->child_begin + exact_callee->parameter_count] ==
                exact_callee->return_type);
    const XrTargetValueRepRecord *binding = xr_target_plan_value_rep(plan, closure->result_value);
    REQUIRE(binding != NULL && binding->slot < plan->slots_count);
    XrTargetMachineRepRecord *dynamic = &plan->machine_reps[binding->memory_rep];
    REQUIRE(plan->machine_reps[binding->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            dynamic->kind == XR_MACHINE_REP_DYN_VALUE &&
            dynamic->root_kind == XR_TARGET_ROOT_DYNAMIC &&
            dynamic->ownership == XR_TARGET_OWNERSHIP_OWNED &&
            dynamic->null_encoding == XR_TARGET_NULL_TAGGED);
    const XrTargetMachineFacts *facts = xr_target_profile_machine_facts(profile);
    REQUIRE(facts != NULL && dynamic->memory_size == facts->data_layout.xr_value.size &&
            dynamic->memory_align == facts->data_layout.xr_value.align &&
            dynamic->register_bits == facts->data_layout.xr_value.size * 8u);

    XrTargetLayoutRecord *closure_layout = NULL;
    for (uint32_t i = 0; i < plan->layouts_count; i++) {
        if (plan->layouts[i].semantic_type == closure->result_type) {
            REQUIRE(closure_layout == NULL);
            closure_layout = &plan->layouts[i];
        }
    }
    REQUIRE(closure_layout != NULL && closure_layout->kind == XR_TARGET_LAYOUT_DYNAMIC &&
            closure_layout->field_count == 0 && closure_layout->root_field_count == 0 &&
            closure_layout->fixed_prefix_size == dynamic->memory_size &&
            closure_layout->align == dynamic->memory_align);
    XrTargetSlotRecord *slot = &plan->slots[binding->slot];
    REQUIRE(slot->semantic_value == closure->result_value &&
            slot->semantic_operation == closure_operation && slot->function == closure->function &&
            slot->role == XR_TARGET_SLOT_TEMPORARY && slot->root_kind == XR_TARGET_ROOT_DYNAMIC &&
            slot->ownership == XR_TARGET_OWNERSHIP_OWNED);

    /* This family freezes the outer slot representation only. Semantic
     * ownership and the existing AOT closure path still own lifetime. */
    REQUIRE(plan->allocations_count == 0 && plan->root_maps_count == 0 &&
            plan->root_slots_count == 0 && plan->cleanups_count == 0);
    REQUIRE(plan->functions[closure->function].root_count == 0 &&
            plan->functions[closure->function].cleanup_count == 0);

    uint64_t saved_families = plan->completed_family_mask;
    plan->completed_family_mask &= ~XR_TARGET_FAMILY_CLOSURE_STORAGE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->completed_family_mask = saved_families;

    uint8_t saved_ownership = dynamic->ownership;
    dynamic->ownership = XR_TARGET_OWNERSHIP_TRIVIAL;
    expect_verify_failure(plan, "XR_TARGET_1001");
    dynamic->ownership = saved_ownership;

    uint8_t saved_root = dynamic->root_kind;
    dynamic->root_kind = XR_TARGET_ROOT_NONE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    dynamic->root_kind = saved_root;

    uint8_t saved_null = dynamic->null_encoding;
    dynamic->null_encoding = XR_TARGET_NULL_NOT_NULLABLE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    dynamic->null_encoding = saved_null;

    uint32_t saved_operation = slot->semantic_operation;
    slot->semantic_operation = saved_operation + 1u;
    expect_verify_failure(plan, "XR_TARGET_1001");
    slot->semantic_operation = saved_operation;

    uint8_t saved_layout_kind = closure_layout->kind;
    closure_layout->kind = XR_TARGET_LAYOUT_SCALAR;
    expect_verify_failure(plan, "XR_TARGET_1001");
    closure_layout->kind = saved_layout_kind;

    XrSemanticOperationRecord *mutable_closure = &semantic->operations[closure_operation];
    const char *saved_allocation_key = mutable_closure->allocation_key;
    XrStableId saved_allocation_id = mutable_closure->allocation_id;
    const char forged_allocation_key[] = "forged-closure-operation/allocation";
    XrFingerprint allocation_digest;
    REQUIRE(xr_stable_id_from_key(forged_allocation_key, &mutable_closure->allocation_id,
                                  &allocation_digest));
    mutable_closure->allocation_key = forged_allocation_key;
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    semantic->ownership->semantic_fingerprint = semantic->fingerprint;
    semantic->ownership->fingerprint = semantic->fingerprint;
    plan->semantic_fingerprint = semantic->fingerprint;
    for (uint32_t i = 0; i < plan->layouts_count; i++)
        xr_target_layout_compute_fingerprint(plan, i, &plan->layouts[i].fingerprint);
    for (uint32_t i = 0; i < plan->calls_count; i++)
        xr_target_call_compute_fingerprint(plan, i, &plan->calls[i].fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    expect_verify_failure_raw(plan, "XR_TARGET_1001");

    mutable_closure->allocation_key = saved_allocation_key;
    mutable_closure->allocation_id = saved_allocation_id;
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    semantic->ownership->semantic_fingerprint = semantic->fingerprint;
    semantic->ownership->fingerprint = semantic->fingerprint;
    plan->semantic_fingerprint = semantic->fingerprint;
    for (uint32_t i = 0; i < plan->layouts_count; i++)
        xr_target_layout_compute_fingerprint(plan, i, &plan->layouts[i].fingerprint);
    for (uint32_t i = 0; i < plan->calls_count; i++)
        xr_target_call_compute_fingerprint(plan, i, &plan->calls[i].fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);

    semantic = build_heap_closure_semantic(true);
    profile = build_profile(0);
    plan = NULL;
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->opcode != XI_CLOSURE_NEW)
            continue;
        REQUIRE(operation->operand_count == 1);
        REQUIRE(xr_target_plan_value_rep(plan, operation->result_value) == NULL);
        for (uint32_t l = 0; l < plan->layouts_count; l++)
            REQUIRE(plan->layouts[l].semantic_type != operation->result_type);
    }
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_builder_materializes_nested_aggregate_family(void) {
    XrSemanticPlan *semantic = build_nested_aggregate_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *first = NULL;
    XrTargetPlan *second = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &first, error, sizeof(error)));
    REQUIRE(xr_target_plan_build(semantic, profile, &second, error, sizeof(error)));
    REQUIRE(first->completed_family_mask == XR_TARGET_REQUIRED_FAMILIES);
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(first),
                                 xr_target_plan_fingerprint(second)));

    uint32_t fixed_layout = XR_SEMANTIC_INDEX_NONE;
    uint32_t tuple_layout = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < first->layouts_count; i++) {
        const XrSemanticTypeRecord *type =
            xr_semantic_plan_type(semantic, first->layouts[i].semantic_type);
        REQUIRE(type != NULL);
        if (type->kind == XR_KIND_FIXED_ARRAY)
            fixed_layout = i;
        else if (type->kind == XR_KIND_TUPLE)
            tuple_layout = i;
    }
    REQUIRE(fixed_layout != XR_SEMANTIC_INDEX_NONE && tuple_layout != XR_SEMANTIC_INDEX_NONE);
    XrTargetLayoutRecord *fixed = &first->layouts[fixed_layout];
    XrTargetLayoutRecord *tuple = &first->layouts[tuple_layout];
    REQUIRE(fixed->kind == XR_TARGET_LAYOUT_AGGREGATE && fixed->field_count == 3 &&
            fixed->fixed_prefix_size == 24 && fixed->align == 8);
    for (uint32_t i = 0; i < fixed->field_count; i++) {
        const XrTargetFieldRecord *field = &first->fields[fixed->field_begin + i];
        REQUIRE(field->semantic_field == i && field->offset == i * 8u && field->size == 8 &&
                field->align == 8);
    }
    REQUIRE(tuple->kind == XR_TARGET_LAYOUT_AGGREGATE && tuple->field_count == 2 &&
            tuple->fixed_prefix_size == 32 && tuple->align == 8);
    XrTargetFieldRecord *tuple_first = &first->fields[tuple->field_begin];
    XrTargetFieldRecord *tuple_nested = &first->fields[tuple->field_begin + 1u];
    REQUIRE(tuple_first->offset == 0 && tuple_first->size == 1 && tuple_first->align == 1);
    REQUIRE(tuple_nested->offset == 8 && tuple_nested->size == 24 && tuple_nested->align == 8);
    REQUIRE(first->machine_reps[tuple_nested->memory_rep].kind == XR_MACHINE_REP_AGGREGATE);
    REQUIRE(first->machine_reps[tuple_nested->memory_rep].detail == fixed_layout);

    uint32_t aggregate_bindings = 0;
    for (uint32_t i = 0; i < first->value_reps_count; i++) {
        const XrTargetValueRepRecord *value = &first->value_reps[i];
        const XrTargetMachineRepRecord *rep = &first->machine_reps[value->memory_rep];
        if (rep->kind != XR_MACHINE_REP_AGGREGATE)
            continue;
        aggregate_bindings++;
        REQUIRE(value->slot < first->slots_count);
        const XrTargetSlotRecord *slot = &first->slots[value->slot];
        REQUIRE(slot->semantic_value == value->semantic_value &&
                slot->role == XR_TARGET_SLOT_TEMPORARY &&
                slot->size == first->layouts[rep->detail].fixed_prefix_size &&
                slot->align == first->layouts[rep->detail].align);
    }
    REQUIRE(aggregate_bindings == 2);

    XrFingerprint saved_tuple_fingerprint = tuple->fingerprint;
    uint32_t saved_offset = tuple_nested->offset;
    tuple_nested->offset = tuple_first->offset;
    xr_target_layout_compute_fingerprint(first, tuple_layout, &tuple->fingerprint);
    expect_verify_failure(first, "XR_TARGET_1002");
    tuple_nested->offset = saved_offset;
    tuple->fingerprint = saved_tuple_fingerprint;

    uint16_t saved_rep = tuple_nested->memory_rep;
    tuple_nested->memory_rep = tuple_first->memory_rep;
    xr_target_layout_compute_fingerprint(first, tuple_layout, &tuple->fingerprint);
    expect_verify_failure(first, "XR_TARGET_1002");
    tuple_nested->memory_rep = saved_rep;
    tuple->fingerprint = saved_tuple_fingerprint;

    uint16_t saved_field_count = tuple->field_count;
    tuple->field_count--;
    expect_verify_failure(first, "XR_TARGET_1002");
    tuple->field_count = saved_field_count + 1u;
    expect_verify_failure(first, "XR_TARGET_1002");
    tuple->field_count = saved_field_count;

    tuple_nested->offset = UINT32_MAX;
    xr_target_layout_compute_fingerprint(first, tuple_layout, &tuple->fingerprint);
    expect_verify_failure(first, "XR_TARGET_1002");
    tuple_nested->offset = saved_offset;
    tuple->fingerprint = saved_tuple_fingerprint;

    tuple_nested->offset++;
    expect_verify_failure(first, "XR_TARGET_1002");
    tuple_nested->offset = saved_offset;
    REQUIRE(xr_target_plan_verify(first, error, sizeof(error)));

    xr_target_plan_free(second);
    xr_target_plan_free(first);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_builder_materializes_struct_and_named_aggregates(void) {
    XrSemanticPlan *semantic = build_struct_and_named_aggregate_semantic(false);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error,
                                      sizeof(error));
    if (!built)
        fprintf(stderr, "named aggregate TargetPlan fixture failed: %s\n", error);
    REQUIRE(built);

    uint32_t structural_type = XR_SEMANTIC_INDEX_NONE;
    uint32_t dynamic_type = XR_SEMANTIC_INDEX_NONE;
    uint32_t named_type = XR_SEMANTIC_INDEX_NONE;
    uint32_t child_table_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_table_count);
    for (uint32_t i = 0; i < xr_semantic_plan_type_count(semantic); i++) {
        const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, i);
        REQUIRE(type != NULL);
        if (type->kind == XR_KIND_INSTANCE && (type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) != 0) {
            named_type = i;
            REQUIRE(type->aggregate_extent == 2 && type->aggregate_align == 16 &&
                    type->child_count == 2);
        } else if (type->kind == XR_KIND_STRUCT_OBJECT) {
            REQUIRE(type->child_begin <= child_table_count && type->child_count == 2 &&
                    type->child_count <= child_table_count - type->child_begin &&
                    type->aggregate_extent == 2);
            const XrSemanticTypeRecord *second =
                xr_semantic_plan_type(semantic, children[type->child_begin + 1u]);
            REQUIRE(second != NULL);
            if (second->kind == XR_KIND_BOOL)
                structural_type = i;
            else if (second->kind == XR_KIND_STRING)
                dynamic_type = i;
        }
    }
    REQUIRE(structural_type != XR_SEMANTIC_INDEX_NONE && dynamic_type != XR_SEMANTIC_INDEX_NONE &&
            named_type != XR_SEMANTIC_INDEX_NONE);

    const XrTargetLayoutRecord *structural_layout = NULL;
    const XrTargetLayoutRecord *named_layout = NULL;
    uint32_t named_layout_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->layouts_count; i++) {
        const XrTargetLayoutRecord *layout = &plan->layouts[i];
        REQUIRE(layout->semantic_type != dynamic_type);
        if (layout->semantic_type == structural_type)
            structural_layout = layout;
        else if (layout->semantic_type == named_type) {
            named_layout = layout;
            named_layout_index = i;
        }
    }
    REQUIRE(structural_layout != NULL && structural_layout->kind == XR_TARGET_LAYOUT_AGGREGATE &&
            structural_layout->field_count == 2 && structural_layout->fixed_prefix_size == 16 &&
            structural_layout->align == 8);
    REQUIRE(named_layout != NULL && named_layout->kind == XR_TARGET_LAYOUT_AGGREGATE &&
            named_layout->field_count == 2 && named_layout->fixed_prefix_size == 16 &&
            named_layout->align == 16);
    const XrTargetValueRepRecord *named_binding = NULL;
    for (uint32_t i = 0; i < plan->value_reps_count; i++) {
        const XrTargetMachineRepRecord *rep =
            &plan->machine_reps[plan->value_reps[i].memory_rep];
        if (rep->kind == XR_MACHINE_REP_AGGREGATE &&
            rep->detail == named_layout_index) {
            REQUIRE(named_binding == NULL);
            named_binding = &plan->value_reps[i];
        }
    }
    REQUIRE(named_binding != NULL);
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(plan, profile_fingerprint, &emission,
                                     error, sizeof(error)));
    XrCValueEmissionView aggregate_view = {0};
    REQUIRE(xr_c_emission_plan_value_view(
                emission, named_binding->semantic_value, &aggregate_view,
                error, sizeof(error)) &&
            aggregate_view.rep == XR_C_VALUE_REP_AGGREGATE &&
            aggregate_view.target_memory_kind == XR_MACHINE_REP_AGGREGATE &&
            aggregate_view.c_type &&
            strncmp(aggregate_view.c_type, "xrt_struct_abi_", 15) == 0);
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    XrTargetFieldRecord *named_first =
        &plan->fields[named_layout->field_begin];
    XrTargetFieldRecord *named_second =
        &plan->fields[named_layout->field_begin + 1u];
    REQUIRE(metadata && named_first->semantic_name < metadata_count &&
            named_second->semantic_name < metadata_count &&
            strcmp(metadata[named_first->semantic_name], "x") == 0 &&
            strcmp(metadata[named_second->semantic_name], "flag") == 0);

    XrFingerprint saved_named_fingerprint = named_layout->fingerprint;
    uint32_t saved_name = named_first->semantic_name;
    named_first->semantic_name = named_second->semantic_name;
    xr_target_layout_compute_fingerprint(plan, named_layout_index,
                                         &plan->layouts[named_layout_index].fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1002");
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, profile_fingerprint,
                                        error, sizeof(error)));
    named_first->semantic_name = saved_name;
    plan->layouts[named_layout_index].fingerprint = saved_named_fingerprint;
    REQUIRE(xr_c_emission_plan_verify(emission, plan, profile_fingerprint,
                                      error, sizeof(error)));

    uint32_t supported_bindings = 0;
    bool dynamic_binding = false;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->result_value == XR_SEMANTIC_INDEX_NONE)
            continue;
        const XrTargetValueRepRecord *binding =
            xr_target_plan_value_rep(plan, operation->result_value);
        if (operation->result_type == dynamic_type)
            dynamic_binding = binding != NULL;
        else if (operation->result_type == structural_type ||
                 operation->result_type == named_type) {
            REQUIRE(binding != NULL && binding->memory_rep < plan->machine_reps_count &&
                    plan->machine_reps[binding->memory_rep].kind == XR_MACHINE_REP_AGGREGATE);
            supported_bindings++;
        }
    }
    REQUIRE(supported_bindings == 2 && !dynamic_binding);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static XrSemanticPlan *build_direct_local_scalar_calls(uint16_t call_opcode,
                                                       XrType *value_type,
                                                       XrType *callable_type) {
    XiFunc *root = xi_func_new("target_direct_call_root", value_type);
    XiFunc *child = xi_func_new("target_direct_call_child", value_type);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);
    child->nparams = child->min_params = 1;
    child->params = (XiValue **) xr_calloc(1, sizeof(*child->params));
    REQUIRE(child->params != NULL);
    child->params[0] = xi_param(child, child_entry, 0, value_type);
    REQUIRE(child->params[0] != NULL);
    xi_block_set_return(child_entry, child->params[0]);

    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;

    XiValue *closure = xi_value_new(root, root_entry,
                                    call_opcode == XI_TAIL_CALL ? XI_CLOSURE_NEW : XI_STACK_ALLOC,
                                    callable_type, 0);
    REQUIRE(closure != NULL);
    if (call_opcode != XI_TAIL_CALL)
        closure->aux_int = XI_CLOSURE_NEW;
    closure->aux = child;
    XiValue *alias = xi_value_new(root, root_entry, XI_COPY, callable_type, 1);
    REQUIRE(alias != NULL);
    alias->args[0] = closure;
    alias->aux_int = XI_COPY_KIND_IDENTITY;
    XiValue *argument = xi_const_int(root, root_entry, 41, value_type);
    REQUIRE(argument != NULL);
    XiValue *first = xi_value_new(root, root_entry, call_opcode, value_type, 2);
    XiValue *second =
        call_opcode == XI_CALL ? xi_value_new(root, root_entry, call_opcode, value_type, 2) : NULL;
    REQUIRE(first != NULL && (call_opcode != XI_CALL || second != NULL));
    first->args[0] = alias;
    first->args[1] = argument;
    if (second) {
        second->args[0] = alias;
        second->args[1] = argument;
    }
    xi_block_set_return(root_entry, second ? second : first);
    root->stage = child->stage = XI_STAGE_OPTIMIZED;

    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "direct-local target fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    REQUIRE(xr_semantic_plan_call_target_count(plan) == (call_opcode == XI_CALL ? 2u : 1u));
    xi_func_free(root);
    return plan;
}

static XrSemanticPlan *build_channel_method_semantic(const char *selector,
                                                     int64_t selector_immediate,
                                                     XrType *receiver_type, XrType *result_type,
                                                     bool extra_argument) {
    XiFunc *function = xi_func_new("target_channel_close", &stub_unit);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *capacity = xi_const_int(function, entry, 1, &stub_int);
    XiValue *channel = receiver_type == &stub_channel
                           ? xi_value_new(function, entry, XI_CHAN_NEW, &stub_channel, 1)
                           : xi_const_int(function, entry, 2, &stub_int);
    XiValue *alias = xi_value_new(function, entry, XI_COPY, receiver_type, 1);
    XiValue *close =
        xi_value_new(function, entry, XI_CALL_METHOD, result_type, extra_argument ? 2 : 1);
    REQUIRE(capacity && channel && alias && close);
    if (receiver_type == &stub_channel)
        channel->args[0] = capacity;
    alias->args[0] = channel;
    alias->aux_int = XI_COPY_KIND_IDENTITY;
    close->args[0] = alias;
    close->aux = (void *) selector;
    close->aux_int = selector_immediate;
    if (extra_argument)
        close->args[1] = capacity;
    xi_block_set_return(entry, NULL);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "channel-close semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_channel_close_semantic(void) {
    return build_channel_method_semantic("close", 314, &stub_channel, &stub_unit, false);
}

static XrSemanticPlan *build_channel_receive_semantic(XrType *channel_type, XrType *result_type,
                                                      bool receiver_is_parameter) {
    XiFunc *function = xi_func_new("target_channel_receive", &stub_unit);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *receiver = NULL;
    if (receiver_is_parameter) {
        function->nparams = 1;
        function->min_params = 1;
        function->params = (XiValue **) xr_calloc(1, sizeof(*function->params));
        REQUIRE(function->params != NULL);
        receiver = xi_param(function, entry, 0, channel_type);
        function->params[0] = receiver;
    } else {
        XiValue *capacity = xi_const_int(function, entry, 1, &stub_int);
        receiver = xi_value_new(function, entry, XI_CHAN_NEW, channel_type, 1);
        REQUIRE(capacity && receiver);
        receiver->args[0] = capacity;
    }
    XiValue *receive = xi_value_new(function, entry, XI_CHAN_TRY_RECV, result_type, 1);
    REQUIRE(receiver && receive);
    receive->args[0] = receiver;
    xi_block_set_return(entry, NULL);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "channel-receive semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(function);
    return plan;
}

static void test_channel_receive_storage_authority(void) {
    XrTargetProfile *profile = build_profile(0);
    XrSemanticPlan *semantic = build_channel_receive_semantic(&stub_channel, &stub_int, false);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    REQUIRE(plan != NULL &&
            (plan->completed_family_mask & XR_TARGET_FAMILY_CHANNEL_RECEIVE_STORAGE) != 0);
    const XrSemanticOperationRecord *receive = NULL;
    uint32_t receive_operation = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < semantic->operation_count; i++)
        if (semantic->operations[i].opcode == XI_CHAN_TRY_RECV) {
            receive = &semantic->operations[i];
            receive_operation = i;
        }
    REQUIRE(receive != NULL);
    const XrTargetValueRepRecord *binding = xr_target_plan_value_rep(plan, receive->result_value);
    REQUIRE(binding != NULL && binding->slot < plan->slots_count);
    REQUIRE(plan->machine_reps[binding->register_rep].kind == XR_MACHINE_REP_I64 &&
            plan->machine_reps[binding->memory_rep].kind == XR_MACHINE_REP_I64 &&
            plan->slots[binding->slot].semantic_operation == receive_operation &&
            plan->slots[binding->slot].root_kind == XR_TARGET_ROOT_NONE &&
            plan->slots[binding->slot].ownership == XR_TARGET_OWNERSHIP_TRIVIAL);
    uint64_t saved_families = plan->completed_family_mask;
    plan->completed_family_mask &= ~XR_TARGET_FAMILY_CHANNEL_RECEIVE_STORAGE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->completed_family_mask = saved_families;
    XrTargetMachineRepRecord saved_rep = plan->machine_reps[binding->register_rep];
    plan->machine_reps[binding->register_rep].kind = XR_MACHINE_REP_DYN_VALUE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[binding->register_rep] = saved_rep;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);

    semantic = build_channel_receive_semantic(&stub_channel, &stub_int, true);
    plan = NULL;
    REQUIRE(!xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL && strncmp(error, "XR_TARGET_1001", 14) == 0);
    xr_semantic_plan_free(semantic);

    semantic = build_channel_receive_semantic(&stub_float_channel, &stub_int, false);
    plan = NULL;
    REQUIRE(!xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL && strncmp(error, "XR_TARGET_1001", 14) == 0);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static XrSemanticPlan *build_source_export_semantic(XrSemanticPlan **dependency_out,
                                                    bool with_argument) {
    XiFunc *dependency_root = xi_func_new("net_init", &stub_unit);
    XiFunc *write_bytes = xi_func_new("writeBytes", &stub_unit);
    REQUIRE(dependency_root && write_bytes);
    XiBlock *dependency_entry = xi_block_new(dependency_root);
    XiBlock *write_entry = xi_block_new(write_bytes);
    REQUIRE(dependency_entry && write_entry);
    dependency_entry->sealed = write_entry->sealed = true;
    dependency_root->children = (XiFunc **) xr_calloc(1, sizeof(*dependency_root->children));
    REQUIRE(dependency_root->children);
    dependency_root->children[0] = write_bytes;
    dependency_root->nchildren = dependency_root->children_cap = 1;
    write_bytes->parent_func = dependency_root;
    if (with_argument) {
        write_bytes->nparams = write_bytes->min_params = 1;
        write_bytes->params =
            (XiValue **) xr_calloc(1, sizeof(*write_bytes->params));
        REQUIRE(write_bytes->params);
        write_bytes->params[0] = xi_param(write_bytes, write_entry, 0, &stub_bool);
        REQUIRE(write_bytes->params[0]);
    }
    XiValue *closure =
        xi_value_new(dependency_root, dependency_entry, XI_CLOSURE_NEW, &stub_function, 0);
    XiValue *store = xi_value_new(dependency_root, dependency_entry, XI_SET_SHARED, &stub_unit, 1);
    REQUIRE(closure && store);
    closure->aux = write_bytes;
    store->args[0] = closure;
    store->aux_int = 0;
    dependency_root->nshared = 1;
    xi_block_set_return(dependency_entry, NULL);
    if (!with_argument)
        REQUIRE(xi_value_new(write_bytes, write_entry, XI_YIELD, &stub_unit, 0));
    xi_block_set_return(write_entry, NULL);
    dependency_root->stage = write_bytes->stage = XI_STAGE_SEMANTIC_LOWERED;
    dependency_root->invariant_mask = write_bytes->invariant_mask =
        xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(dependency_root, NULL));
    dependency_root->stage = write_bytes->stage = XI_STAGE_OPTIMIZED;
    XiModule *dependency_module = xi_module_new("stdlib/net/net.xr", "net", dependency_root);
    REQUIRE(dependency_module);
    dependency_root->module = dependency_module;
    dependency_module->nslots = 1;
    dependency_module->nexports = 1;
    dependency_module->exports =
        (XiModuleExport *) xr_calloc(1, sizeof(*dependency_module->exports));
    REQUIRE(dependency_module->exports);
    dependency_module->exports[0].name = "writeBytes";
    dependency_module->exports[0].shared_slot = 0;
    dependency_module->exports[0].function = write_bytes;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(dependency_root, error, sizeof(error)));
    XrSemanticPlan *dependency = xr_semantic_plan_retain(dependency_root->semantic_plan);
    REQUIRE(dependency && xr_semantic_plan_source_export_count(dependency) == 1);

    XiFunc *caller_root = xi_func_new("http_init", &stub_unit);
    XiFunc *caller = xi_func_new("_serverWriteAll", &stub_unit);
    REQUIRE(caller_root && caller);
    XiBlock *root_entry = xi_block_new(caller_root);
    XiBlock *caller_entry = xi_block_new(caller);
    REQUIRE(root_entry && caller_entry);
    root_entry->sealed = caller_entry->sealed = true;
    caller_root->children = (XiFunc **) xr_calloc(1, sizeof(*caller_root->children));
    REQUIRE(caller_root->children);
    caller_root->children[0] = caller;
    caller_root->nchildren = caller_root->children_cap = 1;
    caller->parent_func = caller_root;
    XiImportRef import_ref = {
        .module_path = "stdlib/net/net.xr",
        .resolved_mod_index = 0,
        .resolved_shared_slot = -1,
        .resolved_export_slot = -1,
        .resolved_module = dependency_module,
    };
    XiValue *namespace_ref =
        xi_value_new(caller_root, root_entry, XI_IMPORT_REF, &stub_module_namespace, 0);
    XiValue *namespace_alias =
        xi_value_new(caller_root, root_entry, XI_COPY, &stub_module_namespace, 1);
    XiValue *namespace_store = xi_value_new(caller_root, root_entry, XI_SET_SHARED, &stub_unit, 1);
    REQUIRE(namespace_ref && namespace_alias && namespace_store);
    namespace_ref->aux = &import_ref;
    namespace_alias->args[0] = namespace_ref;
    namespace_alias->aux_int = XI_COPY_KIND_IDENTITY;
    namespace_store->args[0] = namespace_alias;
    namespace_store->aux_int = 0;
    caller_root->nshared = 1;
    xi_block_set_return(root_entry, NULL);
    XiValue *receiver =
        xi_value_new(caller, caller_entry, XI_GET_SHARED, &stub_module_namespace, 0);
    XiValue *receiver_alias =
        xi_value_new(caller, caller_entry, XI_COPY, &stub_module_namespace, 1);
    XiValue *argument =
        with_argument ? xi_const_bool(caller, caller_entry, true, &stub_bool) : NULL;
    XiValue *method =
        xi_value_new(caller, caller_entry, XI_CALL_METHOD, &stub_unit, with_argument ? 2 : 1);
    REQUIRE(receiver && receiver_alias && (!with_argument || argument) && method);
    receiver->aux_int = 0;
    receiver_alias->args[0] = receiver;
    receiver_alias->aux_int = XI_COPY_KIND_IDENTITY;
    method->args[0] = receiver_alias;
    if (with_argument)
        method->args[1] = argument;
    method->aux = (void *) "writeBytes";
    method->aux_int = 0;
    xi_block_set_return(caller_entry, method);
    caller_root->stage = caller->stage = XI_STAGE_SEMANTIC_LOWERED;
    caller_root->invariant_mask = caller->invariant_mask =
        xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    SourceExportResolverFixture fixture = {
        .callee = write_bytes,
        .suspendability = with_argument ? 0 : 1,
    };
    XiCoroResolver resolver = {
        .resolve_method = source_export_resolve_method,
        .call_suspendability = source_export_call_suspendability,
        .ud = &fixture,
    };
    REQUIRE(xi_coro_lower(caller_root, &resolver));
    caller_root->stage = caller->stage = XI_STAGE_OPTIMIZED;
    XiModule *caller_module = xi_module_new("stdlib/http/http.xr", "http", caller_root);
    REQUIRE(caller_module);
    caller_root->module = caller_module;
    caller_module->nslots = 1;
    XiModule *dependency_modules[] = {dependency_module};
    bool module_set_built = xr_semantic_plan_build_and_attach_module_set(
        caller_root, dependency_modules, 1, error, sizeof(error));
    if (!module_set_built) {
        fprintf(stderr, "source export module-set fixture failed: %s\n", error);
    }
    REQUIRE(module_set_built);
    XrSemanticPlan *semantic = xr_semantic_plan_retain(caller_root->semantic_plan);
    REQUIRE(semantic);
    xi_func_free(caller_root);
    xi_func_free(dependency_root);
    *dependency_out = dependency;
    return semantic;
}

typedef enum ChannelCloseCallMutation {
    CHANNEL_CLOSE_MUTATE_IDENTITY,
    CHANNEL_CLOSE_MUTATE_SEMANTIC_TARGET,
    CHANNEL_CLOSE_MUTATE_SEMANTIC_OPERATION,
    CHANNEL_CLOSE_MUTATE_CALLEE,
    CHANNEL_CLOSE_MUTATE_RESULT_SLOT,
    CHANNEL_CLOSE_MUTATE_CALLER_STORAGE,
    CHANNEL_CLOSE_MUTATE_ARGUMENT_COUNT,
    CHANNEL_CLOSE_MUTATE_FLAGS,
    CHANNEL_CLOSE_MUTATE_CONVENTION,
    CHANNEL_CLOSE_MUTATE_TARGET_KIND,
    CHANNEL_CLOSE_MUTATION_COUNT,
} ChannelCloseCallMutation;

static void mutate_channel_close_call(XrTargetCallRecord *call, ChannelCloseCallMutation mutation) {
    switch (mutation) {
        case CHANNEL_CLOSE_MUTATE_IDENTITY:
            call->identity.bytes[0] ^= 1;
            break;
        case CHANNEL_CLOSE_MUTATE_SEMANTIC_TARGET:
            call->semantic_call_target = 0;
            break;
        case CHANNEL_CLOSE_MUTATE_SEMANTIC_OPERATION:
            call->semantic_operation--;
            break;
        case CHANNEL_CLOSE_MUTATE_CALLEE:
            call->callee_function = 0;
            break;
        case CHANNEL_CLOSE_MUTATE_RESULT_SLOT:
            call->result_slot = 0;
            break;
        case CHANNEL_CLOSE_MUTATE_CALLER_STORAGE:
            call->caller_storage_slot = 0;
            break;
        case CHANNEL_CLOSE_MUTATE_ARGUMENT_COUNT:
            call->argument_count = 1;
            break;
        case CHANNEL_CLOSE_MUTATE_FLAGS:
            call->flags = XR_TARGET_CALL_SUSPEND;
            break;
        case CHANNEL_CLOSE_MUTATE_CONVENTION:
            call->calling_convention = XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL;
            break;
        case CHANNEL_CLOSE_MUTATE_TARGET_KIND:
            call->target_kind = XR_TARGET_CALL_TARGET_DIRECT_LOCAL;
            break;
        case CHANNEL_CLOSE_MUTATION_COUNT:
            abort();
    }
}

static void test_channel_close_call_authority(void) {
    XrSemanticPlan *semantic = build_channel_close_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "channel-close target fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    REQUIRE(plan->calls_count == 1 && plan->call_arguments_count == 0);
    REQUIRE(plan->calls[0].semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
            plan->calls[0].callee_function == XR_SEMANTIC_INDEX_NONE &&
            plan->calls[0].calling_convention == XR_TARGET_CALL_CONVENTION_CHANNEL_CLOSE &&
            plan->calls[0].target_kind == XR_TARGET_CALL_TARGET_CHANNEL_CLOSE &&
            plan->calls[0].argument_count == 0 && plan->calls[0].flags == 0 &&
            plan->calls[0].result_slot == XR_SEMANTIC_INDEX_NONE);
    REQUIRE((plan->completed_family_mask & XR_TARGET_FAMILY_CHANNEL_ALLOCATION_STORAGE) != 0);
    const XrTargetValueRepRecord *channel_binding = xr_target_plan_value_rep(plan, 1);
    const XrTargetValueRepRecord *alias_binding = xr_target_plan_value_rep(plan, 2);
    REQUIRE(channel_binding && alias_binding && channel_binding->slot < plan->slots_count &&
            alias_binding->slot < plan->slots_count);
    REQUIRE(
        plan->machine_reps[channel_binding->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
        plan->machine_reps[channel_binding->register_rep].ownership == XR_TARGET_OWNERSHIP_OWNED &&
        plan->machine_reps[alias_binding->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
        plan->machine_reps[alias_binding->register_rep].ownership == XR_TARGET_OWNERSHIP_BORROWED);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    uint64_t saved_families = plan->completed_family_mask;
    plan->completed_family_mask &= ~XR_TARGET_FAMILY_CHANNEL_ALLOCATION_STORAGE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->completed_family_mask = saved_families;
    XrTargetMachineRepRecord saved_rep = plan->machine_reps[channel_binding->register_rep];
    plan->machine_reps[channel_binding->register_rep].ownership = XR_TARGET_OWNERSHIP_BORROWED;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[channel_binding->register_rep] = saved_rep;
    uint32_t saved_semantic_operation = plan->slots[channel_binding->slot].semantic_operation;
    plan->slots[channel_binding->slot].semantic_operation =
        plan->slots[alias_binding->slot].semantic_operation;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[channel_binding->slot].semantic_operation = saved_semantic_operation;

    XrSemanticOperationRecord *mutable_channel = &semantic->operations[saved_semantic_operation];
    REQUIRE(mutable_channel->opcode == XI_CHAN_NEW);
    const char *saved_allocation_key = mutable_channel->allocation_key;
    XrStableId saved_allocation_id = mutable_channel->allocation_id;
    const char forged_allocation_key[] = "forged-channel-operation/allocation";
    XrFingerprint allocation_digest;
    REQUIRE(xr_stable_id_from_key(forged_allocation_key, &mutable_channel->allocation_id,
                                  &allocation_digest));
    mutable_channel->allocation_key = forged_allocation_key;
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    semantic->ownership->semantic_fingerprint = semantic->fingerprint;
    semantic->ownership->fingerprint = semantic->fingerprint;
    plan->semantic_fingerprint = semantic->fingerprint;
    for (uint32_t i = 0; i < plan->layouts_count; i++)
        xr_target_layout_compute_fingerprint(plan, i, &plan->layouts[i].fingerprint);
    for (uint32_t i = 0; i < plan->calls_count; i++)
        xr_target_call_compute_fingerprint(plan, i, &plan->calls[i].fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    expect_verify_failure_raw(plan, "XR_TARGET_1001");

    mutable_channel->allocation_key = saved_allocation_key;
    mutable_channel->allocation_id = saved_allocation_id;
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    semantic->ownership->semantic_fingerprint = semantic->fingerprint;
    semantic->ownership->fingerprint = semantic->fingerprint;
    plan->semantic_fingerprint = semantic->fingerprint;
    for (uint32_t i = 0; i < plan->layouts_count; i++)
        xr_target_layout_compute_fingerprint(plan, i, &plan->layouts[i].fingerprint);
    for (uint32_t i = 0; i < plan->calls_count; i++)
        xr_target_call_compute_fingerprint(plan, i, &plan->calls[i].fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    char call_hex[XR_FINGERPRINT_BYTES * 2 + 1];
    xr_fingerprint_hex(plan->calls[0].fingerprint, call_hex);
    REQUIRE(strcmp(call_hex,
                   "056f77a2e7eea7e17325e2aae74113bf39a9189b46f117531ed8799d1a86c193") == 0);
    for (uint32_t mutation = 0; mutation < CHANNEL_CLOSE_MUTATION_COUNT; mutation++) {
        XrTargetCallRecord saved = plan->calls[0];
        XrTargetCallArgumentRecord fabricated_argument = {0};
        XrTargetCallArgumentRecord *saved_arguments = plan->call_arguments;
        uint32_t saved_argument_count = plan->call_arguments_count;
        mutate_channel_close_call(&plan->calls[0], (ChannelCloseCallMutation) mutation);
        if (mutation == CHANNEL_CLOSE_MUTATE_ARGUMENT_COUNT) {
            plan->call_arguments = &fabricated_argument;
            plan->call_arguments_count = 1;
        }
        xr_target_call_compute_fingerprint(plan, 0, &plan->calls[0].fingerprint);
        expect_verify_failure(plan, "XR_TARGET_1003");
        plan->call_arguments = saved_arguments;
        plan->call_arguments_count = saved_argument_count;
        plan->calls[0] = saved;
    }
    plan->calls_count = 0;
    expect_verify_failure(plan, "XR_TARGET_1003");
    plan->calls_count = 1;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);

    static const struct {
        const char *selector;
        int64_t selector_immediate;
        XrType *receiver_type;
        XrType *result_type;
        bool extra_argument;
    } rejected[] = {
        {"send", 314, &stub_channel, &stub_unit, false},
        {"close", 315, &stub_channel, &stub_unit, false},
        {"close", 314, &stub_channel, &stub_unit, true},
        {"close", 314, &stub_int, &stub_unit, false},
        {"close", 314, &stub_channel, &stub_int, false},
    };
    profile = build_profile(0);
    for (uint32_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        semantic = build_channel_method_semantic(
            rejected[i].selector, rejected[i].selector_immediate, rejected[i].receiver_type,
            rejected[i].result_type, rejected[i].extra_argument);
        plan = NULL;
        error[0] = '\0';
        REQUIRE(!xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
        REQUIRE(plan == NULL && strncmp(error, "XR_TARGET_1003", 14) == 0);
        xr_semantic_plan_free(semantic);
    }
    xr_target_profile_free(profile);
}

static XrSemanticPlan *build_lowered_coroutine_direct_call(bool unit_result) {
    XrType unit_function = {
        .kind = XR_KIND_FUNCTION,
        .id = 109,
        .frozen = true,
        .function =
            {
                .return_type = &stub_unit,
                .throw_effect = XR_FN_EFFECT_NO_THROW,
            },
    };
    XrType *result_type = unit_result ? &stub_unit : &stub_int;
    XrType *function_type = unit_result ? &unit_function : &stub_function;
    XiFunc *root =
        xi_func_new(unit_result ? "target_coro_unit_root" : "target_coro_i64_root", result_type);
    XiFunc *child =
        xi_func_new(unit_result ? "target_coro_unit_child" : "target_coro_i64_child", result_type);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);
    root_entry->sealed = child_entry->sealed = true;

    XiValue *yield = xi_value_new(child, child_entry, XI_YIELD, &stub_unit, 0);
    REQUIRE(yield != NULL);
    if (unit_result) {
        xi_block_set_return(child_entry, NULL);
    } else {
        XiValue *child_result = xi_const_int(child, child_entry, 73, &stub_int);
        REQUIRE(child_result != NULL);
        xi_block_set_return(child_entry, child_result);
    }

    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;

    XiValue *closure = xi_value_new(root, root_entry, XI_CLOSURE_NEW, function_type, 0);
    REQUIRE(closure != NULL);
    closure->aux = child;
    XiValue *call = xi_value_new(root, root_entry, XI_CALL, result_type, 1);
    REQUIRE(call != NULL);
    call->args[0] = closure;
    xi_block_set_return(root_entry, unit_result ? NULL : call);

    root->stage = child->stage = XI_STAGE_SEMANTIC_LOWERED;
    root->invariant_mask = child->invariant_mask = xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(root, NULL));
    REQUIRE(root->coro_plan && root->coro_plan->is_coroutine && child->coro_plan &&
            child->coro_plan->is_coroutine);
    root->stage = child->stage = XI_STAGE_OPTIMIZED;

    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "lowered coroutine semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    REQUIRE(xr_semantic_plan_call_target_count(plan) == 1);
    xi_func_free(root);
    return plan;
}

static XrSemanticPlan *build_lowered_coroutine_with_sync_call(void) {
    XiFunc *root = xi_func_new("target_coro_sync_caller", &stub_int);
    XiFunc *child = xi_func_new("target_coro_sync_child", &stub_int);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);
    root_entry->sealed = child_entry->sealed = true;
    XiValue *child_result = xi_const_int(child, child_entry, 17, &stub_int);
    REQUIRE(child_result != NULL);
    xi_block_set_return(child_entry, child_result);
    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;
    XiValue *yield = xi_value_new(root, root_entry, XI_YIELD, &stub_unit, 0);
    XiValue *closure = xi_value_new(root, root_entry, XI_CLOSURE_NEW, &stub_function, 0);
    XiValue *call = xi_value_new(root, root_entry, XI_CALL, &stub_int, 1);
    REQUIRE(yield != NULL && closure != NULL && call != NULL);
    closure->aux = child;
    call->args[0] = closure;
    xi_block_set_return(root_entry, call);
    root->stage = child->stage = XI_STAGE_SEMANTIC_LOWERED;
    root->invariant_mask = child->invariant_mask = xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(root, NULL));
    REQUIRE(root->coro_plan && root->coro_plan->is_coroutine && child->coro_plan &&
            !child->coro_plan->is_coroutine);
    root->stage = child->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "sync call in coroutine semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL && xr_semantic_plan_call_target_count(plan) == 1);
    xi_func_free(root);
    return plan;
}

static XrSemanticPlan *build_lowered_tail_coroutine_chain(void) {
    XiFunc *root = xi_func_new("target_coro_tail_root", &stub_int);
    XiFunc *wrapper = xi_func_new("target_coro_tail_wrapper", &stub_int);
    XiFunc *leaf = xi_func_new("target_coro_tail_leaf", &stub_int);
    REQUIRE(root != NULL && wrapper != NULL && leaf != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *wrapper_entry = xi_block_new(wrapper);
    XiBlock *leaf_entry = xi_block_new(leaf);
    REQUIRE(root_entry != NULL && wrapper_entry != NULL && leaf_entry != NULL);
    root_entry->sealed = wrapper_entry->sealed = leaf_entry->sealed = true;

    XiValue *yield = xi_value_new(leaf, leaf_entry, XI_YIELD, &stub_unit, 0);
    XiValue *leaf_result = xi_const_int(leaf, leaf_entry, 29, &stub_int);
    REQUIRE(yield != NULL && leaf_result != NULL);
    xi_block_set_return(leaf_entry, leaf_result);

    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = wrapper;
    root->nchildren = root->children_cap = 1;
    wrapper->parent_func = root;
    wrapper->children = (XiFunc **) xr_calloc(1, sizeof(*wrapper->children));
    REQUIRE(wrapper->children != NULL);
    wrapper->children[0] = leaf;
    wrapper->nchildren = wrapper->children_cap = 1;
    leaf->parent_func = wrapper;

    /* Seed XiCoroLower so it materializes the caller state CFG.  The seed is
     * rewritten below before SemanticPlan is frozen: schema 29 must then
     * derive wrapper
     * suspendability solely through the DIRECT_LOCAL tail edge, while the wrapper itself has no
     * state row. */
    XiValue *wrapper_seed = xi_value_new(wrapper, wrapper_entry, XI_YIELD, &stub_unit, 0);
    XiValue *leaf_closure = xi_value_new(wrapper, wrapper_entry, XI_CLOSURE_NEW, &stub_function, 0);
    XiValue *tail = xi_value_new(wrapper, wrapper_entry, XI_TAIL_CALL, &stub_int, 1);
    REQUIRE(wrapper_seed != NULL && leaf_closure != NULL && tail != NULL);
    leaf_closure->aux = leaf;
    tail->args[0] = leaf_closure;
    xi_block_set_return(wrapper_entry, tail);

    XiValue *wrapper_closure = xi_value_new(root, root_entry, XI_CLOSURE_NEW, &stub_function, 0);
    XiValue *call = xi_value_new(root, root_entry, XI_CALL, &stub_int, 1);
    REQUIRE(wrapper_closure != NULL && call != NULL);
    wrapper_closure->aux = wrapper;
    call->args[0] = wrapper_closure;
    xi_block_set_return(root_entry, call);

    root->stage = wrapper->stage = leaf->stage = XI_STAGE_SEMANTIC_LOWERED;
    root->invariant_mask = wrapper->invariant_mask = leaf->invariant_mask =
        xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(root, NULL));
    REQUIRE(wrapper->coro_plan && wrapper->coro_plan->nstates == 1);
    wrapper_seed->op = XI_CONST;
    wrapper_seed->type = &stub_int;
    wrapper_seed->aux_int = 0;
    wrapper->coro_plan->nstates = 0;
    REQUIRE(root->coro_plan && root->coro_plan->is_coroutine && wrapper->coro_plan &&
            wrapper->coro_plan->is_coroutine && wrapper->coro_plan->nstates == 0 &&
            leaf->coro_plan && leaf->coro_plan->is_coroutine);
    root->stage = wrapper->stage = leaf->stage = XI_STAGE_OPTIMIZED;

    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "tail coroutine chain semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL && xr_semantic_plan_call_target_count(plan) == 2);
    xi_func_free(root);
    return plan;
}

static XrSemanticPlan *build_direct_local_aggregate_call(void) {
    XrType *aggregate_elements[2] = {&stub_int, &stub_bool};
    XrType aggregate = {
        .kind = XR_KIND_TUPLE,
        .id = 107,
        .frozen = true,
        .is_value_type = true,
        .scalar_rep = XR_SCALAR_REP_NONE,
    };
    aggregate.tuple.element_types = aggregate_elements;
    aggregate.tuple.element_count = 2;
    XrType function_type = {
        .kind = XR_KIND_FUNCTION,
        .id = 108,
        .frozen = true,
        .function = {.return_type = &aggregate, .throw_effect = XR_FN_EFFECT_NO_THROW},
    };
    XiFunc *root = xi_func_new("target_aggregate_call_root", &aggregate);
    XiFunc *child = xi_func_new("target_aggregate_call_child", &aggregate);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);
    XiValue *child_int = xi_const_int(child, child_entry, 1, &stub_int);
    XiValue *child_bool = xi_const_bool(child, child_entry, true, &stub_bool);
    XiValue *child_result = xi_value_new(child, child_entry, XI_TUPLE_NEW, &aggregate, 2);
    REQUIRE(child_int != NULL && child_bool != NULL && child_result != NULL);
    child_result->args[0] = child_int;
    child_result->args[1] = child_bool;
    child_result->aux_int = xi_tuple_pack_aux(2, 0);
    xi_block_set_return(child_entry, child_result);
    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;
    XiValue *closure = xi_value_new(root, root_entry, XI_STACK_ALLOC, &function_type, 0);
    REQUIRE(closure != NULL);
    closure->aux_int = XI_CLOSURE_NEW;
    closure->aux = child;
    XiValue *call = xi_value_new(root, root_entry, XI_CALL, &aggregate, 1);
    REQUIRE(call != NULL);
    call->args[0] = closure;
    XiValue *root_int = xi_const_int(root, root_entry, 2, &stub_int);
    XiValue *root_bool = xi_const_bool(root, root_entry, false, &stub_bool);
    XiValue *root_result = xi_value_new(root, root_entry, XI_TUPLE_NEW, &aggregate, 2);
    REQUIRE(root_int != NULL && root_bool != NULL && root_result != NULL);
    root_result->args[0] = root_int;
    root_result->args[1] = root_bool;
    root_result->aux_int = xi_tuple_pack_aux(2, 0);
    xi_block_set_return(root_entry, root_result);
    root->stage = child->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "aggregate-call semantic fixture failed: %s\n", error);
    REQUIRE(built);
    REQUIRE(plan != NULL && xr_semantic_plan_call_target_count(plan) == 1);
    xi_func_free(root);
    return plan;
}

static void test_unknown_call_target_fails_closed(void) {
    XrSemanticPlan *semantic = build_struct_and_named_aggregate_semantic(true);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(!xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL);
    REQUIRE(strncmp(error, "XR_TARGET_1003", strlen("XR_TARGET_1003")) == 0);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

typedef enum SourceExportCallMutation {
    SOURCE_EXPORT_MUTATE_DEPENDENCY,
    SOURCE_EXPORT_MUTATE_EXPORT,
    SOURCE_EXPORT_MUTATE_EXPORT_IDENTITY,
    SOURCE_EXPORT_MUTATE_CALLEE_IDENTITY,
    SOURCE_EXPORT_MUTATE_CALLEE_FUNCTION,
    SOURCE_EXPORT_MUTATE_ARGUMENT_COUNT,
    SOURCE_EXPORT_MUTATE_FLAGS,
    SOURCE_EXPORT_MUTATE_CONVENTION,
    SOURCE_EXPORT_MUTATE_KIND,
    SOURCE_EXPORT_MUTATION_COUNT,
} SourceExportCallMutation;

static void mutate_source_export_call(XrTargetCallRecord *call, SourceExportCallMutation mutation) {
    switch (mutation) {
        case SOURCE_EXPORT_MUTATE_DEPENDENCY:
            call->source_dependency = XR_SEMANTIC_INDEX_NONE;
            break;
        case SOURCE_EXPORT_MUTATE_EXPORT:
            call->source_export = XR_SEMANTIC_INDEX_NONE;
            break;
        case SOURCE_EXPORT_MUTATE_EXPORT_IDENTITY:
            call->source_export_identity.bytes[0] ^= 1;
            break;
        case SOURCE_EXPORT_MUTATE_CALLEE_IDENTITY:
            call->source_callee_identity.bytes[0] ^= 1;
            break;
        case SOURCE_EXPORT_MUTATE_CALLEE_FUNCTION:
            call->callee_function = 0;
            break;
        case SOURCE_EXPORT_MUTATE_ARGUMENT_COUNT:
            call->argument_begin = 1;
            break;
        case SOURCE_EXPORT_MUTATE_FLAGS:
            call->flags = 0;
            break;
        case SOURCE_EXPORT_MUTATE_CONVENTION:
            call->calling_convention = XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL;
            break;
        case SOURCE_EXPORT_MUTATE_KIND:
            call->target_kind = XR_TARGET_CALL_TARGET_DIRECT_LOCAL;
            break;
        case SOURCE_EXPORT_MUTATION_COUNT:
            abort();
    }
}

static void test_source_export_call_authority(void) {
    XrSemanticPlan *dependency = NULL;
    XrSemanticPlan *semantic = build_source_export_semantic(&dependency, false);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(!xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL && strncmp(error, "XR_TARGET_1000", strlen("XR_TARGET_1000")) == 0);
    const XrSemanticPlan *dependencies[] = {dependency};
    bool built = xr_target_plan_build_module_set(semantic, dependencies, 1, profile, &plan, error,
                                                 sizeof(error));
    if (!built)
        fprintf(stderr, "source-export Target build failed: %s\n", error);
    REQUIRE(built);
    REQUIRE(plan && xr_target_plan_verify(plan, error, sizeof(error)));
    REQUIRE(plan->semantic_dependency_count == 1 && plan->semantic_dependencies[0] == dependency &&
            plan->calls_count == 1 && plan->call_arguments_count == 0 &&
            plan->coroutines_count == 1);
    XrTargetCallRecord *call = &plan->calls[0];
    REQUIRE(call->semantic_call_target == 0 && call->callee_function == XR_SEMANTIC_INDEX_NONE &&
            call->source_dependency == 0 && call->source_export == 0 && call->argument_count == 0 &&
            call->calling_convention == XR_TARGET_CALL_CONVENTION_SOURCE_EXPORT &&
            call->target_kind == XR_TARGET_CALL_TARGET_SOURCE_EXPORT &&
            call->flags == XR_TARGET_CALL_SUSPEND);
    const XrSemanticCallTargetRecord *semantic_target = xr_semantic_plan_call_target(semantic, 0);
    REQUIRE(semantic_target &&
            xr_stable_id_equal(call->source_export_identity, semantic_target->export_identity) &&
            xr_stable_id_equal(call->source_callee_identity, semantic_target->callee_function));
    REQUIRE(plan->coroutines[0].direct_call == call->id &&
            plan->coroutines[0].result_slot == call->result_slot &&
            plan->coroutines[0].flags ==
                (XR_TARGET_COROUTINE_DIRECT_CHILD | XR_TARGET_COROUTINE_SOURCE_CHILD));
    REQUIRE((plan->completed_family_mask & XR_TARGET_FAMILY_SOURCE_IMPORT_STORAGE) != 0);
    uint32_t import_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t load_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t copy_values[2] = {XR_SEMANTIC_INDEX_NONE, XR_SEMANTIC_INDEX_NONE};
    uint32_t copy_operations[2] = {XR_SEMANTIC_INDEX_NONE, XR_SEMANTIC_INDEX_NONE};
    uint32_t copy_count = 0;
    uint32_t import_operation = XR_SEMANTIC_INDEX_NONE;
    uint32_t load_operation = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (operation && operation->opcode == XI_IMPORT_REF) {
            REQUIRE(import_value == XR_SEMANTIC_INDEX_NONE);
            import_value = operation->result_value;
            import_operation = i;
        } else if (operation && operation->opcode == XI_GET_SHARED) {
            REQUIRE(load_value == XR_SEMANTIC_INDEX_NONE);
            load_value = operation->result_value;
            load_operation = i;
        } else if (operation && operation->opcode == XI_COPY) {
            REQUIRE(copy_count < 2);
            copy_values[copy_count] = operation->result_value;
            copy_operations[copy_count++] = i;
        }
    }
    const XrTargetValueRepRecord *import_binding = xr_target_plan_value_rep(plan, import_value);
    const XrTargetValueRepRecord *load_binding = xr_target_plan_value_rep(plan, load_value);
    REQUIRE(import_binding && load_binding && import_binding->slot < plan->slots_count &&
            load_binding->slot < plan->slots_count);
    const XrTargetSlotRecord *import_slot = &plan->slots[import_binding->slot];
    const XrTargetSlotRecord *load_slot = &plan->slots[load_binding->slot];
    REQUIRE(import_slot->semantic_operation == import_operation &&
            load_slot->semantic_operation == load_operation &&
            import_slot->ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            load_slot->ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            import_slot->root_kind == XR_TARGET_ROOT_DYNAMIC &&
            load_slot->root_kind == XR_TARGET_ROOT_DYNAMIC &&
            plan->machine_reps[import_binding->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[load_binding->register_rep].kind == XR_MACHINE_REP_DYN_VALUE);
    REQUIRE(copy_count == 2);
    for (uint32_t i = 0; i < copy_count; i++) {
        const XrTargetValueRepRecord *copy_binding = xr_target_plan_value_rep(plan, copy_values[i]);
        REQUIRE(copy_binding && copy_binding->slot < plan->slots_count &&
                plan->slots[copy_binding->slot].semantic_operation == copy_operations[i] &&
                plan->slots[copy_binding->slot].semantic_value == copy_values[i] &&
                plan->slots[copy_binding->slot].ownership == XR_TARGET_OWNERSHIP_BORROWED &&
                plan->slots[copy_binding->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                plan->machine_reps[copy_binding->register_rep].kind == XR_MACHINE_REP_DYN_VALUE);
    }

    uint64_t saved_mask = plan->completed_family_mask;
    plan->completed_family_mask &= ~XR_TARGET_FAMILY_SOURCE_IMPORT_STORAGE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->completed_family_mask = saved_mask;
    uint16_t saved_ownership = plan->machine_reps[import_binding->register_rep].ownership;
    plan->machine_reps[import_binding->register_rep].ownership = XR_TARGET_OWNERSHIP_OWNED;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[import_binding->register_rep].ownership = saved_ownership;
    uint32_t saved_operation = plan->slots[import_binding->slot].semantic_operation;
    plan->slots[import_binding->slot].semantic_operation = load_operation;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[import_binding->slot].semantic_operation = saved_operation;

    for (uint32_t mutation = 0; mutation < SOURCE_EXPORT_MUTATION_COUNT; mutation++) {
        XrTargetCallRecord saved = *call;
        mutate_source_export_call(call, (SourceExportCallMutation) mutation);
        xr_target_call_compute_fingerprint(plan, 0, &call->fingerprint);
        expect_verify_failure(plan, "XR_TARGET_1003");
        *call = saved;
    }
    uint16_t saved_state_flags = plan->coroutines[0].flags;
    plan->coroutines[0].flags &= ~XR_TARGET_COROUTINE_SOURCE_CHILD;
    expect_verify_failure(plan, "XR_CORO_4000");
    plan->coroutines[0].flags = saved_state_flags;
    uint32_t saved_direct_call = plan->coroutines[0].direct_call;
    plan->coroutines[0].direct_call = XR_SEMANTIC_INDEX_NONE;
    expect_verify_failure(plan, "XR_CORO_4000");
    plan->coroutines[0].direct_call = saved_direct_call;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    XrTargetPlan *failed = NULL;
    REQUIRE(!xr_target_plan_build_module_set(semantic, NULL, 1, profile, &failed, error,
                                             sizeof(error)));
    REQUIRE(failed == NULL);
    const XrSemanticPlan *wrong_dependencies[] = {semantic};
    REQUIRE(!xr_target_plan_build_module_set(semantic, wrong_dependencies, 1, profile, &failed,
                                             error, sizeof(error)));
    REQUIRE(failed == NULL);
    xr_target_plan_free(plan);

    XrSemanticOperationRecord saved_copy = semantic->operations[copy_operations[1]];
    semantic->operations[copy_operations[1]].semantic_immediate = XI_COPY_KIND_VALUE_CLONE;
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    REQUIRE(!xr_target_plan_build_module_set(semantic, dependencies, 1, profile, &failed, error,
                                             sizeof(error)));
    REQUIRE(failed == NULL);
    semantic->operations[copy_operations[1]] = saved_copy;

    semantic->operations[copy_operations[1]].function = 0;
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    REQUIRE(!xr_target_plan_build_module_set(semantic, dependencies, 1, profile, &failed, error,
                                             sizeof(error)));
    REQUIRE(failed == NULL);
    semantic->operations[copy_operations[1]] = saved_copy;

    uint32_t copy_operand = saved_copy.operand_begin;
    XrSemanticOperandRecord saved_operand = semantic->operands[copy_operand];
    semantic->operands[copy_operand].value = saved_copy.result_value;
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    REQUIRE(!xr_target_plan_build_module_set(semantic, dependencies, 1, profile, &failed, error,
                                             sizeof(error)));
    REQUIRE(failed == NULL);
    semantic->operands[copy_operand] = saved_operand;

    uint32_t saved_control = semantic->blocks[0].control_value;
    semantic->blocks[0].control_value = copy_values[0];
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    REQUIRE(!xr_target_plan_build_module_set(semantic, dependencies, 1, profile, &failed, error,
                                             sizeof(error)));
    REQUIRE(failed == NULL);
    semantic->blocks[0].control_value = saved_control;
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    xr_semantic_plan_free(dependency);
}

static void test_source_export_call_argument_authority(void) {
    XrSemanticPlan *dependency = NULL;
    XrSemanticPlan *semantic = build_source_export_semantic(&dependency, true);
    XrTargetProfile *profile = build_profile(0);
    const XrSemanticPlan *dependencies[] = {dependency};
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build_module_set(semantic, dependencies, 1, profile, &plan, error,
                                                 sizeof(error));
    if (!built)
        fprintf(stderr, "source export argument Target fixture failed: %s\n", error);
    REQUIRE(built && plan && xr_target_plan_verify(plan, error, sizeof(error)));
    REQUIRE(plan->calls_count == 1 && plan->call_arguments_count == 1 &&
            plan->coroutines_count == 0);
    XrTargetCallRecord *call = &plan->calls[0];
    XrTargetCallArgumentRecord *argument = &plan->call_arguments[0];
    REQUIRE(call->target_kind == XR_TARGET_CALL_TARGET_SOURCE_EXPORT &&
            call->calling_convention == XR_TARGET_CALL_CONVENTION_SOURCE_EXPORT &&
            call->argument_begin == 0 && call->argument_count == 1 && call->flags == 0);
    REQUIRE(argument->call == call->id && argument->ordinal == 0 &&
            argument->callee_parameter == 0 &&
            argument->callee_slot == XR_SEMANTIC_INDEX_NONE &&
            argument->mode == XR_TARGET_CALL_VALUE &&
            argument->ownership == XR_TARGET_CALL_CONSUME &&
            argument->flags == 0 && argument->callee_register_rep == argument->register_rep &&
            argument->callee_memory_rep == argument->memory_rep);

    XrTargetCallArgumentRecord saved = *argument;
    argument->mode = XR_TARGET_CALL_REFERENCE;
    xr_target_call_compute_fingerprint(plan, 0, &call->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1003");
    *argument = saved;
    xr_target_call_compute_fingerprint(plan, 0, &call->fingerprint);
    argument->identity.bytes[0] ^= 1u;
    xr_target_call_compute_fingerprint(plan, 0, &call->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1003");
    *argument = saved;
    xr_target_call_compute_fingerprint(plan, 0, &call->fingerprint);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    xr_semantic_plan_free(dependency);
}

static void test_direct_local_call_adapter_family(void) {
    XrSemanticPlan *semantic =
        build_direct_local_scalar_calls(XI_CALL, &stub_int, &stub_function);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *first = NULL;
    XrTargetPlan *second = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &first, error, sizeof(error)));
    REQUIRE(xr_target_plan_build(semantic, profile, &second, error, sizeof(error)));
    REQUIRE(first->calls_count == 2 && first->call_arguments_count == 2 &&
            first->adapters_count == 0);
    uint32_t root_instruction_count = 0;
    uint32_t child_instruction_count = 0;
    const XrTargetInstructionRecord *root_instructions =
        xr_target_plan_function_instructions(first, 0, &root_instruction_count);
    const XrTargetInstructionRecord *child_instructions =
        xr_target_plan_function_instructions(first, 1, &child_instruction_count);
    REQUIRE(root_instructions != NULL && root_instruction_count == 4u &&
            root_instructions[0].opcode == XR_TARGET_INSTRUCTION_CONST_I64 &&
            root_instructions[1].opcode ==
                XR_TARGET_INSTRUCTION_CALL_DIRECT_I64 &&
            root_instructions[1].immediate_bits == 0 &&
            root_instructions[2].opcode ==
                XR_TARGET_INSTRUCTION_CALL_DIRECT_I64 &&
            root_instructions[2].immediate_bits == 1 &&
            root_instructions[3].opcode == XR_TARGET_INSTRUCTION_RETURN_I64);
    REQUIRE(child_instructions != NULL && child_instruction_count == 2u &&
            child_instructions[0].opcode == XR_TARGET_INSTRUCTION_PARAM_I64 &&
            child_instructions[1].opcode == XR_TARGET_INSTRUCTION_RETURN_I64);
    REQUIRE(xr_fingerprint_equal(first->fingerprint, second->fingerprint));
    char call_hex[XR_FINGERPRINT_BYTES * 2 + 1];
    xr_fingerprint_hex(first->calls[0].fingerprint, call_hex);
    REQUIRE(strcmp(call_hex,
                   "9f400d8697367d34487eab48e2b121d33abe565bad843e369bcdfae435755b1e") == 0);
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(profile);
    REQUIRE(machine != NULL);
    for (uint32_t i = 0; i < first->calls_count; i++) {
        const XrTargetCallRecord *call = &first->calls[i];
        const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(semantic, i);
        REQUIRE(
            target != NULL && call->id == i && call->semantic_call_target == i &&
            call->semantic_operation == target->operation &&
            call->callee_function == target->function &&
            call->calling_convention == XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL &&
            call->target_kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
            call->native_abi == machine->native_abi && call->result_mode == XR_TARGET_CALL_VALUE &&
            call->result_ownership == XR_TARGET_CALL_NONE &&
            call->caller_storage_slot == XR_SEMANTIC_INDEX_NONE &&
            call->error_slot == XR_SEMANTIC_INDEX_NONE &&
            call->error_mode == XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL && call->adapter_begin == 0 &&
            call->adapter_count == 0 && call->argument_begin == i && call->argument_count == 1);
        REQUIRE(first->machine_reps[call->error_register_rep].kind == XR_MACHINE_REP_VOID &&
                first->machine_reps[call->error_memory_rep].kind == XR_MACHINE_REP_VOID);
        const XrTargetCallArgumentRecord *argument = &first->call_arguments[i];
        REQUIRE(argument->call == i && argument->ordinal == 0 &&
                argument->mode == XR_TARGET_CALL_VALUE && argument->flags == 0 &&
                argument->caller_slot < first->slots_count &&
                argument->callee_slot < first->slots_count &&
                first->slots[argument->caller_slot].function == call->caller_function &&
                first->slots[argument->callee_slot].function == call->callee_function &&
                argument->register_rep == first->slots[argument->caller_slot].register_rep &&
                argument->memory_rep == first->slots[argument->callee_slot].memory_rep);
    }

    first->calls[1].argument_begin = 0;
    expect_verify_failure(first, "XR_TARGET_1003");
    first->calls[1].argument_begin = 1;
    first->call_arguments[1].semantic_operand--;
    expect_verify_failure(first, "XR_TARGET_1003");
    first->call_arguments[1].semantic_operand++;
    first->calls[0].identity.bytes[0] ^= 1;
    expect_verify_failure(first, "XR_TARGET_1003");
    first->calls[0].identity.bytes[0] ^= 1;
    first->calls[0].native_abi++;
    expect_verify_failure(first, "XR_TARGET_1003");
    first->calls[0].native_abi--;
    first->calls[0].fingerprint.bytes[0] ^= 1;
    expect_verify_failure(first, "XR_TARGET_1003");
    first->calls[0].fingerprint.bytes[0] ^= 1;
    first->calls_count--;
    expect_verify_failure(first, "XR_TARGET_1003");
    first->calls_count++;
    REQUIRE(xr_target_plan_verify(first, error, sizeof(error)));

    xr_target_plan_free(second);
    xr_target_plan_free(first);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_direct_local_raw_pointer_call_authority(void) {
    XrSemanticPlan *semantic = build_direct_local_scalar_calls(
        XI_CALL, &stub_raw_pointer, &stub_raw_pointer_function);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    REQUIRE(plan != NULL && plan->calls_count == 2 &&
            plan->call_arguments_count == 2);

    uint32_t pointer_type = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < semantic->type_count; i++)
        if (semantic->types[i].kind == XR_KIND_POINTER) {
            REQUIRE(pointer_type == XR_SEMANTIC_INDEX_NONE);
            pointer_type = i;
        }
    REQUIRE(pointer_type != XR_SEMANTIC_INDEX_NONE);

    uint32_t raw_rep = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->machine_reps_count; i++)
        if (plan->machine_reps[i].kind == XR_MACHINE_REP_RAW_PTR) {
            REQUIRE(raw_rep == XR_SEMANTIC_INDEX_NONE);
            raw_rep = i;
        }
    REQUIRE(raw_rep != XR_SEMANTIC_INDEX_NONE &&
            plan->machine_reps[raw_rep].ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
            plan->machine_reps[raw_rep].null_encoding == XR_TARGET_NULL_ZERO);
    uint32_t raw_pointer_values = 0;
    for (uint32_t i = 0; i < plan->value_reps_count; i++)
        if (plan->value_reps[i].register_rep == raw_rep) {
            REQUIRE(plan->value_reps[i].memory_rep == raw_rep);
            raw_pointer_values++;
        }
    REQUIRE(raw_pointer_values >= 3);
    for (uint32_t i = 0; i < plan->call_arguments_count; i++)
        REQUIRE(plan->call_arguments[i].register_rep == raw_rep &&
                plan->call_arguments[i].memory_rep == raw_rep);

    plan->machine_reps[raw_rep].ownership = XR_TARGET_OWNERSHIP_BORROWED;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[raw_rep].ownership = XR_TARGET_OWNERSHIP_TRIVIAL;
    plan->machine_reps[raw_rep].null_encoding = XR_TARGET_NULL_NOT_NULLABLE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[raw_rep].null_encoding = XR_TARGET_NULL_ZERO;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    XrSemanticTypeRecord *mutated_type = &semantic->types[pointer_type];
    mutated_type->flags = XR_SEM_TYPE_REFERENCE_CAPABLE;
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    XrTargetPlan *failed = NULL;
    error[0] = '\0';
    REQUIRE(!xr_target_plan_build(semantic, profile, &failed, error, sizeof(error)));
    REQUIRE(failed == NULL && error[0] != '\0');

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_source_instance_method_target_fails_closed(void) {
    XrSemanticPlan *semantic = build_source_instance_method_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(!xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_open_source_instance_method_target_fails_closed(void) {
    XrSemanticPlan *dependency = NULL;
    XrSemanticPlan *semantic = build_open_source_instance_method_semantic(&dependency);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    const XrSemanticPlan *dependencies[] = {dependency};
    REQUIRE(!xr_target_plan_build_module_set(semantic, dependencies, 1, profile, &plan, error,
                                             sizeof(error)));
    REQUIRE(plan == NULL);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    xr_semantic_plan_free(dependency);
}

static void test_coroutine_state_call_family(void) {
    XrTargetProfile *profile = build_profile(0);
    for (uint32_t unit_result = 0; unit_result < 2; unit_result++) {
        XrSemanticPlan *semantic = build_lowered_coroutine_direct_call(unit_result != 0);
        XrTargetPlan *plan = NULL;
        char error[512] = {0};
        REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
        REQUIRE(plan != NULL && plan->calls_count == 1 && plan->coroutines_count == 2);
        const XrTargetCallRecord *call = &plan->calls[0];
        REQUIRE(call->flags == XR_TARGET_CALL_SUSPEND &&
                call->caller_storage_slot == XR_SEMANTIC_INDEX_NONE);
        const XrTargetCoroutineStateRecord *call_state = NULL;
        const XrTargetCoroutineStateRecord *yield_state = NULL;
        for (uint32_t i = 0; i < plan->coroutines_count; i++) {
            const XrTargetCoroutineStateRecord *state = &plan->coroutines[i];
            REQUIRE(state->id == i && state->logical_state != 0 &&
                    state->resume_predecessor == state->suspend_block &&
                    state->resume_predecessor_ordinal == 0);
            if (state->direct_call == 0)
                call_state = state;
            else {
                REQUIRE(state->direct_call == XR_SEMANTIC_INDEX_NONE);
                yield_state = state;
            }
        }
        REQUIRE(call_state != NULL && yield_state != NULL &&
                call_state->result_slot == call->result_slot &&
                (call_state->flags & XR_TARGET_COROUTINE_DIRECT_CHILD) != 0);
        if (unit_result) {
            REQUIRE(call->result_slot == XR_SEMANTIC_INDEX_NONE &&
                    call_state->flags == XR_TARGET_COROUTINE_DIRECT_CHILD);
        } else {
            REQUIRE(call->result_slot != XR_SEMANTIC_INDEX_NONE &&
                    call_state->flags ==
                        (XR_TARGET_COROUTINE_DIRECT_CHILD | XR_TARGET_COROUTINE_RESULT_SLOT_BOUND));
        }
        REQUIRE(yield_state->result_slot == XR_SEMANTIC_INDEX_NONE && yield_state->flags == 0);
        uint32_t cursor = 0;
        for (uint32_t function = 0; function < plan->functions_count; function++) {
            REQUIRE(plan->functions[function].coroutine_begin == cursor);
            cursor += plan->functions[function].coroutine_count;
        }
        REQUIRE(cursor == plan->coroutines_count);

        uint8_t *encoded = NULL;
        size_t encoded_size = 0;
        XrXtpCandidate *candidate = NULL;
        XrTargetPlan *decoded = NULL;
        REQUIRE(xr_xtp_encode_plan(plan, &encoded, &encoded_size, error, sizeof(error)));
        REQUIRE(xr_xtp_decode_candidate(encoded, encoded_size, &candidate, error, sizeof(error)));
        REQUIRE(xr_xtp_materialize_target_plan(candidate, semantic, profile, &decoded, error,
                                               sizeof(error)));
        REQUIRE(decoded != NULL && decoded->coroutines_count == 2 &&
                xr_fingerprint_equal(decoded->fingerprint, plan->fingerprint));
        xr_target_plan_free(decoded);
        xr_xtp_candidate_release(candidate);
        xr_xtp_encoded_free(encoded);

        if (!unit_result) {
            XrTargetCoroutineStateRecord *mutable_state = &plan->coroutines[call_state->id];
            uint32_t saved_u32 = mutable_state->semantic_entity;
            mutable_state->semantic_entity = (uint32_t) xr_semantic_plan_entity_count(semantic);
            expect_verify_failure(plan, "XR_CORO_4000");
            mutable_state->semantic_entity = saved_u32;
            saved_u32 = mutable_state->semantic_operation;
            mutable_state->semantic_operation = XR_SEMANTIC_INDEX_NONE;
            expect_verify_failure(plan, "XR_CORO_4000");
            mutable_state->semantic_operation = saved_u32;
            saved_u32 = mutable_state->direct_call;
            mutable_state->direct_call = XR_SEMANTIC_INDEX_NONE;
            expect_verify_failure(plan, "XR_CORO_4000");
            mutable_state->direct_call = saved_u32;
            saved_u32 = mutable_state->result_slot;
            mutable_state->result_slot = XR_SEMANTIC_INDEX_NONE;
            expect_verify_failure(plan, "XR_CORO_4000");
            mutable_state->result_slot = saved_u32;
            mutable_state->resume_predecessor_ordinal = 1;
            expect_verify_failure(plan, "XR_CORO_4000");
            mutable_state->resume_predecessor_ordinal = 0;
            uint16_t saved_flags = mutable_state->flags;
            mutable_state->flags = 0;
            expect_verify_failure(plan, "XR_CORO_4000");
            mutable_state->flags = saved_flags;
            uint32_t saved_coroutine_count = plan->coroutines_count;
            plan->coroutines_count--;
            expect_verify_failure(plan, "XR_CORO_4000");
            plan->coroutines_count = saved_coroutine_count;
            XrTargetCoroutineStateRecord *saved_coroutines = plan->coroutines;
            XrTargetCoroutineStateRecord extra_coroutines[3] = {
                plan->coroutines[0],
                plan->coroutines[1],
                plan->coroutines[0],
            };
            extra_coroutines[2].id = 2;
            plan->coroutines = extra_coroutines;
            plan->coroutines_count = 3;
            expect_verify_failure(plan, "XR_CORO_4000");
            plan->coroutines = saved_coroutines;
            plan->coroutines_count = saved_coroutine_count;
            uint32_t saved_entity = plan->coroutines[1].semantic_entity;
            plan->coroutines[1].semantic_entity = plan->coroutines[0].semantic_entity;
            expect_verify_failure(plan, "XR_CORO_4000");
            plan->coroutines[1].semantic_entity = saved_entity;
            uint32_t state_function = mutable_state->function;
            uint32_t saved_function_count = plan->functions[state_function].coroutine_count;
            plan->functions[state_function].coroutine_count--;
            expect_verify_failure(plan, "XR_CORO_4000");
            plan->functions[state_function].coroutine_count = saved_function_count;
            plan->calls[0].caller_storage_slot = plan->calls[0].result_slot;
            expect_verify_failure(plan, "XR_TARGET_1003");
            plan->calls[0].caller_storage_slot = XR_SEMANTIC_INDEX_NONE;
            plan->calls[0].flags = 0;
            expect_verify_failure(plan, "XR_TARGET_1003");
            plan->calls[0].flags = XR_TARGET_CALL_SUSPEND;
            REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
        }
        xr_target_plan_free(plan);
        xr_semantic_plan_free(semantic);
    }

    XrSemanticPlan *sync_semantic = build_lowered_coroutine_with_sync_call();
    XrTargetPlan *sync_plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(sync_semantic, profile, &sync_plan, error, sizeof(error)));
    REQUIRE(sync_plan->calls_count == 1 && sync_plan->calls[0].flags == 0 &&
            sync_plan->coroutines_count == 1 &&
            sync_plan->coroutines[0].direct_call == XR_SEMANTIC_INDEX_NONE);
    xr_target_plan_free(sync_plan);
    xr_semantic_plan_free(sync_semantic);

    XrSemanticPlan *tail_semantic = build_lowered_tail_coroutine_chain();
    XrTargetPlan *tail_plan = NULL;
    REQUIRE(xr_target_plan_build(tail_semantic, profile, &tail_plan, error, sizeof(error)));
    REQUIRE(tail_plan->calls_count == 2 && tail_plan->coroutines_count == 2);
    const XrTargetCallRecord *tail_call = NULL;
    const XrTargetCallRecord *suspend_call = NULL;
    for (uint32_t i = 0; i < tail_plan->calls_count; i++) {
        if ((tail_plan->calls[i].flags & XR_TARGET_CALL_TAIL) != 0)
            tail_call = &tail_plan->calls[i];
        if ((tail_plan->calls[i].flags & XR_TARGET_CALL_SUSPEND) != 0)
            suspend_call = &tail_plan->calls[i];
    }
    REQUIRE(tail_call != NULL && suspend_call != NULL && tail_call->flags == XR_TARGET_CALL_TAIL &&
            tail_plan->functions[tail_call->caller_function].coroutine_count == 0);
    char tail_hex[XR_FINGERPRINT_BYTES * 2 + 1];
    xr_fingerprint_hex(tail_call->fingerprint, tail_hex);
    REQUIRE(strcmp(tail_hex,
                   "c2470b8063a95c334bb7d050b1304d5d1a9680f7891d27dbff02ceb9016b08d8") == 0);
    uint32_t tail_id = tail_call->id;
    tail_plan->calls[tail_id].flags = 0;
    expect_verify_failure(tail_plan, "XR_TARGET_1003");
    tail_plan->calls[tail_id].flags = XR_TARGET_CALL_TAIL;
    REQUIRE(xr_target_plan_verify(tail_plan, error, sizeof(error)));
    xr_target_plan_free(tail_plan);
    xr_semantic_plan_free(tail_semantic);
    xr_target_profile_free(profile);
}

static void test_direct_local_future_storage_fails_closed(void) {
    XrTargetProfile *profile = build_profile(0);
    char error[512] = {0};
    XrTargetPlan *plan = NULL;
    XrSemanticPlan *aggregate = build_direct_local_aggregate_call();
    error[0] = '\0';
    REQUIRE(!xr_target_plan_build(aggregate, profile, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL && strncmp(error, "XR_TARGET_1003", 14) == 0);
    xr_semantic_plan_free(aggregate);
    xr_target_profile_free(profile);
}

static void test_stringbuilder_constructor_call_authority(void) {
    XrSemanticPlan *semantic = build_stringbuilder_constructor_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "StringBuilder TargetPlan failed: %s\n", error);
    REQUIRE(built);
    const XrSemanticOperationRecord *operation = NULL;
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (candidate && candidate->opcode == XI_CALL_BUILTIN) {
            operation = candidate;
            operation_index = i;
        }
    }
    REQUIRE(operation && plan->calls_count == 1);
    XrTargetCallRecord *call = &plan->calls[0];
    REQUIRE(call->semantic_operation == operation_index &&
            call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
            call->result_value == operation->result_value && call->argument_count == 0 &&
            call->flags == 0 && call->result_ownership == XR_TARGET_CALL_RETURN_OWNED &&
            call->calling_convention == XR_TARGET_CALL_CONVENTION_STRINGBUILDER_CONSTRUCTOR &&
            call->target_kind == XR_TARGET_CALL_TARGET_STRINGBUILDER_CONSTRUCTOR);
    const XrTargetValueRepRecord *result = xr_target_plan_value_rep(plan, operation->result_value);
    REQUIRE(result && result->slot == call->result_slot &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
            plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED);

    XrStableId saved_identity = call->identity;
    call->identity.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->identity = saved_identity;
    uint8_t saved_convention = call->calling_convention;
    call->calling_convention = XR_TARGET_CALL_CONVENTION_CHANNEL_CLOSE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->calling_convention = saved_convention;
    uint8_t saved_kind = call->target_kind;
    call->target_kind = XR_TARGET_CALL_TARGET_CHANNEL_CLOSE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->target_kind = saved_kind;
    uint8_t saved_ownership = call->result_ownership;
    call->result_ownership = XR_TARGET_CALL_NONE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->result_ownership = saved_ownership;
    uint16_t saved_rep = call->result_register_rep;
    call->result_register_rep = call->error_register_rep;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->result_register_rep = saved_rep;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_array_intrinsic_call_authority(void) {
    XrSemanticPlan *semantic = build_array_intrinsic_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error,
                                      sizeof(error));
    if (!built)
        fprintf(stderr, "Array intrinsic TargetPlan failed: %s\n", error);
    REQUIRE(built && plan != NULL && plan->calls_count == 2 &&
            plan->call_arguments_count == 3);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    XrTargetCallRecord *with_capacity = NULL;
    XrTargetCallRecord *filled = NULL;
    for (uint32_t i = 0; i < plan->calls_count; i++) {
        XrTargetCallRecord *call = &plan->calls[i];
        REQUIRE(call->calling_convention ==
                    XR_TARGET_CALL_CONVENTION_ARRAY_INTRINSIC &&
                call->target_kind == XR_TARGET_CALL_TARGET_ARRAY_INTRINSIC &&
                call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                call->result_mode == XR_TARGET_CALL_VALUE &&
                call->result_ownership == XR_TARGET_CALL_RETURN_OWNED &&
                call->adapter_count == 0 && call->flags == 0);
        if (call->array_intrinsic_kind ==
            XR_TARGET_ARRAY_INTRINSIC_WITH_CAPACITY)
            with_capacity = call;
        else if (call->array_intrinsic_kind ==
                 XR_TARGET_ARRAY_INTRINSIC_FILLED_NEW)
            filled = call;
    }
    REQUIRE(with_capacity != NULL && filled != NULL &&
            with_capacity->array_element_storage ==
                XR_TARGET_ARRAY_STORAGE_U8 &&
            with_capacity->argument_count == 1 &&
            filled->array_element_storage == XR_TARGET_ARRAY_STORAGE_U8 &&
            filled->argument_count == 2);
    for (uint32_t i = 0; i < plan->call_arguments_count; i++) {
        XrTargetCallArgumentRecord *argument = &plan->call_arguments[i];
        REQUIRE(argument->callee_parameter == XR_SEMANTIC_INDEX_NONE &&
                argument->callee_slot == XR_SEMANTIC_INDEX_NONE &&
                argument->mode == XR_TARGET_CALL_VALUE &&
                argument->ownership == XR_TARGET_CALL_CONSUME &&
                argument->flags == 0);
    }

    uint8_t saved_kind = with_capacity->array_intrinsic_kind;
    with_capacity->array_intrinsic_kind =
        XR_TARGET_ARRAY_INTRINSIC_FILLED_NEW;
    expect_verify_failure(plan, "XR_TARGET_1003");
    with_capacity->array_intrinsic_kind = saved_kind;

    uint8_t saved_storage = with_capacity->array_element_storage;
    with_capacity->array_element_storage = XR_TARGET_ARRAY_STORAGE_I64;
    expect_verify_failure(plan, "XR_TARGET_1003");
    with_capacity->array_element_storage = saved_storage;

    XrTargetCallArgumentRecord *first =
        &plan->call_arguments[with_capacity->argument_begin];
    uint16_t saved_ordinal = first->ordinal;
    first->ordinal = 1;
    expect_verify_failure(plan, "XR_TARGET_1003");
    first->ordinal = saved_ordinal;

    uint8_t saved_ownership = first->ownership;
    first->ownership = XR_TARGET_CALL_READ;
    expect_verify_failure(plan, "XR_TARGET_1003");
    first->ownership = saved_ownership;

    XrStableId saved_identity = first->identity;
    first->identity.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1003");
    first->identity = saved_identity;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(
        plan, xr_target_profile_fingerprint(profile), &emission, error,
        sizeof(error)));
    XrCValueEmissionView with_capacity_view = {0};
    XrCValueEmissionView filled_view = {0};
    REQUIRE(xr_c_emission_plan_value_view(
                emission, with_capacity->result_value, &with_capacity_view,
                error, sizeof(error)) &&
            xr_c_emission_plan_value_view(
                emission, filled->result_value, &filled_view, error,
                sizeof(error)));
    const XrSemanticOperationRecord *with_capacity_operation =
        xr_semantic_plan_operation(semantic,
                                   with_capacity->semantic_operation);
    const XrSemanticOperationRecord *filled_operation =
        xr_semantic_plan_operation(semantic, filled->semantic_operation);
    uint32_t semantic_operand_count = 0;
    const XrSemanticOperandRecord *semantic_operands =
        xr_semantic_plan_operands(semantic, &semantic_operand_count);
    REQUIRE(with_capacity_operation && filled_operation && semantic_operands &&
            with_capacity_operation->operand_begin < semantic_operand_count &&
            filled_operation->operand_begin + 1u < semantic_operand_count &&
            with_capacity_view.rep == XR_C_VALUE_REP_TAGGED &&
            with_capacity_view.materialization ==
                XR_C_VALUE_MATERIALIZATION_ARRAY_WITH_CAPACITY &&
            with_capacity_view.recipe_discriminant ==
                XR_TARGET_ARRAY_STORAGE_U8 &&
            with_capacity_view.recipe_operand_value ==
                semantic_operands[with_capacity_operation->operand_begin].value &&
            with_capacity_view.recipe_argument_value == UINT32_MAX &&
            with_capacity_view.recipe_symbol &&
            strcmp(with_capacity_view.recipe_symbol,
                   "xrt_array_with_capacity_value") == 0 &&
            filled_view.rep == XR_C_VALUE_REP_TAGGED &&
            filled_view.materialization ==
                XR_C_VALUE_MATERIALIZATION_ARRAY_FILLED_NEW &&
            filled_view.recipe_discriminant ==
                XR_TARGET_ARRAY_STORAGE_U8 &&
            filled_view.recipe_operand_value ==
                semantic_operands[filled_operation->operand_begin].value &&
            filled_view.recipe_argument_value ==
                semantic_operands[filled_operation->operand_begin + 1u].value &&
            filled_view.recipe_symbol &&
            strcmp(filled_view.recipe_symbol,
                   "xrt_array_new_filled_value") == 0);
    xr_c_emission_plan_free(emission);

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static XrTargetCallRecord *find_call_by_convention(XrTargetPlan *plan, uint8_t convention) {
    XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; i < plan->calls_count; i++) {
        if (plan->calls[i].calling_convention != convention)
            continue;
        REQUIRE(match == NULL);
        match = &plan->calls[i];
    }
    REQUIRE(match != NULL);
    return match;
}

/* The shared call block and the per-family branch state the same ownership
 * fact about a StringBuilder method result. When they disagree the shared
 * block breaks out first, the per-family branch becomes dead code, and no
 * plan carrying the row can ever verify — so each family asserts that the
 * builder's row verifies as written and that RETURN_OWNED is load-bearing. */
static XrTargetCallRecord *expect_owned_dynamic_stringbuilder_call(
    XrTargetPlan *plan, uint8_t convention, uint8_t target_kind) {
    char error[512] = {0};
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    XrTargetCallRecord *call = find_call_by_convention(plan, convention);
    const XrTargetValueRepRecord *result = xr_target_plan_value_rep(plan, call->result_value);
    REQUIRE(result && result->slot == call->result_slot &&
            call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
            call->callee_function == XR_SEMANTIC_INDEX_NONE && call->argument_count == 0 &&
            call->flags == 0 && call->result_mode == XR_TARGET_CALL_VALUE &&
            call->result_ownership == XR_TARGET_CALL_RETURN_OWNED &&
            call->target_kind == target_kind &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[result->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
            plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED);

    uint8_t saved_ownership = call->result_ownership;
    call->result_ownership = XR_TARGET_CALL_NONE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->result_ownership = XR_TARGET_CALL_BORROW;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->result_ownership = saved_ownership;
    /* Identity, target kind and convention live only in the per-family branch
     * behind the shared `break`; rejecting them proves that branch still runs
     * rather than having been stranded by a disagreeing shared block. */
    XrStableId saved_identity = call->identity;
    call->identity.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->identity = saved_identity;
    uint8_t saved_kind = call->target_kind;
    call->target_kind = XR_TARGET_CALL_TARGET_CHANNEL_CLOSE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->target_kind = saved_kind;
    uint8_t saved_convention = call->calling_convention;
    call->calling_convention = XR_TARGET_CALL_CONVENTION_CHANNEL_CLOSE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->calling_convention = saved_convention;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    return call;
}

static void test_stringbuilder_append_rune_call_authority(void) {
    XrSemanticPlan *semantic = build_stringbuilder_append_rune_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "StringBuilder.append(rune) TargetPlan failed: %s\n", error);
    REQUIRE(built && plan);
    expect_owned_dynamic_stringbuilder_call(
        plan, XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_RUNE,
        XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_RUNE);
    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_stringbuilder_to_string_call_authority(void) {
    XrSemanticPlan *semantic = build_stringbuilder_to_string_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "StringBuilder.toString TargetPlan failed: %s\n", error);
    REQUIRE(built && plan);
    expect_owned_dynamic_stringbuilder_call(
        plan, XR_TARGET_CALL_CONVENTION_STRINGBUILDER_TO_STRING,
        XR_TARGET_CALL_TARGET_STRINGBUILDER_TO_STRING);
    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_stringbuilder_append_string_call_authority(void) {
    XrSemanticPlan *semantic = build_stringbuilder_append_string_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "StringBuilder.append(string) TargetPlan failed: %s\n", error);
    REQUIRE(built && plan);
    expect_owned_dynamic_stringbuilder_call(
        plan, XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_STRING,
        XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_STRING);
    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_string_runes_call_authority(void) {
    XrSemanticPlan *semantic = build_string_runes_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error,
                                      sizeof(error));
    if (!built)
        fprintf(stderr, "String.runes TargetPlan failed: %s\n", error);
    REQUIRE(built && plan &&
            (plan->completed_family_mask &
             XR_TARGET_FAMILY_STRING_RUNES_RESULT_STORAGE) != 0);
    XrTargetCallRecord *call = find_call_by_convention(
        plan, XR_TARGET_CALL_CONVENTION_STRING_RUNES);
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(semantic, call->semantic_operation);
    const XrTargetValueRepRecord *result =
        operation ? xr_target_plan_value_rep(plan, operation->result_value) : NULL;
    REQUIRE(operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_STRING_RUNES &&
            call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
            call->callee_function == XR_SEMANTIC_INDEX_NONE &&
            call->result_value == operation->result_value && call->argument_count == 0 &&
            call->flags == 0 && call->result_mode == XR_TARGET_CALL_VALUE &&
            call->result_ownership == XR_TARGET_CALL_RETURN_OWNED &&
            call->target_kind == XR_TARGET_CALL_TARGET_STRING_RUNES && result &&
            result->slot == call->result_slot &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[result->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
            plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED);

    XrStableId saved_identity = call->identity;
    call->identity.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->identity = saved_identity;
    uint8_t saved_convention = call->calling_convention;
    call->calling_convention = XR_TARGET_CALL_CONVENTION_CHANNEL_CLOSE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->calling_convention = saved_convention;
    uint8_t saved_kind = call->target_kind;
    call->target_kind = XR_TARGET_CALL_TARGET_CHANNEL_CLOSE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->target_kind = saved_kind;
    uint8_t saved_ownership = call->result_ownership;
    call->result_ownership = XR_TARGET_CALL_BORROW;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->result_ownership = saved_ownership;
    uint8_t saved_slot_ownership = plan->slots[result->slot].ownership;
    plan->slots[result->slot].ownership = XR_TARGET_OWNERSHIP_BORROWED;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[result->slot].ownership = saved_slot_ownership;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_iterator_rune_has_next_call_authority(void) {
    XrSemanticPlan *semantic = build_iterator_rune_has_next_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error,
                                 sizeof(error)) && plan);
    XrTargetCallRecord *call = find_call_by_convention(
        plan, XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_HAS_NEXT);
    const XrSemanticOperationRecord *operation =
        call ? xr_semantic_plan_operation(semantic, call->semantic_operation) : NULL;
    const XrTargetValueRepRecord *result =
        operation ? xr_target_plan_value_rep(plan, operation->result_value) : NULL;
    REQUIRE(operation &&
            operation->intrinsic_kind == XR_SEM_INTRINSIC_ITERATOR_RUNE_HAS_NEXT &&
            call->target_kind == XR_TARGET_CALL_TARGET_ITERATOR_RUNE_HAS_NEXT &&
            call->result_ownership == XR_TARGET_CALL_NONE &&
            call->result_mode == XR_TARGET_CALL_VALUE && call->argument_count == 0 &&
            result && plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_I1 &&
            plan->machine_reps[result->memory_rep].kind == XR_MACHINE_REP_I1 &&
            plan->slots[result->slot].root_kind == XR_TARGET_ROOT_NONE &&
            plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_TRIVIAL);

    XrStableId saved_identity = call->identity;
    call->identity.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->identity = saved_identity;
    uint8_t saved_kind = call->target_kind;
    call->target_kind = XR_TARGET_CALL_TARGET_STRING_RUNES;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->target_kind = saved_kind;
    uint8_t saved_ownership = call->result_ownership;
    call->result_ownership = XR_TARGET_CALL_BORROW;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->result_ownership = saved_ownership;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_iterator_rune_next_call_authority(void) {
    XrSemanticPlan *semantic = build_iterator_rune_next_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error,
                                 sizeof(error)) && plan);
    XrTargetCallRecord *call = find_call_by_convention(
        plan, XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_NEXT);
    const XrSemanticOperationRecord *operation =
        call ? xr_semantic_plan_operation(semantic, call->semantic_operation) : NULL;
    const XrTargetValueRepRecord *result =
        operation ? xr_target_plan_value_rep(plan, operation->result_value) : NULL;
    REQUIRE(operation &&
            operation->intrinsic_kind == XR_SEM_INTRINSIC_ITERATOR_RUNE_NEXT &&
            call->target_kind == XR_TARGET_CALL_TARGET_ITERATOR_RUNE_NEXT &&
            call->result_ownership == XR_TARGET_CALL_NONE &&
            call->result_mode == XR_TARGET_CALL_VALUE && call->argument_count == 0 &&
            result &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_RUNE &&
            plan->machine_reps[result->memory_rep].kind == XR_MACHINE_REP_RUNE &&
            plan->slots[result->slot].root_kind == XR_TARGET_ROOT_NONE &&
            plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_TRIVIAL);

    XrStableId saved_identity = call->identity;
    call->identity.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->identity = saved_identity;
    uint8_t saved_kind = call->target_kind;
    call->target_kind = XR_TARGET_CALL_TARGET_ITERATOR_RUNE_HAS_NEXT;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->target_kind = saved_kind;
    uint8_t saved_convention = call->calling_convention;
    call->calling_convention = XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_HAS_NEXT;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->calling_convention = saved_convention;
    uint8_t saved_ownership = call->result_ownership;
    call->result_ownership = XR_TARGET_CALL_BORROW;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->result_ownership = saved_ownership;
    uint8_t saved_root = plan->slots[result->slot].root_kind;
    plan->slots[result->slot].root_kind = XR_TARGET_ROOT_DYNAMIC;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[result->slot].root_kind = saved_root;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    XrSemanticOperationRecord *runes_operation = NULL;
    for (uint32_t i = 0; i < semantic->operation_count; i++)
        if (semantic->operations[i].intrinsic_kind == XR_SEM_INTRINSIC_STRING_RUNES)
            runes_operation = &semantic->operations[i];
    REQUIRE(runes_operation != NULL);
    uint8_t saved_intrinsic = runes_operation->intrinsic_kind;
    runes_operation->intrinsic_kind = XR_SEM_INTRINSIC_NONE;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    runes_operation->intrinsic_kind = saved_intrinsic;
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_rune_to_uint32_call_authority(void) {
    XrSemanticPlan *semantic = build_rune_to_uint32_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error,
                                 sizeof(error)) && plan);
    XrTargetCallRecord *call = find_call_by_convention(
        plan, XR_TARGET_CALL_CONVENTION_RUNE_TO_UINT32);
    const XrSemanticOperationRecord *operation =
        call ? xr_semantic_plan_operation(semantic, call->semantic_operation) : NULL;
    const XrTargetValueRepRecord *result =
        operation ? xr_target_plan_value_rep(plan, operation->result_value) : NULL;
    REQUIRE(operation &&
            operation->intrinsic_kind == XR_SEM_INTRINSIC_RUNE_TO_UINT32 &&
            call->target_kind == XR_TARGET_CALL_TARGET_RUNE_TO_UINT32 &&
            call->result_ownership == XR_TARGET_CALL_NONE &&
            call->result_mode == XR_TARGET_CALL_VALUE && call->argument_count == 0 &&
            result &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_U32 &&
            plan->machine_reps[result->memory_rep].kind == XR_MACHINE_REP_U32 &&
            plan->slots[result->slot].root_kind == XR_TARGET_ROOT_NONE &&
            plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_TRIVIAL);

    XrStableId saved_identity = call->identity;
    call->identity.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->identity = saved_identity;
    uint8_t saved_kind = call->target_kind;
    call->target_kind = XR_TARGET_CALL_TARGET_ITERATOR_RUNE_NEXT;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->target_kind = saved_kind;
    uint8_t saved_convention = call->calling_convention;
    call->calling_convention = XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_NEXT;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->calling_convention = saved_convention;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    XrSemanticOperationRecord *next_operation = NULL;
    for (uint32_t i = 0; i < semantic->operation_count; i++)
        if (semantic->operations[i].intrinsic_kind ==
            XR_SEM_INTRINSIC_ITERATOR_RUNE_NEXT)
            next_operation = &semantic->operations[i];
    REQUIRE(next_operation != NULL);
    uint8_t saved_intrinsic = next_operation->intrinsic_kind;
    next_operation->intrinsic_kind = XR_SEM_INTRINSIC_NONE;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    next_operation->intrinsic_kind = saved_intrinsic;
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_rune_is_whitespace_call_authority(void) {
    XrSemanticPlan *semantic = build_rune_is_whitespace_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error,
                                 sizeof(error)) && plan);
    XrTargetCallRecord *call = find_call_by_convention(
        plan, XR_TARGET_CALL_CONVENTION_RUNE_IS_WHITESPACE);
    const XrSemanticOperationRecord *operation =
        call ? xr_semantic_plan_operation(semantic, call->semantic_operation) : NULL;
    const XrTargetValueRepRecord *result =
        operation ? xr_target_plan_value_rep(plan, operation->result_value) : NULL;
    REQUIRE(operation &&
            operation->intrinsic_kind == XR_SEM_INTRINSIC_RUNE_IS_WHITESPACE &&
            call->target_kind == XR_TARGET_CALL_TARGET_RUNE_IS_WHITESPACE &&
            call->result_ownership == XR_TARGET_CALL_NONE &&
            call->result_mode == XR_TARGET_CALL_VALUE && call->argument_count == 0 &&
            result &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_I1 &&
            plan->machine_reps[result->memory_rep].kind == XR_MACHINE_REP_I1 &&
            plan->slots[result->slot].root_kind == XR_TARGET_ROOT_NONE &&
            plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_TRIVIAL);

    XrStableId saved_identity = call->identity;
    call->identity.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->identity = saved_identity;
    uint8_t saved_kind = call->target_kind;
    call->target_kind = XR_TARGET_CALL_TARGET_RUNE_TO_UINT32;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->target_kind = saved_kind;
    uint8_t saved_convention = call->calling_convention;
    call->calling_convention = XR_TARGET_CALL_CONVENTION_RUNE_TO_UINT32;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->calling_convention = saved_convention;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    XrSemanticOperationRecord *next_operation = NULL;
    for (uint32_t i = 0; i < semantic->operation_count; i++)
        if (semantic->operations[i].intrinsic_kind ==
            XR_SEM_INTRINSIC_ITERATOR_RUNE_NEXT)
            next_operation = &semantic->operations[i];
    REQUIRE(next_operation != NULL);
    uint8_t saved_intrinsic = next_operation->intrinsic_kind;
    next_operation->intrinsic_kind = XR_SEM_INTRINSIC_NONE;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    next_operation->intrinsic_kind = saved_intrinsic;
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_string_byte_slice_view_target_authority(void) {
    XrSemanticPlan *semantic = build_string_byte_slice_view_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "string byte-slice TargetPlan failed: %s\n", error);
    REQUIRE(built && plan &&
            (plan->completed_family_mask &
             XR_TARGET_FAMILY_STRING_BYTE_SLICE_VIEW_STORAGE) != 0);
    const XrSemanticOperationRecord *operation = NULL;
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *candidate =
            xr_semantic_plan_operation(semantic, i);
        if (candidate && candidate->intrinsic_kind ==
            XR_SEM_INTRINSIC_STRING_BYTE_SLICE_VIEW) {
            operation = candidate;
            operation_index = i;
        }
    }
    REQUIRE(operation && plan->calls_count == 1);
    XrTargetCallRecord *call = &plan->calls[0];
    const XrTargetValueRepRecord *result =
        xr_target_plan_value_rep(plan, operation->result_value);
    REQUIRE(result && call->semantic_operation == operation_index &&
            call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
            call->result_value == operation->result_value && call->argument_count == 0 &&
            call->flags == 0 && call->result_ownership == XR_TARGET_CALL_BORROW &&
            call->calling_convention ==
                XR_TARGET_CALL_CONVENTION_STRING_BYTE_SLICE_VIEW &&
            call->target_kind == XR_TARGET_CALL_TARGET_STRING_BYTE_SLICE_VIEW &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_VIEW &&
            plan->machine_reps[result->memory_rep].kind == XR_MACHINE_REP_VIEW &&
            plan->slots[result->slot].root_kind == XR_TARGET_ROOT_VIEW_OWNER &&
            plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_BORROWED);
    XrStableId saved_identity = call->identity;
    call->identity.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->identity = saved_identity;
    uint8_t saved_convention = call->calling_convention;
    call->calling_convention = XR_TARGET_CALL_CONVENTION_STRINGBUILDER_CONSTRUCTOR;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->calling_convention = saved_convention;
    uint8_t saved_kind = call->target_kind;
    call->target_kind = XR_TARGET_CALL_TARGET_STRINGBUILDER_CONSTRUCTOR;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->target_kind = saved_kind;
    uint8_t saved_ownership = call->result_ownership;
    call->result_ownership = XR_TARGET_CALL_RETURN_OWNED;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->result_ownership = saved_ownership;
    uint16_t saved_rep = call->result_register_rep;
    call->result_register_rep = call->error_register_rep;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->result_register_rep = saved_rep;
    uint8_t saved_root = plan->slots[result->slot].root_kind;
    plan->slots[result->slot].root_kind = XR_TARGET_ROOT_NONE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[result->slot].root_kind = saved_root;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_structural_mutations_fail_closed(void) {
    XrSemanticPlan *semantic = build_semantic_plan();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = build_target_plan(semantic, profile);
    plan->schema_version++;
    expect_verify_failure(plan, "XR_ARTIFACT_2000");
    plan->schema_version--;

    plan->machine_reps[1].memory_size = 7;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[1].memory_size = 8;
    plan->machine_reps[1].kind = UINT16_MAX;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[1].kind = XR_MACHINE_REP_I64;

    XrTargetMachineRepRecord saved_rep2 = plan->machine_reps[2];
    plan->machine_reps[1].legal_conversion_mask[0] = 0;
    plan->machine_reps[2] = (XrTargetMachineRepRecord) {
        .id = 2,
        .kind = XR_MACHINE_REP_VECTOR,
        .register_bits = 128,
        .memory_size = 16,
        .memory_align = 16,
        .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
        .detail = 1,
        .lane_count = 2,
    };
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    REQUIRE(xr_target_plan_verify(plan, NULL, 0));
    plan->machine_reps[2].root_kind = XR_TARGET_ROOT_OBJECT;
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[2].root_kind = XR_TARGET_ROOT_NONE;
    plan->machine_reps[2].ownership = XR_TARGET_OWNERSHIP_SHARED;
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[2].ownership = XR_TARGET_OWNERSHIP_TRIVIAL;
    plan->machine_reps[2].null_encoding = XR_TARGET_NULL_TAGGED;
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[2].null_encoding = XR_TARGET_NULL_NOT_NULLABLE;
    plan->machine_reps[2].lane_count = 3;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[2].lane_count = 4;
    plan->machine_reps[2].register_bits = 256;
    plan->machine_reps[2].memory_size = 32;
    plan->machine_reps[2].memory_align = 32;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[2] = saved_rep2;
    plan->machine_reps[1].legal_conversion_mask[0] = UINT64_C(1) << 2;
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);

    plan->extents[0].operand_count = 1;
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->extents[0].operand_count = 0;
    plan->extents[0].kind = UINT8_MAX;
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->extents[0].kind = XR_TARGET_EXTENT_FIXED;

    plan->layouts[0].kind = UINT8_MAX;
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->layouts[0].kind = XR_TARGET_LAYOUT_SCALAR;

    XrTargetFieldRecord fabricated_field = {
        .layout = 0,
        .size = 8,
        .align = 4,
        .memory_rep = 1,
    };
    plan->fields = &fabricated_field;
    plan->fields_count = 1;
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->fields_count = 0;
    plan->fields = NULL;

    plan->slots[1].offset = 0;
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->slots[1].offset = 8;
    plan->slots[0].offset = UINT32_MAX - 7u;
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->slots[0].offset = 0;

    XrTargetStorageRecord fabricated_storage = {0};
    XrTargetAllocationRecord fabricated_allocation = {0};
    XrTargetExtentOperandRecord fabricated_extent_operand = {0};
    fabricated_storage.domain.bytes[0] = 1;
    plan->storage = &fabricated_storage;
    plan->allocations = &fabricated_allocation;
    plan->extent_operands = &fabricated_extent_operand;
    plan->storage_count = 1;
    plan->allocations_count = 1;
    plan->extent_operands_count = 1;
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->storage_count = 0;
    plan->allocations_count = 0;
    plan->extent_operands_count = 0;
    plan->storage = NULL;
    plan->allocations = NULL;
    plan->extent_operands = NULL;

    XrTargetCallRecord fabricated_call = {.semantic_operation = 0, .callee_function = 1};
    XrTargetCallArgumentRecord fabricated_argument = {0};
    plan->calls = &fabricated_call;
    plan->call_arguments = &fabricated_argument;
    plan->calls_count = 1;
    plan->call_arguments_count = 1;
    expect_verify_failure(plan, "XR_TARGET_1003");
    plan->calls_count = 0;
    plan->call_arguments_count = 0;
    plan->calls = NULL;
    plan->call_arguments = NULL;

    XrTargetRootMapRecord fabricated_root = {.function = 1};
    uint32_t fabricated_root_slot = 0;
    XrTargetCleanupRecord fabricated_cleanup = {.function = 1};
    plan->root_maps = &fabricated_root;
    plan->root_slots = &fabricated_root_slot;
    plan->cleanups = &fabricated_cleanup;
    plan->root_maps_count = 1;
    plan->root_slots_count = 1;
    plan->cleanups_count = 1;
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->root_maps_count = 0;
    plan->root_slots_count = 0;
    plan->cleanups_count = 0;
    plan->root_maps = NULL;
    plan->root_slots = NULL;
    plan->cleanups = NULL;

    XrTargetAdapterRecord fabricated_adapter = {0};
    plan->adapters = &fabricated_adapter;
    plan->adapters_count = 1;
    expect_verify_failure(plan, "XR_TARGET_1003");
    plan->adapters_count = 0;
    plan->adapters = NULL;

    XrTargetCapabilityRecord *saved_capabilities = plan->capabilities;
    uint32_t saved_capability_count = plan->capabilities_count;
    XrTargetCapabilityRecord fabricated_capability = {0};
    plan->capabilities = &fabricated_capability;
    plan->capabilities_count = 1;
    expect_verify_failure(plan, "XR_TARGET_1004");
    plan->capabilities = saved_capabilities;
    plan->capabilities_count = 1;
    expect_verify_failure(plan, "XR_TARGET_1004");
    plan->capabilities_count = saved_capability_count;
    plan->capabilities[0].provider = XR_TARGET_PROVIDER_PANIC;
    expect_verify_failure(plan, "XR_TARGET_1004");
    plan->capabilities[0].provider = XR_TARGET_PROVIDER_ALLOCATOR;
    plan->capabilities[1].flags = 0;
    expect_verify_failure(plan, "XR_TARGET_1004");
    plan->capabilities[1].flags = XR_TARGET_CAPABILITY_REQUIRED;

    XrTargetCoroutineStateRecord fabricated_coroutine = {.function = 1};
    plan->coroutines = &fabricated_coroutine;
    plan->coroutines_count = 1;
    expect_verify_failure(plan, "XR_CORO_4000");
    plan->coroutines_count = 0;
    plan->coroutines = NULL;

    plan->extents[0].flags = XR_TARGET_EXTENT_ZERO;
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->extents[0].flags = 0;
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);

    XrTargetExtentRecord *saved_extents = plan->extents;
    XrTargetExtentRecord extra_extents[2] = {
        saved_extents[0],
        {.id = 1, .kind = XR_TARGET_EXTENT_FIXED, .element_layout = XR_SEMANTIC_INDEX_NONE},
    };
    plan->extents = extra_extents;
    plan->extents_count = 2;
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->extents = saved_extents;
    plan->extents_count = 1;
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);

    plan->semantic_fingerprint.bytes[0] ^= 1;
    expect_verify_failure(plan, "XR_TARGET_1000");
    plan->semantic_fingerprint.bytes[0] ^= 1;
    plan->fingerprint.bytes[0] ^= 1;
    expect_verify_failure_raw(plan, "XR_TARGET_1000");
    plan->fingerprint.bytes[0] ^= 1;

    plan->calls_count = 10000001u;
    expect_verify_failure_raw(plan, "XR_EXEC_5003");
    plan->calls_count = 0;
    plan->frozen = false;
    expect_verify_failure(plan, "XR_EXEC_5000");
    plan->frozen = true;

    REQUIRE(xr_target_plan_verify(plan, NULL, 0));
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_value_rep_mutations_fail_closed(void) {
    XrSemanticPlan *semantic = build_semantic_plan();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = build_target_plan(semantic, profile);
    uint32_t first_slot = plan->value_reps[0].slot;
    uint32_t last_slot = plan->value_reps[2].slot;
    REQUIRE(first_slot < plan->slots_count && last_slot < plan->slots_count &&
            first_slot != last_slot);

    plan->value_reps[2].semantic_value = 4;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[2].semantic_value = 3;
    plan->value_reps[2].semantic_value = 2;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[2].semantic_value = 3;
    plan->value_reps[0].semantic_value = 2;
    plan->value_reps[1].semantic_value = 0;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[0].semantic_value = 0;
    plan->value_reps[1].semantic_value = 2;
    plan->value_reps[1].semantic_value = 1;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[1].semantic_value = 2;
    plan->value_reps[0].register_rep = plan->machine_reps_count;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[0].register_rep = 1;
    plan->machine_reps[1].legal_conversion_mask[0] = 0;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[1].legal_conversion_mask[0] = UINT64_C(1) << 2;
    plan->machine_reps[2].legal_conversion_mask[0] = 0;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[2].legal_conversion_mask[0] = UINT64_C(1) << 1;
    plan->value_reps[0].register_rep = 0;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[0].register_rep = 1;
    plan->value_reps_count = 2;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps_count = 3;
    plan->value_reps[0].slot = XR_SEMANTIC_INDEX_NONE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[0].slot = last_slot;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[0].slot = first_slot;
    plan->slots[first_slot].function = 1;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[first_slot].function = 0;
    plan->slots[first_slot].register_rep = 4;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[first_slot].register_rep = 1;
    plan->slots[first_slot].size = 4;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[first_slot].size = 8;
    plan->slots[first_slot].align = 4;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[first_slot].align = 8;
    plan->value_reps[2].slot = first_slot;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[2].slot = last_slot;
    plan->value_reps[1].slot = first_slot;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[1].slot = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_target_plan_verify(plan, NULL, 0));
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_freeze_rejects_invalid_draft(void) {
    XrSemanticPlan *semantic = build_semantic_plan();
    XrTargetProfile *profile = build_profile(0);
    TargetFixture fixture;
    fill_fixture(&fixture, semantic, profile);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    fixture.draft.completed_family_mask = 0;
    REQUIRE(!xr_target_plan_freeze(&fixture.draft, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL);
    REQUIRE(strncmp(error, "XR_TARGET_1001", strlen("XR_TARGET_1001")) == 0);
    fixture.draft.completed_family_mask = XR_TARGET_REQUIRED_FAMILIES;
    fixture.slots[1].offset = 0;
    REQUIRE(!xr_target_plan_freeze(&fixture.draft, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL);
    REQUIRE(strncmp(error, "XR_TARGET_1002", strlen("XR_TARGET_1002")) == 0);
    fixture.slots[1].offset = 8;
    fixture.draft.value_reps_count = 40000000u;
    fixture.draft.extents_count = 1000000u;
    fixture.draft.layouts_count = 1000000u;
    fixture.draft.fields_count = 16000000u;
    fixture.draft.storage_count = 4000000u;
    fixture.draft.allocations_count = 10000000u;
    fixture.draft.extent_operands_count = 40000000u;
    fixture.draft.functions_count = 100000u;
    fixture.draft.slots_count = 16000000u;
    fixture.draft.calls_count = 10000000u;
    fixture.draft.call_arguments_count = 40000000u;
    fixture.draft.root_maps_count = 10000000u;
    fixture.draft.root_slots_count = 40000000u;
    fixture.draft.cleanups_count = 40000000u;
    fixture.draft.adapters_count = 1000000u;
    fixture.draft.capabilities_count = 65536u;
    fixture.draft.coroutines_count = 10000000u;
    REQUIRE(!xr_target_plan_freeze(&fixture.draft, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL);
    REQUIRE(strncmp(error, "XR_EXEC_5003", strlen("XR_EXEC_5003")) == 0);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_bool_and_nullable_scalar_boundary(void) {
    XrTargetProfile *profile = build_profile(0);
    char error[512] = {0};

    XrSemanticPlan *boolean_semantic = build_single_scalar_semantic(&stub_bool);
    const XrSemanticTypeRecord *boolean_type = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_type_count(boolean_semantic); i++)
        if (xr_semantic_plan_type(boolean_semantic, i)->kind == XR_KIND_BOOL)
            boolean_type = xr_semantic_plan_type(boolean_semantic, i);
    REQUIRE(boolean_type != NULL && boolean_type->scalar_rep == XR_SCALAR_REP_NONE);
    XrTargetPlan *boolean_plan = NULL;
    REQUIRE(freeze_single_scalar(boolean_semantic, profile, true, true, &boolean_plan, error,
                                 sizeof(error)));
    boolean_plan->value_reps[0].memory_rep = 0;
    expect_verify_failure(boolean_plan, "XR_TARGET_1001");
    boolean_plan->value_reps[0].memory_rep = 1;
    REQUIRE(xr_target_plan_verify(boolean_plan, NULL, 0));
    xr_target_plan_free(boolean_plan);
    xr_semantic_plan_free(boolean_semantic);

    XrSemanticPlan *nullable_semantic = build_single_scalar_semantic(&stub_nullable_int);
    const XrSemanticTypeRecord *nullable_type = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_type_count(nullable_semantic); i++)
        if (xr_semantic_plan_type(nullable_semantic, i)->kind == XR_KIND_INT)
            nullable_type = xr_semantic_plan_type(nullable_semantic, i);
    REQUIRE(nullable_type != NULL && (nullable_type->flags & XR_SEM_TYPE_NULLABLE) != 0);
    /* A nullable scalar now carries a storage fact of its own, the tagged
     * carrier, so leaving the value unbound is refused where it once passed.
     * Claiming the payload's native scalar row for it stays refused: the null
     * tag has no spelling in a bare machine word. */
    XrTargetPlan *nullable_plan = NULL;
    REQUIRE(!freeze_single_scalar(nullable_semantic, profile, false, false, &nullable_plan, error,
                                  sizeof(error)));
    REQUIRE(nullable_plan == NULL);
    REQUIRE(strncmp(error, "XR_TARGET_1001", strlen("XR_TARGET_1001")) == 0);
    REQUIRE(!freeze_single_scalar(nullable_semantic, profile, true, false, &nullable_plan, error,
                                  sizeof(error)));
    REQUIRE(nullable_plan == NULL);
    REQUIRE(strncmp(error, "XR_TARGET_1001", strlen("XR_TARGET_1001")) == 0);
    xr_semantic_plan_free(nullable_semantic);
    xr_target_profile_free(profile);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "array-intrinsic-authority") == 0) {
        test_array_intrinsic_call_authority();
        puts("Array intrinsic TargetPlan authority tests passed");
        return 0;
    }
    test_direct_local_raw_pointer_call_authority();
    test_unit_enum_target_rep_mutations();
    test_array_intrinsic_call_authority();
    test_stringbuilder_constructor_call_authority();
    test_stringbuilder_append_rune_call_authority();
    test_stringbuilder_to_string_call_authority();
    test_stringbuilder_append_string_call_authority();
    test_string_runes_call_authority();
    test_iterator_rune_has_next_call_authority();
    test_iterator_rune_next_call_authority();
    test_rune_to_uint32_call_authority();
    test_rune_is_whitespace_call_authority();
    test_string_byte_slice_view_target_authority();
    test_channel_receive_storage_authority();
    test_channel_close_call_authority();
    test_source_export_call_authority();
    test_source_export_call_argument_authority();
    test_profile_freeze_and_determinism();
    test_plan_snapshot_and_determinism();
    test_builder_materializes_canonical_scalar_intents();
    test_builder_materializes_parameter_without_operation();
    test_builder_materializes_effect_void_independent_of_type();
    test_builder_materializes_exact_heap_closure_storage();
    test_builder_materializes_nested_aggregate_family();
    test_builder_materializes_struct_and_named_aggregates();
    test_unknown_call_target_fails_closed();
    test_direct_local_call_adapter_family();
    test_source_instance_method_target_fails_closed();
    test_open_source_instance_method_target_fails_closed();
    test_coroutine_state_call_family();
    test_direct_local_future_storage_fails_closed();
    test_structural_mutations_fail_closed();
    test_value_rep_mutations_fail_closed();
    test_freeze_rejects_invalid_draft();
    test_bool_and_nullable_scalar_boundary();
    printf("TargetProfile/TargetPlan tests passed\n");
    return 0;
}
