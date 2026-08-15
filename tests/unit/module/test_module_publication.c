/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "../test_framework.h"
#include "module/xmodule.h"
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

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Module publication");
RUN_TEST(module_exports_are_invisible_until_atomic_publication);
RUN_TEST(failed_module_never_publishes_partial_exports);
TEST_MAIN_END()
