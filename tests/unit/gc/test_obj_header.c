/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_obj_header.c - Unit tests for the unified object header dup/drop
 * primitives (xgc_header.h). Verifies refcount arithmetic, region no-op,
 * atomic mode, and last-reference detection.
 */

#include "../../../src/runtime/gc/xgc_header.h"
#include <stdio.h>

static int g_passed = 0;
static int g_failed = 0;

#define ASSERT_EQ(actual, expected, msg)                                                           \
    do {                                                                                           \
        if ((long) (actual) != (long) (expected)) {                                                \
            fprintf(stderr, "  FAIL: %s (got %ld, expected %ld)\n", msg, (long) (actual),          \
                    (long) (expected));                                                            \
            g_failed++;                                                                            \
        } else {                                                                                   \
            g_passed++;                                                                            \
        }                                                                                          \
    } while (0)

#define ASSERT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL: %s\n", msg);                                                  \
            g_failed++;                                                                            \
        } else {                                                                                   \
            g_passed++;                                                                            \
        }                                                                                          \
    } while (0)

static void test_header_size(void) {
    ASSERT_EQ(sizeof(XrObjHeader), 16, "XrObjHeader is 16 bytes");
    ASSERT_EQ(sizeof(XrGCHeader), sizeof(XrObjHeader), "XrGCHeader aliases XrObjHeader");
}

/* Plain (non-region, non-atomic) dup/drop: 1-based refcount. */
static void test_plain_dup_drop(void) {
    XrObjHeader o = {0};
    o.refcount = 1; /* one owner */

    xr_obj_dup(&o);
    ASSERT_EQ(o.refcount, 2, "dup increments refcount");
    xr_obj_dup(&o);
    ASSERT_EQ(o.refcount, 3, "dup increments again");

    ASSERT_TRUE(!xr_obj_drop_is_last(&o), "drop from 3 not last");
    ASSERT_EQ(o.refcount, 2, "drop decrements to 2");
    ASSERT_TRUE(!xr_obj_drop_is_last(&o), "drop from 2 not last");
    ASSERT_EQ(o.refcount, 1, "drop decrements to 1");
    ASSERT_TRUE(xr_obj_drop_is_last(&o), "drop from 1 IS last");
    ASSERT_EQ(o.refcount, 0, "refcount reaches 0");
}

/* Region objects: dup/drop are no-ops, never report last. */
static void test_region_noop(void) {
    XrObjHeader o = {0};
    o.refcount = 1;
    XR_OBJ_SET_FLAG(&o, XR_OBJ_REGION);

    xr_obj_dup(&o);
    ASSERT_EQ(o.refcount, 1, "region dup is no-op");
    ASSERT_TRUE(!xr_obj_drop_is_last(&o), "region drop never last");
    ASSERT_EQ(o.refcount, 1, "region drop is no-op");
    ASSERT_TRUE(XR_OBJ_IS_REGION(&o), "region flag set");
}

/* Atomic objects: refcount adjusted atomically, same last-ref semantics. */
static void test_atomic_dup_drop(void) {
    XrObjHeader o = {0};
    o.refcount = 1;
    XR_OBJ_SET_FLAG(&o, XR_OBJ_ATOMIC);

    xr_obj_dup(&o);
    ASSERT_EQ(o.refcount, 2, "atomic dup increments");
    ASSERT_TRUE(!xr_obj_drop_is_last(&o), "atomic drop from 2 not last");
    ASSERT_TRUE(xr_obj_drop_is_last(&o), "atomic drop from 1 IS last");
    ASSERT_EQ(o.refcount, 0, "atomic refcount reaches 0");
    ASSERT_TRUE(XR_OBJ_IS_ATOMIC(&o), "atomic flag set");
}

/* NULL safety. */
static void test_null_safe(void) {
    xr_obj_dup(NULL);
    ASSERT_TRUE(!xr_obj_drop_is_last(NULL), "drop(NULL) is false");
    ASSERT_TRUE(true, "NULL dup/drop did not crash");
}

/* Flags are independent and do not corrupt refcount/objsize. */
static void test_flag_independence(void) {
    XrObjHeader o = {0};
    o.refcount = 5;
    o.objsize = 128;
    XR_OBJ_SET_FLAG(&o, XR_OBJ_HAS_DTOR);
    XR_OBJ_SET_FLAG(&o, XR_OBJ_WEAKABLE);

    ASSERT_TRUE(XR_OBJ_HAS_DESTRUCTOR(&o), "HAS_DTOR set");
    ASSERT_TRUE(XR_OBJ_GET_FLAG(&o, XR_OBJ_WEAKABLE), "WEAKABLE set");
    ASSERT_TRUE(!XR_OBJ_IS_REGION(&o), "REGION not set");
    ASSERT_EQ(o.refcount, 5, "refcount intact");
    ASSERT_EQ(o.objsize, 128, "objsize intact");

    XR_OBJ_CLEAR_FLAG(&o, XR_OBJ_HAS_DTOR);
    ASSERT_TRUE(!XR_OBJ_HAS_DESTRUCTOR(&o), "HAS_DTOR cleared");
    ASSERT_TRUE(XR_OBJ_GET_FLAG(&o, XR_OBJ_WEAKABLE), "WEAKABLE still set");
}

int main(void) {
    test_header_size();
    test_plain_dup_drop();
    test_region_noop();
    test_atomic_dup_drop();
    test_null_safe();
    test_flag_independence();

    printf("\n=== test_obj_header: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
