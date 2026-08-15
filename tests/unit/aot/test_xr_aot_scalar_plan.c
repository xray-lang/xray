/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_aot_scalar_plan.c - Scalar TargetPlan and C emission boundary tests
 */

#include "../../../src/aot/emit_c/xr_c_emission_plan.h"
#ifdef XAOT_REP_H
#error "C value emission plan must not include the legacy XaotValueRep model"
#endif
#ifdef XI_H
#error "C value emission plan must not include compiler IR pointers"
#endif
#ifdef XR_SEMANTIC_PLAN_H
#error "C value emission plan must not include SemanticPlan"
#endif
#ifdef XTYPE_H
#error "C value emission plan must not include analyzer/runtime type objects"
#endif
#include "../../../src/aot/refine/xr_aot_scalar_value.h"
#include "../../../src/aot/refine/xr_aot_representation_refinement.h"
#include "../../../src/ir/xi_opt.h"
#include "../../../src/ir/xi_module.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_plan_internal.h"
#include "../../../src/plan/target/xr_target_profile.h"
#include "../../../src/plan/target/xr_target_verify.h"
#include "../../../src/frontend/analyzer/xa_intrinsic_registry.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/runtime/value/xenum_layout.h"
#include "../plan/target_profile_test_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Complete the otherwise opaque type only in this mutation test translation
 * unit. Production consumers receive read-only row views. */
struct XrCEmissionPlan {
    XrCValueEmissionView *values;
    uint32_t value_count;
    XrCCallArgumentEmissionView *call_arguments;
    uint32_t call_argument_count;
    XrCRecipeArgumentView *recipe_arguments;
    uint32_t recipe_argument_count;
    uint32_t schema_version;
    XrFingerprint target_fingerprint;
    XrFingerprint profile_fingerprint;
    XrFingerprint fingerprint;
    bool verified;
};

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

typedef struct ScalarFixture {
    XiFunc *function;
    XiValue *integer;
    XiValue *boolean;
    XiValue *string;
    XiValue *release;
} ScalarFixture;

static XrType scalar_int = {
    .kind = XR_KIND_INT,
    .id = 1,
    .scalar_rep = XR_NATIVE_I64,
    .frozen = true,
};
static XrType scalar_bool = {
    .kind = XR_KIND_BOOL,
    .id = 2,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .frozen = true,
};
static XrType scalar_string = {
    .kind = XR_KIND_STRING,
    .id = 3,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .frozen = true,
};
static XrType scalar_unit = {
    .kind = XR_KIND_UNIT,
    .id = 4,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .frozen = true,
};
static XrType dynamic_closure = {
    .kind = XR_KIND_FUNCTION,
    .id = 5,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .frozen = true,
    .function = {
        .return_type = &scalar_int,
        .throw_effect = XR_FN_EFFECT_NO_THROW,
    },
};
static XrType channel_type = {
    .kind = XR_KIND_CHANNEL,
    .id = 6,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .frozen = true,
    .container = {.element_type = &scalar_int},
};
static XrType stringbuilder_type = {
    .kind = XR_KIND_INSTANCE,
    .id = 7,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .frozen = true,
    .instance = {.class_name = "StringBuilder"},
};
static XrType byte_type = {
    .kind = XR_KIND_INT,
    .id = 8,
    .scalar_rep = XR_NATIVE_U8,
    .frozen = true,
};
static XrType byte_slice_type = {
    .kind = XR_KIND_SLICE,
    .id = 9,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .frozen = true,
    .container = {.element_type = &byte_type},
};
static XrType dynamic_any = {
    .kind = XR_KIND_UNKNOWN,
    .id = 10,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .frozen = true,
};
static XrType rune_type = {
    .kind = XR_KIND_RUNE,
    .id = 11,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .frozen = true,
};
static XrType u32_type = {
    .kind = XR_KIND_INT,
    .id = 13,
    .scalar_rep = XR_NATIVE_U32,
    .frozen = true,
};
static XrType *iterator_rune_args[] = {&rune_type};
static XrType iterator_rune_type = {
    .kind = XR_KIND_INSTANCE,
    .id = 12,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .frozen = true,
    .instance = {
        .class_name = "Iterator",
        .type_args = iterator_rune_args,
        .type_arg_count = 1,
    },
};

static XrTargetProfile *build_profile(bool ilp32, bool freestanding_runtime) {
    XrTargetProfile *profile = xr_test_target_profile_build(
        ilp32, freestanding_runtime ? XR_TARGET_RUNTIME_PROFILE_FREESTANDING
                                   : XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    return profile;
}

static XrTargetProfile *build_exact_profile(void) {
    return build_profile(false, false);
}

static ScalarFixture build_scalar_fixture(void) {
    ScalarFixture fixture = {0};
    fixture.function = xi_func_new("scalar_target_cutover", &scalar_int);
    REQUIRE(fixture.function != NULL);
    XiBlock *entry = xi_block_new(fixture.function);
    REQUIRE(entry != NULL);
    fixture.boolean = xi_const_bool(fixture.function, entry, true, &scalar_bool);
    fixture.integer = xi_const_int(fixture.function, entry, 42, &scalar_int);
    fixture.string = xi_const_str(fixture.function, entry, "not scalar", &scalar_string);
    fixture.release = xi_value_new(fixture.function, entry, XI_RELEASE, &scalar_unit, 1);
    REQUIRE(fixture.integer && fixture.boolean && fixture.string && fixture.release);
    fixture.release->args[0] = fixture.string;
    xi_block_set_return(entry, fixture.integer);
    fixture.function->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(fixture.function, error, sizeof(error)));
    return fixture;
}

static XrTargetPlan *build_target_plan(const XrSemanticPlan *semantic_plan,
                                      XrTargetProfile *profile) {
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_target_plan_build(semantic_plan, profile, &plan, error,
                                      sizeof(error));
    if (!built)
        fprintf(stderr, "TargetPlan fixture failed: %s\n", error);
    REQUIRE(built);
    return plan;
}

static void require_ready(const XrTargetPlan *target_plan,
                          const XrCEmissionPlan *emission_plan,
                          const ScalarFixture *fixture,
                          const XiValue *value, XrCValueRep expected_rep,
                          const char *expected_c_type) {
    XrCValueEmissionView view = {0};
    char error[512] = {0};
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(target_plan, fixture->function, value,
                                            &semantic_function, &semantic_value, error,
                                            sizeof(error)));
    REQUIRE(semantic_function == fixture->function->semantic_plan_function_index);
    REQUIRE(xr_c_emission_plan_value_view(emission_plan, semantic_value, &view, error,
                                           sizeof(error)));
    REQUIRE(view.rep == expected_rep);
    REQUIRE(view.target_register_rep == view.target_memory_rep);
    REQUIRE(view.target_register_kind == view.target_memory_kind);
    REQUIRE(view.register_bits != 0 || expected_rep == XR_C_VALUE_REP_VOID);
    REQUIRE(strcmp(view.c_type, expected_c_type) == 0);
}

