/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xglobal_summary.c - Unit tests for global summary/evidence data model
 */

#include "../test_framework.h"
#include "../../../src/analysis/xglobal_producer.h"
#include "../../../src/analysis/xglobal_summary.h"
#include "../../../src/aot/xaot_bundle.h"
#include "../../../src/aot/xaot_verify.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/frontend/parser/xparse.h"
#include "../../../src/ir/xi_module.h"
#include "../../../src/module/xmodule_graph.h"
#include "../../../src/toolchain/xcompiler_session.h"
#include "xray_vm.h"
#include <string.h>

static XrVMRuntime *g_iso = NULL;
static XrCompilerSession *g_session = NULL;
static XrType stub_int_type = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static void setup_parser_session(void) {
    if (g_iso)
        return;
    XrVMConfig vm_cfg;
    xray_vm_config_init(&vm_cfg);
    g_iso = xray_vm_new(&vm_cfg);
    XrCompilerSessionConfig cfg = {.vm_host = g_iso};
    g_session = xr_compiler_session_new(&cfg);
    xr_compiler_session_attach_isolate(g_iso, g_session);
}

static void teardown_parser_session(void) {
    if (g_session) {
        xr_compiler_session_delete(g_session);
        g_session = NULL;
    }
    if (g_iso) {
        xray_vm_delete(g_iso);
        g_iso = NULL;
    }
}

static uint32_t evidence_body_count_with_capability(const XgGlobalEvidence *ev, uint32_t cap) {
    uint32_t count = 0;
    if (!ev)
        return 0;
    for (uint32_t i = 0; i < ev->nbodies; i++) {
        if ((ev->bodies[i].capability_bits & cap) != 0)
            count++;
    }
    return count;
}

static uint32_t evidence_body_count_with_metadata(const XgGlobalEvidence *ev, uint32_t metadata) {
    uint32_t count = 0;
    if (!ev)
        return 0;
    for (uint32_t i = 0; i < ev->nbodies; i++) {
        if ((ev->bodies[i].metadata_use_bits & metadata) != 0)
            count++;
    }
    return count;
}

static uint32_t evidence_body_count_with_static_data(const XgGlobalEvidence *ev,
                                                     uint32_t static_data) {
    uint32_t count = 0;
    if (!ev)
        return 0;
    for (uint32_t i = 0; i < ev->nbodies; i++) {
        if ((ev->bodies[i].static_data_use_bits & static_data) != 0)
            count++;
    }
    return count;
}

static uint32_t evidence_decl_count_with_flags(const XgGlobalEvidence *ev, uint32_t flags) {
    uint32_t count = 0;
    if (!ev)
        return 0;
    for (uint32_t i = 0; i < ev->ndecls; i++) {
        if ((ev->decls[i].flags & flags) == flags)
            count++;
    }
    return count;
}

static void assert_body_callsite_ordinals(const XgGlobalEvidence *ev) {
    ASSERT_NOT_NULL(ev);
    for (uint32_t i = 0; i < ev->nbodies; i++) {
        const XgBodySummary *body = &ev->bodies[i];
        for (uint32_t j = 0; j < body->callsite_count; j++) {
            const XgCallsiteSummary *call =
                xg_global_evidence_find_callsite(ev, (XgCallsiteId) (body->callsite_start + j));
            ASSERT_NOT_NULL(call);
            ASSERT_EQ_UINT(call->owner_func_id, body->func_id);
            ASSERT_EQ_UINT(call->body_ordinal, j);
        }
    }
}

static const XgBodySummary *evidence_find_body_by_func(const XgGlobalEvidence *ev,
                                                       XgFuncId func_id) {
    if (!ev || func_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->nbodies; i++) {
        if (ev->bodies[i].func_id == func_id)
            return &ev->bodies[i];
    }
    return NULL;
}

static void assert_single_callsite_rejected(const XgCallsiteSummary *call,
                                            const char *expected_error) {
    XgGlobalEvidence ev;
    XgBuildKey key = {.source_hash = 0x9010,
                      .compiler_semver_hash = 0x9011,
                      .profile_hash = 0x9012,
                      .imported_summary_hash = 0x9013,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    XgDeclSummary decl = {.module_id = 1,
                          .decl_id = 1,
                          .kind = XG_DECL_FUNC,
                          .name_id = 10,
                          .signature_key = 20,
                          .source_span_id = 3};
    XgBodySummary body = {.func_id = 1,
                          .module_id = 1,
                          .owner_decl_id = 1,
                          .name_id = 10,
                          .signature_key = 20,
                          .source_span_id = 3,
                          .kind = XG_BODY_FUNCTION,
                          .body_hash = 0x9014,
                          .callsite_start = 1,
                          .callsite_count = 1};
    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    XaotBundle bundle;
    char err[256];

    xg_global_evidence_init(&ev, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&ev, &body));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&ev, call));

    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    modules[0] = &module;

    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    bundle.modules = modules;
    bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, expected_error));

    xaot_bundle_free(&bundle);
    xg_global_evidence_free(&ev);
}

TEST(global_evidence_adds_rows_and_grows) {
    XgGlobalEvidence ev;
    XgBuildKey key = {.source_hash = 0x10,
                      .compiler_semver_hash = 0x20,
                      .profile_hash = 0x30,
                      .imported_summary_hash = 0x40,
                      .module_id = 7,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    xg_global_evidence_init(&ev, key);

    for (uint32_t i = 0; i < 20; i++) {
        XgDeclSummary decl = {.module_id = 7,
                              .decl_id = i + 1,
                              .kind = XG_DECL_FUNC,
                              .flags = XG_DECL_PUBLIC,
                              .name_id = 100 + i,
                              .type_key = 200 + i,
                              .signature_key = 300 + i,
                              .source_span_id = 400 + i};
        ASSERT_NOT_NULL(xg_global_evidence_add_decl(&ev, &decl));
    }

    ASSERT_EQ_UINT(ev.ndecls, 20);
    ASSERT_TRUE(ev.decl_cap >= 20);
    ASSERT_EQ_UINT(ev.decls[19].decl_id, 20);
    ASSERT_EQ_UINT(ev.decls[19].signature_key, 319);

    xg_global_evidence_free(&ev);
}

TEST(global_evidence_hash_is_content_stable) {
    XgGlobalEvidence a;
    XgGlobalEvidence b;
    XgBuildKey key = {.source_hash = 1,
                      .compiler_semver_hash = 2,
                      .profile_hash = 3,
                      .imported_summary_hash = 4,
                      .module_id = 5,
                      .profile = XG_BUILD_FREESTANDING};
    XgClassSummary cls = {.class_id = 10,
                          .parent_class_id = XG_NO_ID,
                          .flags = XG_CLASS_EXPLICIT_FINAL | XG_CLASS_INFERRED_FINAL,
                          .field_start = 0,
                          .field_count = 2,
                          .method_start = 0,
                          .method_count = 1,
                          .interface_start = 0,
                          .interface_count = 0};

    xg_global_evidence_init(&a, key);
    xg_global_evidence_init(&b, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&a, &cls));
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&b, &cls));
    ASSERT_EQ_UINT(xg_global_evidence_hash(&a), xg_global_evidence_hash(&b));

    b.classes[0].flags |= XG_CLASS_HAS_SUBCLASS;
    ASSERT_NE(xg_global_evidence_hash(&a), xg_global_evidence_hash(&b));

    xg_global_evidence_free(&a);
    xg_global_evidence_free(&b);
}

TEST(global_evidence_dump_lists_core_rows) {
    XgGlobalEvidence ev;
    char *dump;
    XgBuildKey key = {.source_hash = 0xa,
                      .compiler_semver_hash = 0xb,
                      .profile_hash = 0xc,
                      .imported_summary_hash = 0xd,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    XgDeclSummary decl = {.module_id = 1,
                          .decl_id = 2,
                          .kind = XG_DECL_CLASS,
                          .flags = XG_DECL_FINAL,
                          .name_id = 44,
                          .type_key = 55,
                          .signature_key = 66,
                          .source_span_id = 77};
    XgClassSummary cls = {.class_id = 2,
                          .module_id = 1,
                          .decl_id = 2,
                          .name_id = 44,
                          .parent_class_id = XG_NO_ID,
                          .flags = XG_CLASS_EXPLICIT_FINAL | XG_CLASS_INFERRED_FINAL,
                          .field_start = 0,
                          .field_count = 1,
                          .method_start = 0,
                          .method_count = 1,
                          .interface_start = 0,
                          .interface_count = 0};
    XgMethodSummary method = {.method_id = 3,
                              .owner_class_id = 2,
                              .name_id = 88,
                              .signature_key = 99,
                              .override_of = XG_NO_ID,
                              .default_arg_contract_id = XG_NO_ID,
                              .flags = XG_METHOD_DIRECT_ONLY};
    XgInterfaceImplSummary impl = {.implementor_class_id = 2,
                                   .interface_id = 123,
                                   .name_id = 123,
                                   .type_key = 456,
                                   .source_span_id = 77,
                                   .flags = 0};
    XgInterfaceExtendsSummary interface_extends = {.child_interface_id = 123,
                                                   .parent_interface_id = 124,
                                                   .name_id = 124,
                                                   .type_key = 457,
                                                   .source_span_id = 78,
                                                   .flags = 0};
    XgInterfaceMethodSummary interface_method = {.interface_method_id = 7,
                                                 .owner_interface_id = 123,
                                                 .name_id = 700,
                                                 .signature_key = 701,
                                                 .ordinal = 0,
                                                 .source_span_id = 78,
                                                 .flags = 0};
    XgBodySummary body = {.func_id = 4,
                          .module_id = 1,
                          .owner_decl_id = 2,
                          .owner_class_id = XG_NO_ID,
                          .owner_method_id = XG_NO_ID,
                          .name_id = 88,
                          .signature_key = 66,
                          .source_span_id = 77,
                          .kind = XG_BODY_FUNCTION,
                          .body_hash = 0x1234,
                          .effect_bits = 1,
                          .escape_bits = 2,
                          .capability_bits = 4,
                          .callsite_start = 0,
                          .callsite_count = 1,
                          .metadata_use_bits = 8,
                          .static_data_use_bits = 16};
    XgCallsiteSummary call = {.callsite_id = 5,
                              .owner_func_id = 4,
                              .body_ordinal = 2,
                              .kind = XG_CALL_METHOD,
                              .static_target_func_id = 0,
                              .receiver_static_class_id = 2,
                              .method_id = 3,
                              .arg_type_key_start = 0,
                              .arg_count = 1,
                              .flags = XG_CALL_USES_DEFAULT_ARGS};
    XgLinkDependencySummary link_dep = {.link_id = 6,
                                        .module_id = 1,
                                        .decl_id = 2,
                                        .source_span_id = 77,
                                        .name_id = 101,
                                        .kind = XG_LINK_DEP_EXTERN_DYLIB,
                                        .flags = 0};
    memcpy(link_dep.name, "m", 2);

    xg_global_evidence_init(&ev, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &cls));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&ev, &method));
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_impl(&ev, &impl));
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_extends(&ev, &interface_extends));
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_method(&ev, &interface_method));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&ev, &body));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&ev, &call));
    ASSERT_NOT_NULL(xg_global_evidence_add_link_dependency(&ev, &link_dep));

    dump = xg_global_evidence_dump(&ev);
    ASSERT_NOT_NULL(dump);
    ASSERT_NOT_NULL(strstr(dump, "xglobal-evidence v1 profile=native_release"));
    ASSERT_NOT_NULL(strstr(dump, "decl 0 id=2 module=1 kind=class"));
    ASSERT_NOT_NULL(strstr(dump, "class 0 id=2 module=1 decl=2 name=44 parent=0"));
    ASSERT_NOT_NULL(strstr(dump, "method 0 id=3 owner=2"));
    ASSERT_NOT_NULL(strstr(dump, "interface-impl 0 class=2 interface=123"));
    ASSERT_NOT_NULL(strstr(dump, "interface-extends 0 child=123 parent=124"));
    ASSERT_NOT_NULL(strstr(dump, "interface-method 0 id=7 owner=123 name=700"));
    ASSERT_NOT_NULL(strstr(dump, "body 0 func=4 module=1 decl=2"));
    ASSERT_NOT_NULL(strstr(dump, "name=88 sig=66"));
    ASSERT_NOT_NULL(strstr(dump, "kind=function"));
    ASSERT_NOT_NULL(strstr(dump, "callsite 0 id=5 owner=4 span=0 kind=method ordinal=2"));
    ASSERT_NOT_NULL(strstr(dump, "link-dep 0 id=6 module=1 decl=2 span=77 kind=extern_dylib"));

    xr_free(dump);
    xg_global_evidence_free(&ev);
}

TEST(global_evidence_lowers_to_aot_class_plans) {
    XgGlobalEvidence ev;
    XgBuildKey key = {.source_hash = 0x11,
                      .compiler_semver_hash = 0x22,
                      .profile_hash = 0x33,
                      .imported_summary_hash = 0x44,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    XgClassSummary base = {.class_id = 1,
                           .parent_class_id = XG_NO_ID,
                           .flags = XG_CLASS_HAS_SUBCLASS,
                           .field_start = 1,
                           .field_count = 1,
                           .method_start = 1,
                           .method_count = 1,
                           .interface_start = 0,
                           .interface_count = 0,
                           .decl_kind = XG_DECL_CLASS};
    XgClassSummary child = {.class_id = 2,
                            .parent_class_id = 1,
                            .flags = XG_CLASS_INFERRED_FINAL,
                            .field_start = 2,
                            .field_count = 1,
                            .method_start = 2,
                            .method_count = 1,
                            .interface_start = 1,
                            .interface_count = 1,
                            .decl_kind = XG_DECL_CLASS};
    XgMethodSummary base_method = {.method_id = 1,
                                   .owner_class_id = 1,
                                   .name_id = 900,
                                   .signature_key = 901,
                                   .override_of = XG_NO_ID,
                                   .default_arg_contract_id = XG_NO_ID,
                                   .flags = XG_METHOD_OVERRIDDEN};
    XgMethodSummary child_method = {.method_id = 2,
                                    .owner_class_id = 2,
                                    .name_id = 900,
                                    .signature_key = 901,
                                    .override_of = 1,
                                    .default_arg_contract_id = XG_NO_ID,
                                    .flags = 0};
    XgCallsiteSummary call = {.callsite_id = 7,
                              .owner_func_id = 99,
                              .kind = XG_CALL_METHOD,
                              .static_target_func_id = 0,
                              .receiver_static_class_id = 2,
                              .method_id = 2,
                              .method_name_id = 900,
                              .method_signature_key = 901,
                              .arg_type_key_start = 0,
                              .arg_count = 0,
                              .flags = 0};
    XgInterfaceImplSummary impl = {.implementor_class_id = 2,
                                   .interface_id = 77,
                                   .name_id = 77,
                                   .type_key = 770,
                                   .source_span_id = 1,
                                   .flags = 0};
    XgBodySummary cap_body = {.func_id = 123,
                              .module_id = 1,
                              .owner_decl_id = 42,
                              .owner_class_id = XG_NO_ID,
                              .owner_method_id = XG_NO_ID,
                              .name_id = 123,
                              .source_span_id = 5,
                              .kind = XG_BODY_FUNCTION,
                              .body_hash = 0x999,
                              .effect_bits = 0,
                              .escape_bits = 0,
                              .capability_bits = XG_CAP_COROUTINE | XG_CAP_CHANNEL,
                              .callsite_start = 0,
                              .callsite_count = 0,
                              .metadata_use_bits = XG_METADATA_TYPENAME,
                              .static_data_use_bits = XG_STATIC_DATA_COMPTIME_VALUE};
    XgLinkDependencySummary link_dep = {.link_id = 1,
                                        .module_id = 1,
                                        .decl_id = 42,
                                        .source_span_id = 5,
                                        .name_id = 501,
                                        .kind = XG_LINK_DEP_EXTERN_DYLIB,
                                        .flags = 0};
    XaotBundle bundle;
    char *dump;
    memcpy(link_dep.name, "xrayffi_smoke", sizeof("xrayffi_smoke"));

    xg_global_evidence_init(&ev, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &base));
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &child));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&ev, &base_method));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&ev, &child_method));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&ev, &call));
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_impl(&ev, &impl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&ev, &cap_body));
    ASSERT_NOT_NULL(xg_global_evidence_add_link_dependency(&ev, &link_dep));

    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(bundle.nclass_hierarchy_plans, 2);
    ASSERT_EQ_UINT(bundle.nclass_layout_plans, 2);
    ASSERT_EQ_UINT(bundle.nmethod_dispatch_plans, 1);
    ASSERT_EQ_UINT(bundle.ninterface_use_plans, 1);
    ASSERT_EQ_UINT(bundle.nmetadata_plans, 1);
    ASSERT_EQ_UINT(bundle.ncapability_plans, 2);
    ASSERT_EQ_UINT(bundle.nstatic_data_plans, 1);
    ASSERT_EQ_UINT(bundle.nlink_dependency_plans, 1);
    ASSERT_NOT_NULL(xaot_bundle_find_class_hierarchy_plan(&bundle, 1));
    ASSERT_NOT_NULL(xaot_bundle_find_class_layout_plan(&bundle, 2));
    const XaotMethodDispatchPlan *dispatch = xaot_bundle_find_method_dispatch_plan(&bundle, 7);
    ASSERT_NOT_NULL(dispatch);
    ASSERT_EQ_UINT(dispatch->kind, XAOT_DISPATCH_DIRECT);
    ASSERT_EQ_UINT(dispatch->source_span_id, 0);
    ASSERT_EQ_UINT(dispatch->target_count, 1);
    ASSERT_NOT_NULL(xaot_bundle_find_interface_use_plan(&bundle, 77, 2, XG_NO_ID));
    const XaotCapabilityPlan *cap = xaot_bundle_find_capability_plan(&bundle, XG_CAP_COROUTINE);
    ASSERT_NOT_NULL(cap);
    ASSERT_EQ_UINT(cap->body_count, 1);
    ASSERT_EQ_UINT(cap->profile_action, XAOT_CAPABILITY_ACTION_LINK);
    const XaotMetadataReachabilityPlan *metadata =
        xaot_bundle_find_metadata_plan(&bundle, XG_METADATA_TYPENAME);
    ASSERT_NOT_NULL(metadata);
    ASSERT_EQ_UINT(metadata->body_count, 1);
    ASSERT_EQ_UINT(metadata->decl_count, 0);
    ASSERT_EQ_UINT(metadata->profile_action, XAOT_CAPABILITY_ACTION_LINK);
    const XaotStaticDataPlan *static_data =
        xaot_bundle_find_static_data_plan(&bundle, XG_STATIC_DATA_COMPTIME_VALUE);
    ASSERT_NOT_NULL(static_data);
    ASSERT_EQ_UINT(static_data->body_count, 1);
    ASSERT_EQ_UINT(static_data->action, XAOT_STATIC_DATA_ACTION_MATERIALIZE);
    const XaotLinkDependencyPlan *link_plan =
        xaot_bundle_find_link_dependency_plan(&bundle, link_dep.link_id);
    ASSERT_NOT_NULL(link_plan);
    ASSERT_EQ_UINT(link_plan->kind, XG_LINK_DEP_EXTERN_DYLIB);
    ASSERT_STR_EQ(link_plan->name, "xrayffi_smoke");

    dump = xaot_bundle_dump_plan(&bundle);
    ASSERT_NOT_NULL(dump);
    ASSERT_NOT_NULL(strstr(dump, "class-hierarchy 0 id=1"));
    ASSERT_NOT_NULL(strstr(dump, "class-layout 1 id=2"));
    ASSERT_NOT_NULL(strstr(dump, "method-dispatch 0 callsite=7 span=0 kind=direct"));
    ASSERT_NOT_NULL(strstr(dump, "method_name=900 method_sig=901 args=0+0"));
    ASSERT_NOT_NULL(strstr(dump, "interface-use 0 interface=77 implementor=2"));
    ASSERT_NOT_NULL(strstr(dump, "metadata 0 name=typename bodies=1 decls=0 action=link"));
    ASSERT_NOT_NULL(strstr(dump, "capability 0 name=coroutine bodies=1 action=link"));
    ASSERT_NOT_NULL(strstr(dump, "static-data 0 name=comptime_value bodies=1 action=materialize"));
    ASSERT_NOT_NULL(strstr(dump, "link-dependency 0 id=1 kind=extern_dylib"));
    xr_free(dump);

    xaot_bundle_free(&bundle);
    xg_global_evidence_free(&ev);
}

