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
#error "scalar C emission plan must not include the legacy XaotValueRep model"
#endif
#ifdef XI_H
#error "scalar C emission plan must not include compiler IR pointers"
#endif
#ifdef XR_SEMANTIC_PLAN_H
#error "scalar C emission plan must not include SemanticPlan"
#endif
#ifdef XTYPE_H
#error "scalar C emission plan must not include analyzer/runtime type objects"
#endif
#include "../../../src/aot/refine/xr_aot_scalar_value.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_profile.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/runtime/value/xtype.h"
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

static XrTargetProfile *build_profile(bool ilp32, uint8_t fingerprint_seed) {
    XrTargetProfileDraft draft = {0};
    draft.schema_version = XR_TARGET_PROFILE_SCHEMA_VERSION;
    draft.architecture = ilp32 ? XR_TARGET_ARCH_WASM32 : XR_TARGET_ARCH_X86_64;
    draft.operating_system = ilp32 ? XR_TARGET_OS_WASI : XR_TARGET_OS_WINDOWS;
    draft.environment = ilp32 ? XR_TARGET_ENV_WASI : XR_TARGET_ENV_MSVC;
    draft.native_abi = ilp32 ? XR_TARGET_ABI_WASM : XR_TARGET_ABI_WIN64_X86_64;
    draft.runtime_profile = XR_TARGET_RUNTIME_PROFILE_HOSTED;
    REQUIRE(ilp32 ? xr_target_data_layout_init_ilp32(&draft.data_layout)
                  : xr_target_data_layout_init_lp64(&draft.data_layout));
    draft.atomic_width_mask = XR_TARGET_ATOMIC_WIDTH_8 | XR_TARGET_ATOMIC_WIDTH_16 |
                              XR_TARGET_ATOMIC_WIDTH_32 | XR_TARGET_ATOMIC_WIDTH_64;
    draft.atomic_order_mask = XR_TARGET_ATOMIC_RELAXED | XR_TARGET_ATOMIC_ACQUIRE |
                              XR_TARGET_ATOMIC_RELEASE | XR_TARGET_ATOMIC_ACQ_REL |
                              XR_TARGET_ATOMIC_SEQ_CST;
    draft.float_feature_mask = XR_TARGET_FLOAT_IEEE754 | XR_TARGET_FLOAT_STRICT;
    draft.vector_feature_mask = ilp32 ? XR_TARGET_VECTOR_WASM128 : XR_TARGET_VECTOR_SSE2;
    draft.maximum_vector_bits = 128;
    draft.provider_mask = XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_ALLOCATOR) |
                          XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_PANIC);
    draft.provider_set_fingerprint.bytes[0] = fingerprint_seed;
    draft.object_header_fingerprint.bytes[0] = (uint8_t) (fingerprint_seed + 1u);
    draft.runtime_abi_fingerprint.bytes[0] = (uint8_t) (fingerprint_seed + 2u);
    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_profile_freeze(&draft, &profile, error, sizeof(error)));
    return profile;
}

static XrTargetProfile *build_exact_profile(void) {
    return build_profile(false, 0x11);
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
    REQUIRE(xr_target_plan_build(semantic_plan, profile, &plan, error, sizeof(error)));
    return plan;
}

