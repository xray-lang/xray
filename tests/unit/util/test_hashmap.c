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

/* ========== Basic Operations ========== */

TEST(hashmap_create_free) {
    XrHashMap *map = xr_hashmap_new();
    ASSERT_NOT_NULL(map);
    ASSERT_EQ_INT(map->count, 0);
    xr_hashmap_free(map);
}

TEST(hashmap_set_get_single) {
    XrHashMap *map = xr_hashmap_new();

    int value = 42;
    xr_hashmap_set(map, "key1", &value);

    ASSERT_EQ_INT(map->count, 1);

    int *result = (int *) xr_hashmap_get(map, "key1");
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_INT(*result, 42);

    xr_hashmap_free(map);
}

TEST(hashmap_set_get_multiple) {
    XrHashMap *map = xr_hashmap_new();

    int values[] = {10, 20, 30, 40, 50};
    xr_hashmap_set(map, "a", &values[0]);
    xr_hashmap_set(map, "b", &values[1]);
    xr_hashmap_set(map, "c", &values[2]);
    xr_hashmap_set(map, "d", &values[3]);
    xr_hashmap_set(map, "e", &values[4]);

    ASSERT_EQ_INT(map->count, 5);

    ASSERT_EQ_INT(*(int *) xr_hashmap_get(map, "a"), 10);
    ASSERT_EQ_INT(*(int *) xr_hashmap_get(map, "b"), 20);
    ASSERT_EQ_INT(*(int *) xr_hashmap_get(map, "c"), 30);
    ASSERT_EQ_INT(*(int *) xr_hashmap_get(map, "d"), 40);
    ASSERT_EQ_INT(*(int *) xr_hashmap_get(map, "e"), 50);

    xr_hashmap_free(map);
}

TEST(hashmap_overwrite) {
    XrHashMap *map = xr_hashmap_new();

    int v1 = 100, v2 = 200;
    xr_hashmap_set(map, "key", &v1);
    ASSERT_EQ_INT(*(int *) xr_hashmap_get(map, "key"), 100);

    xr_hashmap_set(map, "key", &v2);
    ASSERT_EQ_INT(*(int *) xr_hashmap_get(map, "key"), 200);
    ASSERT_EQ_INT(map->count, 1);  // count unchanged

    xr_hashmap_free(map);
}

TEST(hashmap_get_nonexistent) {
    XrHashMap *map = xr_hashmap_new();

    void *result = xr_hashmap_get(map, "nonexistent");
    ASSERT_TRUE(result == NULL);

    xr_hashmap_free(map);
}

/* ========== Has/Delete Operations ========== */

TEST(hashmap_has) {
    XrHashMap *map = xr_hashmap_new();

    int value = 1;
    xr_hashmap_set(map, "exists", &value);

    ASSERT_TRUE(xr_hashmap_has(map, "exists"));
    ASSERT_FALSE(xr_hashmap_has(map, "not_exists"));

    xr_hashmap_free(map);
}

TEST(hashmap_delete) {
    XrHashMap *map = xr_hashmap_new();

    int value = 42;
    xr_hashmap_set(map, "key", &value);
    ASSERT_TRUE(xr_hashmap_has(map, "key"));
    ASSERT_EQ_INT(map->count, 1);

    bool deleted = xr_hashmap_delete(map, "key");
    ASSERT_TRUE(deleted);
    ASSERT_FALSE(xr_hashmap_has(map, "key"));
    ASSERT_TRUE(xr_hashmap_get(map, "key") == NULL);

    xr_hashmap_free(map);
}

TEST(hashmap_delete_nonexistent) {
    XrHashMap *map = xr_hashmap_new();

    bool deleted = xr_hashmap_delete(map, "nonexistent");
    ASSERT_FALSE(deleted);

    xr_hashmap_free(map);
}

TEST(hashmap_clear) {
    XrHashMap *map = xr_hashmap_new();

    int values[] = {1, 2, 3};
    xr_hashmap_set(map, "a", &values[0]);
    xr_hashmap_set(map, "b", &values[1]);
    xr_hashmap_set(map, "c", &values[2]);
    ASSERT_EQ_INT(map->count, 3);

    xr_hashmap_clear(map);
    ASSERT_EQ_INT(map->count, 0);
    ASSERT_FALSE(xr_hashmap_has(map, "a"));
    ASSERT_FALSE(xr_hashmap_has(map, "b"));
    ASSERT_FALSE(xr_hashmap_has(map, "c"));

    xr_hashmap_free(map);
}

/* ========== Growth Tests ========== */

TEST(hashmap_growth) {
    XrHashMap *map = xr_hashmap_new();

    // Insert many entries to trigger growth
    char keys[100][16];
    int values[100];

    for (int i = 0; i < 100; i++) {
        snprintf(keys[i], sizeof(keys[i]), "key_%d", i);
        values[i] = i * 10;
        xr_hashmap_set(map, keys[i], &values[i]);
    }

    ASSERT_EQ_INT(map->count, 100);

    // Verify all entries
    for (int i = 0; i < 100; i++) {
        int *result = (int *) xr_hashmap_get(map, keys[i]);
        ASSERT_NOT_NULL(result);
        ASSERT_EQ_INT(*result, i * 10);
    }

    xr_hashmap_free(map);
}

