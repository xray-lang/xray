/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xtuple.c - Unit tests for the heap-allocated tuple object and
 *                 the matching type-system rules.
 */

#include "../test_framework.h"
#include "../test_helper.h"
#include "runtime/object/xtuple.h"
#include "runtime/value/xtype.h"
#include "runtime/value/xtype_pool.h"

static XrayIsolate *X = NULL;
static XrCoroutine *main_coro = NULL;
static XrTypePool *type_pool = NULL;

/* ========== Setup / Teardown ========== */

static void setup(void) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    X = xray_isolate_new(&params);
    ASSERT_NOT_NULL(X);
    main_coro = xr_test_init_coro(X);
    ASSERT_NOT_NULL(main_coro);
    /* Type-system entry points read the active pool from a thread-
     * local slot, and the singletons (int / float / string / unit / ...)
     * are populated by xr_type_global_init. Both are normally driven
     * by the analyzer/CLI entry points, not by xray_isolate_new. */
    xr_type_global_init();
    type_pool = xr_type_pool_new();
    ASSERT_NOT_NULL(type_pool);
    xr_type_set_current_pool(type_pool, &type_pool->next_type_id);
}

static void teardown(void) {
    if (type_pool) {
        xr_type_set_current_pool(NULL, NULL);
        xr_type_pool_free(type_pool);
        type_pool = NULL;
    }
    if (X) {
        xray_isolate_delete(X);
        X = NULL;
        main_coro = NULL;
    }
}

/* ========== Object Creation ========== */

TEST(tuple_new_zero_arity) {
    setup();
    XrTuple *t = xr_tuple_new(main_coro, 0);
    ASSERT_NOT_NULL(t);
    ASSERT_EQ_INT(xr_tuple_arity(t), 0);
    teardown();
}

TEST(tuple_new_default_null_elements) {
    setup();
    XrTuple *t = xr_tuple_new(main_coro, 3);
    ASSERT_NOT_NULL(t);
    ASSERT_EQ_INT(xr_tuple_arity(t), 3);
    for (uint16_t i = 0; i < 3; i++) {
        XrValue v = xr_tuple_get(t, i);
        ASSERT_TRUE(XR_IS_NULL(v));
    }
    teardown();
}

TEST(tuple_from_values_copies_payload) {
    setup();
    XrValue src[3] = {xr_int(10), xr_int(20), xr_int(30)};
    XrTuple *t = xr_tuple_from_values(main_coro, src, 3);
    ASSERT_NOT_NULL(t);
    ASSERT_EQ_INT(xr_tuple_arity(t), 3);
    ASSERT_EQ_INT(XR_TO_INT(xr_tuple_get(t, 0)), 10);
    ASSERT_EQ_INT(XR_TO_INT(xr_tuple_get(t, 1)), 20);
    ASSERT_EQ_INT(XR_TO_INT(xr_tuple_get(t, 2)), 30);
    teardown();
}

/* ========== Element Access ========== */

TEST(tuple_get_out_of_bounds_returns_null) {
    setup();
    XrTuple *t = xr_tuple_new(main_coro, 2);
    XrValue v = xr_tuple_get(t, 99);
    ASSERT_TRUE(XR_IS_NULL(v));
    teardown();
}

TEST(tuple_set_updates_slot) {
    setup();
    XrTuple *t = xr_tuple_new(main_coro, 2);
    xr_tuple_set(t, 0, xr_int(7));
    xr_tuple_set(t, 1, xr_int(11));
    ASSERT_EQ_INT(XR_TO_INT(xr_tuple_get(t, 0)), 7);
    ASSERT_EQ_INT(XR_TO_INT(xr_tuple_get(t, 1)), 11);
    teardown();
}

/* ========== Value Predicates ========== */

TEST(tuple_value_macros_classify_heap_object) {
    setup();
    XrTuple *t = xr_tuple_from_values(main_coro, (XrValue[]) {xr_int(1), xr_int(2)}, 2);
    XrValue v = xr_value_from_tuple(t);
    ASSERT_TRUE(xr_value_is_tuple(v));
    ASSERT_TRUE(!XR_IS_ARRAY(v));
    ASSERT_TRUE(xr_value_to_tuple(v) == t);
    teardown();
}

