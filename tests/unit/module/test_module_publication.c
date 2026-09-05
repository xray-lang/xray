/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "../test_framework.h"
#include "base/xmalloc.h"
#include "module/xmodule.h"
#include "module/xmodule_resolver.h"
#include "module/xstdlib_embedded.h"
#include "xray_vm.h"

TEST(module_exports_are_invisible_until_atomic_publication) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(isolate);

    XrModule *module = xr_module_create_native(isolate, "publication_test");
    ASSERT_NOT_NULL(module);
    ASSERT_EQ_INT(xr_module_state(module), XR_MODULE_NEW);

    xr_module_add_export_sym(isolate, module, 10, xr_int(41), true);
    xr_module_add_export_sym(isolate, module, 11, xr_int(1), false);
    ASSERT_TRUE(XR_IS_NULL(xr_module_get_sym(module, 10)));
    ASSERT_FALSE(xr_module_has_sym(module, 10));

    ASSERT_TRUE(xr_module_begin_initialization(module));
    ASSERT_EQ_INT(xr_module_state(module), XR_MODULE_INITIALIZING);
    ASSERT_FALSE(xr_module_begin_initialization(module));
    ASSERT_TRUE(XR_IS_NULL(xr_module_get_sym(module, 10)));

    ASSERT_TRUE(xr_module_publish(module));
    ASSERT_EQ_INT(xr_module_state(module), XR_MODULE_PUBLISHED);
    ASSERT_EQ_INT(XR_TO_INT(xr_module_get_sym(module, 10)), 41);
    ASSERT_EQ_INT(XR_TO_INT(xr_module_get_sym(module, 11)), 1);
    ASSERT_TRUE(xr_module_is_const_sym(module, 10));
    ASSERT_FALSE(xr_module_is_const_sym(module, 11));

    ASSERT_FALSE(xr_module_set_sym(module, 10, xr_int(99)));
    ASSERT_TRUE(xr_module_set_sym(module, 11, xr_int(2)));
    ASSERT_EQ_INT(XR_TO_INT(xr_module_get_sym(module, 10)), 41);
    ASSERT_EQ_INT(XR_TO_INT(xr_module_get_sym(module, 11)), 2);

    xr_module_add_export_sym(isolate, module, 10, xr_int(99), true);
    ASSERT_EQ_INT(XR_TO_INT(xr_module_get_sym(module, 10)), 41);
    ASSERT_FALSE(xr_module_publish(module));

    XrValue text_value = xr_module_import(isolate, "text");
    ASSERT_TRUE(xr_value_is_module(text_value));
    XrValue lower = xr_module_get_export(isolate, xr_value_to_module(text_value), "lower");
    ASSERT_TRUE(xr_value_is_closure(lower));

    XrValue base64_value = xr_module_import(isolate, "base64");
    ASSERT_TRUE(xr_value_is_module(base64_value));
    XrValue encode = xr_module_get_export(isolate, xr_value_to_module(base64_value), "encode");
    ASSERT_TRUE(xr_value_is_closure(encode));

#if defined(XR_HAS_DATA_FORMATS)
    XrValue csv_value = xr_module_import(isolate, "csv");
    ASSERT_TRUE(xr_value_is_module(csv_value));
    XrValue parse = xr_module_get_export(isolate, xr_value_to_module(csv_value), "parse");
    ASSERT_TRUE(xr_value_is_closure(parse));
#else
    ASSERT_TRUE(XR_IS_NULL(xr_module_import(isolate, "csv")));
#endif

#if defined(XR_HAS_FILESYSTEM)
    XrValue os_value = xr_module_import(isolate, "os");
    ASSERT_TRUE(xr_value_is_module(os_value));
    XrModule *os_module = xr_value_to_module(os_value);
    ASSERT_TRUE(XR_IS_NULL(xr_module_get_export(isolate, os_module, "sleep")));
    ASSERT_TRUE(XR_IS_NULL(xr_module_get_export(isolate, os_module, "__sleep")));
    ASSERT_TRUE(xr_value_is_cfunction(xr_module_get_export(isolate, os_module, "__getpid")));
#else
    ASSERT_TRUE(XR_IS_NULL(xr_module_import(isolate, "os")));
#endif

    xr_module_free(module);
    xray_vm_delete(isolate);
}

