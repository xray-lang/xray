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
#include "../../../src/frontend/analyzer/xa_ownership.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/base/xsha256.h"
#include "../../../src/plan/format/xr_xtp_internal.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/semantic/xr_semantic_class_shape.h"
#include "../../../src/plan/semantic/xr_semantic_source_class_field_shape.h"
#include "../../../src/plan/semantic/xr_semantic_source_structural_field_shape.h"
#include "../../../src/plan/semantic/xr_semantic_string_slice_shape.h"
#include "../../../src/plan/semantic/xr_semantic_array_index_shape.h"
#include "../../../src/plan/semantic/xr_semantic_array_type_shape.h"
#include "../../../src/plan/semantic/xr_semantic_array_member_shape.h"
#include "../../../src/plan/semantic/xr_semantic_container_copy_shape.h"
#include "../../../src/plan/semantic/xr_semantic_owner_transfer_shape.h"
#include "../../../src/plan/semantic/xr_semantic_rune_to_string_shape.h"
#include "../../../src/plan/semantic/xr_semantic_value_aggregate_shape.h"
#include "../../../src/plan/semantic/xr_semantic_plan_internal.h"
#include "../../../src/plan/semantic/xr_semantic_verify.h"
#include "../../../src/plan/ownership/xr_ownership_certificate_internal.h"
#include "../../../src/plan/format/xr_xsm_schema.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_capability.h"
#include "../../../src/aot/emit_c/xr_c_emission_plan_internal.h"
#include "../../../src/aot/refine/xr_aot_representation_refinement.h"
#include "../../../src/ir/xi_opt.h"
#include "../../../src/plan/target/xr_target_plan_internal.h"
#include "../../../src/plan/target/xr_target_profile_internal.h"
#include "../../../src/plan/target/xr_target_instruction_verify.h"
#include "../../../src/plan/target/xr_target_program_reachability.h"
#include "../../../src/plan/target/xr_target_verify.h"
#include "../../../src/aot/emit_c/xr_c_emission_rule_ids_gen.h"
#include "../../../src/runtime/class/xclass_info.h"
#include "../../../src/runtime/object/xarray.h"
#include "../../../src/runtime/value/xstruct_layout.h"
#include "../../../src/runtime/value/xenum_layout.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/vm/xr_typed_dispatch.h"
#include "../../../src/stdlib/xstdlib_defs_generated.h"
#include "../../../src/runtime/abi/xr_runtime_target_authority.h"
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

static XrEnumLayout *make_unit_enum_type(XrType *out, uint32_t id, const char *owner,
                                         const char *name) {
    static const char *members[] = {"First", "Second"};
    XrEnumLayout *layout = xr_enum_layout_new(owner, name, members, 2);
    REQUIRE(out != NULL && layout != NULL && layout->is_zero_payload && layout->layout_id != 0);
    *out = (XrType) {
        .kind = XR_KIND_ENUM,
        .id = id,
        .frozen = true,
        .scalar_rep = XR_SCALAR_REP_NONE,
        .enum_type =
            {
                .enum_name = name,
                .layout_id = layout->layout_id,
                .layout = layout,
            },
    };
    return layout;
}
static XrType stub_module_namespace = {
    .kind = XR_KIND_UNKNOWN,
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
    .instance =
        {
            .class_name = "Iterator",
            .type_args = stub_iterator_rune_args,
            .type_arg_count = 1,
        },
};
static XrType stub_unknown = {
    .kind = XR_KIND_UNKNOWN,
    .id = 130,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_map_string_string = {
    .kind = XR_KIND_MAP,
    .id = 131,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .map = {.key_type = &stub_exact_string, .value_type = &stub_exact_string},
};
static XrType *stub_map_entry_elements[] = {&stub_exact_string, &stub_exact_string};
static XrType stub_map_entry = {
    .kind = XR_KIND_TUPLE,
    .id = 132,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .tuple = {.element_types = stub_map_entry_elements, .element_count = 2},
};
static XrType *stub_map_entry_iterator_args[] = {&stub_map_entry};
static XrType stub_map_entry_iterator = {
    .kind = XR_KIND_INSTANCE,
    .id = 133,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance =
        {
            .class_name = "Iterator",
            .type_args = stub_map_entry_iterator_args,
            .type_arg_count = 1,
        },
};
static XrType *stub_unknown_iterator_args[] = {&stub_unknown};
static XrType stub_unknown_iterator = {
    .kind = XR_KIND_INSTANCE,
    .id = 134,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance =
        {
            .class_name = "Iterator",
            .type_args = stub_unknown_iterator_args,
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
    .function = {.return_type = &stub_raw_pointer, .throw_effect = XR_FN_EFFECT_NO_THROW},
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
static XrType stub_target_const_i64_array = {
    .kind = XR_KIND_ARRAY,
    .id = 183,
    .frozen = true,
    .is_const = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .container = {.element_type = &stub_int},
};
static XrType stub_target_string_array = {
    .kind = XR_KIND_ARRAY,
    .id = 135,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .container = {.element_type = &stub_exact_string},
};
static XrType stub_target_string_array_array = {
    .kind = XR_KIND_ARRAY,
    .id = 184,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .container = {.element_type = &stub_target_string_array},
};
static XrFunctionParam stub_array_hof_unary_params[] = {
    {.type = &stub_int, .mode = XR_PARAM_READ},
};
static XrType stub_array_hof_callback = {
    .kind = XR_KIND_FUNCTION,
    .id = 123,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .function =
        {
            .params = stub_array_hof_unary_params,
            .param_count = 1,
            .min_params = 1,
            .return_type = &stub_int,
            .throw_effect = XR_FN_EFFECT_NO_THROW,
        },
};
static XrType stub_array_hof_filter_callback = {
    .kind = XR_KIND_FUNCTION,
    .id = 125,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .function =
        {
            .params = stub_array_hof_unary_params,
            .param_count = 1,
            .min_params = 1,
            .return_type = &stub_bool,
            .throw_effect = XR_FN_EFFECT_NO_THROW,
        },
};
static XrFunctionParam stub_array_hof_reduce_params[] = {
    {.type = &stub_int, .mode = XR_PARAM_READ},
    {.type = &stub_int, .mode = XR_PARAM_READ},
};
static XrType stub_array_hof_reduce_callback = {
    .kind = XR_KIND_FUNCTION,
    .id = 126,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .function =
        {
            .params = stub_array_hof_reduce_params,
            .param_count = 2,
            .min_params = 2,
            .return_type = &stub_int,
            .throw_effect = XR_FN_EFFECT_NO_THROW,
        },
};
static XrType stub_array_hof_int_array = {
    .kind = XR_KIND_ARRAY,
    .id = 124,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .container = {.element_type = &stub_int},
};
static XrType stub_target_u8_slice = {
    .kind = XR_KIND_SLICE,
    .id = 118,
    .frozen = true,
    .is_const = true,
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
static XrType stub_target_nullable_source_instance = {
    .kind = XR_KIND_INSTANCE,
    .id = 182,
    .frozen = true,
    .is_nullable = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance =
        {
            .class_name = "FinalTargetWorker",
            .class_ref = &stub_target_source_class_info,
        },
};
static XrType stub_target_source_instance_array = {
    .kind = XR_KIND_ARRAY,
    .id = 181,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .container = {.element_type = &stub_target_source_instance},
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
static XrClassInfo stub_imported_constructor_class_info = {
    .name = "ImportedDiagnostic",
    .xg_class_id = 802,
};
static XrType stub_imported_constructor_instance = {
    .kind = XR_KIND_INSTANCE,
    .id = 180,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .instance =
        {
            .class_name = "ImportedDiagnostic",
            .class_ref = &stub_imported_constructor_class_info,
        },
};

static bool build_target_unit_fixture_semantic(XiFunc *function, XrSemanticPlan **out, char *error,
                                               size_t error_size) {
    XiModule fixture_module = {
        .identity = "memory-module-v1:id=27:target-plan-unit-fixture-v1",
        .path = "target-plan-unit-fixture.xr",
        .name = "target_plan_unit_fixture",
        .init = function,
    };
    REQUIRE(function != NULL && function->module == NULL);
    function->module = &fixture_module;
    bool built = xr_semantic_plan_build(function, out, error, error_size);
    function->module = NULL;
    return built;
}

static XrSemanticPlan *build_native_target_leaf_semantic(void) {
    XiFunc *function = xi_func_new("native_target_leaf_probe", &stub_int);
    XiBlock *entry = function ? xi_block_new(function) : NULL;
    REQUIRE(function != NULL && entry != NULL);
    XiImportRef import_ref = {
        .module_path = "os",
        .member_name = "__getpid",
        .resolved_mod_index = -1,
        .resolved_shared_slot = -1,
        .resolved_export_slot = -1,
        .resolution_attempted = true,
    };
    XiValue *callee = xi_value_new(function, entry, XI_IMPORT_REF, &stub_function, 0);
    XiValue *call = xi_value_new(function, entry, XI_CALL, &stub_int, 1);
    REQUIRE(callee != NULL && call != NULL);
    callee->aux = &import_ref;
    call->args[0] = callee;
    xi_block_set_return(entry, call);
    function->stage = XI_STAGE_OPTIMIZED;

    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = build_target_unit_fixture_semantic(function, &semantic, error, sizeof(error));
    if (!built)
        fprintf(stderr, "native target leaf semantic fixture failed: %s\n", error);
    REQUIRE(built && semantic != NULL);
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_assertion_semantic(XrCoreBuiltinId builtin_id) {
    XiFunc *function = xi_func_new("assertion_capability_probe", &stub_unit);
    XiBlock *entry = function ? xi_block_new(function) : NULL;
    REQUIRE(function != NULL && entry != NULL);
    const XrCoreIntrinsicDesc *desc = xr_core_intrinsic_by_id(builtin_id);
    REQUIRE(desc != NULL && desc->category == XR_CORE_INTRINSIC_CATEGORY_ASSERTION);
    XiValue *arguments[2] = {0};
    uint16_t arity = 1;
    if (builtin_id == XR_CORE_BUILTIN_ASSERT) {
        arguments[0] = xi_const_bool(function, entry, true, &stub_bool);
    } else if (builtin_id == XR_CORE_BUILTIN_ASSERT_EQUAL) {
        arity = 2;
        arguments[0] = xi_const_int(function, entry, 1, &stub_int);
        arguments[1] = xi_const_int(function, entry, 1, &stub_int);
    } else {
        arguments[0] = xi_param(function, entry, 0, &stub_exact_function);
        function->params = (XiValue **) xr_malloc(sizeof(*function->params));
        REQUIRE(function->params != NULL);
        function->params[0] = arguments[0];
        function->nparams = 1;
    }
    REQUIRE(arguments[0] != NULL && (arity == 1 || arguments[1] != NULL));
    XiValue *assertion = xi_value_new(function, entry, XI_ASSERTION, &stub_unit, arity);
    REQUIRE(assertion != NULL);
    for (uint16_t i = 0; i < arity; i++)
        assertion->args[i] = arguments[i];
    assertion->source_span = (XiSourceSpan) {3, 5, 3, 24};
    XrLocation source = {"assertion-capability.xr", 3, 5, 3, 24};
    XrAssertionPlan plan;
    REQUIRE(xr_assertion_plan_build(builtin_id, arity, source,
                                    XR_CORE_INTRINSIC_TARGET_ASSERTION_ALL,
                                    XR_ASSERTION_CAPABILITY_NONE, &plan) == XR_ASSERTION_PLAN_OK);
    REQUIRE(xi_value_set_assertion_plan(function, assertion, &plan));
    xi_block_set_return(entry, assertion);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = build_target_unit_fixture_semantic(function, &semantic, error, sizeof(error));
    if (!built)
        fprintf(stderr, "assertion semantic fixture failed: %s\n", error);
    REQUIRE(built);
    xi_func_free(function);
    return semantic;
}

static XrTargetProfile *
build_native_profile_with_provider_count(XrRuntimeTargetAuthority *authority,
                                         size_t provider_count) {
    XrTargetProfileBuildInput input = {
        .machine = authority->machine,
        .runtime_abi = &authority->runtime_abi,
        .object_header_materialization = &authority->object_header_materialization,
        .string_contract = &authority->string_contract,
        .providers = authority->providers,
        .provider_count = provider_count,
    };
    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_profile_build(&input, &profile, error, sizeof(error)));
    return profile;
}

static void set_single_parameter_ownership(XiFunc *function, XiOwnership ownership);

static XrSemanticPlan *build_source_instance_method_semantic(bool owned_instance_result,
                                                             bool nullable_instance_result) {
    XrType *method_result_type = nullable_instance_result ? &stub_target_nullable_source_instance
                                                          : &stub_target_source_instance;
    XiFunc *root = xi_func_new("target_source_instance_root", &stub_unit);
    XiFunc *callee = xi_func_new("wait", owned_instance_result ? method_result_type : &stub_unit);
    XiFunc *caller = xi_func_new("run", owned_instance_result ? method_result_type : &stub_unit);
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
    callee->nparams = callee->min_params = 1;
    caller->nparams = caller->min_params = 1;
    callee->has_receiver = true;
    callee->receiver_mode = XR_PARAM_MOVE;
    callee->receiver_borrowed = false;
    caller->has_receiver = true;
    caller->receiver_mode = XR_PARAM_MOVE;
    caller->receiver_borrowed = false;
    callee_this->transfer_mode = XR_TRANSFER_SHARE;
    caller_this->transfer_mode = XR_TRANSFER_SHARE;
    set_single_parameter_ownership(callee, XI_OWN_BORROWED);
    set_single_parameter_ownership(caller, XI_OWN_OWNED);
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
    XiValue *callee_result = NULL;
    if (owned_instance_result) {
        root->is_module_initializer = true;
        XiValue *class_object = xi_value_new(root, root_entry, XI_CLASS_CREATE, &stub_unknown, 0);
        XiValue *class_store = xi_value_new(root, root_entry, XI_SET_SHARED, &stub_unit, 1);
        REQUIRE(class_object != NULL && class_store != NULL);
        class_object->aux = &source_class;
        class_store->args[0] = class_object;
        class_store->aux_int = 0;
        root->nshared = 1;

        XiValue *class_load = xi_value_new(callee, callee_entry, XI_GET_SHARED, &stub_unknown, 0);
        callee_result =
            xi_value_new(callee, callee_entry, XI_CALL, &stub_target_source_instance, 1);
        REQUIRE(class_load != NULL && callee_result != NULL);
        class_load->aux_int = 0;
        callee_result->args[0] = class_load;
        callee_result->lowering_flags |= XI_LOWERING_FLAG_CONSTRUCTOR_CALL;
        callee_result->call_return_ownership = (XiReturnOwnership) {
            .kind = XI_RETURN_OWNERSHIP_OWNED,
            .param_index = -1,
            .complete = true,
        };
        callee->arc_return_ownership = callee_result->call_return_ownership;
    }
    xi_block_set_return(callee_entry, callee_result);
    XiValue *moved =
        xi_value_new(caller, caller_entry, XI_SOURCE_MOVE, &stub_target_source_instance, 1);
    XiValue *call = xi_value_new(caller, caller_entry, XI_CALL_METHOD,
                                 owned_instance_result ? method_result_type : &stub_unit, 1);
    REQUIRE(moved != NULL && call != NULL);
    moved->args[0] = caller_this;
    moved->move_evidence_id = 1;
    moved->move_source_root_id = 1;
    moved->move_source_symbol_id = 1;
    moved->move_storage_plan_id = 1;
    moved->move_evidence_bits = XA_OWNERSHIP_EV_BINDING_LIVE | XA_OWNERSHIP_EV_ROOT_UNIQUE |
                                XA_OWNERSHIP_EV_LOAN_FREE | XA_OWNERSHIP_EV_ALIAS_FREE |
                                XA_OWNERSHIP_EV_ESCAPE_FREE | XA_OWNERSHIP_EV_CAPABILITY |
                                XA_OWNERSHIP_EV_CFG_CONSISTENT | XA_OWNERSHIP_EV_STORAGE;
    moved->move_source_capability = XA_CAP_MUTABLE;
    moved->move_target_capability = XA_CAP_MUTABLE;
    moved->move_source_domain = XR_STORAGE_TRANSFERABLE;
    moved->move_target_domain = XR_STORAGE_TRANSFERABLE;
    call->args[0] = moved;
    call->aux = "wait";
    XiCallPlan call_plan = {
        .receiver =
            {
                .param_mode = XR_PARAM_MOVE,
                .access = XR_CALL_ARG_MOVE,
                .origin_var_id = XI_NO_VAR_ID,
            },
        .has_receiver = true,
        .verified = true,
    };
    call->call_plan = &call_plan;
    if (owned_instance_result) {
        call->call_return_ownership = (XiReturnOwnership) {
            .kind = XI_RETURN_OWNERSHIP_OWNED,
            .param_index = -1,
            .complete = true,
        };
        caller->arc_return_ownership = call->call_return_ownership;
    }
    xi_block_set_return(caller_entry, call);
    xi_block_set_return(root_entry, NULL);
    root->stage = callee->stage = caller->stage = XI_STAGE_OPTIMIZED;
    XiModule *module =
        xi_module_new("pkg/target_source_instance.xr", "target_source_instance", root);
    REQUIRE(module != NULL);
    REQUIRE(xi_module_set_identity(module, "memory-module-v1:id=25:target-source-instance-v1"));
    root->module = module;
    module->classes = (XiClassData **) xr_malloc(sizeof(*module->classes));
    REQUIRE(module->classes != NULL);
    module->classes[0] = &source_class;
    module->nclasses = 1;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(root, &semantic, error, sizeof(error)));
    REQUIRE(semantic != NULL && semantic->call_target_count == (owned_instance_result ? 2u : 1u));
    uint32_t local_method_targets = 0;
    for (uint32_t i = 0; i < semantic->call_target_count; i++)
        if (semantic->call_targets[i].kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL)
            local_method_targets++;
    REQUIRE(local_method_targets == 1);
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
    REQUIRE(xi_module_set_identity(dependency_module,
                                   "memory-module-v1:id=25:open-target-dependency-v1"));
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
    REQUIRE(xi_module_set_identity(caller_module, "memory-module-v1:id=21:open-target-caller-v1"));
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
    REQUIRE(build_target_unit_fixture_semantic(function, &semantic, error, sizeof(error)));
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
        xi_value_new(function, entry, XI_CALL_BUILTIN, &stub_target_u8_array, 1);
    REQUIRE(capacity != NULL && with_capacity != NULL);
    with_capacity->args[0] = capacity;
    with_capacity->aux = (void *) "array_with_capacity";
    with_capacity->array_intrinsic_kind = XI_ARRAY_INTRINSIC_WITH_CAPACITY;
    with_capacity->array_element_storage = XR_ELEM_U8;
    XiValue *release_capacity = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release_capacity != NULL);
    release_capacity->args[0] = with_capacity;

    XiValue *count = xi_const_int(function, entry, 3, &stub_int);
    XiValue *fill = xi_const_int(function, entry, 1, &stub_int);
    XiValue *filled = xi_value_new(function, entry, XI_CALL_BUILTIN, &stub_target_u8_array, 2);
    REQUIRE(count != NULL && fill != NULL && filled != NULL);
    filled->args[0] = count;
    filled->args[1] = fill;
    filled->aux = (void *) "array_filled_new";
    filled->array_intrinsic_kind = XI_ARRAY_INTRINSIC_FILLED_NEW;
    filled->array_element_storage = XR_ELEM_U8;
    XiValue *release_filled = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release_filled != NULL);
    release_filled->args[0] = filled;

    XiValue *result = xi_const_int(function, entry, 0, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = build_target_unit_fixture_semantic(function, &semantic, error, sizeof(error));
    if (!built)
        fprintf(stderr, "Array intrinsic semantic failed: %s\n", error);
    REQUIRE(built && semantic != NULL);
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_array_hof_semantic(uint8_t hof_kind) {
    bool is_filter = hof_kind == XI_ARRAY_HOF_FILTER;
    bool is_reduce = hof_kind == XI_ARRAY_HOF_REDUCE;
    uint16_t callback_parameter_count = is_reduce ? 2 : 1;
    XrType *callback_return_type = is_filter ? &stub_bool : &stub_int;
    XrType *callback_type = is_filter   ? &stub_array_hof_filter_callback
                            : is_reduce ? &stub_array_hof_reduce_callback
                                        : &stub_array_hof_callback;
    XrType *result_type = is_reduce ? &stub_int : &stub_array_hof_int_array;
    uint16_t operand_count = is_reduce ? 3 : 2;
    const char *selector = is_filter ? "filter" : is_reduce ? "reduce" : "map";
    XiFunc *root = xi_func_new("target_array_hof", &stub_int);
    XiFunc *callback = xi_func_new("target_array_hof_callback", callback_return_type);
    REQUIRE(root != NULL && callback != NULL);
    XiBlock *entry = xi_block_new(root);
    XiBlock *callback_entry = xi_block_new(callback);
    REQUIRE(entry != NULL && callback_entry != NULL);

    callback->nparams = callback->min_params = callback_parameter_count;
    callback->params = (XiValue **) xr_calloc(callback_parameter_count, sizeof(*callback->params));
    REQUIRE(callback->params != NULL);
    callback->params[0] = xi_param(callback, callback_entry, 0, &stub_int);
    REQUIRE(callback->params[0] != NULL);
    if (is_reduce) {
        callback->params[1] = xi_param(callback, callback_entry, 1, &stub_int);
        REQUIRE(callback->params[1] != NULL);
    }
    XiValue *callback_result =
        is_filter ? xi_const_bool(callback, callback_entry, true, &stub_bool) : callback->params[0];
    REQUIRE(callback_result != NULL);
    xi_block_set_return(callback_entry, callback_result);

    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = callback;
    root->nchildren = root->children_cap = 1;
    callback->parent_func = root;

    XiValue *capacity = xi_const_int(root, entry, 4, &stub_int);
    XiValue *array = xi_value_new(root, entry, XI_ARRAY_NEW, &stub_array_hof_int_array, 1);
    XiValue *closure = xi_value_new(root, entry, XI_CLOSURE_NEW, callback_type, 0);
    XiValue *seed = is_reduce ? xi_const_int(root, entry, 0, &stub_int) : NULL;
    XiValue *hof = xi_value_new(root, entry, XI_CALL_METHOD, result_type, operand_count);
    REQUIRE(capacity != NULL && array != NULL && closure != NULL && hof != NULL &&
            (!is_reduce || seed != NULL));
    array->args[0] = capacity;
    array->array_element_storage = XR_ELEM_I64;
    closure->aux = callback;
    hof->args[0] = array;
    hof->args[1] = closure;
    if (is_reduce)
        hof->args[2] = seed;
    hof->aux = (void *) selector;
    hof->array_hof_kind = hof_kind;
    hof->array_element_storage = XR_ELEM_I64;
    hof->array_result_element_storage = XR_ELEM_I64;
    XiValue *release_hof = NULL;
    if (!is_reduce) {
        hof->call_return_ownership = (XiReturnOwnership) {
            .kind = XI_RETURN_OWNERSHIP_OWNED,
            .param_index = -1,
            .complete = true,
        };
        release_hof = xi_value_new(root, entry, XI_RELEASE, &stub_unit, 1);
        REQUIRE(release_hof != NULL);
        release_hof->args[0] = hof;
    }
    XiValue *release_array = xi_value_new(root, entry, XI_RELEASE, &stub_unit, 1);
    XiValue *result = xi_const_int(root, entry, 0, &stub_int);
    REQUIRE(release_array != NULL && result != NULL);
    release_array->args[0] = array;
    xi_block_set_return(entry, result);
    root->stage = callback->stage = XI_STAGE_OPTIMIZED;

    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = build_target_unit_fixture_semantic(root, &semantic, error, sizeof(error));
    if (!built)
        fprintf(stderr, "Array HOF semantic failed: %s\n", error);
    REQUIRE(built && semantic != NULL);
    xi_func_free(root);
    return semantic;
}

static XrSemanticPlan *build_array_reserve_semantic(void) {
    XiFunc *function = xi_func_new("target_array_reserve", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *initial_capacity = xi_const_int(function, entry, 3, &stub_int);
    XiValue *array = xi_value_new(function, entry, XI_ARRAY_NEW, &stub_target_u8_array, 1);
    XiValue *reserve_capacity = xi_const_int(function, entry, 8, &stub_int);
    XiValue *reserve = xi_value_new(function, entry, XI_CALL_BUILTIN, &stub_target_u8_array, 2);
    REQUIRE(initial_capacity && array && reserve_capacity && reserve);
    array->args[0] = initial_capacity;
    array->array_element_storage = XR_ELEM_U8;
    reserve->args[0] = array;
    reserve->args[1] = reserve_capacity;
    reserve->xa_intrinsic_id = XA_INTRINSIC_ARRAY_RESERVE;
    reserve->result_alias_operand = 0;
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    XiValue *result = xi_const_int(function, entry, 0, &stub_int);
    REQUIRE(release && result);
    release->args[0] = reserve;
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = build_target_unit_fixture_semantic(function, &semantic, error, sizeof(error));
    if (!built)
        fprintf(stderr, "Array.reserve semantic failed: %s\n", error);
    REQUIRE(built && semantic != NULL);
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_tagged_string_array_copy_semantic(void) {
    XiFunc *function = xi_func_new("target_tagged_string_array_copy", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *source = xi_param(function, entry, 0, &stub_target_string_array);
    XiValue *copy = xi_value_new(function, entry, XI_CALL_BUILTIN, &stub_target_string_array, 1);
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    XiValue *result = xi_const_int(function, entry, 0, &stub_int);
    REQUIRE(source && copy && release && result);
    function->nparams = function->min_params = 1;
    function->params = (XiValue **) xr_calloc(1, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = source;
    function->arc_borrow_sig =
        (XiBorrowSig *) xi_func_arena_alloc(function, (uint32_t) sizeof(*function->arc_borrow_sig));
    REQUIRE(function->arc_borrow_sig != NULL);
    function->arc_borrow_sig->nparams = 1;
    function->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    function->arc_borrow_sig->valid = true;
    copy->args[0] = source;
    copy->aux = (void *) "copy";
    copy->call_return_ownership = (XiReturnOwnership) {
        .kind = XI_RETURN_OWNERSHIP_OWNED,
        .param_index = -1,
        .complete = true,
    };
    release->args[0] = copy;
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = build_target_unit_fixture_semantic(function, &semantic, error, sizeof(error));
    if (!built)
        fprintf(stderr, "tagged String Array copy semantic failed: %s\n", error);
    REQUIRE(built && semantic != NULL);
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_tagged_string_array_index_read_semantic(void) {
    XiFunc *function = xi_func_new("target_tagged_string_array_index_read", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *source = xi_param(function, entry, 0, &stub_target_string_array);
    XiValue *index = xi_const_int(function, entry, 0, &stub_int);
    XiValue *read = xi_value_new(function, entry, XI_INDEX_GET, &stub_exact_string, 2);
    XiValue *result = xi_const_int(function, entry, 0, &stub_int);
    REQUIRE(source && index && read && result);
    function->nparams = function->min_params = 1;
    function->params = (XiValue **) xr_calloc(1, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = source;
    function->arc_borrow_sig =
        (XiBorrowSig *) xi_func_arena_alloc(function, (uint32_t) sizeof(*function->arc_borrow_sig));
    REQUIRE(function->arc_borrow_sig != NULL);
    function->arc_borrow_sig->nparams = 1;
    function->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    function->arc_borrow_sig->valid = true;
    read->args[0] = source;
    read->args[1] = index;
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = build_target_unit_fixture_semantic(function, &semantic, error, sizeof(error));
    if (!built)
        fprintf(stderr, "tagged String Array index-read semantic failed: %s\n", error);
    REQUIRE(built && semantic != NULL);
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_shared_const_i64_array_semantic(void) {
    XiFunc *function = xi_func_new("target_shared_const_i64_array", &stub_int);
    XiBlock *entry = function ? xi_block_new(function) : NULL;
    REQUIRE(function != NULL && entry != NULL);
    XiValue *load = xi_value_new(function, entry, XI_GET_SHARED, &stub_target_const_i64_array, 0);
    XiValue *result = xi_const_int(function, entry, 0, &stub_int);
    REQUIRE(load != NULL && result != NULL);
    load->aux_int = 0;
    function->nshared = 1;
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;

    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = build_target_unit_fixture_semantic(function, &semantic, error, sizeof(error));
    if (!built)
        fprintf(stderr, "shared const Array<i64> semantic failed: %s\n", error);
    REQUIRE(built && semantic != NULL);
    xi_func_free(function);
    return semantic;
}

/* The StringBuilder method families are all stated against the same
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
    XiModule fixture_module = {
        .identity = "memory-module-v1:id=27:target-plan-unit-fixture-v1",
        .path = "target-plan-unit-fixture.xr",
        .name = "target_plan_unit_fixture",
        .init = function,
    };
    XiModule *saved_module = function->module;
    if (!saved_module)
        function->module = &fixture_module;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &semantic, error, sizeof(error));
    function->module = saved_module;
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
    append->xa_intrinsic_id = XA_INTRINSIC_STRING_BUILDER_APPEND;
    append->aux = (void *) XA_INTRINSIC_STRING_BUILDER_APPEND_SOURCE_MEMBER;
    append->aux_int = (int64_t) XI_METHOD_SYMBOL_APPEND << 1;
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
    append->xa_intrinsic_id = XA_INTRINSIC_STRING_BUILDER_APPEND;
    append->aux = (void *) XA_INTRINSIC_STRING_BUILDER_APPEND_SOURCE_MEMBER;
    append->aux_int = (int64_t) XI_METHOD_SYMBOL_APPEND << 1;
    append->result_alias_operand = 0;
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = append;
    return finish_stringbuilder_semantic(function, entry, "StringBuilder.append(string)");
}

static XrSemanticPlan *build_stringbuilder_clear_semantic(void) {
    XiFunc *function = xi_func_new("target_stringbuilder_clear", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *builder = emit_exact_string_builder(function, entry);
    XiValue *clear = xi_value_new(function, entry, XI_CALL_METHOD, &stub_string_builder, 1);
    REQUIRE(clear != NULL);
    clear->args[0] = builder;
    clear->aux = (void *) "clear";
    clear->aux_int = (int64_t) XI_METHOD_SYMBOL_CLEAR << 1;
    clear->result_alias_operand = 0;
    clear->call_return_ownership = (XiReturnOwnership) {
        .kind = XI_RETURN_OWNERSHIP_BORROWED_PARAM,
        .param_index = 0,
        .complete = true,
    };
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = builder;
    return finish_stringbuilder_semantic(function, entry, "StringBuilder.clear");
}

/* Keep Iterator<rune> as semantic type zero, matching the hosted text module
 * that exposed the layout gap. Scalar bookkeeping can already mention type
 * zero without publishing a layout; the exact String.runes family must still
 * publish the one dynamic row its owned result requires. */
static XrSemanticPlan *build_string_runes_leading_type_semantic(void) {
    XiFunc *function = xi_func_new("target_string_runes_leading_type", &stub_iterator_rune);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(function, entry, "target-runes-layout", &stub_exact_string);
    XiValue *runes = xi_value_new(function, entry, XI_CALL_METHOD, &stub_iterator_rune, 1);
    REQUIRE(source && runes);
    runes->args[0] = source;
    runes->aux = (void *) "runes";
    runes->aux_int = (int64_t) XI_METHOD_SYMBOL_RUNES << 1;
    xi_block_set_return(entry, runes);
    function->stage = XI_STAGE_OPTIMIZED;
    XiModule fixture_module = {
        .identity = "memory-module-v1:id=27:target-plan-unit-fixture-v1",
        .path = "target-plan-unit-fixture.xr",
        .name = "target_plan_unit_fixture",
        .init = function,
    };
    function->module = &fixture_module;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &semantic, error, sizeof(error));
    function->module = NULL;
    if (!built)
        fprintf(stderr, "String.runes leading-type semantic failed: %s\n", error);
    REQUIRE(built && semantic);
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_string_slice_range_semantic(bool optional_receiver) {
    XiFunc *function = xi_func_new("target_string_slice_range", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *source = NULL;
    if (optional_receiver) {
        function->nparams = 1;
        function->min_params = 0;
        function->params = (XiValue **) xr_calloc(1, sizeof(*function->params));
        REQUIRE(function->params != NULL);
        function->params[0] = xi_param(function, entry, 0, &stub_exact_string);
        REQUIRE(function->params[0] != NULL);
        function->params[0]->transfer_mode = XR_TRANSFER_SHARE;
        set_single_parameter_ownership(function, XI_OWN_BORROWED);
        source = function->params[0];
    } else {
        source = xi_const_str(function, entry, "target-slice", &stub_exact_string);
    }
    XiValue *start = xi_const_int(function, entry, 1, &stub_int);
    XiValue *end = xi_const_int(function, entry, 4, &stub_int);
    XiValue *slice = xi_value_new(function, entry, XI_CALL_METHOD, &stub_exact_string, 3);
    REQUIRE(source && start && end && slice);
    slice->args[0] = source;
    slice->args[1] = start;
    slice->args[2] = end;
    slice->aux = (void *) "slice";
    slice->aux_int = 32;
    slice->flags |= XI_FLAG_TAIL;
    slice->call_return_ownership.kind = XI_RETURN_OWNERSHIP_OWNED;
    slice->call_return_ownership.param_index = -1;
    slice->call_return_ownership.complete = true;
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = slice;
    return finish_stringbuilder_semantic(function, entry, "String.slice(start, end)");
}

static XrSemanticPlan *build_iterator_rune_has_next_semantic(void) {
    XiFunc *function = xi_func_new("target_iterator_rune_has_next", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(function, entry, "target-has-next", &stub_exact_string);
    XiValue *runes = xi_value_new(function, entry, XI_CALL_METHOD, &stub_iterator_rune, 1);
    XiValue *has_next = xi_value_new(function, entry, XI_CALL_METHOD, &stub_bool, 1);
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
    return finish_stringbuilder_semantic(function, entry, "Iterator<rune>.hasNext");
}

static XrSemanticPlan *build_iterator_rune_next_semantic(void) {
    XiFunc *function = xi_func_new("target_iterator_rune_next", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(function, entry, "target-next", &stub_exact_string);
    XiValue *runes = xi_value_new(function, entry, XI_CALL_METHOD, &stub_iterator_rune, 1);
    XiValue *next = xi_value_new(function, entry, XI_CALL_METHOD, &stub_rune, 1);
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
    return finish_stringbuilder_semantic(function, entry, "Iterator<rune>.next");
}

static XrSemanticPlan *build_iterator_rune_nth_semantic(void) {
    XiFunc *function = xi_func_new("target_iterator_rune_nth", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(function, entry, "target-nth", &stub_exact_string);
    XiValue *runes = xi_value_new(function, entry, XI_CALL_METHOD, &stub_iterator_rune, 1);
    XiValue *index = xi_const_int(function, entry, 1, &stub_int);
    XiValue *nth = xi_value_new(function, entry, XI_CALL_METHOD, &stub_rune, 2);
    REQUIRE(source && runes && index && nth);
    runes->args[0] = source;
    runes->aux = (void *) "runes";
    runes->aux_int = (int64_t) XI_METHOD_SYMBOL_RUNES << 1;
    nth->args[0] = runes;
    nth->args[1] = index;
    nth->aux = (void *) "nth";
    nth->aux_int = (int64_t) XI_METHOD_SYMBOL_NTH << 1;
    nth->call_return_ownership.kind = XI_RETURN_OWNERSHIP_OWNED;
    nth->call_return_ownership.param_index = -1;
    nth->call_return_ownership.complete = true;
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = runes;
    return finish_stringbuilder_semantic(function, entry, "Iterator<rune>.nth");
}

static XrSemanticPlan *build_rune_to_string_semantic(void) {
    XiFunc *function = xi_func_new("target_rune_to_string", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(function, entry, "target-rune-string", &stub_exact_string);
    XiValue *runes = xi_value_new(function, entry, XI_CALL_METHOD, &stub_iterator_rune, 1);
    XiValue *index = xi_const_int(function, entry, 1, &stub_int);
    XiValue *nth = xi_value_new(function, entry, XI_CALL_METHOD, &stub_rune, 2);
    XiValue *to_string = xi_value_new(function, entry, XI_CALL_METHOD, &stub_exact_string, 1);
    REQUIRE(source && runes && index && nth && to_string);
    runes->args[0] = source;
    runes->aux = (void *) "runes";
    runes->aux_int = (int64_t) XI_METHOD_SYMBOL_RUNES << 1;
    nth->args[0] = runes;
    nth->args[1] = index;
    nth->aux = (void *) "nth";
    nth->aux_int = (int64_t) XI_METHOD_SYMBOL_NTH << 1;
    nth->call_return_ownership = (XiReturnOwnership) {
        .kind = XI_RETURN_OWNERSHIP_OWNED, .param_index = -1, .complete = true};
    to_string->args[0] = nth;
    to_string->aux = (void *) "toString";
    to_string->aux_int = (int64_t) XI_METHOD_SYMBOL_TOSTRING << 1;
    to_string->call_return_ownership = (XiReturnOwnership) {
        .kind = XI_RETURN_OWNERSHIP_OWNED, .param_index = -1, .complete = true};
    XiValue *release_string = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    XiValue *release_runes = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release_string && release_runes);
    release_string->args[0] = to_string;
    release_runes->args[0] = runes;
    return finish_stringbuilder_semantic(function, entry, "rune.toString");
}

static XrSemanticPlan *build_direct_rune_parameter_to_string_semantic(void) {
    XiFunc *function = xi_func_new("target_direct_rune_parameter_to_string", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *receiver = xi_param(function, entry, 0, &stub_rune);
    XiValue *to_string = xi_value_new(function, entry, XI_CALL_METHOD, &stub_exact_string, 1);
    XiValue *release_string = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(receiver && to_string && release_string);
    function->nparams = function->min_params = 1;
    function->params = (XiValue **) xr_calloc(1, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = receiver;
    to_string->args[0] = receiver;
    to_string->aux = (void *) "toString";
    to_string->aux_int = (int64_t) XI_METHOD_SYMBOL_TOSTRING << 1;
    to_string->call_return_ownership = (XiReturnOwnership) {
        .kind = XI_RETURN_OWNERSHIP_OWNED, .param_index = -1, .complete = true};
    release_string->args[0] = to_string;
    return finish_stringbuilder_semantic(function, entry, "direct rune parameter toString");
}

static XrSemanticPlan *build_map_entry_iterator_semantic(XiMethodSymbolId factory_symbol,
                                                         bool exact_result_types) {
    XiFunc *function = xi_func_new("target_map_entry_iterator", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *map = xi_param(function, entry, 0, &stub_map_string_string);
    REQUIRE(map != NULL);
    function->nparams = function->min_params = 1;
    function->params = (XiValue **) xr_calloc(1, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = map;
    function->arc_borrow_sig =
        (XiBorrowSig *) xi_func_arena_alloc(function, (uint32_t) sizeof(*function->arc_borrow_sig));
    REQUIRE(function->arc_borrow_sig != NULL);
    function->arc_borrow_sig->nparams = 1;
    function->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    function->arc_borrow_sig->valid = true;

    XrType *iterator_type = exact_result_types ? &stub_map_entry_iterator : &stub_unknown_iterator;
    XrType *entry_type = exact_result_types ? &stub_map_entry : &stub_unknown;
    XiValue *iterator = xi_value_new(function, entry, XI_CALL_METHOD, iterator_type, 1);
    XiValue *has_next = xi_value_new(function, entry, XI_CALL_METHOD, &stub_bool, 1);
    XiValue *next = xi_value_new(function, entry, XI_CALL_METHOD, entry_type, 1);
    REQUIRE(iterator && has_next && next);
    iterator->args[0] = map;
    iterator->aux = (void *) "diagnostic-selector-is-not-authority";
    iterator->aux_int = (int64_t) factory_symbol << 1;
    iterator->call_return_ownership = (XiReturnOwnership) {
        .kind = XI_RETURN_OWNERSHIP_OWNED, .param_index = -1, .complete = true};
    iterator->flags |= XI_FLAG_SIDE_EFFECT;
    has_next->args[0] = iterator;
    has_next->aux = (void *) "diagnostic-has-next";
    has_next->aux_int = (int64_t) XI_METHOD_SYMBOL_HAS_NEXT << 1;
    has_next->flags |= XI_FLAG_SIDE_EFFECT;
    next->args[0] = iterator;
    next->aux = (void *) "diagnostic-next";
    next->aux_int = (int64_t) XI_METHOD_SYMBOL_NEXT << 1;
    next->call_return_ownership = (XiReturnOwnership) {
        .kind = XI_RETURN_OWNERSHIP_OWNED, .param_index = -1, .complete = true};
    next->flags |= XI_FLAG_SIDE_EFFECT;
    XiValue *release_entry = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    XiValue *release_iterator = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release_entry && release_iterator);
    release_entry->args[0] = next;
    release_iterator->args[0] = iterator;
    XiValue *result = xi_const_int(function, entry, 0, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = build_target_unit_fixture_semantic(function, &semantic, error, sizeof(error));
    if (!built)
        fprintf(stderr, "Map entry iterator semantic failed: %s\n", error);
    REQUIRE(built && semantic != NULL);
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_rune_to_uint32_semantic_with_receiver(XrType *receiver_type,
                                                                   int64_t method_symbol) {
    XiFunc *function = xi_func_new("target_rune_to_uint32", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *receiver = xi_param(function, entry, 0, receiver_type);
    XiValue *to_u32 = xi_value_new(function, entry, XI_CALL_METHOD, &stub_u32, 1);
    REQUIRE(receiver && to_u32);
    function->nparams = function->min_params = 1;
    function->params = (XiValue **) xr_calloc(1, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = receiver;
    to_u32->args[0] = receiver;
    to_u32->aux = (void *) "toUInt32";
    to_u32->aux_int = method_symbol;
    return finish_stringbuilder_semantic(function, entry, "rune.toUInt32");
}

static XrSemanticPlan *build_rune_to_uint32_semantic(void) {
    return build_rune_to_uint32_semantic_with_receiver(&stub_rune,
                                                       (int64_t) XI_METHOD_SYMBOL_TO_UINT32 << 1);
}

static XrSemanticPlan *build_rune_is_whitespace_semantic(void) {
    XiFunc *function = xi_func_new("target_rune_is_whitespace", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(function, entry, "target-rune-whitespace", &stub_exact_string);
    XiValue *runes = xi_value_new(function, entry, XI_CALL_METHOD, &stub_iterator_rune, 1);
    XiValue *next = xi_value_new(function, entry, XI_CALL_METHOD, &stub_rune, 1);
    XiValue *is_whitespace = xi_value_new(function, entry, XI_CALL_METHOD, &stub_bool, 1);
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
    return finish_stringbuilder_semantic(function, entry, "rune.isWhitespace");
}

static XrSemanticPlan *build_string_byte_slice_view_semantic(void) {
    XiFunc *function = xi_func_new("target_string_byte_slice_view", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(function, entry, "target-view", &stub_exact_string);
    XiValue *view = xi_value_new(function, entry, XI_CALL_BUILTIN, &stub_target_u8_slice, 1);
    REQUIRE(source && view);
    view->args[0] = source;
    view->xa_intrinsic_id = XA_INTRINSIC_STRING_BYTE_SLICE_VIEW;
    XiViewSourceEvidence origin = {
        .source_operand = 0,
        .source_param = -1,
        .origin = XI_VIEW_ORIGIN_RECEIVER,
        .lifetime = 1,
    };
    REQUIRE(xi_value_set_view_evidence(function, view, &origin, 1, stub_target_u8.id, 0, 1));
    XiValue *result = xi_const_int(function, entry, 0, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = build_target_unit_fixture_semantic(function, &semantic, error, sizeof(error));
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
    XiModule fixture_module = {
        .identity = "memory-module-v1:id=27:target-plan-unit-fixture-v1",
        .path = "target-plan-unit-fixture.xr",
        .name = "target_plan_unit_fixture",
        .init = function,
    };
    REQUIRE(function->module == NULL);
    function->module = &fixture_module;
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
    function->module = NULL;
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
    REQUIRE(build_target_unit_fixture_semantic(function, &plan, error, sizeof(error)));
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
    bool built = build_target_unit_fixture_semantic(root, &plan, error, sizeof(error));
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
    REQUIRE(build_target_unit_fixture_semantic(function, &plan, error, sizeof(error)));
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
    REQUIRE(build_target_unit_fixture_semantic(function, &plan, error, sizeof(error)));
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
    bool built = build_target_unit_fixture_semantic(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "nested aggregate semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_unit_enum_aggregate_semantic(XrEnumLayout **out_layout) {
    XrType enum_type;
    XrEnumLayout *layout =
        make_unit_enum_type(&enum_type, 122, "tests/aggregate", "AggregateOrdinal");
    XrType fixed = {
        .kind = XR_KIND_FIXED_ARRAY,
        .id = 123,
        .frozen = true,
        .is_value_type = true,
        .scalar_rep = XR_SCALAR_REP_NONE,
    };
    fixed.fixed_array.element_type = &enum_type;
    fixed.fixed_array.length = 2;
    XiFunc *function = xi_func_new("target_unit_enum_aggregate_probe", &fixed);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(function != NULL && entry != NULL);
    XiValue *result = xi_value_new(function, entry, XI_FIXED_ARRAY_NEW, &fixed, 0);
    REQUIRE(result != NULL);
    result->aux_int = 2;
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = build_target_unit_fixture_semantic(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "unit-enum aggregate semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(function);
    *out_layout = layout;
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
    function->is_module_initializer = true;
    XiModule *module =
        xi_module_new("pkg/target_struct_aggregate.xr", "target_struct_aggregate", function);
    REQUIRE(module != NULL);
    REQUIRE(xi_module_set_identity(module, "memory-module-v1:id=26:target-struct-aggregate-v1"));
    function->module = module;
    module->classes = (XiClassData **) xr_malloc(sizeof(*module->classes));
    REQUIRE(module->classes != NULL);
    module->classes[0] = &declaration;
    module->nclasses = 1;
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *class_declaration = xi_value_new(function, entry, XI_CLASS_CREATE, &class_object, 0);
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

static void resign_mutated_semantic_target(XrSemanticPlan *semantic, XrTargetPlan *plan) {
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    semantic->ownership->semantic_fingerprint = semantic->fingerprint;
    semantic->ownership->fingerprint = semantic->fingerprint;
    plan->semantic_fingerprint = semantic->fingerprint;
    for (uint32_t i = 0; i < plan->layouts_count; i++)
        xr_target_layout_compute_fingerprint(plan, i, &plan->layouts[i].fingerprint);
    for (uint32_t i = 0; i < plan->calls_count; i++)
        xr_target_call_compute_fingerprint(plan, i, &plan->calls[i].fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
}

static uint8_t *target_test_xtp_directory_entry(uint8_t *bytes, XrXtpSectionKind kind) {
    return bytes + XR_XTP_HEADER_SIZE + ((size_t) kind - 1u) * XR_XTP_DIRECTORY_ENTRY_SIZE;
}

static void target_test_xtp_resign_section(uint8_t *bytes, XrXtpSectionKind kind) {
    uint8_t *entry = target_test_xtp_directory_entry(bytes, kind);
    size_t offset = (size_t) xr_xtp_take_u64(entry + 8);
    size_t length = (size_t) xr_xtp_take_u64(entry + 16);
    xr_sha256(bytes + offset, length, entry + 40);
}

static void target_test_xtp_resign_artifact(uint8_t *bytes, size_t size) {
    static const uint8_t zero[XR_FINGERPRINT_BYTES] = {0};
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, bytes, XR_XTP_FULL_DIGEST_OFFSET);
    xr_sha256_update(&context, zero, sizeof(zero));
    xr_sha256_update(&context, bytes + XR_XTP_FULL_DIGEST_OFFSET + sizeof(zero),
                     size - XR_XTP_FULL_DIGEST_OFFSET - sizeof(zero));
    xr_sha256_final(&context, bytes + XR_XTP_FULL_DIGEST_OFFSET);
}

static XrSemanticPlan *build_unit_enum_semantic(XrEnumLayout **out_layout) {
    XrType enum_type;
    XrEnumLayout *layout = make_unit_enum_type(&enum_type, 18, "stdlib/base64", "Base64Alphabet");
    XiFunc *function = xi_func_new("target_source_enum_probe", &stub_int);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(function != NULL && entry != NULL);
    XiValue *ordinal = xi_param(function, entry, 0, &enum_type);
    XiValue *other_scalar = xi_const_bool(function, entry, true, &stub_bool);
    XiValue *result = xi_const_int(function, entry, 0, &stub_int);
    REQUIRE(ordinal != NULL && other_scalar != NULL && result != NULL);
    function->nparams = function->min_params = 1;
    function->params = (XiValue **) xr_calloc(1, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = ordinal;
    function->arc_borrow_sig =
        (XiBorrowSig *) xi_func_arena_alloc(function, (uint32_t) sizeof(*function->arc_borrow_sig));
    REQUIRE(function->arc_borrow_sig != NULL);
    function->arc_borrow_sig->nparams = 1;
    function->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    function->arc_borrow_sig->valid = true;
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XiModule fixture_module = {
        .identity = "memory-module-v1:id=27:target-plan-unit-fixture-v1",
        .path = "target-plan-unit-fixture.xr",
        .name = "target_plan_unit_fixture",
        .init = function,
    };
    REQUIRE(function->module == NULL);
    function->module = &fixture_module;
    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    function->module = NULL;
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
    uint32_t enum_type_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < semantic->type_count; i++)
        if (semantic->types[i].kind == XR_KIND_ENUM) {
            enum_type = &semantic->types[i];
            enum_type_index = i;
        }
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
    REQUIRE(rep->kind == XR_MACHINE_REP_ENUM_ORDINAL && rep->detail < semantic->type_count &&
            &semantic->types[rep->detail] == enum_type && rep->root_kind == XR_TARGET_ROOT_NONE &&
            rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL);
    REQUIRE(rep->detail == enum_type_index);
    uint32_t other_type_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 1; i < semantic->type_count; i++)
        if (i != enum_type_index) {
            other_type_index = i;
            break;
        }
    REQUIRE(other_type_index != XR_SEMANTIC_INDEX_NONE);
    XrTargetMachineRepRecord *ordinary_scalar = NULL;
    for (uint32_t i = 0; i < plan->machine_reps_count; i++)
        if (plan->machine_reps[i].kind == XR_MACHINE_REP_I64)
            ordinary_scalar = &plan->machine_reps[i];
    REQUIRE(ordinary_scalar != NULL && ordinary_scalar->detail == 0);
    uint16_t saved_kind = rep->kind;
    uint32_t saved_detail = rep->detail;
    uint8_t saved_root = rep->root_kind;
    rep->kind = XR_MACHINE_REP_I64;
    expect_verify_failure(plan, "XR_TARGET_1001");
    rep->kind = saved_kind;
    rep->detail = other_type_index;
    expect_verify_failure(plan, "XR_TARGET_1001");
    rep->detail = XR_SEMANTIC_INDEX_NONE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    rep->detail = saved_detail;
    rep->root_kind = XR_TARGET_ROOT_OBJECT;
    expect_verify_failure(plan, "XR_TARGET_1001");
    rep->root_kind = saved_root;
    ordinary_scalar->detail = other_type_index;
    expect_verify_failure(plan, "XR_TARGET_1001");
    ordinary_scalar->detail = 0;
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
    /* Re-anchored because the TargetPlan fingerprint is built on top of the
     * SemanticPlan fingerprint: xr_target_plan_compute_fingerprint hashes
     * plan->semantic_fingerprint, and xr_semantic_plan.c in turn hashes
     * plan->stdlib_registry_fingerprint, which
     * xr_stdlib_metadata_registry_fingerprint derives from every .def entry.
     * Publishing http2, compress, mem, regex and io from .xr bodies renames their
     * entries, so this digest moves even though the fixture imports nothing.
     * Old: 36ad5a6db538aeba86caa7d5c229a5c74fd0db34ce016c8cce964ed978d49679.
     * The canonical-program ownership freeze subsequently re-anchored the
     * SemanticPlan owner registries which this target fingerprint includes.
     * Old ownership-freeze digest:
     * 1b6fd4f3f7ab0f38a264f261835fd21ba56e79ef5f1da4efdc03a474b2298fce. */
    REQUIRE(strcmp(target_hex,
                   "bce8574bb0aad9e5db7433dfc12f880f96295d64a969e5fa2b329209572e4b5f") == 0);

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

static void test_native_target_leaf_scalar_authority(void) {
    XrSemanticPlan *semantic = build_native_target_leaf_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "native target leaf TargetPlan build failed: %s\n", error);
    REQUIRE(built && plan != NULL && xr_target_plan_verify(plan, error, sizeof(error)));

    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(plan, &call_count);
    REQUIRE(calls != NULL && call_count == 1);
    const XrTargetCallRecord *call = &calls[0];
    REQUIRE(call->calling_convention == XR_TARGET_CALL_CONVENTION_NATIVE_TARGET_LEAF_SCALAR &&
            call->target_kind == XR_TARGET_CALL_TARGET_NATIVE_TARGET_LEAF_SCALAR &&
            call->native_leaf == XR_STDLIB_TARGET_LEAF_I64_GETPID &&
            memcmp(call->native_callee_identity.bytes, (const uint8_t[XR_STABLE_ID_BYTES]) {0},
                   XR_STABLE_ID_BYTES) != 0 &&
            call->callee_function == XR_SEMANTIC_INDEX_NONE && call->argument_count == 0 &&
            call->result_mode == XR_TARGET_CALL_VALUE &&
            call->result_ownership == XR_TARGET_CALL_NONE);
    uint32_t argument_count = 0;
    REQUIRE(xr_target_plan_call_arguments(plan, &argument_count) == NULL && argument_count == 0);

    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_function_instructions(plan, call->caller_function, &instruction_count);
    REQUIRE(instructions != NULL && instruction_count == 2 &&
            instructions[0].opcode == XR_TARGET_INSTRUCTION_CALL_NATIVE_LEAF_I64 &&
            instructions[0].immediate_bits == call->id &&
            instructions[1].opcode == XR_TARGET_INSTRUCTION_RETURN_I64);

    static const XrTypedDispatchProvider providers[] = {
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
    };
    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    int64_t first_result = 0;
    for (size_t index = 0; index < sizeof(providers) / sizeof(providers[0]); index++) {
        int64_t result = 0;
        XrTypedDispatchI64Request request = {
            .verified_plan = plan,
            .required_plan_fingerprint = &fingerprint,
            .result = &result,
            .provider = providers[index],
            .function = call->caller_function,
            .argument_count = 0,
        };
        REQUIRE(xr_typed_dispatch_execute_i64(&request) == XR_TYPED_DISPATCH_OK && result > 0 &&
                (index == 0 || result == first_result));
        first_result = result;
    }

    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    XrXtpCandidate *candidate = NULL;
    XrTargetPlan *decoded = NULL;
    REQUIRE(xr_xtp_encode_plan(plan, &encoded, &encoded_size, error, sizeof(error)) &&
            xr_xtp_decode_candidate(encoded, encoded_size, &candidate, error, sizeof(error)) &&
            xr_xtp_materialize_target_plan(candidate, semantic, profile, &decoded, error,
                                           sizeof(error)) &&
            decoded != NULL);
    const XrTargetCallRecord *decoded_calls = xr_target_plan_calls(decoded, &call_count);
    REQUIRE(
        decoded_calls != NULL && call_count == 1 &&
        decoded_calls[0].native_leaf == XR_STDLIB_TARGET_LEAF_I64_GETPID &&
        xr_stable_id_equal(decoded_calls[0].native_callee_identity, call->native_callee_identity) &&
        xr_fingerprint_equal(decoded->fingerprint, plan->fingerprint));
    xr_target_plan_free(decoded);
    xr_xtp_candidate_release(candidate);
    xr_xtp_encoded_free(encoded);

    XrTargetCallRecord *mutable_call = &plan->calls[0];
    uint16_t saved_leaf = mutable_call->native_leaf;
    mutable_call->native_leaf = XR_STDLIB_TARGET_LEAF_COUNT;
    xr_target_call_compute_fingerprint(plan, 0, &mutable_call->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1003");
    mutable_call->native_leaf = saved_leaf;
    xr_target_call_compute_fingerprint(plan, 0, &mutable_call->fingerprint);
    XrStableId saved_native_identity = mutable_call->native_callee_identity;
    mutable_call->native_callee_identity.bytes[0] ^= 1u;
    xr_target_call_compute_fingerprint(plan, 0, &mutable_call->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1003");
    mutable_call->native_callee_identity = saved_native_identity;
    xr_target_call_compute_fingerprint(plan, 0, &mutable_call->fingerprint);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
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

static void test_unit_enum_aggregate_dependency_rep(void) {
    XrEnumLayout *source_layout = NULL;
    XrSemanticPlan *semantic = build_unit_enum_aggregate_semantic(&source_layout);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    uint32_t enum_type = XR_SEMANTIC_INDEX_NONE;
    const XrTargetLayoutRecord *enum_layout = NULL;
    const XrTargetLayoutRecord *fixed_layout = NULL;
    for (uint32_t i = 0; i < semantic->type_count; i++)
        if (semantic->types[i].kind == XR_KIND_ENUM)
            enum_type = i;
    for (uint32_t i = 0; i < plan->layouts_count; i++) {
        const XrSemanticTypeRecord *type =
            xr_semantic_plan_type(semantic, plan->layouts[i].semantic_type);
        if (type && type->kind == XR_KIND_ENUM)
            enum_layout = &plan->layouts[i];
        else if (type && type->kind == XR_KIND_FIXED_ARRAY)
            fixed_layout = &plan->layouts[i];
    }
    REQUIRE(enum_type != XR_SEMANTIC_INDEX_NONE && enum_layout != NULL && fixed_layout != NULL);
    REQUIRE(enum_layout->kind == XR_TARGET_LAYOUT_SCALAR && enum_layout->field_count == 0);
    REQUIRE(fixed_layout->kind == XR_TARGET_LAYOUT_AGGREGATE && fixed_layout->field_count == 2);
    for (uint32_t i = 0; i < fixed_layout->field_count; i++) {
        const XrTargetFieldRecord *field = &plan->fields[fixed_layout->field_begin + i];
        const XrTargetMachineRepRecord *rep = &plan->machine_reps[field->memory_rep];
        REQUIRE(rep->kind == XR_MACHINE_REP_ENUM_ORDINAL && rep->detail == enum_type &&
                field->size == 8 && field->align == 8);
    }
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    xr_enum_layout_free(source_layout);
}

static void test_builder_materializes_struct_and_named_aggregates(void) {
    XrSemanticPlan *semantic = build_struct_and_named_aggregate_semantic(false);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
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
        const XrTargetMachineRepRecord *rep = &plan->machine_reps[plan->value_reps[i].memory_rep];
        if (rep->kind == XR_MACHINE_REP_AGGREGATE && rep->detail == named_layout_index) {
            REQUIRE(named_binding == NULL);
            named_binding = &plan->value_reps[i];
        }
    }
    REQUIRE(named_binding != NULL);
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(plan, xr_target_plan_semantic_plan(plan), profile_fingerprint,
                                     &emission, error, sizeof(error)));
    XrCValueEmissionView aggregate_view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, named_binding->semantic_value, &aggregate_view,
                                          error, sizeof(error)) &&
            aggregate_view.rep == XR_C_VALUE_REP_AGGREGATE &&
            aggregate_view.target_memory_kind == XR_MACHINE_REP_AGGREGATE &&
            aggregate_view.c_type && strncmp(aggregate_view.c_type, "xrt_struct_abi_", 15) == 0);
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    XrTargetFieldRecord *named_first = &plan->fields[named_layout->field_begin];
    XrTargetFieldRecord *named_second = &plan->fields[named_layout->field_begin + 1u];
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
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    named_first->semantic_name = saved_name;
    plan->layouts[named_layout_index].fingerprint = saved_named_fingerprint;
    REQUIRE(xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                      profile_fingerprint, error, sizeof(error)));

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

static XrSemanticPlan *build_direct_local_scalar_calls(uint16_t call_opcode, XrType *value_type,
                                                       XrType *callable_type) {
    XiFunc *root = xi_func_new("target_direct_call_root", value_type);
    XiFunc *child = xi_func_new("target_direct_call_child", value_type);
    REQUIRE(root != NULL && child != NULL);
    XiModule fixture_module = {
        .identity = "memory-module-v1:id=27:target-plan-unit-fixture-v1",
        .path = "target-plan-unit-fixture.xr",
        .name = "target_plan_unit_fixture",
        .init = root,
    };
    REQUIRE(root->module == NULL);
    root->module = &fixture_module;
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
    root->module = NULL;
    xi_func_free(root);
    return plan;
}

static void set_single_parameter_ownership(XiFunc *function, XiOwnership ownership) {
    function->arc_borrow_sig =
        (XiBorrowSig *) xi_func_arena_alloc(function, (uint32_t) sizeof(*function->arc_borrow_sig));
    REQUIRE(function->arc_borrow_sig != NULL);
    function->arc_borrow_sig->nparams = 1;
    function->arc_borrow_sig->param_own[0] = ownership;
    function->arc_borrow_sig->valid = true;
}

static XrSemanticPlan *build_direct_local_scalar_ref_semantic(void) {
    XiFunc *root = xi_func_new("target_scalar_ref_root", &stub_int);
    XiFunc *child = xi_func_new("target_scalar_ref_child", &stub_int);
    XiFunc *decoy = xi_func_new("target_scalar_ref_decoy", &stub_int);
    REQUIRE(root != NULL && child != NULL && decoy != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    XiBlock *decoy_entry = xi_block_new(decoy);
    REQUIRE(root_entry != NULL && child_entry != NULL && decoy_entry != NULL);

    root->nparams = root->min_params = 1;
    child->nparams = child->min_params = 1;
    decoy->nparams = decoy->min_params = 1;
    root->params = (XiValue **) xr_calloc(1, sizeof(*root->params));
    child->params = (XiValue **) xr_calloc(1, sizeof(*child->params));
    decoy->params = (XiValue **) xr_calloc(1, sizeof(*decoy->params));
    REQUIRE(root->params != NULL && child->params != NULL && decoy->params != NULL);
    root->params[0] = xi_param(root, root_entry, 0, &stub_int);
    child->params[0] = xi_param(child, child_entry, 0, &stub_int);
    decoy->params[0] = xi_param(decoy, decoy_entry, 0, &stub_int);
    REQUIRE(root->params[0] != NULL && child->params[0] != NULL && decoy->params[0] != NULL);
    REQUIRE(xi_func_set_param_passing_mode(child, 0, XR_PARAM_REF));
    root->params[0]->transfer_mode = XR_TRANSFER_SHARE;
    child->params[0]->transfer_mode = XR_TRANSFER_SHARE;
    decoy->params[0]->transfer_mode = XR_TRANSFER_SHARE;
    set_single_parameter_ownership(root, XI_OWN_NONE);
    set_single_parameter_ownership(child, XI_OWN_NONE);
    set_single_parameter_ownership(decoy, XI_OWN_NONE);

    XiValue *child_result = xi_const_int(child, child_entry, 7, &stub_int);
    REQUIRE(child_result != NULL);
    xi_block_set_return(child_entry, child_result);

    /* The decoy deliberately assigns the same Xi-local id to a LOCAL_ADDR as
     * the caller below. SemanticPlan globalizes both through each function's
     * value_begin, so neither TargetPlan materialization nor lookup may confuse
     * them. */
    XiValue *decoy_copy = xi_value_new(decoy, decoy_entry, XI_COPY, &stub_int, 1);
    XiValue *decoy_place = xi_value_new(decoy, decoy_entry, XI_LOCAL_ADDR, &stub_int, 1);
    XiValue *decoy_result = xi_const_int(decoy, decoy_entry, 0, &stub_int);
    REQUIRE(decoy_copy != NULL && decoy_place != NULL && decoy_result != NULL);
    decoy_copy->args[0] = decoy->params[0];
    decoy_copy->aux_int = XI_COPY_KIND_IDENTITY;
    decoy_place->args[0] = decoy_copy;
    xi_block_set_return(decoy_entry, decoy_result);

    root->children = (XiFunc **) xr_calloc(2, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->children[1] = decoy;
    root->nchildren = root->children_cap = 2;
    child->parent_func = root;
    decoy->parent_func = root;

    XiValue *closure = xi_value_new(root, root_entry, XI_STACK_ALLOC, &stub_function, 0);
    XiValue *place = xi_value_new(root, root_entry, XI_LOCAL_ADDR, &stub_int, 1);
    XiValue *call = xi_value_new(root, root_entry, XI_CALL, &stub_int, 2);
    REQUIRE(closure != NULL && place != NULL && call != NULL && place->id == decoy_place->id);
    closure->aux_int = XI_CLOSURE_NEW;
    closure->aux = child;
    place->args[0] = root->params[0];
    call->args[0] = closure;
    call->args[1] = place;

    XiCallPlan *call_plan = (XiCallPlan *) xi_func_arena_alloc(root, (uint32_t) sizeof(*call_plan));
    XiCallArgPlan *argument_plan =
        (XiCallArgPlan *) xi_func_arena_alloc(root, (uint32_t) sizeof(*argument_plan));
    REQUIRE(call_plan != NULL && argument_plan != NULL);
    memset(call_plan, 0, sizeof(*call_plan));
    memset(argument_plan, 0, sizeof(*argument_plan));
    argument_plan->param_mode = XR_PARAM_REF;
    argument_plan->access = XR_CALL_ARG_REF;
    argument_plan->origin = XI_PLACE_ORIGIN_STACK_LOCAL;
    argument_plan->lifetime = XI_PLACE_LIFETIME_CALL_BOUND;
    argument_plan->escape = XI_PLACE_ESCAPE_NONE;
    argument_plan->addressable = true;
    argument_plan->origin_var_id = 0;
    argument_plan->place = place;
    call_plan->args = argument_plan;
    call_plan->nargs = 1;
    call_plan->verified = true;
    call->call_plan = call_plan;
    xi_block_set_return(root_entry, call);
    root->stage = child->stage = decoy->stage = XI_STAGE_OPTIMIZED;

    XiModule fixture_module = {
        .identity = "memory-module-v1:id=28:target-scalar-ref-fixture-v1",
        .path = "target-scalar-ref-fixture.xr",
        .name = "target_scalar_ref_fixture",
        .init = root,
    };
    root->module = &fixture_module;
    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "direct-local scalar ref semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL && xr_semantic_plan_call_target_count(plan) == 1);
    root->module = NULL;
    xi_func_free(root);
    return plan;
}

static XrSemanticPlan *build_direct_local_class_argument_semantic(XiOwnership callee_ownership) {
    XiFunc *root = xi_func_new("target_class_argument_root", &stub_int);
    XiFunc *child = xi_func_new("target_class_argument_child", &stub_int);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);

    root->nparams = root->min_params = 1;
    child->nparams = child->min_params = 1;
    root->params = (XiValue **) xr_calloc(1, sizeof(*root->params));
    child->params = (XiValue **) xr_calloc(1, sizeof(*child->params));
    REQUIRE(root->params != NULL && child->params != NULL);
    root->params[0] = xi_param(root, root_entry, 0, &stub_target_source_instance);
    child->params[0] = xi_param(child, child_entry, 0, &stub_target_source_instance);
    REQUIRE(root->params[0] != NULL && child->params[0] != NULL);
    set_single_parameter_ownership(root, XI_OWN_OWNED);
    set_single_parameter_ownership(child, callee_ownership);

    XiValue *child_result = xi_const_int(child, child_entry, 7, &stub_int);
    REQUIRE(child_result != NULL);
    xi_block_set_return(child_entry, child_result);
    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;

    XiValue *closure = xi_value_new(root, root_entry, XI_STACK_ALLOC, &stub_function, 0);
    XiValue *call = xi_value_new(root, root_entry, XI_CALL, &stub_int, 2);
    REQUIRE(closure != NULL && call != NULL);
    closure->aux_int = XI_CLOSURE_NEW;
    closure->aux = child;
    call->args[0] = closure;
    call->args[1] = root->params[0];
    xi_block_set_return(root_entry, call);
    root->stage = child->stage = XI_STAGE_OPTIMIZED;

    XiModule *module = xi_module_new("pkg/target_class_argument.xr", "target_class_argument", root);
    REQUIRE(module != NULL);
    REQUIRE(xi_module_set_identity(module, "memory-module-v1:id=24:target-class-argument-v1"));
    root->module = module;
    XiClassData source_class = {
        .class_info = &stub_target_source_class_info,
        .class_name = "FinalTargetWorker",
        .explicit_final = true,
        .needs_runtime_type = true,
    };
    module->classes = (XiClassData **) xr_malloc(sizeof(*module->classes));
    REQUIRE(module->classes != NULL);
    module->classes[0] = &source_class;
    module->nclasses = 1;

    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "direct-local class argument semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL && xr_semantic_plan_call_target_count(plan) == 1);
    root->module = NULL;
    xi_func_free(root);
    module->init = NULL;
    xi_module_free(module);
    return plan;
}

static XrSemanticPlan *build_direct_local_tagged_ref_semantic(XrType *value_type,
                                                              bool register_source_class,
                                                              bool append_stringbuilder,
                                                              bool forward_ref_parameter) {
    XiFunc *root = xi_func_new("target_array_ref_root", &stub_int);
    XiFunc *child = xi_func_new("target_array_ref_child", &stub_int);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);

    root->nparams = root->min_params = append_stringbuilder ? 0 : 1;
    child->nparams = child->min_params = 1;
    if (!append_stringbuilder)
        root->params = (XiValue **) xr_calloc(1, sizeof(*root->params));
    child->params = (XiValue **) xr_calloc(1, sizeof(*child->params));
    REQUIRE((append_stringbuilder || root->params != NULL) && child->params != NULL);
    if (!append_stringbuilder)
        root->params[0] = xi_param(root, root_entry, 0, value_type);
    child->params[0] = xi_param(child, child_entry, 0, value_type);
    REQUIRE((append_stringbuilder || root->params[0] != NULL) && child->params[0] != NULL);
    REQUIRE(xi_func_set_param_passing_mode(child, 0, XR_PARAM_REF));
    if (forward_ref_parameter)
        REQUIRE(xi_func_set_param_passing_mode(root, 0, XR_PARAM_REF));
    if (!append_stringbuilder)
        root->params[0]->transfer_mode = XR_TRANSFER_SHARE;
    child->params[0]->transfer_mode = XR_TRANSFER_SHARE;
    if (!append_stringbuilder)
        set_single_parameter_ownership(root, XI_OWN_BORROWED);
    set_single_parameter_ownership(child, XI_OWN_BORROWED);
    if (append_stringbuilder) {
        XiValue *receiver = xi_value_new(child, child_entry, XI_PLACE_LOAD, value_type, 1);
        XiValue *rune = xi_const_rune(child, child_entry, 'x', &stub_rune);
        XiValue *append = xi_value_new(child, child_entry, XI_CALL_METHOD, value_type, 2);
        REQUIRE(receiver != NULL && rune != NULL && append != NULL);
        receiver->args[0] = child->params[0];
        append->args[0] = receiver;
        append->args[1] = rune;
        append->xa_intrinsic_id = XA_INTRINSIC_STRING_BUILDER_APPEND;
        append->aux = (void *) XA_INTRINSIC_STRING_BUILDER_APPEND_SOURCE_MEMBER;
        append->aux_int = (int64_t) XI_METHOD_SYMBOL_APPEND << 1;
        append->result_alias_operand = 0;
    }
    XiValue *child_result = xi_const_int(child, child_entry, 7, &stub_int);
    REQUIRE(child_result != NULL);
    xi_block_set_return(child_entry, child_result);

    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;

    XiValue *root_value =
        append_stringbuilder ? emit_exact_string_builder(root, root_entry) : root->params[0];
    XiValue *closure = xi_value_new(root, root_entry, XI_STACK_ALLOC, &stub_function, 0);
    XiValue *place = forward_ref_parameter
                         ? root_value
                         : xi_value_new(root, root_entry, XI_LOCAL_ADDR, value_type, 1);
    XiValue *call = xi_value_new(root, root_entry, XI_CALL, &stub_int, 2);
    REQUIRE(closure != NULL && place != NULL && call != NULL);
    closure->aux_int = XI_CLOSURE_NEW;
    closure->aux = child;
    if (!forward_ref_parameter)
        place->args[0] = root_value;
    call->args[0] = closure;
    call->args[1] = place;

    XiCallPlan *call_plan = (XiCallPlan *) xi_func_arena_alloc(root, (uint32_t) sizeof(*call_plan));
    XiCallArgPlan *argument_plan =
        (XiCallArgPlan *) xi_func_arena_alloc(root, (uint32_t) sizeof(*argument_plan));
    REQUIRE(call_plan != NULL && argument_plan != NULL);
    memset(call_plan, 0, sizeof(*call_plan));
    memset(argument_plan, 0, sizeof(*argument_plan));
    argument_plan->param_mode = XR_PARAM_REF;
    argument_plan->access = XR_CALL_ARG_REF;
    argument_plan->origin =
        forward_ref_parameter ? XI_PLACE_ORIGIN_PARAM : XI_PLACE_ORIGIN_STACK_LOCAL;
    argument_plan->lifetime = XI_PLACE_LIFETIME_CALL_BOUND;
    argument_plan->escape = XI_PLACE_ESCAPE_NONE;
    argument_plan->addressable = true;
    argument_plan->origin_var_id = 0;
    argument_plan->place = place;
    call_plan->args = argument_plan;
    call_plan->nargs = 1;
    call_plan->verified = true;
    call->call_plan = call_plan;
    xi_block_set_return(root_entry, call);
    root->stage = child->stage = XI_STAGE_OPTIMIZED;

    XiModule *module = xi_module_new("pkg/target_array_ref.xr", "target_array_ref", root);
    REQUIRE(module != NULL);
    REQUIRE(xi_module_set_identity(module, "memory-module-v1:id=27:target-array-ref-fixture-v1"));
    root->module = module;
    XiClassData source_class = {
        .class_info = &stub_target_source_class_info,
        .class_name = "FinalTargetWorker",
        .explicit_final = true,
        .needs_runtime_type = true,
    };
    if (register_source_class) {
        module->classes = (XiClassData **) xr_malloc(sizeof(*module->classes));
        REQUIRE(module->classes != NULL);
        module->classes[0] = &source_class;
        module->nclasses = 1;
    }

    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "direct-local tagged ref semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL && xr_semantic_plan_call_target_count(plan) == 1);
    root->module = NULL;
    xi_func_free(root);
    module->init = NULL;
    xi_module_free(module);
    return plan;
}

static XrSemanticPlan *build_source_class_array_push_semantic(void) {
    XiFunc *function = xi_func_new("target_source_class_array_push", &stub_unit);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    function->nparams = function->min_params = 2;
    function->params = (XiValue **) xr_calloc(2, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = xi_param(function, entry, 0, &stub_target_source_instance_array);
    function->params[1] = xi_param(function, entry, 1, &stub_target_source_instance);
    REQUIRE(function->params[0] != NULL && function->params[1] != NULL);
    function->arc_borrow_sig =
        (XiBorrowSig *) xi_func_arena_alloc(function, (uint32_t) sizeof(*function->arc_borrow_sig));
    REQUIRE(function->arc_borrow_sig != NULL);
    function->arc_borrow_sig->nparams = 2;
    function->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    function->arc_borrow_sig->param_own[1] = XI_OWN_OWNED;
    function->arc_borrow_sig->valid = true;

    XiValue *push = xi_value_new(function, entry, XI_CALL_METHOD, &stub_unit, 2);
    REQUIRE(push != NULL);
    push->args[0] = function->params[0];
    push->args[1] = function->params[1];
    push->aux = "push";
    push->aux_int = (int64_t) XI_METHOD_SYMBOL_PUSH << 1;
    xi_block_set_return(entry, push);
    function->stage = XI_STAGE_OPTIMIZED;

    XiModule *module = xi_module_new("pkg/target_source_class_array_push.xr",
                                     "target_source_class_array_push", function);
    REQUIRE(module != NULL);
    REQUIRE(
        xi_module_set_identity(module, "memory-module-v1:id=33:target-source-class-array-push-v1"));
    function->module = module;
    XiClassData source_class = {
        .class_info = &stub_target_source_class_info,
        .class_name = "FinalTargetWorker",
        .explicit_final = true,
        .needs_runtime_type = true,
    };
    module->classes = (XiClassData **) xr_malloc(sizeof(*module->classes));
    REQUIRE(module->classes != NULL);
    module->classes[0] = &source_class;
    module->nclasses = 1;

    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "source-class Array.push semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    function->module = NULL;
    xi_func_free(function);
    module->init = NULL;
    xi_module_free(module);
    return plan;
}

static XrSemanticPlan *build_nested_array_push_semantic(void) {
    XiFunc *function = xi_func_new("target_nested_array_push", &stub_unit);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    function->nparams = function->min_params = 2;
    function->params = (XiValue **) xr_calloc(2, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = xi_param(function, entry, 0, &stub_target_string_array_array);
    function->params[1] = xi_param(function, entry, 1, &stub_target_string_array);
    REQUIRE(function->params[0] != NULL && function->params[1] != NULL);
    function->arc_borrow_sig =
        (XiBorrowSig *) xi_func_arena_alloc(function, (uint32_t) sizeof(*function->arc_borrow_sig));
    REQUIRE(function->arc_borrow_sig != NULL);
    function->arc_borrow_sig->nparams = 2;
    function->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    function->arc_borrow_sig->param_own[1] = XI_OWN_OWNED;
    function->arc_borrow_sig->valid = true;

    XiValue *push = xi_value_new(function, entry, XI_CALL_METHOD, &stub_unit, 2);
    REQUIRE(push != NULL);
    push->args[0] = function->params[0];
    push->args[1] = function->params[1];
    push->aux = "push";
    push->aux_int = (int64_t) XI_METHOD_SYMBOL_PUSH << 1;
    xi_block_set_return(entry, push);
    function->stage = XI_STAGE_OPTIMIZED;

    XiModule *module =
        xi_module_new("pkg/target_nested_array_push.xr", "target_nested_array_push", function);
    REQUIRE(module != NULL);
    REQUIRE(xi_module_set_identity(module, "memory-module-v1:id=27:target-nested-array-push-v1"));
    function->module = module;

    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "nested Array.push semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    function->module = NULL;
    xi_func_free(function);
    module->init = NULL;
    xi_module_free(module);
    return plan;
}

static XrSemanticPlan *build_class_field_array_push_semantic(void) {
    XiFunc *function = xi_func_new("target_class_field_array_push", &stub_unit);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    function->nparams = function->min_params = 2;
    function->params = (XiValue **) xr_calloc(2, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = xi_param(function, entry, 0, &stub_target_source_instance);
    function->params[1] = xi_param(function, entry, 1, &stub_target_string_array);
    REQUIRE(function->params[0] != NULL && function->params[1] != NULL);
    REQUIRE(xi_func_set_param_passing_mode(function, 0, XR_PARAM_REF));
    function->params[0]->transfer_mode = XR_TRANSFER_SHARE;
    function->arc_borrow_sig =
        (XiBorrowSig *) xi_func_arena_alloc(function, (uint32_t) sizeof(*function->arc_borrow_sig));
    REQUIRE(function->arc_borrow_sig != NULL);
    function->arc_borrow_sig->nparams = 2;
    function->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    function->arc_borrow_sig->param_own[1] = XI_OWN_OWNED;
    function->arc_borrow_sig->valid = true;

    XiValue *receiver =
        xi_value_new(function, entry, XI_PLACE_LOAD, &stub_target_source_instance, 1);
    XiValue *field =
        xi_value_new(function, entry, XI_LOAD_FIELD, &stub_target_string_array_array, 1);
    XiValue *push = xi_value_new(function, entry, XI_CALL_METHOD, &stub_unit, 2);
    REQUIRE(receiver != NULL && field != NULL && push != NULL);
    receiver->args[0] = function->params[0];
    field->args[0] = receiver;
    field->aux = "children";
    field->aux_int = 0;
    field->xg_class_field_id = 41;
    push->args[0] = field;
    push->args[1] = function->params[1];
    push->aux = "push";
    push->aux_int = (int64_t) XI_METHOD_SYMBOL_PUSH << 1;
    xi_block_set_return(entry, push);
    function->stage = XI_STAGE_OPTIMIZED;

    XiModule *module = xi_module_new("pkg/target_class_field_array_push.xr",
                                     "target_class_field_array_push", function);
    REQUIRE(module != NULL);
    REQUIRE(
        xi_module_set_identity(module, "memory-module-v1:id=32:target-class-field-array-push-v1"));
    function->module = module;
    XiClassData source_class = {
        .class_info = &stub_target_source_class_info,
        .class_name = "FinalTargetWorker",
        .explicit_final = true,
        .needs_runtime_type = true,
    };
    module->classes = (XiClassData **) xr_malloc(sizeof(*module->classes));
    REQUIRE(module->classes != NULL);
    module->classes[0] = &source_class;
    module->nclasses = 1;

    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "class-field Array.push semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    function->module = NULL;
    xi_func_free(function);
    module->init = NULL;
    xi_module_free(module);
    return plan;
}

static XrSemanticPlan *build_source_class_field_result_semantic(void) {
    XiFunc *root = xi_func_new("target_source_class_field_result", &stub_int);
    XiFunc *consumer = xi_func_new("consume_field", &stub_int);
    REQUIRE(root != NULL && consumer != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *consumer_entry = xi_block_new(consumer);
    REQUIRE(root_entry != NULL && consumer_entry != NULL);

    root->nparams = root->min_params = 1;
    consumer->nparams = consumer->min_params = 1;
    root->params = (XiValue **) xr_calloc(1, sizeof(*root->params));
    consumer->params = (XiValue **) xr_calloc(1, sizeof(*consumer->params));
    REQUIRE(root->params != NULL && consumer->params != NULL);
    root->params[0] = xi_param(root, root_entry, 0, &stub_target_source_instance);
    consumer->params[0] = xi_param(consumer, consumer_entry, 0, &stub_exact_string);
    REQUIRE(root->params[0] != NULL && consumer->params[0] != NULL);
    REQUIRE(xi_func_set_param_passing_mode(root, 0, XR_PARAM_REF));
    root->params[0]->transfer_mode = XR_TRANSFER_SHARE;
    consumer->params[0]->transfer_mode = XR_TRANSFER_SHARE;
    set_single_parameter_ownership(root, XI_OWN_BORROWED);
    set_single_parameter_ownership(consumer, XI_OWN_BORROWED);

    XiValue *consumer_result = xi_const_int(consumer, consumer_entry, 1, &stub_int);
    REQUIRE(consumer_result != NULL);
    xi_block_set_return(consumer_entry, consumer_result);
    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = consumer;
    root->nchildren = root->children_cap = 1;
    consumer->parent_func = root;

    XiValue *receiver =
        xi_value_new(root, root_entry, XI_PLACE_LOAD, &stub_target_source_instance, 1);
    XiValue *field = xi_value_new(root, root_entry, XI_LOAD_FIELD, &stub_exact_string, 1);
    XiValue *closure = xi_value_new(root, root_entry, XI_STACK_ALLOC, &stub_function, 0);
    XiValue *call = xi_value_new(root, root_entry, XI_CALL, &stub_int, 2);
    REQUIRE(receiver != NULL && field != NULL && closure != NULL && call != NULL);
    receiver->args[0] = root->params[0];
    field->args[0] = receiver;
    field->aux = "label";
    field->aux_int = 0;
    field->xg_class_field_id = 47;
    closure->aux_int = XI_CLOSURE_NEW;
    closure->aux = consumer;
    call->args[0] = closure;
    call->args[1] = field;
    xi_block_set_return(root_entry, call);
    root->stage = consumer->stage = XI_STAGE_OPTIMIZED;

    XiModule *module = xi_module_new("pkg/target_source_class_field_result.xr",
                                     "target_source_class_field_result", root);
    REQUIRE(module != NULL);
    REQUIRE(xi_module_set_identity(module,
                                   "memory-module-v1:id=35:target-source-class-field-result-v1"));
    root->module = module;
    XiClassData source_class = {
        .class_info = &stub_target_source_class_info,
        .class_name = "FinalTargetWorker",
        .explicit_final = true,
        .needs_runtime_type = true,
    };
    module->classes = (XiClassData **) xr_malloc(sizeof(*module->classes));
    REQUIRE(module->classes != NULL);
    module->classes[0] = &source_class;
    module->nclasses = 1;

    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "source-class field result semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL && xr_semantic_plan_call_target_count(plan) == 1);
    root->module = NULL;
    xi_func_free(root);
    module->init = NULL;
    xi_module_free(module);
    return plan;
}

static XrSemanticPlan *build_source_structural_field_result_semantic(void) {
    const char *field_names[1] = {"label"};
    XrType *field_types[1] = {&stub_exact_string};
    XrType structural = {
        .kind = XR_KIND_STRUCT_OBJECT,
        .id = 147,
        .frozen = true,
        .is_value_type = true,
        .scalar_rep = XR_SCALAR_REP_NONE,
        .object =
            {
                .field_names = field_names,
                .field_types = field_types,
                .field_count = 1,
            },
    };
    XiFunc *root = xi_func_new("target_source_structural_field_result", &stub_int);
    XiFunc *consumer = xi_func_new("consume_structural_field", &stub_int);
    REQUIRE(root != NULL && consumer != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *consumer_entry = xi_block_new(consumer);
    REQUIRE(root_entry != NULL && consumer_entry != NULL);

    consumer->nparams = consumer->min_params = 1;
    consumer->params = (XiValue **) xr_calloc(1, sizeof(*consumer->params));
    REQUIRE(consumer->params != NULL);
    consumer->params[0] = xi_param(consumer, consumer_entry, 0, &stub_exact_string);
    REQUIRE(consumer->params[0] != NULL);
    consumer->params[0]->transfer_mode = XR_TRANSFER_SHARE;
    set_single_parameter_ownership(consumer, XI_OWN_BORROWED);
    XiValue *consumer_result = xi_const_int(consumer, consumer_entry, 1, &stub_int);
    REQUIRE(consumer_result != NULL);
    xi_block_set_return(consumer_entry, consumer_result);

    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = consumer;
    root->nchildren = root->children_cap = 1;
    consumer->parent_func = root;

    XiValue *object = xi_value_new(root, root_entry, XI_OBJECT_NEW, &structural, 0);
    XiValue *field = xi_value_new(root, root_entry, XI_OBJECT_GET_F, &stub_exact_string, 1);
    XiValue *closure = xi_value_new(root, root_entry, XI_STACK_ALLOC, &stub_function, 0);
    XiValue *call = xi_value_new(root, root_entry, XI_CALL, &stub_int, 2);
    REQUIRE(object != NULL && field != NULL && closure != NULL && call != NULL);
    object->aux = (void *) field_names;
    object->aux_int = xi_object_pack_aux(1, 0);
    field->args[0] = object;
    field->aux_int = 0;
    field->xg_object_access_id = 73;
    closure->aux_int = XI_CLOSURE_NEW;
    closure->aux = consumer;
    call->args[0] = closure;
    call->args[1] = field;
    xi_block_set_return(root_entry, call);
    root->stage = consumer->stage = XI_STAGE_OPTIMIZED;

    XiModule *module = xi_module_new("pkg/target_source_structural_field_result.xr",
                                     "target_source_structural_field_result", root);
    REQUIRE(module != NULL);
    REQUIRE(xi_module_set_identity(
        module, "memory-module-v1:id=40:target-source-structural-field-result-v1"));
    root->module = module;

    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "source structural field result semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL && xr_semantic_plan_call_target_count(plan) == 1);
    root->module = NULL;
    xi_func_free(root);
    module->init = NULL;
    xi_module_free(module);
    return plan;
}

static XrSemanticPlan *build_source_class_array_fill_semantic(void) {
    XiFunc *function = xi_func_new("target_source_class_array_fill", &stub_unit);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    function->nparams = function->min_params = 2;
    function->params = (XiValue **) xr_calloc(2, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = xi_param(function, entry, 0, &stub_target_source_instance_array);
    function->params[1] = xi_param(function, entry, 1, &stub_target_source_instance);
    REQUIRE(function->params[0] != NULL && function->params[1] != NULL);
    function->arc_borrow_sig =
        (XiBorrowSig *) xi_func_arena_alloc(function, (uint32_t) sizeof(*function->arc_borrow_sig));
    REQUIRE(function->arc_borrow_sig != NULL);
    function->arc_borrow_sig->nparams = 2;
    function->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    function->arc_borrow_sig->param_own[1] = XI_OWN_OWNED;
    function->arc_borrow_sig->valid = true;

    XiValue *start = xi_const_int(function, entry, 1, &stub_int);
    XiValue *end = xi_const_int(function, entry, 3, &stub_int);
    XiValue *fill =
        xi_value_new(function, entry, XI_CALL_METHOD, &stub_target_source_instance_array, 4);
    REQUIRE(start != NULL && end != NULL && fill != NULL);
    fill->args[0] = function->params[0];
    fill->args[1] = function->params[1];
    fill->args[2] = start;
    fill->args[3] = end;
    fill->aux = "fill";
    fill->aux_int = (int64_t) XI_METHOD_SYMBOL_FILL << 1;
    fill->result_alias_operand = 0;
    xi_block_set_return(entry, NULL);
    function->stage = XI_STAGE_OPTIMIZED;

    XiModule *module = xi_module_new("pkg/target_source_class_array_fill.xr",
                                     "target_source_class_array_fill", function);
    REQUIRE(module != NULL);
    REQUIRE(
        xi_module_set_identity(module, "memory-module-v1:id=33:target-source-class-array-fill-v1"));
    function->module = module;
    XiClassData source_class = {
        .class_info = &stub_target_source_class_info,
        .class_name = "FinalTargetWorker",
        .explicit_final = true,
        .needs_runtime_type = true,
    };
    module->classes = (XiClassData **) xr_malloc(sizeof(*module->classes));
    REQUIRE(module->classes != NULL);
    module->classes[0] = &source_class;
    module->nclasses = 1;

    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "source-class Array.fill semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    function->module = NULL;
    xi_func_free(function);
    module->init = NULL;
    xi_module_free(module);
    return plan;
}

static void dispose_inplace_array(XrArray *array) {
    REQUIRE(array != NULL && array->storage == NULL && !xr_array_is_slice(array));
    while (array->length > 0)
        (void) xr_array_pop(array);
    xr_free(array->data);
    array->data = NULL;
    array->capacity = 0;
}

static void test_source_class_array_push_managed_execution(const XrTargetPlan *plan) {
    REQUIRE(plan != NULL);
    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    static const XrTypedDispatchProvider providers[] = {
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
    };
    for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
        XrObjHeader instance = {0};
        xr_obj_header_init_type(&instance, XR_TINSTANCE);
        XrValue element = xr_value_from_instance((struct XrObjectInstance *) (void *) &instance);

        XrArray success = {0};
        xr_obj_header_init_type(&success.hdr, XR_TARRAY);
        xr_array_init_inplace(&success, 1, XR_ELEM_ANY);
        XrValue success_arguments[2] = {xr_value_from_array(&success), element};
        XrValue borrowed_receiver = success_arguments[0];
        XrTypedDispatchValueRequest request = {
            .verified_plan = plan,
            .required_plan_fingerprint = &fingerprint,
            .arguments = success_arguments,
            .array_push = xr_array_push_owned_checked,
            .provider = providers[i],
            .function = 0,
            .argument_count = 2,
        };
        request.array_push = NULL;
        REQUIRE(xr_typed_dispatch_execute_values(&request) == XR_TYPED_DISPATCH_INVALID_ARGUMENT &&
                memcmp(&success_arguments[0], &borrowed_receiver, sizeof(borrowed_receiver)) == 0 &&
                memcmp(&success_arguments[1], &element, sizeof(element)) == 0 &&
                success.length == 0);
        request.array_push = xr_array_push_owned_checked;
        REQUIRE(xr_typed_dispatch_execute_values(&request) == XR_TYPED_DISPATCH_OK &&
                memcmp(&success_arguments[0], &borrowed_receiver, sizeof(borrowed_receiver)) == 0 &&
                XR_IS_NULL(success_arguments[1]) && success.length == 1 &&
                success.contains_refs == 1 &&
                memcmp(&((XrValue *) success.data)[0], &element, sizeof(element)) == 0);
        XrValue returned = xr_array_pop(&success);
        REQUIRE(memcmp(&returned, &element, sizeof(element)) == 0);
        dispose_inplace_array(&success);

        XrArray slice = {0};
        xr_obj_header_init_type(&slice.hdr, XR_TARRAY);
        xr_array_init_inplace(&slice, 1, XR_ELEM_ANY);
        slice.data_storage = XR_ARRAY_DATA_BORROWED;
        XrValue slice_arguments[2] = {xr_value_from_array(&slice), element};
        request.arguments = slice_arguments;
        REQUIRE(xr_typed_dispatch_execute_values(&request) == XR_TYPED_DISPATCH_ARRAY_PUSH_SLICE &&
                memcmp(&slice_arguments[1], &element, sizeof(element)) == 0 && slice.length == 0 &&
                slice.capacity == 1);
        slice.data_storage = XR_ARRAY_DATA_HEAP;
        dispose_inplace_array(&slice);

        XrArray wrong_storage = {0};
        xr_obj_header_init_type(&wrong_storage.hdr, XR_TARRAY);
        xr_array_init_inplace(&wrong_storage, 1, XR_ELEM_I64);
        XrValue mismatch_arguments[2] = {xr_value_from_array(&wrong_storage), element};
        request.arguments = mismatch_arguments;
        REQUIRE(xr_typed_dispatch_execute_values(&request) ==
                    XR_TYPED_DISPATCH_ARRAY_PUSH_TYPE_MISMATCH &&
                memcmp(&mismatch_arguments[1], &element, sizeof(element)) == 0 &&
                wrong_storage.length == 0 && wrong_storage.capacity == 1);
        dispose_inplace_array(&wrong_storage);

        XrValue invalid_arguments[2] = {xr_int(7), element};
        request.arguments = invalid_arguments;
        REQUIRE(xr_typed_dispatch_execute_values(&request) ==
                    XR_TYPED_DISPATCH_ARRAY_PUSH_INVALID_RECEIVER &&
                memcmp(&invalid_arguments[1], &element, sizeof(element)) == 0);

        XrArray element_mismatch = {0};
        xr_obj_header_init_type(&element_mismatch.hdr, XR_TARRAY);
        xr_array_init_inplace(&element_mismatch, 1, XR_ELEM_ANY);
        XrValue non_instance_arguments[2] = {xr_value_from_array(&element_mismatch), xr_int(9)};
        request.arguments = non_instance_arguments;
        REQUIRE(xr_typed_dispatch_execute_values(&request) ==
                    XR_TYPED_DISPATCH_ARRAY_PUSH_TYPE_MISMATCH &&
                XR_IS_INT(non_instance_arguments[1]) && XR_TO_INT(non_instance_arguments[1]) == 9 &&
                element_mismatch.length == 0);
        dispose_inplace_array(&element_mismatch);
    }
}

static void test_nested_array_push_managed_execution(const XrTargetPlan *plan) {
    REQUIRE(plan != NULL);
    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    static const XrTypedDispatchProvider providers[] = {
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
    };
    for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
        XrArray inner = {0};
        XrArray outer = {0};
        xr_obj_header_init_type(&inner.hdr, XR_TARRAY);
        xr_obj_header_init_type(&outer.hdr, XR_TARRAY);
        xr_array_init_inplace(&inner, 1, XR_ELEM_ANY);
        xr_array_init_inplace(&outer, 1, XR_ELEM_ANY);
        XrValue element = xr_value_from_array(&inner);
        XrValue arguments[2] = {xr_value_from_array(&outer), element};
        XrValue receiver = arguments[0];
        XrTypedDispatchValueRequest request = {
            .verified_plan = plan,
            .required_plan_fingerprint = &fingerprint,
            .arguments = arguments,
            .array_push = xr_array_push_owned_checked,
            .provider = providers[i],
            .function = 0,
            .argument_count = 2,
        };
        XrObjHeader wrong_instance = {0};
        xr_obj_header_init_type(&wrong_instance, XR_TINSTANCE);
        XrValue hostile_arguments[2] = {
            receiver,
            xr_value_from_instance((struct XrObjectInstance *) (void *) &wrong_instance),
        };
        request.arguments = hostile_arguments;
        REQUIRE(xr_typed_dispatch_execute_values(&request) ==
                    XR_TYPED_DISPATCH_ARRAY_PUSH_TYPE_MISMATCH &&
                outer.length == 0 && !XR_IS_NULL(hostile_arguments[1]));
        request.arguments = arguments;
        REQUIRE(xr_typed_dispatch_execute_values(&request) == XR_TYPED_DISPATCH_OK &&
                memcmp(&arguments[0], &receiver, sizeof(receiver)) == 0 &&
                XR_IS_NULL(arguments[1]) && outer.length == 1 && outer.contains_refs == 1 &&
                memcmp(&((XrValue *) outer.data)[0], &element, sizeof(element)) == 0);
        XrValue returned = xr_array_pop(&outer);
        REQUIRE(memcmp(&returned, &element, sizeof(element)) == 0);
        dispose_inplace_array(&outer);
        dispose_inplace_array(&inner);
    }
}

static XrSemanticPlan *build_channel_method_semantic(const char *selector,
                                                     int64_t selector_immediate,
                                                     XrType *receiver_type, XrType *result_type,
                                                     bool extra_argument) {
    XiFunc *function = xi_func_new("target_channel_close", &stub_unit);
    REQUIRE(function != NULL);
    XiModule fixture_module = {
        .identity = "memory-module-v1:id=27:target-plan-unit-fixture-v1",
        .path = "target-plan-unit-fixture.xr",
        .name = "target_plan_unit_fixture",
        .init = function,
    };
    REQUIRE(function->module == NULL);
    function->module = &fixture_module;
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
    function->module = NULL;
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
    bool built = build_target_unit_fixture_semantic(function, &plan, error, sizeof(error));
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

    XrType enum_type;
    XrEnumLayout *enum_layout =
        make_unit_enum_type(&enum_type, 124, "tests/channel", "ChannelOrdinal");
    XrType enum_channel = {
        .kind = XR_KIND_CHANNEL,
        .id = 125,
        .frozen = true,
        .scalar_rep = XR_SCALAR_REP_NONE,
        .container = {.element_type = &enum_type},
    };
    semantic = build_channel_receive_semantic(&enum_channel, &enum_type, false);
    plan = NULL;
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    receive = NULL;
    for (uint32_t i = 0; i < semantic->operation_count; i++)
        if (semantic->operations[i].opcode == XI_CHAN_TRY_RECV)
            receive = &semantic->operations[i];
    REQUIRE(receive != NULL);
    binding = xr_target_plan_value_rep(plan, receive->result_value);
    REQUIRE(binding != NULL);
    const XrTargetMachineRepRecord *enum_rep = &plan->machine_reps[binding->memory_rep];
    REQUIRE(enum_rep->kind == XR_MACHINE_REP_ENUM_ORDINAL &&
            enum_rep->detail == receive->result_type);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_enum_layout_free(enum_layout);

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

typedef enum SourceExportArgumentKind {
    SOURCE_EXPORT_ARGUMENT_NONE = 0,
    SOURCE_EXPORT_ARGUMENT_VALUE,
    SOURCE_EXPORT_ARGUMENT_RAW_POINTER_REFERENCE,
    SOURCE_EXPORT_ARGUMENT_EXACT_I64,
    SOURCE_EXPORT_ARGUMENT_STRING,
} SourceExportArgumentKind;

static XrSemanticPlan *build_source_export_semantic(XrSemanticPlan **dependency_out,
                                                    SourceExportArgumentKind argument_kind) {
    bool with_argument = argument_kind != SOURCE_EXPORT_ARGUMENT_NONE;
    bool reference = argument_kind == SOURCE_EXPORT_ARGUMENT_RAW_POINTER_REFERENCE;
    bool exact_i64 = argument_kind == SOURCE_EXPORT_ARGUMENT_EXACT_I64;
    bool exact_string = argument_kind == SOURCE_EXPORT_ARGUMENT_STRING;
    XiFunc *dependency_root = xi_func_new("net_init", &stub_unit);
    XiFunc *write_bytes =
        xi_func_new(exact_string ? "trim" : "writeBytes",
                    exact_string ? &stub_exact_string : (exact_i64 ? &stub_int : &stub_unit));
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
        write_bytes->params = (XiValue **) xr_calloc(1, sizeof(*write_bytes->params));
        REQUIRE(write_bytes->params);
        write_bytes->params[0] = xi_param(write_bytes, write_entry, 0,
                                          reference      ? &stub_raw_pointer
                                          : exact_string ? &stub_exact_string
                                          : exact_i64    ? &stub_int
                                                         : &stub_bool);
        REQUIRE(write_bytes->params[0]);
        if (reference)
            REQUIRE(xi_func_set_param_passing_mode(write_bytes, 0, XR_PARAM_REF));
        if (reference || exact_string) {
            write_bytes->params[0]->transfer_mode = XR_TRANSFER_SHARE;
            write_bytes->arc_borrow_sig = (XiBorrowSig *) xi_func_arena_alloc(
                write_bytes, (uint32_t) sizeof(*write_bytes->arc_borrow_sig));
            REQUIRE(write_bytes->arc_borrow_sig);
            write_bytes->arc_borrow_sig->nparams = 1;
            write_bytes->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
            write_bytes->arc_borrow_sig->valid = true;
        }
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
    XiValue *owned_string_left =
        exact_string ? xi_const_str(write_bytes, write_entry, "trim", &stub_exact_string) : NULL;
    XiValue *owned_string_right =
        exact_string ? xi_const_str(write_bytes, write_entry, "med", &stub_exact_string) : NULL;
    XiValue *owned_string_result =
        exact_string ? xi_value_new(write_bytes, write_entry, XI_STR_CONCAT, &stub_exact_string, 2)
                     : NULL;
    REQUIRE(!exact_string || (owned_string_left && owned_string_right && owned_string_result));
    if (exact_string) {
        owned_string_result->args[0] = owned_string_left;
        owned_string_result->args[1] = owned_string_right;
    }
    xi_block_set_return(write_entry, exact_string ? owned_string_result
                                     : exact_i64  ? write_bytes->params[0]
                                                  : NULL);
    dependency_root->stage = write_bytes->stage = XI_STAGE_SEMANTIC_LOWERED;
    dependency_root->invariant_mask = write_bytes->invariant_mask =
        xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(dependency_root, NULL));
    dependency_root->stage = write_bytes->stage = XI_STAGE_OPTIMIZED;
    XiModule *dependency_module = xi_module_new("stdlib/net/net.xr", "net", dependency_root);
    REQUIRE(dependency_module);
    REQUIRE(xi_module_set_identity(dependency_module,
                                   "memory-module-v1:id=27:source-export-dependency-v1"));
    dependency_root->module = dependency_module;
    dependency_module->nslots = 1;
    dependency_module->nexports = 1;
    dependency_module->exports =
        (XiModuleExport *) xr_calloc(1, sizeof(*dependency_module->exports));
    REQUIRE(dependency_module->exports);
    dependency_module->exports[0].name = exact_string ? "trim" : "writeBytes";
    dependency_module->exports[0].shared_slot = 0;
    dependency_module->exports[0].function = write_bytes;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(dependency_root, error, sizeof(error)));
    XrSemanticPlan *dependency = xr_semantic_plan_retain(dependency_root->semantic_plan);
    REQUIRE(dependency && xr_semantic_plan_source_export_count(dependency) == 1);

    XiFunc *caller_root = xi_func_new("http_init", &stub_unit);
    XiFunc *caller =
        xi_func_new(exact_string ? "trim_user" : "_serverWriteAll",
                    exact_string ? &stub_exact_string : (exact_i64 ? &stub_int : &stub_unit));
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
    caller_root->nshared = reference ? 2 : 1;
    xi_block_set_return(root_entry, NULL);
    XiValue *receiver =
        xi_value_new(caller, caller_entry, XI_GET_SHARED, &stub_module_namespace, 0);
    XiValue *receiver_alias =
        xi_value_new(caller, caller_entry, XI_COPY, &stub_module_namespace, 1);
    XiValue *argument_storage = NULL;
    XiValue *argument = NULL;
    if (argument_kind == SOURCE_EXPORT_ARGUMENT_VALUE) {
        argument = xi_const_bool(caller, caller_entry, true, &stub_bool);
    } else if (exact_string) {
        argument = xi_const_str(caller, caller_entry, " source ", &stub_exact_string);
    } else if (exact_i64) {
        argument = xi_const_int(caller, caller_entry, 41, &stub_int);
    } else if (reference) {
        argument_storage = xi_value_new(caller, caller_entry, XI_GET_SHARED, &stub_raw_pointer, 0);
        argument = xi_value_new(caller, caller_entry, XI_LOCAL_ADDR, &stub_raw_pointer, 1);
        REQUIRE(argument_storage && argument);
        argument_storage->aux_int = 1;
        argument->args[0] = argument_storage;
    }
    XiValue *method =
        xi_value_new(caller, caller_entry, XI_CALL_METHOD,
                     exact_string ? &stub_exact_string : (exact_i64 ? &stub_int : &stub_unit),
                     with_argument ? 2 : 1);
    REQUIRE(receiver && receiver_alias && (!with_argument || argument) && method);
    receiver->aux_int = 0;
    receiver_alias->args[0] = receiver;
    receiver_alias->aux_int = XI_COPY_KIND_IDENTITY;
    method->args[0] = receiver_alias;
    if (with_argument)
        method->args[1] = argument;
    if (reference) {
        XiCallPlan *call_plan =
            (XiCallPlan *) xi_func_arena_alloc(caller, (uint32_t) sizeof(*call_plan));
        XiCallArgPlan *argument_plan =
            (XiCallArgPlan *) xi_func_arena_alloc(caller, (uint32_t) sizeof(*argument_plan));
        REQUIRE(call_plan && argument_plan);
        memset(call_plan, 0, sizeof(*call_plan));
        memset(argument_plan, 0, sizeof(*argument_plan));
        argument_plan->param_mode = XR_PARAM_REF;
        argument_plan->access = XR_CALL_ARG_REF;
        argument_plan->origin = XI_PLACE_ORIGIN_STACK_LOCAL;
        argument_plan->lifetime = XI_PLACE_LIFETIME_CALL_BOUND;
        argument_plan->escape = XI_PLACE_ESCAPE_NONE;
        argument_plan->addressable = true;
        argument_plan->origin_var_id = 1;
        argument_plan->place = argument_storage;
        call_plan->args = argument_plan;
        call_plan->nargs = 1;
        call_plan->verified = true;
        method->call_plan = call_plan;
    }
    method->aux = (void *) (exact_string ? "trim" : "writeBytes");
    method->aux_int = 0;
    if (exact_string) {
        XiReturnOwnership owned_return = {
            .kind = XI_RETURN_OWNERSHIP_OWNED,
            .param_index = -1,
            .complete = true,
        };
        write_bytes->arc_return_ownership = owned_return;
        method->call_return_ownership = owned_return;
        caller->arc_return_ownership = owned_return;
    }
    if (reference) {
        XiValue *writeback =
            xi_value_new(caller, caller_entry, XI_PLACE_LOAD, &stub_raw_pointer, 1);
        XiValue *writeback_store = xi_value_new(caller, caller_entry, XI_SET_SHARED, &stub_unit, 1);
        REQUIRE(writeback && writeback_store);
        writeback->args[0] = argument;
        writeback_store->args[0] = writeback;
        writeback_store->aux_int = 1;
    }
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
    REQUIRE(
        xi_module_set_identity(caller_module, "memory-module-v1:id=23:source-export-caller-v1"));
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

/* The real imported-class shape is a named XI_IMPORT_REF stored in the caller
 * root and read as an erased class object by a child function. The dependency
 * freezes the public class export and its constructor declaration; no caller
 * local class slot or function-export fallback exists in this fixture. */
static XrSemanticPlan *
build_imported_source_class_constructor_semantic(XrSemanticPlan **dependency_out, bool exact_member,
                                                 char *error, size_t error_size) {
    XiFunc *dependency_root = xi_func_new("diagnostic_init", &stub_unit);
    XiFunc *constructor = xi_func_new("ImportedDiagnostic", &stub_unit);
    REQUIRE(dependency_root && constructor);
    dependency_root->is_module_initializer = true;
    XiBlock *dependency_entry = xi_block_new(dependency_root);
    XiBlock *constructor_entry = xi_block_new(constructor);
    REQUIRE(dependency_entry && constructor_entry);
    dependency_root->children = (XiFunc **) xr_calloc(1, sizeof(*dependency_root->children));
    REQUIRE(dependency_root->children);
    dependency_root->children[0] = constructor;
    dependency_root->nchildren = dependency_root->children_cap = 1;
    constructor->parent_func = dependency_root;
    constructor->nparams = 4;
    constructor->min_params = 3;
    constructor->params = (XiValue **) xr_calloc(4, sizeof(*constructor->params));
    REQUIRE(constructor->params);
    constructor->params[0] =
        xi_param(constructor, constructor_entry, 0, &stub_imported_constructor_instance);
    constructor->params[1] = xi_param(constructor, constructor_entry, 1, &stub_int);
    constructor->params[2] = xi_param(constructor, constructor_entry, 2, &stub_exact_string);
    constructor->params[3] = xi_param(constructor, constructor_entry, 3, &stub_exact_string);
    REQUIRE(constructor->params[0] && constructor->params[1] && constructor->params[2] &&
            constructor->params[3]);
    constructor->arc_borrow_sig = (XiBorrowSig *) xi_func_arena_alloc(
        constructor, (uint32_t) sizeof(*constructor->arc_borrow_sig));
    REQUIRE(constructor->arc_borrow_sig);
    constructor->arc_borrow_sig->nparams = 4;
    constructor->arc_borrow_sig->param_own[0] = XI_OWN_BORROWED;
    constructor->arc_borrow_sig->param_own[1] = XI_OWN_NONE;
    constructor->arc_borrow_sig->param_own[2] = XI_OWN_OWNED;
    constructor->arc_borrow_sig->param_own[3] = XI_OWN_BORROWED;
    constructor->arc_borrow_sig->valid = true;
    xi_block_set_return(constructor_entry, NULL);

    XiClassMethod constructor_method = {
        .name = "ImportedDiagnostic",
        .is_constructor = true,
    };
    uint16_t constructor_child = 0;
    XiClassData declaration = {
        .class_info = &stub_imported_constructor_class_info,
        .xg_class_id = 802,
        .class_name = "ImportedDiagnostic",
        .methods = &constructor_method,
        .nmethod = 1,
        .child_idx = &constructor_child,
        .ninst = 1,
        .explicit_final = true,
        .needs_runtime_type = true,
    };
    XiValue *class_object =
        xi_value_new(dependency_root, dependency_entry, XI_CLASS_CREATE, &stub_unknown, 0);
    XiValue *class_store =
        xi_value_new(dependency_root, dependency_entry, XI_SET_SHARED, &stub_unit, 1);
    REQUIRE(class_object && class_store);
    class_object->aux = &declaration;
    class_store->args[0] = class_object;
    class_store->aux_int = 0;
    dependency_root->nshared = 1;
    xi_block_set_return(dependency_entry, NULL);
    dependency_root->stage = constructor->stage = XI_STAGE_OPTIMIZED;

    XiModule *dependency_module = xi_module_new("pkg/diagnostic.xr", "diagnostic", dependency_root);
    REQUIRE(dependency_module);
    REQUIRE(xi_module_set_identity(dependency_module,
                                   "memory-module-v1:id=34:imported-constructor-dependency-v1"));
    dependency_root->module = dependency_module;
    dependency_module->classes = (XiClassData **) xr_calloc(1, sizeof(*dependency_module->classes));
    dependency_module->slot_classes =
        (XiClassData **) xr_calloc(1, sizeof(*dependency_module->slot_classes));
    dependency_module->exports =
        (XiModuleExport *) xr_calloc(1, sizeof(*dependency_module->exports));
    REQUIRE(dependency_module->classes && dependency_module->slot_classes &&
            dependency_module->exports);
    dependency_module->classes[0] = &declaration;
    dependency_module->slot_classes[0] = &declaration;
    dependency_module->nclasses = 1;
    dependency_module->nslots = 1;
    dependency_module->nexports = 1;
    dependency_module->exports[0].name = "ImportedDiagnostic";
    dependency_module->exports[0].shared_slot = 0;
    dependency_module->exports[0].class_data = &declaration;
    bool dependency_built = xr_semantic_plan_build_and_attach(dependency_root, error, error_size);
    if (!dependency_built)
        fprintf(stderr, "imported constructor dependency fixture failed: %s\n", error);
    REQUIRE(dependency_built);
    XrSemanticPlan *dependency = xr_semantic_plan_retain(dependency_root->semantic_plan);
    REQUIRE(dependency && dependency->source_export_count == 1 &&
            dependency->source_exports[0].kind == XR_SEM_SOURCE_EXPORT_SOURCE_CLASS &&
            dependency->source_exports[0].function == XR_SEMANTIC_INDEX_NONE &&
            dependency->source_exports[0].source_class == 0 &&
            xr_stable_id_equal(dependency->source_exports[0].exported_entity,
                               dependency->source_classes[0].id));
    uint8_t *dependency_bytes = NULL;
    size_t dependency_size = 0;
    XrSemanticPlan *decoded_dependency = NULL;
    REQUIRE(
        xr_xsm_encode(dependency, &dependency_bytes, &dependency_size, error, error_size) &&
        xr_xsm_decode(dependency_bytes, dependency_size, &decoded_dependency, error, error_size) &&
        decoded_dependency->source_export_count == 1 &&
        decoded_dependency->source_exports[0].kind == XR_SEM_SOURCE_EXPORT_SOURCE_CLASS &&
        decoded_dependency->source_exports[0].source_class == 0 &&
        decoded_dependency->source_exports[0].function == XR_SEMANTIC_INDEX_NONE &&
        xr_stable_id_equal(decoded_dependency->source_exports[0].exported_entity,
                           decoded_dependency->source_classes[0].id));
    xr_free(dependency_bytes);
    xr_semantic_plan_free(decoded_dependency);

    XiFunc *caller_root = xi_func_new("diagnostic_user_init", &stub_unit);
    XiFunc *caller = xi_func_new("make_diagnostic", &stub_imported_constructor_instance);
    REQUIRE(caller_root && caller);
    caller_root->is_module_initializer = true;
    XiBlock *caller_root_entry = xi_block_new(caller_root);
    XiBlock *caller_entry = xi_block_new(caller);
    REQUIRE(caller_root_entry && caller_entry);
    caller_root->children = (XiFunc **) xr_calloc(1, sizeof(*caller_root->children));
    REQUIRE(caller_root->children);
    caller_root->children[0] = caller;
    caller_root->nchildren = caller_root->children_cap = 1;
    caller->parent_func = caller_root;
    XiImportRef import_ref = {
        .module_path = "pkg/diagnostic.xr",
        .member_name = exact_member ? "ImportedDiagnostic" : "WrongDiagnostic",
        .resolved_mod_index = 0,
        .resolved_shared_slot = 0,
        .resolved_export_slot = 0,
        .resolved_module = dependency_module,
        .resolution_attempted = true,
    };
    XiValue *import = xi_value_new(caller_root, caller_root_entry, XI_IMPORT_REF, &stub_unknown, 0);
    XiValue *import_store =
        xi_value_new(caller_root, caller_root_entry, XI_SET_SHARED, &stub_unit, 1);
    REQUIRE(import && import_store);
    import->aux = &import_ref;
    import_store->args[0] = import;
    import_store->aux_int = 0;
    caller_root->nshared = 1;
    xi_block_set_return(caller_root_entry, NULL);

    XiValue *callee = xi_value_new(caller, caller_entry, XI_GET_SHARED, &stub_unknown, 0);
    XiValue *code = xi_const_int(caller, caller_entry, 41, &stub_int);
    XiValue *message =
        xi_const_str(caller, caller_entry, "imported-constructor", &stub_exact_string);
    XiValue *call =
        xi_value_new(caller, caller_entry, XI_CALL, &stub_imported_constructor_instance, 3);
    REQUIRE(callee && code && message && call);
    callee->aux_int = 0;
    call->args[0] = callee;
    call->args[1] = code;
    call->args[2] = message;
    XiCallPlan *call_plan =
        (XiCallPlan *) xi_func_arena_alloc(caller, (uint32_t) sizeof(*call_plan));
    XiCallArgPlan *argument_plans =
        (XiCallArgPlan *) xi_func_arena_alloc(caller, 2u * (uint32_t) sizeof(*argument_plans));
    REQUIRE(call_plan && argument_plans);
    memset(call_plan, 0, sizeof(*call_plan));
    memset(argument_plans, 0, 2u * sizeof(*argument_plans));
    call_plan->args = argument_plans;
    call_plan->nargs = 2;
    call_plan->verified = true;
    call->call_plan = call_plan;
    call->lowering_flags |= XI_LOWERING_FLAG_CONSTRUCTOR_CALL;
    xi_block_set_return(caller_entry, call);
    caller_root->stage = caller->stage = XI_STAGE_OPTIMIZED;
    XiModule *caller_module =
        xi_module_new("pkg/diagnostic_user.xr", "diagnostic_user", caller_root);
    REQUIRE(caller_module);
    REQUIRE(xi_module_set_identity(caller_module,
                                   "memory-module-v1:id=30:imported-constructor-caller-v1"));
    caller_root->module = caller_module;
    caller_module->nslots = 1;
    XiModule *dependency_modules[] = {dependency_module};
    XrSemanticPlan *semantic = NULL;
    bool built = xr_semantic_plan_build_and_attach_module_set(caller_root, dependency_modules, 1,
                                                              error, error_size);
    if (built)
        semantic = xr_semantic_plan_retain(caller_root->semantic_plan);

    caller_root->module = NULL;
    xi_func_free(caller_root);
    caller_module->init = NULL;
    xi_module_free(caller_module);
    dependency_root->module = NULL;
    xi_func_free(dependency_root);
    dependency_module->init = NULL;
    xi_module_free(dependency_module);
    *dependency_out = dependency;
    return semantic;
}

static void test_imported_source_class_constructor_authority(void) {
    char error[512] = {0};
    XrSemanticPlan *negative_dependency = NULL;
    XrSemanticPlan *negative = build_imported_source_class_constructor_semantic(
        &negative_dependency, false, error, sizeof(error));
    REQUIRE(negative == NULL && negative_dependency != NULL &&
            strncmp(error, "XR_SEM_0019", strlen("XR_SEM_0019")) == 0);
    xr_semantic_plan_free(negative_dependency);

    memset(error, 0, sizeof(error));
    XrSemanticPlan *dependency = NULL;
    XrSemanticPlan *semantic =
        build_imported_source_class_constructor_semantic(&dependency, true, error, sizeof(error));
    if (!semantic)
        fprintf(stderr, "imported source class semantic fixture failed: %s\n", error);
    REQUIRE(semantic && dependency && semantic->dependency_count == 1 &&
            semantic->call_target_count == 1);
    const XrSemanticCallTargetRecord *semantic_target = &semantic->call_targets[0];
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(semantic, semantic_target->operation);
    const XrSemanticSourceExportRecord *source_export = &dependency->source_exports[0];
    uint32_t constructor = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(operation && operation->opcode == XI_CALL &&
            semantic_target->kind == XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR &&
            semantic_target->dependency == 0 && semantic_target->source_export == 0 &&
            semantic_target->function == XR_SEMANTIC_INDEX_NONE &&
            semantic_target->callable_type == operation->result_type &&
            xr_stable_id_equal(semantic_target->export_identity, source_export->id) &&
            strstr(semantic_target->canonical_key, "call-target-v10:schema=46:") != NULL &&
            xr_semantic_imported_class_construction_authority_source_class(
                semantic, dependency, &semantic->dependencies[0], source_export, operation,
                &constructor) == 0 &&
            constructor != XR_SEMANTIC_INDEX_NONE &&
            xr_stable_id_equal(semantic_target->callee_function,
                               dependency->functions[constructor].id));

    const XrSemanticPlan *dependencies[] = {dependency};
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    bool built = xr_target_plan_build_module_set(semantic, dependencies, 1, profile, &plan, error,
                                                 sizeof(error));
    if (!built)
        fprintf(stderr, "imported source class Target fixture failed: %s\n", error);
    REQUIRE(built && plan && xr_target_plan_verify(plan, error, sizeof(error)) &&
            plan->semantic_dependency_count == 1 && plan->calls_count == 1 &&
            plan->call_arguments_count == 2 &&
            (plan->completed_family_mask & XR_TARGET_FAMILY_SOURCE_CLASS_INSTANCE_STORAGE) != 0);
    XrTargetCallRecord *call = &plan->calls[0];
    REQUIRE(call->semantic_call_target == 0 &&
            call->semantic_operation == semantic_target->operation &&
            call->source_dependency == 0 && call->source_export == 0 &&
            call->callee_function == XR_SEMANTIC_INDEX_NONE && call->argument_count == 2 &&
            call->result_ownership == XR_TARGET_CALL_RETURN_OWNED && call->flags == 0 &&
            call->calling_convention == XR_TARGET_CALL_CONVENTION_SOURCE_CLASS_CONSTRUCTOR &&
            call->target_kind == XR_TARGET_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR &&
            xr_stable_id_equal(call->source_export_identity, source_export->id) &&
            xr_stable_id_equal(call->source_callee_identity, semantic_target->callee_function));
    const XrTargetValueRepRecord *result = xr_target_plan_value_rep(plan, operation->result_value);
    REQUIRE(result && result->slot < plan->slots_count &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[result->register_rep].ownership == XR_TARGET_OWNERSHIP_OWNED &&
            plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
            plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED);
    REQUIRE(plan->machine_reps[plan->call_arguments[0].register_rep].kind == XR_MACHINE_REP_I64 &&
            plan->machine_reps[plan->call_arguments[1].register_rep].kind ==
                XR_MACHINE_REP_DYN_VALUE &&
            plan->call_arguments[0].callee_slot == XR_SEMANTIC_INDEX_NONE &&
            plan->call_arguments[1].callee_slot == XR_SEMANTIC_INDEX_NONE);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic refinement_diag = {0};
    XrAotRefinementPlan *refinement = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        plan, xr_target_plan_semantic_plan(plan), &policy, &refinement, &refinement_diag));
    XrAotRefinementPlanView refinement_view = xr_aot_refinement_plan_view(refinement);
    REQUIRE(refinement_view.frozen && refinement_view.verified &&
            xr_aot_refinement_verify(&refinement_view, plan, xr_target_plan_semantic_plan(plan),
                                     &refinement_diag));
    xr_aot_refinement_plan_free(refinement);

    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(plan, xr_target_plan_semantic_plan(plan),
                                     xr_target_profile_fingerprint(profile), &emission, error,
                                     sizeof(error)) &&
            xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                      xr_target_profile_fingerprint(profile), error,
                                      sizeof(error)));
    XrCValueEmissionView result_view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, operation->result_value, &result_view, error,
                                          sizeof(error)));
    REQUIRE(result_view.rep == XR_C_VALUE_REP_TAGGED &&
            xr_c_emission_plan_call_argument_count(emission) == 0);
    xr_c_emission_plan_free(emission);

    XrStableId saved_entity = dependency->source_exports[0].exported_entity;
    dependency->source_exports[0].exported_entity.bytes[0] ^= 1u;
    REQUIRE(xr_semantic_imported_class_construction_authority_source_class(
                semantic, dependency, &semantic->dependencies[0], &dependency->source_exports[0],
                operation, NULL) == XR_SEMANTIC_INDEX_NONE);
    dependency->source_exports[0].exported_entity = saved_entity;
    uint8_t saved_kind = dependency->source_exports[0].kind;
    dependency->source_exports[0].kind = XR_SEM_SOURCE_EXPORT_FUNCTION;
    REQUIRE(xr_semantic_imported_class_construction_authority_source_class(
                semantic, dependency, &semantic->dependencies[0], &dependency->source_exports[0],
                operation, NULL) == XR_SEMANTIC_INDEX_NONE);
    dependency->source_exports[0].kind = saved_kind;

    XrTargetCallRecord saved_call = *call;
    call->source_export_identity.bytes[0] ^= 1u;
    xr_target_call_compute_fingerprint(plan, 0, &call->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1003");
    *call = saved_call;
    saved_call = *call;
    call->source_dependency = XR_SEMANTIC_INDEX_NONE;
    xr_target_call_compute_fingerprint(plan, 0, &call->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1003");
    *call = saved_call;
    XrTargetCallArgumentRecord saved_argument = plan->call_arguments[1];
    plan->call_arguments[1].callee_parameter = XR_SEMANTIC_INDEX_NONE;
    xr_target_call_compute_fingerprint(plan, 0, &call->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1003");
    plan->call_arguments[1] = saved_argument;
    xr_target_call_compute_fingerprint(plan, 0, &call->fingerprint);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    xr_semantic_plan_free(dependency);
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
    /* Re-anchored because a TargetPlan call fingerprint is built on top of the
     * SemanticPlan fingerprint: xr_target_call_compute_fingerprint hashes
     * plan->semantic_fingerprint first, and xr_semantic_plan.c in turn hashes
     * plan->stdlib_registry_fingerprint, which
     * xr_stdlib_metadata_registry_fingerprint derives from every .def entry.
     * Publishing http2, compress, mem, regex and io from .xr bodies renames their
     * entries, so this digest moves even though the fixture imports nothing.
     * Old: 24c5af5d48d07261f6b33e4cb8edbc4fa91bd3492e18213bae80b8e1b1ea579f.
     * The declaration-owned receiver contract subsequently changed this
     * fixture's `close` receiver from a physical READ carrier to its logical
     * REF permission in SemanticPlan. That contract is part of the call
     * fingerprint by design.
     * Old receiver-contract digest:
     * b46b26a760a8d76b5bb434fd8ed148a2202c348761be21518fe98e479a8e2d2f. */
    REQUIRE(strcmp(call_hex, "638ac815ed74ef119cbafa71abf5cb88e276b4fed3ed0a0ba4e46e88bc0dc768") ==
            0);
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
    bool built = build_target_unit_fixture_semantic(root, &plan, error, sizeof(error));
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
    bool built = build_target_unit_fixture_semantic(root, &plan, error, sizeof(error));
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
    XiModule fixture_module = {
        .identity = "memory-module-v1:id=27:target-plan-unit-fixture-v1",
        .path = "target-plan-unit-fixture.xr",
        .name = "target_plan_unit_fixture",
        .init = root,
    };
    REQUIRE(root->module == NULL);
    root->module = &fixture_module;
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
     * rewritten below before SemanticPlan is frozen: schema 30 must then
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
    root->module = NULL;
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
    call->call_return_ownership = (XiReturnOwnership) {
        .kind = XI_RETURN_OWNERSHIP_OWNED,
        .param_index = -1,
        .complete = true,
    };
    child->arc_return_ownership = (XiReturnOwnership) {
        .kind = XI_RETURN_OWNERSHIP_OWNED,
        .param_index = -1,
        .complete = true,
    };
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
    bool built = build_target_unit_fixture_semantic(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "aggregate-call semantic fixture failed: %s\n", error);
    REQUIRE(built);
    REQUIRE(plan != NULL && xr_semantic_plan_call_target_count(plan) == 1);
    xi_func_free(root);
    return plan;
}

static XrSemanticPlan *build_direct_local_managed_aggregate_argument(void) {
    const char *field_names[2] = {"label", "count"};
    XrType *field_types[2] = {&stub_exact_string, &stub_int};
    XrType aggregate = {
        .kind = XR_KIND_STRUCT_OBJECT,
        .id = 146,
        .frozen = true,
        .scalar_rep = XR_SCALAR_REP_NONE,
        .object =
            {
                .field_names = field_names,
                .field_types = field_types,
                .field_count = 2,
            },
    };
    XiFunc *root = xi_func_new("target_managed_aggregate_root", &stub_int);
    XiFunc *child = xi_func_new("target_managed_aggregate_child", &stub_int);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);
    root->nparams = root->min_params = 1;
    child->nparams = child->min_params = 1;
    root->params = (XiValue **) xr_calloc(1, sizeof(*root->params));
    child->params = (XiValue **) xr_calloc(1, sizeof(*child->params));
    REQUIRE(root->params != NULL && child->params != NULL);
    root->params[0] = xi_param(root, root_entry, 0, &aggregate);
    child->params[0] = xi_param(child, child_entry, 0, &aggregate);
    REQUIRE(root->params[0] != NULL && child->params[0] != NULL);
    root->params[0]->transfer_mode = XR_TRANSFER_SHARE;
    child->params[0]->transfer_mode = XR_TRANSFER_SHARE;
    set_single_parameter_ownership(root, XI_OWN_BORROWED);
    set_single_parameter_ownership(child, XI_OWN_BORROWED);
    XiValue *child_result = xi_const_int(child, child_entry, 7, &stub_int);
    REQUIRE(child_result != NULL);
    xi_block_set_return(child_entry, child_result);
    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;
    XiValue *closure = xi_value_new(root, root_entry, XI_STACK_ALLOC, &stub_function, 0);
    XiValue *call = xi_value_new(root, root_entry, XI_CALL, &stub_int, 2);
    REQUIRE(closure != NULL && call != NULL);
    closure->aux_int = XI_CLOSURE_NEW;
    closure->aux = child;
    call->args[0] = closure;
    call->args[1] = root->params[0];
    xi_block_set_return(root_entry, call);
    root->stage = child->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = build_target_unit_fixture_semantic(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "managed aggregate semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL && xr_semantic_plan_call_target_count(plan) == 1);
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
    XrSemanticPlan *semantic =
        build_source_export_semantic(&dependency, SOURCE_EXPORT_ARGUMENT_NONE);
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

    XrSemanticTypeRecord *namespace_type =
        &semantic->types[semantic->operations[import_operation].result_type];
    REQUIRE(namespace_type->kind == XR_KIND_UNKNOWN &&
            namespace_type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT));
    uint8_t saved_namespace_flags = namespace_type->flags;
    namespace_type->flags |= XR_SEM_TYPE_CONST;
    resign_mutated_semantic_target(semantic, plan);
    REQUIRE(xr_semantic_plan_verify_module_set(semantic, dependencies, 1, error, sizeof(error)));
    expect_verify_failure_raw(plan, "XR_TARGET_1001");
    namespace_type->flags = saved_namespace_flags;
    resign_mutated_semantic_target(semantic, plan);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

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
    XrSemanticPlan *semantic =
        build_source_export_semantic(&dependency, SOURCE_EXPORT_ARGUMENT_VALUE);
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
            argument->callee_parameter == 0 && argument->callee_slot == XR_SEMANTIC_INDEX_NONE &&
            argument->mode == XR_TARGET_CALL_VALUE &&
            argument->ownership == XR_TARGET_CALL_CONSUME && argument->flags == 0 &&
            argument->callee_register_rep == argument->register_rep &&
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

static void test_source_export_string_result_authority(void) {
    XrSemanticPlan *dependency = NULL;
    XrSemanticPlan *semantic =
        build_source_export_semantic(&dependency, SOURCE_EXPORT_ARGUMENT_STRING);
    XrTargetProfile *profile = build_profile(0);
    const XrSemanticPlan *dependencies[] = {dependency};
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build_module_set(semantic, dependencies, 1, profile, &plan, error,
                                                 sizeof(error));
    if (!built)
        fprintf(stderr, "source-export String result Target fixture failed: %s\n", error);
    REQUIRE(built && plan && xr_target_plan_verify(plan, error, sizeof(error)) &&
            plan->calls_count == 1 && plan->call_arguments_count == 1);
    XrTargetCallRecord *call = &plan->calls[0];
    XrTargetCallArgumentRecord *argument = &plan->call_arguments[0];
    const XrTargetValueRepRecord *result = xr_target_plan_value_rep(plan, call->result_value);
    REQUIRE(call->target_kind == XR_TARGET_CALL_TARGET_SOURCE_EXPORT &&
            call->calling_convention == XR_TARGET_CALL_CONVENTION_SOURCE_EXPORT &&
            call->result_mode == XR_TARGET_CALL_VALUE &&
            call->result_ownership == XR_TARGET_CALL_RETURN_OWNED && result &&
            result->slot < plan->slots_count &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[result->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED &&
            argument->mode == XR_TARGET_CALL_VALUE && argument->ownership == XR_TARGET_CALL_READ &&
            plan->machine_reps[argument->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            argument->callee_slot == XR_SEMANTIC_INDEX_NONE);

    XrTargetCallRecord saved_call = *call;
    call->result_ownership = XR_TARGET_CALL_NONE;
    xr_target_call_compute_fingerprint(plan, 0, &call->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1003");
    *call = saved_call;
    xr_target_call_compute_fingerprint(plan, 0, &call->fingerprint);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    uint8_t saved_complete =
        semantic->operations[semantic->call_targets[0].operation].return_complete;
    semantic->operations[semantic->call_targets[0].operation].return_complete = 0;
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    XrTargetPlan *failed = NULL;
    REQUIRE(!xr_target_plan_build_module_set(semantic, dependencies, 1, profile, &failed, error,
                                             sizeof(error)) &&
            failed == NULL && strncmp(error, "XR_TARGET_1000", strlen("XR_TARGET_1000")) == 0);
    semantic->operations[semantic->call_targets[0].operation].return_complete = saved_complete;
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    xr_semantic_plan_free(dependency);
}

static void test_source_export_ref_argument_is_not_array_projection(void) {
    XrSemanticPlan *dependency = NULL;
    XrSemanticPlan *semantic =
        build_source_export_semantic(&dependency, SOURCE_EXPORT_ARGUMENT_RAW_POINTER_REFERENCE);
    XrTargetProfile *profile = build_profile(0);
    const XrSemanticPlan *dependencies[] = {dependency};
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build_module_set(semantic, dependencies, 1, profile, &plan, error,
                                                 sizeof(error));
    if (!built)
        fprintf(stderr, "source-export raw-pointer ref Target fixture failed: %s\n", error);
    REQUIRE(built && plan && xr_target_plan_verify(plan, error, sizeof(error)));
    REQUIRE(plan->calls_count == 1 && plan->call_arguments_count == 1);
    const XrTargetCallRecord *call = &plan->calls[0];
    const XrTargetCallArgumentRecord *argument = &plan->call_arguments[0];
    REQUIRE(call->target_kind == XR_TARGET_CALL_TARGET_SOURCE_EXPORT &&
            call->calling_convention == XR_TARGET_CALL_CONVENTION_SOURCE_EXPORT &&
            argument->call == call->id && argument->mode == XR_TARGET_CALL_REFERENCE &&
            argument->ownership == XR_TARGET_CALL_WRITEBACK &&
            argument->flags == XR_TARGET_CALL_ARGUMENT_ADDRESSABLE &&
            argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
            argument->callee_slot == XR_SEMANTIC_INDEX_NONE &&
            plan->machine_reps[argument->register_rep].kind == XR_MACHINE_REP_RAW_PTR &&
            argument->callee_register_rep == argument->register_rep &&
            argument->callee_memory_rep == argument->memory_rep);

    XrCEmissionPlan *emission = NULL;
    error[0] = '\0';
    built = xr_c_emission_plan_build(plan, xr_target_plan_semantic_plan(plan),
                                     xr_target_profile_fingerprint(profile), &emission, error,
                                     sizeof(error));
    if (!built)
        fprintf(stderr, "source-export raw-pointer ref C emission failed: %s\n", error);
    REQUIRE(built && emission && xr_c_emission_plan_is_verified(emission));
    REQUIRE(xr_c_emission_plan_call_argument_count(emission) == 0);
    XrCValueEmissionView value = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, argument->semantic_value, &value, error,
                                          sizeof(error)));
    REQUIRE(value.rep == XR_C_VALUE_REP_RAW_PTR && value.c_type &&
            strcmp(value.c_type, "const void * *") == 0);

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    xr_semantic_plan_free(dependency);
}

static void test_exact_i64_dynamic_entry_authority(void) {
    XrSemanticPlan *dependency = NULL;
    XrSemanticPlan *semantic =
        build_source_export_semantic(&dependency, SOURCE_EXPORT_ARGUMENT_EXACT_I64);
    XrTargetProfile *profile = build_profile(0);
    const XrSemanticPlan *dependencies[] = {dependency};
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build_module_set(semantic, dependencies, 1, profile, &plan, error,
                                                 sizeof(error));
    if (!built)
        fprintf(stderr, "exact i64 dynamic-entry fixture failed: %s\n", error);
    REQUIRE(built && plan && xr_target_plan_verify(plan, error, sizeof(error)));
    REQUIRE(plan->entry_expectations_count == 1 && plan->calls_count == 1 &&
            plan->call_arguments_count == 1 && plan->entry_expectations[0].id == 0 &&
            plan->entry_expectations[0].call == 0 &&
            plan->entry_expectations[0].abi_schema_version == 1 &&
            plan->entry_expectations[0].parameter_count == 1 &&
            plan->entry_expectations[0].value_kind == XR_TARGET_ENTRY_VALUE_EXACT_I64 &&
            plan->entry_expectations[0].adapter_kind == XR_TARGET_ENTRY_ADAPTER_IDENTITY);
    uint32_t entry_rows = 0;
    uint32_t entry_function = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->instructions_count; i++) {
        if (plan->instructions[i].opcode != XR_TARGET_INSTRUCTION_CALL_ENTRY_I64)
            continue;
        entry_rows++;
        entry_function = plan->instructions[i].function;
        REQUIRE(plan->instructions[i].immediate_bits == 0);
    }
    REQUIRE(entry_rows == 1 && entry_function != XR_SEMANTIC_INDEX_NONE &&
            xr_target_plan_function_execution_family_mask(plan, entry_function) ==
                XR_TARGET_EXECUTION_SCALAR_I64_DYNAMIC);
    REQUIRE((plan->completed_family_mask & XR_TARGET_FAMILY_DYNAMIC_ENTRY_EXPECTATION) != 0);

    XrFingerprint saved = plan->entry_expectations[0].entry_abi_fingerprint;
    plan->entry_expectations[0].entry_abi_fingerprint.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1005");
    plan->entry_expectations[0].entry_abi_fingerprint = saved;
    saved = plan->entry_expectations[0].adapter_fingerprint;
    plan->entry_expectations[0].adapter_fingerprint.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1005");
    plan->entry_expectations[0].adapter_fingerprint = saved;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    xr_semantic_plan_free(dependency);
}

static void test_direct_local_call_adapter_family(void) {
    XrSemanticPlan *semantic = build_direct_local_scalar_calls(XI_CALL, &stub_int, &stub_function);
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
            root_instructions[1].opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_I64 &&
            root_instructions[1].immediate_bits == 0 &&
            root_instructions[2].opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_I64 &&
            root_instructions[2].immediate_bits == 1 &&
            root_instructions[3].opcode == XR_TARGET_INSTRUCTION_RETURN_I64);
    REQUIRE(child_instructions != NULL && child_instruction_count == 2u &&
            child_instructions[0].opcode == XR_TARGET_INSTRUCTION_PARAM_I64 &&
            child_instructions[1].opcode == XR_TARGET_INSTRUCTION_RETURN_I64);
    REQUIRE(xr_fingerprint_equal(first->fingerprint, second->fingerprint));
    char call_hex[XR_FINGERPRINT_BYTES * 2 + 1];
    xr_fingerprint_hex(first->calls[0].fingerprint, call_hex);
    /* Re-anchored because a TargetPlan call fingerprint is built on top of the
     * SemanticPlan fingerprint: xr_target_call_compute_fingerprint hashes
     * plan->semantic_fingerprint first, and xr_semantic_plan.c in turn hashes
     * plan->stdlib_registry_fingerprint, which
     * xr_stdlib_metadata_registry_fingerprint derives from every .def entry.
     * Publishing http2, compress, mem, regex and io from .xr bodies renames their
     * entries, so this digest moves even though the fixture imports nothing.
     * Old: 83a65b53740d1cac6db72bfea6bc4a2ed4dc25bbdba4e9d755bc61d1fa0417dc.
     * The canonical-program ownership registry freeze subsequently moved the
     * SemanticPlan fingerprint beneath this otherwise unchanged scalar call.
     * Old ownership-freeze digest:
     * 9e3078b2d60b479b8eab553d5e6a3421b107f303cbc7d0449064977f3b61bd6f.
     * BorrowOriginSet then made normalized borrowed-result origins part of
     * function type identity and changed the stdlib registry fingerprint. */
    if (strcmp(call_hex, "94bdd31a214392344cd369fc2fe4379f6541a3649d0c7f5e017b16c55e9b3874") != 0)
        fprintf(stderr, "direct-local call fingerprint drift: actual=%s\n", call_hex);
    REQUIRE(strcmp(call_hex, "94bdd31a214392344cd369fc2fe4379f6541a3649d0c7f5e017b16c55e9b3874") ==
            0);
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

static void test_direct_local_scalar_ref_argument_authority(void) {
    XrSemanticPlan *semantic = build_direct_local_scalar_ref_semantic();
    const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(semantic, 0);
    const XrSemanticOperationRecord *call_operation =
        target ? xr_semantic_plan_operation(semantic, target->operation) : NULL;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    const XrSemanticOperandRecord *call_operand =
        call_operation && call_operation->operand_begin + 1u < operand_count
            ? &operands[call_operation->operand_begin + 1u]
            : NULL;
    const XrSemanticOperationRecord *caller_place = NULL;
    const XrSemanticOperationRecord *same_local_id_decoy = NULL;
    for (uint32_t i = 0; call_operand && i < semantic->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &semantic->operations[i];
        if (operation->opcode != XI_LOCAL_ADDR)
            continue;
        const XrSemanticFunctionRecord *function =
            xr_semantic_plan_function(semantic, operation->function);
        if (!function || operation->result_value < function->value_begin)
            continue;
        if (operation->function == call_operation->function &&
            operation->result_value == call_operand->value) {
            REQUIRE(caller_place == NULL);
            caller_place = operation;
            continue;
        }
        if (!caller_place || operation->function == caller_place->function)
            continue;
        const XrSemanticFunctionRecord *caller_function =
            xr_semantic_plan_function(semantic, caller_place->function);
        if (caller_function && operation->result_value - function->value_begin ==
                                   caller_place->result_value - caller_function->value_begin)
            same_local_id_decoy = operation;
    }
    const XrSemanticFunctionRecord *caller_function =
        caller_place ? xr_semantic_plan_function(semantic, caller_place->function) : NULL;
    const XrSemanticFunctionRecord *decoy_function =
        same_local_id_decoy ? xr_semantic_plan_function(semantic, same_local_id_decoy->function)
                            : NULL;
    REQUIRE(target && call_operation && call_operand && caller_place && same_local_id_decoy &&
            caller_function && decoy_function &&
            caller_place->result_value != same_local_id_decoy->result_value &&
            caller_place->result_value - caller_function->value_begin ==
                same_local_id_decoy->result_value - decoy_function->value_begin &&
            xr_semantic_plan_verify(semantic, NULL, 0));

    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "direct-local scalar ref TargetPlan failed: %s\n", error);
    REQUIRE(built && plan && plan->calls_count == 1 && plan->call_arguments_count == 1 &&
            xr_target_plan_verify(plan, error, sizeof(error)));
    XrTargetCallRecord *call = &plan->calls[0];
    XrTargetCallArgumentRecord *argument = &plan->call_arguments[0];
    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(semantic, call->callee_function);
    const XrSemanticParameterRecord *parameter =
        callee ? xr_semantic_plan_parameter(semantic, callee->parameter_begin) : NULL;
    const XrSemanticOperandRecord *place_source =
        caller_place->operand_count == 1 && caller_place->operand_begin < operand_count
            ? &operands[caller_place->operand_begin]
            : NULL;
    const XrTargetValueRepRecord *caller_value =
        place_source ? xr_target_plan_value_rep(plan, place_source->value) : NULL;
    const XrTargetValueRepRecord *callee_value =
        parameter ? xr_target_plan_value_rep(plan, parameter->value) : NULL;
    const XrTargetValueRepRecord *caller_place_value =
        xr_target_plan_value_rep(plan, caller_place->result_value);
    const XrTargetValueRepRecord *decoy_place_value =
        xr_target_plan_value_rep(plan, same_local_id_decoy->result_value);
    const XrTargetSlotRecord *caller_place_slot =
        caller_place_value && caller_place_value->slot < plan->slots_count
            ? &plan->slots[caller_place_value->slot]
            : NULL;
    REQUIRE(parameter && place_source && caller_value && callee_value && caller_place_value &&
            decoy_place_value && caller_place_slot && caller_place_value != decoy_place_value &&
            caller_place_value->semantic_value == caller_place->result_value &&
            decoy_place_value->semantic_value == same_local_id_decoy->result_value &&
            plan->machine_reps[caller_value->register_rep].kind == XR_MACHINE_REP_I64 &&
            plan->machine_reps[callee_value->register_rep].kind == XR_MACHINE_REP_I64 &&
            plan->machine_reps[caller_place_value->register_rep].kind == XR_MACHINE_REP_RAW_PTR &&
            plan->machine_reps[caller_place_value->memory_rep].kind == XR_MACHINE_REP_RAW_PTR &&
            plan->machine_reps[caller_place_slot->register_rep].kind == XR_MACHINE_REP_RAW_PTR &&
            plan->machine_reps[caller_place_slot->memory_rep].kind == XR_MACHINE_REP_RAW_PTR &&
            plan->machine_reps[decoy_place_value->register_rep].kind == XR_MACHINE_REP_I64 &&
            argument->semantic_value == caller_place->result_value &&
            argument->caller_slot == caller_value->slot &&
            argument->callee_slot == callee_value->slot &&
            argument->mode == XR_TARGET_CALL_REFERENCE &&
            argument->ownership == XR_TARGET_CALL_BORROW &&
            argument->transfer_mode == XR_TRANSFER_SHARE &&
            argument->flags == XR_TARGET_CALL_ARGUMENT_ADDRESSABLE &&
            argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
            argument->register_rep == argument->callee_register_rep &&
            argument->memory_rep == argument->callee_memory_rep &&
            plan->machine_reps[argument->register_rep].kind == XR_MACHINE_REP_I64 &&
            plan->slots[argument->caller_slot].function == call->caller_function &&
            plan->slots[argument->caller_slot].semantic_value == place_source->value &&
            plan->slots[argument->callee_slot].function == call->callee_function &&
            plan->slots[argument->callee_slot].semantic_value == parameter->value);

    XrTargetCallArgumentRecord saved_argument = *argument;
    XrTargetCallRecord saved_call = *call;
    XrFingerprint saved_plan_fingerprint = plan->fingerprint;
    argument->semantic_value = same_local_id_decoy->result_value;
    xr_target_call_compute_fingerprint(plan, argument->call, &call->fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    REQUIRE(!xr_target_plan_verify(plan, error, sizeof(error)));
    *argument = saved_argument;
    *call = saved_call;
    plan->fingerprint = saved_plan_fingerprint;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    XrTargetValueRepRecord *mutable_place =
        &plan->value_reps[caller_place_value - plan->value_reps];
    XrTargetSlotRecord *mutable_place_slot = &plan->slots[mutable_place->slot];
    XrTargetValueRepRecord saved_place = *mutable_place;
    XrTargetSlotRecord saved_place_slot = *mutable_place_slot;
    mutable_place->register_rep = caller_value->register_rep;
    mutable_place->memory_rep = caller_value->memory_rep;
    mutable_place_slot->register_rep = caller_value->register_rep;
    mutable_place_slot->memory_rep = caller_value->memory_rep;
    expect_verify_failure(plan, "XR_TARGET_1001");
    *mutable_place = saved_place;
    *mutable_place_slot = saved_place_slot;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    bool emission_built =
        xr_c_emission_plan_build(plan, xr_target_plan_semantic_plan(plan), profile_fingerprint,
                                 &emission, error, sizeof(error));
    if (!emission_built)
        fprintf(stderr, "direct-local scalar ref C emission failed: %s\n", error);
    REQUIRE(emission_built && emission && xr_c_emission_plan_call_argument_count(emission) == 1 &&
            xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                      profile_fingerprint, error, sizeof(error)));
    XrCCallArgumentEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_call_argument_view(emission, call->result_value, 0, &view, error,
                                                  sizeof(error)) &&
            view.semantic_value == caller_place->result_value &&
            view.callee_parameter == callee->parameter_begin && view.ordinal == 0 &&
            view.caller_register_kind == XR_MACHINE_REP_I64 &&
            view.caller_memory_kind == XR_MACHINE_REP_I64 &&
            view.callee_register_kind == XR_MACHINE_REP_I64 &&
            view.callee_memory_kind == XR_MACHINE_REP_I64 &&
            view.mode == XR_TARGET_CALL_REFERENCE && view.ownership == XR_TARGET_CALL_BORROW &&
            view.transfer_mode == XR_TRANSFER_SHARE &&
            view.flags == XR_TARGET_CALL_ARGUMENT_ADDRESSABLE &&
            view.array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE && view.c_type &&
            strcmp(view.c_type, "int64_t *") == 0);
    const char *saved_c_type = emission->call_arguments[0].c_type;
    emission->call_arguments[0].c_type = "uint64_t *";
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    emission->call_arguments[0].c_type = saved_c_type;
    REQUIRE(xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                      profile_fingerprint, error, sizeof(error)));

    XrFingerprint plan_fingerprint = xr_target_plan_fingerprint(plan);
    int64_t vm_argument = 41;
    int64_t vm_result = 99;
    XrTypedDispatchI64Request request = {
        .verified_plan = plan,
        .required_plan_fingerprint = &plan_fingerprint,
        .arguments = &vm_argument,
        .result = &vm_result,
        .provider = XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        .function = call->caller_function,
        .argument_count = 1,
    };
    REQUIRE(xr_target_plan_function_execution_family_mask(plan, call->caller_function) == 0 &&
            xr_typed_dispatch_execute_i64(&request) == XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE &&
            vm_result == 0);

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_direct_local_class_argument_authority(void) {
    const XiOwnership callee_ownerships[] = {XI_OWN_OWNED, XI_OWN_BORROWED};
    for (uint32_t test_case = 0; test_case < 2; test_case++) {
        XiOwnership callee_ownership = callee_ownerships[test_case];
        XrSemanticPlan *semantic = build_direct_local_class_argument_semantic(callee_ownership);
        XrTargetProfile *profile = build_profile(0);
        XrTargetPlan *plan = NULL;
        char error[512] = {0};
        const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(semantic, 0);
        const XrSemanticOperationRecord *operation =
            target ? xr_semantic_plan_operation(semantic, target->operation) : NULL;
        const XrSemanticFunctionRecord *callee =
            target ? xr_semantic_plan_function(semantic, target->function) : NULL;
        const XrSemanticParameterRecord *parameter =
            callee ? xr_semantic_plan_parameter(semantic, callee->parameter_begin) : NULL;
        uint32_t operand_count = 0;
        XrSemanticOperandRecord *operands =
            (XrSemanticOperandRecord *) xr_semantic_plan_operands(semantic, &operand_count);
        XrSemanticOperandRecord *operand =
            operation && operation->operand_begin + 1u < operand_count
                ? &operands[operation->operand_begin + 1u]
                : NULL;
        REQUIRE(target && target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL && operation && callee &&
                parameter && operand && parameter->ownership == callee_ownership &&
                operand->ownership_action == (callee_ownership == XI_OWN_OWNED
                                                  ? XR_SEM_OPERAND_CONSUME
                                                  : XR_SEM_OPERAND_BORROW) &&
                xr_semantic_class_argument_source_class(semantic, callee->parameter_begin) !=
                    XR_SEMANTIC_INDEX_NONE &&
                xr_semantic_class_call_parameter_source_class(semantic, callee->parameter_begin,
                                                              operand) != XR_SEMANTIC_INDEX_NONE);
        uint8_t saved_action = operand->ownership_action;
        operand->ownership_action =
            saved_action == XR_SEM_OPERAND_CONSUME ? XR_SEM_OPERAND_BORROW : XR_SEM_OPERAND_CONSUME;
        REQUIRE(xr_semantic_class_call_parameter_source_class(semantic, callee->parameter_begin,
                                                              operand) == XR_SEMANTIC_INDEX_NONE);
        operand->ownership_action = saved_action;
        uint8_t saved_role = operand->role;
        operand->role = XR_SEM_OPERAND_RECEIVER;
        REQUIRE(xr_semantic_class_call_parameter_source_class(semantic, callee->parameter_begin,
                                                              operand) == XR_SEMANTIC_INDEX_NONE);
        operand->role = saved_role;

        bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
        if (!built)
            fprintf(stderr, "direct-local class argument TargetPlan failed: %s\n", error);
        REQUIRE(built && plan && plan->calls_count == 1 && plan->call_arguments_count == 1 &&
                (plan->completed_family_mask & XR_TARGET_FAMILY_SOURCE_CLASS_ARGUMENT_STORAGE) !=
                    0);
        XrTargetCallArgumentRecord *argument = &plan->call_arguments[0];
        REQUIRE(
            argument->mode == XR_TARGET_CALL_VALUE && argument->flags == 0 &&
            argument->ownership ==
                (callee_ownership == XI_OWN_OWNED ? XR_TARGET_CALL_CONSUME : XR_TARGET_CALL_READ) &&
            argument->caller_slot < plan->slots_count &&
            argument->callee_slot < plan->slots_count &&
            plan->slots[argument->caller_slot].ownership == XR_TARGET_OWNERSHIP_OWNED &&
            plan->slots[argument->callee_slot].ownership == (callee_ownership == XI_OWN_OWNED
                                                                 ? XR_TARGET_OWNERSHIP_OWNED
                                                                 : XR_TARGET_OWNERSHIP_BORROWED) &&
            plan->machine_reps[argument->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[argument->register_rep].ownership == XR_TARGET_OWNERSHIP_OWNED &&
            plan->machine_reps[argument->memory_rep].ownership == XR_TARGET_OWNERSHIP_OWNED &&
            plan->machine_reps[argument->callee_register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[argument->callee_register_rep].ownership ==
                (callee_ownership == XI_OWN_OWNED ? XR_TARGET_OWNERSHIP_OWNED
                                                  : XR_TARGET_OWNERSHIP_BORROWED) &&
            plan->machine_reps[argument->callee_memory_rep].ownership ==
                (callee_ownership == XI_OWN_OWNED ? XR_TARGET_OWNERSHIP_OWNED
                                                  : XR_TARGET_OWNERSHIP_BORROWED) &&
            xr_target_plan_verify(plan, error, sizeof(error)));

        uint8_t saved_ownership = argument->ownership;
        argument->ownership = saved_ownership == XR_TARGET_CALL_CONSUME ? XR_TARGET_CALL_READ
                                                                        : XR_TARGET_CALL_CONSUME;
        expect_verify_failure(plan, "XR_TARGET_1003");
        argument->ownership = saved_ownership;
        uint8_t saved_slot_ownership = plan->slots[argument->callee_slot].ownership;
        plan->slots[argument->callee_slot].ownership =
            saved_slot_ownership == XR_TARGET_OWNERSHIP_OWNED ? XR_TARGET_OWNERSHIP_BORROWED
                                                              : XR_TARGET_OWNERSHIP_OWNED;
        expect_verify_failure(plan, "XR_TARGET_1001");
        plan->slots[argument->callee_slot].ownership = saved_slot_ownership;
        REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

        xr_target_plan_free(plan);
        xr_target_profile_free(profile);
        xr_semantic_plan_free(semantic);
    }

    XrSemanticPlan *scalar = build_direct_local_scalar_calls(XI_CALL, &stub_int, &stub_function);
    const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(scalar, 0);
    const XrSemanticOperationRecord *operation =
        target ? xr_semantic_plan_operation(scalar, target->operation) : NULL;
    const XrSemanticFunctionRecord *callee =
        target ? xr_semantic_plan_function(scalar, target->function) : NULL;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(scalar, &operand_count);
    const XrSemanticOperandRecord *operand =
        operation && operation->operand_begin + 1u < operand_count
            ? &operands[operation->operand_begin + 1u]
            : NULL;
    REQUIRE(callee && operand &&
            xr_semantic_class_call_parameter_source_class(scalar, callee->parameter_begin,
                                                          operand) == XR_SEMANTIC_INDEX_NONE);
    xr_semantic_plan_free(scalar);
}

static void test_direct_local_source_class_array_ref_authority(void) {
    XrSemanticPlan *semantic = build_direct_local_tagged_ref_semantic(
        &stub_target_source_instance_array, true, false, false);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "source-class Array ref TargetPlan failed: %s\n", error);
    REQUIRE(built && plan != NULL && plan->calls_count == 1 && plan->call_arguments_count == 1 &&
            (plan->completed_family_mask &
             XR_TARGET_FAMILY_DIRECT_LOCAL_TAGGED_REF_ARGUMENT_STORAGE) != 0);

    XrTargetCallRecord saved_call = plan->calls[0];
    XrTargetCallArgumentRecord saved_argument = plan->call_arguments[0];
    XrFingerprint saved_fingerprint = plan->fingerprint;
    XrTargetCallArgumentRecord *argument = &plan->call_arguments[0];
    const XrSemanticParameterRecord *parameter =
        xr_semantic_plan_parameter(semantic, argument->callee_parameter);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    const XrSemanticTypeRecord *array_type =
        parameter ? xr_semantic_plan_type(semantic, parameter->type) : NULL;
    const XrSemanticTypeRecord *element_type =
        array_type && children && array_type->child_begin < child_count
            ? xr_semantic_plan_type(semantic, children[array_type->child_begin])
            : NULL;
    REQUIRE(parameter != NULL && xr_semantic_array_type_row_is_exact(array_type) &&
            xr_semantic_class_instance_type_source_class(semantic, element_type) !=
                XR_SEMANTIC_INDEX_NONE &&
            argument->mode == XR_TARGET_CALL_REFERENCE &&
            argument->ownership == XR_TARGET_CALL_BORROW &&
            argument->transfer_mode == XR_TRANSFER_SHARE &&
            argument->flags == XR_TARGET_CALL_ARGUMENT_ADDRESSABLE &&
            argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_TAGGED &&
            argument->caller_slot < plan->slots_count &&
            argument->callee_slot < plan->slots_count &&
            plan->machine_reps[argument->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[argument->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[argument->callee_register_rep].kind == XR_MACHINE_REP_RAW_PTR &&
            plan->machine_reps[argument->callee_memory_rep].kind == XR_MACHINE_REP_RAW_PTR &&
            xr_target_plan_verify(plan, error, sizeof(error)));
    uint32_t array_layout = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->layouts_count; i++)
        if (plan->layouts[i].semantic_type == parameter->type) {
            REQUIRE(array_layout == XR_SEMANTIC_INDEX_NONE);
            array_layout = i;
        }
    REQUIRE(array_layout != XR_SEMANTIC_INDEX_NONE &&
            plan->layouts[array_layout].array_element_storage == XR_TARGET_ARRAY_STORAGE_TAGGED);

    argument->array_element_storage = XR_TARGET_ARRAY_STORAGE_U8;
    xr_target_call_compute_fingerprint(plan, 0, &plan->calls[0].fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1003");
    plan->call_arguments[0] = saved_argument;
    plan->calls[0] = saved_call;
    plan->fingerprint = saved_fingerprint;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);

    semantic =
        build_direct_local_tagged_ref_semantic(&stub_target_string_array, false, false, false);
    plan = NULL;
    error[0] = '\0';
    REQUIRE(!xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan == NULL &&
            strncmp(error, "XR_TARGET_1003", 14) == 0);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_direct_local_forwarded_source_class_ref_authority(void) {
    XrSemanticPlan *semantic =
        build_direct_local_tagged_ref_semantic(&stub_target_source_instance, true, false, true);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "forwarded source-class ref TargetPlan failed: %s\n", error);
    REQUIRE(built && plan != NULL && plan->calls_count == 1 && plan->call_arguments_count == 1 &&
            (plan->completed_family_mask &
             XR_TARGET_FAMILY_DIRECT_LOCAL_TAGGED_REF_ARGUMENT_STORAGE) != 0);

    XrTargetCallArgumentRecord *argument = &plan->call_arguments[0];
    XrSemanticOperandRecord *operand = argument->semantic_operand < semantic->operand_count
                                           ? &semantic->operands[argument->semantic_operand]
                                           : NULL;
    REQUIRE(operand != NULL && operand->origin == XI_PLACE_ORIGIN_PARAM &&
            argument->mode == XR_TARGET_CALL_REFERENCE &&
            argument->ownership == XR_TARGET_CALL_BORROW &&
            argument->caller_slot < plan->slots_count &&
            argument->callee_slot < plan->slots_count &&
            plan->slots[argument->caller_slot].role == XR_TARGET_SLOT_PARAMETER &&
            plan->machine_reps[argument->register_rep].kind == XR_MACHINE_REP_RAW_PTR &&
            plan->machine_reps[argument->memory_rep].kind == XR_MACHINE_REP_RAW_PTR &&
            plan->machine_reps[argument->callee_register_rep].kind == XR_MACHINE_REP_RAW_PTR &&
            plan->machine_reps[argument->callee_memory_rep].kind == XR_MACHINE_REP_RAW_PTR &&
            xr_target_plan_verify(plan, error, sizeof(error)));

    uint8_t saved_origin = operand->origin;
    operand->origin = XI_PLACE_ORIGIN_STACK_LOCAL;
    resign_mutated_semantic_target(semantic, plan);
    expect_verify_failure(plan, "XR_TARGET_1003");
    operand->origin = saved_origin;
    resign_mutated_semantic_target(semantic, plan);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(semantic, 0);
    const XrSemanticOperationRecord *call =
        target ? xr_semantic_plan_operation(semantic, target->operation) : NULL;
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    bool emission_built =
        call != NULL && xr_c_emission_plan_build(plan, semantic, profile_fingerprint, &emission,
                                                 error, sizeof(error));
    if (!emission_built)
        fprintf(stderr, "forwarded source-class ref C emission failed: %s\n", error);
    REQUIRE(emission_built && emission != NULL &&
            xr_c_emission_plan_call_argument_count(emission) == 1 &&
            xr_c_emission_plan_verify(emission, plan, semantic, profile_fingerprint, error,
                                      sizeof(error)));
    XrCCallArgumentEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_call_argument_view(emission, call->result_value, 0, &view, error,
                                                  sizeof(error)) &&
            view.semantic_value == operand->value &&
            view.caller_register_kind == XR_MACHINE_REP_RAW_PTR &&
            view.caller_memory_kind == XR_MACHINE_REP_RAW_PTR &&
            view.callee_register_kind == XR_MACHINE_REP_RAW_PTR &&
            view.callee_memory_kind == XR_MACHINE_REP_RAW_PTR && view.c_type &&
            strcmp(view.c_type, "XrValue *") == 0);
    uint16_t saved_caller_kind = emission->call_arguments[0].caller_register_kind;
    emission->call_arguments[0].caller_register_kind = XR_MACHINE_REP_DYN_VALUE;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, semantic, profile_fingerprint, error,
                                       sizeof(error)));
    emission->call_arguments[0].caller_register_kind = saved_caller_kind;
    REQUIRE(xr_c_emission_plan_verify(emission, plan, semantic, profile_fingerprint, error,
                                      sizeof(error)));
    xr_c_emission_plan_free(emission);

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_direct_local_raw_pointer_call_authority(void) {
    XrSemanticPlan *semantic =
        build_direct_local_scalar_calls(XI_CALL, &stub_raw_pointer, &stub_raw_pointer_function);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    REQUIRE(plan != NULL && plan->calls_count == 2 && plan->call_arguments_count == 2);

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

static void test_source_instance_move_receiver_authority(void) {
    XrSemanticPlan *semantic = build_source_instance_method_semantic(false, false);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(semantic, 0);
    const XrSemanticOperationRecord *call =
        target ? xr_semantic_plan_operation(semantic, target->operation) : NULL;
    const XrSemanticFunctionRecord *callee =
        target ? xr_semantic_plan_function(semantic, target->function) : NULL;
    XrSemanticParameterRecord *parameter =
        callee ? &semantic->parameters[callee->parameter_begin] : NULL;
    XrSemanticOperandRecord *receiver = call && call->operand_begin < semantic->operand_count
                                            ? &semantic->operands[call->operand_begin]
                                            : NULL;
    const XrSemanticOperationRecord *move =
        receiver ? xr_semantic_unique_value_definition(semantic, receiver->value) : NULL;
    XrSemanticOperandRecord *move_source = move && move->operand_begin < semantic->operand_count
                                               ? &semantic->operands[move->operand_begin]
                                               : NULL;
    uint32_t moved_source = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(target && target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL && call &&
            callee && parameter && receiver && move && move_source &&
            move->opcode == XI_SOURCE_MOVE && parameter->mode == XR_PARAM_MOVE &&
            parameter->ownership == XI_OWN_BORROWED && receiver->role == XR_SEM_OPERAND_RECEIVER &&
            receiver->ownership_action == XR_SEM_OPERAND_BORROW &&
            receiver->access == XR_CALL_ARG_MOVE &&
            xr_semantic_class_call_parameter_source_class(semantic, callee->parameter_begin,
                                                          receiver) != XR_SEMANTIC_INDEX_NONE &&
            xr_semantic_owner_transfer_is_exact(semantic, move, &moved_source) &&
            moved_source == move_source->value);

    uint8_t saved_role = receiver->role;
    receiver->role = XR_SEM_OPERAND_ARGUMENT;
    REQUIRE(xr_semantic_class_call_parameter_source_class(semantic, callee->parameter_begin,
                                                          receiver) == XR_SEMANTIC_INDEX_NONE);
    receiver->role = saved_role;
    uint8_t saved_parameter_ownership = parameter->ownership;
    parameter->ownership = XI_OWN_NONE;
    REQUIRE(xr_semantic_class_call_parameter_source_class(semantic, callee->parameter_begin,
                                                          receiver) == XR_SEMANTIC_INDEX_NONE);
    parameter->ownership = saved_parameter_ownership;
    uint8_t saved_move_action = move_source->ownership_action;
    move_source->ownership_action = XR_SEM_OPERAND_BORROW;
    REQUIRE(!xr_semantic_owner_transfer_is_exact(semantic, move, NULL));
    move_source->ownership_action = saved_move_action;

    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "source-instance move receiver TargetPlan failed: %s\n", error);
    REQUIRE(built && plan && plan->calls_count == 1 && plan->call_arguments_count == 1 &&
            (plan->completed_family_mask & XR_TARGET_FAMILY_OWNER_TRANSFER_STORAGE) != 0);
    XrTargetCallArgumentRecord *argument = &plan->call_arguments[0];
    REQUIRE(argument->semantic_value == move->result_value &&
            argument->mode == XR_TARGET_CALL_VALUE && argument->ownership == XR_TARGET_CALL_READ &&
            argument->flags == 0 && argument->caller_slot < plan->slots_count &&
            argument->callee_slot < plan->slots_count &&
            plan->slots[argument->caller_slot].ownership == XR_TARGET_OWNERSHIP_OWNED &&
            plan->slots[argument->callee_slot].ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            xr_target_plan_verify(plan, error, sizeof(error)));
    uint8_t saved_argument_ownership = argument->ownership;
    argument->ownership = XR_TARGET_CALL_CONSUME;
    expect_verify_failure(plan, "XR_TARGET_1003");
    argument->ownership = saved_argument_ownership;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_source_instance_method_result_authority(void) {
    XrSemanticPlan *semantic = build_source_instance_method_semantic(true, false);
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    const XrSemanticCallTargetRecord *target = NULL;
    for (uint32_t i = 0; i < semantic->call_target_count; i++)
        if (semantic->call_targets[i].kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL) {
            REQUIRE(target == NULL);
            target = &semantic->call_targets[i];
        }
    XrSemanticOperationRecord *call = target ? &semantic->operations[target->operation] : NULL;
    REQUIRE(call != NULL && call->opcode == XI_CALL_METHOD &&
            xr_semantic_class_instance_result_source_class(semantic, call) !=
                XR_SEMANTIC_INDEX_NONE);

    uint16_t saved_opcode = call->opcode;
    call->opcode = XI_CALL_METHOD_DIRECT;
    REQUIRE(xr_semantic_class_instance_result_source_class(semantic, call) ==
            XR_SEMANTIC_INDEX_NONE);
    call->opcode = saved_opcode;
    uint8_t saved_ownership = call->result_ownership;
    call->result_ownership = XI_GEN_RESULT_OWNERSHIP_BORROWED;
    REQUIRE(xr_semantic_class_instance_result_source_class(semantic, call) ==
            XR_SEMANTIC_INDEX_NONE);
    call->result_ownership = saved_ownership;

    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "source-instance method result TargetPlan failed: %s\n", error);
    REQUIRE(built && plan && xr_target_plan_verify(plan, error, sizeof(error)));
    XrTargetCallRecord *method = NULL;
    for (uint32_t i = 0; i < plan->calls_count; i++)
        if (plan->calls[i].semantic_operation == target->operation) {
            REQUIRE(method == NULL);
            method = &plan->calls[i];
        }
    const XrTargetValueRepRecord *result = xr_target_plan_value_rep(plan, call->result_value);
    REQUIRE(method != NULL &&
            method->calling_convention == XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL &&
            method->target_kind == XR_TARGET_CALL_TARGET_DIRECT_LOCAL &&
            method->result_mode == XR_TARGET_CALL_VALUE &&
            method->result_ownership == XR_TARGET_CALL_RETURN_OWNED && result != NULL &&
            result->slot < plan->slots_count &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED);
    uint8_t saved_result_ownership = method->result_ownership;
    method->result_ownership = XR_TARGET_CALL_NONE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    method->result_ownership = saved_result_ownership;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_nullable_source_instance_method_result_authority(void) {
    XrSemanticPlan *semantic = build_source_instance_method_semantic(true, true);
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    const XrSemanticCallTargetRecord *target = NULL;
    for (uint32_t i = 0; i < semantic->call_target_count; i++)
        if (semantic->call_targets[i].kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL) {
            REQUIRE(target == NULL);
            target = &semantic->call_targets[i];
        }
    XrSemanticOperationRecord *call = target ? &semantic->operations[target->operation] : NULL;
    XrSemanticTypeRecord *result_type = call ? &semantic->types[call->result_type] : NULL;
    REQUIRE(
        call != NULL && result_type != NULL &&
        result_type->flags ==
            (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
        xr_semantic_nullable_class_instance_type_source_class(semantic, result_type) !=
            XR_SEMANTIC_INDEX_NONE &&
        xr_semantic_class_instance_result_source_class(semantic, call) != XR_SEMANTIC_INDEX_NONE);

    uint8_t saved_flags = result_type->flags;
    result_type->flags |= XR_SEM_TYPE_BORROW_VIEW;
    REQUIRE(xr_semantic_nullable_class_instance_type_source_class(semantic, result_type) ==
                XR_SEMANTIC_INDEX_NONE &&
            xr_semantic_class_instance_result_source_class(semantic, call) ==
                XR_SEMANTIC_INDEX_NONE);
    result_type->flags = saved_flags;

    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "nullable source-instance result TargetPlan failed: %s\n", error);
    REQUIRE(built && plan && xr_target_plan_verify(plan, error, sizeof(error)));
    const XrTargetValueRepRecord *result = xr_target_plan_value_rep(plan, call->result_value);
    REQUIRE(result != NULL && result->slot < plan->slots_count &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[result->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED);
    xr_target_plan_free(plan);
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

static void test_tail_coroutine_chain_fingerprint(void) {
    XrTargetProfile *profile = build_profile(0);
    XrSemanticPlan *semantic = build_lowered_tail_coroutine_chain();
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    REQUIRE(plan->calls_count == 2 && plan->coroutines_count == 2);
    const XrTargetCallRecord *tail_call = NULL;
    const XrTargetCallRecord *suspend_call = NULL;
    for (uint32_t i = 0; i < plan->calls_count; i++) {
        if ((plan->calls[i].flags & XR_TARGET_CALL_TAIL) != 0)
            tail_call = &plan->calls[i];
        if ((plan->calls[i].flags & XR_TARGET_CALL_SUSPEND) != 0)
            suspend_call = &plan->calls[i];
    }
    REQUIRE(tail_call != NULL && suspend_call != NULL && tail_call->flags == XR_TARGET_CALL_TAIL &&
            plan->functions[tail_call->caller_function].coroutine_count == 0);
    char tail_hex[XR_FINGERPRINT_BYTES * 2 + 1];
    xr_fingerprint_hex(tail_call->fingerprint, tail_hex);
    /* Re-anchored because a TargetPlan call fingerprint is built on top of the
     * SemanticPlan fingerprint: xr_target_call_compute_fingerprint hashes
     * plan->semantic_fingerprint first, and xr_semantic_plan.c in turn hashes
     * plan->stdlib_registry_fingerprint, which
     * xr_stdlib_metadata_registry_fingerprint derives from every .def entry.
     * Publishing http2, compress, mem, regex and io from .xr bodies renames their
     * entries, so this digest moves even though the fixture imports nothing.
     * Old: ff35390197a3db0ae3f39e363c1d18700599a3b3ad555e353a3efa78fe945f1e.
     * The canonical-program ownership registry freeze subsequently moved the
     * SemanticPlan fingerprint beneath this unchanged tail-call contract.
     * Old ownership-freeze digest:
     * 6749158010ff69b1cd6d87630c9c7b0ab0acc4b37b8e7e6debd83ea53d4c9d7e.
     * BorrowOriginSet then changed the function-type and stdlib identities
     * hashed into the enclosing SemanticPlan. */
    if (strcmp(tail_hex, "3e537461fb50b52a558dd45fb726b46ffbc103fa962a32c821dbacf1757195b8") != 0)
        fprintf(stderr, "tail-call fingerprint drift: actual=%s\n", tail_hex);
    REQUIRE(strcmp(tail_hex, "3e537461fb50b52a558dd45fb726b46ffbc103fa962a32c821dbacf1757195b8") ==
            0);
    uint32_t tail_id = tail_call->id;
    plan->calls[tail_id].flags = 0;
    expect_verify_failure(plan, "XR_TARGET_1003");
    plan->calls[tail_id].flags = XR_TARGET_CALL_TAIL;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
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
    xr_target_profile_free(profile);
    test_tail_coroutine_chain_fingerprint();
}

static void test_direct_local_value_aggregate_result_storage(void) {
    XrTargetProfile *profile = build_profile(0);
    char error[512] = {0};
    XrTargetPlan *plan = NULL;
    XrSemanticPlan *semantic = build_direct_local_aggregate_call();
    const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(semantic, 0);
    const XrSemanticOperationRecord *operation =
        target ? xr_semantic_plan_operation(semantic, target->operation) : NULL;
    const XrSemanticFunctionRecord *callee =
        target ? xr_semantic_plan_function(semantic, target->function) : NULL;
    REQUIRE(target && target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL && operation && callee &&
            xr_semantic_direct_local_aggregate_result_is_exact(semantic, operation, callee));
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    REQUIRE(plan != NULL && plan->calls_count == 1);
    XrTargetCallRecord *call = &plan->calls[0];
    const XrTargetValueRepRecord *result = xr_target_plan_value_rep(plan, operation->result_value);
    REQUIRE(call->semantic_operation == target->operation &&
            call->calling_convention == XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL &&
            call->target_kind == XR_TARGET_CALL_TARGET_DIRECT_LOCAL &&
            call->result_mode == XR_TARGET_CALL_CALLER_STORAGE &&
            call->result_ownership == XR_TARGET_CALL_NONE && result &&
            call->result_slot == result->slot && call->caller_storage_slot == result->slot &&
            result->register_rep < plan->machine_reps_count &&
            result->memory_rep < plan->machine_reps_count &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_AGGREGATE &&
            plan->machine_reps[result->memory_rep].kind == XR_MACHINE_REP_AGGREGATE);
    uint8_t saved_mode = call->result_mode;
    call->result_mode = XR_TARGET_CALL_VALUE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->result_mode = saved_mode;
    uint32_t saved_storage = call->caller_storage_slot;
    call->caller_storage_slot = XR_SEMANTIC_INDEX_NONE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->caller_storage_slot = saved_storage;
    uint8_t saved_ownership = call->result_ownership;
    call->result_ownership = XR_TARGET_CALL_RETURN_OWNED;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->result_ownership = saved_ownership;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_direct_local_managed_aggregate_lifecycle_authority(void) {
    XrSemanticPlan *semantic = build_direct_local_managed_aggregate_argument();
    const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(semantic, 0);
    const XrSemanticOperationRecord *operation =
        target ? xr_semantic_plan_operation(semantic, target->operation) : NULL;
    const XrSemanticFunctionRecord *callee =
        target ? xr_semantic_plan_function(semantic, target->function) : NULL;
    XrSemanticManagedAggregateArgumentShape shape;
    REQUIRE(target && target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL && operation && callee &&
            xr_semantic_direct_local_managed_aggregate_argument_is_exact(semantic, operation,
                                                                         callee, 0, &shape));
    REQUIRE(shape.semantic_type != XR_SEMANTIC_INDEX_NONE && shape.field_count == 2 &&
            shape.managed_field_count == 1 && xr_semantic_plan_verify(semantic, NULL, 0));

    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "managed aggregate TargetPlan failed: %s\n", error);
    REQUIRE(built && plan != NULL && plan->calls_count == 1 && plan->call_arguments_count == 1 &&
            xr_target_plan_verify(plan, error, sizeof(error)));
    uint32_t layout_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->layouts_count; i++)
        if (plan->layouts[i].semantic_type == shape.semantic_type) {
            REQUIRE(layout_index == XR_SEMANTIC_INDEX_NONE);
            layout_index = i;
        }
    XrStableId zero = {{0}};
    REQUIRE(layout_index != XR_SEMANTIC_INDEX_NONE);
    XrTargetLayoutRecord *layout = &plan->layouts[layout_index];
    XrTargetCallArgumentRecord *argument = &plan->call_arguments[0];
    REQUIRE(layout->kind == XR_TARGET_LAYOUT_AGGREGATE && layout->field_count == 2 &&
            layout->root_field_count == 1 && !xr_stable_id_equal(layout->destructor, zero) &&
            !xr_stable_id_equal(layout->clone, zero) &&
            !xr_stable_id_equal(layout->destructor, layout->clone) &&
            xr_stable_id_equal(layout->equality_hash, zero) &&
            argument->mode == XR_TARGET_CALL_VALUE && argument->ownership == XR_TARGET_CALL_READ &&
            argument->register_rep < plan->machine_reps_count &&
            argument->memory_rep < plan->machine_reps_count &&
            plan->machine_reps[argument->register_rep].kind == XR_MACHINE_REP_AGGREGATE &&
            plan->machine_reps[argument->memory_rep].kind == XR_MACHINE_REP_AGGREGATE &&
            plan->root_maps_count == 0 && plan->cleanups_count == 0);

    XrStableId saved_clone = layout->clone;
    layout->clone.bytes[0] ^= 1u;
    xr_target_layout_compute_fingerprint(plan, layout_index, &layout->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1002");
    layout->clone = saved_clone;
    xr_target_layout_compute_fingerprint(plan, layout_index, &layout->fingerprint);
    XrStableId saved_destructor = layout->destructor;
    layout->destructor.bytes[0] ^= 1u;
    xr_target_layout_compute_fingerprint(plan, layout_index, &layout->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1002");
    layout->destructor = saved_destructor;
    xr_target_layout_compute_fingerprint(plan, layout_index, &layout->fingerprint);
    uint16_t saved_roots = layout->root_field_count;
    layout->root_field_count = 0;
    xr_target_layout_compute_fingerprint(plan, layout_index, &layout->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1002");
    layout->root_field_count = saved_roots;
    xr_target_layout_compute_fingerprint(plan, layout_index, &layout->fingerprint);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);

    semantic = build_direct_local_managed_aggregate_argument();
    target = xr_semantic_plan_call_target(semantic, 0);
    operation = target ? xr_semantic_plan_operation(semantic, target->operation) : NULL;
    callee = target ? xr_semantic_plan_function(semantic, target->function) : NULL;
    REQUIRE(xr_semantic_direct_local_managed_aggregate_argument_is_exact(semantic, operation,
                                                                         callee, 0, &shape));
    XrSemanticParameterRecord *parameter = &semantic->parameters[shape.parameter];
    parameter->ownership = XI_OWN_OWNED;
    REQUIRE(!xr_semantic_direct_local_managed_aggregate_argument_is_exact(semantic, operation,
                                                                          callee, 0, &shape));
    error[0] = '\0';
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)) &&
            strncmp(error, "XR_SEM_", 7) == 0);
    xr_semantic_plan_free(semantic);

    semantic = build_direct_local_managed_aggregate_argument();
    target = xr_semantic_plan_call_target(semantic, 0);
    operation = target ? xr_semantic_plan_operation(semantic, target->operation) : NULL;
    callee = target ? xr_semantic_plan_function(semantic, target->function) : NULL;
    REQUIRE(xr_semantic_direct_local_managed_aggregate_argument_is_exact(semantic, operation,
                                                                         callee, 0, &shape));
    XrSemanticOperandRecord *operand = &semantic->operands[shape.operand];
    operand->ownership_action = XR_SEM_OPERAND_CONSUME;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)) &&
            strstr(error, "XR_SEM_0019") != NULL);
    xr_semantic_plan_free(semantic);

    semantic = build_direct_local_managed_aggregate_argument();
    target = xr_semantic_plan_call_target(semantic, 0);
    operation = target ? xr_semantic_plan_operation(semantic, target->operation) : NULL;
    callee = target ? xr_semantic_plan_function(semantic, target->function) : NULL;
    REQUIRE(xr_semantic_direct_local_managed_aggregate_argument_is_exact(semantic, operation,
                                                                         callee, 0, &shape));
    parameter = &semantic->parameters[shape.parameter];
    uint32_t saved_child = semantic->type_children[semantic->types[parameter->type].child_begin];
    semantic->type_children[semantic->types[parameter->type].child_begin] =
        semantic->types[parameter->type].child_begin == saved_child ? saved_child + 1u : 0u;
    REQUIRE(!xr_semantic_direct_local_managed_aggregate_argument_is_exact(semantic, operation,
                                                                          callee, 0, &shape));
    semantic->type_children[semantic->types[parameter->type].child_begin] = saved_child;
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));

    xr_semantic_plan_free(semantic);
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
    call->calling_convention = XR_TARGET_CALL_CONVENTION_COUNT;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->calling_convention = saved_convention;
    uint8_t saved_kind = call->target_kind;
    call->target_kind = XR_TARGET_CALL_TARGET_CHANNEL_CLOSE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->target_kind = XR_TARGET_CALL_TARGET_COUNT;
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
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "Array intrinsic TargetPlan failed: %s\n", error);
    REQUIRE(built && plan != NULL && plan->calls_count == 2 && plan->call_arguments_count == 3);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    XrTargetCallRecord *with_capacity = NULL;
    XrTargetCallRecord *filled = NULL;
    for (uint32_t i = 0; i < plan->calls_count; i++) {
        XrTargetCallRecord *call = &plan->calls[i];
        REQUIRE(call->calling_convention == XR_TARGET_CALL_CONVENTION_ARRAY_INTRINSIC &&
                call->target_kind == XR_TARGET_CALL_TARGET_ARRAY_INTRINSIC &&
                call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                call->result_mode == XR_TARGET_CALL_VALUE &&
                call->result_ownership == XR_TARGET_CALL_RETURN_OWNED && call->adapter_count == 0 &&
                call->flags == 0);
        if (call->array_intrinsic_kind == XR_TARGET_ARRAY_INTRINSIC_WITH_CAPACITY)
            with_capacity = call;
        else if (call->array_intrinsic_kind == XR_TARGET_ARRAY_INTRINSIC_FILLED_NEW)
            filled = call;
    }
    REQUIRE(with_capacity != NULL && filled != NULL &&
            with_capacity->array_element_storage == XR_TARGET_ARRAY_STORAGE_U8 &&
            with_capacity->argument_count == 1 &&
            filled->array_element_storage == XR_TARGET_ARRAY_STORAGE_U8 &&
            filled->argument_count == 2);
    for (uint32_t i = 0; i < plan->call_arguments_count; i++) {
        XrTargetCallArgumentRecord *argument = &plan->call_arguments[i];
        REQUIRE(argument->callee_parameter == XR_SEMANTIC_INDEX_NONE &&
                argument->callee_slot == XR_SEMANTIC_INDEX_NONE &&
                argument->mode == XR_TARGET_CALL_VALUE &&
                argument->ownership == XR_TARGET_CALL_CONSUME && argument->flags == 0);
    }

    uint8_t saved_kind = with_capacity->array_intrinsic_kind;
    with_capacity->array_intrinsic_kind = XR_TARGET_ARRAY_INTRINSIC_FILLED_NEW;
    expect_verify_failure(plan, "XR_TARGET_1003");
    with_capacity->array_intrinsic_kind = saved_kind;

    uint8_t saved_storage = with_capacity->array_element_storage;
    with_capacity->array_element_storage = XR_TARGET_ARRAY_STORAGE_I64;
    expect_verify_failure(plan, "XR_TARGET_1003");
    with_capacity->array_element_storage = saved_storage;

    XrTargetCallArgumentRecord *first = &plan->call_arguments[with_capacity->argument_begin];
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
    REQUIRE(xr_c_emission_plan_build(plan, xr_target_plan_semantic_plan(plan),
                                     xr_target_profile_fingerprint(profile), &emission, error,
                                     sizeof(error)));
    XrCValueEmissionView with_capacity_view = {0};
    XrCValueEmissionView filled_view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, with_capacity->result_value,
                                          &with_capacity_view, error, sizeof(error)) &&
            xr_c_emission_plan_value_view(emission, filled->result_value, &filled_view, error,
                                          sizeof(error)));
    const XrSemanticOperationRecord *with_capacity_operation =
        xr_semantic_plan_operation(semantic, with_capacity->semantic_operation);
    const XrSemanticOperationRecord *filled_operation =
        xr_semantic_plan_operation(semantic, filled->semantic_operation);
    uint32_t semantic_operand_count = 0;
    const XrSemanticOperandRecord *semantic_operands =
        xr_semantic_plan_operands(semantic, &semantic_operand_count);
    REQUIRE(with_capacity_operation && filled_operation && semantic_operands &&
            with_capacity_operation->operand_begin < semantic_operand_count &&
            filled_operation->operand_begin + 1u < semantic_operand_count &&
            with_capacity_view.rep == XR_C_VALUE_REP_TAGGED &&
            with_capacity_view.materialization == XR_C_VALUE_MATERIALIZATION_ARRAY_WITH_CAPACITY &&
            with_capacity_view.recipe_discriminant == XR_TARGET_ARRAY_STORAGE_U8 &&
            with_capacity_view.recipe_operand_value ==
                semantic_operands[with_capacity_operation->operand_begin].value &&
            with_capacity_view.recipe_argument_value == UINT32_MAX &&
            with_capacity_view.recipe_symbol &&
            strcmp(with_capacity_view.recipe_symbol, "xrt_array_with_capacity_value") == 0 &&
            filled_view.rep == XR_C_VALUE_REP_TAGGED &&
            filled_view.materialization == XR_C_VALUE_MATERIALIZATION_ARRAY_FILLED_NEW &&
            filled_view.recipe_discriminant == XR_TARGET_ARRAY_STORAGE_U8 &&
            filled_view.recipe_operand_value ==
                semantic_operands[filled_operation->operand_begin].value &&
            filled_view.recipe_argument_value ==
                semantic_operands[filled_operation->operand_begin + 1u].value &&
            filled_view.recipe_symbol &&
            strcmp(filled_view.recipe_symbol, "xrt_array_new_filled_value") == 0);
    xr_c_emission_plan_free(emission);

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

typedef struct ArrayHofExpectation {
    uint8_t xi_kind;
    uint8_t semantic_kind;
    uint8_t target_kind;
    uint8_t c_kind;
    uint8_t c_result_rep;
    uint8_t callback_parameter_reps[2];
    uint8_t callback_return_rep;
    uint8_t result_ownership;
    uint16_t operand_count;
} ArrayHofExpectation;

static const ArrayHofExpectation array_hof_expectations[] = {
    {XI_ARRAY_HOF_MAP,
     XR_SEM_ARRAY_HOF_MAP,
     XR_TARGET_ARRAY_HOF_MAP,
     XR_C_ARRAY_HOF_MAP,
     XR_C_VALUE_REP_TAGGED,
     {XR_C_VALUE_REP_I64, XR_C_VALUE_REP_VOID},
     XR_C_VALUE_REP_I64,
     XR_TARGET_CALL_RETURN_OWNED,
     2},
    {XI_ARRAY_HOF_FILTER,
     XR_SEM_ARRAY_HOF_FILTER,
     XR_TARGET_ARRAY_HOF_FILTER,
     XR_C_ARRAY_HOF_FILTER,
     XR_C_VALUE_REP_TAGGED,
     {XR_C_VALUE_REP_I64, XR_C_VALUE_REP_VOID},
     XR_C_VALUE_REP_BOOL,
     XR_TARGET_CALL_RETURN_OWNED,
     2},
    {XI_ARRAY_HOF_REDUCE,
     XR_SEM_ARRAY_HOF_REDUCE,
     XR_TARGET_ARRAY_HOF_REDUCE,
     XR_C_ARRAY_HOF_REDUCE,
     XR_C_VALUE_REP_I64,
     {XR_C_VALUE_REP_I64, XR_C_VALUE_REP_I64},
     XR_C_VALUE_REP_I64,
     XR_TARGET_CALL_NONE,
     3},
};

static void test_array_hof_call_authority_case(const ArrayHofExpectation *expected) {
    XrSemanticPlan *semantic = build_array_hof_semantic(expected->xi_kind);
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    XrSemanticOperationRecord *operation = NULL;
    for (uint32_t i = 0; i < semantic->operation_count; i++)
        if (semantic->operations[i].intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_HOF)
            operation = &semantic->operations[i];
    REQUIRE(operation != NULL && operation->array_hof_kind == expected->semantic_kind &&
            operation->array_element_storage == XR_ELEM_I64 &&
            operation->array_result_element_storage == XR_ELEM_I64 &&
            operation->callable_function < semantic->function_count &&
            operation->operand_count == expected->operand_count);
    XrSemanticOperandRecord *operands = &semantic->operands[operation->operand_begin];
    REQUIRE(operands[0].role == XR_SEM_OPERAND_RECEIVER && operands[0].parameter == -1 &&
            operands[1].role == XR_SEM_OPERAND_ARGUMENT && operands[1].parameter == 0 &&
            (expected->operand_count == 2 ||
             (operands[2].role == XR_SEM_OPERAND_ARGUMENT && operands[2].parameter == 1)));

    uint8_t saved_u8 = operation->array_hof_kind;
    operation->array_hof_kind = expected->semantic_kind == XR_SEM_ARRAY_HOF_MAP
                                    ? XR_SEM_ARRAY_HOF_FILTER
                                    : XR_SEM_ARRAY_HOF_MAP;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    operation->array_hof_kind = saved_u8;
    saved_u8 = operation->array_result_element_storage;
    operation->array_result_element_storage = XR_ELEM_U8;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    operation->array_result_element_storage = saved_u8;
    uint32_t saved_u32 = operation->callable_function;
    operation->callable_function = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    operation->callable_function = saved_u32;
    int16_t saved_i16 = operands[1].parameter;
    operands[1].parameter = 1;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    operands[1].parameter = saved_i16;
    XrSemanticFunctionRecord *callee = &semantic->functions[operation->callable_function];
    uint32_t saved_effects = callee->semantic_effects;
    callee->semantic_effects |= XI_EFFECT_MAY_THROW;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    callee->semantic_effects = saved_effects;
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));

    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "Array HOF TargetPlan failed: %s\n", error);
    REQUIRE(built && plan != NULL && plan->calls_count == 1 &&
            plan->call_arguments_count == expected->operand_count &&
            (plan->completed_family_mask & XR_TARGET_FAMILY_ARRAY_HOF_RESULT_STORAGE) != 0);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    XrTargetCallRecord *call = &plan->calls[0];
    REQUIRE(call->semantic_operation == (uint32_t) (operation - semantic->operations) &&
            call->callee_function == operation->callable_function &&
            call->calling_convention == XR_TARGET_CALL_CONVENTION_ARRAY_HOF &&
            call->target_kind == XR_TARGET_CALL_TARGET_ARRAY_HOF &&
            call->array_hof_kind == expected->target_kind &&
            call->array_element_storage == XR_TARGET_ARRAY_STORAGE_I64 &&
            call->array_result_element_storage == XR_TARGET_ARRAY_STORAGE_I64 &&
            call->result_ownership == expected->result_ownership &&
            call->argument_count == expected->operand_count);
    XrTargetCallArgumentRecord *arguments = &plan->call_arguments[call->argument_begin];
    for (uint16_t i = 0; i < expected->operand_count; i++) {
        REQUIRE(arguments[i].semantic_value == operands[i].value && arguments[i].ordinal == i &&
                arguments[i].ownership ==
                    (i == 0 ? XR_TARGET_CALL_BORROW : XR_TARGET_CALL_CONSUME) &&
                arguments[i].callee_parameter == XR_SEMANTIC_INDEX_NONE);
    }
    XrTargetValueRepRecord *result = &plan->value_reps[call->result_value];
    REQUIRE(result->slot == call->result_slot);
    if (expected->target_kind == XR_TARGET_ARRAY_HOF_REDUCE) {
        REQUIRE(plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_I64 &&
                plan->slots[result->slot].root_kind == XR_TARGET_ROOT_NONE &&
                plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_TRIVIAL);
    } else {
        REQUIRE(plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
                plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED);
    }

    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(plan, xr_target_plan_semantic_plan(plan), profile_fingerprint,
                                     &emission, error, sizeof(error)));
    XrCValueEmissionView hof_view = {0};
    REQUIRE(
        xr_c_emission_plan_value_view(emission, call->result_value, &hof_view, error,
                                      sizeof(error)) &&
        hof_view.materialization == XR_C_VALUE_MATERIALIZATION_ARRAY_HOF_DIRECT &&
        hof_view.rep == expected->c_result_rep &&
        hof_view.recipe_callee_function == call->callee_function &&
        hof_view.recipe_hof_kind == expected->c_kind &&
        hof_view.recipe_hof_source_storage == XR_TARGET_ARRAY_STORAGE_I64 &&
        hof_view.recipe_hof_result_storage == XR_TARGET_ARRAY_STORAGE_I64 &&
        hof_view.recipe_hof_callback_parameter_reps[0] == expected->callback_parameter_reps[0] &&
        hof_view.recipe_hof_callback_parameter_reps[1] == expected->callback_parameter_reps[1] &&
        hof_view.recipe_hof_callback_return_rep == expected->callback_return_rep &&
        hof_view.recipe_hof_reserved == 0 &&
        hof_view.recipe_argument_count == expected->operand_count &&
        hof_view.recipe_arguments == emission->recipe_arguments &&
        emission->recipe_argument_count == expected->operand_count &&
        xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                  profile_fingerprint, error, sizeof(error)));
    const uint8_t expected_argument_kinds[3] = {
        XR_C_RECIPE_ARGUMENT_ARRAY_HOF_RECEIVER,
        XR_C_RECIPE_ARGUMENT_ARRAY_HOF_CALLBACK,
        XR_C_RECIPE_ARGUMENT_ARRAY_HOF_SEED,
    };
    XrCRecipeArgumentView *hof_arguments = (XrCRecipeArgumentView *) hof_view.recipe_arguments;
    for (uint16_t i = 0; i < expected->operand_count; i++) {
        REQUIRE(hof_arguments[i].kind == expected_argument_kinds[i] &&
                hof_arguments[i].semantic_value == operands[i].value &&
                hof_arguments[i].source_semantic_value == operands[i].value &&
                hof_arguments[i].reserved[0] == 0 && hof_arguments[i].reserved[1] == 0 &&
                hof_arguments[i].reserved[2] == 0);
    }

    XrCValueEmissionView *mutable_hof = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == call->result_value)
            mutable_hof = &emission->values[i];
    REQUIRE(mutable_hof != NULL);
    saved_u32 = mutable_hof->recipe_callee_function;
    mutable_hof->recipe_callee_function ^= 1u;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_hof->recipe_callee_function = saved_u32;
    saved_u8 = mutable_hof->recipe_hof_kind;
    mutable_hof->recipe_hof_kind =
        saved_u8 == XR_C_ARRAY_HOF_MAP ? XR_C_ARRAY_HOF_FILTER : XR_C_ARRAY_HOF_MAP;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_hof->recipe_hof_kind = saved_u8;
    saved_u8 = mutable_hof->recipe_hof_source_storage;
    mutable_hof->recipe_hof_source_storage = XR_TARGET_ARRAY_STORAGE_U8;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_hof->recipe_hof_source_storage = saved_u8;
    saved_u8 = mutable_hof->recipe_hof_result_storage;
    mutable_hof->recipe_hof_result_storage = XR_TARGET_ARRAY_STORAGE_U8;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_hof->recipe_hof_result_storage = saved_u8;
    saved_u8 = mutable_hof->recipe_hof_callback_parameter_reps[0];
    mutable_hof->recipe_hof_callback_parameter_reps[0] = XR_C_VALUE_REP_BOOL;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_hof->recipe_hof_callback_parameter_reps[0] = saved_u8;
    saved_u8 = mutable_hof->recipe_hof_callback_parameter_reps[1];
    mutable_hof->recipe_hof_callback_parameter_reps[1] =
        saved_u8 == XR_C_VALUE_REP_VOID ? XR_C_VALUE_REP_I64 : XR_C_VALUE_REP_VOID;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_hof->recipe_hof_callback_parameter_reps[1] = saved_u8;
    saved_u8 = mutable_hof->recipe_hof_callback_return_rep;
    mutable_hof->recipe_hof_callback_return_rep =
        saved_u8 == XR_C_VALUE_REP_BOOL ? XR_C_VALUE_REP_I64 : XR_C_VALUE_REP_BOOL;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_hof->recipe_hof_callback_return_rep = saved_u8;
    mutable_hof->recipe_hof_reserved = 1;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_hof->recipe_hof_reserved = 0;
    uint16_t saved_u16 = mutable_hof->recipe_argument_count;
    mutable_hof->recipe_argument_count = 0;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_hof->recipe_argument_count = saved_u16;
    const XrCRecipeArgumentView *saved_recipe_arguments = mutable_hof->recipe_arguments;
    mutable_hof->recipe_arguments = NULL;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_hof->recipe_arguments = saved_recipe_arguments;
    mutable_hof->recipe_arguments = saved_recipe_arguments + 1;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_hof->recipe_arguments = saved_recipe_arguments;
    saved_u32 = emission->recipe_argument_count;
    emission->recipe_argument_count--;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    emission->recipe_argument_count = saved_u32 + 1;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    emission->recipe_argument_count = saved_u32;
    for (uint16_t i = 0; i < expected->operand_count; i++) {
        saved_u8 = hof_arguments[i].kind;
        hof_arguments[i].kind = XR_C_RECIPE_ARGUMENT_INVALID;
        REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                           profile_fingerprint, error, sizeof(error)));
        hof_arguments[i].kind = saved_u8;
        saved_u32 = hof_arguments[i].semantic_value;
        hof_arguments[i].semantic_value = UINT32_MAX;
        REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                           profile_fingerprint, error, sizeof(error)));
        hof_arguments[i].semantic_value = saved_u32;
        saved_u32 = hof_arguments[i].source_semantic_value;
        hof_arguments[i].source_semantic_value = UINT32_MAX;
        REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                           profile_fingerprint, error, sizeof(error)));
        hof_arguments[i].source_semantic_value = saved_u32;
        for (uint8_t reserved = 0; reserved < 3; reserved++) {
            hof_arguments[i].reserved[reserved] = 1;
            REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                               profile_fingerprint, error, sizeof(error)));
            hof_arguments[i].reserved[reserved] = 0;
        }
    }
    XrFingerprint saved_fingerprint = emission->fingerprint;
    emission->fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    emission->fingerprint = saved_fingerprint;
    REQUIRE(xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                      profile_fingerprint, error, sizeof(error)));

    XrCEmissionPlan *same_emission = NULL;
    REQUIRE(xr_c_emission_plan_build(plan, xr_target_plan_semantic_plan(plan), profile_fingerprint,
                                     &same_emission, error, sizeof(error)) &&
            xr_fingerprint_equal(xr_c_emission_plan_fingerprint(emission),
                                 xr_c_emission_plan_fingerprint(same_emission)));
    XrCValueEmissionView same_view = {0};
    REQUIRE(xr_c_emission_plan_value_view(same_emission, call->result_value, &same_view, error,
                                          sizeof(error)) &&
            same_view.recipe_callee_function == hof_view.recipe_callee_function &&
            same_view.recipe_hof_kind == hof_view.recipe_hof_kind &&
            same_view.recipe_hof_source_storage == hof_view.recipe_hof_source_storage &&
            same_view.recipe_hof_result_storage == hof_view.recipe_hof_result_storage &&
            same_view.recipe_hof_callback_parameter_reps[0] ==
                hof_view.recipe_hof_callback_parameter_reps[0] &&
            same_view.recipe_hof_callback_parameter_reps[1] ==
                hof_view.recipe_hof_callback_parameter_reps[1] &&
            same_view.recipe_hof_callback_return_rep == hof_view.recipe_hof_callback_return_rep &&
            same_view.recipe_hof_reserved == 0 &&
            same_view.recipe_argument_count == expected->operand_count);
    for (uint16_t i = 0; i < expected->operand_count; i++)
        REQUIRE(memcmp(&same_view.recipe_arguments[i], &hof_arguments[i],
                       sizeof(hof_arguments[i])) == 0);
    xr_c_emission_plan_free(same_emission);

    saved_u8 = call->array_hof_kind;
    call->array_hof_kind =
        saved_u8 == XR_TARGET_ARRAY_HOF_MAP ? XR_TARGET_ARRAY_HOF_FILTER : XR_TARGET_ARRAY_HOF_MAP;
    expect_verify_failure(plan, "XR_TARGET_1003");
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    call->array_hof_kind = saved_u8;
    saved_u8 = call->array_element_storage;
    call->array_element_storage = XR_TARGET_ARRAY_STORAGE_U8;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->array_element_storage = saved_u8;
    saved_u8 = call->array_result_element_storage;
    call->array_result_element_storage = XR_TARGET_ARRAY_STORAGE_U8;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->array_result_element_storage = saved_u8;
    saved_u32 = call->callee_function;
    call->callee_function = XR_SEMANTIC_INDEX_NONE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->callee_function = saved_u32;
    saved_u16 = arguments[0].ordinal;
    arguments[0].ordinal = 1;
    expect_verify_failure(plan, "XR_TARGET_1003");
    arguments[0].ordinal = saved_u16;
    saved_u8 = arguments[1].ownership;
    arguments[1].ownership = XR_TARGET_CALL_READ;
    expect_verify_failure(plan, "XR_TARGET_1003");
    arguments[1].ownership = saved_u8;
    XrStableId saved_identity = call->identity;
    call->identity.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->identity = saved_identity;
    uint64_t saved_mask = plan->completed_family_mask;
    plan->completed_family_mask &= ~XR_TARGET_FAMILY_ARRAY_HOF_RESULT_STORAGE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->completed_family_mask = saved_mask;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)) &&
            xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                      profile_fingerprint, error, sizeof(error)));

    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    XrXtpCandidate *candidate = NULL;
    XrTargetPlan *decoded = NULL;
    REQUIRE(xr_xtp_encode_plan(plan, &encoded, &encoded_size, error, sizeof(error)));
    uint8_t *call_entry = target_test_xtp_directory_entry(encoded, XR_XTP_SECTION_CALLS);
    size_t call_offset = (size_t) xr_xtp_take_u64(call_entry + 8);
    REQUIRE(encoded[call_offset + 140] == XR_TARGET_ARRAY_STORAGE_I64 &&
            encoded[call_offset + 141] == expected->target_kind &&
            encoded[call_offset + 142] == XR_TARGET_ARRAY_STORAGE_I64);
    REQUIRE(xr_xtp_decode_candidate(encoded, encoded_size, &candidate, error, sizeof(error)) &&
            xr_xtp_materialize_target_plan(candidate, semantic, profile, &decoded, error,
                                           sizeof(error)));
    REQUIRE(decoded->calls_count == 1 &&
            decoded->calls[0].array_hof_kind == expected->target_kind &&
            decoded->calls[0].array_element_storage == XR_TARGET_ARRAY_STORAGE_I64 &&
            decoded->calls[0].array_result_element_storage == XR_TARGET_ARRAY_STORAGE_I64 &&
            xr_fingerprint_equal(decoded->fingerprint, plan->fingerprint));
    xr_target_plan_free(decoded);
    xr_xtp_candidate_release(candidate);

    encoded[call_offset + 141] = expected->target_kind == XR_TARGET_ARRAY_HOF_MAP
                                     ? XR_TARGET_ARRAY_HOF_FILTER
                                     : XR_TARGET_ARRAY_HOF_MAP;
    target_test_xtp_resign_section(encoded, XR_XTP_SECTION_CALLS);
    target_test_xtp_resign_artifact(encoded, encoded_size);
    candidate = NULL;
    decoded = NULL;
    REQUIRE(xr_xtp_decode_candidate(encoded, encoded_size, &candidate, error, sizeof(error)));
    REQUIRE(!xr_xtp_materialize_target_plan(candidate, semantic, profile, &decoded, error,
                                            sizeof(error)) &&
            decoded == NULL);
    xr_xtp_candidate_release(candidate);
    xr_xtp_encoded_free(encoded);

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_array_hof_call_authority(void) {
    for (size_t i = 0; i < sizeof(array_hof_expectations) / sizeof(array_hof_expectations[0]); i++)
        test_array_hof_call_authority_case(&array_hof_expectations[i]);

    char error[512] = {0};
    XrTargetProfile *profile = build_profile(0);
    XrSemanticPlan *direct_semantic =
        build_direct_local_scalar_calls(XI_CALL, &stub_int, &stub_function);
    XrTargetPlan *direct = NULL;
    REQUIRE(xr_target_plan_build(direct_semantic, profile, &direct, error, sizeof(error)) &&
            xr_target_instruction_program_verify(direct, error, sizeof(error)));
    XrTargetCallRecord *direct_call = &direct->calls[0];
    direct_call->array_hof_kind = XR_TARGET_ARRAY_HOF_MAP;
    REQUIRE(!xr_target_instruction_program_verify(direct, error, sizeof(error)));
    direct_call->array_hof_kind = XR_TARGET_ARRAY_HOF_NONE;
    direct_call->array_result_element_storage = XR_TARGET_ARRAY_STORAGE_I64;
    REQUIRE(!xr_target_instruction_program_verify(direct, error, sizeof(error)));
    direct_call->array_result_element_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    REQUIRE(xr_target_instruction_program_verify(direct, error, sizeof(error)));
    xr_target_plan_free(direct);
    xr_semantic_plan_free(direct_semantic);
    xr_target_profile_free(profile);
}

static void test_tagged_string_array_copy_authority(void) {
    XrSemanticPlan *semantic = build_tagged_string_array_copy_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "tagged String Array copy TargetPlan failed: %s\n", error);
    REQUIRE(built && plan && xr_target_plan_verify(plan, error, sizeof(error)));

    const XrSemanticOperationRecord *operation = NULL;
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    uint32_t argument_value = XR_SEMANTIC_INDEX_NONE;
    uint8_t element_storage = UINT8_MAX;
    for (uint32_t i = 0; i < (uint32_t) xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        uint32_t candidate_argument = XR_SEMANTIC_INDEX_NONE;
        uint8_t candidate_storage = UINT8_MAX;
        if (!xr_semantic_container_copy_is_exact(semantic, candidate, &candidate_argument,
                                                 &candidate_storage))
            continue;
        REQUIRE(operation == NULL);
        operation = candidate;
        operation_index = i;
        argument_value = candidate_argument;
        element_storage = candidate_storage;
    }
    REQUIRE(operation && operation_index != XR_SEMANTIC_INDEX_NONE &&
            element_storage == XR_ELEM_ANY);
    const XrTargetValueRepRecord *source = xr_target_plan_value_rep(plan, argument_value);
    const XrTargetValueRepRecord *result = xr_target_plan_value_rep(plan, operation->result_value);
    REQUIRE(source && result && source->slot < plan->slots_count &&
            result->slot < plan->slots_count &&
            plan->machine_reps[source->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[source->register_rep].ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            plan->slots[source->slot].role == XR_TARGET_SLOT_PARAMETER &&
            plan->slots[source->slot].ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[result->register_rep].ownership == XR_TARGET_OWNERSHIP_OWNED &&
            plan->slots[result->slot].semantic_operation == operation_index &&
            plan->slots[result->slot].role == XR_TARGET_SLOT_TEMPORARY &&
            plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
            plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED);

    XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; i < plan->calls_count; i++) {
        if (plan->calls[i].calling_convention != XR_TARGET_CALL_CONVENTION_CONTAINER_COPY)
            continue;
        REQUIRE(call == NULL);
        call = &plan->calls[i];
    }
    REQUIRE(call != NULL);
    REQUIRE(call->semantic_operation == operation_index && call->argument_count == 0 &&
            call->result_value == operation->result_value && call->result_slot == result->slot &&
            call->result_mode == XR_TARGET_CALL_VALUE &&
            call->result_ownership == XR_TARGET_CALL_RETURN_OWNED &&
            call->target_kind == XR_TARGET_CALL_TARGET_CONTAINER_COPY &&
            call->array_element_storage == XR_TARGET_ARRAY_STORAGE_TAGGED && call->flags == 0);
    uint32_t layout_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->layouts_count; i++) {
        if (plan->layouts[i].semantic_type != operation->result_type)
            continue;
        REQUIRE(layout_index == XR_SEMANTIC_INDEX_NONE);
        layout_index = i;
    }
    REQUIRE(layout_index != XR_SEMANTIC_INDEX_NONE &&
            plan->layouts[layout_index].kind == XR_TARGET_LAYOUT_DYNAMIC &&
            plan->layouts[layout_index].array_element_storage == XR_TARGET_ARRAY_STORAGE_TAGGED);

    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(plan, xr_target_plan_semantic_plan(plan), profile_fingerprint,
                                     &emission, error, sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, operation->result_value, &view, error,
                                          sizeof(error)) &&
            view.rep == XR_C_VALUE_REP_TAGGED &&
            view.target_register_kind == XR_MACHINE_REP_DYN_VALUE &&
            view.target_memory_kind == XR_MACHINE_REP_DYN_VALUE &&
            view.materialization == XR_C_VALUE_MATERIALIZATION_NONE &&
            xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                      profile_fingerprint, error, sizeof(error)));

    uint8_t saved_kind = call->target_kind;
    call->target_kind = XR_TARGET_CALL_TARGET_SCALAR_COPY;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->target_kind = saved_kind;
    uint8_t saved_call_storage = call->array_element_storage;
    call->array_element_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    call->array_element_storage = saved_call_storage;
    REQUIRE(xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                      profile_fingerprint, error, sizeof(error)));
    uint8_t saved_ownership = call->result_ownership;
    call->result_ownership = XR_TARGET_CALL_NONE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->result_ownership = saved_ownership;
    uint8_t saved_storage = plan->layouts[layout_index].array_element_storage;
    plan->layouts[layout_index].array_element_storage = XR_TARGET_ARRAY_STORAGE_U8;
    xr_target_layout_compute_fingerprint(plan, layout_index,
                                         &plan->layouts[layout_index].fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->layouts[layout_index].array_element_storage = saved_storage;
    xr_target_layout_compute_fingerprint(plan, layout_index,
                                         &plan->layouts[layout_index].fingerprint);

    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    XrSemanticTypeRecord *result_type = &semantic->types[operation->result_type];
    REQUIRE(children && result_type->child_begin < child_count);
    XrSemanticTypeRecord *element = &semantic->types[children[result_type->child_begin]];
    uint16_t saved_element_kind = element->kind;
    element->kind = XR_KIND_INSTANCE;
    REQUIRE(!xr_semantic_container_copy_is_exact(semantic, operation, NULL, NULL));
    element->kind = saved_element_kind;
    REQUIRE(xr_semantic_container_copy_is_exact(semantic, operation, NULL, NULL) &&
            xr_target_plan_verify(plan, error, sizeof(error)));

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_tagged_array_index_read_authority(void) {
    XrSemanticPlan *semantic = build_tagged_string_array_index_read_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "tagged Array index-read TargetPlan failed: %s\n", error);
    REQUIRE(built && plan && xr_target_plan_verify(plan, error, sizeof(error)));

    XrSemanticOperationRecord *read = NULL;
    uint32_t read_operation = XR_SEMANTIC_INDEX_NONE;
    uint32_t array_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t index_value = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < (uint32_t) xr_semantic_plan_operation_count(semantic); i++) {
        XrSemanticOperationRecord *candidate = &semantic->operations[i];
        uint32_t candidate_array = XR_SEMANTIC_INDEX_NONE;
        uint32_t candidate_index = XR_SEMANTIC_INDEX_NONE;
        if (!xr_semantic_array_index_tagged_read_is_exact(semantic, candidate, &candidate_array,
                                                          &candidate_index))
            continue;
        REQUIRE(read == NULL);
        read = candidate;
        read_operation = i;
        array_value = candidate_array;
        index_value = candidate_index;
    }
    REQUIRE(read && read_operation != XR_SEMANTIC_INDEX_NONE &&
            array_value != XR_SEMANTIC_INDEX_NONE && index_value != XR_SEMANTIC_INDEX_NONE);
    const XrTargetValueRepRecord *binding = xr_target_plan_value_rep(plan, read->result_value);
    REQUIRE(binding && binding->slot < plan->slots_count &&
            plan->machine_reps[binding->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[binding->register_rep].ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            plan->machine_reps[binding->register_rep].root_kind == XR_TARGET_ROOT_DYNAMIC &&
            plan->slots[binding->slot].semantic_operation == read_operation &&
            plan->slots[binding->slot].role == XR_TARGET_SLOT_TEMPORARY &&
            plan->slots[binding->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
            plan->slots[binding->slot].ownership == XR_TARGET_OWNERSHIP_BORROWED);

    uint8_t saved_slot_ownership = plan->slots[binding->slot].ownership;
    plan->slots[binding->slot].ownership = XR_TARGET_OWNERSHIP_OWNED;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[binding->slot].ownership = saved_slot_ownership;
    uint8_t saved_rep_ownership = plan->machine_reps[binding->register_rep].ownership;
    plan->machine_reps[binding->register_rep].ownership = XR_TARGET_OWNERSHIP_OWNED;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[binding->register_rep].ownership = saved_rep_ownership;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    uint8_t saved_result_ownership = read->result_ownership;
    read->result_ownership = XI_GEN_RESULT_OWNERSHIP_OWNED;
    REQUIRE(!xr_semantic_array_index_read_is_exact(semantic, read, NULL, NULL));
    error[0] = '\0';
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)) &&
            strstr(error, "XR_SEM_0019") != NULL);
    read->result_ownership = saved_result_ownership;
    XrSemanticOperandRecord *receiver = &semantic->operands[read->operand_begin];
    XrSemanticOperandRecord *index = receiver + 1;
    uint32_t saved_result_type = read->result_type;
    read->result_type = index->type;
    REQUIRE(!xr_semantic_array_index_read_is_exact(semantic, read, NULL, NULL));
    read->result_type = saved_result_type;
    uint32_t saved_index_type = index->type;
    index->type = read->result_type;
    REQUIRE(!xr_semantic_array_index_read_is_exact(semantic, read, NULL, NULL));
    index->type = saved_index_type;
    uint8_t saved_receiver_flags = receiver->flags;
    receiver->flags = XR_SEM_OPERAND_CALL_CONTRACT;
    REQUIRE(!xr_semantic_array_index_read_is_exact(semantic, read, NULL, NULL));
    receiver->flags = saved_receiver_flags;
    REQUIRE(xr_semantic_array_index_tagged_read_is_exact(semantic, read, NULL, NULL) &&
            xr_semantic_plan_verify(semantic, error, sizeof(error)) &&
            xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_shared_const_i64_array_layout_authority(void) {
    XrSemanticPlan *semantic = build_shared_const_i64_array_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "shared const Array<i64> TargetPlan failed: %s\n", error);
    REQUIRE(built && plan != NULL && xr_target_plan_verify(plan, error, sizeof(error)));

    const XrSemanticOperationRecord *load = NULL;
    uint32_t load_operation = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < (uint32_t) xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->opcode != XI_GET_SHARED)
            continue;
        REQUIRE(load == NULL);
        load = operation;
        load_operation = i;
    }
    REQUIRE(load != NULL && load_operation != XR_SEMANTIC_INDEX_NONE);
    const XrSemanticTypeRecord *array_type = xr_semantic_plan_type(semantic, load->result_type);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    REQUIRE(array_type != NULL && xr_semantic_array_type_row_is_exact(array_type) &&
            (array_type->flags & XR_SEM_TYPE_CONST) != 0 && children != NULL &&
            array_type->child_begin < child_count);
    const XrSemanticTypeRecord *element =
        xr_semantic_plan_type(semantic, children[array_type->child_begin]);
    REQUIRE(element != NULL && element->kind == XR_KIND_INT);

    const XrTargetValueRepRecord *binding = xr_target_plan_value_rep(plan, load->result_value);
    REQUIRE(binding != NULL && binding->slot < plan->slots_count &&
            plan->slots[binding->slot].semantic_operation == load_operation &&
            plan->slots[binding->slot].ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            plan->machine_reps[binding->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[binding->register_rep].ownership == XR_TARGET_OWNERSHIP_BORROWED);

    uint32_t layout_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->layouts_count; i++) {
        if (plan->layouts[i].semantic_type != load->result_type)
            continue;
        REQUIRE(layout_index == XR_SEMANTIC_INDEX_NONE);
        layout_index = i;
    }
    REQUIRE(layout_index != XR_SEMANTIC_INDEX_NONE &&
            plan->layouts[layout_index].kind == XR_TARGET_LAYOUT_DYNAMIC &&
            plan->layouts[layout_index].array_element_storage == XR_TARGET_ARRAY_STORAGE_I64);

    uint8_t saved_storage = plan->layouts[layout_index].array_element_storage;
    plan->layouts[layout_index].array_element_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    xr_target_layout_compute_fingerprint(plan, layout_index,
                                         &plan->layouts[layout_index].fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->layouts[layout_index].array_element_storage = saved_storage;
    xr_target_layout_compute_fingerprint(plan, layout_index,
                                         &plan->layouts[layout_index].fingerprint);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_array_reserve_call_authority(void) {
    XrSemanticPlan *semantic = build_array_reserve_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "Array.reserve TargetPlan failed: %s\n", error);
    REQUIRE(built && plan && plan->calls_count == 1 && plan->call_arguments_count == 0);
    XrTargetCallRecord *call = &plan->calls[0];
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(semantic, call->semantic_operation);
    REQUIRE(operation && operation->opcode == XI_CALL_BUILTIN &&
            operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_MEMBER_SCALAR &&
            operation->evidence[1] == XA_INTRINSIC_ARRAY_RESERVE &&
            operation->metadata_count == 0 && operation->auxiliary_kind == XI_AUX_KIND_NONE &&
            operation->result_alias_operand == 0 &&
            call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
            call->result_value == operation->result_value &&
            call->calling_convention == XR_TARGET_CALL_CONVENTION_ARRAY_MEMBER_SCALAR &&
            call->target_kind == XR_TARGET_CALL_TARGET_ARRAY_MEMBER_SCALAR &&
            call->result_mode == XR_TARGET_CALL_VALUE &&
            call->result_ownership == XR_TARGET_CALL_NONE && call->adapter_count == 0 &&
            call->flags == 0);
    const XrTargetValueRepRecord *result = xr_target_plan_value_rep(plan, operation->result_value);
    REQUIRE(result && result->slot < plan->slots_count &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[result->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
            plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED);
    bool found_layout = false;
    for (uint32_t i = 0; i < plan->layouts_count; i++)
        if (plan->layouts[i].semantic_type == operation->result_type &&
            plan->layouts[i].kind == XR_TARGET_LAYOUT_DYNAMIC)
            found_layout = true;
    REQUIRE(found_layout);

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
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_source_class_array_push_authority(void) {
    XrSemanticPlan *semantic = build_source_class_array_push_semantic();
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    XrSemanticOperationRecord *operation = NULL;
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < semantic->operation_count; i++) {
        XrSemanticOperationRecord *candidate = &semantic->operations[i];
        const char *selector =
            candidate->metadata_count == 1 && candidate->metadata_begin < semantic->metadata_count
                ? semantic->metadata[candidate->metadata_begin]
                : NULL;
        if (candidate->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_MEMBER_SCALAR && selector &&
            strcmp(selector, "push") == 0) {
            REQUIRE(operation == NULL);
            operation = candidate;
            operation_index = i;
        }
    }
    REQUIRE(operation != NULL && operation->operand_count == 2);
    XrSemanticOperandRecord *receiver = &semantic->operands[operation->operand_begin];
    XrSemanticOperandRecord *element = receiver + 1;
    const XrSemanticTypeRecord *array_type = xr_semantic_plan_type(semantic, receiver->type);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    REQUIRE(array_type && children && array_type->child_begin < child_count);
    const XrSemanticTypeRecord *element_type =
        xr_semantic_plan_type(semantic, children[array_type->child_begin]);
    const XrArrayMemberShape *shape = xr_array_member_shape("push", operation->operand_count);
    REQUIRE(shape && shape->element_access == XR_ARRAY_MEMBER_ELEMENT_ACCESS_STORE &&
            shape->reference_action == XR_ARRAY_MEMBER_REFERENCE_CONSUME_INTO_STORAGE &&
            shape->reference_drop == XR_ARRAY_MEMBER_REFERENCE_DROP_RELEASE_ON_ERASE_OR_DESTROY &&
            xr_semantic_class_instance_type_source_class(semantic, element_type) !=
                XR_SEMANTIC_INDEX_NONE &&
            receiver->ownership_action == XR_SEM_OPERAND_BORROW &&
            element->ownership_action == XR_SEM_OPERAND_CONSUME);

    uint8_t saved_semantic_ownership = element->ownership_action;
    element->ownership_action = XR_SEM_OPERAND_BORROW;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    element->ownership_action = saved_semantic_ownership;
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));

    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "source-class Array.push TargetPlan failed: %s\n", error);
    REQUIRE(built && plan && plan->calls_count == 1 && plan->call_arguments_count == 2);
    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_function_instructions(plan, 0, &instruction_count);
    REQUIRE(instructions && instruction_count == 4 && plan->instructions_count == 4 &&
            instructions[0].opcode == XR_TARGET_INSTRUCTION_PARAM_DYN_BORROW &&
            instructions[1].opcode == XR_TARGET_INSTRUCTION_PARAM_DYN_OWNED &&
            instructions[2].opcode == XR_TARGET_INSTRUCTION_ARRAY_PUSH_TAGGED &&
            instructions[3].opcode == XR_TARGET_INSTRUCTION_RETURN_UNIT &&
            instructions[0].result_slot == plan->call_arguments[0].caller_slot &&
            instructions[1].result_slot == plan->call_arguments[1].caller_slot &&
            instructions[2].result_slot == XR_TARGET_INSTRUCTION_SLOT_NONE &&
            instructions[2].operand_count == 2 &&
            instructions[2].operand_slots[0] == instructions[0].result_slot &&
            instructions[2].operand_slots[1] == instructions[1].result_slot &&
            instructions[2].immediate_bits == 0 &&
            xr_target_plan_function_execution_family_mask(plan, 0) ==
                XR_TARGET_EXECUTION_MANAGED_ARRAY_PUSH_TAGGED);
    const XrTargetInstructionContract *push_contract =
        xr_target_instruction_contract(XR_TARGET_INSTRUCTION_ARRAY_PUSH_TAGGED);
    REQUIRE(push_contract && !push_contract->terminator &&
            push_contract->result_rep == XR_TARGET_INSTRUCTION_REP_NONE &&
            push_contract->operand_rep[0] == XR_TARGET_INSTRUCTION_REP_DYN_VALUE &&
            push_contract->operand_rep[1] == XR_TARGET_INSTRUCTION_REP_DYN_VALUE &&
            push_contract->operand_ownership[0] == XR_TARGET_INSTRUCTION_OPERAND_OWNERSHIP_BORROW &&
            push_contract->operand_ownership[1] ==
                XR_TARGET_INSTRUCTION_OPERAND_OWNERSHIP_CONSUME &&
            (push_contract->effects & XR_TARGET_INSTRUCTION_EFFECT_MEMORY_WRITE) != 0 &&
            (push_contract->effects & XR_TARGET_INSTRUCTION_EFFECT_MAY_ERROR) != 0 &&
            push_contract->error_kind == XR_TARGET_INSTRUCTION_ERROR_ARRAY_PUSH);

    XrTargetInstructionRecord *mutable_instructions = plan->instructions;
    uint16_t saved_opcode = mutable_instructions[0].opcode;
    mutable_instructions[0].opcode = XR_TARGET_INSTRUCTION_PARAM_DYN_OWNED;
    expect_verify_failure(plan, "XR_TARGET_1005");
    mutable_instructions[0].opcode = saved_opcode;
    uint32_t saved_slot = mutable_instructions[2].operand_slots[0];
    mutable_instructions[2].operand_slots[0] = mutable_instructions[2].operand_slots[1];
    expect_verify_failure(plan, "XR_TARGET_1005");
    mutable_instructions[2].operand_slots[0] = saved_slot;
    uint64_t saved_immediate = mutable_instructions[2].immediate_bits;
    mutable_instructions[2].immediate_bits = UINT64_MAX;
    expect_verify_failure(plan, "XR_TARGET_1005");
    mutable_instructions[2].immediate_bits = saved_immediate;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    XrTargetCallRecord *call = &plan->calls[0];
    REQUIRE(call->semantic_operation == operation_index && call->argument_count == 2 &&
            call->calling_convention == XR_TARGET_CALL_CONVENTION_ARRAY_MEMBER_SCALAR &&
            call->target_kind == XR_TARGET_CALL_TARGET_ARRAY_MEMBER_SCALAR &&
            call->array_element_storage == XR_TARGET_ARRAY_STORAGE_TAGGED);
    XrTargetCallArgumentRecord *receiver_argument = &plan->call_arguments[call->argument_begin];
    XrTargetCallArgumentRecord *element_argument = receiver_argument + 1;
    REQUIRE(receiver_argument->semantic_operand == operation->operand_begin &&
            receiver_argument->semantic_value == receiver->value &&
            receiver_argument->ownership == XR_TARGET_CALL_BORROW &&
            receiver_argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
            element_argument->semantic_operand == operation->operand_begin + 1u &&
            element_argument->semantic_value == element->value &&
            element_argument->ownership == XR_TARGET_CALL_CONSUME &&
            element_argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_TAGGED &&
            plan->machine_reps[receiver_argument->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[element_argument->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[element_argument->register_rep].ownership ==
                XR_TARGET_OWNERSHIP_OWNED &&
            element_argument->caller_slot < plan->slots_count &&
            plan->slots[element_argument->caller_slot].ownership == XR_TARGET_OWNERSHIP_OWNED &&
            xr_target_plan_verify(plan, error, sizeof(error)));
    bool tagged_layout = false;
    uint32_t array_layout = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->layouts_count; i++) {
        if (plan->layouts[i].semantic_type != receiver->type)
            continue;
        REQUIRE(!tagged_layout);
        tagged_layout = plan->layouts[i].array_element_storage == XR_TARGET_ARRAY_STORAGE_TAGGED;
        array_layout = i;
    }
    REQUIRE(tagged_layout && array_layout != XR_SEMANTIC_INDEX_NONE);

    uint8_t saved = receiver_argument->ownership;
    receiver_argument->ownership = XR_TARGET_CALL_CONSUME;
    expect_verify_failure(plan, "XR_TARGET_1003");
    receiver_argument->ownership = saved;
    saved = element_argument->ownership;
    element_argument->ownership = XR_TARGET_CALL_READ;
    expect_verify_failure(plan, "XR_TARGET_1003");
    element_argument->ownership = saved;
    saved = element_argument->array_element_storage;
    element_argument->array_element_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    element_argument->array_element_storage = saved;
    uint16_t saved_rep = element_argument->register_rep;
    element_argument->register_rep = call->error_register_rep;
    expect_verify_failure(plan, "XR_TARGET_1003");
    element_argument->register_rep = saved_rep;
    saved = call->array_element_storage;
    call->array_element_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->array_element_storage = saved;
    uint16_t saved_count = call->argument_count;
    call->argument_count = 1;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->argument_count = saved_count;
    uint32_t saved_operand = element_argument->semantic_operand;
    element_argument->semantic_operand = receiver_argument->semantic_operand;
    expect_verify_failure(plan, "XR_TARGET_1003");
    element_argument->semantic_operand = saved_operand;
    XrStableId saved_identity = element_argument->identity;
    element_argument->identity.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1003");
    element_argument->identity = saved_identity;
    saved = plan->layouts[array_layout].array_element_storage;
    plan->layouts[array_layout].array_element_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    xr_target_layout_compute_fingerprint(plan, array_layout,
                                         &plan->layouts[array_layout].fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->layouts[array_layout].array_element_storage = saved;
    xr_target_layout_compute_fingerprint(plan, array_layout,
                                         &plan->layouts[array_layout].fingerprint);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    test_source_class_array_push_managed_execution(plan);

    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    built = xr_c_emission_plan_build(plan, xr_target_plan_semantic_plan(plan), profile_fingerprint,
                                     &emission, error, sizeof(error));
    if (!built)
        fprintf(stderr, "source-class Array.push CEmissionPlan failed: %s\n", error);
    REQUIRE(built);
    XrCValueEmissionView push_view = {0};
    bool projected = xr_c_emission_plan_value_view(emission, operation->result_value, &push_view,
                                                   error, sizeof(error));
    if (!projected)
        fprintf(stderr, "source-class Array.push C emission view failed: %s\n", error);
    REQUIRE(projected);
    bool emission_verified =
        xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                  profile_fingerprint, error, sizeof(error));
    if (!emission_verified)
        fprintf(stderr, "source-class Array.push C emission verify failed: %s\n", error);
    if (push_view.recipe_rule_id != XR_C_EMISSION_RULE_C_EMISSION_ARRAY_PUSH_TAGGED_V1 ||
        push_view.materialization != XR_C_VALUE_MATERIALIZATION_ARRAY_PUSH_TAGGED ||
        push_view.rep != XR_C_VALUE_REP_VOID ||
        push_view.target_register_kind != XR_MACHINE_REP_VOID ||
        push_view.target_memory_kind != XR_MACHINE_REP_VOID ||
        push_view.recipe_operand_value != receiver->value ||
        push_view.recipe_argument_value != element->value ||
        push_view.recipe_discriminant != XR_TARGET_ARRAY_STORAGE_TAGGED ||
        !push_view.recipe_symbol || !push_view.c_type)
        fprintf(stderr,
                "source-class Array.push C emission mismatch: rule=%u recipe=%u rep=%u "
                "reg=%u mem=%u receiver=%u/%u element=%u/%u storage=%u symbol=%s ctype=%s\n",
                (unsigned) push_view.recipe_rule_id, (unsigned) push_view.materialization,
                (unsigned) push_view.rep, (unsigned) push_view.target_register_kind,
                (unsigned) push_view.target_memory_kind, push_view.recipe_operand_value,
                receiver->value, push_view.recipe_argument_value, element->value,
                push_view.recipe_discriminant,
                push_view.recipe_symbol ? push_view.recipe_symbol : "<null>",
                push_view.c_type ? push_view.c_type : "<null>");
    REQUIRE(push_view.recipe_rule_id == XR_C_EMISSION_RULE_C_EMISSION_ARRAY_PUSH_TAGGED_V1 &&
            push_view.materialization == XR_C_VALUE_MATERIALIZATION_ARRAY_PUSH_TAGGED &&
            push_view.rep == XR_C_VALUE_REP_VOID &&
            push_view.target_register_kind == XR_MACHINE_REP_VOID &&
            push_view.target_memory_kind == XR_MACHINE_REP_VOID &&
            push_view.recipe_operand_value == receiver->value &&
            push_view.recipe_argument_value == element->value &&
            push_view.recipe_discriminant == XR_TARGET_ARRAY_STORAGE_TAGGED &&
            push_view.recipe_symbol && strcmp(push_view.recipe_symbol, "xrt_array_push") == 0 &&
            push_view.c_type && strcmp(push_view.c_type, "void") == 0 && emission_verified);

    XrCValueEmissionView *mutable_push = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == operation->result_value)
            mutable_push = &emission->values[i];
    REQUIRE(mutable_push != NULL);
    uint16_t saved_rule = mutable_push->recipe_rule_id;
    mutable_push->recipe_rule_id = XR_C_EMISSION_RULE_NONE;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_push->recipe_rule_id = saved_rule;
    uint8_t saved_materialization = mutable_push->materialization;
    mutable_push->materialization = XR_C_VALUE_MATERIALIZATION_NONE;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_push->materialization = saved_materialization;
    uint32_t saved_u32 = mutable_push->recipe_operand_value;
    mutable_push->recipe_operand_value = mutable_push->recipe_argument_value;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_push->recipe_operand_value = saved_u32;
    saved_u32 = mutable_push->recipe_argument_value;
    mutable_push->recipe_argument_value = UINT32_MAX;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_push->recipe_argument_value = saved_u32;
    uint32_t saved_discriminant = mutable_push->recipe_discriminant;
    mutable_push->recipe_discriminant = XR_TARGET_ARRAY_STORAGE_NONE;
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_push->recipe_discriminant = saved_discriminant;
    const char *saved_symbol = mutable_push->recipe_symbol;
    mutable_push->recipe_symbol = "xrt_array_set";
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_push->recipe_symbol = saved_symbol;
    REQUIRE(xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                      profile_fingerprint, error, sizeof(error)));

    xr_c_emission_plan_free(emission);

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void verify_nested_array_push_emission(const XrTargetPlan *plan,
                                              const XrSemanticOperationRecord *operation,
                                              const XrSemanticOperandRecord *receiver,
                                              const XrSemanticOperandRecord *element,
                                              XrFingerprint profile_fingerprint) {
    char error[512] = {0};
    XrCEmissionPlan *emission = NULL;
    bool built = xr_c_emission_plan_build(plan, xr_target_plan_semantic_plan(plan),
                                          profile_fingerprint, &emission, error, sizeof(error));
    if (!built)
        fprintf(stderr, "nested Array.push CEmissionPlan failed: %s\n", error);
    REQUIRE(built);
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, operation->result_value, &view, error,
                                          sizeof(error)) &&
            view.recipe_rule_id == XR_C_EMISSION_RULE_C_EMISSION_ARRAY_PUSH_TAGGED_V1 &&
            view.materialization == XR_C_VALUE_MATERIALIZATION_ARRAY_PUSH_TAGGED &&
            view.rep == XR_C_VALUE_REP_VOID && view.recipe_operand_value == receiver->value &&
            view.recipe_argument_value == element->value &&
            view.recipe_discriminant == XR_TARGET_ARRAY_STORAGE_TAGGED && view.recipe_symbol &&
            strcmp(view.recipe_symbol, "xrt_array_push") == 0 && view.c_type &&
            strcmp(view.c_type, "void") == 0);
    XrCValueEmissionView *mutable_view = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == operation->result_value)
            mutable_view = &emission->values[i];
    REQUIRE(mutable_view != NULL);
    const char *saved_symbol = mutable_view->recipe_symbol;
    mutable_view->recipe_symbol = "xrt_array_set";
    REQUIRE(!xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                       profile_fingerprint, error, sizeof(error)));
    mutable_view->recipe_symbol = saved_symbol;
    REQUIRE(xr_c_emission_plan_verify(emission, plan, xr_target_plan_semantic_plan(plan),
                                      profile_fingerprint, error, sizeof(error)));
    xr_c_emission_plan_free(emission);
}

static void test_nested_array_push_authority(void) {
    XrSemanticPlan *semantic = build_nested_array_push_semantic();
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    XrSemanticOperationRecord *operation = NULL;
    for (uint32_t i = 0; i < semantic->operation_count; i++) {
        XrSemanticOperationRecord *candidate = &semantic->operations[i];
        const char *selector =
            candidate->metadata_count == 1 && candidate->metadata_begin < semantic->metadata_count
                ? semantic->metadata[candidate->metadata_begin]
                : NULL;
        if (candidate->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_MEMBER_SCALAR && selector &&
            strcmp(selector, "push") == 0) {
            REQUIRE(operation == NULL);
            operation = candidate;
        }
    }
    REQUIRE(operation != NULL && operation->operand_count == 2);
    XrSemanticOperandRecord *receiver = &semantic->operands[operation->operand_begin];
    XrSemanticOperandRecord *element = receiver + 1;
    XrSemanticTypeRecord *receiver_type = &semantic->types[receiver->type];
    REQUIRE(receiver_type->child_count == 1 &&
            receiver_type->child_begin < semantic->type_child_count);
    uint32_t element_type_index = semantic->type_children[receiver_type->child_begin];
    XrSemanticTypeRecord *element_type = &semantic->types[element_type_index];
    const XrArrayMemberShape *shape = xr_array_member_shape("push", operation->operand_count);
    REQUIRE(xr_semantic_array_type_row_is_exact(receiver_type) &&
            xr_semantic_array_type_row_is_exact(element_type) &&
            xr_semantic_array_member_owned_reference_type_is_exact(semantic, element_type) &&
            xr_semantic_array_member_reference_contract_is_exact(
                semantic, shape, operation, element_type_index, element_type) &&
            receiver->ownership_action == XR_SEM_OPERAND_BORROW &&
            element->ownership_action == XR_SEM_OPERAND_CONSUME);
    XrSemanticOperationRecord fake_fill = *operation;
    fake_fill.operand_count = 4;
    fake_fill.semantic_immediate = (int64_t) XI_METHOD_SYMBOL_FILL << 1;
    REQUIRE(!xr_semantic_array_member_reference_contract_is_exact(
        semantic, xr_array_member_shape("fill", 4), &fake_fill, element_type_index, element_type));
    uint16_t saved_kind = element_type->kind;
    element_type->kind = XR_KIND_MAP;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    element_type->kind = saved_kind;
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));

    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "nested Array.push TargetPlan failed: %s\n", error);
    REQUIRE(built && plan && plan->calls_count == 1 && plan->call_arguments_count == 2);
    XrTargetCallRecord *call = &plan->calls[0];
    XrTargetCallArgumentRecord *arguments = &plan->call_arguments[call->argument_begin];
    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_function_instructions(plan, 0, &instruction_count);
    REQUIRE(call->target_kind == XR_TARGET_CALL_TARGET_ARRAY_MEMBER_SCALAR &&
            call->calling_convention == XR_TARGET_CALL_CONVENTION_ARRAY_MEMBER_SCALAR &&
            call->array_element_storage == XR_TARGET_ARRAY_STORAGE_TAGGED &&
            arguments[0].ownership == XR_TARGET_CALL_BORROW &&
            arguments[1].ownership == XR_TARGET_CALL_CONSUME &&
            arguments[1].array_element_storage == XR_TARGET_ARRAY_STORAGE_TAGGED &&
            instruction_count == 4 &&
            instructions[2].opcode == XR_TARGET_INSTRUCTION_ARRAY_PUSH_TAGGED &&
            xr_target_plan_function_execution_family_mask(plan, 0) ==
                XR_TARGET_EXECUTION_MANAGED_ARRAY_PUSH_TAGGED);
    uint8_t saved_storage = call->array_element_storage;
    call->array_element_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->array_element_storage = saved_storage;
    uint8_t saved_ownership = arguments[1].ownership;
    arguments[1].ownership = XR_TARGET_CALL_READ;
    expect_verify_failure(plan, "XR_TARGET_1003");
    arguments[1].ownership = saved_ownership;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    test_nested_array_push_managed_execution(plan);
    verify_nested_array_push_emission(plan, operation, receiver, element,
                                      xr_target_profile_fingerprint(profile));
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_class_field_array_push_authority(void) {
    XrSemanticPlan *semantic = build_class_field_array_push_semantic();
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    XrSemanticOperationRecord *field = NULL;
    XrSemanticOperationRecord *push = NULL;
    uint32_t field_operation = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < semantic->operation_count; i++) {
        XrSemanticOperationRecord *operation = &semantic->operations[i];
        if (operation->opcode == XI_LOAD_FIELD) {
            REQUIRE(field == NULL);
            field = operation;
            field_operation = i;
        }
        const char *selector =
            operation->metadata_count == 1 && operation->metadata_begin < semantic->metadata_count
                ? semantic->metadata[operation->metadata_begin]
                : NULL;
        if (operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_MEMBER_SCALAR && selector &&
            strcmp(selector, "push") == 0) {
            REQUIRE(push == NULL);
            push = operation;
        }
    }
    REQUIRE(field != NULL && field_operation != XR_SEMANTIC_INDEX_NONE && push != NULL &&
            push->operand_count == 2 &&
            xr_semantic_class_field_read_source_class(semantic, field) != XR_SEMANTIC_INDEX_NONE &&
            xr_semantic_tagged_array_field_read_is_exact(semantic, field));
    const XrSemanticOperandRecord *push_receiver = &semantic->operands[push->operand_begin];
    REQUIRE(push_receiver->value == field->result_value &&
            push_receiver->ownership_action == XR_SEM_OPERAND_BORROW);
    uint32_t saved_metadata_count = field->metadata_count;
    field->metadata_count = 0;
    REQUIRE(!xr_semantic_tagged_array_field_read_is_exact(semantic, field));
    field->metadata_count = saved_metadata_count;
    REQUIRE(xr_semantic_tagged_array_field_read_is_exact(semantic, field));

    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "class-field Array.push TargetPlan failed: %s\n", error);
    REQUIRE(built && plan != NULL && plan->calls_count == 1 && plan->call_arguments_count == 2 &&
            xr_target_plan_verify(plan, error, sizeof(error)));
    const XrTargetValueRepRecord *binding = xr_target_plan_value_rep(plan, field->result_value);
    REQUIRE(binding != NULL && binding->slot < plan->slots_count &&
            plan->slots[binding->slot].semantic_operation == field_operation &&
            plan->slots[binding->slot].role == XR_TARGET_SLOT_TEMPORARY &&
            plan->slots[binding->slot].ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            plan->machine_reps[binding->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[binding->register_rep].ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            plan->machine_reps[binding->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[binding->memory_rep].ownership == XR_TARGET_OWNERSHIP_BORROWED);
    XrTargetSlotRecord *slot = &plan->slots[binding->slot];
    uint8_t saved_ownership = slot->ownership;
    slot->ownership = XR_TARGET_OWNERSHIP_OWNED;
    expect_verify_failure(plan, "XR_TARGET_1001");
    slot->ownership = saved_ownership;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_source_class_field_result_authority(void) {
    XrSemanticPlan *semantic = build_source_class_field_result_semantic();
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    XrSemanticOperationRecord *field = NULL;
    uint32_t field_operation = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < semantic->operation_count; i++) {
        XrSemanticOperationRecord *operation = &semantic->operations[i];
        if (operation->opcode != XI_LOAD_FIELD)
            continue;
        REQUIRE(field == NULL);
        field = operation;
        field_operation = i;
    }
    uint8_t carrier = XR_SEM_SOURCE_CLASS_FIELD_RESULT_NONE;
    REQUIRE(field != NULL && field_operation != XR_SEMANTIC_INDEX_NONE &&
            field->evidence[5] == 47 &&
            xr_semantic_source_class_field_read_is_exact(semantic, field, NULL) &&
            xr_semantic_source_class_field_result_carrier_is_exact(semantic, field, &carrier) &&
            carrier == XR_SEM_SOURCE_CLASS_FIELD_RESULT_BORROWED_TAGGED);

    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "source-class field result TargetPlan failed: %s\n", error);
    REQUIRE(built && plan != NULL && plan->calls_count == 1 && plan->call_arguments_count == 1 &&
            (plan->completed_family_mask & XR_TARGET_FAMILY_SOURCE_CLASS_FIELD_RESULT_STORAGE) !=
                0 &&
            xr_target_plan_verify(plan, error, sizeof(error)));
    const XrTargetValueRepRecord *binding = xr_target_plan_value_rep(plan, field->result_value);
    REQUIRE(binding != NULL && binding->slot < plan->slots_count &&
            plan->slots[binding->slot].semantic_operation == field_operation &&
            plan->slots[binding->slot].role == XR_TARGET_SLOT_TEMPORARY &&
            plan->slots[binding->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
            plan->slots[binding->slot].ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            plan->machine_reps[binding->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[binding->register_rep].ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            plan->machine_reps[binding->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[binding->memory_rep].ownership == XR_TARGET_OWNERSHIP_BORROWED);
    XrTargetCallArgumentRecord *argument = &plan->call_arguments[0];
    REQUIRE(argument->semantic_value == field->result_value &&
            argument->caller_slot == binding->slot && argument->mode == XR_TARGET_CALL_VALUE &&
            argument->ownership == XR_TARGET_CALL_READ);

    uint32_t saved_field_id = field->evidence[5];
    field->evidence[5] = 0;
    resign_mutated_semantic_target(semantic, plan);
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)) &&
            strncmp(error, "XR_SEM_0019", strlen("XR_SEM_0019")) == 0);
    expect_verify_failure_raw(plan, "XR_TARGET_1000");
    field->evidence[5] = saved_field_id;
    resign_mutated_semantic_target(semantic, plan);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    uint8_t saved_slot_ownership = plan->slots[binding->slot].ownership;
    plan->slots[binding->slot].ownership = XR_TARGET_OWNERSHIP_OWNED;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[binding->slot].ownership = saved_slot_ownership;
    uint8_t saved_argument_ownership = argument->ownership;
    argument->ownership = XR_TARGET_CALL_CONSUME;
    xr_target_call_compute_fingerprint(plan, 0, &plan->calls[0].fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1003");
    argument->ownership = saved_argument_ownership;
    xr_target_call_compute_fingerprint(plan, 0, &plan->calls[0].fingerprint);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_source_structural_field_result_authority(void) {
    XrSemanticPlan *semantic = build_source_structural_field_result_semantic();
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    XrSemanticOperationRecord *field = NULL;
    uint32_t field_operation = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < semantic->operation_count; i++) {
        XrSemanticOperationRecord *operation = &semantic->operations[i];
        if (operation->opcode != XI_OBJECT_GET_F)
            continue;
        REQUIRE(field == NULL);
        field = operation;
        field_operation = i;
    }
    uint8_t carrier = XR_SEM_SOURCE_STRUCTURAL_FIELD_RESULT_NONE;
    uint32_t receiver_type = XR_SEMANTIC_INDEX_NONE;
    uint32_t field_ordinal = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(
        field != NULL && field_operation != XR_SEMANTIC_INDEX_NONE &&
        xr_semantic_source_structural_field_read_is_exact(semantic, field, &receiver_type,
                                                          &field_ordinal) &&
        xr_semantic_source_structural_shape_is_exact(semantic, receiver_type) &&
        field_ordinal == 0 &&
        xr_semantic_source_structural_field_result_carrier_is_exact(semantic, field, &carrier) &&
        carrier == XR_SEM_SOURCE_STRUCTURAL_FIELD_RESULT_BORROWED_TAGGED);

    int64_t saved_immediate = field->semantic_immediate;
    field->semantic_immediate = 1;
    REQUIRE(!xr_semantic_source_structural_field_read_is_exact(semantic, field, NULL, NULL));
    field->semantic_immediate = saved_immediate;
    uint32_t saved_result_type = field->result_type;
    field->result_type = receiver_type;
    REQUIRE(!xr_semantic_source_structural_field_read_is_exact(semantic, field, NULL, NULL));
    field->result_type = saved_result_type;
    uint8_t saved_result_ownership = field->result_ownership;
    field->result_ownership = XI_GEN_RESULT_OWNERSHIP_OWNED;
    REQUIRE(!xr_semantic_source_structural_field_read_is_exact(semantic, field, NULL, NULL));
    field->result_ownership = saved_result_ownership;

    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "source structural field result TargetPlan failed: %s\n", error);
    REQUIRE(built && plan != NULL && plan->calls_count == 1 && plan->call_arguments_count == 1 &&
            (plan->completed_family_mask &
             XR_TARGET_FAMILY_SOURCE_STRUCTURAL_FIELD_RESULT_STORAGE) != 0 &&
            xr_target_plan_verify(plan, error, sizeof(error)));
    const XrTargetValueRepRecord *binding = xr_target_plan_value_rep(plan, field->result_value);
    REQUIRE(binding != NULL && binding->slot < plan->slots_count &&
            plan->slots[binding->slot].semantic_operation == field_operation &&
            plan->slots[binding->slot].role == XR_TARGET_SLOT_TEMPORARY &&
            plan->slots[binding->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
            plan->slots[binding->slot].ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            plan->machine_reps[binding->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[binding->register_rep].ownership == XR_TARGET_OWNERSHIP_BORROWED &&
            plan->machine_reps[binding->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[binding->memory_rep].ownership == XR_TARGET_OWNERSHIP_BORROWED);
    XrTargetCallArgumentRecord *argument = &plan->call_arguments[0];
    REQUIRE(argument->semantic_value == field->result_value &&
            argument->caller_slot == binding->slot && argument->mode == XR_TARGET_CALL_VALUE &&
            argument->ownership == XR_TARGET_CALL_READ);

    uint8_t saved_slot_ownership = plan->slots[binding->slot].ownership;
    plan->slots[binding->slot].ownership = XR_TARGET_OWNERSHIP_OWNED;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[binding->slot].ownership = saved_slot_ownership;
    uint8_t saved_argument_ownership = argument->ownership;
    argument->ownership = XR_TARGET_CALL_CONSUME;
    xr_target_call_compute_fingerprint(plan, 0, &plan->calls[0].fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1003");
    argument->ownership = saved_argument_ownership;
    xr_target_call_compute_fingerprint(plan, 0, &plan->calls[0].fingerprint);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_source_class_array_fill_authority(void) {
    XrSemanticPlan *semantic = build_source_class_array_fill_semantic();
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    XrSemanticOperationRecord *operation = NULL;
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < semantic->operation_count; i++) {
        XrSemanticOperationRecord *candidate = &semantic->operations[i];
        const char *selector =
            candidate->metadata_count == 1 && candidate->metadata_begin < semantic->metadata_count
                ? semantic->metadata[candidate->metadata_begin]
                : NULL;
        if (candidate->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_MEMBER_SCALAR && selector &&
            strcmp(selector, "fill") == 0) {
            REQUIRE(operation == NULL);
            operation = candidate;
            operation_index = i;
        }
    }
    REQUIRE(operation != NULL && operation->operand_count == 4 &&
            operation->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_FILL << 1 &&
            operation->result_alias_operand == 0);
    XrSemanticOperandRecord *receiver = &semantic->operands[operation->operand_begin];
    XrSemanticOperandRecord *element = receiver + 1;
    XrSemanticOperandRecord *start = receiver + 2;
    XrSemanticOperandRecord *end = receiver + 3;
    const XrSemanticTypeRecord *array_type = xr_semantic_plan_type(semantic, receiver->type);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    REQUIRE(array_type && children && array_type->child_begin < child_count);
    const XrSemanticTypeRecord *element_type =
        xr_semantic_plan_type(semantic, children[array_type->child_begin]);
    const XrArrayMemberShape *shape = xr_array_member_shape("fill", operation->operand_count);
    REQUIRE(
        shape && shape->element_access == XR_ARRAY_MEMBER_ELEMENT_ACCESS_STORE &&
        shape->reference_action == XR_ARRAY_MEMBER_REFERENCE_CONSUME_INTO_STORAGE &&
        shape->reference_drop == XR_ARRAY_MEMBER_REFERENCE_DROP_RELEASE_ON_ERASE_OR_DESTROY &&
        xr_semantic_class_instance_type_source_class(semantic, element_type) !=
            XR_SEMANTIC_INDEX_NONE &&
        receiver->ownership_action == XR_SEM_OPERAND_BORROW &&
        element->ownership_action == XR_SEM_OPERAND_CONSUME &&
        xr_semantic_array_member_i64_type_is_exact(xr_semantic_plan_type(semantic, start->type)) &&
        xr_semantic_array_member_i64_type_is_exact(xr_semantic_plan_type(semantic, end->type)));

    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "source-class Array.fill TargetPlan failed: %s\n", error);
    REQUIRE(built && plan && plan->calls_count == 1 && plan->call_arguments_count == 4);
    XrTargetCallRecord *call = &plan->calls[0];
    REQUIRE(call->semantic_operation == operation_index && call->argument_count == 4 &&
            call->calling_convention == XR_TARGET_CALL_CONVENTION_ARRAY_MEMBER_SCALAR &&
            call->target_kind == XR_TARGET_CALL_TARGET_ARRAY_MEMBER_SCALAR &&
            call->array_element_storage == XR_TARGET_ARRAY_STORAGE_TAGGED);
    XrTargetCallArgumentRecord *arguments = &plan->call_arguments[call->argument_begin];
    for (uint16_t ordinal = 0; ordinal < 4; ordinal++) {
        const XrSemanticOperandRecord *operand = receiver + ordinal;
        const XrTargetCallArgumentRecord *argument = arguments + ordinal;
        REQUIRE(argument->semantic_operand == operation->operand_begin + ordinal &&
                argument->semantic_value == operand->value && argument->ordinal == ordinal &&
                argument->ownership ==
                    (ordinal == 0 ? XR_TARGET_CALL_BORROW : XR_TARGET_CALL_CONSUME) &&
                argument->array_element_storage == (ordinal == 1 ? XR_TARGET_ARRAY_STORAGE_TAGGED
                                                                 : XR_TARGET_ARRAY_STORAGE_NONE) &&
                plan->machine_reps[argument->register_rep].kind ==
                    (ordinal < 2 ? XR_MACHINE_REP_DYN_VALUE : XR_MACHINE_REP_I64));
    }
    REQUIRE(
        plan->machine_reps[arguments[1].register_rep].ownership == XR_TARGET_OWNERSHIP_OWNED &&
        plan->machine_reps[arguments[2].register_rep].ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
        plan->machine_reps[arguments[3].register_rep].ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
        xr_target_plan_verify(plan, error, sizeof(error)));

    uint8_t saved_storage = call->array_element_storage;
    call->array_element_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    xr_target_call_compute_fingerprint(plan, 0, &call->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->array_element_storage = saved_storage;
    xr_target_call_compute_fingerprint(plan, 0, &call->fingerprint);

    saved_storage = arguments[1].array_element_storage;
    arguments[1].array_element_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    xr_target_call_compute_fingerprint(plan, 0, &call->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1003");
    arguments[1].array_element_storage = saved_storage;
    xr_target_call_compute_fingerprint(plan, 0, &call->fingerprint);

    uint8_t saved_ownership = arguments[1].ownership;
    arguments[1].ownership = XR_TARGET_CALL_BORROW;
    xr_target_call_compute_fingerprint(plan, 0, &call->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1003");
    arguments[1].ownership = saved_ownership;
    xr_target_call_compute_fingerprint(plan, 0, &call->fingerprint);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    const char *saved_selector = semantic->metadata[operation->metadata_begin];
    semantic->metadata[operation->metadata_begin] = "push";
    resign_mutated_semantic_target(semantic, plan);
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    REQUIRE(!xr_target_plan_verify(plan, error, sizeof(error)));
    semantic->metadata[operation->metadata_begin] = saved_selector;
    resign_mutated_semantic_target(semantic, plan);

    uint8_t saved_intrinsic = operation->intrinsic_kind;
    operation->intrinsic_kind = XR_SEM_INTRINSIC_NONE;
    resign_mutated_semantic_target(semantic, plan);
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    REQUIRE(!xr_target_plan_verify(plan, error, sizeof(error)));
    operation->intrinsic_kind = saved_intrinsic;
    resign_mutated_semantic_target(semantic, plan);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
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
 * plan carrying the row can ever verify. Each family therefore proves that
 * its exact owned or borrowed result contract is load-bearing. */
static XrTargetCallRecord *expect_dynamic_stringbuilder_call(XrTargetPlan *plan, uint8_t convention,
                                                             uint8_t target_kind,
                                                             uint8_t call_ownership,
                                                             uint8_t slot_ownership) {
    char error[512] = {0};
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
    XrTargetCallRecord *call = find_call_by_convention(plan, convention);
    const XrTargetValueRepRecord *result = xr_target_plan_value_rep(plan, call->result_value);
    REQUIRE(result && result->slot == call->result_slot &&
            call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
            call->callee_function == XR_SEMANTIC_INDEX_NONE && call->argument_count == 0 &&
            call->flags == 0 && call->result_mode == XR_TARGET_CALL_VALUE &&
            call->result_ownership == call_ownership && call->target_kind == target_kind &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[result->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
            plan->slots[result->slot].ownership == slot_ownership);

    uint8_t saved_ownership = call->result_ownership;
    call->result_ownership = saved_ownership == XR_TARGET_CALL_BORROW ? XR_TARGET_CALL_RETURN_OWNED
                                                                      : XR_TARGET_CALL_BORROW;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->result_ownership = XR_TARGET_CALL_NONE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->result_ownership = saved_ownership;
    uint8_t saved_slot_ownership = plan->slots[result->slot].ownership;
    plan->slots[result->slot].ownership = saved_slot_ownership == XR_TARGET_OWNERSHIP_BORROWED
                                              ? XR_TARGET_OWNERSHIP_OWNED
                                              : XR_TARGET_OWNERSHIP_BORROWED;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[result->slot].ownership = saved_slot_ownership;
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
    expect_dynamic_stringbuilder_call(plan, XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_RUNE,
                                      XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_RUNE,
                                      XR_TARGET_CALL_RETURN_OWNED, XR_TARGET_OWNERSHIP_OWNED);
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
    expect_dynamic_stringbuilder_call(plan, XR_TARGET_CALL_CONVENTION_STRINGBUILDER_TO_STRING,
                                      XR_TARGET_CALL_TARGET_STRINGBUILDER_TO_STRING,
                                      XR_TARGET_CALL_RETURN_OWNED, XR_TARGET_OWNERSHIP_OWNED);
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
    expect_dynamic_stringbuilder_call(plan, XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_STRING,
                                      XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_STRING,
                                      XR_TARGET_CALL_RETURN_OWNED, XR_TARGET_OWNERSHIP_OWNED);
    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_stringbuilder_clear_call_authority(void) {
    XrSemanticPlan *semantic = build_stringbuilder_clear_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "StringBuilder.clear TargetPlan failed: %s\n", error);
    REQUIRE(built && plan &&
            (plan->completed_family_mask & XR_TARGET_FAMILY_STRINGBUILDER_CLEAR_STORAGE) != 0);
    expect_dynamic_stringbuilder_call(plan, XR_TARGET_CALL_CONVENTION_STRINGBUILDER_CLEAR,
                                      XR_TARGET_CALL_TARGET_STRINGBUILDER_CLEAR,
                                      XR_TARGET_CALL_RETURN_OWNED, XR_TARGET_OWNERSHIP_OWNED);

    XrSemanticOperationRecord *operation = NULL;
    for (uint32_t i = 0; i < semantic->operation_count; i++) {
        XrSemanticOperationRecord *candidate = &semantic->operations[i];
        if (candidate->opcode == XI_CALL_METHOD && candidate->metadata_count == 1 &&
            candidate->metadata_begin < semantic->metadata_count &&
            strcmp(semantic->metadata[candidate->metadata_begin], "clear") == 0) {
            REQUIRE(operation == NULL);
            operation = candidate;
        }
    }
    REQUIRE(operation != NULL);
    int32_t saved_alias = operation->result_alias_operand;
    operation->result_alias_operand = -1;
    resign_mutated_semantic_target(semantic, plan);
    expect_verify_failure(plan, "XR_TARGET_1000");
    operation->result_alias_operand = saved_alias;
    resign_mutated_semantic_target(semantic, plan);
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_direct_local_stringbuilder_ref_append_authority(void) {
    XrSemanticPlan *semantic =
        build_direct_local_tagged_ref_semantic(&stub_string_builder, false, true, false);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    const XrSemanticOperationRecord *append = NULL;
    for (uint32_t i = 0; i < semantic->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &semantic->operations[i];
        if (operation->intrinsic_kind != XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_RUNE)
            continue;
        REQUIRE(append == NULL);
        append = operation;
    }
    REQUIRE(append != NULL && append->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
            append->result_alias_operand == 0);
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "direct-local ref StringBuilder.append(rune) TargetPlan failed: %s\n",
                error);
    REQUIRE(built && plan && plan->call_arguments_count == 1 &&
            (plan->completed_family_mask &
             XR_TARGET_FAMILY_DIRECT_LOCAL_TAGGED_REF_ARGUMENT_STORAGE) != 0);
    const XrTargetCallArgumentRecord *argument = &plan->call_arguments[0];
    REQUIRE(argument->mode == XR_TARGET_CALL_REFERENCE &&
            argument->ownership == XR_TARGET_CALL_BORROW &&
            argument->transfer_mode == XR_TRANSFER_SHARE &&
            argument->flags == XR_TARGET_CALL_ARGUMENT_ADDRESSABLE &&
            argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
            plan->machine_reps[argument->callee_register_rep].kind == XR_MACHINE_REP_RAW_PTR &&
            plan->machine_reps[argument->callee_memory_rep].kind == XR_MACHINE_REP_RAW_PTR);
    expect_dynamic_stringbuilder_call(plan, XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_RUNE,
                                      XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_RUNE,
                                      XR_TARGET_CALL_BORROW, XR_TARGET_OWNERSHIP_BORROWED);
    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_string_runes_call_authority(void) {
    XrSemanticPlan *semantic = build_string_runes_leading_type_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "String.runes TargetPlan failed: %s\n", error);
    REQUIRE(built && plan &&
            (plan->completed_family_mask & XR_TARGET_FAMILY_STRING_RUNES_RESULT_STORAGE) != 0);
    XrTargetCallRecord *call =
        find_call_by_convention(plan, XR_TARGET_CALL_CONVENTION_STRING_RUNES);
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(semantic, call->semantic_operation);
    XrTargetValueRepRecord *result =
        operation
            ? (XrTargetValueRepRecord *) xr_target_plan_value_rep(plan, operation->result_value)
            : NULL;
    XrTargetSlotRecord *slot =
        result && result->slot < plan->slots_count ? &plan->slots[result->slot] : NULL;
    XrTargetLayoutRecord *layout = NULL;
    uint32_t layout_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; operation && i < plan->layouts_count; i++) {
        if (plan->layouts[i].semantic_type != operation->result_type)
            continue;
        REQUIRE(layout == NULL);
        layout = &plan->layouts[i];
        layout_index = i;
    }
    REQUIRE(operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_STRING_RUNES &&
            operation->result_type == 0 && call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
            call->callee_function == XR_SEMANTIC_INDEX_NONE &&
            call->result_value == operation->result_value && call->argument_count == 0 &&
            call->flags == 0 && call->result_mode == XR_TARGET_CALL_VALUE &&
            call->result_ownership == XR_TARGET_CALL_RETURN_OWNED &&
            call->target_kind == XR_TARGET_CALL_TARGET_STRING_RUNES && result && slot && layout &&
            result->slot == call->result_slot &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[result->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[result->register_rep].root_kind == XR_TARGET_ROOT_DYNAMIC &&
            plan->machine_reps[result->register_rep].ownership == XR_TARGET_OWNERSHIP_OWNED &&
            slot->semantic_value == operation->result_value &&
            slot->semantic_operation == call->semantic_operation &&
            slot->role == XR_TARGET_SLOT_TEMPORARY && slot->root_kind == XR_TARGET_ROOT_DYNAMIC &&
            slot->ownership == XR_TARGET_OWNERSHIP_OWNED &&
            layout->kind == XR_TARGET_LAYOUT_DYNAMIC && layout->extent == 0 &&
            layout->field_count == 0 && layout->root_field_count == 0);

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
    uint8_t saved_slot_ownership = slot->ownership;
    slot->ownership = XR_TARGET_OWNERSHIP_BORROWED;
    expect_verify_failure(plan, "XR_TARGET_1001");
    slot->ownership = saved_slot_ownership;
    XrStableId saved_slot_identity = slot->identity;
    slot->identity.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1001");
    slot->identity = saved_slot_identity;
    uint16_t saved_layout_kind = layout->kind;
    layout->kind = XR_TARGET_LAYOUT_SCALAR;
    xr_target_layout_compute_fingerprint(plan, layout_index, &layout->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1001");
    layout->kind = saved_layout_kind;
    xr_target_layout_compute_fingerprint(plan, layout_index, &layout->fingerprint);
    uint32_t saved_fixed_prefix_size = layout->fixed_prefix_size;
    layout->fixed_prefix_size += layout->align;
    xr_target_layout_compute_fingerprint(plan, layout_index, &layout->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1002");
    layout->fixed_prefix_size = saved_fixed_prefix_size;
    xr_target_layout_compute_fingerprint(plan, layout_index, &layout->fingerprint);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t wrong_layout_type = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_type_count(semantic); i++) {
        const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, i);
        if (type && type->kind == XR_KIND_RUNE)
            wrong_layout_type = i;
    }
    REQUIRE(operands && operation->operand_begin < operand_count &&
            operands[operation->operand_begin].type != operation->result_type &&
            wrong_layout_type != XR_SEMANTIC_INDEX_NONE &&
            wrong_layout_type != operation->result_type);
    uint32_t saved_layout_type = layout->semantic_type;
    layout->semantic_type = wrong_layout_type;
    xr_target_layout_compute_fingerprint(plan, layout_index, &layout->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1001");
    layout->semantic_type = saved_layout_type;
    xr_target_layout_compute_fingerprint(plan, layout_index, &layout->fingerprint);
    XrTargetMachineRepRecord *register_rep = &plan->machine_reps[result->register_rep];
    XrTargetMachineRepRecord *memory_rep = &plan->machine_reps[result->memory_rep];
    uint8_t saved_register_root = register_rep->root_kind;
    uint8_t saved_memory_root = memory_rep->root_kind;
    uint8_t saved_slot_root = slot->root_kind;
    register_rep->root_kind = XR_TARGET_ROOT_NONE;
    memory_rep->root_kind = XR_TARGET_ROOT_NONE;
    slot->root_kind = XR_TARGET_ROOT_NONE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    register_rep->root_kind = saved_register_root;
    memory_rep->root_kind = saved_memory_root;
    slot->root_kind = saved_slot_root;
    uint8_t saved_register_ownership = register_rep->ownership;
    uint8_t saved_memory_ownership = memory_rep->ownership;
    register_rep->ownership = XR_TARGET_OWNERSHIP_BORROWED;
    memory_rep->ownership = XR_TARGET_OWNERSHIP_BORROWED;
    slot->ownership = XR_TARGET_OWNERSHIP_BORROWED;
    expect_verify_failure(plan, "XR_TARGET_1001");
    register_rep->ownership = saved_register_ownership;
    memory_rep->ownership = saved_memory_ownership;
    slot->ownership = saved_slot_ownership;
    if (plan->layouts_count > 1) {
        uint32_t duplicate_index = layout_index == 0 ? 1u : 0u;
        XrTargetLayoutRecord *duplicate = &plan->layouts[duplicate_index];
        uint32_t saved_duplicate_type = duplicate->semantic_type;
        duplicate->semantic_type = operation->result_type;
        xr_target_layout_compute_fingerprint(plan, duplicate_index, &duplicate->fingerprint);
        expect_verify_failure(plan, "XR_TARGET_1001");
        duplicate->semantic_type = saved_duplicate_type;
        xr_target_layout_compute_fingerprint(plan, duplicate_index, &duplicate->fingerprint);
    }
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_string_slice_range_call_authority(void) {
    XrSemanticPlan *semantic = build_string_slice_range_semantic(false);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan);
    REQUIRE((plan->completed_family_mask & XR_TARGET_FAMILY_STRING_SLICE_RANGE_RESULT_STORAGE) !=
            0);
    XrTargetCallRecord *call =
        find_call_by_convention(plan, XR_TARGET_CALL_CONVENTION_STRING_SLICE_RANGE);
    const XrSemanticOperationRecord *operation =
        call ? xr_semantic_plan_operation(semantic, call->semantic_operation) : NULL;
    const XrTargetValueRepRecord *result =
        operation ? xr_target_plan_value_rep(plan, operation->result_value) : NULL;
    REQUIRE(operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_STRING_SLICE_RANGE &&
            call->target_kind == XR_TARGET_CALL_TARGET_STRING_SLICE_RANGE &&
            call->flags == XR_TARGET_CALL_TAIL &&
            call->result_ownership == XR_TARGET_CALL_RETURN_OWNED &&
            call->result_mode == XR_TARGET_CALL_VALUE && call->argument_count == 0 &&
            plan->call_arguments_count == 0 && result &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->machine_reps[result->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
            plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED);

    XrStableId saved_identity = call->identity;
    call->identity.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->identity = saved_identity;
    uint8_t saved_kind = call->target_kind;
    call->target_kind = XR_TARGET_CALL_TARGET_STRING_RUNES;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->target_kind = saved_kind;
    uint8_t saved_convention = call->calling_convention;
    call->calling_convention = XR_TARGET_CALL_CONVENTION_STRING_RUNES;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->calling_convention = saved_convention;
    uint16_t saved_flags = call->flags;
    call->flags = 0;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->flags = saved_flags;
    uint8_t saved_ownership = call->result_ownership;
    call->result_ownership = XR_TARGET_CALL_BORROW;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->result_ownership = saved_ownership;
    uint8_t saved_root = plan->slots[result->slot].root_kind;
    plan->slots[result->slot].root_kind = XR_TARGET_ROOT_NONE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[result->slot].root_kind = saved_root;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    XrSemanticOperationRecord *slice_operation = &semantic->operations[call->semantic_operation];
    uint8_t saved_intrinsic = slice_operation->intrinsic_kind;
    slice_operation->intrinsic_kind = XR_SEM_INTRINSIC_NONE;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    slice_operation->intrinsic_kind = saved_intrinsic;
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_string_slice_optional_parameter_authority(void) {
    XrSemanticPlan *semantic = build_string_slice_range_semantic(true);
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)) &&
            semantic->parameter_count == 1 && semantic->parameters[0].flags == 0);
    XrSemanticOperationRecord *slice = NULL;
    for (uint32_t i = 0; i < semantic->operation_count; i++)
        if (semantic->operations[i].intrinsic_kind == XR_SEM_INTRINSIC_STRING_SLICE_RANGE) {
            REQUIRE(slice == NULL);
            slice = &semantic->operations[i];
        }
    REQUIRE(slice && xr_semantic_string_slice_range_is_exact(semantic, slice, NULL, NULL, NULL));

    uint8_t saved_flags = semantic->parameters[0].flags;
    semantic->parameters[0].flags = XR_SEM_PARAMETER_VARIADIC;
    REQUIRE(!xr_semantic_string_slice_range_is_exact(semantic, slice, NULL, NULL, NULL));
    semantic->parameters[0].flags = saved_flags;

    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan &&
            xr_target_plan_verify(plan, error, sizeof(error)) &&
            find_call_by_convention(plan, XR_TARGET_CALL_CONVENTION_STRING_SLICE_RANGE) != NULL);
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_iterator_rune_has_next_call_authority(void) {
    XrSemanticPlan *semantic = build_iterator_rune_has_next_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan);
    XrTargetCallRecord *call =
        find_call_by_convention(plan, XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_HAS_NEXT);
    const XrSemanticOperationRecord *operation =
        call ? xr_semantic_plan_operation(semantic, call->semantic_operation) : NULL;
    const XrTargetValueRepRecord *result =
        operation ? xr_target_plan_value_rep(plan, operation->result_value) : NULL;
    REQUIRE(operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_ITERATOR_RUNE_HAS_NEXT &&
            call->target_kind == XR_TARGET_CALL_TARGET_ITERATOR_RUNE_HAS_NEXT &&
            call->result_ownership == XR_TARGET_CALL_NONE &&
            call->result_mode == XR_TARGET_CALL_VALUE && call->argument_count == 0 && result &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_I1 &&
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
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan);
    XrTargetCallRecord *call =
        find_call_by_convention(plan, XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_NEXT);
    const XrSemanticOperationRecord *operation =
        call ? xr_semantic_plan_operation(semantic, call->semantic_operation) : NULL;
    const XrTargetValueRepRecord *result =
        operation ? xr_target_plan_value_rep(plan, operation->result_value) : NULL;
    REQUIRE(operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_ITERATOR_RUNE_NEXT &&
            call->target_kind == XR_TARGET_CALL_TARGET_ITERATOR_RUNE_NEXT &&
            call->result_ownership == XR_TARGET_CALL_NONE &&
            call->result_mode == XR_TARGET_CALL_VALUE && call->argument_count == 0 && result &&
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

static void test_iterator_rune_nth_call_argument_authority(void) {
    XrSemanticPlan *semantic = build_iterator_rune_nth_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan);
    XrTargetCallRecord *call =
        find_call_by_convention(plan, XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_NTH);
    XrSemanticOperationRecord *operation =
        call ? &semantic->operations[call->semantic_operation] : NULL;
    uint32_t semantic_operand = operation ? operation->operand_begin + 1u : 0;
    XrSemanticOperandRecord *operand =
        semantic_operand < semantic->operand_count ? &semantic->operands[semantic_operand] : NULL;
    const XrTargetValueRepRecord *result =
        operation ? xr_target_plan_value_rep(plan, operation->result_value) : NULL;
    const XrTargetValueRepRecord *caller =
        operand ? xr_target_plan_value_rep(plan, operand->value) : NULL;
    XrTargetCallArgumentRecord *argument =
        call && call->argument_count == 1 ? &plan->call_arguments[call->argument_begin] : NULL;
    XrStableId zero = {{0}};
    REQUIRE(operation && operand && caller && result && argument &&
            operation->intrinsic_kind == XR_SEM_INTRINSIC_ITERATOR_RUNE_NTH &&
            operation->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_NTH << 1 && call->id != 0 &&
            call->target_kind == XR_TARGET_CALL_TARGET_ITERATOR_RUNE_NTH &&
            call->result_ownership == XR_TARGET_CALL_NONE && call->argument_count == 1 &&
            plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_RUNE &&
            plan->slots[result->slot].root_kind == XR_TARGET_ROOT_NONE &&
            argument->call == call->id && argument->semantic_operand == semantic_operand &&
            argument->semantic_value == operand->value &&
            argument->callee_parameter == XR_SEMANTIC_INDEX_NONE &&
            argument->caller_slot == caller->slot &&
            argument->callee_slot == XR_SEMANTIC_INDEX_NONE &&
            argument->register_rep == caller->register_rep &&
            argument->memory_rep == caller->memory_rep &&
            argument->callee_register_rep == caller->register_rep &&
            argument->callee_memory_rep == caller->memory_rep &&
            plan->machine_reps[argument->register_rep].kind == XR_MACHINE_REP_I64 &&
            argument->ordinal == 0 && argument->mode == XR_TARGET_CALL_VALUE &&
            argument->ownership == XR_TARGET_CALL_CONSUME &&
            argument->transfer_mode == XR_TRANSFER_SHARE && argument->flags == 0 &&
            argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
            !xr_stable_id_equal(argument->identity, zero));

    XrStableId saved_identity = argument->identity;
    argument->identity = zero;
    expect_verify_failure(plan, "XR_TARGET_1003");
    argument->identity = saved_identity;
    argument->identity.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1003");
    argument->identity = saved_identity;
    uint32_t saved_u32 = argument->call;
    argument->call = 0;
    expect_verify_failure(plan, "XR_TARGET_1003");
    argument->call = saved_u32;
    saved_u32 = argument->semantic_operand;
    argument->semantic_operand = 0;
    expect_verify_failure(plan, "XR_TARGET_1003");
    argument->semantic_operand = saved_u32;
    saved_u32 = argument->semantic_value;
    argument->semantic_value = 0;
    expect_verify_failure(plan, "XR_TARGET_1003");
    argument->semantic_value = saved_u32;
    saved_u32 = argument->callee_parameter;
    argument->callee_parameter = 0;
    expect_verify_failure(plan, "XR_TARGET_1003");
    argument->callee_parameter = saved_u32;
    saved_u32 = argument->callee_slot;
    argument->callee_slot = 0;
    expect_verify_failure(plan, "XR_TARGET_1003");
    argument->callee_slot = saved_u32;
    uint16_t saved_u16 = argument->callee_register_rep;
    argument->callee_register_rep = result->register_rep;
    expect_verify_failure(plan, "XR_TARGET_1003");
    argument->callee_register_rep = saved_u16;
    saved_u16 = argument->ordinal;
    argument->ordinal = 1;
    expect_verify_failure(plan, "XR_TARGET_1003");
    argument->ordinal = saved_u16;
    uint8_t saved_u8 = argument->mode;
    argument->mode = XR_TARGET_CALL_REFERENCE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    argument->mode = saved_u8;
    saved_u8 = argument->ownership;
    argument->ownership = XR_TARGET_CALL_NONE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    argument->ownership = saved_u8;
    saved_u8 = argument->transfer_mode;
    argument->transfer_mode = XR_TRANSFER_MOVE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    argument->transfer_mode = saved_u8;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    int64_t saved_immediate = operation->semantic_immediate;
    operation->semantic_immediate = (int64_t) XI_METHOD_SYMBOL_NEXT << 1;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    operation->semantic_immediate = saved_immediate;
    uint8_t saved_action = operand->ownership_action;
    operand->ownership_action = XR_SEM_OPERAND_BORROW;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    operand->ownership_action = saved_action;
    uint32_t saved_type = operand->type;
    operand->type = operation->result_type;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    operand->type = saved_type;
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_rune_to_string_result_authority(void) {
    XrSemanticPlan *semantic = build_rune_to_string_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "rune.toString TargetPlan failed: %s\n", error);
    REQUIRE(built && plan);
    XrTargetCallRecord *call =
        find_call_by_convention(plan, XR_TARGET_CALL_CONVENTION_RUNE_TO_STRING);
    XrSemanticOperationRecord *operation =
        call ? &semantic->operations[call->semantic_operation] : NULL;
    XrSemanticOperandRecord *receiver =
        operation && operation->operand_begin < semantic->operand_count
            ? &semantic->operands[operation->operand_begin]
            : NULL;
    XrTargetValueRepRecord *result =
        operation
            ? (XrTargetValueRepRecord *) xr_target_plan_value_rep(plan, operation->result_value)
            : NULL;
    XrTargetSlotRecord *slot =
        result && result->slot < plan->slots_count ? &plan->slots[result->slot] : NULL;
    XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; operation && i < plan->layouts_count; i++) {
        if (plan->layouts[i].semantic_type != operation->result_type)
            continue;
        REQUIRE(layout == NULL);
        layout = &plan->layouts[i];
    }
    REQUIRE(
        (plan->completed_family_mask & XR_TARGET_FAMILY_RUNE_TO_STRING_RESULT_STORAGE) != 0 &&
        operation && receiver && result && slot && layout &&
        operation->intrinsic_kind == XR_SEM_INTRINSIC_RUNE_TO_STRING &&
        operation->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_TOSTRING << 1 &&
        operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
        operation->return_provenance == XR_SEM_RETURN_OWNED && operation->return_complete == 1 &&
        receiver->ownership_action == XR_SEM_OPERAND_BORROW &&
        call->target_kind == XR_TARGET_CALL_TARGET_RUNE_TO_STRING &&
        call->result_ownership == XR_TARGET_CALL_RETURN_OWNED && call->argument_count == 0 &&
        plan->machine_reps[result->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
        plan->machine_reps[result->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
        plan->machine_reps[result->register_rep].root_kind == XR_TARGET_ROOT_DYNAMIC &&
        plan->machine_reps[result->register_rep].ownership == XR_TARGET_OWNERSHIP_OWNED &&
        slot->semantic_value == operation->result_value &&
        slot->semantic_operation == call->semantic_operation &&
        slot->role == XR_TARGET_SLOT_TEMPORARY && slot->root_kind == XR_TARGET_ROOT_DYNAMIC &&
        slot->ownership == XR_TARGET_OWNERSHIP_OWNED && layout->kind == XR_TARGET_LAYOUT_DYNAMIC &&
        layout->field_count == 0 && layout->root_field_count == 0);

    uint64_t saved_mask = plan->completed_family_mask;
    plan->completed_family_mask &= ~XR_TARGET_FAMILY_RUNE_TO_STRING_RESULT_STORAGE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->completed_family_mask = saved_mask;
    XrStableId saved_identity = call->identity;
    call->identity.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->identity = saved_identity;
    uint8_t saved_u8 = call->result_ownership;
    call->result_ownership = XR_TARGET_CALL_NONE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->result_ownership = saved_u8;
    saved_u8 = call->target_kind;
    call->target_kind = XR_TARGET_CALL_TARGET_RUNE_TO_UINT32;
    expect_verify_failure(plan, "XR_TARGET_1003");
    call->target_kind = saved_u8;
    saved_u8 = slot->root_kind;
    slot->root_kind = XR_TARGET_ROOT_NONE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    slot->root_kind = saved_u8;
    saved_u8 = slot->ownership;
    slot->ownership = XR_TARGET_OWNERSHIP_BORROWED;
    expect_verify_failure(plan, "XR_TARGET_1001");
    slot->ownership = saved_u8;
    uint32_t saved_u32 = slot->semantic_operation;
    slot->semantic_operation = XR_SEMANTIC_INDEX_NONE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    slot->semantic_operation = saved_u32;
    uint16_t saved_u16 = layout->kind;
    layout->kind = XR_TARGET_LAYOUT_SCALAR;
    expect_verify_failure(plan, "XR_TARGET_1001");
    layout->kind = saved_u16;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    int64_t saved_immediate = operation->semantic_immediate;
    operation->semantic_immediate = (int64_t) XI_METHOD_SYMBOL_TO_UINT32 << 1;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    operation->semantic_immediate = saved_immediate;
    saved_u8 = operation->result_ownership;
    operation->result_ownership = XI_GEN_RESULT_OWNERSHIP_CALL_RESULT;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    operation->result_ownership = saved_u8;
    saved_u8 = operation->return_provenance;
    operation->return_provenance = XR_SEM_RETURN_NONE;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    operation->return_provenance = saved_u8;
    saved_u8 = receiver->ownership_action;
    receiver->ownership_action = XR_SEM_OPERAND_CONSUME;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    receiver->ownership_action = saved_u8;
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);

    semantic = build_direct_rune_parameter_to_string_semantic();
    profile = build_profile(0);
    plan = NULL;
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan);
    call = find_call_by_convention(plan, XR_TARGET_CALL_CONVENTION_RUNE_TO_STRING);
    operation = call ? &semantic->operations[call->semantic_operation] : NULL;
    receiver = operation && operation->operand_begin < semantic->operand_count
                   ? &semantic->operands[operation->operand_begin]
                   : NULL;
    REQUIRE(call && operation && receiver && semantic->parameter_count == 1 &&
            semantic->parameters[0].value == receiver->value &&
            semantic->parameters[0].function == operation->function &&
            semantic->parameters[0].type == receiver->type &&
            xr_semantic_rune_to_string_is_exact(semantic, operation, NULL));

    XrSemanticParameterRecord *parameter = &semantic->parameters[0];
    uint8_t saved_parameter_u8 = parameter->flags;
    parameter->flags = 0;
    REQUIRE(!xr_semantic_rune_to_string_is_exact(semantic, operation, NULL));
    parameter->flags = saved_parameter_u8;
    saved_parameter_u8 = parameter->ownership;
    parameter->ownership = XI_OWN_BORROWED;
    REQUIRE(!xr_semantic_rune_to_string_is_exact(semantic, operation, NULL));
    parameter->ownership = saved_parameter_u8;
    uint32_t saved_parameter_u32 = parameter->function;
    parameter->function = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(!xr_semantic_rune_to_string_is_exact(semantic, operation, NULL));
    parameter->function = saved_parameter_u32;
    saved_parameter_u32 = parameter->type;
    parameter->type = operation->result_type;
    REQUIRE(!xr_semantic_rune_to_string_is_exact(semantic, operation, NULL));
    parameter->type = saved_parameter_u32;
    REQUIRE(xr_semantic_rune_to_string_is_exact(semantic, operation, NULL) &&
            xr_target_plan_verify(plan, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_map_entry_iterator_call_authority(void) {
    XrSemanticPlan *semantic =
        build_map_entry_iterator_semantic(XI_METHOD_SYMBOL_ENTRIES_ITERATOR, true);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "Map entry iterator TargetPlan failed: %s\n", error);
    REQUIRE(built && plan && plan->calls_count == 3 && plan->call_arguments_count == 0);
    XrTargetCallRecord *factory =
        find_call_by_convention(plan, XR_TARGET_CALL_CONVENTION_MAP_ENTRIES_ITERATOR);
    XrTargetCallRecord *has_next =
        find_call_by_convention(plan, XR_TARGET_CALL_CONVENTION_MAP_ENTRY_ITERATOR_HAS_NEXT);
    XrTargetCallRecord *next =
        find_call_by_convention(plan, XR_TARGET_CALL_CONVENTION_MAP_ENTRY_ITERATOR_NEXT);
    XrSemanticOperationRecord *factory_operation =
        &semantic->operations[factory->semantic_operation];
    XrSemanticOperationRecord *has_next_operation =
        &semantic->operations[has_next->semantic_operation];
    XrSemanticOperationRecord *next_operation = &semantic->operations[next->semantic_operation];
    const XrTargetValueRepRecord *factory_result =
        xr_target_plan_value_rep(plan, factory->result_value);
    const XrTargetValueRepRecord *has_next_result =
        xr_target_plan_value_rep(plan, has_next->result_value);
    const XrTargetValueRepRecord *next_result = xr_target_plan_value_rep(plan, next->result_value);
    REQUIRE(factory_operation->intrinsic_kind == XR_SEM_INTRINSIC_MAP_ENTRIES_ITERATOR &&
            has_next_operation->intrinsic_kind == XR_SEM_INTRINSIC_MAP_ENTRY_ITERATOR_HAS_NEXT &&
            next_operation->intrinsic_kind == XR_SEM_INTRINSIC_MAP_ENTRY_ITERATOR_NEXT &&
            factory_operation->semantic_immediate ==
                ((int64_t) XI_METHOD_SYMBOL_ENTRIES_ITERATOR << 1) &&
            has_next_operation->semantic_immediate == ((int64_t) XI_METHOD_SYMBOL_HAS_NEXT << 1) &&
            next_operation->semantic_immediate == ((int64_t) XI_METHOD_SYMBOL_NEXT << 1) &&
            factory->target_kind == XR_TARGET_CALL_TARGET_MAP_ENTRIES_ITERATOR &&
            has_next->target_kind == XR_TARGET_CALL_TARGET_MAP_ENTRY_ITERATOR_HAS_NEXT &&
            next->target_kind == XR_TARGET_CALL_TARGET_MAP_ENTRY_ITERATOR_NEXT &&
            factory->result_ownership == XR_TARGET_CALL_RETURN_OWNED &&
            has_next->result_ownership == XR_TARGET_CALL_NONE &&
            next->result_ownership == XR_TARGET_CALL_RETURN_OWNED && factory_result &&
            has_next_result && next_result &&
            plan->machine_reps[factory_result->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->slots[factory_result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
            plan->slots[factory_result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED &&
            plan->machine_reps[has_next_result->register_rep].kind == XR_MACHINE_REP_I1 &&
            plan->slots[has_next_result->slot].root_kind == XR_TARGET_ROOT_NONE &&
            plan->slots[has_next_result->slot].ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
            plan->machine_reps[next_result->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
            plan->slots[next_result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
            plan->slots[next_result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED);

    XrStableId saved_identity = factory->identity;
    factory->identity.bytes[0] ^= 1u;
    expect_verify_failure(plan, "XR_TARGET_1003");
    factory->identity = saved_identity;
    uint8_t saved_kind = next->target_kind;
    next->target_kind = XR_TARGET_CALL_TARGET_ITERATOR_RUNE_NEXT;
    expect_verify_failure(plan, "XR_TARGET_1003");
    next->target_kind = saved_kind;
    uint8_t saved_convention = has_next->calling_convention;
    has_next->calling_convention = XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_HAS_NEXT;
    expect_verify_failure(plan, "XR_TARGET_1003");
    has_next->calling_convention = saved_convention;
    uint8_t saved_ownership = next->result_ownership;
    next->result_ownership = XR_TARGET_CALL_NONE;
    expect_verify_failure(plan, "XR_TARGET_1003");
    next->result_ownership = saved_ownership;
    XrTargetValueRepRecord *mutable_next_result = (XrTargetValueRepRecord *) next_result;
    uint16_t saved_next_register_rep = mutable_next_result->register_rep;
    mutable_next_result->register_rep = has_next_result->register_rep;
    expect_verify_failure(plan, "XR_TARGET_1001");
    mutable_next_result->register_rep = saved_next_register_rep;
    uint8_t saved_next_root = plan->slots[next_result->slot].root_kind;
    plan->slots[next_result->slot].root_kind = XR_TARGET_ROOT_NONE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[next_result->slot].root_kind = saved_next_root;
    REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));

    int64_t saved_immediate = factory_operation->semantic_immediate;
    factory_operation->semantic_immediate = (int64_t) XI_METHOD_SYMBOL_ITERATOR << 1;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    factory_operation->semantic_immediate = saved_immediate;
    XrSemanticOperandRecord *factory_receiver =
        &semantic->operands[factory_operation->operand_begin];
    uint8_t saved_action = factory_receiver->ownership_action;
    factory_receiver->ownership_action = XR_SEM_OPERAND_CONSUME;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    factory_receiver->ownership_action = saved_action;
    uint32_t saved_result_type = next_operation->result_type;
    next_operation->result_type = factory_receiver->type;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    next_operation->result_type = saved_result_type;
    XrSemanticTypeRecord *map_type = &semantic->types[factory_receiver->type];
    XrSemanticTypeRecord *iterator_type = &semantic->types[factory_operation->result_type];
    uint32_t saved_entry_type = semantic->type_children[iterator_type->child_begin];
    semantic->type_children[iterator_type->child_begin] =
        semantic->type_children[map_type->child_begin];
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    semantic->type_children[iterator_type->child_begin] = saved_entry_type;
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);

    semantic = build_map_entry_iterator_semantic(XI_METHOD_SYMBOL_ITERATOR, true);
    profile = build_profile(0);
    plan = NULL;
    REQUIRE(!xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan == NULL);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);

    semantic = build_map_entry_iterator_semantic(XI_METHOD_SYMBOL_ENTRIES_ITERATOR, false);
    profile = build_profile(0);
    plan = NULL;
    REQUIRE(!xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan == NULL);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_rune_to_uint32_call_authority(void) {
    XrSemanticPlan *semantic = build_rune_to_uint32_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan);
    XrTargetCallRecord *call =
        find_call_by_convention(plan, XR_TARGET_CALL_CONVENTION_RUNE_TO_UINT32);
    XrSemanticOperationRecord *operation =
        call ? &semantic->operations[call->semantic_operation] : NULL;
    XrSemanticOperandRecord *receiver =
        operation ? &semantic->operands[operation->operand_begin] : NULL;
    const XrTargetValueRepRecord *result =
        operation ? xr_target_plan_value_rep(plan, operation->result_value) : NULL;
    REQUIRE(operation && receiver && operation->intrinsic_kind == XR_SEM_INTRINSIC_RUNE_TO_UINT32 &&
            call->target_kind == XR_TARGET_CALL_TARGET_RUNE_TO_UINT32 &&
            call->result_ownership == XR_TARGET_CALL_NONE &&
            call->result_mode == XR_TARGET_CALL_VALUE && call->argument_count == 0 && result &&
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

    int64_t saved_immediate = semantic->operations[call->semantic_operation].semantic_immediate;
    semantic->operations[call->semantic_operation].semantic_immediate =
        (int64_t) XI_METHOD_SYMBOL_TOSTRING << 1;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    semantic->operations[call->semantic_operation].semantic_immediate = saved_immediate;
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    uint8_t saved_ownership = receiver->ownership_action;
    receiver->ownership_action = XR_SEM_OPERAND_CONSUME;
    REQUIRE(!xr_semantic_plan_verify(semantic, error, sizeof(error)));
    receiver->ownership_action = saved_ownership;
    REQUIRE(xr_semantic_plan_verify(semantic, error, sizeof(error)));

    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);

    semantic = build_rune_to_uint32_semantic_with_receiver(
        &stub_rune, (int64_t) XI_METHOD_SYMBOL_TOSTRING << 1);
    profile = build_profile(0);
    plan = NULL;
    REQUIRE(!xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan == NULL);
    REQUIRE(strstr(error, "XR_TARGET_1003") != NULL);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);

    semantic = build_rune_to_uint32_semantic_with_receiver(
        &stub_u32, (int64_t) XI_METHOD_SYMBOL_TO_UINT32 << 1);
    profile = build_profile(0);
    plan = NULL;
    REQUIRE(!xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan == NULL);
    REQUIRE(strstr(error, "XR_TARGET_1003") != NULL);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_rune_is_whitespace_call_authority(void) {
    XrSemanticPlan *semantic = build_rune_is_whitespace_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan);
    XrTargetCallRecord *call =
        find_call_by_convention(plan, XR_TARGET_CALL_CONVENTION_RUNE_IS_WHITESPACE);
    const XrSemanticOperationRecord *operation =
        call ? xr_semantic_plan_operation(semantic, call->semantic_operation) : NULL;
    const XrTargetValueRepRecord *result =
        operation ? xr_target_plan_value_rep(plan, operation->result_value) : NULL;
    REQUIRE(operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_RUNE_IS_WHITESPACE &&
            call->target_kind == XR_TARGET_CALL_TARGET_RUNE_IS_WHITESPACE &&
            call->result_ownership == XR_TARGET_CALL_NONE &&
            call->result_mode == XR_TARGET_CALL_VALUE && call->argument_count == 0 && result &&
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
        if (semantic->operations[i].intrinsic_kind == XR_SEM_INTRINSIC_ITERATOR_RUNE_NEXT)
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
            (plan->completed_family_mask & XR_TARGET_FAMILY_STRING_BYTE_SLICE_VIEW_STORAGE) != 0);
    const XrSemanticOperationRecord *operation = NULL;
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (candidate && candidate->intrinsic_kind == XR_SEM_INTRINSIC_STRING_BYTE_SLICE_VIEW) {
            operation = candidate;
            operation_index = i;
        }
    }
    REQUIRE(operation && plan->calls_count == 1);
    XrTargetCallRecord *call = &plan->calls[0];
    const XrTargetValueRepRecord *result = xr_target_plan_value_rep(plan, operation->result_value);
    REQUIRE(result && call->semantic_operation == operation_index &&
            call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
            call->result_value == operation->result_value && call->argument_count == 0 &&
            call->flags == 0 && call->result_ownership == XR_TARGET_CALL_BORROW &&
            call->calling_convention == XR_TARGET_CALL_CONVENTION_STRING_BYTE_SLICE_VIEW &&
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

static void test_assertion_target_capability_authority(void) {
    {
        XrRuntimeTargetAuthority authority;
        REQUIRE(xr_runtime_target_authority_native_hosted(&authority) == XR_RUNTIME_ABI_OK);
        XrTargetProfile *profile =
            build_native_profile_with_provider_count(&authority, authority.provider_count);
        XrSemanticPlan *entry = build_assertion_semantic(XR_CORE_BUILTIN_ASSERT);
        XrSemanticPlan *producer = build_assertion_semantic(XR_CORE_BUILTIN_ASSERT_THROWS);
        const XrSemanticPlan *modules[2] = {entry, producer};
        uint64_t mask = 0u;
        char error[512] = {0};
        REQUIRE(xr_target_semantic_capability_requirements(modules, 1u, profile, &mask, error,
                                                           sizeof(error)));
        REQUIRE(mask == XR_TARGET_FOUNDATION_CAPABILITY_MASK);
        REQUIRE(xr_target_semantic_capability_requirements(modules, 2u, profile, &mask, error,
                                                           sizeof(error)));
        REQUIRE(mask == (XR_TARGET_FOUNDATION_CAPABILITY_MASK |
                         XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_TYPED_ERROR_BOUNDARY)));
        modules[1] = entry;
        REQUIRE(xr_target_semantic_capability_requirements(modules, 2u, profile, &mask, error,
                                                           sizeof(error)));
        REQUIRE(mask == XR_TARGET_FOUNDATION_CAPABILITY_MASK);
        xr_semantic_plan_free(producer);
        xr_semantic_plan_free(entry);
        xr_target_profile_free(profile);
    }
    const XrCoreBuiltinId builtins[] = {
        XR_CORE_BUILTIN_ASSERT,
        XR_CORE_BUILTIN_ASSERT_EQUAL,
        XR_CORE_BUILTIN_ASSERT_THROWS,
        XR_CORE_BUILTIN_ASSERT_PANICS,
    };
    const uint64_t expected_extra[] = {
        0,
        0,
        XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_TYPED_ERROR_BOUNDARY),
        XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_PANIC_BOUNDARY),
    };
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        XrRuntimeTargetAuthority authority;
        REQUIRE(xr_runtime_target_authority_native_hosted(&authority) == XR_RUNTIME_ABI_OK);
        XrTargetProfile *profile =
            build_native_profile_with_provider_count(&authority, authority.provider_count);
        XrSemanticPlan *semantic = build_assertion_semantic(builtins[i]);
        XrTargetPlan *plan = NULL;
        char error[512] = {0};
        bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
        if (!built)
            fprintf(stderr, "assertion capability plan failed: %s\n", error);
        REQUIRE(built && plan != NULL);
        uint64_t mask = 0;
        REQUIRE(xr_target_plan_capability_mask(plan, &mask));
        REQUIRE(mask == (XR_TARGET_FOUNDATION_CAPABILITY_MASK | expected_extra[i]));
        uint32_t capability_count = 0;
        const XrTargetCapabilityRecord *records =
            xr_target_plan_capabilities(plan, &capability_count);
        REQUIRE(records != NULL &&
                capability_count == (builtins[i] == XR_CORE_BUILTIN_ASSERT ||
                                             builtins[i] == XR_CORE_BUILTIN_ASSERT_EQUAL
                                         ? 2u
                                         : 3u));
        XrTargetCapabilityRecord *mutable_records = (XrTargetCapabilityRecord *) records;
        if (capability_count == 3) {
            uint16_t saved_provider = mutable_records[2].provider;
            mutable_records[2].provider = saved_provider == XR_TARGET_PROVIDER_PANIC
                                              ? XR_TARGET_PROVIDER_IO
                                              : XR_TARGET_PROVIDER_PANIC;
            expect_verify_failure(plan, "XR_TARGET_1004");
            mutable_records[2].provider = saved_provider;
            uint32_t saved_capability = mutable_records[2].capability;
            mutable_records[2].capability = XR_TARGET_CAPABILITY_ASSERTION_REPORT;
            expect_verify_failure(plan, "XR_TARGET_1004");
            mutable_records[2].capability = saved_capability;
        }
        REQUIRE(xr_target_plan_verify(plan, error, sizeof(error)));
        xr_target_plan_free(plan);
        xr_semantic_plan_free(semantic);
        xr_target_profile_free(profile);
    }

    XrRuntimeTargetAuthority hosted_without_report;
    REQUIRE(xr_runtime_target_authority_native_hosted(&hosted_without_report) == XR_RUNTIME_ABI_OK);
    XrTargetProfile *profile = build_native_profile_with_provider_count(
        &hosted_without_report, hosted_without_report.provider_count);
    XrSemanticPlan *semantic = build_assertion_semantic(XR_CORE_BUILTIN_ASSERT);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(hosted_without_report.provider_count == 2 &&
            (hosted_without_report.providers[0].provider_kind == XR_TARGET_PROVIDER_IO ||
             hosted_without_report.providers[1].provider_kind == XR_TARGET_PROVIDER_IO) == false);
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan != NULL);
    uint64_t hosted_mask = 0;
    REQUIRE(xr_target_plan_capability_mask(plan, &hosted_mask) &&
            hosted_mask == XR_TARGET_FOUNDATION_CAPABILITY_MASK);
    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);

    XrRuntimeTargetAuthority freestanding;
    const uint64_t freestanding_provider_mask =
        XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_ALLOCATOR) |
        XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_PANIC) |
        XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_IO) |
        XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_ASSERTION_REPORT);
    REQUIRE(xr_runtime_target_authority_native_freestanding(freestanding_provider_mask,
                                                            &freestanding) == XR_RUNTIME_ABI_OK);
    profile = build_native_profile_with_provider_count(&freestanding, freestanding.provider_count);
    semantic = build_assertion_semantic(XR_CORE_BUILTIN_ASSERT);
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan != NULL);
    xr_target_plan_free(plan);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);

    const XrCoreBuiltinId unsupported_freestanding_actions[] = {
        XR_CORE_BUILTIN_ASSERT_THROWS,
        XR_CORE_BUILTIN_ASSERT_PANICS,
    };
    for (size_t i = 0;
         i < sizeof(unsupported_freestanding_actions) / sizeof(unsupported_freestanding_actions[0]);
         i++) {
        profile =
            build_native_profile_with_provider_count(&freestanding, freestanding.provider_count);
        semantic = build_assertion_semantic(unsupported_freestanding_actions[i]);
        plan = NULL;
        REQUIRE(!xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) &&
                plan == NULL && strstr(error, "XR_TARGET_1004") != NULL &&
                strstr(error, "capturable failure boundary") != NULL);
        xr_semantic_plan_free(semantic);
        xr_target_profile_free(profile);
    }

    XrRuntimeTargetAuthority freestanding_wrong_report_id = freestanding;
    freestanding_wrong_report_id.providers[2].operations[0].stable_id.bytes[0] ^= 1u;
    profile = build_native_profile_with_provider_count(&freestanding_wrong_report_id,
                                                       freestanding_wrong_report_id.provider_count);
    semantic = build_assertion_semantic(XR_CORE_BUILTIN_ASSERT);
    plan = NULL;
    REQUIRE(!xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan == NULL &&
            strstr(error, "XR_TARGET_1004") != NULL);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);

    XrRuntimeTargetAuthority freestanding_wrong_report_abi = freestanding;
    freestanding_wrong_report_abi.providers[2].operations[0].call_abi.result.width = 2;
    freestanding_wrong_report_abi.providers[2].operations[0].call_abi.result.alignment = 2;
    profile = build_native_profile_with_provider_count(
        &freestanding_wrong_report_abi, freestanding_wrong_report_abi.provider_count);
    semantic = build_assertion_semantic(XR_CORE_BUILTIN_ASSERT_EQUAL);
    plan = NULL;
    REQUIRE(!xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan == NULL &&
            strstr(error, "XR_TARGET_1004") != NULL);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);

    profile = build_native_profile_with_provider_count(&freestanding, 2);
    semantic = build_assertion_semantic(XR_CORE_BUILTIN_ASSERT);
    plan = NULL;
    REQUIRE(!xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan == NULL &&
            strstr(error, "XR_TARGET_1004") != NULL);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);

    XrRuntimeTargetAuthority no_unwind;
    REQUIRE(xr_runtime_target_authority_native_hosted(&no_unwind) == XR_RUNTIME_ABI_OK);
    no_unwind.providers[1].panic_behavior = XR_TARGET_PROVIDER_PANIC_NO_RETURN;
    no_unwind.providers[1].operations[0].failure_flags = XR_TARGET_PROVIDER_FAILURE_NO_RETURN;
    profile = build_native_profile_with_provider_count(&no_unwind, no_unwind.provider_count);
    semantic = build_assertion_semantic(XR_CORE_BUILTIN_ASSERT_PANICS);
    REQUIRE(!xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)) && plan == NULL &&
            strstr(error, "XR_TARGET_1004") != NULL);
    xr_semantic_plan_free(semantic);
    xr_target_profile_free(profile);
}

static void test_structural_mutations_fail_closed(void) {
    XrSemanticPlan *semantic = build_semantic_plan();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = build_target_plan(semantic, profile);
    plan->schema_version = XR_TARGET_PLAN_SCHEMA_VERSION - UINT32_C(1);
    expect_verify_failure(plan, "XR_ARTIFACT_2000");
    plan->schema_version = XR_TARGET_PLAN_SCHEMA_VERSION;
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

static XrSemanticPlan *build_owned_string_coroutine_lifecycle_semantic(void) {
    XiFunc *function = xi_func_new("target_owned_string_coroutine_lifecycle", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    entry->sealed = true;
    XiValue *left = xi_const_str(function, entry, "target", &stub_exact_string);
    XiValue *right = xi_const_str(function, entry, "-frame", &stub_exact_string);
    XiValue *text = xi_value_new(function, entry, XI_STR_CONCAT, &stub_exact_string, 2);
    XiValue *yield = xi_value_new(function, entry, XI_YIELD, &stub_unit, 0);
    XiValue *length = xi_value_new(function, entry, XI_LEN, &stub_int, 1);
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(left && right && text && yield && length && release);
    text->args[0] = left;
    text->args[1] = right;
    length->args[0] = text;
    release->args[0] = text;
    xi_block_set_return(entry, length);
    function->stage = XI_STAGE_SEMANTIC_LOWERED;
    function->invariant_mask = xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(function, NULL));
    REQUIRE(function->coro_plan && function->coro_plan->nstates == 1 &&
            function->coro_plan->points[0].nroots == 1 &&
            function->coro_plan->points[0].ndrops == 1);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = build_target_unit_fixture_semantic(function, &semantic, error, sizeof(error));
    if (!built)
        fprintf(stderr, "owned String coroutine SemanticPlan failed: %s\n", error);
    REQUIRE(built && semantic != NULL);
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_many_owned_string_coroutine_lifecycles(uint32_t owner_count) {
    XiFunc *function = xi_func_new("target_many_owned_string_lifecycles", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    entry->sealed = true;
    XiValue **owners = (XiValue **) xr_calloc(owner_count, sizeof(*owners));
    REQUIRE(owners != NULL);
    for (uint32_t i = 0; i < owner_count; i++) {
        XiValue *left = xi_const_str(function, entry, "target", &stub_exact_string);
        XiValue *right = xi_const_str(function, entry, "-projection", &stub_exact_string);
        owners[i] = xi_value_new(function, entry, XI_STR_CONCAT, &stub_exact_string, 2);
        REQUIRE(left && right && owners[i]);
        owners[i]->args[0] = left;
        owners[i]->args[1] = right;
    }
    XiValue *yield = xi_value_new(function, entry, XI_YIELD, &stub_unit, 0);
    XiValue *sum = xi_const_int(function, entry, 0, &stub_int);
    REQUIRE(yield && sum);
    for (uint32_t i = 0; i < owner_count; i++) {
        XiValue *length = xi_value_new(function, entry, XI_LEN, &stub_int, 1);
        XiValue *next = xi_value_new(function, entry, XI_ADD, &stub_int, 2);
        XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
        REQUIRE(length && next && release);
        length->args[0] = owners[i];
        next->args[0] = sum;
        next->args[1] = length;
        release->args[0] = owners[i];
        sum = next;
    }
    xr_free(owners);
    xi_block_set_return(entry, sum);
    function->stage = XI_STAGE_SEMANTIC_LOWERED;
    function->invariant_mask = xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(function, NULL));
    REQUIRE(function->coro_plan && function->coro_plan->nstates == 1 &&
            function->coro_plan->points[0].nroots >= owner_count &&
            function->coro_plan->points[0].ndrops >= owner_count);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = build_target_unit_fixture_semantic(function, &semantic, error, sizeof(error));
    if (!built)
        fprintf(stderr, "many lifecycle SemanticPlan failed: %s\n", error);
    REQUIRE(built && semantic != NULL);
    xi_func_free(function);
    return semantic;
}

static void test_large_coroutine_lifecycle_projection_is_bounded(void) {
    const uint32_t owner_count = 64;
    XrSemanticPlan *semantic = build_many_owned_string_coroutine_lifecycles(owner_count);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "large lifecycle TargetPlan failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    REQUIRE(plan->root_maps_count == 1 && plan->root_slots_count == owner_count &&
            plan->cleanups_count == owner_count * 2u);
    REQUIRE(xr_target_plan_verify(plan, NULL, 0));
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_owned_string_coroutine_lifecycle_authority(void) {
    XrSemanticPlan *semantic = build_owned_string_coroutine_lifecycle_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "owned String coroutine TargetPlan failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    REQUIRE(plan->root_maps_count == 1 && plan->root_slots_count == 1 &&
            plan->cleanups_count == 2 && plan->coroutines_count == 1);
    XrTargetRootMapRecord *root = &plan->root_maps[0];
    REQUIRE(root->id == 0 && root->function == 0 && root->slot_begin == 0 &&
            root->slot_count == 1 &&
            root->semantic_operation == plan->coroutines[0].semantic_operation &&
            root->flags == (XR_TARGET_ROOT_SUSPEND | XR_TARGET_ROOT_CANCEL | XR_TARGET_ROOT_EXIT));
    uint32_t owned_slot = plan->root_slots[0];
    REQUIRE(owned_slot < plan->slots_count && plan->slots[owned_slot].function == 0 &&
            plan->slots[owned_slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
            plan->slots[owned_slot].ownership == XR_TARGET_OWNERSHIP_OWNED &&
            plan->machine_reps[plan->slots[owned_slot].memory_rep].kind ==
                XR_MACHINE_REP_DYN_VALUE);
    XrTargetCleanupRecord *terminal = NULL;
    XrTargetCleanupRecord *normal = NULL;
    for (uint32_t i = 0; i < plan->cleanups_count; i++) {
        XrTargetCleanupRecord *cleanup = &plan->cleanups[i];
        REQUIRE(cleanup->id == i && cleanup->function == 0 && cleanup->slot == owned_slot &&
                cleanup->action == XR_TARGET_CLEANUP_RELEASE && cleanup->provider == 0);
        if (cleanup->flags == (XR_TARGET_CLEANUP_CANCEL | XR_TARGET_CLEANUP_EXIT))
            terminal = cleanup;
        else if (cleanup->flags == 0)
            normal = cleanup;
    }
    REQUIRE(terminal && normal && terminal->semantic_operation == root->semantic_operation &&
            normal->semantic_operation != root->semantic_operation);

    uint16_t saved_root_flags = root->flags;
    root->flags &= (uint16_t) ~XR_TARGET_ROOT_CANCEL;
    expect_verify_failure(plan, "XR_TARGET_1002");
    root->flags = saved_root_flags;
    uint32_t saved_root_slot = plan->root_slots[0];
    plan->root_slots[0] = UINT32_MAX;
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->root_slots[0] = saved_root_slot;
    uint8_t saved_terminal_flags = terminal->flags;
    terminal->flags = XR_TARGET_CLEANUP_CANCEL;
    expect_verify_failure(plan, "XR_TARGET_1002");
    terminal->flags = saved_terminal_flags;
    uint32_t saved_normal_operation = normal->semantic_operation;
    normal->semantic_operation = terminal->semantic_operation;
    expect_verify_failure(plan, "XR_TARGET_1002");
    normal->semantic_operation = saved_normal_operation;
    REQUIRE(xr_target_plan_verify(plan, NULL, 0));

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_program_reachability_excludes_dead_imported_body(void) {
    XrSemanticFunctionRecord dependency_functions[3] = {
        {.is_module_initializer = 1},
        {0},
        {0},
    };
    XrSemanticSourceExportRecord dependency_exports[1] = {
        {.function = 1, .kind = XR_SEM_SOURCE_EXPORT_FUNCTION},
    };
    XrSemanticEntityRecord dependency_entities[1] = {
        {.id = {{0x11}}, .kind = XR_SEM_ENTITY_MODULE},
    };
    XrSemanticPlan dependency = {
        .fingerprint = {{0x22}},
        .functions = dependency_functions,
        .function_count = 3,
        .source_exports = dependency_exports,
        .source_export_count = 1,
        .entities = dependency_entities,
        .entity_count = 1,
    };

    XrSemanticFunctionRecord entry_functions[1] = {
        {.is_module_initializer = 1},
    };
    XrSemanticOperationRecord entry_operations[1] = {
        {.function = 0, .opcode = XI_CALL},
    };
    XrSemanticCallTargetRecord entry_targets[1] = {
        {
            .operation = 0,
            .dependency = 0,
            .source_export = 0,
            .kind = XR_SEM_CALL_TARGET_SOURCE_EXPORT,
        },
    };
    XrSemanticDependencyRecord entry_dependencies[1] = {
        {
            .module = {{0x11}},
            .semantic_fingerprint = {{0x22}},
        },
    };
    XrSemanticEntityRecord entry_entities[1] = {
        {.id = {{0x33}}, .kind = XR_SEM_ENTITY_MODULE},
    };
    XrSemanticPlan entry = {
        .functions = entry_functions,
        .function_count = 1,
        .operations = entry_operations,
        .operation_count = 1,
        .call_targets = entry_targets,
        .call_target_count = 1,
        .dependencies = entry_dependencies,
        .dependency_count = 1,
        .entities = entry_entities,
        .entity_count = 1,
    };
    const XrSemanticPlan *modules[2] = {&dependency, &entry};
    XrTargetProgramReachability reachability = {0};
    char error[256] = {0};

    REQUIRE(xr_target_program_reachability_build(modules, 2, &reachability, error, sizeof(error)));
    REQUIRE(xr_target_program_function_is_reachable(&reachability, 0, 0));
    REQUIRE(xr_target_program_function_is_reachable(&reachability, 0, 1));
    REQUIRE(!xr_target_program_function_is_reachable(&reachability, 0, 2));
    REQUIRE(xr_target_program_function_is_reachable(&reachability, 1, 0));
    xr_target_program_reachability_dispose(&reachability);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "program-reachability") == 0) {
        test_program_reachability_excludes_dead_imported_body();
        puts("TargetPlan program reachability tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "native-target-leaf-authority") == 0) {
        test_native_target_leaf_scalar_authority();
        puts("Native target leaf authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "fingerprint-snapshot-determinism") == 0) {
        test_plan_snapshot_and_determinism();
        puts("TargetPlan snapshot fingerprint tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "fingerprint-channel-close") == 0) {
        test_channel_close_call_authority();
        puts("Channel-close call fingerprint tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "fingerprint-direct-local-call") == 0) {
        test_direct_local_call_adapter_family();
        puts("Direct-local call fingerprint tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "direct-local-scalar-ref-authority") == 0) {
        test_direct_local_scalar_ref_argument_authority();
        puts("Direct-local scalar ref authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "fingerprint-tail-coroutine-chain") == 0) {
        test_tail_coroutine_chain_fingerprint();
        puts("Tail-coroutine call fingerprint tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "direct-local-class-argument-authority") == 0) {
        test_direct_local_class_argument_authority();
        puts("Direct-local class argument authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "source-instance-move-receiver-authority") == 0) {
        test_source_instance_move_receiver_authority();
        puts("Source-instance move receiver authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "source-instance-method-result-authority") == 0) {
        test_source_instance_method_result_authority();
        test_nullable_source_instance_method_result_authority();
        puts("Source-instance method result authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "direct-local-source-class-array-ref-authority") == 0) {
        test_direct_local_source_class_array_ref_authority();
        puts("Direct-local source-class Array ref authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "direct-local-forwarded-source-class-ref-authority") == 0) {
        test_direct_local_forwarded_source_class_ref_authority();
        puts("Direct-local forwarded source-class ref authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "direct-local-managed-aggregate-precursor") == 0) {
        test_direct_local_managed_aggregate_lifecycle_authority();
        puts("Direct-local managed aggregate precursor tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "direct-local-value-aggregate-result") == 0) {
        test_direct_local_value_aggregate_result_storage();
        puts("Direct-local value aggregate result tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "coroutine-lifecycle-authority") == 0) {
        test_owned_string_coroutine_lifecycle_authority();
        test_large_coroutine_lifecycle_projection_is_bounded();
        puts("Coroutine lifecycle TargetPlan authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "array-reserve-authority") == 0) {
        test_array_reserve_call_authority();
        puts("Array.reserve TargetPlan authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "array-intrinsic-authority") == 0) {
        test_array_intrinsic_call_authority();
        puts("Array intrinsic TargetPlan authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "array-hof-authority") == 0) {
        test_array_hof_call_authority();
        puts("Array HOF TargetPlan authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "tagged-string-array-copy-authority") == 0) {
        test_tagged_string_array_copy_authority();
        puts("Tagged String Array copy TargetPlan authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "array-index-result-authority") == 0) {
        test_tagged_array_index_read_authority();
        puts("Array index result TargetPlan authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "shared-const-i64-array-layout-authority") == 0) {
        test_shared_const_i64_array_layout_authority();
        puts("Shared const Array<i64> layout authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "string-slice-range-authority") == 0) {
        test_string_slice_range_call_authority();
        test_string_slice_optional_parameter_authority();
        puts("String range-slice TargetPlan authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "map-entry-iterator-authority") == 0) {
        test_map_entry_iterator_call_authority();
        puts("Map entry iterator TargetPlan authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "iterator-rune-nth-argument-authority") == 0) {
        test_iterator_rune_nth_call_argument_authority();
        puts("Iterator<rune>.nth call argument authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "rune-to-string-result-authority") == 0) {
        test_rune_to_string_result_authority();
        puts("rune.toString result authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "rune-to-uint32-parameter-authority") == 0) {
        test_rune_to_uint32_call_authority();
        puts("rune.toUInt32 parameter authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "string-runes-result-authority") == 0) {
        test_string_runes_call_authority();
        puts("String.runes result authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "stringbuilder-authority") == 0) {
        test_stringbuilder_constructor_call_authority();
        test_stringbuilder_append_rune_call_authority();
        test_direct_local_stringbuilder_ref_append_authority();
        test_stringbuilder_to_string_call_authority();
        test_stringbuilder_append_string_call_authority();
        test_stringbuilder_clear_call_authority();
        puts("StringBuilder TargetPlan authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "source-export-ref-c-emission") == 0) {
        test_source_export_ref_argument_is_not_array_projection();
        puts("Source-export ref C emission family tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "source-export-call-authority") == 0) {
        test_source_export_call_authority();
        puts("Source-export call authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "imported-source-class-constructor-authority") == 0) {
        test_imported_source_class_constructor_authority();
        puts("Imported source-class constructor authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "source-class-array-push-authority") == 0) {
        test_source_class_array_push_authority();
        puts("Source-class Array.push authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "nested-array-push-authority") == 0) {
        test_nested_array_push_authority();
        puts("Nested Array.push authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "class-field-array-push-authority") == 0) {
        test_class_field_array_push_authority();
        puts("Class-field Array.push authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "source-class-field-result-authority") == 0) {
        test_source_class_field_result_authority();
        puts("Source-class field result authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "source-structural-field-result-authority") == 0) {
        test_source_structural_field_result_authority();
        puts("Source structural field result authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "source-class-array-fill-authority") == 0) {
        test_source_class_array_fill_authority();
        puts("Source-class Array.fill authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "assertion-capability-authority") == 0) {
        test_assertion_target_capability_authority();
        puts("Assertion TargetPlan capability authority tests passed");
        return 0;
    }
    test_direct_local_raw_pointer_call_authority();
    test_unit_enum_target_rep_mutations();
    test_array_intrinsic_call_authority();
    test_array_hof_call_authority();
    test_tagged_string_array_copy_authority();
    test_tagged_array_index_read_authority();
    test_shared_const_i64_array_layout_authority();
    test_array_reserve_call_authority();
    test_source_class_array_push_authority();
    test_nested_array_push_authority();
    test_class_field_array_push_authority();
    test_source_class_field_result_authority();
    test_source_structural_field_result_authority();
    test_source_class_array_fill_authority();
    test_stringbuilder_constructor_call_authority();
    test_stringbuilder_append_rune_call_authority();
    test_direct_local_stringbuilder_ref_append_authority();
    test_stringbuilder_to_string_call_authority();
    test_stringbuilder_append_string_call_authority();
    test_stringbuilder_clear_call_authority();
    test_string_runes_call_authority();
    test_string_slice_range_call_authority();
    test_string_slice_optional_parameter_authority();
    test_iterator_rune_has_next_call_authority();
    test_iterator_rune_next_call_authority();
    test_iterator_rune_nth_call_argument_authority();
    test_rune_to_string_result_authority();
    test_map_entry_iterator_call_authority();
    test_rune_to_uint32_call_authority();
    test_rune_is_whitespace_call_authority();
    test_string_byte_slice_view_target_authority();
    test_assertion_target_capability_authority();
    test_channel_receive_storage_authority();
    test_channel_close_call_authority();
    test_source_export_call_authority();
    test_source_export_call_argument_authority();
    test_source_export_string_result_authority();
    test_source_export_ref_argument_is_not_array_projection();
    test_imported_source_class_constructor_authority();
    test_exact_i64_dynamic_entry_authority();
    test_profile_freeze_and_determinism();
    test_plan_snapshot_and_determinism();
    test_native_target_leaf_scalar_authority();
    test_builder_materializes_canonical_scalar_intents();
    test_builder_materializes_parameter_without_operation();
    test_builder_materializes_effect_void_independent_of_type();
    test_builder_materializes_exact_heap_closure_storage();
    test_builder_materializes_nested_aggregate_family();
    test_unit_enum_aggregate_dependency_rep();
    test_builder_materializes_struct_and_named_aggregates();
    test_unknown_call_target_fails_closed();
    test_direct_local_call_adapter_family();
    test_direct_local_scalar_ref_argument_authority();
    test_direct_local_class_argument_authority();
    test_direct_local_source_class_array_ref_authority();
    test_direct_local_forwarded_source_class_ref_authority();
    test_direct_local_managed_aggregate_lifecycle_authority();
    test_source_instance_move_receiver_authority();
    test_source_instance_method_result_authority();
    test_nullable_source_instance_method_result_authority();
    test_open_source_instance_method_target_fails_closed();
    test_coroutine_state_call_family();
    test_direct_local_value_aggregate_result_storage();
    test_structural_mutations_fail_closed();
    test_value_rep_mutations_fail_closed();
    test_freeze_rejects_invalid_draft();
    test_bool_and_nullable_scalar_boundary();
    test_owned_string_coroutine_lifecycle_authority();
    test_large_coroutine_lifecycle_projection_is_bounded();
    printf("TargetProfile/TargetPlan tests passed\n");
    return 0;
}
