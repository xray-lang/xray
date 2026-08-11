/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xaot_scalar_target_cutover.c - TargetPlan scalar row cutover tests
 */

#include "../../../src/aot/xaot_prepare.h"
#include "../../../src/aot/xaot_verify.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_plan_internal.h"
#include "../../../src/runtime/value/xenum_layout.h"
#include "../../../src/runtime/value/xtype.h"
#include "../plan/target_profile_test_fixture.h"
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

typedef struct CutoverModule {
    XiModule module;
    XiFunc *function;
    XiValue *integer;
    XiValue *text;
    XiValue *enum_ordinal;
    XiValue *release;
    XrTargetPlan *target_plan;
} CutoverModule;

typedef struct CutoverBundle {
    CutoverModule modules[2];
    XiModule *module_ptrs[2];
    XrTargetProfile *profile;
    XaotBundle bundle;
    XgGlobalEvidence evidence;
    bool bundle_initialized;
    bool evidence_initialized;
} CutoverBundle;

static XrType scalar_int = {
    .kind = XR_KIND_INT,
    .id = 27001,
    .scalar_rep = XR_NATIVE_I64,
    .frozen = true,
};

static XrType scalar_string = {
    .kind = XR_KIND_STRING,
    .id = 27002,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .frozen = true,
};

static XrType scalar_unit = {
    .kind = XR_KIND_UNIT,
    .id = 27003,
    .scalar_rep = XR_SCALAR_REP_NONE,
    .frozen = true,
};

static XrEnumLayout scalar_enum_layout = {
    .layout_id = 27004,
    .name = "ScalarCutoverState",
    .variant_count = 2,
    .tag_size = 1,
    .align = 1,
    .size = 1,
    .is_zero_payload = true,
};

static XiEnumMemberData scalar_enum_members[] = {
    {.name = "idle", .ordinal = 0},
    {.name = "ready", .ordinal = 1},
};

static XiEnumData scalar_enum_data = {
    .name = "ScalarCutoverState",
    .member_count = 2,
    .max_payload = 0,
    .layout_id = 27004,
    .members = scalar_enum_members,
};

static XiEnumData *scalar_enum_slots[] = {&scalar_enum_data};

static XrType scalar_enum = {
    .kind = XR_KIND_ENUM,
    .id = 27004,
    .frozen = true,
    .enum_type = {
        .enum_name = "ScalarCutoverState",
        .layout_id = 27004,
        .layout = &scalar_enum_layout,
    },
};

static CutoverModule cutover_module_create(const char *name, int64_t value) {
    CutoverModule module = {0};
    char error[512] = {0};

    module.function = xi_func_new(name, &scalar_int);
    REQUIRE(module.function != NULL);
    XiBlock *entry = xi_block_new(module.function);
    REQUIRE(entry != NULL);
    module.integer = xi_const_int(module.function, entry, value, &scalar_int);
    module.text = xi_const_str(module.function, entry, name, &scalar_string);
    module.enum_ordinal = xi_value_new(module.function, entry,
                                       XI_GET_SHARED, &scalar_enum, 0);
    if (module.enum_ordinal)
        module.enum_ordinal->aux_int = 0;
    module.release = xi_value_new(module.function, entry, XI_RELEASE, &scalar_unit, 1);
    REQUIRE(module.integer && module.text && module.enum_ordinal && module.release);
    module.release->args[0] = module.text;
    xi_block_set_return(entry, module.integer);
    module.function->stage = XI_STAGE_OPTIMIZED;
    bool semantic_built = xr_semantic_plan_build_and_attach(
        module.function, error, sizeof(error));
    if (!semantic_built)
        fprintf(stderr, "cutover semantic plan failed: %s\n", error);
    REQUIRE(semantic_built);
    module.module.init = module.function;
    module.module.name = name;
    module.module.nslots = 1;
    module.module.slot_enums = scalar_enum_slots;
    return module;
}

