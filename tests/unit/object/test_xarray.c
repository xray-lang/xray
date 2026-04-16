/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xarray.c - Unit tests for Array object
 */

#include "../test_framework.h"
#include "../test_helper.h"
#include "runtime/object/xarray.h"

// Helper macros to match xvalue.h API
#define xr_is_null(v)   XR_IS_NULL(v)
#define xr_is_int(v)    XR_IS_INT(v)
#define xr_is_float(v)  XR_IS_FLOAT(v)
#define xr_is_bool(v)   XR_IS_BOOL(v)
#define xr_as_int(v)    XR_TO_INT(v)

static XrayIsolate *X = NULL;
static XrCoroutine *main_coro = NULL;

/* ========== Setup / Teardown ========== */

static void setup(void) {
    X = xray_isolate_new(NULL);
    ASSERT_NOT_NULL(X);
    // Initialize test coroutine
    main_coro = xr_test_init_coro(X);
    ASSERT_NOT_NULL(main_coro);
}

static void teardown(void) {
    if (X) {
        xray_isolate_delete(X);
        X = NULL;
        main_coro = NULL;
    }
}

/* ========== Creation Tests ========== */

TEST(array_new_empty) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ_INT(xr_array_size(arr), 0);
    ASSERT_TRUE(xr_array_is_empty(arr));
    teardown();
}

TEST(array_with_capacity) {
    setup();
    XrArray *arr = xr_array_with_capacity(main_coro, 100);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ_INT(xr_array_size(arr), 0);
    ASSERT_GE(arr->capacity, 100);
    teardown();
}

TEST(array_from_values) {
    setup();
    XrValue values[] = {xr_int(1), xr_int(2), xr_int(3)};
    XrArray *arr = xr_array_from_values(main_coro, values, 3);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ_INT(xr_array_size(arr), 3);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 0)), 1);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 1)), 2);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 2)), 3);
    teardown();
}

/* ========== Push / Pop Tests ========== */

TEST(array_push_single) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_push(arr, xr_int(42));
    ASSERT_EQ_INT(xr_array_size(arr), 1);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 0)), 42);
    teardown();
}

TEST(array_push_multiple) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    for (int i = 0; i < 100; i++) {
        xr_array_push(arr, xr_int(i));
    }
    ASSERT_EQ_INT(xr_array_size(arr), 100);
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, i)), i);
    }
    teardown();
}

TEST(array_pop_single) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_push(arr, xr_int(42));
    XrValue val = xr_array_pop(arr);
    ASSERT_EQ_INT(xr_as_int(val), 42);
    ASSERT_EQ_INT(xr_array_size(arr), 0);
    teardown();
}

TEST(array_pop_empty) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    XrValue val = xr_array_pop(arr);
    ASSERT_TRUE(xr_is_null(val));
    teardown();
}

TEST(array_push_pop_sequence) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    // Push 1, 2, 3
    xr_array_push(arr, xr_int(1));
    xr_array_push(arr, xr_int(2));
    xr_array_push(arr, xr_int(3));
    // Pop should return 3, 2, 1
    ASSERT_EQ_INT(xr_as_int(xr_array_pop(arr)), 3);
    ASSERT_EQ_INT(xr_as_int(xr_array_pop(arr)), 2);
    ASSERT_EQ_INT(xr_as_int(xr_array_pop(arr)), 1);
    ASSERT_TRUE(xr_array_is_empty(arr));
    teardown();
}

/* ========== Unshift / Shift Tests ========== */

TEST(array_unshift_single) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_unshift(arr, xr_int(42));
    ASSERT_EQ_INT(xr_array_size(arr), 1);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 0)), 42);
    teardown();
}

TEST(array_unshift_multiple) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    // Unshift 1, 2, 3 -> array should be [3, 2, 1]
    xr_array_unshift(arr, xr_int(1));
    xr_array_unshift(arr, xr_int(2));
    xr_array_unshift(arr, xr_int(3));
    ASSERT_EQ_INT(xr_array_size(arr), 3);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 0)), 3);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 1)), 2);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 2)), 1);
    teardown();
}

TEST(array_shift_single) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_push(arr, xr_int(1));
    xr_array_push(arr, xr_int(2));
    XrValue val = xr_array_shift(arr);
    ASSERT_EQ_INT(xr_as_int(val), 1);
    ASSERT_EQ_INT(xr_array_size(arr), 1);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 0)), 2);
    teardown();
}

