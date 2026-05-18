/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_dynamic_class.c - Unit tests for dynamic-layout XrClass and
 *                       hidden-class transition infrastructure.
 */

#include "../test_framework.h"
#include "../test_helper.h"
#include "base/xmalloc.h"
#include "runtime/class/xclass.h"
#include "runtime/class/xinstance.h"
#include "runtime/symbol/xsymbol_table.h"
#include "runtime/xisolate_api.h"

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

/* ========== Helpers ========== */

// Allocate a minimal dynamic-layout root class with the given in-object
// capacity. Field list starts empty; transitions populate it.
static XrClass *make_dynamic_root(uint16_t capacity, bool sealed) {
    XrClass *cls = (XrClass *) xr_calloc(1, sizeof(XrClass));
    if (!cls)
        return NULL;
    cls->name = "DynRoot";
    cls->flags = XR_CLASS_DYNAMIC_LAYOUT | (sealed ? XR_CLASS_DYNAMIC_SEALED : 0);
    cls->in_object_capacity = capacity;
    cls->field_count = 0;
    cls->own_field_count = 0;
    cls->field_map_capacity = 0;
    return cls;
}

// Bare unit tests don't initialize the symbol table, so just use
// distinct non-zero integers. The transition logic is symbol-agnostic.
static int sym(const char *name) {
    static int next_sym = 1;
    static const char *names[16] = {0};
    static int ids[16] = {0};
    for (int i = 0; i < 16 && names[i]; i++) {
        if (strcmp(names[i], name) == 0)
            return ids[i];
    }
    int id = next_sym++;
    for (int i = 0; i < 16; i++) {
        if (!names[i]) {
            names[i] = name;
            ids[i] = id;
            break;
        }
    }
    return id;
}

/* ========== Transition Tests ========== */

TEST(transition_create_first_field) {
    setup();
    XrClass *root = make_dynamic_root(8, false);
    int s_a = sym("a");

    XrClass *child = xr_class_transition_get_or_create(X, root, s_a, "a");
    ASSERT_NOT_NULL(child);
    ASSERT_EQ_INT(child->field_count, 1);
    ASSERT_EQ_INT(child->transition_parent == root, 1);
    ASSERT_EQ_INT(child->transition_symbol, s_a);
    ASSERT_EQ_INT(child->in_object_capacity, 8);

    // Look up the new field
    int idx = xr_class_lookup_field(child, s_a);
    ASSERT_EQ_INT(idx, 0);
    teardown();
}

TEST(transition_idempotent) {
    setup();
    XrClass *root = make_dynamic_root(8, false);
    int s_a = sym("a");

    XrClass *c1 = xr_class_transition_get_or_create(X, root, s_a, "a");
    XrClass *c2 = xr_class_transition_get_or_create(X, root, s_a, "a");
    ASSERT_EQ_INT(c1 == c2, 1);  // Same field symbol → same target class
    teardown();
}

TEST(transition_chain) {
    setup();
    XrClass *root = make_dynamic_root(8, false);
    int s_a = sym("a"), s_b = sym("b"), s_c = sym("c");

    XrClass *c1 = xr_class_transition_get_or_create(X, root, s_a, "a");
    XrClass *c2 = xr_class_transition_get_or_create(X, c1, s_b, "b");
    XrClass *c3 = xr_class_transition_get_or_create(X, c2, s_c, "c");

    ASSERT_EQ_INT(c3->field_count, 3);
    ASSERT_EQ_INT(xr_class_lookup_field(c3, s_a), 0);
    ASSERT_EQ_INT(xr_class_lookup_field(c3, s_b), 1);
    ASSERT_EQ_INT(xr_class_lookup_field(c3, s_c), 2);
    teardown();
}

TEST(transition_sealed_rejects) {
    setup();
    XrClass *root = make_dynamic_root(8, true);  // Sealed
    int s_a = sym("a");

    XrClass *child = xr_class_transition_get_or_create(X, root, s_a, "a");
    ASSERT_EQ_INT(child == NULL, 1);  // Sealed root rejects new field
    teardown();
}

/* ========== Field Access Tests ========== */

TEST(field_inobject_get_set) {
    setup();
    XrClass *root = make_dynamic_root(8, false);
    int s_a = sym("a"), s_b = sym("b");
    XrClass *c1 = xr_class_transition_get_or_create(X, root, s_a, "a");
    XrClass *c2 = xr_class_transition_get_or_create(X, c1, s_b, "b");

    XrInstance *inst = xr_instance_new(X, c2);
    ASSERT_NOT_NULL(inst);

    ASSERT_TRUE(xr_instance_set_dynamic_field(X, inst, 0, XR_FROM_INT(42)));
    ASSERT_TRUE(xr_instance_set_dynamic_field(X, inst, 1, XR_FROM_INT(99)));

    XrValue v0 = xr_instance_get_dynamic_field(inst, 0);
    XrValue v1 = xr_instance_get_dynamic_field(inst, 1);
    ASSERT_EQ_INT(XR_TO_INT(v0), 42);
    ASSERT_EQ_INT(XR_TO_INT(v1), 99);
    teardown();
}

TEST(field_overflow_growth) {
    setup();
    // capacity=4 → in-object slots [0..2], slot[3] = overflow ptr.
    XrClass *root = make_dynamic_root(4, false);
    XrClass *cls = root;
    // Build a class with 10 fields to force overflow
    char name[8];
    for (int i = 0; i < 10; i++) {
        snprintf(name, sizeof(name), "f%d", i);
        cls = xr_class_transition_get_or_create(X, cls, sym(name), name);
        ASSERT_NOT_NULL(cls);
    }
    ASSERT_EQ_INT(cls->field_count, 10);

    XrInstance *inst = xr_instance_new(X, cls);
    ASSERT_NOT_NULL(inst);

    // Set and verify all 10 fields (3 in-object + 7 overflow)
    for (uint16_t i = 0; i < 10; i++) {
        ASSERT_TRUE(xr_instance_set_dynamic_field(X, inst, i, XR_FROM_INT(i * 100)));
    }
    for (uint16_t i = 0; i < 10; i++) {
        XrValue v = xr_instance_get_dynamic_field(inst, i);
        ASSERT_EQ_INT(XR_TO_INT(v), i * 100);
    }
    teardown();
}

TEST(field_missing_overflow_reads_null) {
    setup();
    XrClass *root = make_dynamic_root(4, false);
    int s_a = sym("a");
    XrClass *c1 = xr_class_transition_get_or_create(X, root, s_a, "a");

    XrInstance *inst = xr_instance_new(X, c1);
    ASSERT_NOT_NULL(inst);

    // Field 0 (in-object) defaults to null
    XrValue v = xr_instance_get_dynamic_field(inst, 0);
    ASSERT_TRUE(XR_IS_NULL(v));
    teardown();
}

/* ========== Run All ========== */

static void run_all_tests(void) {
    RUN_TEST_SUITE("Dynamic Class Transition");
    RUN_TEST(transition_create_first_field);
    RUN_TEST(transition_idempotent);
    RUN_TEST(transition_chain);
    RUN_TEST(transition_sealed_rejects);

    RUN_TEST_SUITE("Dynamic Field Access");
    RUN_TEST(field_inobject_get_set);
    RUN_TEST(field_overflow_growth);
    RUN_TEST(field_missing_overflow_reads_null);
}

TEST_MAIN_BEGIN()
printf("=== xray Dynamic-Layout Class Unit Tests ===\n");
run_all_tests();
TEST_MAIN_END()
