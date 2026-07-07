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
    XgBodySummary body = {.func_id = 4,
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
                              .kind = XG_CALL_METHOD,
                              .static_target_func_id = 0,
                              .receiver_static_class_id = 2,
                              .method_id = 3,
                              .arg_type_key_start = 0,
                              .arg_count = 1,
                              .flags = XG_CALL_USES_DEFAULT_ARGS};

    xg_global_evidence_init(&ev, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&ev, &decl));
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &cls));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&ev, &method));
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_impl(&ev, &impl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&ev, &body));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&ev, &call));

    dump = xg_global_evidence_dump(&ev);
    ASSERT_NOT_NULL(dump);
    ASSERT_NOT_NULL(strstr(dump, "xglobal-evidence v0 profile=native_release"));
    ASSERT_NOT_NULL(strstr(dump, "decl 0 id=2 module=1 kind=class"));
    ASSERT_NOT_NULL(strstr(dump, "class 0 id=2 module=1 decl=2 name=44 parent=0"));
    ASSERT_NOT_NULL(strstr(dump, "method 0 id=3 owner=2"));
    ASSERT_NOT_NULL(strstr(dump, "interface-impl 0 class=2 interface=123"));
    ASSERT_NOT_NULL(strstr(dump, "body 0 func=4"));
    ASSERT_NOT_NULL(strstr(dump, "callsite 0 id=5 owner=4 span=0 kind=method"));

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
                              .body_hash = 0x999,
                              .effect_bits = 0,
                              .escape_bits = 0,
                              .capability_bits = XG_CAP_COROUTINE | XG_CAP_CHANNEL,
                              .callsite_start = 0,
                              .callsite_count = 0,
                              .metadata_use_bits = XG_METADATA_TYPENAME,
                              .static_data_use_bits = XG_STATIC_DATA_COMPTIME_VALUE};
    XaotBundle bundle;
    char *dump;

    xg_global_evidence_init(&ev, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &base));
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &child));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&ev, &base_method));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&ev, &child_method));
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&ev, &call));
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_impl(&ev, &impl));
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&ev, &cap_body));

    memset(&bundle, 0, sizeof(bundle));
    ASSERT_TRUE(xaot_bundle_set_global_evidence(&bundle, &ev, XG_BUILD_NATIVE_RELEASE));
    ASSERT_EQ_UINT(bundle.nclass_hierarchy_plans, 2);
    ASSERT_EQ_UINT(bundle.nclass_layout_plans, 2);
    ASSERT_EQ_UINT(bundle.nmethod_dispatch_plans, 1);
    ASSERT_EQ_UINT(bundle.ninterface_use_plans, 1);
    ASSERT_EQ_UINT(bundle.nmetadata_plans, 1);
    ASSERT_EQ_UINT(bundle.ncapability_plans, 2);
    ASSERT_EQ_UINT(bundle.nstatic_data_plans, 1);
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

    dump = xaot_bundle_dump_plan(&bundle);
    ASSERT_NOT_NULL(dump);
    ASSERT_NOT_NULL(strstr(dump, "class-hierarchy 0 id=1"));
    ASSERT_NOT_NULL(strstr(dump, "class-layout 1 id=2"));
    ASSERT_NOT_NULL(strstr(dump, "method-dispatch 0 callsite=7 span=0 kind=direct"));
    ASSERT_NOT_NULL(strstr(dump, "interface-use 0 interface=77 implementor=2"));
    ASSERT_NOT_NULL(strstr(dump, "metadata 0 name=typename bodies=1 decls=0 action=link"));
    ASSERT_NOT_NULL(strstr(dump, "capability 0 name=coroutine bodies=1 action=link"));
    ASSERT_NOT_NULL(strstr(dump, "static-data 0 name=comptime_value bodies=1 action=materialize"));
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
    XgCallsiteSummary call = {.callsite_id = 1,
                              .owner_func_id = 9,
                              .source_span_id = 42,
                              .kind = XG_CALL_METHOD,
                              .receiver_static_class_id = 1,
                              .method_id = 1,
                              .method_name_id = draw_name_id,
                              .arg_count = 1};
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
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &cls));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&ev, &method));
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
    xi_call.line = 99;
    ASSERT_NULL(xaot_bundle_find_method_dispatch_plan_for_xi_call(&good, &xi_call));
    xi_call.line = 42;
    xi_call.aux = (void *) "other";
    ASSERT_NULL(xaot_bundle_find_method_dispatch_plan_for_xi_call(&good, &xi_call));
    xi_call.aux = NULL;
    ASSERT_NULL(xaot_bundle_find_method_dispatch_plan_for_xi_call(&good, &xi_call));
    xi_call.aux = (void *) "draw";
    ev.callsites[0].method_name_id = 0;
    ASSERT_NULL(xaot_bundle_find_method_dispatch_plan_for_xi_call(&good, &xi_call));
    ev.callsites[0].method_name_id = draw_name_id;
    memset(err, 0, sizeof(err));
    ASSERT_MSG(xaot_verify_bundle(&good, XAOT_VERIFY_AOT_READY, err, sizeof(err)), err);
    good.method_dispatch_plans[0].kind = XAOT_DISPATCH_VTABLE;
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(!xaot_verify_bundle(&good, XAOT_VERIFY_AOT_READY, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "AOT dispatch plan kind does not re-derive"));
    xaot_bundle_free(&good);

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

