/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xset.c - Unit tests for Set object
 *
 * KEY CONCEPT:
 *   Tests Set creation, add/has/delete operations,
 *   set algebra (union, intersection, difference),
 *   and subset/superset checks.
 */

#include "../test_framework.h"
#include "../test_helper.h"
#include "runtime/object/xset.h"
#include "runtime/object/xarray.h"

static XrayIsolate *X = NULL;
static XrCoroutine *main_coro = NULL;

/* ========== Setup / Teardown ========== */

static void setup(void) {
    X = xray_isolate_new(NULL);
    ASSERT_NOT_NULL(X);
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

TEST(set_new_empty) {
    setup();
    XrSet *set = xr_set_new(main_coro);
    ASSERT_NOT_NULL(set);
    ASSERT_EQ_INT(xr_set_size(set), 0);
    ASSERT_TRUE(xr_set_is_empty(set));
    teardown();
}

TEST(set_with_capacity) {
    setup();
    XrSet *set = xr_set_new_with_capacity(main_coro, 64);
    ASSERT_NOT_NULL(set);
    ASSERT_EQ_INT(xr_set_size(set), 0);
    teardown();
}

/* ========== Add/Has/Delete Tests ========== */

TEST(set_add_has) {
    setup();
    XrSet *set = xr_set_new(main_coro);
    ASSERT_TRUE(xr_set_add(set, xr_int(1)));
    ASSERT_TRUE(xr_set_add(set, xr_int(2)));
    ASSERT_TRUE(xr_set_add(set, xr_int(3)));

    ASSERT_TRUE(xr_set_has(set, xr_int(1)));
    ASSERT_TRUE(xr_set_has(set, xr_int(2)));
    ASSERT_TRUE(xr_set_has(set, xr_int(3)));
    ASSERT_FALSE(xr_set_has(set, xr_int(99)));
    ASSERT_EQ_INT(xr_set_size(set), 3);
    teardown();
}

TEST(set_add_duplicate) {
    setup();
    XrSet *set = xr_set_new(main_coro);
    ASSERT_TRUE(xr_set_add(set, xr_int(42)));
    // Adding duplicate returns false
    ASSERT_FALSE(xr_set_add(set, xr_int(42)));
    ASSERT_EQ_INT(xr_set_size(set), 1);
    teardown();
}

TEST(set_delete) {
    setup();
    XrSet *set = xr_set_new(main_coro);
    xr_set_add(set, xr_int(1));
    xr_set_add(set, xr_int(2));
    xr_set_add(set, xr_int(3));

    ASSERT_TRUE(xr_set_delete(set, xr_int(2)));
    ASSERT_FALSE(xr_set_has(set, xr_int(2)));
    ASSERT_EQ_INT(xr_set_size(set), 2);

    // Delete non-existent
    ASSERT_FALSE(xr_set_delete(set, xr_int(999)));
    teardown();
}

TEST(set_clear) {
    setup();
    XrSet *set = xr_set_new(main_coro);
    xr_set_add(set, xr_int(1));
    xr_set_add(set, xr_int(2));

    xr_set_clear(set);
    ASSERT_EQ_INT(xr_set_size(set), 0);
    ASSERT_TRUE(xr_set_is_empty(set));
    ASSERT_FALSE(xr_set_has(set, xr_int(1)));
    teardown();
}

/* ========== Set Algebra Tests ========== */

TEST(set_union) {
    setup();
    XrSet *a = xr_set_new(main_coro);
    xr_set_add(a, xr_int(1));
    xr_set_add(a, xr_int(2));

    XrSet *b = xr_set_new(main_coro);
    xr_set_add(b, xr_int(2));
    xr_set_add(b, xr_int(3));

    XrSet *u = xr_set_union(main_coro, a, b);
    ASSERT_NOT_NULL(u);
    ASSERT_EQ_INT(xr_set_size(u), 3);
    ASSERT_TRUE(xr_set_has(u, xr_int(1)));
    ASSERT_TRUE(xr_set_has(u, xr_int(2)));
    ASSERT_TRUE(xr_set_has(u, xr_int(3)));
    teardown();
}

TEST(set_intersection) {
    setup();
    XrSet *a = xr_set_new(main_coro);
    xr_set_add(a, xr_int(1));
    xr_set_add(a, xr_int(2));
    xr_set_add(a, xr_int(3));

    XrSet *b = xr_set_new(main_coro);
    xr_set_add(b, xr_int(2));
    xr_set_add(b, xr_int(3));
    xr_set_add(b, xr_int(4));

    XrSet *inter = xr_set_intersection(main_coro, a, b);
    ASSERT_NOT_NULL(inter);
    ASSERT_EQ_INT(xr_set_size(inter), 2);
    ASSERT_TRUE(xr_set_has(inter, xr_int(2)));
    ASSERT_TRUE(xr_set_has(inter, xr_int(3)));
    ASSERT_FALSE(xr_set_has(inter, xr_int(1)));
    teardown();
}

TEST(set_difference) {
    setup();
    XrSet *a = xr_set_new(main_coro);
    xr_set_add(a, xr_int(1));
    xr_set_add(a, xr_int(2));
    xr_set_add(a, xr_int(3));

    XrSet *b = xr_set_new(main_coro);
    xr_set_add(b, xr_int(2));

    XrSet *diff = xr_set_difference(main_coro, a, b);
    ASSERT_NOT_NULL(diff);
    ASSERT_EQ_INT(xr_set_size(diff), 2);
    ASSERT_TRUE(xr_set_has(diff, xr_int(1)));
    ASSERT_TRUE(xr_set_has(diff, xr_int(3)));
    ASSERT_FALSE(xr_set_has(diff, xr_int(2)));
    teardown();
}

TEST(set_subset_superset) {
    setup();
    XrSet *a = xr_set_new(main_coro);
    xr_set_add(a, xr_int(1));
    xr_set_add(a, xr_int(2));

    XrSet *b = xr_set_new(main_coro);
    xr_set_add(b, xr_int(1));
    xr_set_add(b, xr_int(2));
    xr_set_add(b, xr_int(3));

    ASSERT_TRUE(xr_set_is_subset(a, b));
    ASSERT_FALSE(xr_set_is_subset(b, a));
    ASSERT_TRUE(xr_set_is_superset(b, a));
    ASSERT_FALSE(xr_set_is_superset(a, b));
    teardown();
}

/* ========== Iteration Tests ========== */

TEST(set_values) {
    setup();
    XrSet *set = xr_set_new(main_coro);
    xr_set_add(set, xr_int(10));
    xr_set_add(set, xr_int(20));
    xr_set_add(set, xr_int(30));

    XrArray *vals = xr_set_values(main_coro, set);
    ASSERT_NOT_NULL(vals);
    ASSERT_EQ_INT(xr_array_size(vals), 3);
    teardown();
}

/* ========== from_array Tests ========== */

TEST(set_from_array) {
    setup();
    XrArray *arr = xr_array_new(main_coro);
    xr_array_push(arr, xr_int(1));
    xr_array_push(arr, xr_int(2));
    xr_array_push(arr, xr_int(2));  // duplicate
    xr_array_push(arr, xr_int(3));

    XrSet *set = xr_set_from_array(main_coro, arr);
    ASSERT_NOT_NULL(set);
    ASSERT_EQ_INT(xr_set_size(set), 3);
    ASSERT_TRUE(xr_set_has(set, xr_int(1)));
    ASSERT_TRUE(xr_set_has(set, xr_int(2)));
    ASSERT_TRUE(xr_set_has(set, xr_int(3)));
    teardown();
}

/* ========== Stress Tests ========== */

TEST(set_many_entries) {
    setup();
    XrSet *set = xr_set_new(main_coro);
    int N = 200;
    for (int i = 0; i < N; i++) {
        xr_set_add(set, xr_int(i));
    }
    ASSERT_EQ_INT(xr_set_size(set), (uint32_t) N);

    for (int i = 0; i < N; i++) {
        ASSERT_TRUE(xr_set_has(set, xr_int(i)));
    }
    ASSERT_FALSE(xr_set_has(set, xr_int(N)));
    teardown();
}

/* ========== Main ========== */

int main(void) {
    xr_test_suppress_dialogs();
    RUN_TEST_SUITE("Set Tests");

    RUN_TEST(set_new_empty);
    RUN_TEST(set_with_capacity);
    RUN_TEST(set_add_has);
    RUN_TEST(set_add_duplicate);
    RUN_TEST(set_delete);
    RUN_TEST(set_clear);
    RUN_TEST(set_union);
    RUN_TEST(set_intersection);
    RUN_TEST(set_difference);
    RUN_TEST(set_subset_superset);
    RUN_TEST(set_values);
    RUN_TEST(set_from_array);
    RUN_TEST(set_many_entries);

    TEST_REPORT();
    return TEST_EXIT();
}
