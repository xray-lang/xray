/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xmap.c - Unit tests for Map object
 *
 * KEY CONCEPT:
 *   Tests Map creation, set/get/has/delete operations,
 *   iteration (keys/values), and mixed-type key support.
 */

#include "../test_framework.h"
#include "../test_helper.h"
#include "runtime/object/xmap.h"
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

TEST(map_new_empty) {
    setup();
    XrMap *map = xr_map_new(main_coro);
    ASSERT_NOT_NULL(map);
    ASSERT_EQ_INT(xr_map_size(map), 0);
    ASSERT_TRUE(xr_map_is_empty(map));
    teardown();
}

TEST(map_with_capacity) {
    setup();
    XrMap *map = xr_map_with_capacity(main_coro, 64);
    ASSERT_NOT_NULL(map);
    ASSERT_EQ_INT(xr_map_size(map), 0);
    teardown();
}

/* ========== Set/Get Tests ========== */

TEST(map_set_get_int_key) {
    setup();
    XrMap *map = xr_map_new(main_coro);
    xr_map_set(map, xr_int(1), xr_int(100));
    xr_map_set(map, xr_int(2), xr_int(200));

    bool found = false;
    XrValue v = xr_map_get(map, xr_int(1), &found);
    ASSERT_TRUE(found);
    ASSERT_EQ_INT(XR_TO_INT(v), 100);

    v = xr_map_get(map, xr_int(2), &found);
    ASSERT_TRUE(found);
    ASSERT_EQ_INT(XR_TO_INT(v), 200);

    ASSERT_EQ_INT(xr_map_size(map), 2);
    teardown();
}

TEST(map_get_missing_key) {
    setup();
    XrMap *map = xr_map_new(main_coro);
    xr_map_set(map, xr_int(1), xr_int(100));

    bool found = false;
    xr_map_get(map, xr_int(999), &found);
    ASSERT_FALSE(found);
    teardown();
}

TEST(map_overwrite_value) {
    setup();
    XrMap *map = xr_map_new(main_coro);
    xr_map_set(map, xr_int(1), xr_int(100));
    xr_map_set(map, xr_int(1), xr_int(999));

    bool found = false;
    XrValue v = xr_map_get(map, xr_int(1), &found);
    ASSERT_TRUE(found);
    ASSERT_EQ_INT(XR_TO_INT(v), 999);
    ASSERT_EQ_INT(xr_map_size(map), 1);
    teardown();
}

/* ========== Has/Delete Tests ========== */

TEST(map_has) {
    setup();
    XrMap *map = xr_map_new(main_coro);
    xr_map_set(map, xr_int(42), xr_int(1));

    ASSERT_TRUE(xr_map_has(map, xr_int(42)));
    ASSERT_FALSE(xr_map_has(map, xr_int(99)));
    teardown();
}

TEST(map_delete) {
    setup();
    XrMap *map = xr_map_new(main_coro);
    xr_map_set(map, xr_int(1), xr_int(100));
    xr_map_set(map, xr_int(2), xr_int(200));

    ASSERT_TRUE(xr_map_delete(map, xr_int(1)));
    ASSERT_FALSE(xr_map_has(map, xr_int(1)));
    ASSERT_EQ_INT(xr_map_size(map), 1);

    // Delete non-existent key
    ASSERT_FALSE(xr_map_delete(map, xr_int(999)));
    teardown();
}

TEST(map_clear) {
    setup();
    XrMap *map = xr_map_new(main_coro);
    xr_map_set(map, xr_int(1), xr_int(100));
    xr_map_set(map, xr_int(2), xr_int(200));
    xr_map_set(map, xr_int(3), xr_int(300));

    xr_map_clear(map);
    ASSERT_EQ_INT(xr_map_size(map), 0);
    ASSERT_TRUE(xr_map_is_empty(map));
    ASSERT_FALSE(xr_map_has(map, xr_int(1)));
    teardown();
}

/* ========== Iteration Tests ========== */

TEST(map_keys) {
    setup();
    XrMap *map = xr_map_new(main_coro);
    xr_map_set(map, xr_int(10), xr_int(100));
    xr_map_set(map, xr_int(20), xr_int(200));

    XrArray *keys = xr_map_keys(main_coro, map);
    ASSERT_NOT_NULL(keys);
    ASSERT_EQ_INT(xr_array_size(keys), 2);
    teardown();
}

TEST(map_values) {
    setup();
    XrMap *map = xr_map_new(main_coro);
    xr_map_set(map, xr_int(10), xr_int(100));
    xr_map_set(map, xr_int(20), xr_int(200));

    XrArray *vals = xr_map_values(main_coro, map);
    ASSERT_NOT_NULL(vals);
    ASSERT_EQ_INT(xr_array_size(vals), 2);
    teardown();
}

/* ========== has_value Tests ========== */

TEST(map_has_value) {
    setup();
    XrMap *map = xr_map_new(main_coro);
    xr_map_set(map, xr_int(1), xr_int(100));

    ASSERT_TRUE(xr_map_has_value(map, xr_int(100)));
    ASSERT_FALSE(xr_map_has_value(map, xr_int(999)));
    teardown();
}

/* ========== Stress Tests ========== */

TEST(map_many_entries) {
    setup();
    XrMap *map = xr_map_new(main_coro);
    int N = 200;
    for (int i = 0; i < N; i++) {
        xr_map_set(map, xr_int(i), xr_int(i * 10));
    }
    ASSERT_EQ_INT(xr_map_size(map), (uint32_t) N);

    // Verify all entries
    for (int i = 0; i < N; i++) {
        bool found = false;
        XrValue v = xr_map_get(map, xr_int(i), &found);
        ASSERT_TRUE(found);
        ASSERT_EQ_INT(XR_TO_INT(v), i * 10);
    }
    teardown();
}

/* ========== Main ========== */

int main(void) {
    xr_test_suppress_dialogs();
    RUN_TEST_SUITE("Map Tests");

    RUN_TEST(map_new_empty);
    RUN_TEST(map_with_capacity);
    RUN_TEST(map_set_get_int_key);
    RUN_TEST(map_get_missing_key);
    RUN_TEST(map_overwrite_value);
    RUN_TEST(map_has);
    RUN_TEST(map_delete);
    RUN_TEST(map_clear);
    RUN_TEST(map_keys);
    RUN_TEST(map_values);
    RUN_TEST(map_has_value);
    RUN_TEST(map_many_entries);

    TEST_REPORT();
    return TEST_EXIT();
}