TEST(global_evidence_verifier_rederives_dispatch_plans) {
    XgGlobalEvidence ev;
    XgBuildKey key = {.source_hash = 0x21,
                      .compiler_semver_hash = 0x22,
                      .profile_hash = 0x23,
                      .imported_summary_hash = 0x24,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    uint32_t shape_name_id = xg_name_id("Shape");
    uint32_t draw_name_id = xg_name_id("draw");
    XgClassSummary cls = {.class_id = 1,
                          .module_id = 1,
                          .decl_id = 1,
                          .name_id = shape_name_id,
                          .parent_class_id = XG_NO_ID,
                          .flags = XG_CLASS_INFERRED_FINAL,
                          .field_start = 0,
                          .field_count = 0,
                          .method_start = 1,
                          .method_count = 1,
                          .decl_kind = XG_DECL_CLASS};
    XgMethodSummary method = {.method_id = 1,
                              .owner_class_id = 1,
                              .name_id = draw_name_id,
                              .signature_key = 701,
                              .override_of = XG_NO_ID,
                              .default_arg_contract_id = XG_NO_ID,
                              .flags = 0};
    XgDeclSummary class_decl = {.module_id = 1,
                                .decl_id = 1,
                                .kind = XG_DECL_CLASS,
                                .name_id = shape_name_id,
                                .source_span_id = 40};
    XgCallsiteSummary call = {.callsite_id = 1,
                              .owner_func_id = 9,
                              .source_span_id = 42,
                              .body_ordinal = 0,
                              .kind = XG_CALL_METHOD,
                              .receiver_static_class_id = 1,
                              .method_id = 1,
                              .method_name_id = draw_name_id,
                              .method_signature_key = 701,
                              .arg_type_key_start = 25,
                              .arg_count = 1};
    XgDeclSummary body_decl = {
        .module_id = 1, .decl_id = 9, .kind = XG_DECL_FUNC, .name_id = 9, .source_span_id = 41};
    XgBodySummary body = {.func_id = 9,
                          .module_id = 1,
                          .owner_decl_id = 9,
                          .owner_class_id = XG_NO_ID,
                          .owner_method_id = XG_NO_ID,
                          .name_id = 9,
                          .source_span_id = 41,
                          .kind = XG_BODY_FUNCTION,
                          .body_hash = 0x999,
                          .callsite_start = 1,
                          .callsite_count = 1};
    XgBodySummary method_body = {.func_id = 10,
                                 .module_id = 1,
                                 .owner_decl_id = 1,
                                 .owner_class_id = 1,
                                 .owner_method_id = 1,
                                 .name_id = draw_name_id,
                                 .signature_key = 701,
                                 .source_span_id = 43,
                                 .kind = XG_BODY_METHOD,
                                 .body_hash = 0x1001};
    XiFunc init_func;
    XiFunc draw_func;
    XiFunc *children[1];
    XiClassMethod class_methods[1];
    uint16_t child_idx[1] = {0};
    XiClassData class_data;
    XiClassData *classes[1];
    memset(&init_func, 0, sizeof(init_func));
    memset(&draw_func, 0, sizeof(draw_func));
    memset(class_methods, 0, sizeof(class_methods));
    memset(&class_data, 0, sizeof(class_data));
    init_func.name = "init";
    draw_func.name = "draw";
    children[0] = &draw_func;
    init_func.children = children;
    init_func.nchildren = 1;
    class_methods[0].name = "draw";
    class_methods[0].symbol_id = 17;
    class_data.class_name = "Shape";
    class_data.methods = class_methods;
    class_data.nmethod = 1;
    class_data.ninst = 1;
    class_data.child_idx = child_idx;
    classes[0] = &class_data;
    XiModule module;
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    module.classes = classes;
    module.nclasses = 1;
    XiModule *modules[1] = {&module};
    char err[256];

    xg_global_evidence_init(&ev, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&ev, &class_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&ev, &body_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &cls));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&ev, &method));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&ev, &body));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&ev, &method_body));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&ev, &call));

    XaotBundle good;
    memset(&good, 0, sizeof(good));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&good, &ev, XG_BUILD_NATIVE_RELEASE));
    good.modules = modules;
    good.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&good, &init_func, 0, 0));
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&good, &draw_func, 0, 1));
    const char *target_prefix = NULL;
    ASSERT_EQ_PTR(xaot_bundle_find_method_func(&good, method.method_id, &target_prefix),
                  &draw_func);
    ASSERT_STR_EQ(target_prefix, "test");
    ASSERT_EQ_PTR(xaot_bundle_find_dispatch_target_func(&good, &good.dispatch_target_cases[0],
                                                        &target_prefix),
                  &draw_func);
    ASSERT_STR_EQ(target_prefix, "test");
    XiValue xi_call;
    memset(&xi_call, 0, sizeof(xi_call));
    xi_call.op = XI_CALL_METHOD;
    xi_call.nargs = 2;
    xi_call.aux = (void *) "draw";
    xi_call.line = 42;
    ASSERT_EQ_PTR(xaot_bundle_find_method_dispatch_plan_for_xi_call(&good, &xi_call),
                  &good.method_dispatch_plans[0]);
    xi_call.xg_callsite_id = call.callsite_id;
    xi_call.line = 0;
    ASSERT_EQ_PTR(xaot_bundle_find_method_dispatch_plan_for_xi_call(&good, &xi_call),
                  &good.method_dispatch_plans[0]);
    xi_call.line = 99;
    ASSERT_NULL(xaot_bundle_find_method_dispatch_plan_for_xi_call(&good, &xi_call));
    xi_call.line = 0;
    xi_call.aux = (void *) "other";
    ASSERT_NULL(xaot_bundle_find_method_dispatch_plan_for_xi_call(&good, &xi_call));
    xi_call.aux = (void *) "draw";
    xi_call.nargs = 3;
    ASSERT_NULL(xaot_bundle_find_method_dispatch_plan_for_xi_call(&good, &xi_call));
    xi_call.nargs = 2;
    xi_call.xg_callsite_id = XG_NO_ID;
    xi_call.line = 99;
    ASSERT_NULL(xaot_bundle_find_method_dispatch_plan_for_xi_call(&good, &xi_call));
    xi_call.line = 42;
    xi_call.aux = (void *) "other";
    ASSERT_NULL(xaot_bundle_find_method_dispatch_plan_for_xi_call(&good, &xi_call));
    xi_call.aux = NULL;
    ASSERT_NULL(xaot_bundle_find_method_dispatch_plan_for_xi_call(&good, &xi_call));
    xi_call.aux = (void *) "draw";
    good.method_dispatch_plans[0].method_name_id = 0;
    ASSERT_NULL(xaot_bundle_find_method_dispatch_plan_for_xi_call(&good, &xi_call));
    good.method_dispatch_plans[0].method_name_id = draw_name_id;
    memset(err, 0, sizeof(err));
    ASSERT_MSG(xaot_verify_bundle(&good, XAOT_VERIFY_AOT_READY, err, sizeof(err)), err);
    good.method_dispatch_plans[0].kind = XAOT_DISPATCH_VTABLE;
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&good, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT dispatch plan kind does not re-derive"));
    xaot_bundle_free(&good);

    XaotBundle stale_owner;
    memset(&stale_owner, 0, sizeof(stale_owner));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&stale_owner, &ev, XG_BUILD_NATIVE_RELEASE));
    stale_owner.modules = modules;
    stale_owner.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&stale_owner, &init_func, 0, 0));
    stale_owner.method_dispatch_plans[0].owner_func_id = 99;
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&stale_owner, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT dispatch plan owner function does not re-derive"));
    xaot_bundle_free(&stale_owner);

    XaotBundle stale_source;
    memset(&stale_source, 0, sizeof(stale_source));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&stale_source, &ev, XG_BUILD_NATIVE_RELEASE));
    stale_source.modules = modules;
    stale_source.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&stale_source, &init_func, 0, 0));
    stale_source.method_dispatch_plans[0].source_span_id = 99;
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&stale_source, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT dispatch plan source span does not re-derive"));
    xaot_bundle_free(&stale_source);

    XaotBundle stale_ordinal;
    memset(&stale_ordinal, 0, sizeof(stale_ordinal));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&stale_ordinal, &ev, XG_BUILD_NATIVE_RELEASE));
    stale_ordinal.modules = modules;
    stale_ordinal.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&stale_ordinal, &init_func, 0, 0));
    stale_ordinal.method_dispatch_plans[0].body_ordinal = 99;
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&stale_ordinal, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT dispatch plan body ordinal does not re-derive"));
    xaot_bundle_free(&stale_ordinal);

    XaotBundle stale_method_name;
    memset(&stale_method_name, 0, sizeof(stale_method_name));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&stale_method_name, &ev, XG_BUILD_NATIVE_RELEASE));
    stale_method_name.modules = modules;
    stale_method_name.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&stale_method_name, &init_func, 0, 0));
    stale_method_name.method_dispatch_plans[0].method_name_id = xg_name_id("other");
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&stale_method_name, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT dispatch plan method name does not re-derive"));
    xaot_bundle_free(&stale_method_name);

    XaotBundle stale_signature;
    memset(&stale_signature, 0, sizeof(stale_signature));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&stale_signature, &ev, XG_BUILD_NATIVE_RELEASE));
    stale_signature.modules = modules;
    stale_signature.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&stale_signature, &init_func, 0, 0));
    stale_signature.method_dispatch_plans[0].method_signature_key = 702;
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&stale_signature, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT dispatch plan method signature does not re-derive"));
    xaot_bundle_free(&stale_signature);

    XaotBundle stale_arg_range;
    memset(&stale_arg_range, 0, sizeof(stale_arg_range));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&stale_arg_range, &ev, XG_BUILD_NATIVE_RELEASE));
    stale_arg_range.modules = modules;
    stale_arg_range.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&stale_arg_range, &init_func, 0, 0));
    stale_arg_range.method_dispatch_plans[0].arg_type_key_start = 26;
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&stale_arg_range, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT dispatch plan argument type range does not re-derive"));
    xaot_bundle_free(&stale_arg_range);

    XaotBundle stale_arg_count;
    memset(&stale_arg_count, 0, sizeof(stale_arg_count));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&stale_arg_count, &ev, XG_BUILD_NATIVE_RELEASE));
    stale_arg_count.modules = modules;
    stale_arg_count.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&stale_arg_count, &init_func, 0, 0));
    stale_arg_count.method_dispatch_plans[0].arg_count = 2;
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&stale_arg_count, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT dispatch plan argument count does not re-derive"));
    xaot_bundle_free(&stale_arg_count);

    XaotBundle stale_target;
    memset(&stale_target, 0, sizeof(stale_target));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&stale_target, &ev, XG_BUILD_NATIVE_RELEASE));
    stale_target.modules = modules;
    stale_target.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&stale_target, &init_func, 0, 0));
    stale_target.method_dispatch_plans[0].target_start = 99;
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&stale_target, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT dispatch direct target does not re-derive"));
    xaot_bundle_free(&stale_target);

    XaotBundle stale_target_count;
    memset(&stale_target_count, 0, sizeof(stale_target_count));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&stale_target_count, &ev, XG_BUILD_NATIVE_RELEASE));
    stale_target_count.modules = modules;
    stale_target_count.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&stale_target_count, &init_func, 0, 0));
    stale_target_count.method_dispatch_plans[0].target_count = 2;
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&stale_target_count, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT dispatch direct target does not re-derive"));
    xaot_bundle_free(&stale_target_count);

    XaotBundle missing;
    memset(&missing, 0, sizeof(missing));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&missing, &ev, XG_BUILD_NATIVE_RELEASE));
    missing.modules = modules;
    missing.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&missing, &init_func, 0, 0));
    missing.nmethod_dispatch_plans = 0;
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&missing, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT method callsite has no dispatch plan"));
    xaot_bundle_free(&missing);

    xg_global_evidence_free(&ev);
}

TEST(global_evidence_attaches_callsite_ids_to_xi_calls) {
    XgGlobalEvidence ev;
    XgBuildKey key = {.source_hash = 0x31,
                      .compiler_semver_hash = 0x32,
                      .profile_hash = 0x33,
                      .imported_summary_hash = 0x34,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    uint32_t shape_name_id = xg_name_id("Shape");
    uint32_t draw_name_id = xg_name_id("draw");
    XgClassSummary cls = {.class_id = 1,
                          .module_id = 1,
                          .decl_id = 1,
                          .name_id = shape_name_id,
                          .parent_class_id = XG_NO_ID,
                          .flags = XG_CLASS_INFERRED_FINAL,
                          .method_start = 1,
                          .method_count = 1,
                          .decl_kind = XG_DECL_CLASS};
    XgMethodSummary method = {.method_id = 1,
                              .owner_class_id = 1,
                              .name_id = draw_name_id,
                              .signature_key = 701,
                              .override_of = XG_NO_ID,
                              .default_arg_contract_id = XG_NO_ID};
    XgCallsiteSummary call = {.callsite_id = 7,
                              .owner_func_id = 9,
                              .source_span_id = 42,
                              .kind = XG_CALL_METHOD,
                              .receiver_static_class_id = 1,
                              .method_id = 1,
                              .method_name_id = draw_name_id,
                              .method_signature_key = 701,
                              .arg_count = 1};

    XiFunc *init = xi_func_new("init", &stub_int_type);
    ASSERT_NOT_NULL(init);
    XiBlock *entry = xi_block_new(init);
    ASSERT_NOT_NULL(entry);
    XiValue *xi_call = xi_value_new(init, entry, XI_CALL_METHOD, &stub_int_type, 2);
    ASSERT_NOT_NULL(xi_call);
    xi_call->aux = (void *) "draw";
    xi_call->line = 42;

    XiModule module;
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = init;
    XiModule *modules[1] = {&module};

    xg_global_evidence_init(&ev, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &cls));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&ev, &method));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&ev, &call));

    XaotBundle bundle;
    memset(&bundle, 0, sizeof(bundle));
    bundle.modules = modules;
    bundle.nmodules = 1;
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(xi_call->xg_callsite_id, call.callsite_id);
    ASSERT_EQ_PTR(xaot_bundle_find_method_dispatch_plan_for_xi_call(&bundle, xi_call),
                  &bundle.method_dispatch_plans[0]);

    xaot_bundle_free(&bundle);
    xg_global_evidence_free(&ev);
    xi_func_free(init);
}

