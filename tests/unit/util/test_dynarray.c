/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_dynarray.c - Unit tests for dynamic array
 */

#include "../test_framework.h"
#include "base/xdynarray.h"

/* ========== Basic Operations ========== */

TEST(dynarray_init) {
    XrDynArray arr;
    DYNARRAY_INIT(&arr, int);
    
    ASSERT_EQ_INT(DYNARRAY_COUNT(&arr), 0);
    ASSERT_EQ_INT(DYNARRAY_CAPACITY(&arr), 0);
    ASSERT_EQ_INT((int)arr.elem_size, (int)sizeof(int));
    
    DYNARRAY_FREE(&arr);
}

TEST(dynarray_add_single) {
    XrDynArray arr;
    DYNARRAY_INIT(&arr, int);
    
    int idx = DYNARRAY_ADD(&arr, 42, int);
    
    ASSERT_EQ_INT(idx, 0);
    ASSERT_EQ_INT(DYNARRAY_COUNT(&arr), 1);
    ASSERT_TRUE(DYNARRAY_CAPACITY(&arr) >= 1);
    ASSERT_EQ_INT(DYNARRAY_GET(&arr, 0, int), 42);
    
    DYNARRAY_FREE(&arr);
}

TEST(dynarray_add_multiple) {
    XrDynArray arr;
    DYNARRAY_INIT(&arr, int);
    
    for (int i = 0; i < 10; i++) {
        DYNARRAY_ADD(&arr, i * 10, int);
    }
    
    ASSERT_EQ_INT(DYNARRAY_COUNT(&arr), 10);
    
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ_INT(DYNARRAY_GET(&arr, i, int), i * 10);
    }
    
    DYNARRAY_FREE(&arr);
}

TEST(dynarray_get_set) {
    XrDynArray arr;
    DYNARRAY_INIT(&arr, int);
    
    DYNARRAY_ADD(&arr, 100, int);
    DYNARRAY_ADD(&arr, 200, int);
    DYNARRAY_ADD(&arr, 300, int);
    
    ASSERT_EQ_INT(DYNARRAY_GET(&arr, 0, int), 100);
    ASSERT_EQ_INT(DYNARRAY_GET(&arr, 1, int), 200);
    ASSERT_EQ_INT(DYNARRAY_GET(&arr, 2, int), 300);
    
    DYNARRAY_SET(&arr, 1, 999, int);
    ASSERT_EQ_INT(DYNARRAY_GET(&arr, 1, int), 999);
    
    DYNARRAY_FREE(&arr);
}

TEST(dynarray_clear) {
    XrDynArray arr;
    DYNARRAY_INIT(&arr, int);
    
    for (int i = 0; i < 5; i++) {
        DYNARRAY_ADD(&arr, i, int);
    }
    
    ASSERT_EQ_INT(DYNARRAY_COUNT(&arr), 5);
    
    DYNARRAY_CLEAR(&arr);
    
    ASSERT_EQ_INT(DYNARRAY_COUNT(&arr), 0);
    // Capacity should remain unchanged after clear
    ASSERT_TRUE(DYNARRAY_CAPACITY(&arr) >= 5);
    
    DYNARRAY_FREE(&arr);
}

/* ========== Growth Tests ========== */

TEST(dynarray_growth) {
    XrDynArray arr;
    DYNARRAY_INIT(&arr, int);
    
    // Add many elements to trigger multiple growth cycles
    for (int i = 0; i < 1000; i++) {
        DYNARRAY_ADD(&arr, i, int);
    }
    
    ASSERT_EQ_INT(DYNARRAY_COUNT(&arr), 1000);
    
    // Verify all values
    for (int i = 0; i < 1000; i++) {
        ASSERT_EQ_INT(DYNARRAY_GET(&arr, i, int), i);
    }
    
    DYNARRAY_FREE(&arr);
}

TEST(dynarray_reserve) {
    XrDynArray arr;
    DYNARRAY_INIT(&arr, int);
    
    bool ok = xr_dynarray_reserve(&arr, 100);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(DYNARRAY_CAPACITY(&arr) >= 100);
    ASSERT_EQ_INT(DYNARRAY_COUNT(&arr), 0);  // Count unchanged
    
    // Add elements without triggering growth
    for (int i = 0; i < 100; i++) {
        DYNARRAY_ADD(&arr, i, int);
    }
    
    ASSERT_EQ_INT(DYNARRAY_COUNT(&arr), 100);
    
    DYNARRAY_FREE(&arr);
}

/* ========== Different Types ========== */

TEST(dynarray_double_type) {
    XrDynArray arr;
    DYNARRAY_INIT(&arr, double);
    
    DYNARRAY_ADD(&arr, 3.14159, double);
    DYNARRAY_ADD(&arr, 2.71828, double);
    DYNARRAY_ADD(&arr, 1.41421, double);
    
    ASSERT_EQ_INT(DYNARRAY_COUNT(&arr), 3);
    ASSERT_FLOAT_EQ(DYNARRAY_GET(&arr, 0, double), 3.14159, 1e-10);
    ASSERT_FLOAT_EQ(DYNARRAY_GET(&arr, 1, double), 2.71828, 1e-10);
    ASSERT_FLOAT_EQ(DYNARRAY_GET(&arr, 2, double), 1.41421, 1e-10);
    
    DYNARRAY_FREE(&arr);
}

