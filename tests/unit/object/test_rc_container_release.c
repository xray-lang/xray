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
#include "runtime/closure/xcell.h"
#include "runtime/closure/xclosure.h"
#include "runtime/gc/xalloc_unified.h"
#include "runtime/gc/xgc.h"
#include "runtime/gc/xcoro_gc.h"
#include "runtime/object/xarray.h"
#include "runtime/object/xmap.h"
#include "runtime/object/xset.h"
#include "runtime/object/xtuple.h"
#include "runtime/value/xchunk.h"

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

static bool is_dead(XrObjHeader *obj) {
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

static XrProto *make_proto_with_upvalues(int count) {
    XrProto *proto = xr_vm_proto_new();
    if (!proto)
        return NULL;
    for (int i = 0; i < count; i++) {
        UpvalInfo uv = {0};
        uv.source = UPVAL_SRC_REG;
        DYNARRAY_ADD(&proto->upvalues, uv, UpvalInfo);
    }
    return proto;
}

TEST(array_destroy_releases_child_array) {
    setup();
    XrArray *parent = xr_array_new(main_coro);
    XrArray *child = xr_array_new(main_coro);

    xr_array_push(parent, xr_value_from_array(child));
    /* 0-based RC: a uniquely-owned child reads 0 (push transfers ownership,
     * it does not retain). */
    ASSERT_EQ_INT(child->hdr.refcount, 0);

    XrCoroGC *gc = test_gc();
    ASSERT_NOT_NULL(gc);
    xr_rc_release_value(gc, xr_value_from_array(parent));
    ASSERT_TRUE(is_dead(&parent->hdr));
    ASSERT_TRUE(is_dead(&child->hdr));
    teardown();
}

TEST(map_destroy_releases_key_and_value) {
    setup();
    XrMap *map = xr_map_new(main_coro);
    XrArray *key = xr_array_new(main_coro);
    XrArray *value = xr_array_new(main_coro);

    xr_map_set(map, xr_value_from_array(key), xr_value_from_array(value));
    ASSERT_EQ_INT(key->hdr.refcount, 0);
    ASSERT_EQ_INT(value->hdr.refcount, 0);

    XrCoroGC *gc = test_gc();
    ASSERT_NOT_NULL(gc);
    xr_rc_release_value(gc, xr_value_from_map(map));
    ASSERT_TRUE(is_dead(&map->hdr));
    ASSERT_TRUE(is_dead(&key->hdr));
    ASSERT_TRUE(is_dead(&value->hdr));
    teardown();
}

TEST(set_destroy_releases_value) {
    setup();
    XrSet *set = xr_set_new(main_coro);
    XrArray *child = xr_array_new(main_coro);

    ASSERT_TRUE(xr_set_add(set, xr_value_from_array(child)));
    ASSERT_EQ_INT(child->hdr.refcount, 0);

    XrCoroGC *gc = test_gc();
    ASSERT_NOT_NULL(gc);
    xr_rc_release_value(gc, xr_value_from_set(set));
    ASSERT_TRUE(is_dead(&set->hdr));
    ASSERT_TRUE(is_dead(&child->hdr));
    teardown();
}

TEST(weak_map_does_not_retain_key_and_purges_value) {
    setup();
    XrMap *map = xr_map_new(main_coro);
    map->flags |= XR_MAP_FLAG_WEAK;
    XrArray *key = xr_array_new(main_coro);
    XrArray *value = xr_array_new(main_coro);

    /* Simulate language-call ownership: the local `key` owns one reference and
     * the WeakMap.set argument owns a temporary duplicate. WeakMap must consume
     * that duplicate without keeping the key alive. */
    xr_rc_retain_value(xr_value_from_array(key));
    xr_map_set(map, xr_value_from_array(key), xr_value_from_array(value));
    ASSERT_EQ_INT(key->hdr.refcount, 0);
    ASSERT_EQ_INT(map->count, 1);
    ASSERT_FALSE(is_dead(&key->hdr));
    ASSERT_FALSE(is_dead(&value->hdr));

    XrCoroGC *gc = test_gc();
    ASSERT_NOT_NULL(gc);
    xr_rc_release_value(gc, xr_value_from_array(key));
    ASSERT_TRUE(is_dead(&key->hdr));
    ASSERT_TRUE(is_dead(&value->hdr));
    ASSERT_EQ_INT(map->count, 0);

    xr_rc_release_value(gc, xr_value_from_map(map));
    ASSERT_TRUE(is_dead(&map->hdr));
    teardown();
}

TEST(weak_set_does_not_retain_element) {
    setup();
    XrSet *set = xr_set_new(main_coro);
    set->flags |= XR_SET_FLAG_WEAK;
    XrArray *elem = xr_array_new(main_coro);

    xr_rc_retain_value(xr_value_from_array(elem));
    ASSERT_TRUE(xr_set_add(set, xr_value_from_array(elem)));
    ASSERT_EQ_INT(elem->hdr.refcount, 0);
    ASSERT_EQ_INT(set->count, 1);
    ASSERT_FALSE(is_dead(&elem->hdr));

    XrCoroGC *gc = test_gc();
    ASSERT_NOT_NULL(gc);
    xr_rc_release_value(gc, xr_value_from_array(elem));
    ASSERT_TRUE(is_dead(&elem->hdr));
    ASSERT_EQ_INT(set->count, 0);

    xr_rc_release_value(gc, xr_value_from_set(set));
    ASSERT_TRUE(is_dead(&set->hdr));
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
    ASSERT_TRUE(is_dead(&tuple->hdr));
    ASSERT_TRUE(is_dead(&left->hdr));
    ASSERT_TRUE(is_dead(&right->hdr));
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
    ASSERT_TRUE(is_dead(&inst->hdr));
    ASSERT_TRUE(is_dead(&a->hdr));
    ASSERT_TRUE(is_dead(&b->hdr));
    ASSERT_TRUE(is_dead(&c->hdr));
    teardown();
}

TEST(closure_destroy_releases_upvals) {
    setup();
    XrProto *proto = make_proto_with_upvalues(1);
    ASSERT_NOT_NULL(proto);
    XrClosure *closure = xr_closure_new(X, proto, main_coro);
    XrArray *child = xr_array_new(main_coro);
    ASSERT_NOT_NULL(closure);
    ASSERT_NOT_NULL(child);

    closure->upvals[0] = xr_value_from_array(child);

    XrCoroGC *gc = test_gc();
    ASSERT_NOT_NULL(gc);
    xr_rc_release_value(gc, xr_value_from_closure(closure));
    ASSERT_TRUE(is_dead(&closure->hdr));
    ASSERT_TRUE(is_dead(&child->hdr));

    xr_vm_proto_free(proto);
    teardown();
}

TEST(cell_destroy_and_replace_release_values) {
    setup();
    XrCell *cell = xr_cell_new(X, main_coro);
    XrArray *first = xr_array_new(main_coro);
    XrArray *second = xr_array_new(main_coro);
    ASSERT_NOT_NULL(cell);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);

    cell->value = xr_value_from_array(first);
    XrCoroGC *gc = test_gc();
    ASSERT_NOT_NULL(gc);

    XrValue old = cell->value;
    cell->value = xr_value_from_array(second);
    xr_rc_release_value(gc, old);
    ASSERT_TRUE(is_dead(&first->hdr));
    ASSERT_FALSE(is_dead(&second->hdr));

    xr_rc_release_value(gc, XR_FROM_PTR(cell));
    ASSERT_TRUE(is_dead(&cell->hdr));
    ASSERT_TRUE(is_dead(&second->hdr));
    teardown();
}