static void require_ready(const XrTargetPlan *target_plan,
                          const XrCEmissionPlan *emission_plan,
                          const ScalarFixture *fixture,
                          const XiValue *value, XrCScalarRep expected_rep,
                          const char *expected_c_type) {
    XrCScalarEmissionView view = {0};
    char error[512] = {0};
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(target_plan, fixture->function, value,
                                            &semantic_function, &semantic_value, error,
                                            sizeof(error)));
    REQUIRE(semantic_function == fixture->function->semantic_plan_function_index);
    REQUIRE(xr_c_emission_plan_scalar_view(emission_plan, semantic_value, &view, error,
                                           sizeof(error)));
    REQUIRE(view.rep == expected_rep);
    REQUIRE(view.target_register_rep == view.target_memory_rep);
    REQUIRE(view.target_register_kind == view.target_memory_kind);
    REQUIRE(view.register_bits != 0 || expected_rep == XR_C_SCALAR_REP_VOID);
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
    REQUIRE(xr_c_emission_plan_scalar_count(emission) == 3);
    REQUIRE(xr_fingerprint_equal(xr_c_emission_plan_target_fingerprint(emission),
                                 xr_target_plan_fingerprint(first)));
    REQUIRE(xr_fingerprint_equal(xr_c_emission_plan_profile_fingerprint(emission),
                                 profile_fingerprint));
    REQUIRE(xr_fingerprint_equal(xr_c_emission_plan_fingerprint(emission),
                                 xr_c_emission_plan_fingerprint(same_emission)));

    uint32_t count = 0;
    REQUIRE(xr_target_plan_value_reps(first, &count) != NULL && count == 3);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(first, &count);
    REQUIRE(slots != NULL && count == 2);
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
    REQUIRE(functions[semantic_function].frame_size == 16);
    REQUIRE(functions[semantic_function].frame_align == 8);
    REQUIRE(xr_target_plan_layouts(first, &count) != NULL && count == 2);
    require_ready(first, emission, &fixture, fixture.integer, XR_C_SCALAR_REP_I64, "int64_t");
    require_ready(first, emission, &fixture, fixture.boolean, XR_C_SCALAR_REP_BOOL, "uint8_t");
    require_ready(first, emission, &fixture, fixture.release, XR_C_SCALAR_REP_VOID, "void");

    XrCScalarEmissionView view = {0};
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(first, fixture.function, fixture.string,
                                            &semantic_function, &semantic_value, error,
                                            sizeof(error)));
    REQUIRE(!xr_c_emission_plan_scalar_view(emission, semantic_value, &view, error,
                                            sizeof(error)));
    REQUIRE(view.c_type == NULL);
    REQUIRE(strncmp(error, "XR_TARGET_1001", strlen("XR_TARGET_1001")) == 0);
    REQUIRE(!xr_c_emission_plan_scalar_view(NULL, semantic_value, &view, error, sizeof(error)));
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
    XrCScalarRep c_rep;
    uint16_t machine_kind;
    uint16_t fixed_register_bits;
    uint32_t fixed_memory_size;
    const char *c_type;
    bool pointer_sized;
} ScalarKnownAnswer;

