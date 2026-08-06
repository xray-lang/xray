/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_type_surface.c - CI gate for stdlib type native method
 *                              declarations versus runtime method tables.
 */

#include "../test_framework.h"

#include "xray_vm.h"
#include "../../../src/frontend/analyzer/xanalyzer_builtins.h"
#include "../../../src/frontend/analyzer/xanalyzer_native_types.h"
#include "../../../src/runtime/class/xenum.h"
#include "../../../stdlib/stdlib_cache.h"

static XrVMRuntime *make_full_isolate(void) {
    XrVMConfig params;
    xray_vm_config_init(&params);
    return xray_vm_new_full(&params);
}

static const XaBuiltinMember *find_module_member(const char *module_name, const char *member_name) {
    const XaBuiltinModule *module = xa_builtin_get_module_info(module_name);
    if (!module || !member_name)
        return NULL;
    for (int i = 0; i < module->function_count; i++) {
        if (module->functions[i].name && strcmp(module->functions[i].name, member_name) == 0)
            return &module->functions[i];
    }
    return NULL;
}

TEST(native_type_methods_match_runtime_tables) {
    XrVMRuntime *iso = make_full_isolate();
    ASSERT_NOT_NULL(iso);
    ASSERT_EQ_INT(xray_vm_dostring(iso, "import mem\n"), 0);

    int mismatches = xa_native_verify_protocol(iso);
    xray_vm_delete(iso);

    ASSERT_EQ_INT(mismatches, 0);
}

TEST(native_type_protocol_rejects_null_isolate) {
    ASSERT_EQ_INT(xa_native_verify_protocol(NULL), -1);
}

TEST(native_type_lookup_and_typed_json_contract_are_total) {
    ASSERT_NULL(xa_builtin_get_type_info(NULL));

    const XaBuiltinType *json = xa_builtin_get_by_name("Json");
    ASSERT_NOT_NULL(json);
    const XaEffectContract *decode =
        xa_builtin_get_named_type_member_effect_contract("Json", "decode", true);
    ASSERT_NOT_NULL(decode);
    ASSERT_EQ_INT(decode->kind, XA_EFFECT_CONTRACT_NOTHROW);
}

