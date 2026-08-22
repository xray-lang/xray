/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_api_defense.c - Unit tests for API boundary defense
 *
 * KEY CONCEPT:
 *   Verifies that public isolate API functions
 *   gracefully handle NULL parameters without crashing.
 *   In Release builds, these return early with safe defaults.
 *   In Debug builds, XR_DCHECK fires before the early return.
 */

#include "../test_framework.h"
#include "xray_vm.h"
#include "runtime/xisolate_api.h"
#include "module/xmodule_identity.h"
#include <stddef.h>

static const XrModuleIdentityAuthority k_api_defense_memory_authority = {
    .kind = XR_MODULE_IDENTITY_MEMORY,
    .namespace_id = "api-defense-fixture-v1",
};

/* ========== Isolate Lifecycle NULL Safety ========== */

TEST(api_isolate_delete_null) {
    // xray_vm_delete(NULL) should be safe (no-op)
    xray_vm_delete(NULL);
    ASSERT_TRUE(1);  // survived without crash
}

/* ========== Isolate Scripting NULL Safety ========== */

#ifndef NDEBUG
// In debug builds, xray_api_checkr triggers XR_DCHECK (abort).
// Only test NULL safety in Release builds.
#define SKIP_NULL_RETURN_TESTS 1
#else
#define SKIP_NULL_RETURN_TESTS 0
#endif

TEST(api_isolate_dostring_null_isolate) {
    if (SKIP_NULL_RETURN_TESTS) {
        ASSERT_TRUE(1);
        return;
    }
    int result =
        xr_isolate_dostring(NULL, "print(1)", &k_api_defense_memory_authority);
    ASSERT_EQ_INT(result, -1);
}

TEST(api_isolate_dostring_null_source) {
    if (SKIP_NULL_RETURN_TESTS) {
        ASSERT_TRUE(1);
        return;
    }
    // Need a valid isolate to test NULL source
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    if (!iso) {
        ASSERT_TRUE(1);
        return;
    }  // alloc failure

    int result = xr_isolate_dostring(iso, NULL, &k_api_defense_memory_authority);
    ASSERT_EQ_INT(result, -1);

    xray_vm_delete(iso);
}

TEST(api_isolate_dostring_missing_or_invalid_authority) {
    if (SKIP_NULL_RETURN_TESTS) {
        ASSERT_TRUE(1);
        return;
    }
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);
    XrModuleIdentityAuthority invalid = {
        .kind = XR_MODULE_IDENTITY_MEMORY,
        .namespace_id = "<eval>",
    };
    ASSERT_EQ_INT(xr_isolate_dostring(iso, "print(1)", NULL), -1);
    ASSERT_EQ_INT(xr_isolate_dostring(iso, "print(1)", &invalid), -1);
    xray_vm_delete(iso);
}

TEST(api_isolate_dofile_null_isolate) {
    if (SKIP_NULL_RETURN_TESTS) {
        ASSERT_TRUE(1);
        return;
    }
    int result = xr_isolate_dofile(NULL, "test.xr", NULL);
    ASSERT_EQ_INT(result, -1);
}

TEST(api_isolate_dofile_null_filename) {
    if (SKIP_NULL_RETURN_TESTS) {
        ASSERT_TRUE(1);
        return;
    }
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    if (!iso) {
        ASSERT_TRUE(1);
        return;
    }

    int result = xr_isolate_dofile(iso, NULL, NULL);
    ASSERT_EQ_INT(result, -1);

    xray_vm_delete(iso);
}

/* ========== Isolate Lifecycle (valid) ========== */

TEST(api_isolate_create_destroy) {
    XrVMConfig params = {0};

    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);
    ASSERT_TRUE(xr_isolate_current() == iso);
    xr_isolate_exit();
    ASSERT_NULL(xr_isolate_current());
    xr_isolate_enter(NULL);
    ASSERT_NULL(xr_isolate_current());
    xr_isolate_enter(iso);
    ASSERT_TRUE(xr_isolate_current() == iso);
    ASSERT_NULL(xr_isolate_get_scheduler_runtime(iso));

    xr_isolate_multicore_init(NULL, 1);
    xr_isolate_multicore_init(iso, 1);
    ASSERT_NOT_NULL(xr_isolate_get_scheduler_runtime(iso));

    xray_vm_delete(iso);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("API Boundary Defense - NULL Safety");
RUN_TEST(api_isolate_delete_null);
RUN_TEST(api_isolate_dostring_null_isolate);
RUN_TEST(api_isolate_dostring_null_source);
RUN_TEST(api_isolate_dostring_missing_or_invalid_authority);
RUN_TEST(api_isolate_dofile_null_isolate);
RUN_TEST(api_isolate_dofile_null_filename);

RUN_TEST_SUITE("API Lifecycle - Valid Operations");
RUN_TEST(api_isolate_create_destroy);

TEST_MAIN_END()