TEST(global_evidence_leaves_ambiguous_xi_callsite_unbound) {
    XgGlobalEvidence ev;
    XgBuildKey key = {.source_hash = 0x41,
                      .compiler_semver_hash = 0x42,
                      .profile_hash = 0x43,
                      .imported_summary_hash = 0x44,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    uint32_t shape_name_id = xg_name_id("Shape");
    uint32_t draw_name_id = xg_name_id("draw");
    XgClassSummary cls = {.class_id = 1,
                          .module_id = 1,
                          .decl_id = 1,
                          .name_id = shape_name_id,
                          .parent_class_id = XG_NO_ID,
                          .flags = XG_CLASS_INFERRED_FINAL,
                          .method_start = 1,
                          .method_count = 1,
                          .decl_kind = XG_DECL_CLASS};
    XgMethodSummary method = {.method_id = 1,
                              .owner_class_id = 1,
                              .name_id = draw_name_id,
                              .signature_key = 701,
                              .override_of = XG_NO_ID,
                              .default_arg_contract_id = XG_NO_ID};
    XgCallsiteSummary call1 = {.callsite_id = 1,
                               .owner_func_id = 9,
                               .source_span_id = 42,
                               .kind = XG_CALL_METHOD,
                               .receiver_static_class_id = 1,
                               .method_id = 1,
                               .method_name_id = draw_name_id,
                               .method_signature_key = 701,
                               .arg_count = 1};
    XgCallsiteSummary call2 = call1;
    call2.callsite_id = 2;
    call2.owner_func_id = 10;

    XiFunc *init = xi_func_new("init", &stub_int_type);
    ASSERT_NOT_NULL(init);
    XiBlock *entry = xi_block_new(init);
    ASSERT_NOT_NULL(entry);
    XiValue *xi_call = xi_value_new(init, entry, XI_CALL_METHOD, &stub_int_type, 2);
    ASSERT_NOT_NULL(xi_call);
    xi_call->aux = (void *) "draw";
    xi_call->line = 42;

    XiModule module;
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = init;
    XiModule *modules[1] = {&module};

    xg_global_evidence_init(&ev, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &cls));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&ev, &method));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&ev, &call1));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&ev, &call2));

    XaotBundle bundle;
    memset(&bundle, 0, sizeof(bundle));
    bundle.modules = modules;
    bundle.nmodules = 1;
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(xi_call->xg_callsite_id, XG_NO_ID);
    ASSERT_NULL(xaot_bundle_find_method_dispatch_plan_for_xi_call(&bundle, xi_call));

    xaot_bundle_free(&bundle);
    xg_global_evidence_free(&ev);
    xi_func_free(init);
}

TEST(global_evidence_lowers_interface_call_to_type_switch) {
    XgGlobalEvidence ev;
    XgBuildKey key = {.source_hash = 0x81,
                      .compiler_semver_hash = 0x82,
                      .profile_hash = 0x83,
                      .imported_summary_hash = 0x84,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    XgClassSummary circle = {.module_id = 1,
                             .decl_id = 1,
                             .class_id = 1,
                             .name_id = 101,
                             .parent_class_id = XG_NO_ID,
                             .flags = XG_CLASS_INFERRED_FINAL,
                             .method_start = 1,
                             .method_count = 1,
                             .interface_start = 1,
                             .interface_count = 1,
                             .decl_kind = XG_DECL_CLASS};
    XgClassSummary rect = {.module_id = 1,
                           .decl_id = 2,
                           .class_id = 2,
                           .name_id = 102,
                           .parent_class_id = XG_NO_ID,
                           .flags = XG_CLASS_INFERRED_FINAL,
                           .method_start = 2,
                           .method_count = 1,
                           .interface_start = 2,
                           .interface_count = 1,
                           .decl_kind = XG_DECL_CLASS};
    XgMethodSummary circle_draw = {.method_id = 1,
                                   .owner_class_id = 1,
                                   .name_id = 700,
                                   .signature_key = 701,
                                   .override_of = XG_NO_ID,
                                   .flags = 0};
    XgMethodSummary rect_draw = {.method_id = 2,
                                 .owner_class_id = 2,
                                 .name_id = 700,
                                 .signature_key = 701,
                                 .override_of = XG_NO_ID,
                                 .flags = 0};
    XgInterfaceImplSummary impls[] = {
        {.implementor_class_id = 1, .interface_id = 77, .name_id = 77, .type_key = 770},
        {.implementor_class_id = 2, .interface_id = 77, .name_id = 77, .type_key = 770},
    };
    XgInterfaceMethodSummary shape_draw = {.interface_method_id = 3,
                                           .owner_interface_id = 77,
                                           .name_id = 700,
                                           .signature_key = 701,
                                           .ordinal = 0,
                                           .source_span_id = 4};
    XgCallsiteSummary call = {.callsite_id = 1,
                              .owner_func_id = 9,
                              .body_ordinal = 0,
                              .kind = XG_CALL_INTERFACE,
                              .receiver_static_interface_id = 77,
                              .method_id = 3,
                              .method_name_id = 700,
                              .method_signature_key = 701};
    XgDeclSummary body_decl = {
        .module_id = 1, .decl_id = 9, .kind = XG_DECL_FUNC, .name_id = 9, .source_span_id = 1};
    XgDeclSummary circle_decl = {
        .module_id = 1, .decl_id = 1, .kind = XG_DECL_CLASS, .name_id = 101, .source_span_id = 2};
    XgDeclSummary rect_decl = {
        .module_id = 1, .decl_id = 2, .kind = XG_DECL_CLASS, .name_id = 102, .source_span_id = 3};
    XgDeclSummary shape_decl = {.module_id = 1,
                                .decl_id = 3,
                                .kind = XG_DECL_INTERFACE,
                                .name_id = 77,
                                .signature_key = 1,
                                .source_span_id = 4};
    XgBodySummary body = {.func_id = 9,
                          .module_id = 1,
                          .owner_decl_id = 9,
                          .owner_class_id = XG_NO_ID,
                          .owner_method_id = XG_NO_ID,
                          .name_id = 9,
                          .source_span_id = 1,
                          .kind = XG_BODY_FUNCTION,
                          .body_hash = 0x999,
                          .callsite_start = 1,
                          .callsite_count = 1};
    XgBodySummary circle_body = {.func_id = 10,
                                 .module_id = 1,
                                 .owner_decl_id = 1,
                                 .owner_class_id = 1,
                                 .owner_method_id = 1,
                                 .name_id = 700,
                                 .signature_key = 701,
                                 .source_span_id = 2,
                                 .kind = XG_BODY_METHOD,
                                 .body_hash = 0x1001};
    XgBodySummary rect_body = {.func_id = 11,
                               .module_id = 1,
                               .owner_decl_id = 2,
                               .owner_class_id = 2,
                               .owner_method_id = 2,
                               .name_id = 700,
                               .signature_key = 701,
                               .source_span_id = 3,
                               .kind = XG_BODY_METHOD,
                               .body_hash = 0x1002};
    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    XaotBundle bundle;
    const XaotMethodDispatchPlan *plan;
    char err[256];

    xg_global_evidence_init(&ev, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&ev, &circle_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&ev, &rect_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&ev, &shape_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&ev, &body_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &circle));
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &rect));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&ev, &circle_draw));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&ev, &rect_draw));
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_impl(&ev, &impls[0]));
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_impl(&ev, &impls[1]));
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_method(&ev, &shape_draw));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&ev, &body));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&ev, &circle_body));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&ev, &rect_body));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&ev, &call));

    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    modules[0] = &module;

    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    bundle.modules = modules;
    bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&bundle, &init_func, 0, 0));

    plan = xaot_bundle_find_method_dispatch_plan(&bundle, 1);
    ASSERT_NOT_NULL(plan);
    ASSERT_EQ_UINT(plan->kind, XAOT_DISPATCH_TYPE_SWITCH);
    ASSERT_EQ_UINT(plan->receiver_static_interface_id, 77);
    ASSERT_EQ_UINT(plan->method_name_id, 700);
    ASSERT_EQ_UINT(plan->method_signature_key, 701);
    ASSERT_EQ_UINT(plan->target_count, 2);
    ASSERT_EQ_UINT(bundle.ndispatch_target_cases, 2);
    ASSERT_EQ_UINT(bundle.dispatch_target_cases[0].receiver_class_id, 1);
    ASSERT_EQ_UINT(bundle.dispatch_target_cases[0].method_id, 1);
    ASSERT_EQ_UINT(bundle.dispatch_target_cases[1].receiver_class_id, 2);
    ASSERT_EQ_UINT(bundle.dispatch_target_cases[1].method_id, 2);
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));

    bundle.method_dispatch_plans[0].receiver_static_interface_id = 78;
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT dispatch plan receiver interface does not re-derive"));
    bundle.method_dispatch_plans[0].receiver_static_interface_id = 77;

    bundle.dispatch_target_cases[1].method_id = 1;
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT dispatch type-switch targets do not re-derive"));

    xaot_bundle_free(&bundle);
    xg_global_evidence_free(&ev);
}

static void assert_class_graph_verifier_rejects(const XgClassSummary *classes, uint32_t nclasses,
                                                const char *expected_error) {
    XgGlobalEvidence ev;
    XgBuildKey key = {.source_hash = 0x41,
                      .compiler_semver_hash = 0x42,
                      .profile_hash = 0x43,
                      .imported_summary_hash = 0x44,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    XaotBundle bundle;
    char err[256];

    xg_global_evidence_init(&ev, key);
    for (uint32_t i = 0; i < nclasses; i++)
        ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &classes[i]));

    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    modules[0] = &module;

    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    bundle.modules = modules;
    bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&bundle, &init_func, 0, 0));

    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, expected_error));

    xaot_bundle_free(&bundle);
    xg_global_evidence_free(&ev);
}

static void assert_method_graph_verifier_rejects(const XgClassSummary *classes, uint32_t nclasses,
                                                 const XgMethodSummary *methods, uint32_t nmethods,
                                                 const char *expected_error) {
    XgGlobalEvidence ev;
    XgBuildKey key = {.source_hash = 0x51,
                      .compiler_semver_hash = 0x52,
                      .profile_hash = 0x53,
                      .imported_summary_hash = 0x54,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    XaotBundle bundle;
    char err[256];

    xg_global_evidence_init(&ev, key);
    for (uint32_t i = 0; i < nclasses; i++)
        ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &classes[i]));
    for (uint32_t i = 0; i < nmethods; i++) {
        XgMethodSummary method = methods[i];
        method.flags |= XG_METHOD_NATIVE;
        ASSERT_NOT_NULL(xg_global_evidence_add_method(&ev, &method));
    }

    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    modules[0] = &module;

    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    bundle.modules = modules;
    bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&bundle, &init_func, 0, 0));

    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, expected_error));

    xaot_bundle_free(&bundle);
    xg_global_evidence_free(&ev);
}

static void assert_interface_graph_verifier_rejects(const XgClassSummary *classes,
                                                    uint32_t nclasses,
                                                    const XgInterfaceImplSummary *impls,
                                                    uint32_t nimpls, const char *expected_error) {
    XgGlobalEvidence ev;
    XgBuildKey key = {.source_hash = 0x61,
                      .compiler_semver_hash = 0x62,
                      .profile_hash = 0x63,
                      .imported_summary_hash = 0x64,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    XaotBundle bundle;
    char err[256];

    xg_global_evidence_init(&ev, key);
    for (uint32_t i = 0; i < nclasses; i++)
        ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &classes[i]));
    for (uint32_t i = 0; i < nimpls; i++)
        ASSERT_NOT_NULL(xg_global_evidence_add_interface_impl(&ev, &impls[i]));

    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    modules[0] = &module;

    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    bundle.modules = modules;
    bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&bundle, &init_func, 0, 0));

    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, expected_error));

    xaot_bundle_free(&bundle);
    xg_global_evidence_free(&ev);
}

TEST(global_evidence_verifier_rederives_class_graph_flags) {
    XgClassSummary missing_has_subclass[] = {
        {.class_id = 1,
         .parent_class_id = XG_NO_ID,
         .flags = XG_CLASS_INFERRED_FINAL,
         .decl_kind = XG_DECL_CLASS},
        {.class_id = 2,
         .parent_class_id = 1,
         .flags = XG_CLASS_INFERRED_FINAL,
         .decl_kind = XG_DECL_CLASS},
    };
    assert_class_graph_verifier_rejects(missing_has_subclass, 2,
                                        "has_subclass flag does not re-derive");

    XgClassSummary stale_inferred_final[] = {
        {.class_id = 1,
         .parent_class_id = XG_NO_ID,
         .flags = XG_CLASS_HAS_SUBCLASS | XG_CLASS_INFERRED_FINAL,
         .decl_kind = XG_DECL_CLASS},
        {.class_id = 2,
         .parent_class_id = 1,
         .flags = XG_CLASS_INFERRED_FINAL,
         .decl_kind = XG_DECL_CLASS},
    };
    assert_class_graph_verifier_rejects(stale_inferred_final, 2,
                                        "inferred-final flag does not re-derive");

    XgClassSummary final_with_subclass[] = {
        {.class_id = 1,
         .parent_class_id = XG_NO_ID,
         .flags = XG_CLASS_EXPLICIT_FINAL | XG_CLASS_HAS_SUBCLASS,
         .decl_kind = XG_DECL_CLASS},
        {.class_id = 2,
         .parent_class_id = 1,
         .flags = XG_CLASS_INFERRED_FINAL,
         .decl_kind = XG_DECL_CLASS},
    };
    assert_class_graph_verifier_rejects(final_with_subclass, 2, "final class has subclass");
}

TEST(global_evidence_verifier_rederives_method_override_graph) {
    XgClassSummary classes[] = {
        {.class_id = 1,
         .parent_class_id = XG_NO_ID,
         .flags = XG_CLASS_HAS_SUBCLASS,
         .method_start = 1,
         .method_count = 1,
         .decl_kind = XG_DECL_CLASS},
        {.class_id = 2,
         .parent_class_id = 1,
         .flags = XG_CLASS_INFERRED_FINAL,
         .method_start = 2,
         .method_count = 1,
         .decl_kind = XG_DECL_CLASS},
    };
    XgMethodSummary missing_override_of[] = {
        {.method_id = 1,
         .owner_class_id = 1,
         .name_id = 700,
         .signature_key = 701,
         .override_of = XG_NO_ID,
         .flags = XG_METHOD_OVERRIDDEN},
        {.method_id = 2,
         .owner_class_id = 2,
         .name_id = 700,
         .signature_key = 701,
         .override_of = XG_NO_ID,
         .flags = 0},
    };
    XgMethodSummary missing_overridden_flag[] = {
        {.method_id = 1,
         .owner_class_id = 1,
         .name_id = 700,
         .signature_key = 701,
         .override_of = XG_NO_ID,
         .flags = 0},
        {.method_id = 2,
         .owner_class_id = 2,
         .name_id = 700,
         .signature_key = 701,
         .override_of = 1,
         .flags = 0},
    };
    XgClassSummary unrelated_classes[] = {
        {.class_id = 1,
         .parent_class_id = XG_NO_ID,
         .flags = XG_CLASS_HAS_SUBCLASS,
         .method_start = 1,
         .method_count = 1,
         .decl_kind = XG_DECL_CLASS},
        {.class_id = 2,
         .parent_class_id = 1,
         .flags = XG_CLASS_INFERRED_FINAL,
         .method_start = 2,
         .method_count = 1,
         .decl_kind = XG_DECL_CLASS},
        {.class_id = 3,
         .parent_class_id = XG_NO_ID,
         .flags = XG_CLASS_INFERRED_FINAL,
         .method_start = 3,
         .method_count = 1,
         .decl_kind = XG_DECL_CLASS},
    };
    XgMethodSummary unrelated_override_of[] = {
        {.method_id = 1,
         .owner_class_id = 1,
         .name_id = 700,
         .signature_key = 701,
         .override_of = XG_NO_ID,
         .flags = XG_METHOD_OVERRIDDEN},
        {.method_id = 2,
         .owner_class_id = 2,
         .name_id = 700,
         .signature_key = 701,
         .override_of = 3,
         .flags = 0},
        {.method_id = 3,
         .owner_class_id = 3,
         .name_id = 700,
         .signature_key = 701,
         .override_of = XG_NO_ID,
         .flags = 0},
    };

    assert_method_graph_verifier_rejects(classes, 2, missing_override_of, 2,
                                         "method override_of does not re-derive");
    assert_method_graph_verifier_rejects(classes, 2, missing_overridden_flag, 2,
                                         "method overridden flag does not re-derive");
    assert_method_graph_verifier_rejects(unrelated_classes, 3, unrelated_override_of, 3,
                                         "method override_of does not re-derive");
}

TEST(global_evidence_verifier_rederives_interface_implementor_set) {
    XgClassSummary range_points_to_other_class[] = {
        {.class_id = 1,
         .parent_class_id = XG_NO_ID,
         .flags = XG_CLASS_INFERRED_FINAL,
         .interface_start = 1,
         .interface_count = 1,
         .decl_kind = XG_DECL_CLASS},
        {.class_id = 2,
         .parent_class_id = XG_NO_ID,
         .flags = XG_CLASS_INFERRED_FINAL,
         .decl_kind = XG_DECL_CLASS},
    };
    XgInterfaceImplSummary other_class_impl[] = {
        {.implementor_class_id = 2, .interface_id = 77, .name_id = 77, .type_key = 770},
    };
    XgClassSummary impl_outside_range[] = {
        {.class_id = 1,
         .parent_class_id = XG_NO_ID,
         .flags = XG_CLASS_INFERRED_FINAL,
         .decl_kind = XG_DECL_CLASS},
    };
    XgInterfaceImplSummary orphan_impl[] = {
        {.implementor_class_id = 1, .interface_id = 77, .name_id = 77, .type_key = 770},
    };
    XgClassSummary duplicate_classes[] = {
        {.class_id = 1,
         .parent_class_id = XG_NO_ID,
         .flags = XG_CLASS_INFERRED_FINAL,
         .interface_start = 1,
         .interface_count = 2,
         .decl_kind = XG_DECL_CLASS},
    };
    XgInterfaceImplSummary duplicate_impls[] = {
        {.implementor_class_id = 1, .interface_id = 77, .name_id = 77, .type_key = 770},
        {.implementor_class_id = 1, .interface_id = 77, .name_id = 77, .type_key = 770},
    };
    XgGlobalEvidence ev;
    XgBuildKey key = {.source_hash = 0x71,
                      .compiler_semver_hash = 0x72,
                      .profile_hash = 0x73,
                      .imported_summary_hash = 0x74,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    XgClassSummary valid_class = {.class_id = 1,
                                  .parent_class_id = XG_NO_ID,
                                  .flags = XG_CLASS_INFERRED_FINAL,
                                  .interface_start = 1,
                                  .interface_count = 1,
                                  .decl_kind = XG_DECL_CLASS};
    XgInterfaceImplSummary valid_impl = {
        .implementor_class_id = 1, .interface_id = 77, .name_id = 77, .type_key = 770};
    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    XaotBundle bundle;
    char err[256];

    assert_interface_graph_verifier_rejects(range_points_to_other_class, 2, other_class_impl, 1,
                                            "class interface range does not re-derive");
    assert_interface_graph_verifier_rejects(impl_outside_range, 1, orphan_impl, 1,
                                            "interface impl is outside implementor range");
    assert_interface_graph_verifier_rejects(duplicate_classes, 1, duplicate_impls, 2,
                                            "interface impl is duplicated");

    xg_global_evidence_init(&ev, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &valid_class));
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_impl(&ev, &valid_impl));
    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    modules[0] = &module;
    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    bundle.modules = modules;
    bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&bundle, &init_func, 0, 0));
    bundle.ninterface_use_plans = 0;
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT interface impl has no use plan"));
    xaot_bundle_free(&bundle);
    xg_global_evidence_free(&ev);
}

