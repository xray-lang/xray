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
#include "../../../src/analysis/xglobal_summary.h"
#include "../../../src/runtime/class/xclass.h"
#include "../../../src/runtime/class/xenum.h"
#include "../../../src/shared/xobject_shape.h"
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

static const XaBuiltinMember *find_type_member(const char *type_name, const char *member_name) {
    const XaBuiltinType *type = xa_builtin_get_by_name(type_name);
    if (!type || !member_name)
        return NULL;
    for (int i = 0; i < type->member_count; i++) {
        if (type->members[i].name && strcmp(type->members[i].name, member_name) == 0)
            return &type->members[i];
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

TEST(native_receiver_alias_contracts_are_typed_data) {
    XrVMRuntime *iso = make_full_isolate();
    ASSERT_NOT_NULL(iso);

    XrType *array_type = xr_type_new_array(iso, xr_type_new_int(iso));
    ASSERT_NOT_NULL(array_type);
    ASSERT_TRUE(xa_builtin_member_returns_receiver(array_type, "reserve"));
    ASSERT_TRUE(xa_builtin_member_returns_receiver(array_type, "resize"));
    ASSERT_TRUE(xa_builtin_member_returns_receiver(array_type, "reverse"));
    ASSERT_TRUE(xa_builtin_member_returns_receiver(array_type, "sort"));
    ASSERT_TRUE(xa_builtin_member_returns_receiver(array_type, "fill"));
    ASSERT_FALSE(xa_builtin_member_returns_receiver(array_type, "concat"));

    static const struct {
        const char *type;
        const char *member;
    } receiver_aliases[] = {
        {"Array", "reserve"},        {"Array", "resize"},        {"Array", "reverse"},
        {"Array", "sort"},           {"Array", "fill"},          {"iterator", "iterator"},
        {"StringBuilder", "append"}, {"StringBuilder", "clear"},
    };
    for (size_t i = 0; i < sizeof(receiver_aliases) / sizeof(receiver_aliases[0]); i++) {
        const XaBuiltinMember *member =
            find_type_member(receiver_aliases[i].type, receiver_aliases[i].member);
        ASSERT_NOT_NULL(member);
        ASSERT_EQ_INT(member->return_ownership, XA_BUILTIN_RETURN_RECEIVER);
    }
    const XaBuiltinMember *concat = find_type_member("Array", "concat");
    ASSERT_NOT_NULL(concat);
    ASSERT_EQ_INT(concat->return_ownership, XA_BUILTIN_RETURN_UNKNOWN);

    xray_vm_delete(iso);
}

TEST(native_type_lookup_and_typed_json_contract_are_total) {
    ASSERT_NULL(xa_builtin_get_type_info(NULL));

    const XaBuiltinType *json = xa_builtin_get_by_name("JSON");
    ASSERT_NOT_NULL(json);
    const XaEffectContract *decode =
        xa_builtin_get_named_type_member_effect_contract("JSON", "decode", true);
    ASSERT_NOT_NULL(decode);
    ASSERT_EQ_INT(decode->kind, XA_EFFECT_CONTRACT_NOTHROW);
}

TEST(native_module_object_and_enum_metadata) {
    XrVMRuntime *iso = make_full_isolate();
    ASSERT_NOT_NULL(iso);

    const XaBuiltinObjectShape *object_shape = xa_builtin_get_object_shape("Coro", "CoroDeadlock");
    ASSERT_NOT_NULL(object_shape);
    ASSERT_TRUE(object_shape->is_exact);
    ASSERT_EQ_INT(object_shape->field_count, 2);
    ASSERT_TRUE(strcmp(object_shape->fields[0].name, "members") == 0);
    ASSERT_TRUE(strcmp(object_shape->fields[1].name, "reason") == 0);

    XrType *object_shape_type = xa_builtin_object_shape_decl_type(iso, object_shape);
    ASSERT_NOT_NULL(object_shape_type);
    ASSERT_EQ_INT(object_shape_type->kind, XR_KIND_STRUCT_OBJECT);
    ASSERT_EQ_INT(object_shape_type->object.field_count, 2);

    XrClass *record_class = xr_stdlib_record_class_get(iso, "net", "__CopyBidirectionalResult");
    ASSERT_NOT_NULL(record_class);
    int a_to_b_index = xr_class_lookup_field_by_name(iso, record_class, "aToB");
    int b_to_a_index = xr_class_lookup_field_by_name(iso, record_class, "bToA");
    int expected_a_to_b =
        xg_object_stable_name_key("bToA") < xg_object_stable_name_key("aToB") ? 1 : 0;
    ASSERT_EQ_INT(a_to_b_index, expected_a_to_b);
    ASSERT_EQ_INT(b_to_a_index, 1 - expected_a_to_b);

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

    const XaBuiltinMember *connect_fd = find_module_member("net", "__connectFd");
    ASSERT_NOT_NULL(connect_fd);
    ASSERT_TRUE(connect_fd->is_internal);
    XrType *fn = xa_builtin_parse_full_signature(iso, connect_fd->signature);
    ASSERT_NOT_NULL(fn);
    ASSERT_EQ_INT(fn->kind, XR_KIND_FUNCTION);
    ASSERT_NOT_NULL(fn->function.return_type);
    /* __connectFd returns NetConn? (a nullable builtin handle), never a
     * `NetConn | int` union: unioning a native handle with a scalar poisons
     * handle typing module-wide (see xray-docs/known_bugs.md 2026-08-09). */
    ASSERT_EQ_INT(fn->function.return_type->kind, XR_KIND_INSTANCE);
    ASSERT_TRUE(fn->function.return_type->is_nullable);

    /* ws is a pure-script module: its entire connection layer (WsConn,
     * connect, send/recv, serve) lives in stdlib/ws/ws.xr, so ws exposes no
     * native object shapes and no native module-function signatures. Both
     * lookups must resolve to NULL; a non-NULL result means ws regressed
     * back into a dual C/script track. */
    ASSERT_NULL(xa_builtin_get_object_shape("ws", "WsConnectOptions"));
    ASSERT_NULL(xa_builtin_get_module_func_signature("ws", "connect"));

    const XaBuiltinObjectShape *cluster_config =
        xa_builtin_get_object_shape("cluster", "ClusterConfig");
    const XaBuiltinObjectShape *cluster_info =
        xa_builtin_get_object_shape("cluster", "ClusterInfo");
    ASSERT_NULL(cluster_config);
    ASSERT_NOT_NULL(cluster_info);
    ASSERT_TRUE(cluster_info->is_exact);
    ASSERT_EQ_INT(cluster_info->field_count, 10);
    ASSERT_TRUE(strcmp(cluster_info->fields[4].name, "listeners") == 0);
    ASSERT_TRUE(strcmp(cluster_info->fields[5].name, "deadNodes") == 0);

    XrClass *cluster_info_class = xr_stdlib_record_class_get(iso, "cluster", "ClusterInfo");
    ASSERT_NOT_NULL(cluster_info_class);
    ASSERT_TRUE(cluster_info_class == xr_stdlib_record_class_get(iso, "cluster", "ClusterInfo"));

    const XaBuiltinEnum *cluster_state = xa_builtin_get_enum_type("cluster", "ClusterNodeState");
    ASSERT_NOT_NULL(cluster_state);
    ASSERT_EQ_INT(cluster_state->variant_count, 5);
    ASSERT_TRUE(cluster_state->layout_id != 0);
    ASSERT_TRUE(strcmp(cluster_state->variants[3].name, "Connected") == 0);
    XrEnumType *runtime_cluster_state = xr_stdlib_enum_type_get(iso, "cluster", "ClusterNodeState");
    ASSERT_NOT_NULL(runtime_cluster_state);
    ASSERT_EQ_INT(runtime_cluster_state->layout->layout_id, cluster_state->layout_id);

    const char *cluster_start_signature = xa_builtin_get_module_func_signature("cluster", "start");
    const XaBuiltinMember *cluster_start_primitive = find_module_member("cluster", "__start");
    const char *cluster_info_signature = xa_builtin_get_module_func_signature("cluster", "info");
    ASSERT_NULL(cluster_start_signature);
    ASSERT_NOT_NULL(cluster_start_primitive);
    ASSERT_TRUE(cluster_start_primitive->is_internal);
    ASSERT_NOT_NULL(cluster_info_signature);
    XrType *cluster_start_fn =
        xa_builtin_parse_full_signature(iso, cluster_start_primitive->signature);
    XrType *cluster_info_fn = xa_builtin_parse_full_signature(iso, cluster_info_signature);
    ASSERT_NOT_NULL(cluster_start_fn);
    ASSERT_NOT_NULL(cluster_info_fn);
    ASSERT_EQ_INT(cluster_start_fn->kind, XR_KIND_FUNCTION);
    ASSERT_EQ_INT(cluster_start_fn->function.param_count, 8);
    ASSERT_EQ_INT(cluster_start_fn->function.params[0].type->kind, XR_KIND_STRING);
    ASSERT_EQ_INT(cluster_start_fn->function.params[1].type->kind, XR_KIND_INT);
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
RUN_TEST(native_receiver_alias_contracts_are_typed_data);
RUN_TEST(native_type_lookup_and_typed_json_contract_are_total);
RUN_TEST(native_module_object_and_enum_metadata);
TEST_MAIN_END()