static void test_scalar_plan_and_emission_view(void) {
    ScalarFixture fixture = build_scalar_fixture();
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *first = NULL;
    XrTargetPlan *same = NULL;
    XrCEmissionPlan *emission = NULL;
    XrCEmissionPlan *same_emission = NULL;
    char error[512] = {0};
    first = build_target_plan(fixture.function->semantic_plan, profile);
    same = build_target_plan(fixture.function->semantic_plan, profile);
    REQUIRE(xr_target_plan_is_verified(first));
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(first),
                                 xr_target_plan_fingerprint(same)));
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    REQUIRE(xr_c_emission_plan_build(first, profile_fingerprint, &emission, error,
                                     sizeof(error)));
    REQUIRE(xr_c_emission_plan_build(same, profile_fingerprint, &same_emission, error,
                                     sizeof(error)));
    REQUIRE(xr_c_emission_plan_is_verified(emission));
    REQUIRE(xr_c_emission_plan_value_count(emission) == 4);
    REQUIRE(xr_fingerprint_equal(xr_c_emission_plan_target_fingerprint(emission),
                                 xr_target_plan_fingerprint(first)));
    REQUIRE(xr_fingerprint_equal(xr_c_emission_plan_profile_fingerprint(emission),
                                 profile_fingerprint));
    REQUIRE(xr_fingerprint_equal(xr_c_emission_plan_fingerprint(emission),
                                 xr_c_emission_plan_fingerprint(same_emission)));

    uint32_t count = 0;
    REQUIRE(xr_target_plan_value_reps(first, &count) != NULL && count == 4);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(first, &count);
    REQUIRE(slots != NULL && count == 3);
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t integer_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t boolean_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(first, fixture.function, fixture.integer,
                                            &semantic_function, &integer_value, error,
                                            sizeof(error)));
    REQUIRE(xr_aot_scalar_semantic_value_id(first, fixture.function, fixture.boolean,
                                            &semantic_function, &boolean_value, error,
                                            sizeof(error)));
    const XrTargetValueRepRecord *integer_binding =
        xr_target_plan_value_rep(first, integer_value);
    const XrTargetValueRepRecord *boolean_binding =
        xr_target_plan_value_rep(first, boolean_value);
    REQUIRE(integer_binding != NULL && boolean_binding != NULL);
    REQUIRE(integer_binding->slot != XR_SEMANTIC_INDEX_NONE &&
            integer_binding->slot < count);
    REQUIRE(boolean_binding->slot != XR_SEMANTIC_INDEX_NONE &&
            boolean_binding->slot < count);
    REQUIRE(integer_binding->slot != boolean_binding->slot);
    const XrTargetSlotRecord *integer_slot = &slots[integer_binding->slot];
    const XrTargetSlotRecord *boolean_slot = &slots[boolean_binding->slot];
    REQUIRE(integer_slot->size == 8 && integer_slot->align == 8);
    REQUIRE(boolean_slot->size == 1 && boolean_slot->align == 1);
    REQUIRE(integer_slot->offset % integer_slot->align == 0);
    REQUIRE(boolean_slot->offset % boolean_slot->align == 0);
    uint32_t integer_end = integer_slot->offset + integer_slot->size;
    uint32_t boolean_end = boolean_slot->offset + boolean_slot->size;
    REQUIRE(integer_end <= boolean_slot->offset || boolean_end <= integer_slot->offset);
    const XrTargetFunctionRecord *functions = xr_target_plan_functions(first, &count);
    REQUIRE(functions != NULL && semantic_function < count);
    REQUIRE(functions[semantic_function].frame_size == 32);
    REQUIRE(functions[semantic_function].frame_align == 8);
    REQUIRE(xr_target_plan_layouts(first, &count) != NULL && count == 3);
    require_ready(first, emission, &fixture, fixture.integer, XR_C_VALUE_REP_I64, "int64_t");
    require_ready(first, emission, &fixture, fixture.boolean, XR_C_VALUE_REP_BOOL, "uint8_t");
    require_ready(first, emission, &fixture, fixture.release, XR_C_VALUE_REP_VOID, "void");

    XrCValueEmissionView view = {0};
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(first, fixture.function, fixture.string,
                                            &semantic_function, &semantic_value, error,
                                            sizeof(error)));
    REQUIRE(xr_c_emission_plan_value_view(emission, semantic_value, &view,
                                          error, sizeof(error)));
    REQUIRE(view.rep == XR_C_VALUE_REP_TAGGED &&
            view.target_register_kind == XR_MACHINE_REP_DYN_VALUE &&
            view.target_memory_kind == XR_MACHINE_REP_DYN_VALUE &&
            view.materialization ==
                XR_C_VALUE_MATERIALIZATION_STRING_LITERAL_VIEW &&
            view.literal_byte_length == strlen("not scalar") &&
            view.literal_bytes &&
            strcmp(view.literal_bytes, "not scalar") == 0 &&
            view.recipe_operand_value == UINT32_MAX &&
            view.recipe_symbol == NULL &&
            strcmp(view.c_type, "XrValue") == 0);

    XrCValueEmissionView *string_row = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == semantic_value)
            string_row = &emission->values[i];
    REQUIRE(string_row != NULL);
    uint8_t saved_materialization = string_row->materialization;
    string_row->materialization = XR_C_VALUE_MATERIALIZATION_NONE;
    REQUIRE(!xr_c_emission_plan_verify(emission, first, profile_fingerprint,
                                       error, sizeof(error)));
    string_row->materialization = saved_materialization;
    const char *saved_literal = string_row->literal_bytes;
    string_row->literal_bytes = "not scalAr";
    REQUIRE(!xr_c_emission_plan_verify(emission, first, profile_fingerprint,
                                       error, sizeof(error)));
    string_row->literal_bytes = saved_literal;
    uint32_t saved_length = string_row->literal_byte_length;
    string_row->literal_byte_length--;
    REQUIRE(!xr_c_emission_plan_verify(emission, first, profile_fingerprint,
                                       error, sizeof(error)));
    string_row->literal_byte_length = saved_length;
    const char *saved_spelling = string_row->c_type;
    string_row->c_type = "void *";
    REQUIRE(!xr_c_emission_plan_verify(emission, first, profile_fingerprint,
                                       error, sizeof(error)));
    string_row->c_type = saved_spelling;
    REQUIRE(xr_c_emission_plan_verify(emission, first, profile_fingerprint,
                                      error, sizeof(error)));
    REQUIRE(!xr_c_emission_plan_value_view(NULL, semantic_value, &view, error, sizeof(error)));
    REQUIRE(strncmp(error, "XR_TARGET_1001", strlen("XR_TARGET_1001")) == 0);

    xr_c_emission_plan_free(same_emission);
    xr_c_emission_plan_free(emission);
    xr_target_plan_free(same);
    xr_target_plan_free(first);
    xr_target_profile_free(profile);
    xi_func_free(fixture.function);
}

static void test_missing_semantic_authority_fails_closed(void) {
    ScalarFixture fixture = build_scalar_fixture();
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    plan = build_target_plan(fixture.function->semantic_plan, profile);
    XiFunc *unattached = xi_func_new("unattached", &scalar_int);
    REQUIRE(unattached != NULL);
    XiBlock *entry = xi_block_new(unattached);
    REQUIRE(entry != NULL);
    XiValue *value = xi_const_int(unattached, entry, 1, &scalar_int);
    REQUIRE(value != NULL);
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(!xr_aot_scalar_semantic_value_id(plan, unattached, value, &semantic_function,
                                             &semantic_value, error, sizeof(error)));
    REQUIRE(strncmp(error, "XR_TARGET_1001", strlen("XR_TARGET_1001")) == 0);
    xi_func_free(unattached);
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xi_func_free(fixture.function);
}

static void test_cross_function_value_substitution_fails_closed(void) {
    XiFunc *root = xi_func_new("root", &scalar_int);
    XiFunc *child = xi_func_new("child", &scalar_int);
    REQUIRE(root && child);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry && child_entry);
    XiValue *root_value = xi_const_int(root, root_entry, 1, &scalar_int);
    XiValue *child_value = xi_const_int(child, child_entry, 2, &scalar_int);
    REQUIRE(root_value && child_value);
    xi_block_set_return(root_entry, root_value);
    xi_block_set_return(child_entry, child_value);
    root->stage = XI_STAGE_OPTIMIZED;
    child->stage = XI_STAGE_OPTIMIZED;
    child->parent_func = root;
    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->children_cap = 1;
    root->nchildren = 1;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(root, error, sizeof(error)));
    REQUIRE(root->semantic_plan == child->semantic_plan);

    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *plan = build_target_plan(root->semantic_plan, profile);
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(!xr_aot_scalar_semantic_value_id(plan, root, child_value, &semantic_function,
                                             &semantic_value, error, sizeof(error)));
    REQUIRE(strncmp(error, "XR_TARGET_1001", strlen("XR_TARGET_1001")) == 0);
    REQUIRE(xr_aot_scalar_semantic_value_id(plan, child, child_value, &semantic_function,
                                            &semantic_value, error, sizeof(error)));
    REQUIRE(semantic_function == child->semantic_plan_function_index);

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xi_func_free(root);
}

static void test_parameter_identity_requires_exact_member(void) {
    XiFunc *function = xi_func_new("parameter_identity", &scalar_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *parameter = xi_param(function, entry, 0, &scalar_int);
    XiValue *replacement = xi_const_int(function, entry, 7, &scalar_int);
    REQUIRE(parameter && replacement);
    function->params = (XiValue **) xr_calloc(1, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = parameter;
    function->nparams = 1;
    xi_block_set_return(entry, parameter);
    function->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(function, error, sizeof(error)));
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *plan = build_target_plan(function->semantic_plan, profile);
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(plan, function, parameter, &semantic_function,
                                            &semantic_value, error, sizeof(error)));
    function->params[0] = replacement;
    REQUIRE(!xr_aot_scalar_semantic_value_id(plan, function, parameter, &semantic_function,
                                             &semantic_value, error, sizeof(error)));
    REQUIRE(strncmp(error, "XR_TARGET_1001", strlen("XR_TARGET_1001")) == 0);

    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xi_func_free(function);
}

typedef struct ScalarKnownAnswer {
    XrType type;
    XrCValueRep c_rep;
    uint16_t machine_kind;
    uint16_t fixed_register_bits;
    uint32_t fixed_memory_size;
    const char *c_type;
    bool pointer_sized;
} ScalarKnownAnswer;

static ScalarKnownAnswer scalar_known_answers[] = {
    {{.kind = XR_KIND_INT, .id = 100, .frozen = true, .scalar_rep = XR_NATIVE_I8},
     XR_C_VALUE_REP_I8, XR_MACHINE_REP_I8, 8, 1, "int8_t", false},
    {{.kind = XR_KIND_INT, .id = 101, .frozen = true, .scalar_rep = XR_NATIVE_U8},
     XR_C_VALUE_REP_U8, XR_MACHINE_REP_U8, 8, 1, "uint8_t", false},
    {{.kind = XR_KIND_INT, .id = 102, .frozen = true, .scalar_rep = XR_NATIVE_I16},
     XR_C_VALUE_REP_I16, XR_MACHINE_REP_I16, 16, 2, "int16_t", false},
    {{.kind = XR_KIND_INT, .id = 103, .frozen = true, .scalar_rep = XR_NATIVE_U16},
     XR_C_VALUE_REP_U16, XR_MACHINE_REP_U16, 16, 2, "uint16_t", false},
    {{.kind = XR_KIND_INT, .id = 104, .frozen = true, .scalar_rep = XR_NATIVE_I32},
     XR_C_VALUE_REP_I32, XR_MACHINE_REP_I32, 32, 4, "int32_t", false},
    {{.kind = XR_KIND_INT, .id = 105, .frozen = true, .scalar_rep = XR_NATIVE_U32},
     XR_C_VALUE_REP_U32, XR_MACHINE_REP_U32, 32, 4, "uint32_t", false},
    {{.kind = XR_KIND_INT, .id = 106, .frozen = true, .scalar_rep = XR_NATIVE_I64},
     XR_C_VALUE_REP_I64, XR_MACHINE_REP_I64, 64, 8, "int64_t", false},
    {{.kind = XR_KIND_INT, .id = 107, .frozen = true, .scalar_rep = XR_NATIVE_U64},
     XR_C_VALUE_REP_U64, XR_MACHINE_REP_U64, 64, 8, "uint64_t", false},
    {{.kind = XR_KIND_INT, .id = 108, .frozen = true, .scalar_rep = XR_NATIVE_ISIZE},
     XR_C_VALUE_REP_ISIZE, XR_MACHINE_REP_ISIZE, 0, 0, "ptrdiff_t", true},
    {{.kind = XR_KIND_INT, .id = 109, .frozen = true, .scalar_rep = XR_NATIVE_USIZE},
     XR_C_VALUE_REP_USIZE, XR_MACHINE_REP_USIZE, 0, 0, "size_t", true},
    {{.kind = XR_KIND_FLOAT, .id = 110, .frozen = true, .scalar_rep = XR_NATIVE_F32},
     XR_C_VALUE_REP_F32, XR_MACHINE_REP_F32, 32, 4, "float", false},
    {{.kind = XR_KIND_FLOAT, .id = 111, .frozen = true, .scalar_rep = XR_NATIVE_F64},
     XR_C_VALUE_REP_F64, XR_MACHINE_REP_F64, 64, 8, "double", false},
    {{.kind = XR_KIND_BOOL, .id = 112, .frozen = true, .scalar_rep = XR_SCALAR_REP_NONE},
     XR_C_VALUE_REP_BOOL, XR_MACHINE_REP_I1, 1, 1, "uint8_t", false},
    {{.kind = XR_KIND_RUNE, .id = 113, .frozen = true, .scalar_rep = XR_SCALAR_REP_NONE},
     XR_C_VALUE_REP_RUNE, XR_MACHINE_REP_RUNE, 32, 4, "uint32_t", false},
    {{.kind = XR_KIND_UNIT, .id = 114, .frozen = true, .scalar_rep = XR_SCALAR_REP_NONE},
     XR_C_VALUE_REP_VOID, XR_MACHINE_REP_VOID, 0, 0, "void", false},
    {{.kind = XR_KIND_NEVER, .id = 115, .frozen = true, .scalar_rep = XR_SCALAR_REP_NONE},
     XR_C_VALUE_REP_VOID, XR_MACHINE_REP_VOID, 0, 0, "void", false},
};

