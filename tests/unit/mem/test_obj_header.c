/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_obj_header.c - Unit tests for the unified object header dup/drop
 * primitives (xobj_header.h). Verifies refcount arithmetic, region no-op,
 * atomic mode, and last-reference detection.
 */

#include "../../../src/runtime/mem/xobj_header.h"
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
}

/* Plain thread-local dup/drop: 0-based refcount (rc == 0 means one owner). */
static void test_plain_dup_drop(void) {
    XrObjHeader o = {0};
    o.refcount = XR_RC_INIT; /* one owner == unique == 0 */

    ASSERT_TRUE(xr_obj_is_unique(&o), "fresh object is unique");
    xr_obj_dup(&o);
    ASSERT_EQ(o.refcount, 1, "dup increments refcount (2 refs)");
    ASSERT_TRUE(!xr_obj_is_unique(&o), "no longer unique after dup");
    xr_obj_dup(&o);
    ASSERT_EQ(o.refcount, 2, "dup increments again (3 refs)");

    ASSERT_TRUE(!xr_obj_drop_is_last(&o), "drop from 3 refs not last");
    ASSERT_EQ(o.refcount, 1, "drop decrements to 2 refs");
    ASSERT_TRUE(!xr_obj_drop_is_last(&o), "drop from 2 refs not last");
    ASSERT_EQ(o.refcount, 0, "drop decrements to 1 ref (unique)");
    ASSERT_TRUE(xr_obj_drop_is_last(&o), "drop from unique IS last");
}

/* Immortal (sticky) objects: dup/drop are no-ops, never report last. The
 * sign-tagged sticky refcount — not a flag — drives the no-op. */
static void test_sticky_noop(void) {
    XrObjHeader o = {0};
    o.refcount = XR_RC_STICKY;

    xr_obj_dup(&o);
    ASSERT_EQ(o.refcount, XR_RC_STICKY, "sticky dup is no-op");
    ASSERT_TRUE(!xr_obj_drop_is_last(&o), "sticky drop never last");
    ASSERT_EQ(o.refcount, XR_RC_STICKY, "sticky drop is no-op");
    ASSERT_TRUE(!xr_obj_is_unique(&o), "sticky object is not unique");
}

/* AOT bump objects are bulk-freed by their arena. They must remain invisible to
 * VM/JIT RC, even if a stale or external producer gives them rc==0. */
static void test_bump_storage_noop(void) {
    XrObjHeader o = {0};
    o.refcount = XR_RC_INIT;
    XR_OBJ_SET_FLAG(&o, XR_OBJ_STORAGE_BUMP);

    xr_obj_dup(&o);
    ASSERT_EQ(o.refcount, XR_RC_INIT, "bump dup is no-op");
    ASSERT_TRUE(!xr_obj_drop_is_last(&o), "bump drop never last");
    ASSERT_EQ(o.refcount, XR_RC_INIT, "bump drop is no-op");
    ASSERT_TRUE(!xr_obj_is_unique(&o), "bump object is not unique");
}

/* Atomic (thread-shared) objects: negative encoding, references = -rc.
 * Last-ref detection still works through the cold path. */
static void test_atomic_dup_drop(void) {
    XrObjHeader o = {0};
    o.refcount = -1; /* one shared reference */
    XR_OBJ_SET_FLAG(&o, XR_OBJ_ATOMIC);

    xr_obj_dup(&o);
    ASSERT_EQ(o.refcount, -2, "atomic dup → more negative (2 refs)");
    ASSERT_TRUE(!xr_obj_drop_is_last(&o), "atomic drop from 2 refs not last");
    ASSERT_EQ(o.refcount, -1, "atomic drop → toward zero (1 ref)");
    ASSERT_TRUE(xr_obj_drop_is_last(&o), "atomic drop from 1 ref IS last");
    ASSERT_EQ(o.refcount, 0, "atomic refcount reaches 0 on last drop");
    ASSERT_TRUE(XR_OBJ_IS_ATOMIC(&o), "atomic flag set");
}

/* Managed objects (Channel/Coroutine/...): the runtime owns their lifetime,
 * so the compiler-inserted dup/drop are no-ops even though they sit in the
 * atomic band (the runtime counts them separately via xshared.h). */
static void test_managed_noop(void) {
    XrObjHeader o = {0};
    o.refcount = -1; /* runtime holds one reference */
    XR_OBJ_SET_FLAG(&o, XR_OBJ_ATOMIC);
    XR_OBJ_SET_FLAG(&o, XR_OBJ_MANAGED);

    xr_obj_dup(&o);
    ASSERT_EQ(o.refcount, -1, "managed dup is no-op (runtime-owned)");
    ASSERT_TRUE(!xr_obj_drop_is_last(&o), "managed drop never last");
    ASSERT_EQ(o.refcount, -1, "managed drop is no-op");
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
    test_sticky_noop();
    test_bump_storage_noop();
    test_atomic_dup_drop();
    test_managed_noop();
    test_null_safe();
    test_flag_independence();

    printf("\n=== test_obj_header: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