TEST(array_shift_empty) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    XrValue val = xr_array_shift(arr);
    ASSERT_TRUE(xr_is_null(val));
    teardown();
}

/* ========== Get / Set Tests ========== */

TEST(array_get_valid_index) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_push(arr, xr_int(10));
    xr_array_push(arr, xr_int(20));
    xr_array_push(arr, xr_int(30));
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 0)), 10);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 1)), 20);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 2)), 30);
    teardown();
}

TEST(array_get_invalid_index) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_push(arr, xr_int(10));
    // Out of bounds should return null
    XrValue val = xr_array_get(arr, 100);
    ASSERT_TRUE(xr_is_null(val));
    // Negative index
    val = xr_array_get(arr, -1);
    ASSERT_TRUE(xr_is_null(val));
    teardown();
}

TEST(array_set_valid_index) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_push(arr, xr_int(10));
    xr_array_push(arr, xr_int(20));
    xr_array_set(arr, 0, xr_int(100));
    xr_array_set(arr, 1, xr_int(200));
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 0)), 100);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 1)), 200);
    teardown();
}

/* ========== Query Methods Tests ========== */

TEST(array_index_of_found) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_push(arr, xr_int(10));
    xr_array_push(arr, xr_int(20));
    xr_array_push(arr, xr_int(30));
    ASSERT_EQ_INT(xr_array_index_of(arr, xr_int(10)), 0);
    ASSERT_EQ_INT(xr_array_index_of(arr, xr_int(20)), 1);
    ASSERT_EQ_INT(xr_array_index_of(arr, xr_int(30)), 2);
    teardown();
}

TEST(array_index_of_not_found) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_push(arr, xr_int(10));
    xr_array_push(arr, xr_int(20));
    ASSERT_EQ_INT(xr_array_index_of(arr, xr_int(99)), -1);
    teardown();
}

TEST(array_has) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_push(arr, xr_int(10));
    xr_array_push(arr, xr_int(20));
    ASSERT_TRUE(xr_array_has(arr, xr_int(10)));
    ASSERT_TRUE(xr_array_has(arr, xr_int(20)));
    ASSERT_FALSE(xr_array_has(arr, xr_int(30)));
    teardown();
}

/* ========== Clear Tests ========== */

TEST(array_clear) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_push(arr, xr_int(1));
    xr_array_push(arr, xr_int(2));
    xr_array_push(arr, xr_int(3));
    ASSERT_EQ_INT(xr_array_size(arr), 3);
    xr_array_clear(arr);
    ASSERT_EQ_INT(xr_array_size(arr), 0);
    ASSERT_TRUE(xr_array_is_empty(arr));
    teardown();
}

/* ========== Reverse Tests ========== */

TEST(array_reverse_even) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_push(arr, xr_int(1));
    xr_array_push(arr, xr_int(2));
    xr_array_push(arr, xr_int(3));
    xr_array_push(arr, xr_int(4));
    xr_array_reverse(arr);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 0)), 4);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 1)), 3);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 2)), 2);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 3)), 1);
    teardown();
}

TEST(array_reverse_odd) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_push(arr, xr_int(1));
    xr_array_push(arr, xr_int(2));
    xr_array_push(arr, xr_int(3));
    xr_array_reverse(arr);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 0)), 3);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 1)), 2);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 2)), 1);
    teardown();
}

TEST(array_reverse_empty) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_reverse(arr);  // Should not crash
    ASSERT_TRUE(xr_array_is_empty(arr));
    teardown();
}

TEST(array_reverse_single) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_push(arr, xr_int(42));
    xr_array_reverse(arr);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 0)), 42);
    teardown();
}

/* ========== Copy Tests ========== */

TEST(array_copy) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_push(arr, xr_int(1));
    xr_array_push(arr, xr_int(2));
    xr_array_push(arr, xr_int(3));
    
    XrArray *copy = xr_array_copy(main_coro, arr);
    ASSERT_NOT_NULL(copy);
    ASSERT_NE(copy, arr);  // Different object
    ASSERT_EQ_INT(xr_array_size(copy), 3);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(copy, 0)), 1);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(copy, 1)), 2);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(copy, 2)), 3);
    
    // Modify original, copy should not change
    xr_array_set(arr, 0, xr_int(100));
    ASSERT_EQ_INT(xr_as_int(xr_array_get(copy, 0)), 1);  // Unchanged
    teardown();
}

/* ========== Slice Tests ========== */