static void cutover_bundle_create(CutoverBundle *fixture) {
    char error[512] = {0};

    memset(fixture, 0, sizeof(*fixture));
    fixture->modules[0] = cutover_module_create("scalar_dep", 11);
    fixture->modules[1] = cutover_module_create("scalar_entry", 22);
    fixture->module_ptrs[0] = &fixture->modules[0].module;
    fixture->module_ptrs[1] = &fixture->modules[1].module;
    fixture->profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(fixture->profile != NULL);
    for (uint32_t module_index = 0; module_index < 2; module_index++) {
        REQUIRE(xr_target_plan_build(fixture->modules[module_index].function->semantic_plan,
                                     fixture->profile,
                                     &fixture->modules[module_index].target_plan, error,
                                     sizeof(error)));
    }
    REQUIRE(xaot_bundle_init(&fixture->bundle, fixture->module_ptrs, 2, 1));
    fixture->bundle_initialized = true;

    XgBuildKey key = {
        .compiler_semver_hash = UINT64_C(0x272),
        .module_id = 2,
        .profile = XG_BUILD_NATIVE_RELEASE,
    };
    xg_global_evidence_init(&fixture->evidence, key);
    fixture->evidence_initialized = true;
    for (uint32_t module_index = 0; module_index < 2; module_index++) {
        XgBodySummary body = {
            .func_id = module_index + 1,
            .module_id = module_index + 1,
            .name_id = xg_name_id("<module-init>"),
            .kind = XG_BODY_MODULE_INIT,
            .body_hash = UINT64_C(0x272000) + module_index,
        };
        fixture->modules[module_index].function->xg_body_func_id = body.func_id;
        REQUIRE(xg_global_evidence_add_body(&fixture->evidence, &body) != NULL);
    }
    REQUIRE(xaot_bundle_set_global_evidence(&fixture->bundle, &fixture->evidence,
                                            XG_BUILD_NATIVE_RELEASE));
}

static void cutover_bundle_bind_all(CutoverBundle *fixture) {
    for (uint32_t module_index = 0; module_index < 2; module_index++)
        REQUIRE(xaot_bundle_set_target_plan(&fixture->bundle, module_index,
                                            fixture->modules[module_index].target_plan));
}

static void cutover_bundle_free(CutoverBundle *fixture) {
    if (fixture->bundle_initialized)
        xaot_bundle_free(&fixture->bundle);
    if (fixture->evidence_initialized)
        xg_global_evidence_free(&fixture->evidence);
    for (uint32_t module_index = 0; module_index < 2; module_index++) {
        xr_target_plan_free(fixture->modules[module_index].target_plan);
        xi_func_free(fixture->modules[module_index].function);
    }
    xr_target_profile_free(fixture->profile);
    memset(fixture, 0, sizeof(*fixture));
}

static void test_missing_and_partial_module_plans_fail_before_prepare(void) {
    CutoverBundle fixture;
    char error[512] = {0};
    cutover_bundle_create(&fixture);

    REQUIRE(!xaot_prepare_bundle(&fixture.bundle, NULL));
    REQUIRE(fixture.bundle.nfunc_plans == 0);
    REQUIRE(fixture.bundle.nvalue_plans == 0);
    REQUIRE(fixture.bundle.error_msg != NULL);
    REQUIRE(strstr(fixture.bundle.error_msg, "TargetPlan") != NULL);
    REQUIRE(!xaot_verify_bundle(&fixture.bundle, XAOT_VERIFY_AOT_READY, error, sizeof(error)));
    REQUIRE(strstr(error, "TargetPlan") != NULL);

    REQUIRE(xaot_bundle_set_target_plan(&fixture.bundle, 0,
                                        fixture.modules[0].target_plan));
    REQUIRE(!xaot_prepare_bundle(&fixture.bundle, NULL));
    REQUIRE(fixture.bundle.nfunc_plans == 0);
    REQUIRE(fixture.bundle.nvalue_plans == 0);
    cutover_bundle_free(&fixture);
}