TEST(dynarray_pointer_type) {
    XrDynArray arr;
    DYNARRAY_INIT(&arr, void*);
    
    int a = 1, b = 2, c = 3;
    DYNARRAY_ADD(&arr, &a, void*);
    DYNARRAY_ADD(&arr, &b, void*);
    DYNARRAY_ADD(&arr, &c, void*);
    
    ASSERT_EQ_INT(DYNARRAY_COUNT(&arr), 3);
    ASSERT_TRUE(DYNARRAY_GET(&arr, 0, void*) == &a);
    ASSERT_TRUE(DYNARRAY_GET(&arr, 1, void*) == &b);
    ASSERT_TRUE(DYNARRAY_GET(&arr, 2, void*) == &c);
    
    DYNARRAY_FREE(&arr);
}

typedef struct {
    int x;
    int y;
    char name[16];
} TestStruct;

TEST(dynarray_struct_type) {
    XrDynArray arr;
    DYNARRAY_INIT(&arr, TestStruct);
    
    TestStruct s1 = {10, 20, "point1"};
    TestStruct s2 = {30, 40, "point2"};
    
    DYNARRAY_ADD(&arr, s1, TestStruct);
    DYNARRAY_ADD(&arr, s2, TestStruct);
    
    ASSERT_EQ_INT(DYNARRAY_COUNT(&arr), 2);
    
    TestStruct *p1 = DYNARRAY_GET_PTR(&arr, 0, TestStruct);
    TestStruct *p2 = DYNARRAY_GET_PTR(&arr, 1, TestStruct);
    
    ASSERT_EQ_INT(p1->x, 10);
    ASSERT_EQ_INT(p1->y, 20);
    ASSERT_STR_EQ(p1->name, "point1");
    
    ASSERT_EQ_INT(p2->x, 30);
    ASSERT_EQ_INT(p2->y, 40);
    ASSERT_STR_EQ(p2->name, "point2");
    
    DYNARRAY_FREE(&arr);
}

/* ========== Edge Cases ========== */

TEST(dynarray_empty) {
    XrDynArray arr;
    DYNARRAY_INIT(&arr, int);
    
    ASSERT_EQ_INT(DYNARRAY_COUNT(&arr), 0);
    
    // Clear empty array should be safe
    DYNARRAY_CLEAR(&arr);
    ASSERT_EQ_INT(DYNARRAY_COUNT(&arr), 0);
    
    DYNARRAY_FREE(&arr);
}

TEST(dynarray_reuse_after_clear) {
    XrDynArray arr;
    DYNARRAY_INIT(&arr, int);
    
    // First usage
    for (int i = 0; i < 10; i++) {
        DYNARRAY_ADD(&arr, i, int);
    }
    ASSERT_EQ_INT(DYNARRAY_COUNT(&arr), 10);
    
    // Clear and reuse
    DYNARRAY_CLEAR(&arr);
    
    for (int i = 0; i < 5; i++) {
        DYNARRAY_ADD(&arr, i * 100, int);
    }
    
    ASSERT_EQ_INT(DYNARRAY_COUNT(&arr), 5);
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ_INT(DYNARRAY_GET(&arr, i, int), i * 100);
    }
    
    DYNARRAY_FREE(&arr);
}

TEST(dynarray_foreach) {
    XrDynArray arr;
    DYNARRAY_INIT(&arr, int);
    
    for (int i = 0; i < 5; i++) {
        DYNARRAY_ADD(&arr, i + 1, int);
    }
    
    int sum = 0;
    DYNARRAY_FOREACH(&arr, i) {
        sum += DYNARRAY_GET(&arr, i, int);
    }
    
    ASSERT_EQ_INT(sum, 15);  // 1+2+3+4+5 = 15
    
    DYNARRAY_FREE(&arr);
}

/* ========== Stress Tests ========== */

TEST(dynarray_stress) {
    XrDynArray arr;
    DYNARRAY_INIT(&arr, int);
    
    // Add 10000 elements
    for (int i = 0; i < 10000; i++) {
        DYNARRAY_ADD(&arr, i, int);
    }
    
    ASSERT_EQ_INT(DYNARRAY_COUNT(&arr), 10000);
    
    // Verify random samples
    ASSERT_EQ_INT(DYNARRAY_GET(&arr, 0, int), 0);
    ASSERT_EQ_INT(DYNARRAY_GET(&arr, 1000, int), 1000);
    ASSERT_EQ_INT(DYNARRAY_GET(&arr, 5000, int), 5000);
    ASSERT_EQ_INT(DYNARRAY_GET(&arr, 9999, int), 9999);
    
    DYNARRAY_FREE(&arr);
}

/* ========== Main ========== */

static void run_all_tests(void) {
    RUN_TEST_SUITE("Basic Operations");
    RUN_TEST(dynarray_init);
    RUN_TEST(dynarray_add_single);
    RUN_TEST(dynarray_add_multiple);
    RUN_TEST(dynarray_get_set);
    RUN_TEST(dynarray_clear);
    
    RUN_TEST_SUITE("Growth");
    RUN_TEST(dynarray_growth);
    RUN_TEST(dynarray_reserve);
    
    RUN_TEST_SUITE("Different Types");
    RUN_TEST(dynarray_double_type);
    RUN_TEST(dynarray_pointer_type);
    RUN_TEST(dynarray_struct_type);
    
    RUN_TEST_SUITE("Edge Cases");
    RUN_TEST(dynarray_empty);
    RUN_TEST(dynarray_reuse_after_clear);
    RUN_TEST(dynarray_foreach);
    
    RUN_TEST_SUITE("Stress Tests");
    RUN_TEST(dynarray_stress);
}

TEST_MAIN_BEGIN()
    printf("=== xray Dynamic Array Unit Tests ===\n");
    run_all_tests();
TEST_MAIN_END()