TEST(array_slice_basic) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    for (int i = 0; i < 5; i++) {
        xr_array_push(arr, xr_int(i));
    }
    
    // Slice [1:4] -> [1, 2, 3]
    XrArray *slice = xr_array_slice(main_coro, arr, 1, 4);
    ASSERT_NOT_NULL(slice);
    ASSERT_EQ_INT(xr_array_size(slice), 3);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(slice, 0)), 1);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(slice, 1)), 2);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(slice, 2)), 3);
    ASSERT_TRUE(xr_array_is_slice(slice));
    teardown();
}

TEST(array_slice_full) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    for (int i = 0; i < 5; i++) {
        xr_array_push(arr, xr_int(i));
    }
    
    // Full slice [0:5]
    XrArray *slice = xr_array_slice(main_coro, arr, 0, 5);
    ASSERT_EQ_INT(xr_array_size(slice), 5);
    teardown();
}

TEST(array_slice_empty) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    for (int i = 0; i < 5; i++) {
        xr_array_push(arr, xr_int(i));
    }
    
    // Empty slice [2:2]
    XrArray *slice = xr_array_slice(main_coro, arr, 2, 2);
    ASSERT_EQ_INT(xr_array_size(slice), 0);
    teardown();
}

/* ========== Mixed Types Tests ========== */

TEST(array_mixed_types) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_push(arr, xr_int(42));
    xr_array_push(arr, xr_float(3.14));
    xr_array_push(arr, xr_bool(true));
    xr_array_push(arr, xr_null());
    
    ASSERT_EQ_INT(xr_array_size(arr), 4);
    ASSERT_TRUE(xr_is_int(xr_array_get(arr, 0)));
    ASSERT_TRUE(xr_is_float(xr_array_get(arr, 1)));
    ASSERT_TRUE(xr_is_bool(xr_array_get(arr, 2)));
    ASSERT_TRUE(xr_is_null(xr_array_get(arr, 3)));
    teardown();
}

/* ========== Stress Tests ========== */

TEST(array_large_push) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    const int N = 10000;
    for (int i = 0; i < N; i++) {
        xr_array_push(arr, xr_int(i));
    }
    ASSERT_EQ_INT(xr_array_size(arr), N);
    // Verify first, middle, last
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, 0)), 0);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, N/2)), N/2);
    ASSERT_EQ_INT(xr_as_int(xr_array_get(arr, N-1)), N-1);
    teardown();
}

/* ========== Main ========== */

static void run_all_tests(void) {
    RUN_TEST_SUITE("Array Creation");
    RUN_TEST(array_new_empty);
    RUN_TEST(array_with_capacity);
    RUN_TEST(array_from_values);
    
    RUN_TEST_SUITE("Array Push/Pop");
    RUN_TEST(array_push_single);
    RUN_TEST(array_push_multiple);
    RUN_TEST(array_pop_single);
    RUN_TEST(array_pop_empty);
    RUN_TEST(array_push_pop_sequence);
    
    RUN_TEST_SUITE("Array Unshift/Shift");
    RUN_TEST(array_unshift_single);
    RUN_TEST(array_unshift_multiple);
    RUN_TEST(array_shift_single);
    RUN_TEST(array_shift_empty);
    
    RUN_TEST_SUITE("Array Get/Set");
    RUN_TEST(array_get_valid_index);
    RUN_TEST(array_get_invalid_index);
    RUN_TEST(array_set_valid_index);
    
    RUN_TEST_SUITE("Array Query");
    RUN_TEST(array_index_of_found);
    RUN_TEST(array_index_of_not_found);
    RUN_TEST(array_has);
    
    RUN_TEST_SUITE("Array Clear");
    RUN_TEST(array_clear);
    
    RUN_TEST_SUITE("Array Reverse");
    RUN_TEST(array_reverse_even);
    RUN_TEST(array_reverse_odd);
    RUN_TEST(array_reverse_empty);
    RUN_TEST(array_reverse_single);
    
    RUN_TEST_SUITE("Array Copy");
    RUN_TEST(array_copy);
    
    RUN_TEST_SUITE("Array Slice");
    RUN_TEST(array_slice_basic);
    RUN_TEST(array_slice_full);
    RUN_TEST(array_slice_empty);
    
    RUN_TEST_SUITE("Array Mixed Types");
    RUN_TEST(array_mixed_types);
    
    RUN_TEST_SUITE("Array Stress");
    RUN_TEST(array_large_push);
}

TEST_MAIN_BEGIN()
    printf("=== xray Array Unit Tests ===\n");
    run_all_tests();
TEST_MAIN_END()
