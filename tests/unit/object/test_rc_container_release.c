/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_rc_container_release.c - Unit tests for container-owned RC child release.
 */

#include "../test_framework.h"
#include "../test_helper.h"
#include "runtime/class/xclass.h"
#include "runtime/class/xinstance.h"
#include "runtime/gc/xalloc_unified.h"
#include "runtime/gc/xgc.h"
#include "runtime/object/xarray.h"
#include "runtime/object/xmap.h"
#include "runtime/object/xset.h"
#include "runtime/object/xtuple.h"

static XrayIsolate *X = NULL;
static XrCoroutine *main_coro = NULL;

static void setup(void) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    X = xray_isolate_new(&params);
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

static XrCoroGC *test_gc(void) {
    XrCoroGC *gc = xr_coro_get_coro_gc(main_coro);
    return gc;
}

static bool is_dead(XrGCHeader *obj) {
    return obj && (obj->extra & XR_OBJ_DEAD);
}

static XrClass *make_dynamic_root(uint16_t capacity) {
    XrClass *cls = (XrClass *) xr_calloc(1, sizeof(XrClass));
    if (!cls)
        return NULL;
    cls->name = "RcDynRoot";
    cls->flags = XR_CLASS_DYNAMIC_LAYOUT;
    cls->in_object_capacity = capacity;
    return cls;
}

TEST(array_destroy_releases_child_array) {
    setup();
    XrArray *parent = xr_array_new(main_coro);
    XrArray *child = xr_array_new(main_coro);

    xr_array_push(parent, xr_value_from_array(child));
    /* 0-based RC: a uniquely-owned child reads 0 (push transfers ownership,
     * it does not retain). */
    ASSERT_EQ_INT(child->gc.refcount, 0);

    XrCoroGC *gc = test_gc();
    ASSERT_NOT_NULL(gc);
    xr_rc_release_value(gc, xr_value_from_array(parent));
    ASSERT_TRUE(is_dead(&parent->gc));
    ASSERT_TRUE(is_dead(&child->gc));
    teardown();
}

TEST(map_destroy_releases_key_and_value) {
    setup();
    XrMap *map = xr_map_new(main_coro);
    XrArray *key = xr_array_new(main_coro);
    XrArray *value = xr_array_new(main_coro);

    xr_map_set(map, xr_value_from_array(key), xr_value_from_array(value));
    ASSERT_EQ_INT(key->gc.refcount, 0);
    ASSERT_EQ_INT(value->gc.refcount, 0);

    XrCoroGC *gc = test_gc();
    ASSERT_NOT_NULL(gc);
    xr_rc_release_value(gc, xr_value_from_map(map));
    ASSERT_TRUE(is_dead(&map->gc));
    ASSERT_TRUE(is_dead(&key->gc));
    ASSERT_TRUE(is_dead(&value->gc));
    teardown();
}

TEST(set_destroy_releases_value) {
    setup();
    XrSet *set = xr_set_new(main_coro);
    XrArray *child = xr_array_new(main_coro);

    ASSERT_TRUE(xr_set_add(set, xr_value_from_array(child)));
    ASSERT_EQ_INT(child->gc.refcount, 0);

    XrCoroGC *gc = test_gc();
    ASSERT_NOT_NULL(gc);
    xr_rc_release_value(gc, xr_value_from_set(set));
    ASSERT_TRUE(is_dead(&set->gc));
    ASSERT_TRUE(is_dead(&child->gc));
    teardown();
}

TEST(tuple_instance_destroy_releases_elements) {
    setup();
    XrTuple *tuple = xr_tuple_new(main_coro, 2);
    XrArray *left = xr_array_new(main_coro);
    XrArray *right = xr_array_new(main_coro);

    xr_tuple_set(tuple, 0, xr_value_from_array(left));
    xr_tuple_set(tuple, 1, xr_value_from_array(right));

    XrCoroGC *gc = test_gc();
    ASSERT_NOT_NULL(gc);
    xr_rc_release_value(gc, xr_value_from_tuple(tuple));
    ASSERT_TRUE(is_dead(&tuple->gc));
    ASSERT_TRUE(is_dead(&left->gc));
    ASSERT_TRUE(is_dead(&right->gc));
    teardown();
}