TEST(global_evidence_verifier_rederives_profile_actions) {
    XgGlobalEvidence ev;
    XgBuildKey key = {.source_hash = 0x31,
                      .compiler_semver_hash = 0x32,
                      .profile_hash = 0x33,
                      .imported_summary_hash = 0x34,
                      .module_id = 1,
                      .profile = XG_BUILD_FREESTANDING};
    XgDeclSummary decl = {.module_id = 1,
                          .decl_id = 1,
                          .kind = XG_DECL_FUNC,
                          .name_id = 7,
                          .signature_key = 901,
                          .source_span_id = 3};
    XgBodySummary body = {.func_id = 1,
                          .module_id = 1,
                          .owner_decl_id = 1,
                          .owner_class_id = XG_NO_ID,
                          .owner_method_id = XG_NO_ID,
                          .name_id = 7,
                          .signature_key = 901,
                          .source_span_id = 3,
                          .kind = XG_BODY_FUNCTION,
                          .body_hash = 0x42,
                          .effect_bits = 0,
                          .escape_bits = 0,
                          .capability_bits = XG_CAP_COROUTINE,
                          .metadata_use_bits = XG_METADATA_TYPENAME,
                          .static_data_use_bits = XG_STATIC_DATA_RUNTIME_INIT};
    XiFunc init_func;
    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    XiModule module;
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    XiModule *modules[1] = {&module};
    char err[256];

    xg_global_evidence_init(&ev, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&ev, &body));

    XaotBundle metadata_bad;
    memset(&metadata_bad, 0, sizeof(metadata_bad));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&metadata_bad, &ev, XG_BUILD_FREESTANDING));
    metadata_bad.modules = modules;
    metadata_bad.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&metadata_bad, &init_func, 0, 0));
    ASSERT_EQ_UINT(metadata_bad.nmetadata_plans, 1);
    ASSERT_EQ_UINT(metadata_bad.metadata_plans[0].profile_action, XAOT_CAPABILITY_ACTION_REJECT);
    metadata_bad.metadata_plans[0].profile_action = XAOT_CAPABILITY_ACTION_LINK;
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&metadata_bad, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT metadata profile action does not re-derive"));
    xaot_bundle_free(&metadata_bad);

    XaotBundle capability_bad;
    memset(&capability_bad, 0, sizeof(capability_bad));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&capability_bad, &ev, XG_BUILD_FREESTANDING));
    capability_bad.modules = modules;
    capability_bad.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&capability_bad, &init_func, 0, 0));
    ASSERT_EQ_UINT(capability_bad.ncapability_plans, 1);
    ASSERT_EQ_UINT(capability_bad.capability_plans[0].profile_action,
                   XAOT_CAPABILITY_ACTION_REJECT);
    capability_bad.capability_plans[0].profile_action = XAOT_CAPABILITY_ACTION_LINK;
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&capability_bad, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT capability profile action does not re-derive"));
    xaot_bundle_free(&capability_bad);

    XaotBundle static_bad;
    memset(&static_bad, 0, sizeof(static_bad));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&static_bad, &ev, XG_BUILD_FREESTANDING));
    static_bad.modules = modules;
    static_bad.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&static_bad, &init_func, 0, 0));
    ASSERT_EQ_UINT(static_bad.nstatic_data_plans, 1);
    ASSERT_EQ_UINT(static_bad.static_data_plans[0].action, XAOT_STATIC_DATA_ACTION_REJECT);
    static_bad.static_data_plans[0].action = XAOT_STATIC_DATA_ACTION_MATERIALIZE;
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&static_bad, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT static-data action does not re-derive"));
    xaot_bundle_free(&static_bad);

    xg_global_evidence_free(&ev);
}

TEST(global_evidence_verifier_rederives_body_callsite_ranges) {
    XgGlobalEvidence ev;
    XgBuildKey key = {.source_hash = 0x41,
                      .compiler_semver_hash = 0x42,
                      .profile_hash = 0x43,
                      .imported_summary_hash = 0x44,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    XgDeclSummary decl = {.module_id = 1,
                          .decl_id = 1,
                          .kind = XG_DECL_FUNC,
                          .name_id = 7,
                          .signature_key = 901,
                          .source_span_id = 3};
    XgDeclSummary target_decl = {.module_id = 1,
                                 .decl_id = 2,
                                 .kind = XG_DECL_FUNC,
                                 .name_id = 8,
                                 .signature_key = 902,
                                 .source_span_id = 6};
    XgBodySummary body = {.func_id = 1,
                          .module_id = 1,
                          .owner_decl_id = 1,
                          .owner_class_id = XG_NO_ID,
                          .owner_method_id = XG_NO_ID,
                          .name_id = 7,
                          .signature_key = 901,
                          .source_span_id = 3,
                          .kind = XG_BODY_FUNCTION,
                          .body_hash = 0x4242,
                          .callsite_start = 1,
                          .callsite_count = 1};
    XgBodySummary target_body = {.func_id = 2,
                                 .module_id = 1,
                                 .owner_decl_id = 2,
                                 .owner_class_id = XG_NO_ID,
                                 .owner_method_id = XG_NO_ID,
                                 .name_id = 8,
                                 .signature_key = 902,
                                 .source_span_id = 6,
                                 .kind = XG_BODY_FUNCTION,
                                 .body_hash = 0x4343};
    XgCallsiteSummary call = {.callsite_id = 1,
                              .owner_func_id = 1,
                              .source_span_id = 4,
                              .body_ordinal = 0,
                              .kind = XG_CALL_DIRECT_FUNC,
                              .static_target_func_id = 2};
    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    char err[256];

    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    modules[0] = &module;

    xg_global_evidence_init(&ev, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&ev, &target_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&ev, &body));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&ev, &target_body));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&ev, &call));

    XaotBundle good;
    memset(&good, 0, sizeof(good));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&good, &ev, XG_BUILD_NATIVE_RELEASE));
    good.modules = modules;
    good.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&good, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_MSG(xaot_verify_bundle(&good, XAOT_VERIFY_AOT_READY, err, sizeof(err)), err);
    xaot_bundle_free(&good);

    XaotBundle stale_kind;
    ev.bodies[0].kind = 0;
    memset(&stale_kind, 0, sizeof(stale_kind));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&stale_kind, &ev, XG_BUILD_NATIVE_RELEASE));
    stale_kind.modules = modules;
    stale_kind.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&stale_kind, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&stale_kind, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence body kind is invalid"));
    xaot_bundle_free(&stale_kind);
    ev.bodies[0].kind = XG_BODY_FUNCTION;

    XaotBundle stale_decl;
    ev.bodies[0].owner_decl_id = 999;
    memset(&stale_decl, 0, sizeof(stale_decl));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&stale_decl, &ev, XG_BUILD_NATIVE_RELEASE));
    stale_decl.modules = modules;
    stale_decl.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&stale_decl, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&stale_decl, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence function body owner decl is missing"));
    xaot_bundle_free(&stale_decl);
    ev.bodies[0].owner_decl_id = 1;

    XaotBundle stale_signature;
    ev.bodies[0].signature_key = 902;
    memset(&stale_signature, 0, sizeof(stale_signature));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&stale_signature, &ev, XG_BUILD_NATIVE_RELEASE));
    stale_signature.modules = modules;
    stale_signature.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&stale_signature, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&stale_signature, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence function body owner decl does not re-derive"));
    xaot_bundle_free(&stale_signature);
    ev.bodies[0].signature_key = 901;

    XaotBundle stale_range;
    ev.bodies[0].callsite_start = 2;
    memset(&stale_range, 0, sizeof(stale_range));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&stale_range, &ev, XG_BUILD_NATIVE_RELEASE));
    stale_range.modules = modules;
    stale_range.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&stale_range, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&stale_range, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_MSG(strstr(err, "AOT global evidence body callsite range is stale") ||
                   strstr(err, "AOT global evidence callsite has no body"),
               err);
    xaot_bundle_free(&stale_range);
    ev.bodies[0].callsite_start = 1;

    XaotBundle stale_owner;
    ev.callsites[0].owner_func_id = 2;
    memset(&stale_owner, 0, sizeof(stale_owner));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&stale_owner, &ev, XG_BUILD_NATIVE_RELEASE));
    stale_owner.modules = modules;
    stale_owner.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&stale_owner, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&stale_owner, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence body callsite owner does not re-derive"));
    xaot_bundle_free(&stale_owner);
    ev.callsites[0].owner_func_id = 1;

    XaotBundle stale_ordinal;
    ev.callsites[0].body_ordinal = 1;
    memset(&stale_ordinal, 0, sizeof(stale_ordinal));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&stale_ordinal, &ev, XG_BUILD_NATIVE_RELEASE));
    stale_ordinal.modules = modules;
    stale_ordinal.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&stale_ordinal, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&stale_ordinal, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence body callsite ordinal does not re-derive"));
    xaot_bundle_free(&stale_ordinal);

    XaotBundle orphan_callsite;
    ev.callsites[0].owner_func_id = 1;
    ev.callsites[0].body_ordinal = 0;
    ev.bodies[0].callsite_count = 0;
    ev.bodies[0].callsite_start = 0;
    memset(&orphan_callsite, 0, sizeof(orphan_callsite));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&orphan_callsite, &ev, XG_BUILD_NATIVE_RELEASE));
    orphan_callsite.modules = modules;
    orphan_callsite.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&orphan_callsite, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&orphan_callsite, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence callsite has no body"));
    xaot_bundle_free(&orphan_callsite);
    ev.bodies[0].callsite_start = 1;
    ev.bodies[0].callsite_count = 1;

    xg_global_evidence_free(&ev);
}

