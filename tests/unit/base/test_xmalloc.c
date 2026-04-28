/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xmalloc.c - Unit tests for memory allocator and debug tracking
 *
 * KEY CONCEPT:
 *   Tests xr_malloc/xr_free basic functionality, convenience macros
 *   (XR_GROW_ARRAY, xr_strdup, etc.), and debug-mode alloc/free
 *   symmetry tracking (XrMemStats).
 */

#include "../test_framework.h"
#include "base/xchecks.h"
#include "base/xmalloc.h"

/* ========== Basic Allocation Tests ========== */

TEST(malloc_basic) {
    void *p = xr_malloc(128);
    ASSERT_NOT_NULL(p);
    memset(p, 0xAB, 128);
    xr_free(p);
}

TEST(calloc_zeroed) {
    int *arr = (int *)xr_calloc(10, sizeof(int));
    ASSERT_NOT_NULL(arr);
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ_INT(arr[i], 0);
    }
    xr_free(arr);
}

TEST(realloc_grow) {
    int *arr = (int *)xr_malloc(4 * sizeof(int));
    ASSERT_NOT_NULL(arr);
    arr[0] = 10; arr[1] = 20; arr[2] = 30; arr[3] = 40;

    arr = (int *)xr_realloc(arr, 8 * sizeof(int));
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ_INT(arr[0], 10);
    ASSERT_EQ_INT(arr[3], 40);
    xr_free(arr);
}

TEST(free_null_safe) {
    // xr_free(NULL) should not crash
    xr_free(NULL);
}

/* ========== Convenience Macro Tests ========== */

TEST(strdup_basic) {
    char *s = xr_strdup("hello world");
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "hello world");
    xr_free(s);
}

TEST(strdup_null) {
    char *s = xr_strdup(NULL);
    ASSERT_TRUE(s == NULL);
}

TEST(strdup_empty) {
    char *s = xr_strdup("");
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "");
    xr_free(s);
}

TEST(grow_array_macro) {
    int *arr = XR_GROW_ARRAY(int, NULL, 0, 4);
    ASSERT_NOT_NULL(arr);
    arr[0] = 1; arr[1] = 2; arr[2] = 3; arr[3] = 4;

    arr = XR_GROW_ARRAY(int, arr, 4, 8);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ_INT(arr[0], 1);
    ASSERT_EQ_INT(arr[3], 4);
    XR_FREE_ARRAY(int, arr);
}

TEST(allocate_macro) {
    typedef struct { int x; int y; } Point;
    Point *p = XR_ALLOCATE(Point);
    ASSERT_NOT_NULL(p);
    p->x = 10;
    p->y = 20;
    ASSERT_EQ_INT(p->x, 10);
    xr_free(p);
}

TEST(grow_capacity_macro) {
    ASSERT_EQ_INT(XR_GROW_CAPACITY(0), 8);
    ASSERT_EQ_INT(XR_GROW_CAPACITY(1), 8);
    ASSERT_EQ_INT(XR_GROW_CAPACITY(7), 8);
    ASSERT_EQ_INT(XR_GROW_CAPACITY(8), 16);
    ASSERT_EQ_INT(XR_GROW_CAPACITY(16), 32);
}

TEST(xr_realloc_macro_grow_ok) {
    int *arr = (int *)xr_malloc(4 * sizeof(int));
    ASSERT_NOT_NULL(arr);
    for (int i = 0; i < 4; i++) arr[i] = i + 1;

    bool ok = XR_REALLOC(arr, 16 * sizeof(int));
    ASSERT_TRUE(ok);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ_INT(arr[0], 1);
    ASSERT_EQ_INT(arr[3], 4);
    for (int i = 4; i < 16; i++) arr[i] = i * 10;
    ASSERT_EQ_INT(arr[15], 150);

    xr_free(arr);
}