static ScalarKnownAnswer scalar_known_answers[] = {
    {{.kind = XR_KIND_INT, .id = 100, .frozen = true, .scalar_rep = XR_NATIVE_I8},
     XR_C_SCALAR_REP_I8, XR_MACHINE_REP_I8, 8, 1, "int8_t", false},
    {{.kind = XR_KIND_INT, .id = 101, .frozen = true, .scalar_rep = XR_NATIVE_U8},
     XR_C_SCALAR_REP_U8, XR_MACHINE_REP_U8, 8, 1, "uint8_t", false},
    {{.kind = XR_KIND_INT, .id = 102, .frozen = true, .scalar_rep = XR_NATIVE_I16},
     XR_C_SCALAR_REP_I16, XR_MACHINE_REP_I16, 16, 2, "int16_t", false},
    {{.kind = XR_KIND_INT, .id = 103, .frozen = true, .scalar_rep = XR_NATIVE_U16},
     XR_C_SCALAR_REP_U16, XR_MACHINE_REP_U16, 16, 2, "uint16_t", false},
    {{.kind = XR_KIND_INT, .id = 104, .frozen = true, .scalar_rep = XR_NATIVE_I32},
     XR_C_SCALAR_REP_I32, XR_MACHINE_REP_I32, 32, 4, "int32_t", false},
    {{.kind = XR_KIND_INT, .id = 105, .frozen = true, .scalar_rep = XR_NATIVE_U32},
     XR_C_SCALAR_REP_U32, XR_MACHINE_REP_U32, 32, 4, "uint32_t", false},
    {{.kind = XR_KIND_INT, .id = 106, .frozen = true, .scalar_rep = XR_NATIVE_I64},
     XR_C_SCALAR_REP_I64, XR_MACHINE_REP_I64, 64, 8, "int64_t", false},
    {{.kind = XR_KIND_INT, .id = 107, .frozen = true, .scalar_rep = XR_NATIVE_U64},
     XR_C_SCALAR_REP_U64, XR_MACHINE_REP_U64, 64, 8, "uint64_t", false},
    {{.kind = XR_KIND_INT, .id = 108, .frozen = true, .scalar_rep = XR_NATIVE_ISIZE},
     XR_C_SCALAR_REP_ISIZE, XR_MACHINE_REP_ISIZE, 0, 0, "ptrdiff_t", true},
    {{.kind = XR_KIND_INT, .id = 109, .frozen = true, .scalar_rep = XR_NATIVE_USIZE},
     XR_C_SCALAR_REP_USIZE, XR_MACHINE_REP_USIZE, 0, 0, "size_t", true},
    {{.kind = XR_KIND_FLOAT, .id = 110, .frozen = true, .scalar_rep = XR_NATIVE_F32},
     XR_C_SCALAR_REP_F32, XR_MACHINE_REP_F32, 32, 4, "float", false},
    {{.kind = XR_KIND_FLOAT, .id = 111, .frozen = true, .scalar_rep = XR_NATIVE_F64},
     XR_C_SCALAR_REP_F64, XR_MACHINE_REP_F64, 64, 8, "double", false},
    {{.kind = XR_KIND_BOOL, .id = 112, .frozen = true, .scalar_rep = XR_SCALAR_REP_NONE},
     XR_C_SCALAR_REP_BOOL, XR_MACHINE_REP_I1, 1, 1, "uint8_t", false},
    {{.kind = XR_KIND_RUNE, .id = 113, .frozen = true, .scalar_rep = XR_SCALAR_REP_NONE},
     XR_C_SCALAR_REP_RUNE, XR_MACHINE_REP_RUNE, 32, 4, "uint32_t", false},
    {{.kind = XR_KIND_UNIT, .id = 114, .frozen = true, .scalar_rep = XR_SCALAR_REP_NONE},
     XR_C_SCALAR_REP_VOID, XR_MACHINE_REP_VOID, 0, 0, "void", false},
    {{.kind = XR_KIND_NEVER, .id = 115, .frozen = true, .scalar_rep = XR_SCALAR_REP_NONE},
     XR_C_SCALAR_REP_VOID, XR_MACHINE_REP_VOID, 0, 0, "void", false},
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
    XrCScalarEmissionView view = {0};
    REQUIRE(xr_c_emission_plan_scalar_view(emission_plan, semantic_value, &view, error,
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
    XrTargetProfile *profile = build_profile(ilp32, ilp32 ? 0x31 : 0x21);
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
    REQUIRE(xr_c_emission_plan_scalar_count(emission) == expected_count);
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(first),
                                 xr_target_plan_fingerprint(same)));
    REQUIRE(xr_fingerprint_equal(xr_c_emission_plan_fingerprint(emission),
                                 xr_c_emission_plan_fingerprint(same_emission)));
    for (uint32_t i = 0; i < expected_count; i++)
        require_scalar_known_answer(first, emission, &fixture, i, ilp32);

    XrCScalarEmissionView missing = {0};
    REQUIRE(!xr_c_emission_plan_scalar_view(emission, UINT32_MAX, &missing, error,
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
    REQUIRE(xr_c_emission_plan_scalar_count(emission) == 1);
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_aot_scalar_semantic_value_id(target_plan, function, nullable,
                                            &semantic_function, &semantic_value, error,
                                            sizeof(error)));
    XrCScalarEmissionView view = {0};
    REQUIRE(!xr_c_emission_plan_scalar_view(emission, semantic_value, &view, error,
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
    XrTargetProfile *different = build_profile(false, 0x41);
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

int main(void) {
    test_scalar_plan_and_emission_view();
    test_missing_semantic_authority_fails_closed();
    test_cross_function_value_substitution_fails_closed();
    test_parameter_identity_requires_exact_member();
    test_all_scalar_c_spelling_known_answers(false);
    test_all_scalar_c_spelling_known_answers(true);
    test_nullable_scalar_binding_is_rejected();
    test_profile_mismatch_fails_before_projection();
    printf("AOT scalar TargetPlan tests passed\n");
    return 0;
}
