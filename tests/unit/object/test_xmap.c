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
#include <stdio.h>
#include <string.h>

static XrVMRuntime *X = NULL;
static XrCoroutine *main_coro = NULL;

/* ========== Setup / Teardown ========== */

static void setup(void) {
    X = xray_vm_new(NULL);
    ASSERT_NOT_NULL(X);
    main_coro = xr_test_init_coro(X);
    ASSERT_NOT_NULL(main_coro);
}

static void teardown(void) {
    if (X) {
        xray_vm_delete(X);
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

TEST(map_tombstone_probe_and_compaction_order) {
    setup();
    XrMap *map = xr_map_with_capacity(main_coro, 8);

    for (int i = 0; i < 24; i++)
        xr_map_set(map, xr_int(i), xr_int(i * 10));
    for (int i = 1; i < 16; i += 2)
        ASSERT_TRUE(xr_map_delete(map, xr_int(i)));
    for (int i = 100; i < 116; i++)
        xr_map_set(map, xr_int(i), xr_int(i * 10));

    for (int i = 1; i < 16; i += 2)
        ASSERT_FALSE(xr_map_has(map, xr_int(i)));
    for (int i = 0; i < 24; i += 2)
        ASSERT_TRUE(xr_map_has(map, xr_int(i)));
    for (int i = 17; i < 24; i += 2)
        ASSERT_TRUE(xr_map_has(map, xr_int(i)));
    for (int i = 100; i < 116; i++)
        ASSERT_TRUE(xr_map_has(map, xr_int(i)));

    XrArray *keys = xr_map_keys(main_coro, map);
    XrArray *values = xr_map_values(main_coro, map);
    ASSERT_NOT_NULL(keys);
    ASSERT_NOT_NULL(values);
    ASSERT_EQ_INT(xr_array_size(keys), 32);
    ASSERT_EQ_INT(xr_array_size(values), 32);

    int out = 0;
    for (int i = 0; i < 24; i++) {
        if (i < 16 && (i & 1))
            continue;
        ASSERT_EQ_INT(XR_TO_INT(xr_array_get(keys, out)), i);
        ASSERT_EQ_INT(XR_TO_INT(xr_array_get(values, out)), i * 10);
        out++;
    }
    for (int i = 100; i < 116; i++) {
        ASSERT_EQ_INT(XR_TO_INT(xr_array_get(keys, out)), i);
        ASSERT_EQ_INT(XR_TO_INT(xr_array_get(values, out)), i * 10);
        out++;
    }
    ASSERT_EQ_INT(out, 32);

    teardown();
}

TEST(map_string_fast_path_swiss_ctrl) {
    setup();
    XrMap *map = xr_map_with_capacity(main_coro, 8);
    XrString *keys[32];
    char name[16];

    for (int i = 0; i < 32; i++) {
        snprintf(name, sizeof(name), "key_%02d", i);
        keys[i] = xr_string_intern(X, name, strlen(name), 0);
        ASSERT_NOT_NULL(keys[i]);
        xr_map_set(map, xr_string_value(keys[i]), xr_int(i * 11));
    }
    for (int i = 1; i < 20; i += 2)
        ASSERT_TRUE(xr_map_delete(map, xr_string_value(keys[i])));
    for (int i = 0; i < 16; i++) {
        snprintf(name, sizeof(name), "late_%02d", i);
        XrString *late = xr_string_intern(X, name, strlen(name), 0);
        ASSERT_NOT_NULL(late);
        xr_map_set(map, xr_string_value(late), xr_int(1000 + i));
    }

    for (int i = 0; i < 32; i++) {
        XrMapEntry *entry = xr_map_find_string_fast(map, keys[i]);
        if (i < 20 && (i & 1)) {
            ASSERT_NULL(entry);
        } else {
            ASSERT_NOT_NULL(entry);
            ASSERT_EQ_INT(XR_TO_INT(entry->value), i * 11);
        }
    }

    XrString *missing = xr_string_intern(X, "missing_key", 11, 0);
    ASSERT_NULL(xr_map_find_string_fast(map, missing));
    teardown();
}

TEST(map_local_string_key_canonicalizes_for_fast_lookup) {
    setup();
    XrMap *map = xr_map_with_capacity(main_coro, 8);

    XrString *local_key = xr_string_new(X, "lazy_key", 8);
    ASSERT_NOT_NULL(local_key);
    ASSERT_TRUE(XR_STR_IS_LOCAL(local_key));
    ASSERT_FALSE(XR_STR_IS_INTERNED(local_key));
    xr_map_set(map, xr_string_value(local_key), xr_int(7));

    XrString *interned = xr_string_intern(X, "lazy_key", 8, 0);
    ASSERT_NOT_NULL(interned);
    XrMapEntry *entry = xr_map_find_string_fast(map, interned);
    ASSERT_NOT_NULL(entry);
    ASSERT_EQ_PTR(XR_TO_STRING(entry->key), interned);
    ASSERT_EQ_INT(XR_TO_INT(entry->value), 7);

    XrString *probe = xr_string_new(X, "lazy_key", 8);
    bool found = false;
    XrValue value = xr_map_get(map, xr_string_value(probe), &found);
    ASSERT_TRUE(found);
    ASSERT_EQ_INT(XR_TO_INT(value), 7);

    XrString *update_key = xr_string_new(X, "lazy_key", 8);
    xr_map_set(map, xr_string_value(update_key), xr_int(11));
    ASSERT_EQ_INT(xr_map_size(map), 1);
    value = xr_map_get(map, xr_string_value(probe), &found);
    ASSERT_TRUE(found);
    ASSERT_EQ_INT(XR_TO_INT(value), 11);

    XrString *delete_key = xr_string_new(X, "lazy_key", 8);
    ASSERT_TRUE(xr_map_delete(map, xr_string_value(delete_key)));
    ASSERT_FALSE(xr_map_has(map, xr_string_value(probe)));
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
    RUN_TEST(map_tombstone_probe_and_compaction_order);
    RUN_TEST(map_string_fast_path_swiss_ctrl);
    RUN_TEST(map_local_string_key_canonicalizes_for_fast_lookup);

    TEST_REPORT();
    return TEST_EXIT();
}