static void test_multi_module_prepare_has_no_scalar_legacy_rows(void) {
    CutoverBundle fixture;
    char error[512] = {0};
    cutover_bundle_create(&fixture);
    cutover_bundle_bind_all(&fixture);

    REQUIRE(xaot_prepare_bundle(&fixture.bundle, NULL));
    REQUIRE(fixture.bundle.nvalue_plans == 4);
    REQUIRE(fixture.bundle.stats.values_total == 8);
    REQUIRE(fixture.bundle.stats.values_scalar == 2);
    REQUIRE(fixture.bundle.stats.values_void == 2);
    REQUIRE(fixture.bundle.stats.values_tagged == 2);
    REQUIRE(fixture.bundle.stats.values_enum_ordinal == 2);
    for (uint32_t module_index = 0; module_index < 2; module_index++) {
        const CutoverModule *module = &fixture.modules[module_index];
        REQUIRE(xaot_bundle_target_plan_for_func(&fixture.bundle, module->function) ==
                module->target_plan);
        REQUIRE(xaot_bundle_find_value_plan(&fixture.bundle, module->integer) == NULL);
        REQUIRE(xaot_bundle_find_value_plan(&fixture.bundle, module->release) == NULL);
        const XaotValuePlan *text_plan =
            xaot_bundle_find_value_plan(&fixture.bundle, module->text);
        REQUIRE(text_plan != NULL);
        REQUIRE(text_plan->rep.kind == XAOT_VALUE_TAGGED);
        const XaotValuePlan *enum_plan =
            xaot_bundle_find_value_plan(&fixture.bundle,
                                        module->enum_ordinal);
        REQUIRE(enum_plan != NULL);
        REQUIRE(xaot_value_plan_is_exact_enum_ordinal_family(
            &fixture.bundle, enum_plan));
    }
    REQUIRE(xaot_verify_bundle(&fixture.bundle, XAOT_VERIFY_AOT_READY, error, sizeof(error)));

    XaotValuePlan *enum_value = xaot_bundle_find_value_plan_mut(
        &fixture.bundle, fixture.modules[0].enum_ordinal);
    REQUIRE(enum_value != NULL);
    uint32_t saved_flags = enum_value->rep.flags;
    enum_value->rep.flags = XAOT_VALUE_FLAG_ENUM | XAOT_VALUE_FLAG_STRUCT;
    REQUIRE(!xaot_value_plan_is_exact_enum_ordinal_family(&fixture.bundle,
                                                        enum_value));
    REQUIRE(!xaot_verify_bundle(&fixture.bundle, XAOT_VERIFY_AOT_READY,
                                error, sizeof(error)));
    enum_value->rep.flags = saved_flags;

    uint16_t saved_payload = fixture.bundle.enum_plans[0].max_payload;
    fixture.bundle.enum_plans[0].max_payload = 1;
    REQUIRE(!xaot_value_plan_is_exact_enum_ordinal_family(&fixture.bundle,
                                                        enum_value));
    fixture.bundle.enum_plans[0].max_payload = saved_payload;

    uint32_t saved_enum_plan_count = fixture.bundle.nenum_plans;
    fixture.bundle.nenum_plans = 0;
    REQUIRE(!xaot_value_plan_is_exact_enum_ordinal_family(&fixture.bundle,
                                                        enum_value));
    fixture.bundle.nenum_plans = saved_enum_plan_count;
    REQUIRE(xaot_verify_bundle(&fixture.bundle, XAOT_VERIFY_AOT_READY,
                               error, sizeof(error)));
    cutover_bundle_free(&fixture);
}

static void test_corrupt_bound_plan_and_scalar_residue_fail_closed(void) {
    CutoverBundle corrupt;
    cutover_bundle_create(&corrupt);
    cutover_bundle_bind_all(&corrupt);
    corrupt.modules[0].target_plan->fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xaot_prepare_bundle(&corrupt.bundle, NULL));
    REQUIRE(corrupt.bundle.nfunc_plans == 0);
    REQUIRE(corrupt.bundle.nvalue_plans == 0);
    corrupt.modules[0].target_plan->fingerprint.bytes[0] ^= 1u;
    cutover_bundle_free(&corrupt);

    CutoverBundle residue;
    char error[512] = {0};
    cutover_bundle_create(&residue);
    cutover_bundle_bind_all(&residue);
    REQUIRE(xaot_prepare_bundle(&residue.bundle, NULL));
    REQUIRE(xaot_bundle_add_value_plan(&residue.bundle, residue.modules[0].function,
                                       residue.modules[0].integer) != NULL);
    XaotValuePlan *forged = xaot_bundle_find_value_plan_mut(
        &residue.bundle, residue.modules[0].integer);
    REQUIRE(forged != NULL);
    forged->rep.flags = XAOT_VALUE_FLAG_ENUM;
    REQUIRE(!xaot_verify_bundle(&residue.bundle, XAOT_VERIFY_AOT_READY, error,
                                sizeof(error)));
    REQUIRE(strstr(error, "scalar") != NULL);
    REQUIRE(strstr(error, "legacy") != NULL);
    cutover_bundle_free(&residue);
}

static XiValue *append_rep_adapter(CutoverModule *module, XiValue *source,
                                   uint16_t op, XiBackendValueOrigin origin,
                                   XrRep rep) {
    XiBlock *block = module && module->function && module->function->nblocks
                         ? module->function->blocks[0]
                         : NULL;
    XiValue *adapter = block
                           ? xi_value_new(module->function, block, op,
                                          source ? source->type : NULL, 1)
                           : NULL;
    REQUIRE(adapter != NULL && source != NULL);
    adapter->args[0] = source;
    adapter->backend_origin = (uint8_t) origin;
    adapter->rep = (uint8_t) rep;
    return adapter;
}