TEST(dynamic_instance_destroy_releases_overflow_fields) {
    setup();
    XrClass *root = make_dynamic_root(2);
    XrClass *c1 = xr_class_transition_get_or_create(X, root, 1, "a");
    XrClass *c2 = xr_class_transition_get_or_create(X, c1, 2, "b");
    XrClass *c3 = xr_class_transition_get_or_create(X, c2, 3, "c");
    XrInstance *inst = xr_instance_new(X, c3);
    XrArray *a = xr_array_new(main_coro);
    XrArray *b = xr_array_new(main_coro);
    XrArray *c = xr_array_new(main_coro);

    ASSERT_TRUE(xr_instance_set_dynamic_field(X, inst, 0, xr_value_from_array(a)));
    ASSERT_TRUE(xr_instance_set_dynamic_field(X, inst, 1, xr_value_from_array(b)));
    ASSERT_TRUE(xr_instance_set_dynamic_field(X, inst, 2, xr_value_from_array(c)));

    XrCoroGC *gc = test_gc();
    ASSERT_NOT_NULL(gc);
    xr_rc_release_value(gc, XR_FROM_PTR(inst));
    ASSERT_TRUE(is_dead(&inst->gc));
    ASSERT_TRUE(is_dead(&a->gc));
    ASSERT_TRUE(is_dead(&b->gc));
    ASSERT_TRUE(is_dead(&c->gc));
    teardown();
}

TEST(whole_block_reclaim_returns_empty_blocks) {
    setup();
    XrCoroGC *gc = test_gc();
    ASSERT_NOT_NULL(gc);

    /* Fill several Immix blocks with freelistable blobs (256B → ~63 per 16KB
     * block; 256 objects span ~4 blocks), then free every one. */
    enum {
        N = 256,
        SZ = 256
    };
    XrGCHeader *objs[N];
    for (int i = 0; i < N; i++) {
        objs[i] = xr_coro_gc_newobj(gc, XR_TBLOB, SZ);
        ASSERT_NOT_NULL(objs[i]);
    }

    XrImmixStats before;
    xr_immix_get_stats(&gc->immix, &before);
    ASSERT_TRUE(before.full_blocks >= 1);

    for (int i = 0; i < N; i++)
        xr_coro_gc_rc_destroy(gc, objs[i]);

    /* Reclaim: fully-dead retired blocks return to the free pool so memory is
     * reusable by ANY size class (bounds peak retention under shifting loads). */
    xr_coro_gc_reclaim_blocks(gc);

    XrImmixStats after;
    xr_immix_get_stats(&gc->immix, &after);
    ASSERT_TRUE(after.free_blocks > before.free_blocks);
    ASSERT_TRUE(after.full_blocks < before.full_blocks);
    /* Memory kept for reuse (not returned to OS), so total is unchanged. */
    ASSERT_TRUE(after.total_blocks == before.total_blocks);

    /* A later allocation of a DIFFERENT size class reuses the reclaimed
     * blocks without growing the heap. */
    size_t total_after = after.total_blocks;
    XrGCHeader *reuse = xr_coro_gc_newobj(gc, XR_TBLOB, 512);
    ASSERT_NOT_NULL(reuse);
    XrImmixStats reused;
    xr_immix_get_stats(&gc->immix, &reused);
    ASSERT_TRUE(reused.total_blocks <= total_after);

    teardown();
}

int main(void) {
    printf("\n=== RC Container Release Tests ===\n");
    RUN_TEST(array_destroy_releases_child_array);
    RUN_TEST(map_destroy_releases_key_and_value);
    RUN_TEST(set_destroy_releases_value);
    RUN_TEST(tuple_instance_destroy_releases_elements);
    RUN_TEST(dynamic_instance_destroy_releases_overflow_fields);
    RUN_TEST(whole_block_reclaim_returns_empty_blocks);
    TEST_REPORT();
    return TEST_EXIT();
}
