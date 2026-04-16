/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xvalue.c - Unit tests for XrValue (Tagged Union, 16 bytes)
 */

#include "../test_framework.h"
#include "runtime/value/xvalue.h"
#include <float.h>
#include <string.h>

/* ========== Integer Tests ========== */

TEST(value_int_zero) {
    XrValue v = xr_int(0);
    ASSERT_TRUE(XR_IS_INT(v));
    ASSERT_FALSE(XR_IS_FLOAT(v));
    ASSERT_FALSE(XR_IS_NULL(v));
    ASSERT_EQ_INT(XR_TO_INT(v), 0);
}

TEST(value_int_positive) {
    XrValue v = xr_int(42);
    ASSERT_TRUE(XR_IS_INT(v));
    ASSERT_EQ_INT(XR_TO_INT(v), 42);
}

TEST(value_int_negative) {
    XrValue v = xr_int(-123);
    ASSERT_TRUE(XR_IS_INT(v));
    ASSERT_EQ_INT(XR_TO_INT(v), -123);
}

TEST(value_int_max) {
    // Full 64-bit int range
    xr_Integer max_val = INT64_MAX;
    XrValue v = xr_int(max_val);
    ASSERT_TRUE(XR_IS_INT(v));
    ASSERT_EQ_INT(XR_TO_INT(v), max_val);
    ASSERT_EQ_INT(XR_TO_INT(v), INT64_MAX);
}

TEST(value_int_min) {
    // Full 64-bit int range
    xr_Integer min_val = INT64_MIN;
    XrValue v = xr_int(min_val);
    ASSERT_TRUE(XR_IS_INT(v));
    ASSERT_EQ_INT(XR_TO_INT(v), min_val);
    ASSERT_EQ_INT(XR_TO_INT(v), INT64_MIN);
}

TEST(value_int_full_64bit) {
    // Values that exceed old 48-bit NaN-boxing limit
    int64_t large_pos = (int64_t)1 << 50;
    XrValue v1 = xr_int(large_pos);
    ASSERT_TRUE(XR_IS_INT(v1));
    ASSERT_EQ_INT(XR_TO_INT(v1), large_pos);

    int64_t large_neg = -((int64_t)1 << 50);
    XrValue v2 = xr_int(large_neg);
    ASSERT_TRUE(XR_IS_INT(v2));
    ASSERT_EQ_INT(XR_TO_INT(v2), large_neg);
}

/* ========== Float Tests ========== */

TEST(value_float_zero) {
    XrValue v = xr_float(0.0);
    ASSERT_TRUE(XR_IS_FLOAT(v));
    ASSERT_FALSE(XR_IS_INT(v));
    ASSERT_FLOAT_EQ(XR_TO_FLOAT(v), 0.0, 1e-10);
}

TEST(value_float_positive) {
    XrValue v = xr_float(3.14159);
    ASSERT_TRUE(XR_IS_FLOAT(v));
    ASSERT_FLOAT_EQ(XR_TO_FLOAT(v), 3.14159, 1e-10);
}

TEST(value_float_negative) {
    XrValue v = xr_float(-2.718);
    ASSERT_TRUE(XR_IS_FLOAT(v));
    ASSERT_FLOAT_EQ(XR_TO_FLOAT(v), -2.718, 1e-10);
}

TEST(value_float_small) {
    XrValue v = xr_float(1e-300);
    ASSERT_TRUE(XR_IS_FLOAT(v));
    ASSERT_FLOAT_EQ(XR_TO_FLOAT(v), 1e-300, 1e-310);
}

TEST(value_float_large) {
    XrValue v = xr_float(1e100);
    ASSERT_TRUE(XR_IS_FLOAT(v));
    ASSERT_FLOAT_EQ(XR_TO_FLOAT(v), 1e100, 1e90);
}

TEST(value_float_infinity) {
    XrValue v = xr_float(INFINITY);
    ASSERT_TRUE(XR_IS_FLOAT(v));
    ASSERT_TRUE(XR_TO_FLOAT(v) == INFINITY);
}

TEST(value_float_neg_infinity) {
    XrValue v = xr_float(-INFINITY);
    ASSERT_TRUE(XR_IS_FLOAT(v));
    ASSERT_TRUE(XR_TO_FLOAT(v) == -INFINITY);
}

