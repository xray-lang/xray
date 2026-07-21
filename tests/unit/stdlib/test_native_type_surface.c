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
#include "../../../src/frontend/analyzer/xanalyzer_native_types.h"
#include "../../../src/runtime/class/xenum.h"
#include "../../../stdlib/stdlib_cache.h"

static XrVMRuntime *make_full_isolate(void) {
    XrVMConfig params;
    xray_vm_config_init(&params);
    return xray_vm_new_full(&params);
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

TEST(native_module_record_and_enum_metadata) {
    XrVMRuntime *iso = make_full_isolate();
    ASSERT_NOT_NULL(iso);

    const XaBuiltinRecord *record = xa_builtin_get_record_type("net", "CopyBidirectionalResult");
    ASSERT_NOT_NULL(record);
    ASSERT_TRUE(record->is_sealed);
    ASSERT_EQ_INT(record->field_count, 2);
    ASSERT_TRUE(strcmp(record->fields[0].name, "aToB") == 0);
    ASSERT_TRUE(strcmp(record->fields[1].name, "bToA") == 0);

    XrType *record_type = xa_builtin_record_decl_type(iso, record);
    ASSERT_NOT_NULL(record_type);
    ASSERT_EQ_INT(record_type->kind, XR_KIND_RECORD);
    ASSERT_TRUE(record_type->object.is_sealed);
    ASSERT_EQ_INT(record_type->object.field_count, 2);

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

    const char *signature = xa_builtin_get_module_func_signature("net", "copyBidirectional");
    ASSERT_NOT_NULL(signature);
    XrType *fn = xa_builtin_parse_full_signature(iso, signature);
    ASSERT_NOT_NULL(fn);
    ASSERT_EQ_INT(fn->kind, XR_KIND_FUNCTION);
    ASSERT_NOT_NULL(fn->function.return_type);
    ASSERT_EQ_INT(fn->function.return_type->kind, XR_KIND_RECORD);

    const XaEffectContract *effect =
        xa_builtin_get_module_func_effect_contract("net", "copyBidirectional");
    ASSERT_NOT_NULL(effect);
    ASSERT_EQ_INT(effect->kind, XA_EFFECT_CONTRACT_ERRORS);
    ASSERT_EQ_INT(effect->error_count, 10);

    const XaBuiltinRecord *ws_options = xa_builtin_get_record_type("ws", "WsConnectOptions");
    ASSERT_NOT_NULL(ws_options);
    ASSERT_TRUE(ws_options->is_sealed);
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
    ASSERT_EQ_INT(ws_fn->function.params[1].type->kind, XR_KIND_RECORD);
    ASSERT_TRUE(ws_fn->function.params[1].type->is_nullable);

    xray_vm_delete(iso);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("stdlib/native-type-surface");
RUN_TEST(native_type_methods_match_runtime_tables);
RUN_TEST(native_type_protocol_rejects_null_isolate);
RUN_TEST(native_module_record_and_enum_metadata);
TEST_MAIN_END()