TEST(closure_cell_cycle_is_collected) {
    setup();
    XrProto *proto = make_proto_with_upvalues(1);
    ASSERT_NOT_NULL(proto);
    XrClosure *closure = xr_closure_new(X, proto, main_coro);
    XrCell *cell = xr_cell_new(X, main_coro);
    ASSERT_NOT_NULL(closure);
    ASSERT_NOT_NULL(cell);

    XrValue closure_value = xr_value_from_closure(closure);
    XrValue cell_value = XR_FROM_PTR(cell);
    xr_rc_retain_value(cell_value);
    closure->upvals[0] = cell_value;
    xr_rc_retain_value(closure_value);
    cell->value = closure_value;

    XrCoroGC *gc = test_gc();
    ASSERT_NOT_NULL(gc);
    ASSERT_EQ_INT(closure->hdr.refcount, 1);
    ASSERT_EQ_INT(cell->hdr.refcount, 1);

    xr_rc_release_value(gc, closure_value);
    xr_rc_release_value(gc, cell_value);
    ASSERT_FALSE(is_dead(&closure->hdr));
    ASSERT_FALSE(is_dead(&cell->hdr));
    ASSERT_EQ_INT(closure->hdr.refcount, 0);
    ASSERT_EQ_INT(cell->hdr.refcount, 0);
    ASSERT_TRUE(gc->cycle_root_count >= 2);

    xr_coro_gc_fullgc(gc);
    ASSERT_TRUE(is_dead(&closure->hdr));
    ASSERT_TRUE(is_dead(&cell->hdr));

    xr_vm_proto_free(proto);
    teardown();
}

