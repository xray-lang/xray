/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_intmap.c - Unit tests for integer-keyed hash table
 *
 * KEY CONCEPT:
 *   Tests XrIntMap operations: create, set, get, has, delete, clear,
 *   tombstone handling, rehash, reserved keys, and arena allocation.
 */

#include "../test_framework.h"
#include "base/xintmap.h"
#include "base/xarena.h"

/* ========== Basic Operations ========== */

TEST(intmap_create_free) {
    XrIntMap *map = xr_intmap_new();
    ASSERT_NOT_NULL(map);
    ASSERT_EQ_UINT(map->count, 0);
    ASSERT_EQ_UINT(map->capacity, XR_INTMAP_MIN_CAPACITY);
    xr_intmap_free(map);
}

TEST(intmap_set_get_single) {
    XrIntMap *map = xr_intmap_new();
    ASSERT_NOT_NULL(map);

    int val = 42;
    xr_intmap_set(map, 100, &val);
    ASSERT_EQ_UINT(map->count, 1);

    void *got = xr_intmap_get(map, 100);
    ASSERT_NOT_NULL(got);
    ASSERT_EQ_INT(*(int*)got, 42);

    xr_intmap_free(map);
}

TEST(intmap_set_get_multiple) {
    XrIntMap *map = xr_intmap_new();
    int vals[5] = {10, 20, 30, 40, 50};

    for (int i = 0; i < 5; i++) {
        xr_intmap_set(map, (uint32_t)(i + 1), &vals[i]);
    }
    ASSERT_EQ_UINT(map->count, 5);

    for (int i = 0; i < 5; i++) {
        void *got = xr_intmap_get(map, (uint32_t)(i + 1));
        ASSERT_NOT_NULL(got);
        ASSERT_EQ_INT(*(int*)got, vals[i]);
    }

    xr_intmap_free(map);
}

TEST(intmap_overwrite) {
    XrIntMap *map = xr_intmap_new();
    int a = 1, b = 2;

    xr_intmap_set(map, 42, &a);
    ASSERT_EQ_INT(*(int*)xr_intmap_get(map, 42), 1);

    xr_intmap_set(map, 42, &b);
    ASSERT_EQ_UINT(map->count, 1);  // count unchanged
    ASSERT_EQ_INT(*(int*)xr_intmap_get(map, 42), 2);

    xr_intmap_free(map);
}

/* ========== Has / Delete ========== */

TEST(intmap_has) {
    XrIntMap *map = xr_intmap_new();
    int v = 99;
    xr_intmap_set(map, 7, &v);

    ASSERT_TRUE(xr_intmap_has(map, 7));
    ASSERT_FALSE(xr_intmap_has(map, 8));
    ASSERT_FALSE(xr_intmap_has(map, 0));

    xr_intmap_free(map);
}

TEST(intmap_delete) {
    XrIntMap *map = xr_intmap_new();
    int v = 55;
    xr_intmap_set(map, 10, &v);
    ASSERT_TRUE(xr_intmap_has(map, 10));

    bool deleted = xr_intmap_delete(map, 10);
    ASSERT_TRUE(deleted);
    ASSERT_EQ_UINT(map->count, 0);
    ASSERT_FALSE(xr_intmap_has(map, 10));
    ASSERT_NULL(xr_intmap_get(map, 10));

    // Double delete
    ASSERT_FALSE(xr_intmap_delete(map, 10));

    xr_intmap_free(map);
}

TEST(intmap_delete_nonexistent) {
    XrIntMap *map = xr_intmap_new();
    ASSERT_FALSE(xr_intmap_delete(map, 999));
    xr_intmap_free(map);
}

/* ========== Clear ========== */

TEST(intmap_clear) {
    XrIntMap *map = xr_intmap_new();
    int v = 1;
    for (uint32_t i = 1; i <= 10; i++) {
        xr_intmap_set(map, i, &v);
    }
    ASSERT_EQ_UINT(map->count, 10);

    xr_intmap_clear(map);
    ASSERT_EQ_UINT(map->count, 0);
    ASSERT_EQ_UINT(map->tombstones, 0);

    for (uint32_t i = 1; i <= 10; i++) {
        ASSERT_FALSE(xr_intmap_has(map, i));
    }

    xr_intmap_free(map);
}

/* ========== Reserved Keys ========== */

TEST(intmap_reserved_keys) {
    XrIntMap *map = xr_intmap_new();
    int v = 1;

    // XR_INTMAP_EMPTY and XR_INTMAP_TOMBSTONE are reserved
    xr_intmap_set(map, XR_INTMAP_EMPTY, &v);
    ASSERT_EQ_UINT(map->count, 0);  // should be rejected

    xr_intmap_set(map, XR_INTMAP_TOMBSTONE, &v);
    ASSERT_EQ_UINT(map->count, 0);  // should be rejected

    ASSERT_FALSE(xr_intmap_has(map, XR_INTMAP_EMPTY));
    ASSERT_FALSE(xr_intmap_has(map, XR_INTMAP_TOMBSTONE));
    ASSERT_NULL(xr_intmap_get(map, XR_INTMAP_EMPTY));
    ASSERT_NULL(xr_intmap_get(map, XR_INTMAP_TOMBSTONE));

    xr_intmap_free(map);
}