TEST(xr_realloc_macro_from_null) {
    int *arr = NULL;
    bool ok = XR_REALLOC(arr, 8 * sizeof(int));
    ASSERT_TRUE(ok);
    ASSERT_NOT_NULL(arr);
    arr[0] = 42;
    ASSERT_EQ_INT(arr[0], 42);
    xr_free(arr);
}

TEST(xr_realloc_macro_struct_field) {
    typedef struct { int *data; size_t cap; } Vec;
    Vec v = {NULL, 0};

    bool ok = XR_REALLOC(v.data, 8 * sizeof(int));
    ASSERT_TRUE(ok);
    ASSERT_NOT_NULL(v.data);
    v.data[7] = 99;
    ASSERT_EQ_INT(v.data[7], 99);

    ok = XR_REALLOC(v.data, 32 * sizeof(int));
    ASSERT_TRUE(ok);
    ASSERT_EQ_INT(v.data[7], 99);

    xr_free(v.data);
}

TEST(xr_realloc_macro_zero_size_is_free) {
    int *arr = (int *)xr_malloc(4 * sizeof(int));
    ASSERT_NOT_NULL(arr);

    bool ok = XR_REALLOC(arr, 0);
    ASSERT_TRUE(ok);
    // arr is now whatever realloc returned for size 0; typically NULL.
    // Assigning NULL via xr_free below is safe.
    xr_free(arr);
}

/* ========== Debug Tracking Tests ========== */

#if XR_DEBUG
TEST(tracking_balance) {
    // Record baseline
    int64_t baseline = xr_mem_check_balance();

    void *p1 = xr_malloc(64);
    void *p2 = xr_malloc(128);
    ASSERT_EQ_INT(xr_mem_check_balance(), baseline + 2);

    xr_free(p1);
    ASSERT_EQ_INT(xr_mem_check_balance(), baseline + 1);

    xr_free(p2);
    ASSERT_EQ_INT(xr_mem_check_balance(), baseline);
}

TEST(tracking_calloc) {
    int64_t baseline = xr_mem_check_balance();

    void *p = xr_calloc(10, sizeof(int));
    ASSERT_EQ_INT(xr_mem_check_balance(), baseline + 1);

    xr_free(p);
    ASSERT_EQ_INT(xr_mem_check_balance(), baseline);
}

TEST(tracking_free_null_no_decrement) {
    int64_t baseline = xr_mem_check_balance();
    xr_free(NULL);
    // NULL free should not change balance
    ASSERT_EQ_INT(xr_mem_check_balance(), baseline);
}
#endif

/* ========== Allocator Name ========== */

TEST(allocator_name) {
    const char *name = xr_mem_get_allocator_name();
    ASSERT_NOT_NULL(name);
    ASSERT_TRUE(strlen(name) > 0);
}

/* ========== Main ========== */

int main(void) {
    xr_test_suppress_dialogs();
    RUN_TEST_SUITE("xmalloc Tests");

    RUN_TEST(malloc_basic);
    RUN_TEST(calloc_zeroed);
    RUN_TEST(realloc_grow);
    RUN_TEST(free_null_safe);
    RUN_TEST(strdup_basic);
    RUN_TEST(strdup_null);
    RUN_TEST(strdup_empty);
    RUN_TEST(grow_array_macro);
    RUN_TEST(allocate_macro);
    RUN_TEST(grow_capacity_macro);
    RUN_TEST(xr_realloc_macro_grow_ok);
    RUN_TEST(xr_realloc_macro_from_null);
    RUN_TEST(xr_realloc_macro_struct_field);
    RUN_TEST(xr_realloc_macro_zero_size_is_free);

#if XR_DEBUG
    RUN_TEST_SUITE("Debug Tracking");
    RUN_TEST(tracking_balance);
    RUN_TEST(tracking_calloc);
    RUN_TEST(tracking_free_null_no_decrement);
#endif

    RUN_TEST(allocator_name);

    TEST_REPORT();
    return TEST_EXIT();
}