typedef struct ScalarMatrixFixture {
    XiFunc *function;
    XiValue *values[sizeof(scalar_known_answers) / sizeof(scalar_known_answers[0])];
} ScalarMatrixFixture;

static XiValue *build_scalar_constant(XiFunc *function, XiBlock *entry,
                                      ScalarKnownAnswer *answer) {
    switch (answer->type.kind) {
        case XR_KIND_INT: return xi_const_int(function, entry, 1, &answer->type);
        case XR_KIND_FLOAT: return xi_const_float(function, entry, 1.0, &answer->type);
        case XR_KIND_BOOL: return xi_const_bool(function, entry, true, &answer->type);
        case XR_KIND_RUNE: return xi_const_rune(function, entry, 'x', &answer->type);
        case XR_KIND_UNIT: return xi_value_new(function, entry, XI_RELEASE, &answer->type, 1);
        default: return NULL;
    }
}

static ScalarMatrixFixture build_scalar_matrix_fixture(void) {
    ScalarMatrixFixture fixture = {0};
    fixture.function = xi_func_new("all_scalar_emission_known_answers",
                                   &scalar_known_answers[0].type);
    REQUIRE(fixture.function != NULL);
    XiBlock *entry = xi_block_new(fixture.function);
    REQUIRE(entry != NULL);
    uint32_t answer_count =
        (uint32_t) (sizeof(scalar_known_answers) / sizeof(scalar_known_answers[0]));
    uint32_t never_index = answer_count - 1u;
    fixture.values[never_index] =
        xi_param(fixture.function, entry, 0, &scalar_known_answers[never_index].type);
    REQUIRE(fixture.values[never_index] != NULL);
    fixture.function->params = (XiValue **) xr_calloc(1, sizeof(*fixture.function->params));
    REQUIRE(fixture.function->params != NULL);
    fixture.function->params[0] = fixture.values[never_index];
    fixture.function->nparams = 1;
    XiValue *release_owner =
        xi_const_str(fixture.function, entry, "scalar matrix owner", &scalar_string);
    REQUIRE(release_owner != NULL);
    for (uint32_t i = 0; i < never_index; i++) {
        fixture.values[i] =
            build_scalar_constant(fixture.function, entry, &scalar_known_answers[i]);
        REQUIRE(fixture.values[i] != NULL);
    }
    fixture.values[answer_count - 2u]->args[0] = release_owner;
    xi_block_set_return(entry, fixture.values[0]);
    fixture.function->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(fixture.function, error, sizeof(error)));
    return fixture;
}

static void require_scalar_known_answer(const XrTargetPlan *target_plan,
                                        const XrCEmissionPlan *emission_plan,
                                        const ScalarMatrixFixture *fixture, uint32_t index,
                                        bool ilp32) {
    const ScalarKnownAnswer *answer = &scalar_known_answers[index];
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    char error[512] = {0};
    REQUIRE(xr_aot_scalar_semantic_value_id(target_plan, fixture->function,
                                            fixture->values[index], &semantic_function,
                                            &semantic_value, error, sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission_plan, semantic_value, &view, error,
                                           sizeof(error)));
    uint16_t expected_bits = answer->pointer_sized ? (ilp32 ? 32u : 64u)
                                                   : answer->fixed_register_bits;
    uint32_t expected_size = answer->pointer_sized ? (ilp32 ? 4u : 8u)
                                                   : answer->fixed_memory_size;
    REQUIRE(semantic_function == fixture->function->semantic_plan_function_index);
    REQUIRE(view.rep == answer->c_rep);
    REQUIRE(view.target_register_kind == answer->machine_kind);
    REQUIRE(view.target_memory_kind == answer->machine_kind);
    REQUIRE(view.register_bits == expected_bits);
    REQUIRE(view.memory_size == expected_size);
    REQUIRE(view.memory_align == expected_size);
    REQUIRE(strcmp(view.c_type, answer->c_type) == 0);
}

static void test_all_scalar_c_spelling_known_answers(bool ilp32) {
    ScalarMatrixFixture fixture = build_scalar_matrix_fixture();
    XrTargetProfile *profile = build_profile(ilp32, false);
    XrTargetPlan *first = build_target_plan(fixture.function->semantic_plan, profile);
    XrTargetPlan *same = build_target_plan(fixture.function->semantic_plan, profile);
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    XrCEmissionPlan *same_emission = NULL;
    char error[512] = {0};
    REQUIRE(xr_c_emission_plan_build(first, profile_fingerprint, &emission, error,
                                     sizeof(error)));
    REQUIRE(xr_c_emission_plan_build(same, profile_fingerprint, &same_emission, error,
                                     sizeof(error)));
    uint32_t expected_count =
        (uint32_t) (sizeof(scalar_known_answers) / sizeof(scalar_known_answers[0]));
    REQUIRE(xr_c_emission_plan_value_count(emission) == expected_count + 1u);
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(first),
                                 xr_target_plan_fingerprint(same)));
    REQUIRE(xr_fingerprint_equal(xr_c_emission_plan_fingerprint(emission),
                                 xr_c_emission_plan_fingerprint(same_emission)));
    for (uint32_t i = 0; i < expected_count; i++)
        require_scalar_known_answer(first, emission, &fixture, i, ilp32);

    XrCValueEmissionView missing = {0};
    REQUIRE(!xr_c_emission_plan_value_view(emission, UINT32_MAX, &missing, error,
                                            sizeof(error)));
    REQUIRE(missing.c_type == NULL);
    REQUIRE(strncmp(error, "XR_TARGET_1001", strlen("XR_TARGET_1001")) == 0);

    xr_c_emission_plan_free(same_emission);
    xr_c_emission_plan_free(emission);
    xr_target_plan_free(same);
    xr_target_plan_free(first);
    xr_target_profile_free(profile);
    xi_func_free(fixture.function);
}

static void test_nullable_scalar_binding_is_rejected(void) {
    XrType nullable_i32 = {
        .kind = XR_KIND_INT,
        .id = 200,
        .frozen = true,
        .is_nullable = true,
        .scalar_rep = XR_NATIVE_I32,
    };
    XrType result_i32 = {
        .kind = XR_KIND_INT,
        .id = 201,
        .frozen = true,
        .scalar_rep = XR_NATIVE_I32,
    };
    XiFunc *function = xi_func_new("nullable_scalar_emission_rejection", &result_i32);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *nullable = xi_const_int(function, entry, 1, &nullable_i32);
    XiValue *result = xi_const_int(function, entry, 2, &result_i32);
    REQUIRE(nullable && result);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(function, error, sizeof(error)));

    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *target_plan = build_target_plan(function->semantic_plan, profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target_plan, xr_target_profile_fingerprint(profile),
                                     &emission, error, sizeof(error)));
    REQUIRE(xr_c_emission_plan_value_count(emission) == 1);
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(target_plan, function, nullable,
                                            &semantic_function, &semantic_value, error,
                                            sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(!xr_c_emission_plan_value_view(emission, semantic_value, &view, error,
                                            sizeof(error)));
    REQUIRE(strncmp(error, "XR_TARGET_1001", strlen("XR_TARGET_1001")) == 0);

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target_plan);
    xr_target_profile_free(profile);
    xi_func_free(function);
}

static void test_aggregate_bindings_are_excluded_from_scalar_projection(void) {
    XrType *elements[2] = {&scalar_int, &scalar_bool};
    XrType tuple = {
        .kind = XR_KIND_TUPLE,
        .id = 300,
        .frozen = true,
        .is_value_type = true,
        .scalar_rep = XR_SCALAR_REP_NONE,
    };
    tuple.tuple.element_types = elements;
    tuple.tuple.element_count = 2;

    XiFunc *function = xi_func_new("aggregate_scalar_projection", &tuple);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *integer = xi_const_int(function, entry, 7, &scalar_int);
    XiValue *boolean = xi_const_bool(function, entry, true, &scalar_bool);
    XiValue *result = xi_value_new(function, entry, XI_TUPLE_NEW, &tuple, 2);
    REQUIRE(integer && boolean && result);
    result->args[0] = integer;
    result->args[1] = boolean;
    result->aux_int = xi_tuple_pack_aux(2, 0);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(function, error, sizeof(error)));
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *target_plan = build_target_plan(function->semantic_plan, profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target_plan, xr_target_profile_fingerprint(profile),
                                     &emission, error, sizeof(error)));
    REQUIRE(xr_c_emission_plan_value_count(emission) == 2);

    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(target_plan, function, result,
                                            &semantic_function, &semantic_value, error,
                                            sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(!xr_c_emission_plan_value_view(emission, semantic_value, &view, error,
                                            sizeof(error)));
    REQUIRE(strncmp(error, "XR_TARGET_1001", strlen("XR_TARGET_1001")) == 0);

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target_plan);
    xr_target_profile_free(profile);
    xi_func_free(function);
}

