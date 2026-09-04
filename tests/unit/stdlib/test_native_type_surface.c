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
#include "runtime/xisolate_api.h"
#include "module/xmodule_identity.h"
#include "../../../src/frontend/analyzer/xanalyzer_builtins.h"
#include "../../../src/frontend/analyzer/xanalyzer_native_types.h"
#include "../../../src/analysis/xglobal_summary.h"
#include "../../../src/runtime/class/xclass.h"
#include "../../../src/runtime/class/xenum.h"
#include "../../../src/shared/xobject_shape.h"
#include "../../../src/stdlib/xstdlib_metadata.h"
#include "../../../src/module/xstdlib_runtime_cache.h"

static const XrModuleIdentityAuthority k_native_surface_memory_authority = {
    .kind = XR_MODULE_IDENTITY_MEMORY,
    .namespace_id = "native-surface-fixture-v1",
};

static XrVMRuntime *make_full_isolate(void) {
    XrVMConfig params = {0};
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
    ASSERT_EQ_INT(xr_isolate_dostring(iso, "import mem\n", &k_native_surface_memory_authority), 0);

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
    ASSERT_EQ_INT(concat->return_ownership, XA_BUILTIN_RETURN_FRESH);

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

    /* Peer records, metrics, framing and queues are pure cluster.xr state.
     * Native metadata therefore exposes neither public source records nor a
     * private transport snapshot shape. */
    ASSERT_NULL(xa_builtin_get_object_shape("cluster", "ClusterConfig"));
    ASSERT_NULL(xa_builtin_get_object_shape("cluster", "ClusterInfo"));
    ASSERT_NULL(xa_builtin_get_object_shape("cluster", "__ClusterNodeSnapshot"));
    ASSERT_NULL(xa_builtin_get_object_shape("cluster", "__ClusterRuntimeSnapshot"));
    ASSERT_NULL(xr_stdlib_record_class_get(iso, "cluster", "__ClusterNodeSnapshot"));

    const XaBuiltinEnum *cluster_state = xa_builtin_get_enum_type("cluster", "ClusterNodeState");
    ASSERT_NULL(cluster_state);
    ASSERT_NULL(xr_stdlib_enum_type_get(iso, "cluster", "ClusterNodeState"));

    const char *cluster_start_signature = xa_builtin_get_module_func_signature("cluster", "start");
    const XaBuiltinMember *cluster_start_primitive = find_module_member("cluster", "__start");
    const char *cluster_info_signature = xa_builtin_get_module_func_signature("cluster", "info");
    ASSERT_NULL(cluster_start_signature);
    ASSERT_NOT_NULL(cluster_start_primitive);
    ASSERT_TRUE(cluster_start_primitive->is_internal);
    ASSERT_NULL(cluster_info_signature);
    ASSERT_NULL(find_module_member("cluster", "__info"));
    ASSERT_NULL(find_module_member("cluster", "__registerNodeMonitor"));
    ASSERT_NULL(find_module_member("cluster", "__registerCoroutineMonitor"));
    ASSERT_NULL(find_module_member("cluster", "__peerName"));
    ASSERT_NULL(find_module_member("cluster", "__peerGeneration"));
    ASSERT_NULL(find_module_member("cluster", "__readPeer"));
    ASSERT_NULL(find_module_member("cluster", "__writePeer"));
    ASSERT_NULL(find_module_member("cluster", "__peerEnqueue"));
    ASSERT_NULL(find_module_member("cluster", "__broadcast"));
    XrType *cluster_start_fn =
        xa_builtin_parse_full_signature(iso, cluster_start_primitive->signature);
    ASSERT_NOT_NULL(cluster_start_fn);
    ASSERT_EQ_INT(cluster_start_fn->kind, XR_KIND_FUNCTION);
    ASSERT_EQ_INT(cluster_start_fn->function.param_count, 0);
    ASSERT_EQ_INT(cluster_start_fn->function.return_type->kind, XR_KIND_BOOL);

    /* Whole-input buffering and path metadata projections are io.xr policy.
     * The native registry retains only the descriptor read and stat leaves. */
    ASSERT_NULL(find_module_member("io", "__readFile"));
    ASSERT_NULL(find_module_member("io", "__readFileBytes"));
    ASSERT_NULL(find_module_member("io", "__readStdin"));
    ASSERT_NULL(find_module_member("io", "__readStdinBytes"));
    ASSERT_NULL(find_module_member("io", "__exists"));
    ASSERT_NULL(find_module_member("io", "__fileSize"));
    ASSERT_NULL(find_module_member("io", "__isDir"));
    ASSERT_NULL(find_module_member("io", "__isFile"));
    ASSERT_NULL(find_module_member("io", "__isSymlink"));
    ASSERT_NOT_NULL(find_module_member("io", "__fileRead"));
    ASSERT_NOT_NULL(find_module_member("io", "__stat"));

    xray_vm_delete(iso);
}

TEST(native_direct_member_resolves_private_page_alloc_leaf) {
    const XrStdlibDefEntry *leaf =
        xr_stdlib_metadata_exact_native_direct_member("mem", "__pageAlloc", 2);

    ASSERT_NOT_NULL(leaf);
    ASSERT_EQ_INT(leaf->argc, 2);
    ASSERT_NULL(xr_stdlib_metadata_exact_native_direct_member("mem", "__pageAlloc", 1));
    ASSERT_NULL(xr_stdlib_metadata_exact_native_direct_member("mem", "pageAlloc", 2));
}

TEST(native_direct_member_identity_rejects_duplicate_tuple) {
    const XrStdlibDefEntry entries[] = {
        {.module = "fixture", .name = "call", .argc = 1},
        {.module = "fixture", .name = "call", .argc = 2},
        {.module = "fixture", .name = "call", .argc = 1},
    };

    ASSERT_NULL(xr_stdlib_metadata_unique_func_arity_in_entries(entries, 3, "fixture", "call", 1));
    ASSERT_TRUE(xr_stdlib_metadata_unique_func_arity_in_entries(entries, 3, "fixture", "call", 2) ==
                &entries[1]);
    ASSERT_NULL(xr_stdlib_metadata_unique_func_arity_in_entries(entries, 3, ".fixture", "call", 2));
    ASSERT_NULL(xr_stdlib_metadata_unique_func_arity_in_entries(entries, 3, "fixture", "", 2));
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("stdlib/native-type-surface");
RUN_TEST(native_type_methods_match_runtime_tables);
RUN_TEST(native_type_protocol_rejects_null_isolate);
RUN_TEST(native_receiver_alias_contracts_are_typed_data);
RUN_TEST(native_type_lookup_and_typed_json_contract_are_total);
RUN_TEST(native_module_object_and_enum_metadata);
RUN_TEST(native_direct_member_resolves_each_page_alloc_arity);
RUN_TEST(native_direct_member_identity_rejects_duplicate_tuple);
TEST_MAIN_END()