/* ========== Boolean Tests ========== */

TEST(value_bool_true) {
    XrValue v = xr_bool(true);
    ASSERT_TRUE(XR_IS_BOOL(v));
    ASSERT_FALSE(XR_IS_INT(v));
    ASSERT_TRUE(XR_TO_BOOL(v));
}

TEST(value_bool_false) {
    XrValue v = xr_bool(false);
    ASSERT_TRUE(XR_IS_BOOL(v));
    ASSERT_FALSE(XR_TO_BOOL(v));
}

TEST(value_bool_from_int) {
    // Non-zero should be true
    XrValue v1 = xr_bool(1);
    ASSERT_TRUE(XR_TO_BOOL(v1));
    
    XrValue v2 = xr_bool(0);
    ASSERT_FALSE(XR_TO_BOOL(v2));
}

/* ========== Null Tests ========== */

TEST(value_null) {
    XrValue v = xr_null();
    ASSERT_TRUE(XR_IS_NULL(v));
    ASSERT_FALSE(XR_IS_INT(v));
    ASSERT_FALSE(XR_IS_FLOAT(v));
    ASSERT_FALSE(XR_IS_BOOL(v));
}

TEST(value_null_consistency) {
    XrValue v1 = xr_null();
    XrValue v2 = xr_null();
    // All null values should be identical
    ASSERT_TRUE(xr_value_same(v1, v2));
}

/* ========== Type Checking Tests ========== */

TEST(value_type_exclusivity) {
    // Each value should have exactly one type
    XrValue int_val = xr_int(42);
    ASSERT_TRUE(XR_IS_INT(int_val));
    ASSERT_FALSE(XR_IS_FLOAT(int_val));
    ASSERT_FALSE(XR_IS_BOOL(int_val));
    ASSERT_FALSE(XR_IS_NULL(int_val));
    ASSERT_FALSE(XR_IS_PTR(int_val));
    
    XrValue float_val = xr_float(3.14);
    ASSERT_FALSE(XR_IS_INT(float_val));
    ASSERT_TRUE(XR_IS_FLOAT(float_val));
    ASSERT_FALSE(XR_IS_BOOL(float_val));
    ASSERT_FALSE(XR_IS_NULL(float_val));
    
    XrValue bool_val = xr_bool(true);
    ASSERT_FALSE(XR_IS_INT(bool_val));
    ASSERT_FALSE(XR_IS_FLOAT(bool_val));
    ASSERT_TRUE(XR_IS_BOOL(bool_val));
    ASSERT_FALSE(XR_IS_NULL(bool_val));
    
    XrValue null_val = xr_null();
    ASSERT_FALSE(XR_IS_INT(null_val));
    ASSERT_FALSE(XR_IS_FLOAT(null_val));
    ASSERT_FALSE(XR_IS_BOOL(null_val));
    ASSERT_TRUE(XR_IS_NULL(null_val));
}

/* ========== Number Conversion Tests ========== */

TEST(value_is_number) {
    ASSERT_TRUE(XR_IS_NUM(xr_int(42)));
    ASSERT_TRUE(XR_IS_NUM(xr_float(3.14)));
    ASSERT_FALSE(XR_IS_NUM(xr_bool(true)));
    ASSERT_FALSE(XR_IS_NUM(xr_null()));
}

TEST(value_as_number) {
    // Int to number
    XrValue int_val = xr_int(42);
    ASSERT_FLOAT_EQ((double)XR_TO_INT(int_val), 42.0, 1e-10);
    
    // Float to number
    XrValue float_val = xr_float(3.14);
    ASSERT_FLOAT_EQ(XR_TO_FLOAT(float_val), 3.14, 1e-10);
}

/* ========== Truthiness Tests ========== */

static inline bool is_truthy(XrValue v) {
    if (XR_IS_NULL(v)) return false;
    if (XR_IS_BOOL(v)) return XR_TO_BOOL(v);
    if (XR_IS_INT(v)) return XR_TO_INT(v) != 0;
    if (XR_IS_FLOAT(v)) return XR_TO_FLOAT(v) != 0.0;
    return true;
}

static inline bool is_falsy(XrValue v) {
    return !is_truthy(v);
}