static void test_profile_mismatch_fails_before_projection(void) {
    ScalarFixture fixture = build_scalar_fixture();
    XrTargetProfile *profile = build_exact_profile();
    XrTargetProfile *different = build_profile(false, true);
    XrTargetPlan *target_plan = build_target_plan(fixture.function->semantic_plan, profile);
    XrCEmissionPlan *emission = NULL;
    char error[512] = {0};
    REQUIRE(!xr_c_emission_plan_build(target_plan,
                                      xr_target_profile_fingerprint(different), &emission,
                                      error, sizeof(error)));
    REQUIRE(emission == NULL);
    REQUIRE(strncmp(error, "XR_TARGET_1000", strlen("XR_TARGET_1000")) == 0);
    REQUIRE(xr_c_emission_plan_build(target_plan, xr_target_profile_fingerprint(profile),
                                     &emission, error, sizeof(error)));

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target_plan);
    xr_target_profile_free(different);
    xr_target_profile_free(profile);
    xi_func_free(fixture.function);
}

static void test_dynamic_closure_c_emission_is_exact_and_mutation_safe(void) {
    XiFunc *root = xi_func_new("dynamic_closure_c_emission", &scalar_unit);
    XiFunc *child = xi_func_new("dynamic_closure_callee", &scalar_int);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(entry != NULL && child_entry != NULL);
    XiValue *child_result = xi_const_int(child, child_entry, 42, &scalar_int);
    REQUIRE(child_result != NULL);
    xi_block_set_return(child_entry, child_result);
    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;
    XiValue *closure =
        xi_value_new(root, entry, XI_CLOSURE_NEW, &dynamic_closure, 0);
    REQUIRE(closure != NULL);
    closure->aux = child;
    xi_block_set_return(entry, NULL);
    root->stage = child->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(root, error, sizeof(error)));
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *target = build_target_plan(root->semantic_plan, profile);
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission,
                                     error, sizeof(error)));
    REQUIRE(xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                      error, sizeof(error)));

    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, root, closure, &semantic_function, &semantic_value, error,
        sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, semantic_value, &view,
                                           error, sizeof(error)));
    REQUIRE(view.rep == XR_C_VALUE_REP_TAGGED &&
            view.target_register_kind == XR_MACHINE_REP_DYN_VALUE &&
            view.target_memory_kind == XR_MACHINE_REP_DYN_VALUE &&
            strcmp(view.c_type, "XrValue") == 0);
    const XrTargetMachineFacts *facts =
        xr_target_profile_machine_facts(profile);
    REQUIRE(facts != NULL &&
            view.register_bits == facts->data_layout.xr_value.size * 8u &&
            view.memory_size == facts->data_layout.xr_value.size &&
            view.memory_align == facts->data_layout.xr_value.align);

    XrCValueEmissionView *tagged = NULL;
    uint32_t tagged_index = UINT32_MAX;
    for (uint32_t i = 0; i < emission->value_count; i++) {
        if (emission->values[i].semantic_value == semantic_value) {
            REQUIRE(tagged == NULL);
            tagged = &emission->values[i];
            tagged_index = i;
        }
    }
    REQUIRE(tagged != NULL && tagged_index != UINT32_MAX &&
            emission->value_count >= 2);

    uint32_t saved_count = emission->value_count;
    XrCValueEmissionView *row_snapshot = (XrCValueEmissionView *) xr_calloc(
        saved_count, sizeof(*row_snapshot));
    REQUIRE(row_snapshot != NULL);
    memcpy(row_snapshot, emission->values,
           saved_count * sizeof(*row_snapshot));
    memmove(&emission->values[tagged_index],
            &emission->values[tagged_index + 1u],
            (saved_count - tagged_index - 1u) * sizeof(*emission->values));
    emission->value_count--;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    REQUIRE(strstr(error, "missing") != NULL);
    emission->value_count = saved_count;
    memcpy(emission->values, row_snapshot,
           saved_count * sizeof(*row_snapshot));
    xr_free(row_snapshot);

    XrCValueEmissionView *saved_rows = emission->values;
    XrCValueEmissionView *extra_rows = (XrCValueEmissionView *) xr_calloc(
        saved_count + 1u, sizeof(*extra_rows));
    REQUIRE(extra_rows != NULL);
    memcpy(extra_rows, saved_rows, saved_count * sizeof(*extra_rows));
    extra_rows[saved_count] = extra_rows[saved_count - 1u];
    extra_rows[saved_count].semantic_value++;
    emission->values = extra_rows;
    emission->value_count++;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    REQUIRE(strstr(error, "extra") != NULL);
    emission->value_count = saved_count;
    emission->values = saved_rows;
    xr_free(extra_rows);
    tagged = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == semantic_value)
            tagged = &emission->values[i];
    REQUIRE(tagged != NULL);

    uint16_t saved_kind = tagged->target_register_kind;
    tagged->target_register_kind = XR_MACHINE_REP_I64;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    tagged->target_register_kind = saved_kind;

    const char *saved_c_type = tagged->c_type;
    tagged->c_type = "int64_t";
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    tagged->c_type = saved_c_type;

    uint8_t saved_rep = tagged->rep;
    tagged->rep = XR_C_VALUE_REP_I64;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    tagged->rep = saved_rep;

    emission->profile_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    REQUIRE(strncmp(error, "XR_TARGET_1000", strlen("XR_TARGET_1000")) == 0);
    emission->profile_fingerprint.bytes[0] ^= 1u;

    emission->target_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    emission->target_fingerprint.bytes[0] ^= 1u;

    emission->fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    REQUIRE(strstr(error, "fingerprint") != NULL);
    emission->fingerprint.bytes[0] ^= 1u;

    uint32_t saved_schema = emission->schema_version;
    emission->schema_version = 6;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    emission->schema_version = saved_schema;
    REQUIRE(xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                      error, sizeof(error)));

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(root);
}

static void test_panic_catch_c_emission_recipe_is_exact(void) {
    XiFunc *function = xi_func_new("panic_catch_c_emission", &scalar_unit);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *caught = xi_value_new(function, entry, XI_CATCH, &dynamic_any, 0);
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &scalar_unit, 1);
    REQUIRE(caught != NULL && release != NULL);
    release->args[0] = caught;
    xi_block_set_return(entry, NULL);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(function, error, sizeof(error)));
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *target = build_target_plan(function->semantic_plan, profile);
    REQUIRE((xr_target_plan_completed_family_mask(target) &
             XR_TARGET_FAMILY_PANIC_CATCH_STORAGE) != 0);
    XrFingerprint profile_fingerprint =
        xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission,
                                     error, sizeof(error)));

    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, function, caught, &semantic_function, &semantic_value, error,
        sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, semantic_value, &view,
                                           error, sizeof(error)));
    REQUIRE(view.rep == XR_C_VALUE_REP_TAGGED &&
            view.materialization == XR_C_VALUE_MATERIALIZATION_PANIC_CATCH &&
            view.target_register_kind == XR_MACHINE_REP_DYN_VALUE &&
            view.target_memory_kind == XR_MACHINE_REP_DYN_VALUE &&
            strcmp(view.c_type, "XrValue") == 0 &&
            view.literal_byte_length == 0 && view.literal_bytes == NULL &&
            view.recipe_operand_value == UINT32_MAX &&
            view.recipe_argument_value == UINT32_MAX &&
            view.recipe_argument_count == 0 &&
            view.recipe_arguments == NULL && view.recipe_symbol == NULL);

    XrTargetValueRepRecord *binding = NULL;
    for (uint32_t i = 0; i < target->value_reps_count; i++)
        if (target->value_reps[i].semantic_value == semantic_value)
            binding = &target->value_reps[i];
    REQUIRE(binding != NULL && binding->slot < target->slots_count);
    uint64_t saved_families = target->completed_family_mask;
    target->completed_family_mask &= ~XR_TARGET_FAMILY_PANIC_CATCH_STORAGE;
    REQUIRE(!xr_target_plan_verify(target, error, sizeof(error)));
    target->completed_family_mask = saved_families;
    XrTargetMachineRepRecord *register_rep =
        &target->machine_reps[binding->register_rep];
    uint8_t saved_ownership = register_rep->ownership;
    register_rep->ownership = XR_TARGET_OWNERSHIP_BORROWED;
    REQUIRE(!xr_target_plan_verify(target, error, sizeof(error)));
    register_rep->ownership = saved_ownership;
    XrTargetSlotRecord *slot = &target->slots[binding->slot];
    uint32_t saved_operation = slot->semantic_operation;
    REQUIRE(saved_operation != XR_SEMANTIC_INDEX_NONE);
    slot->semantic_operation = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(!xr_target_plan_verify(target, error, sizeof(error)));
    slot->semantic_operation = saved_operation;
    REQUIRE(xr_target_plan_verify(target, error, sizeof(error)));

    XrCValueEmissionView *row = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == semantic_value)
            row = &emission->values[i];
    REQUIRE(row != NULL);
    uint8_t saved_recipe = row->materialization;
    row->materialization = XR_C_VALUE_MATERIALIZATION_NONE;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->materialization = saved_recipe;
    uint32_t saved_operand = row->recipe_operand_value;
    row->recipe_operand_value = semantic_value;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->recipe_operand_value = saved_operand;
    REQUIRE(xr_c_emission_plan_verify(emission, target,
                                       profile_fingerprint, error,
                                       sizeof(error)));

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(function);
}

