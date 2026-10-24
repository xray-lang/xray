/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_hashmap.c - Unit tests for string-keyed hash table
 */

#include "../test_framework.h"
#include "base/xhashmap.h"

static void require_map_ok(XrOsIoStatus status) {
    if (status != XR_OS_IO_OK) {
        fprintf(stderr, "hashmap operation failed with status %u\n", (unsigned)status);
        exit(EXIT_FAILURE);
    }
}

static XrHashMap *new_map(void) {
    XrOsIoPolicy policy = xr_os_io_system_policy();
    XrHashMap *map = NULL;
    require_map_ok(xr_hashmap_owned_new(&policy, &map));
    return map;
}

static void map_set(XrHashMap *map, const char *key, void *value) {
    require_map_ok(xr_hashmap_owned_set(map, key, value));
}

static void *map_get(const XrHashMap *map, const char *key) {
    void *value = NULL;
    require_map_ok(xr_hashmap_owned_get(map, key, &value));
    return value;
}

static bool map_has(const XrHashMap *map, const char *key) {
    bool present = false;
    require_map_ok(xr_hashmap_owned_has(map, key, &present));
    return present;
}

static bool map_delete(XrHashMap *map, const char *key) {
    bool deleted = false;
    require_map_ok(xr_hashmap_owned_delete(map, key, &deleted));
    return deleted;
}

static void map_clear(XrHashMap *map) {
    require_map_ok(xr_hashmap_owned_clear(map));
}

/* ========== Basic Operations ========== */

TEST(hashmap_create_free) {
    XrHashMap *map = new_map();
    ASSERT_NOT_NULL(map);
    ASSERT_EQ_INT(map->count, 0);
    xr_hashmap_owned_free(map);
}

TEST(hashmap_set_get_single) {
    XrHashMap *map = new_map();

    int value = 42;
    map_set(map, "key1", &value);

    ASSERT_EQ_INT(map->count, 1);

    int *result = (int *) map_get(map, "key1");
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_INT(*result, 42);

    xr_hashmap_owned_free(map);
}

TEST(hashmap_set_get_multiple) {
    XrHashMap *map = new_map();

    int values[] = {10, 20, 30, 40, 50};
    map_set(map, "a", &values[0]);
    map_set(map, "b", &values[1]);
    map_set(map, "c", &values[2]);
    map_set(map, "d", &values[3]);
    map_set(map, "e", &values[4]);

    ASSERT_EQ_INT(map->count, 5);

    ASSERT_EQ_INT(*(int *) map_get(map, "a"), 10);
    ASSERT_EQ_INT(*(int *) map_get(map, "b"), 20);
    ASSERT_EQ_INT(*(int *) map_get(map, "c"), 30);
    ASSERT_EQ_INT(*(int *) map_get(map, "d"), 40);
    ASSERT_EQ_INT(*(int *) map_get(map, "e"), 50);

    xr_hashmap_owned_free(map);
}

TEST(hashmap_overwrite) {
    XrHashMap *map = new_map();

    int v1 = 100, v2 = 200;
    map_set(map, "key", &v1);
    ASSERT_EQ_INT(*(int *) map_get(map, "key"), 100);

    map_set(map, "key", &v2);
    ASSERT_EQ_INT(*(int *) map_get(map, "key"), 200);
    ASSERT_EQ_INT(map->count, 1);  // count unchanged

    xr_hashmap_owned_free(map);
}

TEST(hashmap_get_nonexistent) {
    XrHashMap *map = new_map();

    void *result = map_get(map, "nonexistent");
    ASSERT_TRUE(result == NULL);

    xr_hashmap_owned_free(map);
}

/* ========== Has/Delete Operations ========== */

TEST(hashmap_has) {
    XrHashMap *map = new_map();

    int value = 1;
    map_set(map, "exists", &value);

    ASSERT_TRUE(map_has(map, "exists"));
    ASSERT_FALSE(map_has(map, "not_exists"));

    xr_hashmap_owned_free(map);
}

