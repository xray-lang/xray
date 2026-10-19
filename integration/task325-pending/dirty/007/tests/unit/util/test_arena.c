/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_arena.c - Unit tests for arena memory allocator
 */

#include "../test_framework.h"
#include "base/xarena.h"
#include <string.h>

/* ========== Basic Operations ========== */

TEST(arena_init_destroy) {
    XrArena arena;
    xr_arena_init(&arena, 1024);

    ASSERT_NOT_NULL(arena.head);
    ASSERT_TRUE(arena.position != NULL);
    ASSERT_TRUE(arena.limit != NULL);

    xr_arena_destroy(&arena);
}

TEST(arena_alloc_single) {
    XrArena arena;
    xr_arena_init(&arena, 1024);

    int *p = (int *) xr_arena_alloc(&arena, sizeof(int));
    ASSERT_NOT_NULL(p);

    *p = 42;
    ASSERT_EQ_INT(*p, 42);

    xr_arena_destroy(&arena);
}

TEST(arena_alloc_multiple) {
    XrArena arena;
    xr_arena_init(&arena, 1024);

    int *a = (int *) xr_arena_alloc(&arena, sizeof(int));
    int *b = (int *) xr_arena_alloc(&arena, sizeof(int));
    int *c = (int *) xr_arena_alloc(&arena, sizeof(int));

    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NOT_NULL(c);

    // Should be different addresses
    ASSERT_TRUE(a != b);
    ASSERT_TRUE(b != c);
    ASSERT_TRUE(a != c);

    *a = 1;
    *b = 2;
    *c = 3;

    ASSERT_EQ_INT(*a, 1);
    ASSERT_EQ_INT(*b, 2);
    ASSERT_EQ_INT(*c, 3);

    xr_arena_destroy(&arena);
}

TEST(arena_alloc_struct) {
    XrArena arena;
    xr_arena_init(&arena, 1024);

    typedef struct {
        int x;
        int y;
        double z;
    } Point3D;

    Point3D *p = xr_arena_new(&arena, Point3D);
    ASSERT_NOT_NULL(p);

    p->x = 10;
    p->y = 20;
    p->z = 30.5;

    ASSERT_EQ_INT(p->x, 10);
    ASSERT_EQ_INT(p->y, 20);
    ASSERT_FLOAT_EQ(p->z, 30.5, 1e-10);

    xr_arena_destroy(&arena);
}

TEST(arena_alloc_array) {
    XrArena arena;
    xr_arena_init(&arena, 1024);

    int *arr = xr_arena_array(&arena, int, 100);
    ASSERT_NOT_NULL(arr);

    for (int i = 0; i < 100; i++) {
        arr[i] = i * 2;
    }

    for (int i = 0; i < 100; i++) {
        ASSERT_EQ_INT(arr[i], i * 2);
    }

    xr_arena_destroy(&arena);
}

/* ========== String Operations ========== */

TEST(arena_strdup) {
    XrArena arena;
    xr_arena_init(&arena, 1024);

    const char *original = "Hello, World!";
    char *copy = xr_arena_strdup(&arena, original);

    ASSERT_NOT_NULL(copy);
    ASSERT_STR_EQ(copy, original);
    ASSERT_TRUE(copy != original);  // Different address

    xr_arena_destroy(&arena);
}

TEST(arena_strndup) {
    XrArena arena;
    xr_arena_init(&arena, 1024);

    const char *original = "Hello, World!";
    char *copy = xr_arena_strndup(&arena, original, 5);

    ASSERT_NOT_NULL(copy);
    ASSERT_STR_EQ(copy, "Hello");

    xr_arena_destroy(&arena);
}

TEST(arena_strdup_empty) {
    XrArena arena;
    xr_arena_init(&arena, 1024);

    char *copy = xr_arena_strdup(&arena, "");
    ASSERT_NOT_NULL(copy);
    ASSERT_STR_EQ(copy, "");

    xr_arena_destroy(&arena);
}

/* ========== Reset Operations ========== */

TEST(arena_reset) {
    XrArena arena;
    xr_arena_init(&arena, 1024);

    // Allocate some memory
    for (int i = 0; i < 10; i++) {
        xr_arena_alloc(&arena, 64);
    }

    size_t before_reset = xr_arena_get_allocated_size(&arena);
    ASSERT_TRUE(before_reset > 0);

    // Reset
    xr_arena_reset(&arena);

    // After reset, can reuse memory
    int *p = (int *) xr_arena_alloc(&arena, sizeof(int));
    ASSERT_NOT_NULL(p);
    *p = 999;
    ASSERT_EQ_INT(*p, 999);

    xr_arena_destroy(&arena);
}

TEST(arena_reset_reuse) {
    XrArena arena;
    xr_arena_init(&arena, 1024);

    // First phase
    char *s1 = xr_arena_strdup(&arena, "first");
    ASSERT_STR_EQ(s1, "first");

    xr_arena_reset(&arena);

    // Second phase - memory reused
    char *s2 = xr_arena_strdup(&arena, "second");
    ASSERT_STR_EQ(s2, "second");

    xr_arena_destroy(&arena);
}

/* ========== Growth Tests ========== */

TEST(arena_growth) {
    XrArena arena;
    xr_arena_init(&arena, 64);  // Small initial size

    // Allocate more than initial capacity
    for (int i = 0; i < 100; i++) {
        int *p = (int *) xr_arena_alloc(&arena, sizeof(int));
        ASSERT_NOT_NULL(p);
        *p = i;
    }

    // Should have grown
    ASSERT_TRUE(xr_arena_get_allocated_size(&arena) > 64);

    xr_arena_destroy(&arena);
}