static void test_string_concat_c_emission_recipe_is_exact(void) {
    XiFunc *function = xi_func_new("string_concat_recipe", &scalar_string);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *left = xi_const_str(function, entry, "left", &scalar_string);
    XiValue *right = xi_const_str(function, entry, "right", &scalar_string);
    XiValue *concat =
        xi_value_new(function, entry, XI_STR_CONCAT, &scalar_string, 2);
    REQUIRE(left && right && concat);
    concat->args[0] = left;
    concat->args[1] = right;
    xi_block_set_return(entry, concat);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(function, error, sizeof(error)));
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *target =
        build_target_plan(function->semantic_plan, profile);
    REQUIRE((xr_target_plan_completed_family_mask(target) &
             XR_TARGET_FAMILY_STRING_CONCAT_RESULT_STORAGE) != 0);
    XrFingerprint profile_fingerprint =
        xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission,
                                     error, sizeof(error)));

    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t concat_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t left_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t right_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, function, concat, &semantic_function, &concat_value, error,
        sizeof(error)));
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, function, left, &semantic_function, &left_value, error,
        sizeof(error)));
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, function, right, &semantic_function, &right_value, error,
        sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, concat_value, &view,
                                           error, sizeof(error)));
    REQUIRE(view.rep == XR_C_VALUE_REP_TAGGED &&
            view.materialization ==
                XR_C_VALUE_MATERIALIZATION_STRING_CONCAT &&
            view.recipe_argument_count == 2 && view.recipe_arguments &&
            strcmp(view.recipe_symbol, "xrt_str_concat_parts") == 0);
    REQUIRE(view.recipe_arguments[0].semantic_value == left_value &&
            view.recipe_arguments[1].semantic_value == right_value &&
            view.recipe_arguments[0].kind ==
                XR_C_RECIPE_ARGUMENT_STRING_VALUE &&
            view.recipe_arguments[1].kind ==
                XR_C_RECIPE_ARGUMENT_STRING_VALUE);

    XrCValueEmissionView *concat_row = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++) {
        if (emission->values[i].semantic_value == concat_value) {
            REQUIRE(concat_row == NULL);
            concat_row = &emission->values[i];
        }
    }
    REQUIRE(concat_row != NULL && emission->recipe_argument_count == 2);
    uint32_t saved_value = emission->recipe_arguments[0].semantic_value;
    emission->recipe_arguments[0].semantic_value = right_value;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    emission->recipe_arguments[0].semantic_value = saved_value;

    uint8_t saved_kind = emission->recipe_arguments[1].kind;
    emission->recipe_arguments[1].kind = XR_C_RECIPE_ARGUMENT_INVALID;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    emission->recipe_arguments[1].kind = saved_kind;

    uint16_t saved_count = concat_row->recipe_argument_count;
    concat_row->recipe_argument_count = 1;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    concat_row->recipe_argument_count = saved_count;

    const char *saved_symbol = concat_row->recipe_symbol;
    concat_row->recipe_symbol = "xrt_add";
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    concat_row->recipe_symbol = saved_symbol;
    REQUIRE(xr_c_emission_plan_verify(emission, target,
                                       profile_fingerprint, error,
                                       sizeof(error)));

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(function);
}

static void test_adt_enum_constructor_c_emission_recipe_is_exact(void) {
    static const char *variant_names[] = {"Text"};
    static const char *payload_names[] = {"value"};
    static uint8_t payload_type_ids[] = {XR_TID_STRING};
    int payload_counts[] = {1};
    XrEnumLayout *layout = xr_enum_layout_new(
        "test.aot.scalar", "RecipeValue", variant_names, 1);
    REQUIRE(layout != NULL);
    REQUIRE(xr_enum_layout_set_payload_counts(layout, payload_counts, 1));
    REQUIRE(xr_enum_layout_set_variant_payload_metadata(
        layout, 0, payload_names, payload_type_ids, 1));

    XrType enum_type = {
        .kind = XR_KIND_ENUM,
        .id = layout->layout_id,
        .scalar_rep = XR_SCALAR_REP_NONE,
        .frozen = true,
        .enum_type = {
            .enum_name = "RecipeValue",
            .layout_id = layout->layout_id,
            .layout = layout,
        },
    };
    XrType *payload_types[] = {&scalar_string};
    XiEnumMemberData members[] = {{
        .name = "Text",
        .ordinal = 0,
        .payload_count = 1,
        .payload_names = payload_names,
        .payload_types = payload_types,
    }};
    XiEnumData enum_data = {
        .name = "RecipeValue",
        .member_count = 1,
        .is_adt = true,
        .max_payload = 1,
        .layout_id = layout->layout_id,
        .members = members,
    };
    XiFunc *function = xi_func_new("adt_enum_recipe", &enum_type);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *namespace_value =
        xi_value_new(function, entry, XI_CONST, &enum_type, 0);
    XiValue *payload =
        xi_const_str(function, entry, "payload", &scalar_string);
    XiValue *constructor =
        xi_value_new(function, entry, XI_CALL_METHOD, &enum_type, 2);
    REQUIRE(namespace_value && payload && constructor);
    namespace_value->aux = &enum_data;
    namespace_value->aux_kind = XI_AUX_KIND_ENUM_NAMESPACE;
    constructor->args[0] = namespace_value;
    constructor->args[1] = payload;
    constructor->aux = "Text";
    constructor->flags = XI_FLAG_CALL_EFFECTS;
    constructor->call_return_ownership.kind = XI_RETURN_OWNERSHIP_OWNED;
    constructor->call_return_ownership.param_index = -1;
    constructor->call_return_ownership.complete = true;
    xi_block_set_return(entry, constructor);
    function->arc_return_ownership.kind = XI_RETURN_OWNERSHIP_OWNED;
    function->arc_return_ownership.param_index = -1;
    function->arc_return_ownership.complete = true;
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    bool semantic_built =
        xr_semantic_plan_build_and_attach(function, error, sizeof(error));
    if (!semantic_built)
        fprintf(stderr, "ADT enum semantic plan failed: %s\n", error);
    REQUIRE(semantic_built && function->semantic_plan != NULL);
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *target = build_target_plan(function->semantic_plan, profile);
    REQUIRE((xr_target_plan_completed_family_mask(target) &
             XR_TARGET_FAMILY_ADT_ENUM_STORAGE) != 0);
    XrFingerprint profile_fingerprint =
        xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission,
                                     error, sizeof(error)));

    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t constructor_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t namespace_semantic_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t payload_semantic_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, function, constructor, &semantic_function,
        &constructor_value, error, sizeof(error)));
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, function, namespace_value, &semantic_function,
        &namespace_semantic_value, error, sizeof(error)));
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, function, payload, &semantic_function,
        &payload_semantic_value, error, sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(
        emission, constructor_value, &view, error, sizeof(error)));
    REQUIRE(view.rep == XR_C_VALUE_REP_TAGGED &&
            view.materialization ==
                XR_C_VALUE_MATERIALIZATION_ADT_ENUM_CONSTRUCTOR &&
            view.recipe_operand_value == namespace_semantic_value &&
            view.recipe_layout_id == layout->layout_id &&
            view.recipe_discriminant == 0 &&
            view.recipe_argument_count == 1 && view.recipe_arguments &&
            view.recipe_arguments[0].semantic_value == payload_semantic_value &&
            view.recipe_arguments[0].kind ==
                XR_C_RECIPE_ARGUMENT_ENUM_PAYLOAD &&
            strcmp(view.recipe_symbol, "xrt_enum_aggregate_box") == 0 &&
            strcmp(view.recipe_type_name, "RecipeValue") == 0 &&
            strcmp(view.recipe_member_name, "Text") == 0);

    XrCValueEmissionView *row = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++) {
        if (emission->values[i].semantic_value == constructor_value) {
            REQUIRE(row == NULL);
            row = &emission->values[i];
        }
    }
    REQUIRE(row != NULL && emission->recipe_argument_count == 1);
    uint32_t saved_layout = row->recipe_layout_id;
    row->recipe_layout_id++;
    REQUIRE(!xr_c_emission_plan_verify(
        emission, target, profile_fingerprint, error, sizeof(error)));
    row->recipe_layout_id = saved_layout;

    const char *saved_member = row->recipe_member_name;
    row->recipe_member_name = "Other";
    REQUIRE(!xr_c_emission_plan_verify(
        emission, target, profile_fingerprint, error, sizeof(error)));
    row->recipe_member_name = saved_member;

    uint32_t saved_payload = emission->recipe_arguments[0].semantic_value;
    emission->recipe_arguments[0].semantic_value = namespace_semantic_value;
    REQUIRE(!xr_c_emission_plan_verify(
        emission, target, profile_fingerprint, error, sizeof(error)));
    emission->recipe_arguments[0].semantic_value = saved_payload;
    REQUIRE(xr_c_emission_plan_verify(
        emission, target, profile_fingerprint, error, sizeof(error)));

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(function);
    xr_enum_layout_free(layout);
}