/* ========== String Key Tests ========== */

TEST(hashmap_long_keys) {
    XrHashMap *map = xr_hashmap_new();

    const char *long_key = "this_is_a_very_long_key_name_that_might_cause_issues";
    int value = 999;

    xr_hashmap_set(map, long_key, &value);
    ASSERT_TRUE(xr_hashmap_has(map, long_key));
    ASSERT_EQ_INT(*(int *) xr_hashmap_get(map, long_key), 999);

    xr_hashmap_free(map);
}

TEST(hashmap_similar_keys) {
    XrHashMap *map = xr_hashmap_new();

    int v1 = 1, v2 = 2, v3 = 3;
    xr_hashmap_set(map, "test", &v1);
    xr_hashmap_set(map, "test1", &v2);
    xr_hashmap_set(map, "test2", &v3);

    ASSERT_EQ_INT(map->count, 3);
    ASSERT_EQ_INT(*(int *) xr_hashmap_get(map, "test"), 1);
    ASSERT_EQ_INT(*(int *) xr_hashmap_get(map, "test1"), 2);
    ASSERT_EQ_INT(*(int *) xr_hashmap_get(map, "test2"), 3);

    xr_hashmap_free(map);
}

TEST(hashmap_empty_key) {
    XrHashMap *map = xr_hashmap_new();

    int value = 42;
    xr_hashmap_set(map, "", &value);

    ASSERT_TRUE(xr_hashmap_has(map, ""));
    ASSERT_EQ_INT(*(int *) xr_hashmap_get(map, ""), 42);

    xr_hashmap_free(map);
}

/* ========== Pointer Value Tests ========== */

TEST(hashmap_null_value) {
    XrHashMap *map = xr_hashmap_new();

    xr_hashmap_set(map, "null_val", NULL);

    ASSERT_TRUE(xr_hashmap_has(map, "null_val"));
    ASSERT_TRUE(xr_hashmap_get(map, "null_val") == NULL);

    xr_hashmap_free(map);
}

TEST(hashmap_struct_values) {
    XrHashMap *map = xr_hashmap_new();

    typedef struct {
        int x;
        int y;
    } Point;
    Point p1 = {10, 20};
    Point p2 = {30, 40};

    xr_hashmap_set(map, "p1", &p1);
    xr_hashmap_set(map, "p2", &p2);

    Point *r1 = (Point *) xr_hashmap_get(map, "p1");
    Point *r2 = (Point *) xr_hashmap_get(map, "p2");

    ASSERT_NOT_NULL(r1);
    ASSERT_NOT_NULL(r2);
    ASSERT_EQ_INT(r1->x, 10);
    ASSERT_EQ_INT(r1->y, 20);
    ASSERT_EQ_INT(r2->x, 30);
    ASSERT_EQ_INT(r2->y, 40);

    xr_hashmap_free(map);
}

/* ========== Delete and Reinsert ========== */

TEST(hashmap_delete_reinsert) {
    XrHashMap *map = xr_hashmap_new();

    int v1 = 100, v2 = 200;

    xr_hashmap_set(map, "key", &v1);
    ASSERT_EQ_INT(*(int *) xr_hashmap_get(map, "key"), 100);

    xr_hashmap_delete(map, "key");
    ASSERT_FALSE(xr_hashmap_has(map, "key"));

    xr_hashmap_set(map, "key", &v2);
    ASSERT_TRUE(xr_hashmap_has(map, "key"));
    ASSERT_EQ_INT(*(int *) xr_hashmap_get(map, "key"), 200);

    xr_hashmap_free(map);
}

/* ========== Stress Test ========== */

TEST(hashmap_stress) {
    XrHashMap *map = xr_hashmap_new();

    // Insert 1000 entries
    char keys[1000][32];
    int values[1000];

    for (int i = 0; i < 1000; i++) {
        snprintf(keys[i], sizeof(keys[i]), "stress_key_%d", i);
        values[i] = i;
        xr_hashmap_set(map, keys[i], &values[i]);
    }

    ASSERT_EQ_INT(map->count, 1000);

    // Verify random samples
    ASSERT_EQ_INT(*(int *) xr_hashmap_get(map, "stress_key_0"), 0);
    ASSERT_EQ_INT(*(int *) xr_hashmap_get(map, "stress_key_500"), 500);
    ASSERT_EQ_INT(*(int *) xr_hashmap_get(map, "stress_key_999"), 999);

    // Delete half
    for (int i = 0; i < 500; i++) {
        xr_hashmap_delete(map, keys[i]);
    }

    // Verify remaining
    for (int i = 500; i < 1000; i++) {
        ASSERT_TRUE(xr_hashmap_has(map, keys[i]));
    }

    xr_hashmap_free(map);
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