TEST(arena_large_allocation) {
    XrArena arena;
    xr_arena_init(&arena, 1024);

    // Allocate larger than segment size
    size_t large_size = XR_ARENA_SEGMENT_SIZE * 2;
    char *large = (char *) xr_arena_alloc(&arena, large_size);
    ASSERT_NOT_NULL(large);

    // Should be usable
    memset(large, 'A', large_size);
    ASSERT_EQ_INT(large[0], 'A');
    ASSERT_EQ_INT(large[large_size - 1], 'A');

    xr_arena_destroy(&arena);
}

TEST(arena_rejects_oversized_allocation) {
    XrArena arena;
    xr_arena_init(&arena, 1024);

    size_t before = xr_arena_get_allocated_size(&arena);
    ASSERT_NULL(xr_arena_alloc(&arena, SIZE_MAX));
    ASSERT_NULL(xr_arena_alloc_raw(&arena, SIZE_MAX));
    ASSERT_NULL(xr_arena_alloc_array(&arena, 16, SIZE_MAX / 16 + 1));
    ASSERT_EQ_UINT(xr_arena_get_allocated_size(&arena), before);

    xr_arena_destroy(&arena);
}

TEST(arena_rejects_strndup_wrap) {
    XrArena arena;
    xr_arena_init(&arena, 1024);

    char ch = 'x';
    ASSERT_NULL(xr_arena_strndup(&arena, &ch, SIZE_MAX));

    xr_arena_destroy(&arena);
}

/* ========== Alignment Tests ========== */

TEST(arena_alignment) {
    XrArena arena;
    xr_arena_init(&arena, 1024);

    // Allocate various sizes
    char *c = (char *) xr_arena_alloc(&arena, 1);
    int *i = (int *) xr_arena_alloc(&arena, sizeof(int));
    double *d = (double *) xr_arena_alloc(&arena, sizeof(double));

    ASSERT_NOT_NULL(c);
    ASSERT_NOT_NULL(i);
    ASSERT_NOT_NULL(d);

    // Check alignment (pointers should be aligned to XR_ARENA_ALIGNMENT)
    ASSERT_TRUE(((uintptr_t) i % XR_ARENA_ALIGNMENT) == 0);
    ASSERT_TRUE(((uintptr_t) d % XR_ARENA_ALIGNMENT) == 0);

    xr_arena_destroy(&arena);
}

/* ========== Allocated Size Tracking ========== */

TEST(arena_get_allocated_size) {
    XrArena arena;
    xr_arena_init(&arena, 1024);

    size_t size1 = xr_arena_get_allocated_size(&arena);

    xr_arena_alloc(&arena, 100);
    size_t size2 = xr_arena_get_allocated_size(&arena);
    ASSERT_TRUE(size2 > size1);

    xr_arena_alloc(&arena, 200);
    size_t size3 = xr_arena_get_allocated_size(&arena);
    ASSERT_TRUE(size3 > size2);

    xr_arena_destroy(&arena);
}

/* ========== Stress Tests ========== */

TEST(arena_stress_many_small) {
    XrArena arena;
    xr_arena_init(&arena, 1024);

    // Many small allocations
    for (int i = 0; i < 10000; i++) {
        int *p = (int *) xr_arena_alloc(&arena, sizeof(int));
        ASSERT_NOT_NULL(p);
        *p = i;
    }

    xr_arena_destroy(&arena);
}

TEST(arena_stress_mixed_sizes) {
    XrArena arena;
    xr_arena_init(&arena, 1024);

    // Mixed size allocations
    for (int i = 0; i < 1000; i++) {
        size_t size = (i % 100) + 1;  // 1 to 100 bytes
        void *p = xr_arena_alloc(&arena, size);
        ASSERT_NOT_NULL(p);
    }

    xr_arena_destroy(&arena);
}

/* ========== Main ========== */

static void run_all_tests(void) {
    RUN_TEST_SUITE("Basic Operations");
    RUN_TEST(arena_init_destroy);
    RUN_TEST(arena_alloc_single);
    RUN_TEST(arena_alloc_multiple);
    RUN_TEST(arena_alloc_struct);
    RUN_TEST(arena_alloc_array);

    RUN_TEST_SUITE("String Operations");
    RUN_TEST(arena_strdup);
    RUN_TEST(arena_strndup);
    RUN_TEST(arena_strdup_empty);

    RUN_TEST_SUITE("Reset Operations");
    RUN_TEST(arena_reset);
    RUN_TEST(arena_reset_reuse);

    RUN_TEST_SUITE("Growth");
    RUN_TEST(arena_growth);
    RUN_TEST(arena_large_allocation);
    RUN_TEST(arena_rejects_oversized_allocation);
    RUN_TEST(arena_rejects_strndup_wrap);

    RUN_TEST_SUITE("Alignment");
    RUN_TEST(arena_alignment);

    RUN_TEST_SUITE("Allocated Size Tracking");
    RUN_TEST(arena_get_allocated_size);

    RUN_TEST_SUITE("Stress Tests");
    RUN_TEST(arena_stress_many_small);
    RUN_TEST(arena_stress_mixed_sizes);
}

TEST_MAIN_BEGIN()
printf("=== xray Arena Allocator Unit Tests ===\n");
run_all_tests();
TEST_MAIN_END()