TEST(global_evidence_lowers_interface_call_to_type_switch) {
    XgGlobalEvidence ev;
    XgBuildKey key = {.source_hash = 0x81,
                      .compiler_semver_hash = 0x82,
                      .profile_hash = 0x83,
                      .imported_summary_hash = 0x84,
                      .module_id = 1,
                      .profile = XG_BUILD_NATIVE_RELEASE};
    XgClassSummary circle = {.class_id = 1,
                             .parent_class_id = XG_NO_ID,
                             .flags = XG_CLASS_INFERRED_FINAL,
                             .method_start = 1,
                             .method_count = 1,
                             .interface_start = 1,
                             .interface_count = 1,
                             .decl_kind = XG_DECL_CLASS};
    XgClassSummary rect = {.class_id = 2,
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
    XgCallsiteSummary call = {.callsite_id = 1,
                              .owner_func_id = 9,
                              .kind = XG_CALL_INTERFACE,
                              .receiver_static_interface_id = 77,
                              .method_id = 700,
                              .method_name_id = 700,
                              .method_signature_key = 701};
    XiFunc init_func;
    XiModule module;
    XiModule *modules[1];
    XaotBundle bundle;
    const XaotMethodDispatchPlan *plan;
    char err[256];

    xg_global_evidence_init(&ev, key);
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &circle));
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&ev, &rect));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&ev, &circle_draw));
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&ev, &rect_draw));
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_impl(&ev, &impls[0]));
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_impl(&ev, &impls[1]));
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
    ASSERT_EQ_UINT(plan->target_count, 2);
    ASSERT_EQ_UINT(bundle.ndispatch_target_cases, 2);
    ASSERT_EQ_UINT(bundle.dispatch_target_cases[0].receiver_class_id, 1);
    ASSERT_EQ_UINT(bundle.dispatch_target_cases[0].method_id, 1);
    ASSERT_EQ_UINT(bundle.dispatch_target_cases[1].receiver_class_id, 2);
    ASSERT_EQ_UINT(bundle.dispatch_target_cases[1].method_id, 2);
    memset(err, 0, sizeof(err));
    ASSERT_TRUE(xaot_verify_bundle(&bundle, XAOT_VERIFY_AOT_READY, err, sizeof(err)));

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
    for (uint32_t i = 0; i < nmethods; i++)
        ASSERT_NOT_NULL(xg_global_evidence_add_method(&ev, &methods[i]));

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
    XgBodySummary body = {.func_id = 1,
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
        if ((ev.bodies[i].capability_bits & XG_CAP_OBJECTS) != 0)
            object_capability_bodies++;
    }
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

    XgInterfaceId shape_id = ev.interface_impls[0].interface_id;
    uint32_t interface_calls = 0;
    uint32_t interface_calls_with_signature = 0;
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
            if (call->source_span_id != 0)
                interface_calls_with_source++;
        }
        if (call->kind == XG_CALL_METHOD)
            class_method_calls++;
    }
    ASSERT_EQ_UINT(interface_calls, 2);
    ASSERT_EQ_UINT(interface_calls_with_signature, 2);
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
RUN_TEST(global_evidence_lowers_interface_call_to_type_switch);
RUN_TEST(global_evidence_verifier_rederives_class_graph_flags);
RUN_TEST(global_evidence_verifier_rederives_method_override_graph);
RUN_TEST(global_evidence_verifier_rederives_interface_implementor_set);
RUN_TEST(global_evidence_verifier_rederives_profile_actions);
RUN_TEST(global_evidence_producer_finalizes_class_graph_order_independently);
RUN_TEST(global_evidence_producer_resolves_method_callsite_receivers);
RUN_TEST(global_evidence_producer_resolves_interface_callsite_receivers);
RUN_TEST(global_evidence_producer_marks_metadata_reachability);
RUN_TEST(global_evidence_producer_marks_static_data_reachability);
RUN_TEST(global_evidence_producer_marks_runtime_capabilities);
RUN_TEST(global_evidence_producer_ignores_user_member_names_for_runtime_capabilities);
RUN_TEST(global_evidence_producer_marks_module_init_body);
TEST_MAIN_END()
