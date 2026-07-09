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

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("stdlib/native-type-surface");
RUN_TEST(native_type_methods_match_runtime_tables);
RUN_TEST(native_type_protocol_rejects_null_isolate);
TEST_MAIN_END()
