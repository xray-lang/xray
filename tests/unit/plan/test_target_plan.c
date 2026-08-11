/*
 * test_target_plan.c - Immutable backend-neutral TargetPlan contract
 */

#include "../../../src/ir/xi.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_plan_internal.h"
#include "../../../src/plan/target/xr_target_profile_internal.h"
#include "../../../src/plan/target/xr_target_verify.h"
#include "../../../src/runtime/class/xclass_info.h"
#include "../../../src/runtime/value/xstruct_layout.h"
#include "../../../src/runtime/value/xtype.h"
#include "target_profile_test_fixture.h"
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

static XrSemanticPlan *build_semantic_plan(void) {
    XiFunc *function = xi_func_new("target_plan_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *result = xi_const_int(function, entry, 42, &stub_int);
    REQUIRE(result != NULL);
    XiValue *string = xi_const_str(function, entry, "target", &stub_string);
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

static XrTargetProfile *build_profile(uint64_t extra_atomic_width) {
    XrTestTargetProfileFixture fixture;
    REQUIRE(xr_test_target_profile_fixture_init(
        &fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    fixture.input.machine.atomic_width_mask |= extra_atomic_width;
    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    bool built = xr_target_profile_build(&fixture.input, &profile, error,
                                         sizeof(error));
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
                                                            uint32_t function,
                                                            uint32_t value) {
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
    const XrSemanticParameterRecord *parameter =
        parameter_for_value(semantic, function, value);
    const XrSemanticFunctionRecord *semantic_function =
        xr_semantic_plan_function(semantic, function);
    REQUIRE(operation && semantic_function && operation->function == function);
    XrStableId source = operation->id;
    slot->role = operation->opcode == XI_PHI ? XR_TARGET_SLOT_PHI
                                             : XR_TARGET_SLOT_TEMPORARY;
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
    int written = snprintf(key, sizeof(key),
                           "xray-target-slot-v2:function=%s:role=%u:source=%s:logical=%u",
                           function_id, (unsigned) slot->role, source_id,
                           XR_SEMANTIC_INDEX_NONE);
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
        .object = {
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
        .object = {
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

    XiFunc *function = xi_func_new("target_struct_aggregate_probe", &named);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *class_declaration =
        xi_value_new(function, entry, XI_CLASS_CREATE, &stub_unit, 0);
    REQUIRE(class_declaration != NULL);
    class_declaration->aux = &declaration;
    XiValue *structural_value =
        xi_value_new(function, entry, XI_OBJECT_NEW, &structural, 0);
    REQUIRE(structural_value != NULL);
    structural_value->aux = (void *) struct_names;
    structural_value->aux_int = xi_object_pack_aux(2, 0);
    XiValue *dynamic_value =
        xi_value_new(function, entry, XI_OBJECT_NEW, &dynamic_structural, 0);
    REQUIRE(dynamic_value != NULL);
    dynamic_value->aux = (void *) dynamic_names;
    dynamic_value->aux_int = xi_object_pack_aux(2, 0);
    XiValue *named_value = xi_value_new(function, entry, XI_AGG_NEW, &named, 1);
    REQUIRE(named_value != NULL);
    named_value->args[0] = class_declaration;
    named_value->aux = &native_layout;
    if (unknown_call) {
        XiValue *deferred_call = xi_value_new(function, entry, XI_CALL, &named, 1);
        REQUIRE(deferred_call != NULL);
        deferred_call->args[0] = class_declaration;
    }
    xi_block_set_return(entry, named_value);
    function->stage = XI_STAGE_OPTIMIZED;

    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "struct aggregate semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(function);
    return plan;
}

static bool freeze_single_scalar(XrSemanticPlan *semantic, XrTargetProfile *profile,
                                 bool bind_value, bool boolean_rep, XrTargetPlan **out,
                                 char *error, size_t error_size) {
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
                                        uint32_t *out_int_layout,
                                        uint32_t *out_string_layout) {
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

static void fill_draft(TargetFixture *fixture, XrSemanticPlan *semantic,
                       XrTargetProfile *profile) {
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
        fprintf(stderr, "mutation at test_target_plan.c:%u unexpectedly verified\n",
                mutation_line);
    REQUIRE(!verified);
    if (strncmp(error, code, strlen(code)) != 0)
        fprintf(stderr, "expected verifier code %s, got: %s\n", code, error);
    REQUIRE(strncmp(error, code, strlen(code)) == 0);
}

static void expect_verify_failure_at(XrTargetPlan *plan, const char *code,
                                     uint32_t mutation_line) {
    XrFingerprint frozen_fingerprint = plan->fingerprint;
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    expect_verify_failure_raw_at(plan, code, mutation_line);
    plan->fingerprint = frozen_fingerprint;
}

#define expect_verify_failure(plan, code) expect_verify_failure_at((plan), (code), __LINE__)
#define expect_verify_failure_raw(plan, code)                                                      \
    expect_verify_failure_raw_at((plan), (code), __LINE__)

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
                   "d0c75c3304cdd60f344ef83fa7c876e4f10c4f7e1e255296579f966708e0a9f4") == 0);

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
    XrSemanticPlan *semantic = build_semantic_plan();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *first = NULL;
    XrTargetPlan *second = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &first, error, sizeof(error)));
    REQUIRE(xr_target_plan_build(semantic, profile, &second, error, sizeof(error)));
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(first),
                                 xr_target_plan_fingerprint(second)));
    REQUIRE(first->completed_family_mask == XR_TARGET_REQUIRED_FAMILIES);
    REQUIRE(first->functions_count == 1 && first->slots_count == 2);
    REQUIRE(first->instructions_count == 0);
    REQUIRE(first->functions[0].slot_begin == 0 && first->functions[0].slot_count == 2);
    REQUIRE(first->functions[0].frame_size == 16 && first->functions[0].frame_align == 8);
    REQUIRE(xr_stable_id_compare(first->slots[0].identity, first->slots[1].identity) < 0);
    for (uint32_t i = 0; i < first->slots_count; i++) {
        const XrTargetSlotRecord *slot = &first->slots[i];
        REQUIRE(slot->id == i && slot->function == 0);
        REQUIRE(slot->logical_slot == XR_SEMANTIC_INDEX_NONE);
        REQUIRE(slot->role == XR_TARGET_SLOT_TEMPORARY);
        REQUIRE(slot->semantic_operation == operation_for_value(semantic,
                                                                 slot->semantic_value));
    }
    REQUIRE(first->value_reps_count == 3);
    REQUIRE(first->value_reps[0].semantic_value < first->value_reps[1].semantic_value);
    REQUIRE(first->value_reps[1].semantic_value < first->value_reps[2].semantic_value);
    uint32_t release_operation = XR_SEMANTIC_INDEX_NONE;
    uint32_t release_value = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, i);
        if (operation && operation->opcode == XI_RELEASE) {
            release_operation = i;
            release_value = operation->result_value;
        }
    }
    REQUIRE(release_operation != XR_SEMANTIC_INDEX_NONE &&
            release_value != XR_SEMANTIC_INDEX_NONE);
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
    XrTargetValueRepRecord saved_values[3];
    memcpy(saved_values, first->value_reps, sizeof(saved_values));
    for (uint32_t i = release_binding + 1; i < first->value_reps_count; i++)
        first->value_reps[i - 1] = first->value_reps[i];
    first->value_reps_count--;
    expect_verify_failure(first, "XR_TARGET_1001");
    memcpy(first->value_reps, saved_values, sizeof(saved_values));
    first->value_reps_count = 3;

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
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error,
                                 sizeof(error)));
    REQUIRE(plan != NULL && plan->slots_count == 1 &&
            plan->value_reps_count == 1);
    REQUIRE(plan->slots[0].role == XR_TARGET_SLOT_PARAMETER);
    REQUIRE(plan->slots[0].semantic_operation == XR_SEMANTIC_INDEX_NONE);
    REQUIRE(plan->slots[0].semantic_value ==
            xr_semantic_plan_parameter(semantic, 0)->value);
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
    XrSemanticPlan *semantic =
        build_scalar_and_effect_void_same_type_semantic();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error,
                                 sizeof(error)));
    uint32_t release_value = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, i);
        if (operation && operation->opcode == XI_RELEASE)
            release_value = operation->result_value;
    }
    REQUIRE(release_value != XR_SEMANTIC_INDEX_NONE);
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(plan, release_value);
    REQUIRE(binding != NULL && binding->slot == XR_SEMANTIC_INDEX_NONE);
    REQUIRE(plan->machine_reps[binding->register_rep].kind ==
            XR_MACHINE_REP_VOID);
    REQUIRE(plan->machine_reps[binding->memory_rep].kind ==
            XR_MACHINE_REP_VOID);
    REQUIRE(plan->slots_count == 2);
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
    REQUIRE(fixed_layout != XR_SEMANTIC_INDEX_NONE &&
            tuple_layout != XR_SEMANTIC_INDEX_NONE);
    XrTargetLayoutRecord *fixed = &first->layouts[fixed_layout];
    XrTargetLayoutRecord *tuple = &first->layouts[tuple_layout];
    REQUIRE(fixed->kind == XR_TARGET_LAYOUT_AGGREGATE && fixed->field_count == 3 &&
            fixed->fixed_prefix_size == 24 && fixed->align == 8);
    for (uint32_t i = 0; i < fixed->field_count; i++) {
        const XrTargetFieldRecord *field = &first->fields[fixed->field_begin + i];
        REQUIRE(field->semantic_field == i && field->offset == i * 8u &&
                field->size == 8 && field->align == 8);
    }
    REQUIRE(tuple->kind == XR_TARGET_LAYOUT_AGGREGATE && tuple->field_count == 2 &&
            tuple->fixed_prefix_size == 32 && tuple->align == 8);
    XrTargetFieldRecord *tuple_first = &first->fields[tuple->field_begin];
    XrTargetFieldRecord *tuple_nested = &first->fields[tuple->field_begin + 1u];
    REQUIRE(tuple_first->offset == 0 && tuple_first->size == 1 && tuple_first->align == 1);
    REQUIRE(tuple_nested->offset == 8 && tuple_nested->size == 24 &&
            tuple_nested->align == 8);
    REQUIRE(first->machine_reps[tuple_nested->memory_rep].kind ==
            XR_MACHINE_REP_AGGREGATE);
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
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));

    uint32_t structural_type = XR_SEMANTIC_INDEX_NONE;
    uint32_t dynamic_type = XR_SEMANTIC_INDEX_NONE;
    uint32_t named_type = XR_SEMANTIC_INDEX_NONE;
    uint32_t child_table_count = 0;
    const uint32_t *children =
        xr_semantic_plan_type_children(semantic, &child_table_count);
    for (uint32_t i = 0; i < xr_semantic_plan_type_count(semantic); i++) {
        const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, i);
        REQUIRE(type != NULL);
        if (type->kind == XR_KIND_INSTANCE &&
            (type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) != 0) {
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
    REQUIRE(structural_type != XR_SEMANTIC_INDEX_NONE &&
            dynamic_type != XR_SEMANTIC_INDEX_NONE &&
            named_type != XR_SEMANTIC_INDEX_NONE);

    const XrTargetLayoutRecord *structural_layout = NULL;
    const XrTargetLayoutRecord *named_layout = NULL;
    for (uint32_t i = 0; i < plan->layouts_count; i++) {
        const XrTargetLayoutRecord *layout = &plan->layouts[i];
        REQUIRE(layout->semantic_type != dynamic_type);
        if (layout->semantic_type == structural_type)
            structural_layout = layout;
        else if (layout->semantic_type == named_type)
            named_layout = layout;
    }
    REQUIRE(structural_layout != NULL && structural_layout->kind == XR_TARGET_LAYOUT_AGGREGATE &&
            structural_layout->field_count == 2 && structural_layout->fixed_prefix_size == 16 &&
            structural_layout->align == 8);
    REQUIRE(named_layout != NULL && named_layout->kind == XR_TARGET_LAYOUT_AGGREGATE &&
            named_layout->field_count == 2 && named_layout->fixed_prefix_size == 16 &&
            named_layout->align == 16);

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

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static XrSemanticPlan *build_direct_local_scalar_calls(uint16_t call_opcode) {
    XiFunc *root = xi_func_new("target_direct_call_root", &stub_int);
    XiFunc *child = xi_func_new("target_direct_call_child", &stub_int);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);
    child->nparams = child->min_params = 1;
    child->params = (XiValue **) xr_calloc(1, sizeof(*child->params));
    REQUIRE(child->params != NULL);
    child->params[0] = xi_param(child, child_entry, 0, &stub_int);
    REQUIRE(child->params[0] != NULL);
    xi_block_set_return(child_entry, child->params[0]);

    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;

    XiValue *closure = xi_value_new(root, root_entry,
                                    call_opcode == XI_TAIL_CALL ? XI_CLOSURE_NEW
                                                               : XI_STACK_ALLOC,
                                    &stub_function, 0);
    REQUIRE(closure != NULL);
    if (call_opcode != XI_TAIL_CALL)
        closure->aux_int = XI_CLOSURE_NEW;
    closure->aux = child;
    XiValue *alias = xi_value_new(root, root_entry, XI_COPY, &stub_function, 1);
    REQUIRE(alias != NULL);
    alias->args[0] = closure;
    alias->aux_int = XI_COPY_KIND_IDENTITY;
    XiValue *argument = xi_const_int(root, root_entry, 41, &stub_int);
    REQUIRE(argument != NULL);
    XiValue *first = xi_value_new(root, root_entry, call_opcode, &stub_int, 2);
    XiValue *second = call_opcode == XI_CALL
                          ? xi_value_new(root, root_entry, call_opcode, &stub_int, 2)
                          : NULL;
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
    XiValue *child_result = xi_value_new(child, child_entry, XI_TUPLE_NEW,
                                         &aggregate, 2);
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

static void test_direct_local_call_adapter_family(void) {
    XrSemanticPlan *semantic = build_direct_local_scalar_calls(XI_CALL);
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *first = NULL;
    XrTargetPlan *second = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &first, error, sizeof(error)));
    REQUIRE(xr_target_plan_build(semantic, profile, &second, error, sizeof(error)));
    REQUIRE(first->calls_count == 2 && first->call_arguments_count == 2 &&
            first->adapters_count == 0);
    REQUIRE(xr_fingerprint_equal(first->fingerprint, second->fingerprint));
    char call_hex[XR_FINGERPRINT_BYTES * 2 + 1];
    xr_fingerprint_hex(first->calls[0].fingerprint, call_hex);
    REQUIRE(strcmp(call_hex,
                   "d979e7af979c1655379d6741552cbbf479db07c7d2330dd947d29c19d70898e8") == 0);
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(profile);
    REQUIRE(machine != NULL);
    for (uint32_t i = 0; i < first->calls_count; i++) {
        const XrTargetCallRecord *call = &first->calls[i];
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(semantic, i);
        REQUIRE(target != NULL && call->id == i && call->semantic_call_target == i &&
                call->semantic_operation == target->operation &&
                call->callee_function == target->function &&
                call->calling_convention == XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL &&
                call->target_kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
                call->native_abi == machine->native_abi && call->result_mode == XR_TARGET_CALL_VALUE &&
                call->result_ownership == XR_TARGET_CALL_NONE &&
                call->caller_storage_slot == XR_SEMANTIC_INDEX_NONE &&
                call->error_slot == XR_SEMANTIC_INDEX_NONE &&
                call->error_mode == XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL &&
                call->adapter_begin == 0 && call->adapter_count == 0 &&
                call->argument_begin == i && call->argument_count == 1);
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

static void test_direct_local_future_storage_fails_closed(void) {
    XrTargetProfile *profile = build_profile(0);
    char error[512] = {0};
    XrTargetPlan *plan = NULL;
    XrSemanticPlan *tail = build_direct_local_scalar_calls(XI_TAIL_CALL);
    REQUIRE(!xr_target_plan_build(tail, profile, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL && strncmp(error, "XR_TARGET_1003", 14) == 0);
    xr_semantic_plan_free(tail);
    XrSemanticPlan *aggregate = build_direct_local_aggregate_call();
    error[0] = '\0';
    REQUIRE(!xr_target_plan_build(aggregate, profile, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL && strncmp(error, "XR_TARGET_1003", 14) == 0);
    xr_semantic_plan_free(aggregate);
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
        {.id = 1, .kind = XR_TARGET_EXTENT_FIXED,
         .element_layout = XR_SEMANTIC_INDEX_NONE},
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
    XrTargetPlan *nullable_plan = NULL;
    REQUIRE(freeze_single_scalar(nullable_semantic, profile, false, false, &nullable_plan, error,
                                 sizeof(error)));
    xr_target_plan_free(nullable_plan);
    nullable_plan = NULL;
    REQUIRE(!freeze_single_scalar(nullable_semantic, profile, true, false, &nullable_plan, error,
                                  sizeof(error)));
    REQUIRE(nullable_plan == NULL);
    REQUIRE(strncmp(error, "XR_TARGET_1001", strlen("XR_TARGET_1001")) == 0);
    xr_semantic_plan_free(nullable_semantic);
    xr_target_profile_free(profile);
}

int main(void) {
    test_profile_freeze_and_determinism();
    test_plan_snapshot_and_determinism();
    test_builder_materializes_canonical_scalar_intents();
    test_builder_materializes_parameter_without_operation();
    test_builder_materializes_effect_void_independent_of_type();
    test_builder_materializes_nested_aggregate_family();
    test_builder_materializes_struct_and_named_aggregates();
    test_unknown_call_target_fails_closed();
    test_direct_local_call_adapter_family();
    test_direct_local_future_storage_fails_closed();
    test_structural_mutations_fail_closed();
    test_value_rep_mutations_fail_closed();
    test_freeze_rejects_invalid_draft();
    test_bool_and_nullable_scalar_boundary();
    printf("TargetProfile/TargetPlan tests passed\n");
    return 0;
}