TEST(native_module_object_and_enum_metadata) {
    XrVMRuntime *iso = make_full_isolate();
    ASSERT_NOT_NULL(iso);

    const XaBuiltinObjectShape *object_shape =
        xa_builtin_get_object_shape("net", "__CopyBidirectionalResult");
    ASSERT_NOT_NULL(object_shape);
    ASSERT_TRUE(object_shape->is_exact);
    ASSERT_EQ_INT(object_shape->field_count, 2);
    ASSERT_TRUE(strcmp(object_shape->fields[0].name, "aToB") == 0);
    ASSERT_TRUE(strcmp(object_shape->fields[1].name, "bToA") == 0);

    XrType *object_shape_type = xa_builtin_object_shape_decl_type(iso, object_shape);
    ASSERT_NOT_NULL(object_shape_type);
    ASSERT_EQ_INT(object_shape_type->kind, XR_KIND_STRUCT_OBJECT);
    ASSERT_EQ_INT(object_shape_type->object.row_mode, XR_OBJECT_ROW_EXACT);
    ASSERT_EQ_INT(object_shape_type->object.field_count, 2);

    const XaBuiltinEnum *enum_decl = xa_builtin_get_enum_type("net", "NetError");
    ASSERT_NOT_NULL(enum_decl);
    ASSERT_EQ_INT(enum_decl->variant_count, 10);
    ASSERT_TRUE(enum_decl->layout_id != 0);
    ASSERT_TRUE(strcmp(enum_decl->variants[0].name, "Timeout") == 0);
    ASSERT_TRUE(strcmp(enum_decl->variants[9].name, "OutOfMemory") == 0);

    XaEnumInfo *enum_info = NULL;
    XrType *enum_type = xa_builtin_enum_decl_type(iso, enum_decl, &enum_info);
    ASSERT_NOT_NULL(enum_type);
    ASSERT_EQ_INT(enum_type->kind, XR_KIND_ENUM);
    ASSERT_NOT_NULL(enum_info);
    ASSERT_EQ_INT(enum_info->variant_count, 10);
    ASSERT_EQ_INT(enum_type->enum_type.layout_id, enum_decl->layout_id);

    XrEnumType *runtime_enum = xr_stdlib_enum_type_get(iso, "net", "NetError");
    ASSERT_NOT_NULL(runtime_enum);
    ASSERT_TRUE(runtime_enum == xr_stdlib_enum_type_get(iso, "net", "NetError"));
    ASSERT_NOT_NULL(runtime_enum->layout);
    ASSERT_EQ_INT(runtime_enum->layout->layout_id, enum_decl->layout_id);
    ASSERT_EQ_INT(runtime_enum->members[0].ctor->layout_id, enum_decl->layout_id);
    xa_enum_info_free(enum_info);

    const XaBuiltinMember *copy_bidi = find_module_member("net", "__copyBidirectional");
    ASSERT_NOT_NULL(copy_bidi);
    ASSERT_TRUE(copy_bidi->is_internal);
    XrType *fn = xa_builtin_parse_full_signature(iso, copy_bidi->signature);
    ASSERT_NOT_NULL(fn);
    ASSERT_EQ_INT(fn->kind, XR_KIND_FUNCTION);
    ASSERT_NOT_NULL(fn->function.return_type);
    ASSERT_EQ_INT(fn->function.return_type->kind, XR_KIND_STRUCT_OBJECT);

    ASSERT_EQ_INT(copy_bidi->effect_contract.kind, XA_EFFECT_CONTRACT_ERRORS);
    ASSERT_EQ_INT(copy_bidi->effect_contract.error_count, 10);

    const XaBuiltinObjectShape *ws_options = xa_builtin_get_object_shape("ws", "WsConnectOptions");
    ASSERT_NOT_NULL(ws_options);
    ASSERT_TRUE(ws_options->is_exact);
    ASSERT_EQ_INT(ws_options->field_count, 4);
    ASSERT_TRUE(strcmp(ws_options->fields[0].name, "timeout") == 0);
    ASSERT_TRUE(strcmp(ws_options->fields[3].name, "maxMessageSize") == 0);

    const char *ws_signature = xa_builtin_get_module_func_signature("ws", "connect");
    ASSERT_NOT_NULL(ws_signature);
    XrType *ws_fn = xa_builtin_parse_full_signature(iso, ws_signature);
    ASSERT_NOT_NULL(ws_fn);
    ASSERT_EQ_INT(ws_fn->kind, XR_KIND_FUNCTION);
    ASSERT_EQ_INT(ws_fn->function.param_count, 2);
    ASSERT_NOT_NULL(ws_fn->function.params[1].type);
    ASSERT_EQ_INT(ws_fn->function.params[1].type->kind, XR_KIND_STRUCT_OBJECT);
    ASSERT_TRUE(ws_fn->function.params[1].type->is_nullable);

    const XaBuiltinObjectShape *cluster_config =
        xa_builtin_get_object_shape("cluster", "ClusterConfig");
    const XaBuiltinObjectShape *cluster_info =
        xa_builtin_get_object_shape("cluster", "ClusterInfo");
    ASSERT_NOT_NULL(cluster_config);
    ASSERT_NOT_NULL(cluster_info);
    ASSERT_TRUE(cluster_config->is_exact);
    ASSERT_TRUE(cluster_info->is_exact);
    ASSERT_EQ_INT(cluster_config->field_count, 4);
    ASSERT_EQ_INT(cluster_info->field_count, 11);
    ASSERT_TRUE(strcmp(cluster_config->fields[3].name, "tls") == 0);
    ASSERT_TRUE(strcmp(cluster_info->fields[6].name, "deadNodes") == 0);

    XrClass *cluster_info_class = xr_stdlib_object_shape_class_get(iso, "cluster", "ClusterInfo");
    ASSERT_NOT_NULL(cluster_info_class);
    ASSERT_TRUE(cluster_info_class ==
                xr_stdlib_object_shape_class_get(iso, "cluster", "ClusterInfo"));

    const XaBuiltinEnum *cluster_state = xa_builtin_get_enum_type("cluster", "ClusterNodeState");
    ASSERT_NOT_NULL(cluster_state);
    ASSERT_EQ_INT(cluster_state->variant_count, 5);
    ASSERT_TRUE(cluster_state->layout_id != 0);
    ASSERT_TRUE(strcmp(cluster_state->variants[3].name, "Connected") == 0);
    XrEnumType *runtime_cluster_state = xr_stdlib_enum_type_get(iso, "cluster", "ClusterNodeState");
    ASSERT_NOT_NULL(runtime_cluster_state);
    ASSERT_EQ_INT(runtime_cluster_state->layout->layout_id, cluster_state->layout_id);

    const char *cluster_start_signature = xa_builtin_get_module_func_signature("cluster", "start");
    const char *cluster_info_signature = xa_builtin_get_module_func_signature("cluster", "info");
    ASSERT_NOT_NULL(cluster_start_signature);
    ASSERT_NOT_NULL(cluster_info_signature);
    XrType *cluster_start_fn = xa_builtin_parse_full_signature(iso, cluster_start_signature);
    XrType *cluster_info_fn = xa_builtin_parse_full_signature(iso, cluster_info_signature);
    ASSERT_NOT_NULL(cluster_start_fn);
    ASSERT_NOT_NULL(cluster_info_fn);
    ASSERT_EQ_INT(cluster_start_fn->kind, XR_KIND_FUNCTION);
    ASSERT_EQ_INT(cluster_start_fn->function.param_count, 1);
    ASSERT_EQ_INT(cluster_start_fn->function.params[0].type->kind, XR_KIND_STRUCT_OBJECT);
    ASSERT_EQ_INT(cluster_start_fn->function.return_type->kind, XR_KIND_BOOL);
    ASSERT_EQ_INT(cluster_info_fn->kind, XR_KIND_FUNCTION);
    ASSERT_EQ_INT(cluster_info_fn->function.return_type->kind, XR_KIND_STRUCT_OBJECT);
    ASSERT_TRUE(cluster_info_fn->function.return_type->is_nullable);

    xray_vm_delete(iso);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("stdlib/native-type-surface");
RUN_TEST(native_type_methods_match_runtime_tables);
RUN_TEST(native_type_protocol_rejects_null_isolate);
RUN_TEST(native_type_lookup_and_typed_json_contract_are_total);
RUN_TEST(native_module_object_and_enum_metadata);
TEST_MAIN_END()