/* ========== Structural Equality ========== */

TEST(tuple_equals_alias_is_true) {
    setup();
    XrTuple *t = xr_tuple_from_values(main_coro, (XrValue[]) {xr_int(1)}, 1);
    ASSERT_TRUE(xr_tuple_equals(t, t));
    teardown();
}

TEST(tuple_equals_structural_match) {
    setup();
    XrTuple *a = xr_tuple_from_values(main_coro, (XrValue[]) {xr_int(1), xr_int(2)}, 2);
    XrTuple *b = xr_tuple_from_values(main_coro, (XrValue[]) {xr_int(1), xr_int(2)}, 2);
    ASSERT_TRUE(a != b);
    ASSERT_TRUE(xr_tuple_equals(a, b));
    teardown();
}

TEST(tuple_equals_different_arity_is_false) {
    setup();
    XrTuple *a = xr_tuple_from_values(main_coro, (XrValue[]) {xr_int(1)}, 1);
    XrTuple *b = xr_tuple_from_values(main_coro, (XrValue[]) {xr_int(1), xr_int(2)}, 2);
    ASSERT_TRUE(!xr_tuple_equals(a, b));
    teardown();
}

TEST(tuple_equals_different_payload_is_false) {
    setup();
    XrTuple *a = xr_tuple_from_values(main_coro, (XrValue[]) {xr_int(1), xr_int(2)}, 2);
    XrTuple *b = xr_tuple_from_values(main_coro, (XrValue[]) {xr_int(1), xr_int(99)}, 2);
    ASSERT_TRUE(!xr_tuple_equals(a, b));
    teardown();
}

TEST(tuple_equals_recurses_into_nested_tuples) {
    setup();
    XrTuple *inner_a = xr_tuple_from_values(main_coro, (XrValue[]) {xr_int(7), xr_int(8)}, 2);
    XrTuple *inner_b = xr_tuple_from_values(main_coro, (XrValue[]) {xr_int(7), xr_int(8)}, 2);
    XrTuple *outer_a =
        xr_tuple_from_values(main_coro, (XrValue[]) {xr_make_ptr_val(inner_a), xr_int(1)}, 2);
    XrTuple *outer_b =
        xr_tuple_from_values(main_coro, (XrValue[]) {xr_make_ptr_val(inner_b), xr_int(1)}, 2);
    ASSERT_TRUE(inner_a != inner_b);
    ASSERT_TRUE(xr_tuple_equals(outer_a, outer_b));
    teardown();
}

/* ========== Hash ========== */

TEST(tuple_hash_matches_for_equal_tuples) {
    setup();
    XrTuple *a = xr_tuple_from_values(main_coro, (XrValue[]) {xr_int(1), xr_int(2)}, 2);
    XrTuple *b = xr_tuple_from_values(main_coro, (XrValue[]) {xr_int(1), xr_int(2)}, 2);
    ASSERT_EQ_INT((int) xr_tuple_hash(a), (int) xr_tuple_hash(b));
    teardown();
}

TEST(tuple_hash_arity_breaks_unit_vs_zero_singleton) {
    setup();
    XrTuple *unit = xr_tuple_new(main_coro, 0);
    XrTuple *zero = xr_tuple_from_values(main_coro, (XrValue[]) {xr_int(0)}, 1);
    /* `()` and `(0,)` are different shapes and must hash differently. */
    ASSERT_TRUE(xr_tuple_hash(unit) != xr_tuple_hash(zero));
    teardown();
}

/* ========== Type System ========== */

TEST(tuple_type_unary_distinct_from_scalar) {
    setup();
    XrType *int_t = xr_type_new_int(X);
    XrType *elems[1] = {int_t};
    XrType *unary = xr_type_new_tuple(X, elems, 1);
    ASSERT_NOT_NULL(unary);
    /* Per the tuple design `(int,)` is a real arity-1 tuple, not the
     * underlying scalar. xr_type_new_tuple must not collapse. */
    ASSERT_TRUE(unary != int_t);
    ASSERT_TRUE(XR_TYPE_IS_TUPLE(unary));
    ASSERT_EQ_INT(xr_type_tuple_count(unary), 1);
    ASSERT_TRUE(xr_type_tuple_get(unary, 0) == int_t);
    teardown();
}