TEST(whole_block_reclaim_returns_empty_blocks) {
    setup();
    XrCoroGC *gc = test_gc();
    ASSERT_NOT_NULL(gc);

    /* Fill several Region blocks with freelistable blobs (256B → ~63 per 16KB
     * block; 256 objects span ~4 blocks), then free every one. */
    enum {
        N = 256,
        SZ = 256
    };
    XrObjHeader *objs[N];
    for (int i = 0; i < N; i++) {
        objs[i] = xr_coro_gc_newobj(gc, XR_TBLOB, SZ);
        ASSERT_NOT_NULL(objs[i]);
    }

    XrRegionStats before;
    xr_region_get_stats(&gc->region, &before);
    ASSERT_TRUE(before.full_blocks >= 1);

    for (int i = 0; i < N; i++)
        xr_coro_gc_rc_destroy(gc, objs[i]);

    /* Reclaim: fully-dead retired blocks return to the free pool so memory is
     * reusable by ANY size class (bounds peak retention under shifting loads). */
    xr_coro_gc_reclaim_blocks(gc);

    XrRegionStats after;
    xr_region_get_stats(&gc->region, &after);
    ASSERT_TRUE(after.free_blocks > before.free_blocks);
    ASSERT_TRUE(after.full_blocks < before.full_blocks);
    /* Memory kept for reuse (not returned to OS), so total is unchanged. */
    ASSERT_TRUE(after.total_blocks == before.total_blocks);

    /* A later allocation of a DIFFERENT size class reuses the reclaimed
     * blocks without growing the heap. */
    size_t total_after = after.total_blocks;
    XrObjHeader *reuse = xr_coro_gc_newobj(gc, XR_TBLOB, 512);
    ASSERT_NOT_NULL(reuse);
    XrRegionStats reused;
    xr_region_get_stats(&gc->region, &reused);
    ASSERT_TRUE(reused.total_blocks <= total_after);

    teardown();
}

int main(void) {
    printf("\n=== RC Container Release Tests ===\n");
    RUN_TEST(array_destroy_releases_child_array);
    RUN_TEST(map_destroy_releases_key_and_value);
    RUN_TEST(set_destroy_releases_value);
    RUN_TEST(weak_map_does_not_retain_key_and_purges_value);
    RUN_TEST(weak_set_does_not_retain_element);
    RUN_TEST(tuple_instance_destroy_releases_elements);
    RUN_TEST(dynamic_instance_destroy_releases_overflow_fields);
    RUN_TEST(closure_destroy_releases_upvals);
    RUN_TEST(cell_destroy_and_replace_release_values);
    RUN_TEST(closure_cell_cycle_is_collected);
    RUN_TEST(whole_block_reclaim_returns_empty_blocks);
    TEST_REPORT();
    return TEST_EXIT();
}