TEST(failed_module_never_publishes_partial_exports) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(isolate);

    XrModule *module = xr_module_create_script(isolate, "failed_publication", "failed.xr");
    ASSERT_NOT_NULL(module);
    ASSERT_TRUE(xr_module_begin_initialization(module));
    xr_module_add_export_sym(isolate, module, 20, xr_int(7), true);

    xr_module_fail(module);
    ASSERT_EQ_INT(xr_module_state(module), XR_MODULE_FAILED);
    ASSERT_TRUE(XR_IS_NULL(xr_module_get_sym(module, 20)));
    ASSERT_FALSE(xr_module_has_sym(module, 20));
    ASSERT_FALSE(xr_module_publish(module));

    xr_module_free(module);
    xray_vm_delete(isolate);
}

TEST(declared_native_entry_binding_is_exact_and_one_shot) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(isolate);

    XrModule *foreign = xr_module_create_native(isolate, "foreign");
    ASSERT_NOT_NULL(foreign);
    ASSERT_FALSE(xr_stdlib_module_install_native_entries(isolate, foreign, "os"));

    XrModule *source_only = xr_module_create_native(isolate, "base64");
    ASSERT_NOT_NULL(source_only);
    ASSERT_TRUE(xr_stdlib_module_install_native_entries(isolate, source_only, "base64"));
    ASSERT_TRUE(xr_module_begin_initialization(source_only));
    ASSERT_TRUE(xr_module_publish(source_only));
    ASSERT_FALSE(xr_stdlib_module_install_native_entries(isolate, source_only, "base64"));

#if defined(XR_HAS_FILESYSTEM)
    XrModule *os_module = xr_module_create_native(isolate, "os");
    ASSERT_NOT_NULL(os_module);
    ASSERT_TRUE(xr_stdlib_module_install_native_entries(isolate, os_module, "os"));
    ASSERT_TRUE(os_module->export_count > 0);
    ASSERT_FALSE(xr_stdlib_module_install_native_entries(isolate, os_module, "os"));
    xr_module_free(os_module);
#endif

    xr_module_free(source_only);
    xr_module_free(foreign);
    xray_vm_delete(isolate);
}

TEST(stdlib_resolver_follows_build_authority) {
    XrModuleResolverConfig config = {0};
    XrModuleResolver *resolver = xr_module_resolver_new(&config);
    ASSERT_NOT_NULL(resolver);

    XrModuleId text_id = {0};
    char *error = NULL;
    ASSERT_EQ_INT(xr_module_resolver_resolve(resolver, "text", NULL, NULL, &text_id, &error), 0);
    ASSERT_NULL(error);
    ASSERT_EQ_INT(text_id.kind, XR_MOD_STDLIB);
    ASSERT_STR_EQ(text_id.canonical, "stdlib-module-v1:module=4:text:path=12:text/text.xr");
    ASSERT_NULL(text_id.source_path);
    xr_module_id_cleanup(&text_id);

    XrModuleId csv_id = {0};
#if defined(XR_HAS_DATA_FORMATS)
    ASSERT_EQ_INT(xr_module_resolver_resolve(resolver, "csv", NULL, NULL, &csv_id, &error), 0);
    ASSERT_NULL(error);
    ASSERT_EQ_INT(csv_id.kind, XR_MOD_STDLIB);
    ASSERT_STR_EQ(csv_id.canonical, "stdlib-module-v1:module=3:csv:path=10:csv/csv.xr");
    ASSERT_NULL(csv_id.source_path);
    xr_module_id_cleanup(&csv_id);
#else
    ASSERT_EQ_INT(xr_module_resolver_resolve(resolver, "csv", NULL, NULL, &csv_id, &error), -1);
    ASSERT_NOT_NULL(error);
    xr_free(error);
    error = NULL;
#endif

#if !defined(XR_HAS_FILESYSTEM)
    XrModuleId os_id = {0};
    ASSERT_EQ_INT(xr_module_resolver_resolve(resolver, "os", NULL, NULL, &os_id, &error), -1);
    ASSERT_NOT_NULL(error);
    xr_free(error);
    error = NULL;
#endif
    xr_module_resolver_free(resolver);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Module publication");
RUN_TEST(module_exports_are_invisible_until_atomic_publication);
RUN_TEST(failed_module_never_publishes_partial_exports);
RUN_TEST(declared_native_entry_binding_is_exact_and_one_shot);
RUN_TEST(stdlib_resolver_follows_build_authority);
TEST_MAIN_END()