static void test_channel_new_c_emission_recipe_is_exact(void) {
    XiFunc *root = xi_func_new("channel_recipe", &scalar_unit);
    REQUIRE(root != NULL);
    XiBlock *entry = xi_block_new(root);
    REQUIRE(entry != NULL);
    XiValue *capacity = xi_const_int(root, entry, 1, &scalar_int);
    XiValue *channel = xi_value_new(root, entry, XI_CHAN_NEW,
                                    &channel_type, 1);
    REQUIRE(capacity && channel);
    channel->args[0] = capacity;
    xi_block_set_return(entry, NULL);
    root->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(root, error, sizeof(error)));
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *target = build_target_plan(root->semantic_plan, profile);
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission,
                                     error, sizeof(error)));
    uint32_t channel_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t channel_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t capacity_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t capacity_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(target, root, channel,
                                             &channel_function,
                                             &channel_value, error,
                                             sizeof(error)));
    REQUIRE(xr_aot_scalar_semantic_value_id(target, root, capacity,
                                             &capacity_function,
                                             &capacity_value, error,
                                             sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, channel_value, &view,
                                           error, sizeof(error)));
    REQUIRE(view.rep == XR_C_VALUE_REP_TAGGED &&
            view.materialization == XR_C_VALUE_MATERIALIZATION_CHANNEL_NEW &&
            view.recipe_operand_value == capacity_value &&
            view.recipe_symbol &&
            strcmp(view.recipe_symbol, "xr_aot_channel_new") == 0 &&
            view.literal_bytes == NULL && view.literal_byte_length == 0);
    XrCValueEmissionView *row = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == channel_value)
            row = &emission->values[i];
    REQUIRE(row != NULL);
    uint8_t saved_recipe = row->materialization;
    row->materialization = XR_C_VALUE_MATERIALIZATION_NONE;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->materialization = saved_recipe;
    uint32_t saved_operand = row->recipe_operand_value;
    row->recipe_operand_value++;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->recipe_operand_value = saved_operand;
    const char *saved_symbol = row->recipe_symbol;
    row->recipe_symbol = "xr_aot_channel_New";
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->recipe_symbol = saved_symbol;
    REQUIRE(xr_c_emission_plan_verify(emission, target,
                                       profile_fingerprint, error,
                                       sizeof(error)));
    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(root);
}

static void test_stringbuilder_new_c_emission_recipe_is_exact(void) {
    XiFunc *root = xi_func_new("stringbuilder_recipe", &scalar_unit);
    REQUIRE(root != NULL);
    XiBlock *entry = xi_block_new(root);
    REQUIRE(entry != NULL);
    XiValue *builder =
        xi_value_new(root, entry, XI_CALL_BUILTIN, &stringbuilder_type, 0);
    REQUIRE(builder != NULL);
    builder->aux = (void *) "StringBuilder";
    XiValue *release = xi_value_new(root, entry, XI_RELEASE, &scalar_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = builder;
    xi_block_set_return(entry, NULL);
    root->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(root, error, sizeof(error)));
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *target = build_target_plan(root->semantic_plan, profile);
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission,
                                     error, sizeof(error)));
    uint32_t ignored_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t builder_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, root, builder, &ignored_function, &builder_value, error,
        sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, builder_value, &view,
                                           error, sizeof(error)));
    REQUIRE(view.rep == XR_C_VALUE_REP_TAGGED &&
            view.materialization ==
                XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_NEW &&
            view.recipe_operand_value == UINT32_MAX && view.recipe_symbol &&
            strcmp(view.recipe_symbol, "xrt_strbuf_new") == 0 &&
            view.literal_bytes == NULL && view.literal_byte_length == 0);
    XrCValueEmissionView *row = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == builder_value)
            row = &emission->values[i];
    REQUIRE(row != NULL);
    uint8_t saved_recipe = row->materialization;
    row->materialization = XR_C_VALUE_MATERIALIZATION_NONE;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->materialization = saved_recipe;
    uint32_t saved_operand = row->recipe_operand_value;
    row->recipe_operand_value = builder_value;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->recipe_operand_value = saved_operand;
    const char *saved_symbol = row->recipe_symbol;
    row->recipe_symbol = "xrt_strbuf_New";
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->recipe_symbol = saved_symbol;
    REQUIRE(xr_c_emission_plan_verify(emission, target,
                                       profile_fingerprint, error,
                                       sizeof(error)));
    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(root);
}

static void test_string_runes_c_emission_recipe_is_exact(void) {
    XiFunc *root = xi_func_new("string_runes_recipe", &scalar_unit);
    REQUIRE(root != NULL);
    XiBlock *entry = xi_block_new(root);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(root, entry, "authority", &scalar_string);
    XiValue *runes = xi_value_new(root, entry, XI_CALL_METHOD,
                                  &iterator_rune_type, 1);
    REQUIRE(source != NULL && runes != NULL);
    runes->args[0] = source;
    runes->aux = (void *) "runes";
    runes->aux_int = 470;
    XiValue *release = xi_value_new(root, entry, XI_RELEASE, &scalar_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = runes;
    xi_block_set_return(entry, NULL);
    root->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(root, error, sizeof(error)));
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *target = build_target_plan(root->semantic_plan, profile);
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic refinement_diag = {0};
    XrAotRefinementPlan *refinement = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        target, &policy, &refinement, &refinement_diag));
    XrAotRefinementPlanView refinement_view =
        xr_aot_refinement_plan_view(refinement);
    REQUIRE(refinement_view.frozen && refinement_view.verified &&
            refinement_view.record_count == 0);
    xr_aot_refinement_plan_free(refinement);
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission,
                                     error, sizeof(error)));
    uint32_t ignored_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t source_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t runes_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, root, source, &ignored_function, &source_value, error,
        sizeof(error)));
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, root, runes, &ignored_function, &runes_value, error,
        sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, runes_value, &view,
                                          error, sizeof(error)));
    REQUIRE(view.rep == XR_C_VALUE_REP_TAGGED &&
            view.target_register_kind == XR_MACHINE_REP_DYN_VALUE &&
            view.target_memory_kind == XR_MACHINE_REP_DYN_VALUE &&
            view.materialization == XR_C_VALUE_MATERIALIZATION_STRING_RUNES &&
            view.recipe_operand_value == source_value && view.recipe_symbol &&
            strcmp(view.recipe_symbol, "xrt_string_runes") == 0 &&
            view.literal_bytes == NULL && view.literal_byte_length == 0);

    XrCValueEmissionView *row = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == runes_value)
            row = &emission->values[i];
    REQUIRE(row != NULL);
    uint8_t saved_recipe = row->materialization;
    row->materialization = XR_C_VALUE_MATERIALIZATION_NONE;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->materialization = saved_recipe;
    uint32_t saved_operand = row->recipe_operand_value;
    row->recipe_operand_value = runes_value;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->recipe_operand_value = saved_operand;
    const char *saved_symbol = row->recipe_symbol;
    row->recipe_symbol = "xrt_string_Runes";
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->recipe_symbol = saved_symbol;
    uint16_t saved_kind = row->target_register_kind;
    row->target_register_kind = XR_MACHINE_REP_OBJECT_REF;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->target_register_kind = saved_kind;
    REQUIRE(xr_c_emission_plan_verify(emission, target,
                                       profile_fingerprint, error,
                                       sizeof(error)));

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(root);
}

static void test_string_slice_range_c_emission_recipe_is_exact(void) {
    XiFunc *root = xi_func_new("string_slice_range_recipe", &scalar_unit);
    REQUIRE(root != NULL);
    XiBlock *entry = xi_block_new(root);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(root, entry, "authority", &scalar_string);
    XiValue *start = xi_const_int(root, entry, 1, &scalar_int);
    XiValue *end = xi_const_int(root, entry, 4, &scalar_int);
    XiValue *slice = xi_value_new(root, entry, XI_CALL_METHOD,
                                  &scalar_string, 3);
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
    XiValue *release = xi_value_new(root, entry, XI_RELEASE, &scalar_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = slice;
    xi_block_set_return(entry, NULL);
    root->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(root, error, sizeof(error)));
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *target = build_target_plan(root->semantic_plan, profile);
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic refinement_diag = {0};
    XrAotRefinementPlan *refinement = NULL;
    bool refined = xr_aot_representation_refinement_build_from_authority(
        target, &policy, &refinement, &refinement_diag);
    if (!refined)
        fprintf(stderr,
                "String.slice refinement failed: issue=%u value=%u operation=%u call=%u\n",
                refinement_diag.issue, refinement_diag.semantic_value,
                refinement_diag.semantic_operation,
                refinement_diag.target_call_index);
    REQUIRE(refined);
    xr_aot_refinement_plan_free(refinement);
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission,
                                     error, sizeof(error)));
    uint32_t ignored_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t source_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t start_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t end_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t slice_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(target, root, source,
                                             &ignored_function, &source_value,
                                             error, sizeof(error)));
    REQUIRE(xr_aot_scalar_semantic_value_id(target, root, start,
                                             &ignored_function, &start_value,
                                             error, sizeof(error)));
    REQUIRE(xr_aot_scalar_semantic_value_id(target, root, end,
                                             &ignored_function, &end_value,
                                             error, sizeof(error)));
    REQUIRE(xr_aot_scalar_semantic_value_id(target, root, slice,
                                             &ignored_function, &slice_value,
                                             error, sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, slice_value, &view,
                                          error, sizeof(error)));
    REQUIRE(view.rep == XR_C_VALUE_REP_TAGGED &&
            view.target_register_kind == XR_MACHINE_REP_DYN_VALUE &&
            view.target_memory_kind == XR_MACHINE_REP_DYN_VALUE &&
            view.materialization ==
                XR_C_VALUE_MATERIALIZATION_STRING_SLICE_RANGE &&
            view.recipe_operand_value == source_value && view.recipe_symbol &&
            strcmp(view.recipe_symbol, "xrt_string_slice_range") == 0 &&
            view.recipe_argument_count == 2 && view.recipe_arguments &&
            view.recipe_arguments[0].semantic_value == start_value &&
            view.recipe_arguments[1].semantic_value == end_value &&
            view.recipe_arguments[0].kind ==
                XR_C_RECIPE_ARGUMENT_STRING_SLICE_BOUND &&
            view.recipe_arguments[1].kind ==
                XR_C_RECIPE_ARGUMENT_STRING_SLICE_BOUND);
    XrCValueEmissionView *row = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == slice_value)
            row = &emission->values[i];
    REQUIRE(row && emission->recipe_argument_count == 2);
    uint8_t saved_materialization = row->materialization;
    row->materialization = XR_C_VALUE_MATERIALIZATION_STRING_RUNES;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->materialization = saved_materialization;
    uint32_t saved_operand = row->recipe_operand_value;
    row->recipe_operand_value = slice_value;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->recipe_operand_value = saved_operand;
    const char *saved_symbol = row->recipe_symbol;
    row->recipe_symbol = "xrt_string_slice";
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->recipe_symbol = saved_symbol;
    uint32_t saved_bound = emission->recipe_arguments[0].semantic_value;
    emission->recipe_arguments[0].semantic_value = end_value;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    emission->recipe_arguments[0].semantic_value = saved_bound;
    uint8_t saved_kind = emission->recipe_arguments[1].kind;
    emission->recipe_arguments[1].kind = XR_C_RECIPE_ARGUMENT_INVALID;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    emission->recipe_arguments[1].kind = saved_kind;
    uint16_t saved_count = row->recipe_argument_count;
    row->recipe_argument_count = 1;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->recipe_argument_count = saved_count;
    REQUIRE(xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));

    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(root);
}