TEST(value_is_truthy) {
    // Numbers: 0 is falsy, others are truthy
    ASSERT_FALSE(is_truthy(xr_int(0)));
    ASSERT_TRUE(is_truthy(xr_int(1)));
    ASSERT_TRUE(is_truthy(xr_int(-1)));
    
    ASSERT_FALSE(is_truthy(xr_float(0.0)));
    ASSERT_TRUE(is_truthy(xr_float(0.1)));
    ASSERT_TRUE(is_truthy(xr_float(-0.1)));
    
    // Bool: direct
    ASSERT_TRUE(is_truthy(xr_bool(true)));
    ASSERT_FALSE(is_truthy(xr_bool(false)));
    
    // Null is always falsy
    ASSERT_FALSE(is_truthy(xr_null()));
}

TEST(value_is_falsy) {
    ASSERT_TRUE(is_falsy(xr_int(0)));
    ASSERT_TRUE(is_falsy(xr_float(0.0)));
    ASSERT_TRUE(is_falsy(xr_bool(false)));
    ASSERT_TRUE(is_falsy(xr_null()));
    
    ASSERT_FALSE(is_falsy(xr_int(1)));
    ASSERT_FALSE(is_falsy(xr_float(1.0)));
    ASSERT_FALSE(is_falsy(xr_bool(true)));
}

/* ========== Value Equality Tests ========== */

TEST(value_equality_int) {
    XrValue a = xr_int(42);
    XrValue b = xr_int(42);
    XrValue c = xr_int(43);
    
    ASSERT_TRUE(xr_value_deep_eq(a, b));
    ASSERT_FALSE(xr_value_deep_eq(a, c));
}

TEST(value_equality_float) {
    XrValue a = xr_float(3.14);
    XrValue b = xr_float(3.14);
    XrValue c = xr_float(2.71);
    
    ASSERT_TRUE(xr_value_deep_eq(a, b));
    ASSERT_FALSE(xr_value_deep_eq(a, c));
}

TEST(value_equality_bool) {
    ASSERT_TRUE(xr_value_deep_eq(xr_bool(true), xr_bool(true)));
    ASSERT_TRUE(xr_value_deep_eq(xr_bool(false), xr_bool(false)));
    ASSERT_FALSE(xr_value_deep_eq(xr_bool(true), xr_bool(false)));
}

TEST(value_equality_null) {
    ASSERT_TRUE(xr_value_deep_eq(xr_null(), xr_null()));
}

TEST(value_equality_cross_type) {
    // Note: xray considers int and float with same numeric value as equal
    ASSERT_TRUE(xr_value_deep_eq(xr_int(0), xr_float(0.0)));
    ASSERT_TRUE(xr_value_deep_eq(xr_int(42), xr_float(42.0)));
    // Different value types (non-numeric) should not be equal
    ASSERT_FALSE(xr_value_deep_eq(xr_int(0), xr_bool(false)));
    ASSERT_FALSE(xr_value_deep_eq(xr_int(0), xr_null()));
    ASSERT_FALSE(xr_value_deep_eq(xr_bool(false), xr_null()));
}

/* ========== Tagged Union Integrity Tests ========== */

TEST(value_tagged_union_integrity) {
    // Ensure values don't get corrupted through encoding/decoding
    for (int i = -1000; i <= 1000; i++) {
        XrValue v = xr_int(i);
        ASSERT_TRUE(XR_IS_INT(v));
        ASSERT_EQ_INT(XR_TO_INT(v), i);
    }
}

TEST(value_float_precision) {
    // Test that float precision is preserved
    double vals[] = {0.1, 0.2, 0.3, 1.0/3.0, M_PI, M_E};
    for (int i = 0; i < 6; i++) {
        XrValue v = xr_float(vals[i]);
        ASSERT_FLOAT_EQ(XR_TO_FLOAT(v), vals[i], 1e-15);
    }
}

TEST(value_sizeof) {
    // Tagged Union must be 16 bytes
    ASSERT_EQ_INT((int)sizeof(XrValue), 16);
}

TEST(value_memset_zero_is_null) {
    // memset to zero should produce null (tag=0 = XR_TAG_NULL)
    XrValue v;
    memset(&v, 0, sizeof(v));
    ASSERT_TRUE(XR_IS_NULL(v));
    ASSERT_EQ_INT((int)v.tag, (int)XR_TAG_NULL);
}