TEST(hashmap_delete) {
    XrHashMap *map = new_map();

    int value = 42;
    map_set(map, "key", &value);
    ASSERT_TRUE(map_has(map, "key"));
    ASSERT_EQ_INT(map->count, 1);

    bool deleted = map_delete(map, "key");
    ASSERT_TRUE(deleted);
    ASSERT_FALSE(map_has(map, "key"));
    ASSERT_TRUE(map_get(map, "key") == NULL);

    xr_hashmap_owned_free(map);
}

TEST(hashmap_delete_nonexistent) {
    XrHashMap *map = new_map();

    bool deleted = map_delete(map, "nonexistent");
    ASSERT_FALSE(deleted);

    xr_hashmap_owned_free(map);
}

TEST(hashmap_clear) {
    XrHashMap *map = new_map();

    int values[] = {1, 2, 3};
    map_set(map, "a", &values[0]);
    map_set(map, "b", &values[1]);
    map_set(map, "c", &values[2]);
    ASSERT_EQ_INT(map->count, 3);

    map_clear(map);
    ASSERT_EQ_INT(map->count, 0);
    ASSERT_FALSE(map_has(map, "a"));
    ASSERT_FALSE(map_has(map, "b"));
    ASSERT_FALSE(map_has(map, "c"));

    xr_hashmap_owned_free(map);
}

/* ========== Growth Tests ========== */

TEST(hashmap_growth) {
    XrHashMap *map = new_map();

    // Insert many entries to trigger growth
    char keys[100][16];
    int values[100];

    for (int i = 0; i < 100; i++) {
        snprintf(keys[i], sizeof(keys[i]), "key_%d", i);
        values[i] = i * 10;
        map_set(map, keys[i], &values[i]);
    }

    ASSERT_EQ_INT(map->count, 100);

    // Verify all entries
    for (int i = 0; i < 100; i++) {
        int *result = (int *) map_get(map, keys[i]);
        ASSERT_NOT_NULL(result);
        ASSERT_EQ_INT(*result, i * 10);
    }

    xr_hashmap_owned_free(map);
}

/* ========== String Key Tests ========== */

TEST(hashmap_long_keys) {
    XrHashMap *map = new_map();

    const char *long_key = "this_is_a_very_long_key_name_that_might_cause_issues";
    int value = 999;

    map_set(map, long_key, &value);
    ASSERT_TRUE(map_has(map, long_key));
    ASSERT_EQ_INT(*(int *) map_get(map, long_key), 999);

    xr_hashmap_owned_free(map);
}

TEST(hashmap_similar_keys) {
    XrHashMap *map = new_map();

    int v1 = 1, v2 = 2, v3 = 3;
    map_set(map, "test", &v1);
    map_set(map, "test1", &v2);
    map_set(map, "test2", &v3);

    ASSERT_EQ_INT(map->count, 3);
    ASSERT_EQ_INT(*(int *) map_get(map, "test"), 1);
    ASSERT_EQ_INT(*(int *) map_get(map, "test1"), 2);
    ASSERT_EQ_INT(*(int *) map_get(map, "test2"), 3);

    xr_hashmap_owned_free(map);
}

TEST(hashmap_empty_key) {
    XrHashMap *map = new_map();

    int value = 42;
    map_set(map, "", &value);

    ASSERT_TRUE(map_has(map, ""));
    ASSERT_EQ_INT(*(int *) map_get(map, ""), 42);

    xr_hashmap_owned_free(map);
}

/* ========== Pointer Value Tests ========== */

TEST(hashmap_null_value) {
    XrHashMap *map = new_map();

    map_set(map, "null_val", NULL);

    ASSERT_TRUE(map_has(map, "null_val"));
    ASSERT_TRUE(map_get(map, "null_val") == NULL);

    xr_hashmap_owned_free(map);
}