TEST(global_evidence_verifier_rejects_stale_callsite_identity_rows) {
    XgBuildKey key = {.source_hash = 0x71,
                      .compiler_semver_hash = 0x72,
                      .profile_hash = 0x73,
                      .imported_summary_hash = 0x74,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    XgDeclSummary decl = {.module_id = 1,
                          .decl_id = 1,
                          .kind = XG_DECL_FUNC,
                          .name_id = 11,
                          .signature_key = 22,
                          .source_span_id = 3};
    XgBodySummary body = {.func_id = 1,
                          .module_id = 1,
                          .owner_decl_id = 1,
                          .name_id = 11,
                          .signature_key = 22,
                          .source_span_id = 3,
                          .kind = XG_BODY_FUNCTION,
                          .body_hash = 0x7171};
    XgCallsiteSummary call = {.callsite_id = 1,
                              .owner_func_id = 1,
                              .source_span_id = 4,
                              .body_ordinal = 0,
                              .kind = XG_CALL_DIRECT_FUNC,
                              .static_target_func_id = 1};
    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    char err[256];

    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    modules[0] = &module;

    XgGlobalEvidence missing_id_ev;
    XgBuildKey missing_id_key = key;
    XgCallsiteSummary missing_id_call = call;
    missing_id_key.source_hash = 0x75;
    missing_id_call.callsite_id = XG_NO_ID;
    xg_global_evidence_init(&missing_id_ev, missing_id_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&missing_id_ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&missing_id_ev, &body));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&missing_id_ev, &missing_id_call));

    XaotBundle missing_id_bundle;
    memset(&missing_id_bundle, 0, sizeof(missing_id_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&missing_id_bundle, &missing_id_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    missing_id_bundle.modules = modules;
    missing_id_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&missing_id_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&missing_id_bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence callsite has no id"));
    xaot_bundle_free(&missing_id_bundle);
    xg_global_evidence_free(&missing_id_ev);

    XgGlobalEvidence duplicate_id_ev;
    XgBuildKey duplicate_id_key = key;
    XgCallsiteSummary duplicate_calls[2] = {call, call};
    XgBodySummary duplicate_body = body;
    duplicate_id_key.source_hash = 0x76;
    duplicate_body.callsite_start = 1;
    duplicate_body.callsite_count = 1;
    xg_global_evidence_init(&duplicate_id_ev, duplicate_id_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&duplicate_id_ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&duplicate_id_ev, &duplicate_body));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&duplicate_id_ev, &duplicate_calls[0]));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&duplicate_id_ev, &duplicate_calls[1]));

    XaotBundle duplicate_id_bundle;
    memset(&duplicate_id_bundle, 0, sizeof(duplicate_id_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&duplicate_id_bundle, &duplicate_id_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    duplicate_id_bundle.modules = modules;
    duplicate_id_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&duplicate_id_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&duplicate_id_bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence callsite id is duplicated"));
    xaot_bundle_free(&duplicate_id_bundle);
    xg_global_evidence_free(&duplicate_id_ev);

    XgGlobalEvidence range_hole_ev;
    XgBuildKey range_hole_key = key;
    XgBodySummary range_hole_body = body;
    range_hole_key.source_hash = 0x77;
    range_hole_body.callsite_start = 1;
    range_hole_body.callsite_count = 2;
    xg_global_evidence_init(&range_hole_ev, range_hole_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&range_hole_ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&range_hole_ev, &range_hole_body));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&range_hole_ev, &call));

    XaotBundle range_hole_bundle;
    memset(&range_hole_bundle, 0, sizeof(range_hole_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&range_hole_bundle, &range_hole_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    range_hole_bundle.modules = modules;
    range_hole_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&range_hole_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&range_hole_bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence body callsite range is stale"));
    xaot_bundle_free(&range_hole_bundle);
    xg_global_evidence_free(&range_hole_ev);

    XgGlobalEvidence invalid_kind_ev;
    XgBuildKey invalid_kind_key = key;
    XgBodySummary invalid_kind_body = body;
    XgCallsiteSummary invalid_kind_call = call;
    invalid_kind_key.source_hash = 0x78;
    invalid_kind_body.callsite_start = 1;
    invalid_kind_body.callsite_count = 1;
    invalid_kind_call.kind = 0;
    xg_global_evidence_init(&invalid_kind_ev, invalid_kind_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&invalid_kind_ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&invalid_kind_ev, &invalid_kind_body));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&invalid_kind_ev, &invalid_kind_call));

    XaotBundle invalid_kind_bundle;
    memset(&invalid_kind_bundle, 0, sizeof(invalid_kind_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&invalid_kind_bundle, &invalid_kind_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    invalid_kind_bundle.modules = modules;
    invalid_kind_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&invalid_kind_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&invalid_kind_bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence callsite kind is invalid"));
    xaot_bundle_free(&invalid_kind_bundle);
    xg_global_evidence_free(&invalid_kind_ev);

    XgCallsiteSummary direct_with_method_payload = {.callsite_id = 1,
                                                    .owner_func_id = 1,
                                                    .source_span_id = 4,
                                                    .body_ordinal = 0,
                                                    .kind = XG_CALL_DIRECT_FUNC,
                                                    .static_target_func_id = 1,
                                                    .method_id = 123};
    assert_single_callsite_rejected(&direct_with_method_payload,
                                    "AOT global evidence direct callsite identity is stale");

    XgCallsiteSummary method_with_direct_payload = {.callsite_id = 1,
                                                    .owner_func_id = 1,
                                                    .source_span_id = 4,
                                                    .body_ordinal = 0,
                                                    .kind = XG_CALL_METHOD,
                                                    .static_target_func_id = 1,
                                                    .receiver_static_class_id = 1,
                                                    .method_id = 123,
                                                    .method_name_id = 123,
                                                    .method_signature_key = 456};
    assert_single_callsite_rejected(&method_with_direct_payload,
                                    "AOT global evidence method callsite identity is stale");

    XgCallsiteSummary interface_with_class_payload = {.callsite_id = 1,
                                                      .owner_func_id = 1,
                                                      .source_span_id = 4,
                                                      .body_ordinal = 0,
                                                      .kind = XG_CALL_INTERFACE,
                                                      .receiver_static_class_id = 1,
                                                      .receiver_static_interface_id = 123,
                                                      .method_id = 123,
                                                      .method_name_id = 123,
                                                      .method_signature_key = 456};
    assert_single_callsite_rejected(&interface_with_class_payload,
                                    "AOT global evidence interface callsite identity is stale");

    XgCallsiteSummary native_with_receiver_payload = {.callsite_id = 1,
                                                      .owner_func_id = 1,
                                                      .source_span_id = 4,
                                                      .body_ordinal = 0,
                                                      .kind = XG_CALL_NATIVE,
                                                      .receiver_static_class_id = 1,
                                                      .method_id = 123,
                                                      .method_name_id = 123};
    assert_single_callsite_rejected(&native_with_receiver_payload,
                                    "AOT global evidence native callsite identity is stale");

    XgCallsiteSummary extern_with_signature_payload = {.callsite_id = 1,
                                                       .owner_func_id = 1,
                                                       .source_span_id = 4,
                                                       .body_ordinal = 0,
                                                       .kind = XG_CALL_EXTERN,
                                                       .method_id = 123,
                                                       .method_name_id = 123,
                                                       .method_signature_key = 456};
    assert_single_callsite_rejected(&extern_with_signature_payload,
                                    "AOT global evidence extern callsite identity is stale");

    XgGlobalEvidence closure_stale_ev;
    XgBuildKey closure_stale_key = key;
    XgBodySummary closure_stale_body = body;
    XgCallsiteSummary closure_stale_call = call;
    closure_stale_key.source_hash = 0x85;
    closure_stale_body.callsite_start = 1;
    closure_stale_body.callsite_count = 1;
    closure_stale_call.kind = XG_CALL_CLOSURE;
    closure_stale_call.static_target_func_id = 123;
    xg_global_evidence_init(&closure_stale_ev, closure_stale_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&closure_stale_ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&closure_stale_ev, &closure_stale_body));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&closure_stale_ev, &closure_stale_call));

    XaotBundle closure_stale_bundle;
    memset(&closure_stale_bundle, 0, sizeof(closure_stale_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&closure_stale_bundle, &closure_stale_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    closure_stale_bundle.modules = modules;
    closure_stale_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&closure_stale_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(
        !xaot_verify_bundle(&closure_stale_bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence closure callsite identity is stale"));
    xaot_bundle_free(&closure_stale_bundle);
    xg_global_evidence_free(&closure_stale_ev);

    XgGlobalEvidence direct_no_target_ev;
    XgBuildKey direct_no_target_key = key;
    XgBodySummary direct_no_target_body = body;
    XgCallsiteSummary direct_no_target_call = call;
    direct_no_target_key.source_hash = 0x79;
    direct_no_target_body.callsite_start = 1;
    direct_no_target_body.callsite_count = 1;
    direct_no_target_call.static_target_func_id = XG_NO_ID;
    xg_global_evidence_init(&direct_no_target_ev, direct_no_target_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&direct_no_target_ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&direct_no_target_ev, &direct_no_target_body));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&direct_no_target_ev, &direct_no_target_call));

    XaotBundle direct_no_target_bundle;
    memset(&direct_no_target_bundle, 0, sizeof(direct_no_target_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&direct_no_target_bundle, &direct_no_target_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    direct_no_target_bundle.modules = modules;
    direct_no_target_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&direct_no_target_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(
        !xaot_verify_bundle(&direct_no_target_bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence direct callsite has no target"));
    xaot_bundle_free(&direct_no_target_bundle);
    xg_global_evidence_free(&direct_no_target_ev);

    XgGlobalEvidence direct_missing_body_ev;
    XgBuildKey direct_missing_body_key = key;
    XgBodySummary direct_missing_body_body = body;
    XgCallsiteSummary direct_missing_body_call = call;
    direct_missing_body_key.source_hash = 0x81;
    direct_missing_body_body.callsite_start = 1;
    direct_missing_body_body.callsite_count = 1;
    direct_missing_body_call.static_target_func_id = 999;
    xg_global_evidence_init(&direct_missing_body_ev, direct_missing_body_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&direct_missing_body_ev, &decl));
    ASSERT_NOT_NULL(
        xg_global_evidence_add_body(&direct_missing_body_ev, &direct_missing_body_body));
    ASSERT_NOT_NULL(
        xg_global_evidence_add_callsite(&direct_missing_body_ev, &direct_missing_body_call));

    XaotBundle direct_missing_body_bundle;
    memset(&direct_missing_body_bundle, 0, sizeof(direct_missing_body_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&direct_missing_body_bundle,
                                                &direct_missing_body_ev, XG_BUILD_NATIVE_RELEASE));
    direct_missing_body_bundle.modules = modules;
    direct_missing_body_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&direct_missing_body_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(
        !xaot_verify_bundle(&direct_missing_body_bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence direct callsite target body is missing"));
    xaot_bundle_free(&direct_missing_body_bundle);
    xg_global_evidence_free(&direct_missing_body_ev);

    XgGlobalEvidence method_stale_ev;
    XgBuildKey method_stale_key = key;
    XgBodySummary method_stale_body = body;
    XgCallsiteSummary method_stale_call = call;
    method_stale_key.source_hash = 0x7a;
    method_stale_body.callsite_start = 1;
    method_stale_body.callsite_count = 1;
    method_stale_call.kind = XG_CALL_METHOD;
    method_stale_call.static_target_func_id = XG_NO_ID;
    method_stale_call.receiver_static_class_id = 1;
    method_stale_call.method_id = XG_NO_ID;
    method_stale_call.method_name_id = 333;
    method_stale_call.method_signature_key = 444;
    xg_global_evidence_init(&method_stale_ev, method_stale_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&method_stale_ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&method_stale_ev, &method_stale_body));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&method_stale_ev, &method_stale_call));

    XaotBundle method_stale_bundle;
    memset(&method_stale_bundle, 0, sizeof(method_stale_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&method_stale_bundle, &method_stale_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    method_stale_bundle.modules = modules;
    method_stale_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&method_stale_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&method_stale_bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence method callsite identity is stale"));
    xaot_bundle_free(&method_stale_bundle);
    xg_global_evidence_free(&method_stale_ev);

    XgGlobalEvidence method_no_receiver_ev;
    XgBuildKey method_no_receiver_key = key;
    XgBodySummary method_no_receiver_body = body;
    XgCallsiteSummary method_no_receiver_call = call;
    method_no_receiver_key.source_hash = 0x7c;
    method_no_receiver_body.callsite_start = 1;
    method_no_receiver_body.callsite_count = 1;
    method_no_receiver_call.kind = XG_CALL_METHOD;
    method_no_receiver_call.static_target_func_id = XG_NO_ID;
    method_no_receiver_call.receiver_static_class_id = XG_NO_ID;
    method_no_receiver_call.method_id = 333;
    method_no_receiver_call.method_name_id = 333;
    method_no_receiver_call.method_signature_key = 444;
    xg_global_evidence_init(&method_no_receiver_ev, method_no_receiver_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&method_no_receiver_ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&method_no_receiver_ev, &method_no_receiver_body));
    ASSERT_NOT_NULL(
        xg_global_evidence_add_callsite(&method_no_receiver_ev, &method_no_receiver_call));

    XaotBundle method_no_receiver_bundle;
    memset(&method_no_receiver_bundle, 0, sizeof(method_no_receiver_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&method_no_receiver_bundle, &method_no_receiver_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    method_no_receiver_bundle.modules = modules;
    method_no_receiver_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&method_no_receiver_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(
        !xaot_verify_bundle(&method_no_receiver_bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence method callsite identity is stale"));
    xaot_bundle_free(&method_no_receiver_bundle);
    xg_global_evidence_free(&method_no_receiver_ev);

    XgGlobalEvidence method_no_signature_ev;
    XgBuildKey method_no_signature_key = key;
    XgBodySummary method_no_signature_body = body;
    XgCallsiteSummary method_no_signature_call = call;
    method_no_signature_key.source_hash = 0x7d;
    method_no_signature_body.callsite_start = 1;
    method_no_signature_body.callsite_count = 1;
    method_no_signature_call.kind = XG_CALL_METHOD;
    method_no_signature_call.static_target_func_id = XG_NO_ID;
    method_no_signature_call.receiver_static_class_id = 1;
    method_no_signature_call.method_id = 333;
    method_no_signature_call.method_name_id = 333;
    method_no_signature_call.method_signature_key = 0;
    xg_global_evidence_init(&method_no_signature_ev, method_no_signature_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&method_no_signature_ev, &decl));
    ASSERT_NOT_NULL(
        xg_global_evidence_add_body(&method_no_signature_ev, &method_no_signature_body));
    ASSERT_NOT_NULL(
        xg_global_evidence_add_callsite(&method_no_signature_ev, &method_no_signature_call));

    XaotBundle method_no_signature_bundle;
    memset(&method_no_signature_bundle, 0, sizeof(method_no_signature_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&method_no_signature_bundle,
                                                &method_no_signature_ev, XG_BUILD_NATIVE_RELEASE));
    method_no_signature_bundle.modules = modules;
    method_no_signature_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&method_no_signature_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(
        !xaot_verify_bundle(&method_no_signature_bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence method callsite identity is stale"));
    xaot_bundle_free(&method_no_signature_bundle);
    xg_global_evidence_free(&method_no_signature_ev);

    XgGlobalEvidence method_wrong_target_ev;
    XgBuildKey method_wrong_target_key = key;
    XgDeclSummary method_owner_decl = {
        .module_id = 1, .decl_id = 2, .kind = XG_DECL_CLASS, .name_id = 500, .source_span_id = 5};
    XgClassSummary method_owner_class = {.module_id = 1,
                                         .decl_id = 2,
                                         .class_id = 1,
                                         .name_id = 500,
                                         .flags = XG_CLASS_INFERRED_FINAL,
                                         .method_start = 1,
                                         .method_count = 1,
                                         .decl_kind = XG_DECL_CLASS};
    XgMethodSummary real_method = {
        .method_id = 501, .owner_class_id = 1, .name_id = 502, .signature_key = 503};
    XgBodySummary method_wrong_target_body = body;
    XgCallsiteSummary method_wrong_target_call = call;
    method_wrong_target_key.source_hash = 0x83;
    method_wrong_target_body.callsite_start = 1;
    method_wrong_target_body.callsite_count = 1;
    method_wrong_target_call.kind = XG_CALL_METHOD;
    method_wrong_target_call.static_target_func_id = XG_NO_ID;
    method_wrong_target_call.receiver_static_class_id = 1;
    method_wrong_target_call.method_id = 999;
    method_wrong_target_call.method_name_id = 502;
    method_wrong_target_call.method_signature_key = 503;
    xg_global_evidence_init(&method_wrong_target_ev, method_wrong_target_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&method_wrong_target_ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&method_wrong_target_ev, &method_owner_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&method_wrong_target_ev, &method_owner_class));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&method_wrong_target_ev, &real_method));
    ASSERT_NOT_NULL(
        xg_global_evidence_add_body(&method_wrong_target_ev, &method_wrong_target_body));
    ASSERT_NOT_NULL(
        xg_global_evidence_add_callsite(&method_wrong_target_ev, &method_wrong_target_call));

    XaotBundle method_wrong_target_bundle;
    memset(&method_wrong_target_bundle, 0, sizeof(method_wrong_target_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&method_wrong_target_bundle,
                                                &method_wrong_target_ev, XG_BUILD_NATIVE_RELEASE));
    method_wrong_target_bundle.modules = modules;
    method_wrong_target_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&method_wrong_target_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(
        !xaot_verify_bundle(&method_wrong_target_bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence method callsite target does not re-derive"));
    xaot_bundle_free(&method_wrong_target_bundle);
    xg_global_evidence_free(&method_wrong_target_ev);

    XgGlobalEvidence interface_stale_ev;
    XgBuildKey interface_stale_key = key;
    XgBodySummary interface_stale_body = body;
    XgCallsiteSummary interface_stale_call = call;
    interface_stale_key.source_hash = 0x7e;
    interface_stale_body.callsite_start = 1;
    interface_stale_body.callsite_count = 1;
    interface_stale_call.kind = XG_CALL_INTERFACE;
    interface_stale_call.static_target_func_id = XG_NO_ID;
    interface_stale_call.receiver_static_interface_id = XG_NO_ID;
    interface_stale_call.method_id = 444;
    interface_stale_call.method_name_id = 444;
    interface_stale_call.method_signature_key = 555;
    xg_global_evidence_init(&interface_stale_ev, interface_stale_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&interface_stale_ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&interface_stale_ev, &interface_stale_body));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&interface_stale_ev, &interface_stale_call));

    XaotBundle interface_stale_bundle;
    memset(&interface_stale_bundle, 0, sizeof(interface_stale_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&interface_stale_bundle, &interface_stale_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    interface_stale_bundle.modules = modules;
    interface_stale_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&interface_stale_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(
        !xaot_verify_bundle(&interface_stale_bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence interface callsite identity is stale"));
    xaot_bundle_free(&interface_stale_bundle);
    xg_global_evidence_free(&interface_stale_ev);

    XgGlobalEvidence interface_missing_decl_ev;
    XgBuildKey interface_missing_decl_key = key;
    XgBodySummary interface_missing_decl_body = body;
    XgCallsiteSummary interface_missing_decl_call = call;
    interface_missing_decl_key.source_hash = 0x84;
    interface_missing_decl_body.callsite_start = 1;
    interface_missing_decl_body.callsite_count = 1;
    interface_missing_decl_call.kind = XG_CALL_INTERFACE;
    interface_missing_decl_call.static_target_func_id = XG_NO_ID;
    interface_missing_decl_call.receiver_static_interface_id = 444;
    interface_missing_decl_call.method_id = 555;
    interface_missing_decl_call.method_name_id = 555;
    interface_missing_decl_call.method_signature_key = 666;
    xg_global_evidence_init(&interface_missing_decl_ev, interface_missing_decl_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&interface_missing_decl_ev, &decl));
    ASSERT_NOT_NULL(
        xg_global_evidence_add_body(&interface_missing_decl_ev, &interface_missing_decl_body));
    ASSERT_NOT_NULL(
        xg_global_evidence_add_callsite(&interface_missing_decl_ev, &interface_missing_decl_call));

    XaotBundle interface_missing_decl_bundle;
    memset(&interface_missing_decl_bundle, 0, sizeof(interface_missing_decl_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(
        &interface_missing_decl_bundle, &interface_missing_decl_ev, XG_BUILD_NATIVE_RELEASE));
    interface_missing_decl_bundle.modules = modules;
    interface_missing_decl_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&interface_missing_decl_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&interface_missing_decl_bundle, XAOT_VERIFY_AOT_READY, err,
                                    sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence interface callsite declaration is missing"));
    xaot_bundle_free(&interface_missing_decl_bundle);
    xg_global_evidence_free(&interface_missing_decl_ev);

    XgGlobalEvidence interface_missing_method_ev;
    XgBuildKey interface_missing_method_key = key;
    XgDeclSummary interface_decl = {.module_id = 1,
                                    .decl_id = 2,
                                    .kind = XG_DECL_INTERFACE,
                                    .name_id = 444,
                                    .signature_key = 0,
                                    .source_span_id = 8};
    XgBodySummary interface_missing_method_body = body;
    XgCallsiteSummary interface_missing_method_call = call;
    interface_missing_method_key.source_hash = 0x86;
    interface_missing_method_body.callsite_start = 1;
    interface_missing_method_body.callsite_count = 1;
    interface_missing_method_call.kind = XG_CALL_INTERFACE;
    interface_missing_method_call.static_target_func_id = XG_NO_ID;
    interface_missing_method_call.receiver_static_interface_id = 444;
    interface_missing_method_call.method_id = 555;
    interface_missing_method_call.method_name_id = 555;
    interface_missing_method_call.method_signature_key = 666;
    xg_global_evidence_init(&interface_missing_method_ev, interface_missing_method_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&interface_missing_method_ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&interface_missing_method_ev, &interface_decl));
    ASSERT_NOT_NULL(
        xg_global_evidence_add_body(&interface_missing_method_ev, &interface_missing_method_body));
    ASSERT_NOT_NULL(
        xg_global_evidence_add_callsite(&interface_missing_method_ev, &interface_missing_method_call));

    XaotBundle interface_missing_method_bundle;
    memset(&interface_missing_method_bundle, 0, sizeof(interface_missing_method_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(
        &interface_missing_method_bundle, &interface_missing_method_ev, XG_BUILD_NATIVE_RELEASE));
    interface_missing_method_bundle.modules = modules;
    interface_missing_method_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&interface_missing_method_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&interface_missing_method_bundle, XAOT_VERIFY_AOT_READY, err,
                                    sizeof(err)));
    ASSERT_NOT_NULL(
        strstr(err, "AOT global evidence interface callsite method does not re-derive"));
    xaot_bundle_free(&interface_missing_method_bundle);
    xg_global_evidence_free(&interface_missing_method_ev);

    XgGlobalEvidence native_stale_ev;
    XgBuildKey native_stale_key = key;
    XgBodySummary native_stale_body = body;
    XgCallsiteSummary native_stale_call = call;
    native_stale_key.source_hash = 0x7f;
    native_stale_body.callsite_start = 1;
    native_stale_body.callsite_count = 1;
    native_stale_call.kind = XG_CALL_NATIVE;
    native_stale_call.static_target_func_id = XG_NO_ID;
    native_stale_call.method_id = XG_NO_ID;
    native_stale_call.method_name_id = 444;
    xg_global_evidence_init(&native_stale_ev, native_stale_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&native_stale_ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&native_stale_ev, &native_stale_body));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&native_stale_ev, &native_stale_call));

    XaotBundle native_stale_bundle;
    memset(&native_stale_bundle, 0, sizeof(native_stale_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&native_stale_bundle, &native_stale_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    native_stale_bundle.modules = modules;
    native_stale_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&native_stale_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&native_stale_bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence native callsite identity is stale"));
    xaot_bundle_free(&native_stale_bundle);
    xg_global_evidence_free(&native_stale_ev);

    XgGlobalEvidence extern_stale_ev;
    XgBuildKey extern_stale_key = key;
    XgBodySummary extern_stale_body = body;
    XgCallsiteSummary extern_stale_call = call;
    extern_stale_key.source_hash = 0x80;
    extern_stale_body.callsite_start = 1;
    extern_stale_body.callsite_count = 1;
    extern_stale_call.kind = XG_CALL_EXTERN;
    extern_stale_call.static_target_func_id = XG_NO_ID;
    extern_stale_call.method_id = 444;
    extern_stale_call.method_name_id = 0;
    xg_global_evidence_init(&extern_stale_ev, extern_stale_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&extern_stale_ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&extern_stale_ev, &extern_stale_body));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&extern_stale_ev, &extern_stale_call));

    XaotBundle extern_stale_bundle;
    memset(&extern_stale_bundle, 0, sizeof(extern_stale_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&extern_stale_bundle, &extern_stale_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    extern_stale_bundle.modules = modules;
    extern_stale_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&extern_stale_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&extern_stale_bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence extern callsite identity is stale"));
    xaot_bundle_free(&extern_stale_bundle);
    xg_global_evidence_free(&extern_stale_ev);

    XgGlobalEvidence extern_missing_decl_ev;
    XgBuildKey extern_missing_decl_key = key;
    XgBodySummary extern_missing_decl_body = body;
    XgCallsiteSummary extern_missing_decl_call = call;
    extern_missing_decl_key.source_hash = 0x82;
    extern_missing_decl_body.callsite_start = 1;
    extern_missing_decl_body.callsite_count = 1;
    extern_missing_decl_call.kind = XG_CALL_EXTERN;
    extern_missing_decl_call.static_target_func_id = XG_NO_ID;
    extern_missing_decl_call.method_id = 444;
    extern_missing_decl_call.method_name_id = 444;
    xg_global_evidence_init(&extern_missing_decl_ev, extern_missing_decl_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&extern_missing_decl_ev, &decl));
    ASSERT_NOT_NULL(
        xg_global_evidence_add_body(&extern_missing_decl_ev, &extern_missing_decl_body));
    ASSERT_NOT_NULL(
        xg_global_evidence_add_callsite(&extern_missing_decl_ev, &extern_missing_decl_call));

    XaotBundle extern_missing_decl_bundle;
    memset(&extern_missing_decl_bundle, 0, sizeof(extern_missing_decl_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&extern_missing_decl_bundle,
                                                &extern_missing_decl_ev, XG_BUILD_NATIVE_RELEASE));
    extern_missing_decl_bundle.modules = modules;
    extern_missing_decl_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&extern_missing_decl_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(
        !xaot_verify_bundle(&extern_missing_decl_bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence extern callsite declaration is missing"));
    xaot_bundle_free(&extern_missing_decl_bundle);
    xg_global_evidence_free(&extern_missing_decl_ev);
}

TEST(global_evidence_verifier_rejects_stale_interface_extends_rows) {
    XgBuildKey key = {.source_hash = 0x91,
                      .compiler_semver_hash = 0x92,
                      .profile_hash = 0x93,
                      .imported_summary_hash = 0x94,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    XgDeclSummary func_decl = {.module_id = 1,
                               .decl_id = 1,
                               .kind = XG_DECL_FUNC,
                               .name_id = 11,
                               .signature_key = 22,
                               .source_span_id = 3};
    XgDeclSummary child_decl = {.module_id = 1,
                                .decl_id = 2,
                                .kind = XG_DECL_INTERFACE,
                                .name_id = 101,
                                .signature_key = 0,
                                .source_span_id = 4};
    XgDeclSummary parent_decl = {.module_id = 1,
                                 .decl_id = 3,
                                 .kind = XG_DECL_INTERFACE,
                                 .name_id = 102,
                                 .signature_key = 0,
                                 .source_span_id = 5};
    XgBodySummary body = {.func_id = 1,
                          .module_id = 1,
                          .owner_decl_id = 1,
                          .name_id = 11,
                          .signature_key = 22,
                          .source_span_id = 3,
                          .kind = XG_BODY_FUNCTION,
                          .body_hash = 0x9192};
    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    char err[256];

    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    modules[0] = &module;

    XgGlobalEvidence missing_parent_ev;
    XgInterfaceExtendsSummary missing_parent_edge = {.child_interface_id = 101,
                                                     .parent_interface_id = 999,
                                                     .name_id = 999,
                                                     .type_key = 777,
                                                     .source_span_id = 4};
    xg_global_evidence_init(&missing_parent_ev, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&missing_parent_ev, &func_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&missing_parent_ev, &child_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_extends(&missing_parent_ev,
                                                            &missing_parent_edge));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&missing_parent_ev, &body));

    XaotBundle missing_parent_bundle;
    memset(&missing_parent_bundle, 0, sizeof(missing_parent_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&missing_parent_bundle, &missing_parent_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    missing_parent_bundle.modules = modules;
    missing_parent_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&missing_parent_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&missing_parent_bundle, XAOT_VERIFY_AOT_READY, err,
                                    sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence interface extends parent is missing"));
    xaot_bundle_free(&missing_parent_bundle);
    xg_global_evidence_free(&missing_parent_ev);

    XgGlobalEvidence cycle_ev;
    XgBuildKey cycle_key = key;
    XgInterfaceExtendsSummary cycle_edges[] = {
        {.child_interface_id = 101,
         .parent_interface_id = 102,
         .name_id = 102,
         .type_key = 1,
         .source_span_id = 4},
        {.child_interface_id = 102,
         .parent_interface_id = 101,
         .name_id = 101,
         .type_key = 2,
         .source_span_id = 5},
    };
    cycle_key.source_hash = 0x95;
    xg_global_evidence_init(&cycle_ev, cycle_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&cycle_ev, &func_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&cycle_ev, &child_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&cycle_ev, &parent_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_extends(&cycle_ev, &cycle_edges[0]));
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_extends(&cycle_ev, &cycle_edges[1]));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&cycle_ev, &body));

    XaotBundle cycle_bundle;
    memset(&cycle_bundle, 0, sizeof(cycle_bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&cycle_bundle, &cycle_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    cycle_bundle.modules = modules;
    cycle_bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&cycle_bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&cycle_bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence interface extends graph has a cycle"));
    xaot_bundle_free(&cycle_bundle);
    xg_global_evidence_free(&cycle_ev);
}

TEST(global_evidence_verifier_rederives_method_body_signature) {
    XgGlobalEvidence ev;
    XgBuildKey key = {.source_hash = 0x51,
                      .compiler_semver_hash = 0x52,
                      .profile_hash = 0x53,
                      .imported_summary_hash = 0x54,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    XgDeclSummary decl = {
        .module_id = 1, .decl_id = 1, .kind = XG_DECL_CLASS, .name_id = 101, .source_span_id = 3};
    XgClassSummary cls = {.module_id = 1,
                          .decl_id = 1,
                          .class_id = 1,
                          .name_id = 101,
                          .flags = XG_CLASS_INFERRED_FINAL,
                          .method_start = 1,
                          .method_count = 1,
                          .decl_kind = XG_DECL_CLASS};
    XgMethodSummary method = {
        .method_id = 1, .owner_class_id = 1, .name_id = 202, .signature_key = 303};
    XgBodySummary body = {.func_id = 1,
                          .module_id = 1,
                          .owner_decl_id = 1,
                          .owner_class_id = 1,
                          .owner_method_id = 1,
                          .name_id = 202,
                          .signature_key = 303,
                          .source_span_id = 4,
                          .kind = XG_BODY_METHOD,
                          .body_hash = 0x5151};
    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    char err[256];

    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    modules[0] = &module;

    xg_global_evidence_init(&ev, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &cls));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&ev, &method));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&ev, &body));

    XaotBundle good;
    memset(&good, 0, sizeof(good));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&good, &ev, XG_BUILD_NATIVE_RELEASE));
    good.modules = modules;
    good.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&good, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_MSG(xaot_verify_bundle(&good, XAOT_VERIFY_AOT_READY, err, sizeof(err)), err);
    xaot_bundle_free(&good);

    XaotBundle stale_signature;
    ev.bodies[0].signature_key = 304;
    memset(&stale_signature, 0, sizeof(stale_signature));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&stale_signature, &ev, XG_BUILD_NATIVE_RELEASE));
    stale_signature.modules = modules;
    stale_signature.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&stale_signature, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&stale_signature, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence method body owner method does not re-derive"));
    xaot_bundle_free(&stale_signature);

    xg_global_evidence_free(&ev);
}