/* ========== Tombstone / Rehash ========== */

TEST(intmap_tombstone_reuse) {
    XrIntMap *map = xr_intmap_new();
    int v1 = 1, v2 = 2;

    xr_intmap_set(map, 5, &v1);
    xr_intmap_delete(map, 5);
    ASSERT_EQ_UINT(map->tombstones, 1);

    // Re-insert at same key should reuse tombstone slot
    xr_intmap_set(map, 5, &v2);
    ASSERT_EQ_UINT(map->count, 1);
    ASSERT_EQ_INT(*(int*)xr_intmap_get(map, 5), 2);

    xr_intmap_free(map);
}

TEST(intmap_grow) {
    XrIntMap *map = xr_intmap_new();
    uint32_t initial_cap = map->capacity;

    // Insert enough to trigger resize (>75% load)
    int vals[32];
    for (uint32_t i = 0; i < 14; i++) {  // 14/16 = 87.5% > 75%
        vals[i] = (int)(i * 10);
        xr_intmap_set(map, i + 1, &vals[i]);
    }

    // Should have grown
    ASSERT_GT(map->capacity, initial_cap);

    // All values still accessible
    for (uint32_t i = 0; i < 14; i++) {
        void *got = xr_intmap_get(map, i + 1);
        ASSERT_NOT_NULL(got);
        ASSERT_EQ_INT(*(int*)got, (int)(i * 10));
    }

    xr_intmap_free(map);
}

/* ========== Foreach ========== */

static void count_callback(uint32_t key, void *value, void *userdata) {
    (void)key;
    (void)value;
    int *count = (int*)userdata;
    (*count)++;
}

TEST(intmap_foreach) {
    XrIntMap *map = xr_intmap_new();
    int v = 1;
    for (uint32_t i = 1; i <= 5; i++) {
        xr_intmap_set(map, i, &v);
    }

    int count = 0;
    xr_intmap_foreach(map, count_callback, &count);
    ASSERT_EQ_INT(count, 5);

    xr_intmap_free(map);
}

/* ========== Arena Allocation ========== */

TEST(intmap_arena) {
    XrArena arena;
    xr_arena_init(&arena, 4096);

    XrIntMap *map = xr_intmap_new_in_arena(&arena);
    ASSERT_NOT_NULL(map);
    ASSERT_TRUE(map->is_arena_allocated);

    int v = 77;
    xr_intmap_set(map, 1, &v);
    ASSERT_EQ_UINT(map->count, 1);
    ASSERT_EQ_INT(*(int*)xr_intmap_get(map, 1), 77);

    // No xr_intmap_free needed - arena handles it
    xr_arena_destroy(&arena);
}

/* ========== NULL Safety ========== */

TEST(intmap_null_safety) {
    // All operations should handle NULL map gracefully
    xr_intmap_set(NULL, 1, NULL);
    ASSERT_NULL(xr_intmap_get(NULL, 1));
    ASSERT_FALSE(xr_intmap_has(NULL, 1));
    ASSERT_FALSE(xr_intmap_delete(NULL, 1));
    xr_intmap_clear(NULL);
    xr_intmap_foreach(NULL, count_callback, NULL);
    xr_intmap_free(NULL);

    ASSERT_NULL(xr_intmap_new_in_arena(NULL));
    ASSERT_TRUE(1);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

    RUN_TEST_SUITE("IntMap - Basic Operations");
    RUN_TEST(intmap_create_free);
    RUN_TEST(intmap_set_get_single);
    RUN_TEST(intmap_set_get_multiple);
    RUN_TEST(intmap_overwrite);

    RUN_TEST_SUITE("IntMap - Has / Delete");
    RUN_TEST(intmap_has);
    RUN_TEST(intmap_delete);
    RUN_TEST(intmap_delete_nonexistent);

    RUN_TEST_SUITE("IntMap - Clear");
    RUN_TEST(intmap_clear);

    RUN_TEST_SUITE("IntMap - Reserved Keys");
    RUN_TEST(intmap_reserved_keys);

    RUN_TEST_SUITE("IntMap - Tombstone / Rehash");
    RUN_TEST(intmap_tombstone_reuse);
    RUN_TEST(intmap_grow);

    RUN_TEST_SUITE("IntMap - Foreach");
    RUN_TEST(intmap_foreach);

    RUN_TEST_SUITE("IntMap - Arena");
    RUN_TEST(intmap_arena);

    RUN_TEST_SUITE("IntMap - NULL Safety");
    RUN_TEST(intmap_null_safety);

TEST_MAIN_END()