static void test_iterator_rune_has_next_c_emission_recipe_is_exact(void) {
    XiFunc *root = xi_func_new("iterator_rune_has_next_recipe", &scalar_unit);
    REQUIRE(root != NULL);
    XiBlock *entry = xi_block_new(root);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(root, entry, "authority", &scalar_string);
    XiValue *runes = xi_value_new(root, entry, XI_CALL_METHOD,
                                  &iterator_rune_type, 1);
    XiValue *has_next = xi_value_new(root, entry, XI_CALL_METHOD,
                                     &scalar_bool, 1);
    REQUIRE(source && runes && has_next);
    runes->args[0] = source;
    runes->aux = (void *) "runes";
    runes->aux_int = 470;
    has_next->args[0] = runes;
    has_next->aux = (void *) "hasNext";
    has_next->aux_int = 112;
    XiValue *release = xi_value_new(root, entry, XI_RELEASE, &scalar_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = runes;
    xi_block_set_return(entry, NULL);
    root->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(root, error, sizeof(error)));
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *target = build_target_plan(root->semantic_plan, profile);
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic refinement_diag = {0};
    XrAotRefinementPlan *refinement = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        target, &policy, &refinement, &refinement_diag));
    xr_aot_refinement_plan_free(refinement);
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission,
                                     error, sizeof(error)));
    uint32_t ignored_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t runes_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t has_next_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, root, runes, &ignored_function, &runes_value, error,
        sizeof(error)));
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, root, has_next, &ignored_function, &has_next_value, error,
        sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, has_next_value, &view,
                                          error, sizeof(error)));
    REQUIRE(view.rep == XR_C_VALUE_REP_BOOL &&
            view.target_register_kind == XR_MACHINE_REP_I1 &&
            view.target_memory_kind == XR_MACHINE_REP_I1 &&
            view.materialization ==
                XR_C_VALUE_MATERIALIZATION_ITERATOR_RUNE_HAS_NEXT &&
            view.recipe_operand_value == runes_value && view.recipe_symbol &&
            strcmp(view.recipe_symbol, "xrt_iterator_rune_has_next") == 0);
    XrCValueEmissionView *row = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == has_next_value)
            row = &emission->values[i];
    REQUIRE(row != NULL);
    uint8_t saved_recipe = row->materialization;
    row->materialization = XR_C_VALUE_MATERIALIZATION_NONE;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->materialization = saved_recipe;
    uint32_t saved_operand = row->recipe_operand_value;
    row->recipe_operand_value = has_next_value;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->recipe_operand_value = saved_operand;
    const char *saved_symbol = row->recipe_symbol;
    row->recipe_symbol = "xrt_iterator_has_next";
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->recipe_symbol = saved_symbol;
    REQUIRE(xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(root);
}

static void test_iterator_rune_next_c_emission_recipe_is_exact(void) {
    XiFunc *root = xi_func_new("iterator_rune_next_recipe", &scalar_unit);
    REQUIRE(root != NULL);
    XiBlock *entry = xi_block_new(root);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(root, entry, "authority", &scalar_string);
    XiValue *runes = xi_value_new(root, entry, XI_CALL_METHOD,
                                  &iterator_rune_type, 1);
    XiValue *next = xi_value_new(root, entry, XI_CALL_METHOD, &rune_type, 1);
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
    XiValue *release = xi_value_new(root, entry, XI_RELEASE, &scalar_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = runes;
    xi_block_set_return(entry, NULL);
    root->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(root, error, sizeof(error)));
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *target = build_target_plan(root->semantic_plan, profile);
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic refinement_diag = {0};
    XrAotRefinementPlan *refinement = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        target, &policy, &refinement, &refinement_diag));
    xr_aot_refinement_plan_free(refinement);
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission,
                                     error, sizeof(error)));
    uint32_t ignored_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t runes_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t next_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, root, runes, &ignored_function, &runes_value, error,
        sizeof(error)));
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, root, next, &ignored_function, &next_value, error,
        sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, next_value, &view,
                                          error, sizeof(error)));
    REQUIRE(view.rep == XR_C_VALUE_REP_RUNE &&
            view.target_register_kind == XR_MACHINE_REP_RUNE &&
            view.target_memory_kind == XR_MACHINE_REP_RUNE &&
            view.materialization == XR_C_VALUE_MATERIALIZATION_ITERATOR_RUNE_NEXT &&
            view.recipe_operand_value == runes_value && view.recipe_symbol &&
            strcmp(view.recipe_symbol, "xrt_iterator_rune_next") == 0);
    XrCValueEmissionView *row = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == next_value)
            row = &emission->values[i];
    REQUIRE(row != NULL);
    uint8_t saved_recipe = row->materialization;
    row->materialization = XR_C_VALUE_MATERIALIZATION_ITERATOR_RUNE_HAS_NEXT;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->materialization = saved_recipe;
    uint32_t saved_operand = row->recipe_operand_value;
    row->recipe_operand_value = next_value;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->recipe_operand_value = saved_operand;
    const char *saved_symbol = row->recipe_symbol;
    row->recipe_symbol = "xrt_iterator_next";
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->recipe_symbol = saved_symbol;
    uint16_t saved_kind = row->target_register_kind;
    row->target_register_kind = XR_MACHINE_REP_I1;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->target_register_kind = saved_kind;
    REQUIRE(xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(root);
}

static void test_rune_to_uint32_c_emission_recipe_is_exact(void) {
    XiFunc *root = xi_func_new("rune_to_uint32_recipe", &scalar_unit);
    REQUIRE(root != NULL);
    XiBlock *entry = xi_block_new(root);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(root, entry, "authority", &scalar_string);
    XiValue *runes = xi_value_new(root, entry, XI_CALL_METHOD,
                                  &iterator_rune_type, 1);
    XiValue *next = xi_value_new(root, entry, XI_CALL_METHOD, &rune_type, 1);
    XiValue *to_u32 = xi_value_new(root, entry, XI_CALL_METHOD, &u32_type, 1);
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
    XiValue *release = xi_value_new(root, entry, XI_RELEASE, &scalar_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = runes;
    xi_block_set_return(entry, NULL);
    root->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(root, error, sizeof(error)));
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *target = build_target_plan(root->semantic_plan, profile);
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic refinement_diag = {0};
    XrAotRefinementPlan *refinement = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        target, &policy, &refinement, &refinement_diag));
    xr_aot_refinement_plan_free(refinement);
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission,
                                     error, sizeof(error)));
    uint32_t ignored_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t next_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t to_u32_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, root, next, &ignored_function, &next_value, error,
        sizeof(error)));
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, root, to_u32, &ignored_function, &to_u32_value, error,
        sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, to_u32_value, &view,
                                          error, sizeof(error)));
    REQUIRE(view.rep == XR_C_VALUE_REP_U32 &&
            view.target_register_kind == XR_MACHINE_REP_U32 &&
            view.target_memory_kind == XR_MACHINE_REP_U32 &&
            view.materialization == XR_C_VALUE_MATERIALIZATION_RUNE_TO_UINT32 &&
            view.recipe_operand_value == next_value && view.recipe_symbol &&
            strcmp(view.recipe_symbol, "xrt_rune_to_uint32") == 0);
    XrCValueEmissionView *row = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == to_u32_value)
            row = &emission->values[i];
    REQUIRE(row != NULL);
    uint8_t saved_recipe = row->materialization;
    row->materialization = XR_C_VALUE_MATERIALIZATION_ITERATOR_RUNE_NEXT;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->materialization = saved_recipe;
    uint32_t saved_operand = row->recipe_operand_value;
    row->recipe_operand_value = to_u32_value;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->recipe_operand_value = saved_operand;
    const char *saved_symbol = row->recipe_symbol;
    row->recipe_symbol = "xrt_rune_to_int";
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->recipe_symbol = saved_symbol;
    REQUIRE(xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(root);
}

