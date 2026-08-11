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
    .frozen = true,
};
static XrType scalar_string = {
    .kind = XR_KIND_STRING,
    .id = 3,
    .frozen = true,
};
static XrType scalar_unit = {
    .kind = XR_KIND_UNIT,
    .id = 4,
    .frozen = true,
};

static XrTargetProfile *build_exact_profile(void) {
    XrTargetProfileDraft draft = {0};
    draft.schema_version = XR_TARGET_PLAN_SCHEMA_VERSION;
    draft.architecture = XR_TARGET_ARCH_X86_64;
    draft.operating_system = XR_TARGET_OS_WINDOWS;
    draft.environment = XR_TARGET_ENV_MSVC;
    draft.native_abi = XR_TARGET_ABI_WIN64_X86_64;
    draft.runtime_profile = XR_TARGET_RUNTIME_HOSTED;
    REQUIRE(xr_target_data_layout_init_lp64(&draft.data_layout));
    draft.atomic_width_mask = XR_TARGET_ATOMIC_WIDTH_8 | XR_TARGET_ATOMIC_WIDTH_16 |
                              XR_TARGET_ATOMIC_WIDTH_32 | XR_TARGET_ATOMIC_WIDTH_64;
    draft.atomic_order_mask = XR_TARGET_ATOMIC_RELAXED | XR_TARGET_ATOMIC_ACQUIRE |
                              XR_TARGET_ATOMIC_RELEASE | XR_TARGET_ATOMIC_ACQ_REL |
                              XR_TARGET_ATOMIC_SEQ_CST;
    draft.float_feature_mask = XR_TARGET_FLOAT_IEEE754 | XR_TARGET_FLOAT_STRICT;
    draft.vector_feature_mask = XR_TARGET_VECTOR_SSE2;
    draft.maximum_vector_bits = 128;
    draft.provider_mask = (UINT64_C(1) << XR_TARGET_PROVIDER_ALLOCATOR) |
                          (UINT64_C(1) << XR_TARGET_PROVIDER_PANIC);
    draft.provider_set_fingerprint.bytes[0] = 0x11;
    draft.object_header_fingerprint.bytes[0] = 0x22;
    draft.runtime_abi_fingerprint.bytes[0] = 0x33;
    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_profile_freeze(&draft, &profile, error, sizeof(error)));
    return profile;
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
    REQUIRE(xr_c_emission_plan_build(first, &emission, error, sizeof(error)));
    REQUIRE(xr_c_emission_plan_build(same, &same_emission, error, sizeof(error)));
    REQUIRE(xr_c_emission_plan_is_verified(emission));
    REQUIRE(xr_c_emission_plan_scalar_count(emission) == 3);
    REQUIRE(xr_fingerprint_equal(xr_c_emission_plan_target_fingerprint(emission),
                                 xr_target_plan_fingerprint(first)));
    REQUIRE(xr_fingerprint_equal(xr_c_emission_plan_fingerprint(emission),
                                 xr_c_emission_plan_fingerprint(same_emission)));

    uint32_t count = 0;
    REQUIRE(xr_target_plan_value_reps(first, &count) != NULL && count == 3);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(first, &count);
    REQUIRE(slots != NULL && count == 2);
    REQUIRE(slots[0].offset == 0 && slots[0].size == 1 && slots[0].align == 1);
    REQUIRE(slots[1].offset == 8 && slots[1].size == 8 && slots[1].align == 8);
    REQUIRE(xr_target_plan_layouts(first, &count) != NULL && count == 2);
    require_ready(first, emission, &fixture, fixture.integer, XR_C_SCALAR_REP_I64, "int64_t");
    require_ready(first, emission, &fixture, fixture.boolean, XR_C_SCALAR_REP_BOOL, "uint8_t");
    require_ready(first, emission, &fixture, fixture.release, XR_C_SCALAR_REP_VOID, "void");

    XrCScalarEmissionView view = {0};
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
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

int main(void) {
    test_scalar_plan_and_emission_view();
    test_missing_semantic_authority_fails_closed();
    test_cross_function_value_substitution_fails_closed();
    test_parameter_identity_requires_exact_member();
    printf("AOT scalar TargetPlan tests passed\n");
    return 0;
}