TEST(global_evidence_verifier_rejects_stale_body_identity_rows) {
    XgGlobalEvidence ev;
    XgBuildKey key = {.source_hash = 0x61,
                      .compiler_semver_hash = 0x62,
                      .profile_hash = 0x63,
                      .imported_summary_hash = 0x64,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    XgDeclSummary decls[2] = {
        {.module_id = 1,
         .decl_id = 1,
         .kind = XG_DECL_FUNC,
         .name_id = 101,
         .signature_key = 201,
         .source_span_id = 3},
        {.module_id = 1,
         .decl_id = 2,
         .kind = XG_DECL_FUNC,
         .name_id = 102,
         .signature_key = 202,
         .source_span_id = 4},
    };
    XgBodySummary bodies[2] = {
        {.func_id = 1,
         .module_id = 1,
         .owner_decl_id = 1,
         .name_id = 101,
         .signature_key = 201,
         .source_span_id = 3,
         .kind = XG_BODY_FUNCTION,
         .body_hash = 0x6101},
        {.func_id = 1,
         .module_id = 1,
         .owner_decl_id = 2,
         .name_id = 102,
         .signature_key = 202,
         .source_span_id = 4,
         .kind = XG_BODY_FUNCTION,
         .body_hash = 0x6102},
    };
    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    char err[256];

    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    modules[0] = &module;

    xg_global_evidence_init(&ev, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&ev, &decls[0]));
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&ev, &decls[1]));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&ev, &bodies[0]));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&ev, &bodies[1]));

    XaotBundle duplicate_body;
    memset(&duplicate_body, 0, sizeof(duplicate_body));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&duplicate_body, &ev, XG_BUILD_NATIVE_RELEASE));
    duplicate_body.modules = modules;
    duplicate_body.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&duplicate_body, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&duplicate_body, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence body function id is duplicated"));
    xaot_bundle_free(&duplicate_body);
    xg_global_evidence_free(&ev);

    XgGlobalEvidence module_ev;
    XgBuildKey module_key = key;
    XgBodySummary module_body = {.func_id = 1,
                                 .module_id = 1,
                                 .owner_decl_id = XG_NO_ID,
                                 .owner_class_id = XG_NO_ID,
                                 .owner_method_id = XG_NO_ID,
                                 .name_id = 999,
                                 .signature_key = 77,
                                 .kind = XG_BODY_MODULE_INIT,
                                 .body_hash = 0x6201};
    module_key.source_hash = 0x65;
    xg_global_evidence_init(&module_ev, module_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&module_ev, &module_body));

    XaotBundle stale_module_body;
    memset(&stale_module_body, 0, sizeof(stale_module_body));
    ASSERT_TRUE(
        xaot_bundle_set_global_evidence(&stale_module_body, &module_ev, XG_BUILD_NATIVE_RELEASE));
    stale_module_body.modules = modules;
    stale_module_body.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&stale_module_body, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&stale_module_body, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence module body has stale owner identity"));
    xaot_bundle_free(&stale_module_body);
    xg_global_evidence_free(&module_ev);

    XgGlobalEvidence missing_ev;
    XgBuildKey missing_key = key;
    XgDeclSummary missing_decl = {.module_id = 1,
                                  .decl_id = 1,
                                  .kind = XG_DECL_FUNC,
                                  .name_id = 301,
                                  .signature_key = 401,
                                  .source_span_id = 5};
    missing_key.source_hash = 0x66;
    xg_global_evidence_init(&missing_ev, missing_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&missing_ev, &missing_decl));

    XaotBundle missing_body;
    memset(&missing_body, 0, sizeof(missing_body));
    ASSERT_TRUE(
        xaot_bundle_set_global_evidence(&missing_body, &missing_ev, XG_BUILD_NATIVE_RELEASE));
    missing_body.modules = modules;
    missing_body.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&missing_body, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&missing_body, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence function decl has no body"));
    xaot_bundle_free(&missing_body);
    xg_global_evidence_free(&missing_ev);

    XgGlobalEvidence native_ev;
    XgBuildKey native_key = key;
    XgDeclSummary native_decl = missing_decl;
    native_key.source_hash = 0x67;
    native_decl.flags = XG_DECL_NATIVE;
    xg_global_evidence_init(&native_ev, native_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&native_ev, &native_decl));

    XaotBundle native_missing_body;
    memset(&native_missing_body, 0, sizeof(native_missing_body));
    ASSERT_TRUE(
        xaot_bundle_set_global_evidence(&native_missing_body, &native_ev, XG_BUILD_NATIVE_RELEASE));
    native_missing_body.modules = modules;
    native_missing_body.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&native_missing_body, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_MSG(xaot_verify_bundle(&native_missing_body, XAOT_VERIFY_AOT_READY, err, sizeof(err)),
               err);
    xaot_bundle_free(&native_missing_body);
    xg_global_evidence_free(&native_ev);

    XgGlobalEvidence duplicate_decl_ev;
    XgBuildKey duplicate_decl_key = key;
    XgDeclSummary duplicate_decl = {.module_id = 1,
                                    .decl_id = 1,
                                    .kind = XG_DECL_FUNC,
                                    .name_id = 501,
                                    .signature_key = 601,
                                    .source_span_id = 7};
    XgBodySummary duplicate_decl_bodies[2] = {
        {.func_id = 1,
         .module_id = 1,
         .owner_decl_id = 1,
         .name_id = 501,
         .signature_key = 601,
         .source_span_id = 7,
         .kind = XG_BODY_FUNCTION,
         .body_hash = 0x6301},
        {.func_id = 2,
         .module_id = 1,
         .owner_decl_id = 1,
         .name_id = 501,
         .signature_key = 601,
         .source_span_id = 7,
         .kind = XG_BODY_FUNCTION,
         .body_hash = 0x6302},
    };
    duplicate_decl_key.source_hash = 0x68;
    xg_global_evidence_init(&duplicate_decl_ev, duplicate_decl_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&duplicate_decl_ev, &duplicate_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&duplicate_decl_ev, &duplicate_decl_bodies[0]));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&duplicate_decl_ev, &duplicate_decl_bodies[1]));

    XaotBundle duplicate_decl_body;
    memset(&duplicate_decl_body, 0, sizeof(duplicate_decl_body));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&duplicate_decl_body, &duplicate_decl_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    duplicate_decl_body.modules = modules;
    duplicate_decl_body.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&duplicate_decl_body, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&duplicate_decl_body, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence function body is duplicated"));
    xaot_bundle_free(&duplicate_decl_body);
    xg_global_evidence_free(&duplicate_decl_ev);

    XgGlobalEvidence duplicate_method_ev;
    XgBuildKey duplicate_method_key = key;
    XgDeclSummary method_decl = {
        .module_id = 1, .decl_id = 1, .kind = XG_DECL_CLASS, .name_id = 701, .source_span_id = 9};
    XgClassSummary method_class = {.module_id = 1,
                                   .decl_id = 1,
                                   .class_id = 1,
                                   .name_id = 701,
                                   .flags = XG_CLASS_INFERRED_FINAL,
                                   .method_start = 1,
                                   .method_count = 1,
                                   .decl_kind = XG_DECL_CLASS};
    XgMethodSummary method_row = {
        .method_id = 1, .owner_class_id = 1, .name_id = 801, .signature_key = 901};
    XgBodySummary method_bodies[2] = {
        {.func_id = 1,
         .module_id = 1,
         .owner_decl_id = 1,
         .owner_class_id = 1,
         .owner_method_id = 1,
         .name_id = 801,
         .signature_key = 901,
         .source_span_id = 10,
         .kind = XG_BODY_METHOD,
         .body_hash = 0x6401},
        {.func_id = 2,
         .module_id = 1,
         .owner_decl_id = 1,
         .owner_class_id = 1,
         .owner_method_id = 1,
         .name_id = 801,
         .signature_key = 901,
         .source_span_id = 10,
         .kind = XG_BODY_METHOD,
         .body_hash = 0x6402},
    };
    duplicate_method_key.source_hash = 0x69;
    xg_global_evidence_init(&duplicate_method_ev, duplicate_method_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&duplicate_method_ev, &method_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&duplicate_method_ev, &method_class));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&duplicate_method_ev, &method_row));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&duplicate_method_ev, &method_bodies[0]));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&duplicate_method_ev, &method_bodies[1]));

    XaotBundle duplicate_method_body;
    memset(&duplicate_method_body, 0, sizeof(duplicate_method_body));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&duplicate_method_body, &duplicate_method_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    duplicate_method_body.modules = modules;
    duplicate_method_body.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&duplicate_method_body, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(
        !xaot_verify_bundle(&duplicate_method_body, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence method body is duplicated"));
    xaot_bundle_free(&duplicate_method_body);
    xg_global_evidence_free(&duplicate_method_ev);

    XgGlobalEvidence missing_method_ev;
    XgBuildKey missing_method_key = key;
    missing_method_key.source_hash = 0x6a;
    xg_global_evidence_init(&missing_method_ev, missing_method_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&missing_method_ev, &method_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&missing_method_ev, &method_class));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&missing_method_ev, &method_row));

    XaotBundle missing_method_body;
    memset(&missing_method_body, 0, sizeof(missing_method_body));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&missing_method_body, &missing_method_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    missing_method_body.modules = modules;
    missing_method_body.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&missing_method_body, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&missing_method_body, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence method has no body"));
    xaot_bundle_free(&missing_method_body);
    xg_global_evidence_free(&missing_method_ev);

    XgGlobalEvidence native_method_ev;
    XgBuildKey native_method_key = key;
    XgDeclSummary native_method_decl = method_decl;
    XgClassSummary native_method_class = method_class;
    XgMethodSummary native_method_row = method_row;
    native_method_key.source_hash = 0x6b;
    native_method_decl.flags = XG_DECL_NATIVE;
    native_method_class.flags = XG_CLASS_NATIVE | XG_CLASS_INFERRED_FINAL;
    native_method_row.flags = XG_METHOD_NATIVE;
    xg_global_evidence_init(&native_method_ev, native_method_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&native_method_ev, &native_method_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&native_method_ev, &native_method_class));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&native_method_ev, &native_method_row));

    XaotBundle native_missing_method_body;
    memset(&native_missing_method_body, 0, sizeof(native_missing_method_body));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&native_missing_method_body, &native_method_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    native_missing_method_body.modules = modules;
    native_missing_method_body.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&native_missing_method_body, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_MSG(
        xaot_verify_bundle(&native_missing_method_body, XAOT_VERIFY_AOT_READY, err, sizeof(err)),
        err);
    xaot_bundle_free(&native_missing_method_body);
    xg_global_evidence_free(&native_method_ev);

    XgGlobalEvidence native_body_ev;
    XgBuildKey native_body_key = key;
    native_body_key.source_hash = 0x6c;
    xg_global_evidence_init(&native_body_ev, native_body_key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&native_body_ev, &native_method_decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&native_body_ev, &native_method_class));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&native_body_ev, &native_method_row));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&native_body_ev, &method_bodies[0]));

    XaotBundle native_method_with_body;
    memset(&native_method_with_body, 0, sizeof(native_method_with_body));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&native_method_with_body, &native_body_ev,
                                                XG_BUILD_NATIVE_RELEASE));
    native_method_with_body.modules = modules;
    native_method_with_body.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&native_method_with_body, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(
        !xaot_verify_bundle(&native_method_with_body, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT global evidence native method has a body"));
    xaot_bundle_free(&native_method_with_body);
    xg_global_evidence_free(&native_body_ev);
}

TEST(global_evidence_verifier_rederives_link_dependency_plans) {
    XgGlobalEvidence ev;
    XgBuildKey key = {.source_hash = 0x31,
                      .compiler_semver_hash = 0x32,
                      .profile_hash = 0x33,
                      .imported_summary_hash = 0x34,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    XgLinkDependencySummary link_dep = {.link_id = 1,
                                        .module_id = 1,
                                        .decl_id = 1,
                                        .source_span_id = 3,
                                        .name_id = 99,
                                        .kind = XG_LINK_DEP_EXTERN_DYLIB,
                                        .flags = 0};
    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    XaotBundle bundle;
    char err[256];
    memcpy(link_dep.name, "m", 2);

    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    modules[0] = &module;

    xg_global_evidence_init(&ev, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_link_dependency(&ev, &link_dep));

    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    bundle.modules = modules;
    bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&bundle, &init_func, 0, 0));
    ASSERT_MSG(xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)), err);

    bundle.link_dependency_plans[0].name[0] = 'z';
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT link dependency plan mismatches evidence"));

    xaot_bundle_free(&bundle);
    xg_global_evidence_free(&ev);
}