TEST(hashmap_struct_values) {
    XrHashMap *map = new_map();

    typedef struct {
        int x;
        int y;
    } Point;
    Point p1 = {10, 20};
    Point p2 = {30, 40};

    map_set(map, "p1", &p1);
    map_set(map, "p2", &p2);

    Point *r1 = (Point *) map_get(map, "p1");
    Point *r2 = (Point *) map_get(map, "p2");

    ASSERT_NOT_NULL(r1);
    ASSERT_NOT_NULL(r2);
    ASSERT_EQ_INT(r1->x, 10);
    ASSERT_EQ_INT(r1->y, 20);
    ASSERT_EQ_INT(r2->x, 30);
    ASSERT_EQ_INT(r2->y, 40);

    xr_hashmap_owned_free(map);
}

/* ========== Delete and Reinsert ========== */

TEST(hashmap_delete_reinsert) {
    XrHashMap *map = new_map();

    int v1 = 100, v2 = 200;

    map_set(map, "key", &v1);
    ASSERT_EQ_INT(*(int *) map_get(map, "key"), 100);

    map_delete(map, "key");
    ASSERT_FALSE(map_has(map, "key"));

    map_set(map, "key", &v2);
    ASSERT_TRUE(map_has(map, "key"));
    ASSERT_EQ_INT(*(int *) map_get(map, "key"), 200);

    xr_hashmap_owned_free(map);
}

/* ========== Stress Test ========== */

TEST(hashmap_stress) {
    XrHashMap *map = new_map();

    // Insert 1000 entries
    char keys[1000][32];
    int values[1000];

    for (int i = 0; i < 1000; i++) {
        snprintf(keys[i], sizeof(keys[i]), "stress_key_%d", i);
        values[i] = i;
        map_set(map, keys[i], &values[i]);
    }

    ASSERT_EQ_INT(map->count, 1000);

    // Verify random samples
    ASSERT_EQ_INT(*(int *) map_get(map, "stress_key_0"), 0);
    ASSERT_EQ_INT(*(int *) map_get(map, "stress_key_500"), 500);
    ASSERT_EQ_INT(*(int *) map_get(map, "stress_key_999"), 999);

    // Delete half
    for (int i = 0; i < 500; i++) {
        map_delete(map, keys[i]);
    }

    // Verify remaining
    for (int i = 500; i < 1000; i++) {
        ASSERT_TRUE(map_has(map, keys[i]));
    }

    xr_hashmap_owned_free(map);
}

/* ========== Main ========== */

static void run_all_tests(void) {
    RUN_TEST_SUITE("Basic Operations");
    RUN_TEST(hashmap_create_free);
    RUN_TEST(hashmap_set_get_single);
    RUN_TEST(hashmap_set_get_multiple);
    RUN_TEST(hashmap_overwrite);
    RUN_TEST(hashmap_get_nonexistent);

    RUN_TEST_SUITE("Has/Delete Operations");
    RUN_TEST(hashmap_has);
    RUN_TEST(hashmap_delete);
    RUN_TEST(hashmap_delete_nonexistent);
    RUN_TEST(hashmap_clear);

    RUN_TEST_SUITE("Growth");
    RUN_TEST(hashmap_growth);

    RUN_TEST_SUITE("String Keys");
    RUN_TEST(hashmap_long_keys);
    RUN_TEST(hashmap_similar_keys);
    RUN_TEST(hashmap_empty_key);

    RUN_TEST_SUITE("Pointer Values");
    RUN_TEST(hashmap_null_value);
    RUN_TEST(hashmap_struct_values);

    RUN_TEST_SUITE("Delete and Reinsert");
    RUN_TEST(hashmap_delete_reinsert);

    RUN_TEST_SUITE("Stress Test");
    RUN_TEST(hashmap_stress);
}

TEST_MAIN_BEGIN()
printf("=== xray HashMap Unit Tests ===\n");
run_all_tests();
TEST_MAIN_END()