static void test_exact_rep_adapter_family_and_mutations(void) {
    CutoverBundle fixture;
    char error[512] = {0};
    cutover_bundle_create(&fixture);
    CutoverModule *module = &fixture.modules[0];
    module->integer->rep = XR_REP_I64;
    module->text->rep = XR_REP_PTR;
    XiValue *scalar_box = append_rep_adapter(
        module, module->integer, XI_BOX, XI_BACKEND_VALUE_REP_BOX,
        XR_REP_TAGGED);
    XiValue *string_box = append_rep_adapter(
        module, module->text, XI_BOX, XI_BACKEND_VALUE_REP_BOX,
        XR_REP_TAGGED);
    cutover_bundle_bind_all(&fixture);
    REQUIRE(xaot_prepare_bundle(&fixture.bundle, NULL));
    REQUIRE(fixture.bundle.stats.values_rep_adapter == 2);
    XaotValuePlan *scalar_plan = xaot_bundle_find_value_plan_mut(
        &fixture.bundle, scalar_box);
    XaotValuePlan *string_plan = xaot_bundle_find_value_plan_mut(
        &fixture.bundle, string_box);
    REQUIRE(scalar_plan && string_plan);
    REQUIRE(xaot_value_plan_is_exact_rep_adapter(&fixture.bundle,
                                                 scalar_plan));
    REQUIRE(xaot_value_plan_is_exact_rep_adapter(&fixture.bundle,
                                                 string_plan));
    REQUIRE(xaot_verify_bundle(&fixture.bundle, XAOT_VERIFY_AOT_READY,
                               error, sizeof(error)));

    const char *saved_c_type = scalar_plan->rep.c_type;
    scalar_plan->rep.c_type = "forged_scalar_t";
    REQUIRE(!xaot_value_plan_is_exact_rep_adapter(&fixture.bundle,
                                                  scalar_plan));
    REQUIRE(!xaot_verify_bundle(&fixture.bundle, XAOT_VERIFY_AOT_READY,
                                error, sizeof(error)));
    scalar_plan->rep.c_type = saved_c_type;
    uint32_t saved_flags = scalar_plan->rep.flags;
    scalar_plan->rep.flags = XAOT_VALUE_FLAG_DYNAMIC_C_TYPE;
    REQUIRE(!xaot_value_plan_is_exact_rep_adapter(&fixture.bundle,
                                                  scalar_plan));
    scalar_plan->rep.flags = saved_flags;

    uint8_t saved_origin = scalar_box->backend_origin;
    scalar_box->backend_origin = XI_BACKEND_VALUE_NONE;
    REQUIRE(!xaot_value_plan_is_exact_rep_adapter(&fixture.bundle,
                                                  scalar_plan));
    scalar_box->backend_origin = UINT8_MAX;
    REQUIRE(!xaot_value_plan_is_exact_rep_adapter(&fixture.bundle,
                                                  scalar_plan));
    scalar_box->backend_origin = saved_origin;
    uint16_t saved_op = scalar_box->op;
    scalar_box->op = XI_UNBOX;
    REQUIRE(!xaot_value_plan_is_exact_rep_adapter(&fixture.bundle,
                                                  scalar_plan));
    scalar_box->op = saved_op;
    uint16_t saved_nargs = scalar_box->nargs;
    scalar_box->nargs = 0;
    REQUIRE(!xaot_value_plan_is_exact_rep_adapter(&fixture.bundle,
                                                  scalar_plan));
    scalar_box->nargs = saved_nargs;
    XiValue *saved_source = scalar_box->args[0];
    scalar_box->args[0] = NULL;
    REQUIRE(!xaot_value_plan_is_exact_rep_adapter(&fixture.bundle,
                                                  scalar_plan));
    scalar_box->args[0] = saved_source;
    module->integer->backend_origin = XI_BACKEND_VALUE_REP_BOX;
    REQUIRE(!xaot_value_plan_is_exact_rep_adapter(&fixture.bundle,
                                                  scalar_plan));
    module->integer->backend_origin = XI_BACKEND_VALUE_NONE;
    scalar_box->args[0] = fixture.modules[1].integer;
    REQUIRE(!xaot_value_plan_is_exact_rep_adapter(&fixture.bundle,
                                                  scalar_plan));
    scalar_box->args[0] = saved_source;
    uint32_t saved_id = scalar_box->id;
    const XrSemanticFunctionRecord *semantic_function =
        xr_semantic_plan_function(module->function->semantic_plan, 0);
    REQUIRE(semantic_function != NULL && semantic_function->value_count > 0);
    scalar_box->id = semantic_function->value_count - 1;
    REQUIRE(!xaot_value_plan_is_exact_rep_adapter(&fixture.bundle,
                                                  scalar_plan));
    scalar_box->id = saved_id;
    XrType *saved_type = scalar_box->type;
    scalar_box->type = &scalar_unit;
    REQUIRE(!xaot_value_plan_is_exact_rep_adapter(&fixture.bundle,
                                                  scalar_plan));
    scalar_box->type = saved_type;
    uint8_t saved_rep = scalar_box->rep;
    scalar_box->rep = XR_REP_PTR;
    REQUIRE(!xaot_value_plan_is_exact_rep_adapter(&fixture.bundle,
                                                  scalar_plan));
    scalar_box->rep = saved_rep;
    module->integer->rep = XR_REP_TAGGED;
    REQUIRE(!xaot_value_plan_is_exact_rep_adapter(&fixture.bundle,
                                                  scalar_plan));

    scalar_box->op = XI_UNBOX;
    scalar_box->backend_origin = XI_BACKEND_VALUE_REP_UNBOX;
    scalar_box->rep = XR_REP_I64;
    scalar_plan->rep.kind = XAOT_VALUE_SCALAR;
    scalar_plan->rep.rep = XAOT_REP_I64;
    scalar_plan->rep.c_type = "int64_t";
    scalar_plan->rep.flags = 0;
    REQUIRE(xaot_value_plan_is_exact_rep_adapter(&fixture.bundle,
                                                 scalar_plan));
    uint32_t saved_binding_count = module->target_plan->value_reps_count;
    module->target_plan->value_reps_count = 0;
    REQUIRE(!xaot_value_plan_is_exact_rep_adapter(&fixture.bundle,
                                                  scalar_plan));
    module->target_plan->value_reps_count = saved_binding_count;
    cutover_bundle_free(&fixture);

    CutoverBundle unbox;
    cutover_bundle_create(&unbox);
    unbox.modules[0].integer->rep = XR_REP_TAGGED;
    unbox.modules[0].text->rep = XR_REP_TAGGED;
    XiValue *scalar_unbox = append_rep_adapter(
        &unbox.modules[0], unbox.modules[0].integer, XI_UNBOX,
        XI_BACKEND_VALUE_REP_UNBOX, XR_REP_I64);
    XiValue *string_unbox = append_rep_adapter(
        &unbox.modules[0], unbox.modules[0].text, XI_UNBOX,
        XI_BACKEND_VALUE_REP_UNBOX, XR_REP_PTR);
    cutover_bundle_bind_all(&unbox);
    REQUIRE(xaot_prepare_bundle(&unbox.bundle, NULL));
    REQUIRE(unbox.bundle.stats.values_rep_adapter == 2);
    REQUIRE(xaot_value_plan_is_exact_rep_adapter(
        &unbox.bundle,
        xaot_bundle_find_value_plan(&unbox.bundle, scalar_unbox)));
    REQUIRE(xaot_value_plan_is_exact_rep_adapter(
        &unbox.bundle,
        xaot_bundle_find_value_plan(&unbox.bundle, string_unbox)));
    REQUIRE(xaot_verify_bundle(&unbox.bundle, XAOT_VERIFY_AOT_READY,
                               error, sizeof(error)));
    cutover_bundle_free(&unbox);

    CutoverBundle forged;
    cutover_bundle_create(&forged);
    forged.modules[0].text->rep = XR_REP_PTR;
    (void) append_rep_adapter(&forged.modules[0], forged.modules[0].text,
                              XI_BOX, XI_BACKEND_VALUE_NONE,
                              XR_REP_TAGGED);
    cutover_bundle_bind_all(&forged);
    REQUIRE(!xaot_prepare_bundle(&forged.bundle, NULL));
    cutover_bundle_free(&forged);
}

int main(void) {
    test_missing_and_partial_module_plans_fail_before_prepare();
    test_multi_module_prepare_has_no_scalar_legacy_rows();
    test_corrupt_bound_plan_and_scalar_residue_fail_closed();
    test_exact_rep_adapter_family_and_mutations();
    puts("test_xaot_scalar_target_cutover: ok");
    return 0;
}