/* ========== Numeric Type Tests ========== */

TEST(value_int_type_checks) {
    ASSERT_TRUE(XR_IS_INT(xr_int(42)));
    ASSERT_TRUE(XR_IS_INT(xr_int(-1)));
    ASSERT_FALSE(XR_IS_INT(xr_float(1.0)));
    ASSERT_FALSE(XR_IS_INT(xr_null()));

    ASSERT_EQ_INT((int)xr_value_to_i64(xr_int(42)), 42);
    ASSERT_EQ_INT((int)xr_value_to_i64(xr_float(1.0)), 0);
}

TEST(value_float_type_checks) {
    ASSERT_TRUE(XR_IS_FLOAT(xr_float(3.14)));
    ASSERT_FALSE(XR_IS_FLOAT(xr_int(1)));
    ASSERT_FALSE(XR_IS_FLOAT(xr_null()));

    ASSERT_TRUE(XR_IS_NUM(xr_int(1)));
    ASSERT_TRUE(XR_IS_NUM(xr_float(1.0)));
    ASSERT_FALSE(XR_IS_NUM(xr_null()));
}

/* ========== Precise Extraction Tests ========== */

TEST(value_to_f64_conversion) {
    // int -> double
    ASSERT_FLOAT_EQ(xr_value_to_f64(xr_int(42)), 42.0, 1e-10);
    // float passthrough
    ASSERT_FLOAT_EQ(xr_value_to_f64(xr_float(3.14)), 3.14, 1e-10);
    // non-numeric -> 0.0
    ASSERT_FLOAT_EQ(xr_value_to_f64(xr_null()), 0.0, 1e-10);
}

/* ========== Main ========== */

static void run_all_tests(void) {
    RUN_TEST_SUITE("Integer Values");
    RUN_TEST(value_int_zero);
    RUN_TEST(value_int_positive);
    RUN_TEST(value_int_negative);
    RUN_TEST(value_int_max);
    RUN_TEST(value_int_min);
    RUN_TEST(value_int_full_64bit);
    
    RUN_TEST_SUITE("Float Values");
    RUN_TEST(value_float_zero);
    RUN_TEST(value_float_positive);
    RUN_TEST(value_float_negative);
    RUN_TEST(value_float_small);
    RUN_TEST(value_float_large);
    RUN_TEST(value_float_infinity);
    RUN_TEST(value_float_neg_infinity);
    
    RUN_TEST_SUITE("Boolean Values");
    RUN_TEST(value_bool_true);
    RUN_TEST(value_bool_false);
    RUN_TEST(value_bool_from_int);
    
    RUN_TEST_SUITE("Null Values");
    RUN_TEST(value_null);
    RUN_TEST(value_null_consistency);
    
    RUN_TEST_SUITE("Type Checking");
    RUN_TEST(value_type_exclusivity);
    
    RUN_TEST_SUITE("Number Conversion");
    RUN_TEST(value_is_number);
    RUN_TEST(value_as_number);
    
    RUN_TEST_SUITE("Truthiness");
    RUN_TEST(value_is_truthy);
    RUN_TEST(value_is_falsy);
    
    RUN_TEST_SUITE("Value Equality");
    RUN_TEST(value_equality_int);
    RUN_TEST(value_equality_float);
    RUN_TEST(value_equality_bool);
    RUN_TEST(value_equality_null);
    RUN_TEST(value_equality_cross_type);
    
    RUN_TEST_SUITE("Tagged Union Integrity");
    RUN_TEST(value_tagged_union_integrity);
    RUN_TEST(value_float_precision);
    RUN_TEST(value_sizeof);
    RUN_TEST(value_memset_zero_is_null);
    
    RUN_TEST_SUITE("Numeric Type Checks");
    RUN_TEST(value_int_type_checks);
    RUN_TEST(value_float_type_checks);
    
    RUN_TEST_SUITE("Precise Extraction");
    RUN_TEST(value_to_f64_conversion);
    
}

TEST_MAIN_BEGIN()
    printf("=== xray Value System Unit Tests ===\n");
    run_all_tests();
TEST_MAIN_END()
