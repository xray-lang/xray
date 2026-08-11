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

static CutoverModule cutover_module_create(const char *name, int64_t value) {
    CutoverModule module = {0};
    char error[512] = {0};

    module.function = xi_func_new(name, &scalar_int);
    REQUIRE(module.function != NULL);
    XiBlock *entry = xi_block_new(module.function);
    REQUIRE(entry != NULL);
    module.integer = xi_const_int(module.function, entry, value, &scalar_int);
    module.text = xi_const_str(module.function, entry, name, &scalar_string);
    module.release = xi_value_new(module.function, entry, XI_RELEASE, &scalar_unit, 1);
    REQUIRE(module.integer && module.text && module.release);
    module.release->args[0] = module.text;
    xi_block_set_return(entry, module.integer);
    module.function->stage = XI_STAGE_OPTIMIZED;
    REQUIRE(xr_semantic_plan_build_and_attach(module.function, error, sizeof(error)));
    module.module.init = module.function;
    module.module.name = name;
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
    REQUIRE(fixture.bundle.nvalue_plans == 2);
    REQUIRE(fixture.bundle.stats.values_total == 6);
    REQUIRE(fixture.bundle.stats.values_scalar == 2);
    REQUIRE(fixture.bundle.stats.values_void == 2);
    REQUIRE(fixture.bundle.stats.values_tagged == 2);
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
    }
    REQUIRE(xaot_verify_bundle(&fixture.bundle, XAOT_VERIFY_AOT_READY, error, sizeof(error)));
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
    REQUIRE(!xaot_verify_bundle(&residue.bundle, XAOT_VERIFY_AOT_READY, error,
                                sizeof(error)));
    REQUIRE(strstr(error, "scalar") != NULL);
    REQUIRE(strstr(error, "legacy") != NULL);
    cutover_bundle_free(&residue);
}

int main(void) {
    test_missing_and_partial_module_plans_fail_before_prepare();
    test_multi_module_prepare_has_no_scalar_legacy_rows();
    test_corrupt_bound_plan_and_scalar_residue_fail_closed();
    puts("test_xaot_scalar_target_cutover: ok");
    return 0;
}