static void test_rune_is_whitespace_c_emission_recipe_is_exact(void) {
    XiFunc *root = xi_func_new("rune_is_whitespace_recipe", &scalar_unit);
    REQUIRE(root != NULL);
    XiBlock *entry = xi_block_new(root);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(root, entry, "authority", &scalar_string);
    XiValue *runes = xi_value_new(root, entry, XI_CALL_METHOD,
                                  &iterator_rune_type, 1);
    XiValue *next = xi_value_new(root, entry, XI_CALL_METHOD, &rune_type, 1);
    XiValue *is_whitespace = xi_value_new(root, entry, XI_CALL_METHOD,
                                          &scalar_bool, 1);
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
    XiValue *release = xi_value_new(root, entry, XI_RELEASE, &scalar_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = runes;
    xi_block_set_return(entry, NULL);
    root->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(root, error, sizeof(error)));
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *target = build_target_plan(root->semantic_plan, profile);
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic refinement_diag = {0};
    XrAotRefinementPlan *refinement = NULL;
    REQUIRE(xr_aot_representation_refinement_build_from_authority(
        target, &policy, &refinement, &refinement_diag));
    xr_aot_refinement_plan_free(refinement);
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission,
                                     error, sizeof(error)));
    uint32_t ignored_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t next_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t predicate_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, root, next, &ignored_function, &next_value, error,
        sizeof(error)));
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, root, is_whitespace, &ignored_function, &predicate_value,
        error, sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, predicate_value, &view,
                                          error, sizeof(error)));
    REQUIRE(view.rep == XR_C_VALUE_REP_BOOL &&
            view.target_register_kind == XR_MACHINE_REP_I1 &&
            view.target_memory_kind == XR_MACHINE_REP_I1 &&
            view.materialization ==
                XR_C_VALUE_MATERIALIZATION_RUNE_IS_WHITESPACE &&
            view.recipe_operand_value == next_value && view.recipe_symbol &&
            strcmp(view.recipe_symbol, "xrt_rune_is_whitespace") == 0);
    XrCValueEmissionView *row = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == predicate_value)
            row = &emission->values[i];
    REQUIRE(row != NULL);
    uint8_t saved_recipe = row->materialization;
    row->materialization = XR_C_VALUE_MATERIALIZATION_RUNE_TO_UINT32;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->materialization = saved_recipe;
    uint32_t saved_operand = row->recipe_operand_value;
    row->recipe_operand_value = predicate_value;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->recipe_operand_value = saved_operand;
    const char *saved_symbol = row->recipe_symbol;
    row->recipe_symbol = "xrt_rune_is_space";
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->recipe_symbol = saved_symbol;
    REQUIRE(xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                       error, sizeof(error)));
    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(root);
}

static void test_string_byte_slice_view_c_emission_recipe_is_exact(void) {
    XiFunc *root = xi_func_new("string_byte_slice_view_recipe", &scalar_unit);
    REQUIRE(root != NULL);
    XiBlock *entry = xi_block_new(root);
    REQUIRE(entry != NULL);
    XiValue *source = xi_const_str(root, entry, "authority", &scalar_string);
    XiValue *view = xi_value_new(root, entry, XI_CALL_BUILTIN,
                                 &byte_slice_type, 1);
    REQUIRE(source && view);
    view->args[0] = source;
    view->xa_intrinsic_id = XA_INTRINSIC_STRING_BYTE_SLICE_VIEW;
    view->view_evidence = (XiViewEvidence) {
        .root_value_id = source->id,
        .element_type_id = byte_type.id,
        .source_operand = 0,
        .source_param = -1,
        .origin = XI_VIEW_ORIGIN_RECEIVER,
        .capability = 1,
        .lifetime = 1,
        .complete = 1,
    };
    xi_block_set_return(entry, NULL);
    root->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(root, error, sizeof(error)));
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *target = build_target_plan(root->semantic_plan, profile);
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission,
                                     error, sizeof(error)));
    uint32_t ignored_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t source_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t view_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(target, root, source,
                                             &ignored_function, &source_value,
                                             error, sizeof(error)));
    REQUIRE(xr_aot_scalar_semantic_value_id(target, root, view,
                                             &ignored_function, &view_value,
                                             error, sizeof(error)));
    XrCValueEmissionView frozen = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, view_value, &frozen,
                                           error, sizeof(error)));
    REQUIRE(frozen.rep == XR_C_VALUE_REP_VIEW &&
            frozen.target_register_kind == XR_MACHINE_REP_VIEW &&
            frozen.target_memory_kind == XR_MACHINE_REP_VIEW &&
            frozen.materialization ==
                XR_C_VALUE_MATERIALIZATION_STRING_BYTE_SLICE_VIEW &&
            frozen.recipe_operand_value == source_value && frozen.recipe_symbol &&
            strcmp(frozen.recipe_symbol, "xrt_span_from_string_bytes") == 0 &&
            frozen.literal_bytes == NULL && frozen.literal_byte_length == 0 &&
            strcmp(frozen.c_type, "xr_span_t") == 0);

    XrCValueEmissionView *row = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == view_value)
            row = &emission->values[i];
    REQUIRE(row != NULL);
    uint8_t saved_recipe = row->materialization;
    row->materialization = XR_C_VALUE_MATERIALIZATION_NONE;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->materialization = saved_recipe;
    uint32_t saved_operand = row->recipe_operand_value;
    row->recipe_operand_value = view_value;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->recipe_operand_value = saved_operand;
    const char *saved_symbol = row->recipe_symbol;
    row->recipe_symbol = "xrt_span_from_string_Bytes";
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->recipe_symbol = saved_symbol;
    uint8_t saved_rep = row->rep;
    row->rep = XR_C_VALUE_REP_TAGGED;
    REQUIRE(!xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                        error, sizeof(error)));
    row->rep = saved_rep;
    REQUIRE(xr_c_emission_plan_verify(emission, target, profile_fingerprint,
                                      error, sizeof(error)));
    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(root);
}

static void test_channel_receive_c_emission_recipe_is_exact(void) {
    XiFunc *root = xi_func_new("channel_receive_recipe", &scalar_unit);
    REQUIRE(root != NULL);
    XiBlock *entry = xi_block_new(root);
    REQUIRE(entry != NULL);
    XiValue *capacity = xi_const_int(root, entry, 1, &scalar_int);
    XiValue *channel = xi_value_new(root, entry, XI_CHAN_NEW,
                                    &channel_type, 1);
    XiValue *receive = xi_value_new(root, entry, XI_CHAN_TRY_RECV,
                                    &scalar_int, 1);
    REQUIRE(capacity && channel && receive);
    channel->args[0] = capacity;
    receive->args[0] = channel;
    xi_block_set_return(entry, NULL);
    root->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(root, error, sizeof(error)));
    XrTargetProfile *profile = build_exact_profile();
    XrTargetPlan *target = build_target_plan(root->semantic_plan, profile);
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    XrCEmissionPlan *emission = NULL;
    REQUIRE(xr_c_emission_plan_build(target, profile_fingerprint, &emission,
                                     error, sizeof(error)));
    uint32_t ignored_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t channel_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t receive_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, root, channel, &ignored_function, &channel_value, error,
        sizeof(error)));
    REQUIRE(xr_aot_scalar_semantic_value_id(
        target, root, receive, &ignored_function, &receive_value, error,
        sizeof(error)));
    XrCValueEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_value_view(emission, receive_value, &view,
                                           error, sizeof(error)));
    REQUIRE(view.rep == XR_C_VALUE_REP_I64 &&
            view.materialization ==
                XR_C_VALUE_MATERIALIZATION_CHANNEL_RECV_PAYLOAD &&
            view.recipe_operand_value == channel_value && view.recipe_symbol &&
            strcmp(view.recipe_symbol, "XR_TO_INT") == 0 &&
            view.literal_bytes == NULL && view.literal_byte_length == 0);
    XrCValueEmissionView *row = NULL;
    for (uint32_t i = 0; i < emission->value_count; i++)
        if (emission->values[i].semantic_value == receive_value)
            row = &emission->values[i];
    REQUIRE(row != NULL);
    uint8_t saved_recipe = row->materialization;
    row->materialization = XR_C_VALUE_MATERIALIZATION_NONE;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->materialization = saved_recipe;
    uint32_t saved_operand = row->recipe_operand_value;
    row->recipe_operand_value = receive_value;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->recipe_operand_value = saved_operand;
    const char *saved_symbol = row->recipe_symbol;
    row->recipe_symbol = "XR_TO_FLOAT";
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->recipe_symbol = saved_symbol;
    uint16_t saved_kind = row->target_register_kind;
    row->target_register_kind = XR_MACHINE_REP_F64;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    row->target_register_kind = saved_kind;
    emission->fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    emission->fingerprint.bytes[0] ^= 1u;
    uint32_t saved_schema = emission->schema_version;
    emission->schema_version = 7;
    REQUIRE(!xr_c_emission_plan_verify(emission, target,
                                        profile_fingerprint, error,
                                        sizeof(error)));
    emission->schema_version = saved_schema;
    REQUIRE(xr_c_emission_plan_verify(emission, target,
                                       profile_fingerprint, error,
                                       sizeof(error)));
    xr_c_emission_plan_free(emission);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(root);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "string-slice-range-emission") == 0) {
        test_string_slice_range_c_emission_recipe_is_exact();
        printf("String range-slice C emission authority tests passed\n");
        return 0;
    }
    test_scalar_plan_and_emission_view();
    test_missing_semantic_authority_fails_closed();
    test_cross_function_value_substitution_fails_closed();
    test_parameter_identity_requires_exact_member();
    test_all_scalar_c_spelling_known_answers(false);
    test_all_scalar_c_spelling_known_answers(true);
    test_nullable_scalar_binding_is_rejected();
    test_aggregate_bindings_are_excluded_from_scalar_projection();
    test_profile_mismatch_fails_before_projection();
    test_dynamic_closure_c_emission_is_exact_and_mutation_safe();
    test_panic_catch_c_emission_recipe_is_exact();
    test_string_concat_c_emission_recipe_is_exact();
    test_adt_enum_constructor_c_emission_recipe_is_exact();
    test_channel_new_c_emission_recipe_is_exact();
    test_stringbuilder_new_c_emission_recipe_is_exact();
    test_string_runes_c_emission_recipe_is_exact();
    test_string_slice_range_c_emission_recipe_is_exact();
    test_iterator_rune_has_next_c_emission_recipe_is_exact();
    test_iterator_rune_next_c_emission_recipe_is_exact();
    test_rune_to_uint32_c_emission_recipe_is_exact();
    test_rune_is_whitespace_c_emission_recipe_is_exact();
    test_string_byte_slice_view_c_emission_recipe_is_exact();
    test_channel_receive_c_emission_recipe_is_exact();
    printf("AOT scalar TargetPlan tests passed\n");
    return 0;
}
