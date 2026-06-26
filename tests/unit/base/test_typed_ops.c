/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_typed_ops.c - Unit tests for shared typed storage helpers.
 */

#include "../test_framework.h"
#include "runtime/value/xvalue.h"
#include "shared/xr_typed_ops.h"

static uint64_t bits_f64(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint64_t bits_f32(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (uint64_t) bits;
}

TEST(typed_scalar_bits_integer_widths) {
    uint64_t bits = 0;

    ASSERT_TRUE(xr_typed_scalar_bits_i64(0x7f, XR_ELEM_I8, &bits));
    ASSERT_EQ_UINT(bits, 0x7fu);

    ASSERT_TRUE(xr_typed_scalar_bits_i64(0xff, XR_ELEM_I8, &bits));
    ASSERT_EQ_UINT(bits, UINT64_MAX);

    ASSERT_TRUE(xr_typed_scalar_bits_i64(-1, XR_ELEM_U8, &bits));
    ASSERT_EQ_UINT(bits, 0xffu);

    ASSERT_TRUE(xr_typed_scalar_bits_i64(0x8000, XR_ELEM_I16, &bits));
    ASSERT_EQ_UINT(bits, (uint64_t) INT64_C(-32768));

    ASSERT_TRUE(xr_typed_scalar_bits_i64(-1, XR_ELEM_U32, &bits));
    ASSERT_EQ_UINT(bits, 0xffffffffu);

    ASSERT_TRUE(xr_typed_scalar_bits_i64(-1, XR_ELEM_I64, &bits));
    ASSERT_EQ_UINT(bits, UINT64_MAX);
}

TEST(typed_scalar_bits_bool_canonicalizes_nonzero) {
    uint64_t bits = 99;

    ASSERT_TRUE(xr_typed_scalar_bits_i64(0, XR_ELEM_BOOL, &bits));
    ASSERT_EQ_UINT(bits, 0u);

    ASSERT_TRUE(xr_typed_scalar_bits_i64(-42, XR_ELEM_BOOL, &bits));
    ASSERT_EQ_UINT(bits, 1u);

    ASSERT_TRUE(xr_typed_scalar_bits_i64(42, XR_ELEM_BOOL, &bits));
    ASSERT_EQ_UINT(bits, 1u);
}

TEST(typed_scalar_bits_float_canonicalizes_negative_zero) {
    uint64_t bits = 0;

    ASSERT_TRUE(xr_typed_scalar_bits_f64(-0.0, XR_ELEM_F64, &bits));
    ASSERT_EQ_UINT(bits, bits_f64(0.0));

    ASSERT_TRUE(xr_typed_scalar_bits_f64(-0.0, XR_ELEM_F32, &bits));
    ASSERT_EQ_UINT(bits, bits_f32(0.0f));

    ASSERT_TRUE(xr_typed_scalar_bits_f64(1.5, XR_ELEM_F32, &bits));
    ASSERT_EQ_UINT(bits, bits_f32(1.5f));

    ASSERT_TRUE(xr_typed_scalar_bits_f64(1.5, XR_ELEM_F64, &bits));
    ASSERT_EQ_UINT(bits, bits_f64(1.5));
}

TEST(typed_scalar_bits_rejects_wrong_families) {
    uint64_t bits = 123;

    ASSERT_FALSE(xr_typed_scalar_bits_i64(1, XR_ELEM_F64, &bits));
    ASSERT_EQ_UINT(bits, 123u);

    ASSERT_FALSE(xr_typed_scalar_bits_f64(1.0, XR_ELEM_I64, &bits));
    ASSERT_EQ_UINT(bits, 123u);

    ASSERT_FALSE(xr_typed_scalar_bits_i64(1, XR_ELEM_ANY, &bits));
    ASSERT_EQ_UINT(bits, 123u);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Typed Ops");
RUN_TEST(typed_scalar_bits_integer_widths);
RUN_TEST(typed_scalar_bits_bool_canonicalizes_nonzero);
RUN_TEST(typed_scalar_bits_float_canonicalizes_negative_zero);
RUN_TEST(typed_scalar_bits_rejects_wrong_families);

TEST_MAIN_END()