TEST(global_evidence_producer_finalizes_class_graph_order_independently) {
    setup_parser_session();
    const char *source = "class Child extends Base {\n"
                         "    speak() -> string { return \"child\" }\n"
                         "}\n"
                         "class Base {\n"
                         "    speak() -> string { return \"base\" }\n"
                         "}\n"
                         "class Solo {\n"
                         "    ping() -> int { return 1 }\n"
                         "}\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(ev.nclasses, 3);
    ASSERT_EQ_UINT(ev.nmethods, 3);

    const XgClassSummary *child = &ev.classes[0];
    const XgClassSummary *base = &ev.classes[1];
    const XgClassSummary *solo = &ev.classes[2];
    ASSERT_EQ_UINT(child->parent_class_id, base->class_id);
    ASSERT_TRUE((base->flags & XG_CLASS_HAS_SUBCLASS) != 0);
    ASSERT_TRUE((base->flags & XG_CLASS_INFERRED_FINAL) == 0);
    ASSERT_TRUE((child->flags & XG_CLASS_INFERRED_FINAL) != 0);
    ASSERT_TRUE((solo->flags & XG_CLASS_INFERRED_FINAL) != 0);
    ASSERT_EQ_UINT(ev.methods[0].override_of, ev.methods[1].method_id);
    ASSERT_TRUE((ev.methods[1].flags & XG_METHOD_OVERRIDDEN) != 0);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST(global_evidence_producer_resolves_direct_function_callsite_targets) {
    setup_parser_session();
    const char *source = "fn callee(x: int) -> int { return x + 1 }\n"
                         "fn caller() -> int { return callee(41) }\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(ev.ndecls, 2);
    ASSERT_EQ_UINT(ev.nbodies, 2);
    ASSERT_EQ_UINT(ev.ncallsites, 1);
    const XgCallsiteSummary *call = &ev.callsites[0];
    ASSERT_EQ_UINT(call->kind, XG_CALL_DIRECT_FUNC);
    ASSERT_TRUE(call->static_target_func_id != XG_NO_ID);
    ASSERT_NE(call->static_target_func_id, call->owner_func_id);

    const XgBodySummary *owner_body = evidence_find_body_by_func(&ev, call->owner_func_id);
    const XgBodySummary *target_body = evidence_find_body_by_func(&ev, call->static_target_func_id);
    ASSERT_NOT_NULL(owner_body);
    ASSERT_NOT_NULL(target_body);
    ASSERT_EQ_UINT(owner_body->kind, XG_BODY_FUNCTION);
    ASSERT_EQ_UINT(target_body->kind, XG_BODY_FUNCTION);
    ASSERT_EQ_UINT(owner_body->callsite_count, 1);
    ASSERT_EQ_UINT(target_body->callsite_count, 0);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST(global_evidence_producer_keeps_unknown_function_values_as_closure_calls) {
    setup_parser_session();
    const char *source = "fn caller() -> int { return unknown(41) }\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(ev.ndecls, 1);
    ASSERT_EQ_UINT(ev.nbodies, 1);
    ASSERT_EQ_UINT(ev.ncallsites, 1);
    ASSERT_EQ_UINT(ev.callsites[0].kind, XG_CALL_CLOSURE);
    ASSERT_EQ_UINT(ev.callsites[0].static_target_func_id, XG_NO_ID);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST(global_evidence_producer_classifies_extern_function_calls_as_boundary_calls) {
    setup_parser_session();
    const char *source = "@extern(\"C\") @dylib(\"m\") fn cos(x: float64) -> float64\n"
                         "fn useCos() -> float64 { return cos(0.0) }\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(ev.ndecls, 2);
    ASSERT_EQ_UINT(ev.nbodies, 1);
    ASSERT_EQ_UINT(ev.ncallsites, 1);
    ASSERT_EQ_UINT(ev.callsites[0].kind, XG_CALL_EXTERN);
    ASSERT_EQ_UINT(ev.callsites[0].static_target_func_id, XG_NO_ID);
    ASSERT_TRUE(ev.callsites[0].method_id != XG_NO_ID);
    ASSERT_TRUE(ev.callsites[0].method_name_id != 0);

    XiFunc init_func;
    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    XiModule module;
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    XiModule *modules[1] = {&module};
    XaotBundle bundle;
    char err[256];
    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    bundle.modules = modules;
    bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&bundle, &init_func, 0, 0));
    memset(err, 0, sizeof(err));
    ASSERT_MSG(xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)), err);
    xaot_bundle_free(&bundle);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST(global_evidence_producer_resolves_method_callsite_receivers) {
    setup_parser_session();
    const char *source = "class Animal {\n"
                         "    speak(sound: string) -> string { return \"animal:\" + sound }\n"
                         "}\n"
                         "class Dog extends Animal {\n"
                         "    speak(sound: string) -> string { return \"dog:\" + sound }\n"
                         "    relay() -> string { return this.speak(\"relay\") }\n"
                         "}\n"
                         "fn callDog() -> string {\n"
                         "    var d = Dog()\n"
                         "    return d.speak(\"local\")\n"
                         "}\n"
                         "fn callBase(b: Animal) -> string {\n"
                         "    return b.speak(\"base\")\n"
                         "}\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(ev.nclasses, 2);
    ASSERT_TRUE(ev.ncallsites >= 4);
    assert_body_callsite_ordinals(&ev);
    for (uint32_t i = 0; i < ev.nbodies; i++) {
        for (uint32_t j = i + 1; j < ev.nbodies; j++)
            ASSERT_NE(ev.bodies[i].func_id, ev.bodies[j].func_id);
    }

    XgClassId animal_id = ev.classes[0].class_id;
    XgClassId dog_id = ev.classes[1].class_id;
    XgMethodId animal_speak_id = ev.methods[0].method_id;
    XgMethodId dog_speak_id = ev.methods[1].method_id;
    uint32_t dog_direct_calls = 0;
    uint32_t animal_static_calls = 0;
    uint32_t unresolved_method_calls = 0;
    uint32_t method_calls_with_source = 0;
    uint32_t direct_dispatch_plans = 0;
    uint32_t vtable_dispatch_plans = 0;
    uint32_t object_capability_bodies = 0;
    uint32_t method_bodies = 0;
    uint32_t function_bodies = 0;

    for (uint32_t i = 0; i < ev.ncallsites; i++) {
        const XgCallsiteSummary *call = &ev.callsites[i];
        if (call->kind != XG_CALL_METHOD)
            continue;
        if (call->receiver_static_class_id == dog_id && call->method_id == dog_speak_id)
            dog_direct_calls++;
        if (call->receiver_static_class_id == animal_id && call->method_id == animal_speak_id)
            animal_static_calls++;
        if (call->receiver_static_class_id == XG_NO_ID)
            unresolved_method_calls++;
        if (call->source_span_id != 0)
            method_calls_with_source++;
    }

    ASSERT_EQ_UINT(dog_direct_calls, 2);
    ASSERT_EQ_UINT(animal_static_calls, 1);
    ASSERT_EQ_UINT(unresolved_method_calls, 0);
    ASSERT_EQ_UINT(method_calls_with_source, 3);
    for (uint32_t i = 0; i < ev.nbodies; i++) {
        const XgBodySummary *body = &ev.bodies[i];
        ASSERT_EQ_UINT(body->module_id, 1);
        ASSERT_TRUE(body->name_id != 0);
        if (body->kind == XG_BODY_METHOD) {
            method_bodies++;
            ASSERT_TRUE(body->owner_decl_id != XG_NO_ID);
            ASSERT_TRUE(body->owner_class_id != XG_NO_ID);
            ASSERT_TRUE(body->owner_method_id != XG_NO_ID);
            ASSERT_TRUE(body->signature_key != 0);
            ASSERT_TRUE(body->source_span_id != 0);
        } else if (body->kind == XG_BODY_FUNCTION) {
            function_bodies++;
            ASSERT_TRUE(body->owner_decl_id != XG_NO_ID);
            ASSERT_EQ_UINT(body->owner_class_id, XG_NO_ID);
            ASSERT_EQ_UINT(body->owner_method_id, XG_NO_ID);
            ASSERT_TRUE(body->signature_key != 0);
            ASSERT_TRUE(body->source_span_id != 0);
        }
        if ((body->capability_bits & XG_CAP_OBJECTS) != 0)
            object_capability_bodies++;
    }
    ASSERT_EQ_UINT(method_bodies, 3);
    ASSERT_EQ_UINT(function_bodies, 2);
    ASSERT_EQ_UINT(object_capability_bodies, 1);

    XaotBundle bundle;
    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(bundle.nmethod_dispatch_plans, 3);
    for (uint32_t i = 0; i < bundle.nmethod_dispatch_plans; i++) {
        const XaotMethodDispatchPlan *plan = &bundle.method_dispatch_plans[i];
        ASSERT_TRUE(plan->source_span_id != 0);
        if (plan->kind == XAOT_DISPATCH_DIRECT)
            direct_dispatch_plans++;
        if (plan->kind == XAOT_DISPATCH_VTABLE)
            vtable_dispatch_plans++;
    }
    ASSERT_EQ_UINT(direct_dispatch_plans, 2);
    ASSERT_EQ_UINT(vtable_dispatch_plans, 1);
    const XaotCapabilityPlan *objects = xaot_bundle_find_capability_plan(&bundle, XG_CAP_OBJECTS);
    ASSERT_NOT_NULL(objects);
    ASSERT_EQ_UINT(objects->body_count, 1);
    xaot_bundle_free(&bundle);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST(global_evidence_producer_keeps_module_member_calls_out_of_method_dispatch) {
    setup_parser_session();
    const char *source = "fn clampScore(x: int) -> int {\n"
                         "    return math.min(x, 10)\n"
                         "}\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(ev.nclasses, 0);
    ASSERT_EQ_UINT(ev.ncallsites, 1);
    ASSERT_EQ_UINT(ev.callsites[0].kind, XG_CALL_NATIVE);
    ASSERT_EQ_UINT(ev.callsites[0].receiver_static_class_id, XG_NO_ID);
    ASSERT_TRUE(ev.callsites[0].method_name_id != 0);

    XaotBundle bundle;
    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(bundle.nmethod_dispatch_plans, 0);
    xaot_bundle_free(&bundle);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST(global_evidence_producer_marks_native_methods_bodyless) {
    setup_parser_session();
    const char *source = "@native class Handle {\n"
                         "    id() -> int\n"
                         "}\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(ev.nclasses, 1);
    ASSERT_EQ_UINT(ev.nmethods, 1);
    ASSERT_TRUE((ev.classes[0].flags & XG_CLASS_NATIVE) != 0);
    ASSERT_TRUE((ev.methods[0].flags & XG_METHOD_NATIVE) != 0);
    for (uint32_t i = 0; i < ev.nbodies; i++)
        ASSERT_TRUE(ev.bodies[i].owner_method_id != ev.methods[0].method_id);

    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    modules[0] = &module;

    XaotBundle bundle;
    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    bundle.modules = modules;
    bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&bundle, &init_func, 0, 0));
    char err[256];
    memset(err, 0, sizeof(err));
    ASSERT_MSG(xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)), err);

    xaot_bundle_free(&bundle);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST(global_evidence_producer_resolves_interface_callsite_receivers) {
    setup_parser_session();
    const char *source = "interface Shape {\n"
                         "    area() -> float\n"
                         "}\n"
                         "class Circle implements Shape {\n"
                         "    area() -> float { return 1.0 }\n"
                         "}\n"
                         "class Square implements Shape {\n"
                         "    area() -> float { return 2.0 }\n"
                         "}\n"
                         "fn describe(shape: Shape) -> float {\n"
                         "    return shape.area()\n"
                         "}\n"
                         "fn describeLocal(circle: Circle) -> float {\n"
                         "    var shape: Shape = circle\n"
                         "    return shape.area()\n"
                         "}\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(ev.nclasses, 2);
    ASSERT_EQ_UINT(ev.ninterface_impls, 2);
    ASSERT_EQ_UINT(ev.ninterface_methods, 1);
    assert_body_callsite_ordinals(&ev);

    XgInterfaceId shape_id = ev.interface_impls[0].interface_id;
    ASSERT_EQ_UINT(ev.interface_methods[0].owner_interface_id, shape_id);
    uint32_t interface_calls = 0;
    uint32_t interface_calls_with_signature = 0;
    uint32_t interface_calls_with_slot = 0;
    uint32_t interface_calls_with_source = 0;
    uint32_t class_method_calls = 0;
    for (uint32_t i = 0; i < ev.ncallsites; i++) {
        const XgCallsiteSummary *call = &ev.callsites[i];
        if (call->kind == XG_CALL_INTERFACE) {
            interface_calls++;
            ASSERT_EQ_UINT(call->receiver_static_interface_id, shape_id);
            ASSERT_TRUE(call->method_name_id != 0);
            if (call->method_signature_key != 0)
                interface_calls_with_signature++;
            if (call->method_id == ev.interface_methods[0].interface_method_id)
                interface_calls_with_slot++;
            if (call->source_span_id != 0)
                interface_calls_with_source++;
        }
        if (call->kind == XG_CALL_METHOD)
            class_method_calls++;
    }
    ASSERT_EQ_UINT(interface_calls, 2);
    ASSERT_EQ_UINT(interface_calls_with_signature, 2);
    ASSERT_EQ_UINT(interface_calls_with_slot, 2);
    ASSERT_EQ_UINT(interface_calls_with_source, 2);
    ASSERT_EQ_UINT(class_method_calls, 0);

    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    modules[0] = &module;

    XaotBundle bundle;
    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    bundle.modules = modules;
    bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&bundle, &init_func, 0, 0));
    ASSERT_EQ_UINT(bundle.nmethod_dispatch_plans, 2);
    for (uint32_t i = 0; i < bundle.nmethod_dispatch_plans; i++) {
        const XaotMethodDispatchPlan *plan = &bundle.method_dispatch_plans[i];
        ASSERT_EQ_UINT(plan->kind, XAOT_DISPATCH_TYPE_SWITCH);
        ASSERT_TRUE(plan->source_span_id != 0);
        ASSERT_TRUE(plan->method_name_id != 0);
        ASSERT_TRUE(plan->method_signature_key != 0);
        ASSERT_EQ_UINT(plan->target_count, 2);
        ASSERT_TRUE((plan->evidence & XAOT_DISPATCH_EV_INTERFACE_OBJECT) != 0);
        ASSERT_TRUE((plan->evidence & XAOT_DISPATCH_EV_SMALL_IMPLEMENTOR_SET) != 0);
    }
    ASSERT_EQ_UINT(bundle.ndispatch_target_cases, 4);

    char err[256];
    memset(err, 0, sizeof(err));
    ASSERT_MSG(xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)), err);

    xaot_bundle_free(&bundle);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST(global_evidence_producer_resolves_interface_extends_callsite_methods) {
    setup_parser_session();
    const char *source = "interface Shape {\n"
                         "    area() -> float\n"
                         "}\n"
                         "interface Drawable extends Shape {\n"
                         "}\n"
                         "class Circle implements Drawable {\n"
                         "    area() -> float { return 1.0 }\n"
                         "}\n"
                         "fn describe(shape: Drawable) -> float {\n"
                         "    return shape.area()\n"
                         "}\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(ev.ninterface_extends, 1);
    ASSERT_EQ_UINT(ev.ninterface_methods, 1);
    ASSERT_EQ_UINT(ev.ninterface_impls, 1);
    XgInterfaceId shape_id = ev.interface_extends[0].parent_interface_id;
    XgInterfaceId drawable_id = ev.interface_extends[0].child_interface_id;
    ASSERT_EQ_UINT(ev.interface_methods[0].owner_interface_id, shape_id);

    uint32_t inherited_interface_calls = 0;
    for (uint32_t i = 0; i < ev.ncallsites; i++) {
        const XgCallsiteSummary *call = &ev.callsites[i];
        if (call->kind != XG_CALL_INTERFACE)
            continue;
        inherited_interface_calls++;
        ASSERT_EQ_UINT(call->receiver_static_interface_id, drawable_id);
        ASSERT_EQ_UINT(call->method_id, ev.interface_methods[0].interface_method_id);
        ASSERT_EQ_UINT(call->method_name_id, ev.interface_methods[0].name_id);
        ASSERT_EQ_UINT(call->method_signature_key, ev.interface_methods[0].signature_key);
    }
    ASSERT_EQ_UINT(inherited_interface_calls, 1);

    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    modules[0] = &module;

    XaotBundle bundle;
    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    bundle.modules = modules;
    bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&bundle, &init_func, 0, 0));
    ASSERT_EQ_UINT(bundle.nmethod_dispatch_plans, 1);
    ASSERT_EQ_UINT(bundle.method_dispatch_plans[0].kind, XAOT_DISPATCH_DIRECT);
    ASSERT_EQ_UINT(bundle.method_dispatch_plans[0].receiver_static_interface_id, drawable_id);
    ASSERT_EQ_UINT(bundle.method_dispatch_plans[0].method_id,
                   ev.interface_methods[0].interface_method_id);
    ASSERT_EQ_UINT(bundle.ndispatch_target_cases, 1);

    char err[256];
    memset(err, 0, sizeof(err));
    ASSERT_MSG(xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)), err);

    xaot_bundle_free(&bundle);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST(global_evidence_verifier_rejects_ambiguous_interface_extends_methods) {
    setup_parser_session();
    const char *source = "interface Left {\n"
                         "    draw() -> int\n"
                         "}\n"
                         "interface Right {\n"
                         "    draw() -> int\n"
                         "}\n"
                         "interface Drawable extends Left, Right {\n"
                         "}\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(ev.ninterface_extends, 2);
    ASSERT_EQ_UINT(ev.ninterface_methods, 2);

    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    modules[0] = &module;

    XaotBundle bundle;
    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    bundle.modules = modules;
    bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&bundle, &init_func, 0, 0));

    char err[256];
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(
        strstr(err, "AOT global evidence interface method inherited slot is ambiguous"));
    xaot_bundle_free(&bundle);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST(global_evidence_producer_resolves_transitive_interface_implementors) {
    setup_parser_session();
    const char *source = "interface Shape {\n"
                         "    area() -> float\n"
                         "}\n"
                         "interface Drawable extends Shape {\n"
                         "}\n"
                         "class Circle implements Drawable {\n"
                         "    area() -> float { return 1.0 }\n"
                         "}\n"
                         "class Square implements Drawable, Shape {\n"
                         "    area() -> float { return 2.0 }\n"
                         "}\n"
                         "fn describe(shape: Shape) -> float {\n"
                         "    return shape.area()\n"
                         "}\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(ev.ninterface_extends, 1);
    ASSERT_EQ_UINT(ev.ninterface_methods, 1);
    ASSERT_EQ_UINT(ev.ninterface_impls, 3);
    XgInterfaceId shape_id = ev.interface_extends[0].parent_interface_id;
    ASSERT_EQ_UINT(ev.interface_methods[0].owner_interface_id, shape_id);

    uint32_t parent_interface_calls = 0;
    XgCallsiteId parent_callsite_id = XG_NO_ID;
    for (uint32_t i = 0; i < ev.ncallsites; i++) {
        const XgCallsiteSummary *call = &ev.callsites[i];
        if (call->kind != XG_CALL_INTERFACE)
            continue;
        parent_interface_calls++;
        parent_callsite_id = call->callsite_id;
        ASSERT_EQ_UINT(call->receiver_static_interface_id, shape_id);
        ASSERT_EQ_UINT(call->method_id, ev.interface_methods[0].interface_method_id);
    }
    ASSERT_EQ_UINT(parent_interface_calls, 1);

    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    memset(&init_func, 0, sizeof(init_func));
    init_func.name = "init";
    memset(&module, 0, sizeof(module));
    module.path = "test.xr";
    module.name = "test";
    module.init = &init_func;
    modules[0] = &module;

    XaotBundle bundle;
    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    bundle.modules = modules;
    bundle.nmodules = 1;
    ASSERT_NOT_NULL(xaot_bundle_add_func_plan(&bundle, &init_func, 0, 0));
    ASSERT_EQ_UINT(bundle.nmethod_dispatch_plans, 1);
    ASSERT_EQ_UINT(bundle.method_dispatch_plans[0].kind, XAOT_DISPATCH_TYPE_SWITCH);
    ASSERT_EQ_UINT(bundle.method_dispatch_plans[0].receiver_static_interface_id, shape_id);
    ASSERT_EQ_UINT(bundle.method_dispatch_plans[0].target_count, 2);
    ASSERT_EQ_UINT(bundle.ndispatch_target_cases, 2);
    ASSERT_TRUE(bundle.dispatch_target_cases[0].receiver_class_id !=
                bundle.dispatch_target_cases[1].receiver_class_id);
    ASSERT_EQ_UINT(bundle.ninterface_use_plans, 5);
    ASSERT_NOT_NULL(xaot_bundle_find_interface_use_plan(
        &bundle, shape_id, bundle.dispatch_target_cases[0].receiver_class_id, parent_callsite_id));
    ASSERT_NOT_NULL(xaot_bundle_find_interface_use_plan(
        &bundle, shape_id, bundle.dispatch_target_cases[1].receiver_class_id, parent_callsite_id));

    char err[256];
    memset(err, 0, sizeof(err));
    ASSERT_MSG(xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)), err);
    for (uint32_t i = 0; i < bundle.ninterface_use_plans; i++) {
        XaotInterfaceUsePlan *use_plan = &bundle.interface_use_plans[i];
        XgClassId saved_implementor;
        if (use_plan->use_site_id != parent_callsite_id)
            continue;
        saved_implementor = use_plan->implementor_class_id;
        use_plan->implementor_class_id = 999999;
        memset(err, 0, sizeof(err));
        ASSERT_TRUE(!xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
        ASSERT_NOT_NULL(
            strstr(err, "AOT interface-use plan has no effective implements evidence"));
        use_plan->implementor_class_id = saved_implementor;
        break;
    }
    xaot_bundle_free(&bundle);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST(global_evidence_producer_marks_metadata_reachability) {
    setup_parser_session();
    const char *source = "@derive(Inspect)\n"
                         "class User {\n"
                         "    value: int\n"
                         "}\n"
                         "fn userTypeName(x: int) -> string {\n"
                         "    return typename(x)\n"
                         "}\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(evidence_body_count_with_metadata(&ev, XG_METADATA_TYPENAME), 1);
    ASSERT_EQ_UINT(evidence_decl_count_with_flags(&ev, XG_DECL_DERIVE), 1);

    XaotBundle bundle;
    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(bundle.nmetadata_plans, 2);
    const XaotMetadataReachabilityPlan *typename_plan =
        xaot_bundle_find_metadata_plan(&bundle, XG_METADATA_TYPENAME);
    const XaotMetadataReachabilityPlan *derive_plan =
        xaot_bundle_find_metadata_plan(&bundle, XG_METADATA_DERIVE);
    ASSERT_NOT_NULL(typename_plan);
    ASSERT_NOT_NULL(derive_plan);
    ASSERT_EQ_UINT(typename_plan->body_count, 1);
    ASSERT_EQ_UINT(typename_plan->decl_count, 0);
    ASSERT_EQ_UINT(derive_plan->body_count, 0);
    ASSERT_EQ_UINT(derive_plan->decl_count, 1);
    xaot_bundle_free(&bundle);

    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_FREESTANDING));
    typename_plan = xaot_bundle_find_metadata_plan(&bundle, XG_METADATA_TYPENAME);
    derive_plan = xaot_bundle_find_metadata_plan(&bundle, XG_METADATA_DERIVE);
    ASSERT_NOT_NULL(typename_plan);
    ASSERT_NOT_NULL(derive_plan);
    ASSERT_EQ_UINT(typename_plan->profile_action, XAOT_CAPABILITY_ACTION_REJECT);
    ASSERT_EQ_UINT(derive_plan->profile_action, XAOT_CAPABILITY_ACTION_REJECT);
    xaot_bundle_free(&bundle);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST(global_evidence_producer_marks_static_data_reachability) {
    setup_parser_session();
    const char *source = "fn useTable() -> int {\n"
                         "    const table = comptime [1, 2, 3]\n"
                         "    const value = comptime 7\n"
                         "    return value\n"
                         "}\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(evidence_body_count_with_static_data(&ev, XG_STATIC_DATA_COMPTIME_VALUE), 1);

    XaotBundle bundle;
    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    const XaotStaticDataPlan *plan =
        xaot_bundle_find_static_data_plan(&bundle, XG_STATIC_DATA_COMPTIME_VALUE);
    ASSERT_NOT_NULL(plan);
    ASSERT_EQ_UINT(plan->body_count, 1);
    ASSERT_EQ_UINT(plan->action, XAOT_STATIC_DATA_ACTION_MATERIALIZE);
    xaot_bundle_free(&bundle);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST(global_evidence_producer_marks_runtime_capabilities) {
    setup_parser_session();
    const char *source =
        "import { Semaphore, CountdownLatch, EventCount, WorkQueue, ResultGroup } from sync\n"
        "fn inc(x: int) -> int { return x + 1 }\n"
        "fn caps() {\n"
        "    var ch = Channel<int>(1)\n"
        "    var task = go inc(1)\n"
        "    var got = await task\n"
        "    scope { cancelled() }\n"
        "    var atom = Atomic(0)\n"
        "    var queue: WorkQueue<int> = WorkQueue<int>(2, 1)\n"
        "    var rg: ResultGroup = ResultGroup(1)\n"
        "    var latch = CountdownLatch(1)\n"
        "    var sem = Semaphore(1)\n"
        "    var event = EventCount(0)\n"
        "    parallel for i in 0..4 workers 2 {\n"
        "        atom.add(i)\n"
        "    }\n"
        "}\n"
        "fn gen(n: int) -> Iterator<int> {\n"
        "    for (var i = 0; i < n; i++) {\n"
        "        yield i\n"
        "    }\n"
        "}\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));

    ASSERT_TRUE(evidence_body_count_with_capability(&ev, XG_CAP_COROUTINE) >= 2);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_CHANNEL), 1);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_SCOPE), 1);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_TASK), 1);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_NETPOLL), 1);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_ATOMIC), 1);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_WORK_QUEUE), 1);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_RESULT_GROUP), 1);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_COUNTDOWN_LATCH), 1);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_SEMAPHORE), 1);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_EVENT_COUNT), 1);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_GENERATOR), 1);

    XaotBundle bundle;
    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    ASSERT_NOT_NULL(xaot_bundle_find_capability_plan(&bundle, XG_CAP_SCOPE));
    ASSERT_NOT_NULL(xaot_bundle_find_capability_plan(&bundle, XG_CAP_TASK));
    ASSERT_NOT_NULL(xaot_bundle_find_capability_plan(&bundle, XG_CAP_WORK_QUEUE));
    ASSERT_NOT_NULL(xaot_bundle_find_capability_plan(&bundle, XG_CAP_GENERATOR));
    xaot_bundle_free(&bundle);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST(global_evidence_producer_marks_sys_thread_spawn_capability) {
    setup_parser_session();
    const char *source = "import sys\n"
                         "fn launch() {\n"
                         "    var t = sys.Thread.spawn(fn() -> int {\n"
                         "        return 42\n"
                         "    })\n"
                         "    t.detach()\n"
                         "}\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_SYS_THREAD), 1);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_COROUTINE), 1);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_TASK), 1);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_OBJECTS), 1);

    XaotBundle bundle;
    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    ASSERT_NOT_NULL(xaot_bundle_find_capability_plan(&bundle, XG_CAP_SYS_THREAD));
    ASSERT_NOT_NULL(xaot_bundle_find_capability_plan(&bundle, XG_CAP_COROUTINE));
    ASSERT_NOT_NULL(xaot_bundle_find_capability_plan(&bundle, XG_CAP_TASK));
    ASSERT_NOT_NULL(xaot_bundle_find_capability_plan(&bundle, XG_CAP_OBJECTS));
    xaot_bundle_free(&bundle);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST(global_evidence_producer_marks_extern_dylib_link_dependency) {
    setup_parser_session();
    const char *source = "@extern(\"C\") @dylib(\"m\") fn cos(x: float64) -> float64\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(ev.nlink_deps, 1);
    ASSERT_TRUE((ev.decls[0].flags & XG_DECL_EXTERN) != 0);
    ASSERT_EQ_UINT(ev.link_deps[0].kind, XG_LINK_DEP_EXTERN_DYLIB);
    ASSERT_STR_EQ(ev.link_deps[0].name, "m");

    XaotBundle bundle;
    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(bundle.nlink_dependency_plans, 1);
    ASSERT_NOT_NULL(xaot_bundle_find_link_dependency_plan(&bundle, ev.link_deps[0].link_id));
    xaot_bundle_free(&bundle);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST(global_evidence_producer_ignores_user_member_names_for_runtime_capabilities) {
    setup_parser_session();
    const char *source = "class Fake {\n"
                         "    Semaphore() -> int { return 1 }\n"
                         "}\n"
                         "fn useFake() -> int {\n"
                         "    var fake = Fake()\n"
                         "    return fake.Semaphore()\n"
                         "}\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_SEMAPHORE), 0);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_COROUTINE), 0);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_OBJECTS), 1);

    XaotBundle bundle;
    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    ASSERT_NULL(xaot_bundle_find_capability_plan(&bundle, XG_CAP_SEMAPHORE));
    xaot_bundle_free(&bundle);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST(global_evidence_producer_marks_module_init_body) {
    setup_parser_session();
    const char *source = "fn inc(x: int) -> int { return x + 1 }\n"
                         "shared ch = Channel<int>(1)\n"
                         "var task = go inc(41)\n"
                         "print(await task)\n";
    AstNode *ast = xr_parse(g_session, source);
    ASSERT_NOT_NULL(ast);

    XrModuleSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.ast = ast;
    int topo_order[1] = {0};
    XrModuleGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.specs = &spec;
    graph.spec_count = 1;
    graph.topo_order = topo_order;
    graph.topo_count = 1;
    graph.entry_index = 0;

    XgGlobalEvidence ev;
    ASSERT_TRUE(xg_global_evidence_build_from_module_graph(&ev, &graph, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(ev.nbodies, 2);
    uint32_t module_init_bodies = 0;
    uint32_t function_bodies = 0;
    for (uint32_t i = 0; i < ev.nbodies; i++) {
        const XgBodySummary *body = &ev.bodies[i];
        ASSERT_EQ_UINT(body->module_id, 1);
        ASSERT_TRUE(body->name_id != 0);
        if (body->kind == XG_BODY_MODULE_INIT) {
            module_init_bodies++;
            ASSERT_EQ_UINT(body->owner_decl_id, XG_NO_ID);
            ASSERT_EQ_UINT(body->owner_class_id, XG_NO_ID);
            ASSERT_EQ_UINT(body->owner_method_id, XG_NO_ID);
            ASSERT_EQ_UINT(body->signature_key, 0);
            ASSERT_EQ_UINT(body->source_span_id, 0);
        } else if (body->kind == XG_BODY_FUNCTION) {
            function_bodies++;
            ASSERT_TRUE(body->owner_decl_id != XG_NO_ID);
            ASSERT_TRUE(body->signature_key != 0);
            ASSERT_TRUE(body->source_span_id != 0);
        }
    }
    ASSERT_EQ_UINT(module_init_bodies, 1);
    ASSERT_EQ_UINT(function_bodies, 1);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_CHANNEL), 1);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_TASK), 1);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_NETPOLL), 1);
    ASSERT_EQ_UINT(evidence_body_count_with_capability(&ev, XG_CAP_OBJECTS), 1);

    XaotBundle bundle;
    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    ASSERT_NOT_NULL(xaot_bundle_find_capability_plan(&bundle, XG_CAP_CHANNEL));
    ASSERT_NOT_NULL(xaot_bundle_find_capability_plan(&bundle, XG_CAP_TASK));
    xaot_bundle_free(&bundle);

    xg_global_evidence_free(&ev);
    teardown_parser_session();
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("xglobal_summary");
RUN_TEST(global_evidence_adds_rows_and_grows);
RUN_TEST(global_evidence_hash_is_content_stable);
RUN_TEST(global_evidence_dump_lists_core_rows);
RUN_TEST(global_evidence_lowers_to_aot_class_plans);
RUN_TEST(global_evidence_verifier_rederives_dispatch_plans);
RUN_TEST(global_evidence_attaches_callsite_ids_to_xi_calls);
RUN_TEST(global_evidence_leaves_ambiguous_xi_callsite_unbound);
RUN_TEST(global_evidence_lowers_interface_call_to_type_switch);
RUN_TEST(global_evidence_verifier_rederives_class_graph_flags);
RUN_TEST(global_evidence_verifier_rederives_method_override_graph);
RUN_TEST(global_evidence_verifier_rederives_interface_implementor_set);
RUN_TEST(global_evidence_verifier_rederives_profile_actions);
RUN_TEST(global_evidence_verifier_rederives_body_callsite_ranges);
RUN_TEST(global_evidence_verifier_rejects_stale_callsite_identity_rows);
RUN_TEST(global_evidence_verifier_rejects_stale_interface_extends_rows);
RUN_TEST(global_evidence_verifier_rederives_method_body_signature);
RUN_TEST(global_evidence_verifier_rejects_stale_body_identity_rows);
RUN_TEST(global_evidence_verifier_rederives_link_dependency_plans);
RUN_TEST(global_evidence_producer_finalizes_class_graph_order_independently);
RUN_TEST(global_evidence_producer_resolves_direct_function_callsite_targets);
RUN_TEST(global_evidence_producer_keeps_unknown_function_values_as_closure_calls);
RUN_TEST(global_evidence_producer_classifies_extern_function_calls_as_boundary_calls);
RUN_TEST(global_evidence_producer_resolves_method_callsite_receivers);
RUN_TEST(global_evidence_producer_keeps_module_member_calls_out_of_method_dispatch);
RUN_TEST(global_evidence_producer_marks_native_methods_bodyless);
RUN_TEST(global_evidence_producer_resolves_interface_callsite_receivers);
RUN_TEST(global_evidence_producer_resolves_interface_extends_callsite_methods);
RUN_TEST(global_evidence_verifier_rejects_ambiguous_interface_extends_methods);
RUN_TEST(global_evidence_producer_resolves_transitive_interface_implementors);
RUN_TEST(global_evidence_producer_marks_metadata_reachability);
RUN_TEST(global_evidence_producer_marks_static_data_reachability);
RUN_TEST(global_evidence_producer_marks_runtime_capabilities);
RUN_TEST(global_evidence_producer_marks_sys_thread_spawn_capability);
RUN_TEST(global_evidence_producer_marks_extern_dylib_link_dependency);
RUN_TEST(global_evidence_producer_ignores_user_member_names_for_runtime_capabilities);
RUN_TEST(global_evidence_producer_marks_module_init_body);
TEST_MAIN_END()