TEST(tuple_type_zero_arity_is_unit) {
    setup();
    XrType *u = xr_type_new_tuple(X, NULL, 0);
    ASSERT_NOT_NULL(u);
    ASSERT_TRUE(XR_TYPE_IS_UNIT(u));
    ASSERT_TRUE(u == xr_type_new_unit(X));
    teardown();
}

TEST(tuple_type_structural_equality) {
    setup();
    XrType *int_t = xr_type_new_int(X);
    XrType *str_t = xr_type_new_string(X);

    XrType *elems_a[2] = {int_t, str_t};
    XrType *elems_b[2] = {int_t, str_t};
    XrType *a = xr_type_new_tuple(X, elems_a, 2);
    XrType *b = xr_type_new_tuple(X, elems_b, 2);
    ASSERT_TRUE(a != b);
    ASSERT_TRUE(xr_type_equals(a, b));

    XrType *elems_diff[2] = {str_t, int_t};
    XrType *c = xr_type_new_tuple(X, elems_diff, 2);
    ASSERT_TRUE(!xr_type_equals(a, c));
    teardown();
}

TEST(tuple_type_covariant_assignable) {
    setup();
    XrType *int_t = xr_type_new_int(X);
    XrType *float_t = xr_type_new_float(X);

    /* int → float is a primitive widening, so (int, int) is assignable
     * to (float, float) by tuple covariance. */
    XrType *elems_src[2] = {int_t, int_t};
    XrType *elems_tgt[2] = {float_t, float_t};
    XrType *src = xr_type_new_tuple(X, elems_src, 2);
    XrType *tgt = xr_type_new_tuple(X, elems_tgt, 2);
    ASSERT_TRUE(xr_type_assignable(tgt, src));

    /* Mismatched arity is rejected regardless of element compatibility. */
    XrType *elems_short[1] = {int_t};
    XrType *shorter = xr_type_new_tuple(X, elems_short, 1);
    ASSERT_TRUE(!xr_type_assignable(tgt, shorter));
    teardown();
}

/* ========== Test Driver ========== */

static void run_all_tests(void) {
    RUN_TEST_SUITE("Tuple Object Creation");
    RUN_TEST(tuple_new_zero_arity);
    RUN_TEST(tuple_new_default_null_elements);
    RUN_TEST(tuple_from_values_copies_payload);

    RUN_TEST_SUITE("Tuple Element Access");
    RUN_TEST(tuple_get_out_of_bounds_returns_null);
    RUN_TEST(tuple_set_updates_slot);

    RUN_TEST_SUITE("Tuple Value Predicates");
    RUN_TEST(tuple_value_macros_classify_heap_object);

    RUN_TEST_SUITE("Tuple Structural Equality");
    RUN_TEST(tuple_equals_alias_is_true);
    RUN_TEST(tuple_equals_structural_match);
    RUN_TEST(tuple_equals_different_arity_is_false);
    RUN_TEST(tuple_equals_different_payload_is_false);
    RUN_TEST(tuple_equals_recurses_into_nested_tuples);

    RUN_TEST_SUITE("Tuple Hash");
    RUN_TEST(tuple_hash_matches_for_equal_tuples);
    RUN_TEST(tuple_hash_arity_breaks_unit_vs_zero_singleton);

    RUN_TEST_SUITE("Tuple Type System");
    RUN_TEST(tuple_type_unary_distinct_from_scalar);
    RUN_TEST(tuple_type_zero_arity_is_unit);
    RUN_TEST(tuple_type_structural_equality);
    RUN_TEST(tuple_type_covariant_assignable);
}

TEST_MAIN_BEGIN()
printf("=== xray Tuple Unit Tests ===\n");
run_all_tests();
TEST_MAIN_END()
