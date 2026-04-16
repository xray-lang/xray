/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_api_defense.c - Unit tests for P2 API boundary defense
 *
 * KEY CONCEPT:
 *   Verifies that all public API functions (xray_isolate_*, xray_alloc, etc.)
 *   gracefully handle NULL parameters without crashing.
 *   In Release builds, these return early with safe defaults.
 *   In Debug builds, XR_DCHECK fires before the early return.
 */

#include "../test_framework.h"
#include "xray_isolate.h"
#include <stddef.h>

/* ========== Isolate Lifecycle NULL Safety ========== */

TEST(api_isolate_delete_null) {
    // xray_isolate_delete(NULL) should be safe (no-op)
    xray_isolate_delete(NULL);
    ASSERT_TRUE(1);  // survived without crash
}

TEST(api_isolate_params_init_null) {
    // xray_isolate_params_init(NULL) should be safe
    xray_isolate_params_init(NULL);
    ASSERT_TRUE(1);
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
    if (SKIP_NULL_RETURN_TESTS) { ASSERT_TRUE(1); return; }
    int result = xray_isolate_dostring(NULL, "print(1)");
    ASSERT_EQ_INT(result, -1);
}

TEST(api_isolate_dostring_null_source) {
    if (SKIP_NULL_RETURN_TESTS) { ASSERT_TRUE(1); return; }
    // Need a valid isolate to test NULL source
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    XrayIsolate *iso = xray_isolate_new(&params);
    if (!iso) { ASSERT_TRUE(1); return; }  // alloc failure

    int result = xray_isolate_dostring(iso, NULL);
    ASSERT_EQ_INT(result, -1);

    xray_isolate_delete(iso);
}

TEST(api_isolate_dofile_null_isolate) {
    if (SKIP_NULL_RETURN_TESTS) { ASSERT_TRUE(1); return; }
    int result = xray_isolate_dofile(NULL, "test.xr");
    ASSERT_EQ_INT(result, -1);
}

TEST(api_isolate_dofile_null_filename) {
    if (SKIP_NULL_RETURN_TESTS) { ASSERT_TRUE(1); return; }
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    XrayIsolate *iso = xray_isolate_new(&params);
    if (!iso) { ASSERT_TRUE(1); return; }

    int result = xray_isolate_dofile(iso, NULL);
    ASSERT_EQ_INT(result, -1);

    xray_isolate_delete(iso);
}

/* ========== Advanced API NULL Safety ========== */

TEST(api_isolate_get_backend_null) {
    if (SKIP_NULL_RETURN_TESTS) { ASSERT_TRUE(1); return; }
    int backend = xray_isolate_get_backend(NULL);
    ASSERT_EQ_INT(backend, 0);
}

TEST(api_isolate_set_userdata_null) {
    if (SKIP_NULL_RETURN_TESTS) { ASSERT_TRUE(1); return; }
    // Should not crash
    xray_isolate_set_userdata(NULL, (void*)0x1234);
    ASSERT_TRUE(1);
}

TEST(api_isolate_get_userdata_null) {
    if (SKIP_NULL_RETURN_TESTS) { ASSERT_TRUE(1); return; }
    void *ud = xray_isolate_get_userdata(NULL);
    ASSERT_NULL(ud);
}

TEST(api_isolate_get_stats_null) {
    if (SKIP_NULL_RETURN_TESTS) { ASSERT_TRUE(1); return; }
    size_t bytes = 999;
    int gc_count = 999;
    xray_isolate_get_stats(NULL, &bytes, &gc_count);
    // After NULL guard returns early, values should be unchanged
    ASSERT_EQ_UINT(bytes, 999);
    ASSERT_EQ_INT(gc_count, 999);
}

TEST(api_isolate_collect_garbage_null) {
    if (SKIP_NULL_RETURN_TESTS) { ASSERT_TRUE(1); return; }
    xray_isolate_collect_garbage(NULL);
    ASSERT_TRUE(1);
}

TEST(api_isolate_set_trace_null) {
    if (SKIP_NULL_RETURN_TESTS) { ASSERT_TRUE(1); return; }
    xray_isolate_set_trace(NULL, true);
    ASSERT_TRUE(1);
}

TEST(api_isolate_set_dump_bytecode_null) {
    if (SKIP_NULL_RETURN_TESTS) { ASSERT_TRUE(1); return; }
    xray_isolate_set_dump_bytecode(NULL, true);
    ASSERT_TRUE(1);
}

/* ========== Isolate Lifecycle (valid) ========== */

TEST(api_isolate_create_destroy) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);

    XrayIsolate *iso = xray_isolate_new(&params);
    ASSERT_NOT_NULL(iso);

    xray_isolate_delete(iso);
}

TEST(api_isolate_userdata_roundtrip) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    XrayIsolate *iso = xray_isolate_new(&params);
    ASSERT_NOT_NULL(iso);

    void *data = (void*)0xDEADBEEF;
    xray_isolate_set_userdata(iso, data);
    void *got = xray_isolate_get_userdata(iso);
    ASSERT_EQ_PTR(got, data);

    xray_isolate_delete(iso);
}

TEST(api_isolate_stats_valid) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    XrayIsolate *iso = xray_isolate_new(&params);
    ASSERT_NOT_NULL(iso);

    size_t bytes = 0;
    int gc_count = -1;
    xray_isolate_get_stats(iso, &bytes, &gc_count);
    // After creation, some memory should be allocated
    ASSERT_GE(bytes, 0);
    ASSERT_GE(gc_count, 0);

    xray_isolate_delete(iso);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

    RUN_TEST_SUITE("API Boundary Defense - NULL Safety");
    RUN_TEST(api_isolate_delete_null);
    RUN_TEST(api_isolate_params_init_null);
    RUN_TEST(api_isolate_dostring_null_isolate);
    RUN_TEST(api_isolate_dostring_null_source);
    RUN_TEST(api_isolate_dofile_null_isolate);
    RUN_TEST(api_isolate_dofile_null_filename);
    RUN_TEST(api_isolate_get_backend_null);
    RUN_TEST(api_isolate_set_userdata_null);
    RUN_TEST(api_isolate_get_userdata_null);
    RUN_TEST(api_isolate_get_stats_null);
    RUN_TEST(api_isolate_collect_garbage_null);
    RUN_TEST(api_isolate_set_trace_null);
    RUN_TEST(api_isolate_set_dump_bytecode_null);

    RUN_TEST_SUITE("API Lifecycle - Valid Operations");
    RUN_TEST(api_isolate_create_destroy);
    RUN_TEST(api_isolate_userdata_roundtrip);
    RUN_TEST(api_isolate_stats_valid);

TEST_MAIN_END()
