/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_numeric_core.c - Unit tests for runtime-neutral numeric method helpers
 */

#include "../test_framework.h"
#include "shared/xr_bits_core.h"
#include "shared/xr_codegen_opaque_core.h"
#include "shared/xr_copy_core.h"
#include "shared/xr_int_arith_core.h"
#include "shared/xr_numeric_conversion_core.h"
#include "shared/xr_numeric_core.h"
#include "shared/xr_owner_forward_core.h"
#include "shared/xr_raw_scalar_core.h"
#include "shared/xr_static_address_core.h"
#include "shared/xr_reference_count_core.h"
#include "shared/xr_sync_core.h"
#include <stdint.h>
#include <string.h>

static void assert_format_i64(int64_t value, const char *expected) {
    char buf[32];
    int len = xr_numeric_core_format_i64(buf, sizeof(buf), value);
    ASSERT_EQ_INT(len, (int64_t) strlen(expected));
    ASSERT(strcmp(buf, expected) == 0);
}

static void assert_format_hex(int64_t value, const char *expected) {
    char buf[32];
    int len = xr_numeric_core_format_i64_hex(buf, sizeof(buf), value);
    ASSERT_EQ_INT(len, (int64_t) strlen(expected));
    ASSERT(strcmp(buf, expected) == 0);
}

TEST(numeric_core_format_i64_handles_boundaries) {
    assert_format_i64(0, "0");
    assert_format_i64(42, "42");
    assert_format_i64(-42, "-42");
    assert_format_i64(INT64_MAX, "9223372036854775807");
    assert_format_i64(INT64_MIN, "-9223372036854775808");
}

TEST(numeric_core_format_i64_rejects_short_buffer) {
    char buf[4] = {0};
    ASSERT_EQ_INT(xr_numeric_core_format_i64(buf, sizeof(buf), 12345), -1);
}

TEST(numeric_core_hex_matches_signed_magnitude_rule) {
    assert_format_hex(0, "0x0");
    assert_format_hex(15, "0xF");
    assert_format_hex(255, "0xFF");
    assert_format_hex(-15, "-0xF");
    assert_format_hex(INT64_MIN, "-0x8000000000000000");
}

TEST(numeric_core_abs_wraps_int64_min) {
    ASSERT_EQ_INT(xr_numeric_core_i64_abs_wrap(0), 0);
    ASSERT_EQ_INT(xr_numeric_core_i64_abs_wrap(-42), 42);
    ASSERT_EQ_INT(xr_numeric_core_i64_abs_wrap(INT64_MIN), INT64_MIN);
}

TEST(numeric_core_integer_arithmetic_wraps) {
    ASSERT_EQ_INT(xr_numeric_core_i64_add_wrap(INT64_MAX, 1), INT64_MIN);
    ASSERT_EQ_INT(xr_numeric_core_i64_sub_wrap(INT64_MIN, 1), INT64_MAX);
    ASSERT_EQ_INT(xr_numeric_core_i64_mul_wrap(INT64_MAX, 2), -2);
    ASSERT_EQ_INT(xr_numeric_core_i64_neg_wrap(INT64_MIN), INT64_MIN);
}

TEST(numeric_neg_owner_freezes_scalar_bits_and_bigint_sign) {
    XrNumericNegResult scalar = XR_NUMERIC_NEG_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_HI, XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_LO,
        XR_SEM_CONSUMER_VM, XR_NUMERIC_NEG_I64, INT64_MIN, 0.0);
    ASSERT_EQ_INT(scalar.i64, INT64_MIN);
    scalar = XR_NUMERIC_NEG_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_HI, XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_LO,
        XR_SEM_CONSUMER_AOT_HOSTED, XR_NUMERIC_NEG_I64, INT64_MAX, 0.0);
    ASSERT_EQ_INT(scalar.i64, -INT64_MAX);

    uint64_t input_bits = UINT64_C(0x0000000000000000);
    double input = 0.0;
    memcpy(&input, &input_bits, sizeof(input));
    scalar = xr_numeric_neg_eval(XR_NUMERIC_NEG_F64, 0, input);
    uint64_t result_bits = 0;
    memcpy(&result_bits, &scalar.f64, sizeof(result_bits));
    ASSERT_EQ_UINT(result_bits, UINT64_C(0x8000000000000000));

    input_bits = UINT64_C(0xfff8123456789abc);
    memcpy(&input, &input_bits, sizeof(input));
    scalar = xr_numeric_neg_eval(XR_NUMERIC_NEG_F64, 0, input);
    memcpy(&result_bits, &scalar.f64, sizeof(result_bits));
    ASSERT_EQ_UINT(result_bits, UINT64_C(0x7ff8123456789abc));

    const uint32_t positive[2] = {0, 1};
    const uint32_t zero[1] = {0};
    const uint32_t denormalized[2] = {1, 0};
    XrNumericNegBigIntPlan plan = XR_NUMERIC_NEG_BIGINT_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_HI, XR_SEM_OWNER_ID_SHARED_NUMERIC_NEG_LO,
        XR_SEM_CONSUMER_RUNTIME, positive, 2, 1);
    ASSERT_TRUE(plan.valid);
    ASSERT_EQ_INT(plan.length, 2);
    ASSERT_EQ_INT(plan.result_sign, -1);
    plan = xr_numeric_neg_bigint_plan(positive, 2, -1);
    ASSERT_TRUE(plan.valid);
    ASSERT_EQ_INT(plan.result_sign, 1);
    plan = xr_numeric_neg_bigint_plan(zero, 1, 1);
    ASSERT_TRUE(plan.valid);
    ASSERT_EQ_INT(plan.result_sign, 1);
    ASSERT_FALSE(xr_numeric_neg_bigint_plan(denormalized, 2, 1).valid);
}

TEST(int_div_mod_owner_freezes_signed_unsigned_and_zero_edges) {
    /* Signed edges: division stays total once the divisor is nonzero. */
    ASSERT_EQ_INT(xr_int_div_mod_apply(XR_INT_DIV_MOD_DIV, XR_INT_DIV_MOD_PROOF_NONZERO,
                                       INT64_MIN, -1),
                  INT64_MIN);
    ASSERT_EQ_INT(xr_int_div_mod_apply(XR_INT_DIV_MOD_MOD, XR_INT_DIV_MOD_PROOF_NONZERO,
                                       INT64_MIN, -1),
                  0);
    ASSERT_EQ_INT(xr_int_div_mod_apply(XR_INT_DIV_MOD_DIV, XR_INT_DIV_MOD_PROOF_NONZERO, 7, -3),
                  -2);
    ASSERT_EQ_INT(xr_int_div_mod_apply(XR_INT_DIV_MOD_MOD, XR_INT_DIV_MOD_PROOF_NONZERO, 7, -3), 1);

    /* A positive-divisor proof selects the same value as the general rule. */
    ASSERT_EQ_INT(xr_int_div_mod_apply(XR_INT_DIV_MOD_DIV, XR_INT_DIV_MOD_PROOF_POSITIVE, -7, 3),
                  xr_int_div_mod_apply(XR_INT_DIV_MOD_DIV, XR_INT_DIV_MOD_PROOF_NONZERO, -7, 3));
    ASSERT_EQ_INT(xr_int_div_mod_apply(XR_INT_DIV_MOD_MOD, XR_INT_DIV_MOD_PROOF_POSITIVE, -7, 3),
                  xr_int_div_mod_apply(XR_INT_DIV_MOD_MOD, XR_INT_DIV_MOD_PROOF_NONZERO, -7, 3));

    /* Unsigned kinds read the same i64 slot as u64. */
    ASSERT_EQ_INT(xr_int_div_mod_apply(XR_INT_DIV_MOD_DIV_U, XR_INT_DIV_MOD_PROOF_NONZERO, -1, 2),
                  (int64_t) (UINT64_MAX / 2u));
    ASSERT_EQ_INT(xr_int_div_mod_apply(XR_INT_DIV_MOD_MOD_U, XR_INT_DIV_MOD_PROOF_NONZERO, -1, 10),
                  (int64_t) (UINT64_MAX % 10u));
    ASSERT_EQ_INT(xr_int_div_mod_apply(XR_INT_DIV_MOD_DIV_U, XR_INT_DIV_MOD_PROOF_NONZERO,
                                       INT64_MIN, -1),
                  0);

    /* The zero divisor is reported, never raised, and only when unproven. */
    XrIntDivModResult zero = XR_INT_DIV_MOD_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_HI, XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_LO,
        XR_SEM_CONSUMER_VM, XR_INT_DIV_MOD_DIV, XR_INT_DIV_MOD_PROOF_NONE, 5, 0);
    ASSERT_TRUE(zero.divisor_is_zero);
    ASSERT_EQ_INT(zero.value, 0);

    XrIntDivModResult ok = XR_INT_DIV_MOD_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_HI, XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_LO,
        XR_SEM_CONSUMER_VM, XR_INT_DIV_MOD_MOD, XR_INT_DIV_MOD_PROOF_NONE, -7, 3);
    ASSERT_FALSE(ok.divisor_is_zero);
    ASSERT_EQ_INT(ok.value, -1);
    ASSERT_EQ_INT(XR_INT_DIV_MOD_OWNER_APPLY_PROVEN(
                      XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_HI,
                      XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_LO, XR_SEM_CONSUMER_VM,
                      XR_INT_DIV_MOD_DIV, XR_INT_DIV_MOD_PROOF_POSITIVE, -7, 3),
                  -2);
}

TEST(numeric_core_shift_counts_are_mod64) {
    ASSERT_EQ_INT(xr_shift_i64(XR_SHIFT_LEFT, 12345, 64), 12345);
    ASSERT_EQ_INT(xr_shift_i64(XR_SHIFT_LEFT, 12345, 65), 24690);
    ASSERT_EQ_INT(xr_shift_i64(XR_SHIFT_RIGHT_SIGNED, 12345, 70), 192);
    ASSERT_EQ_INT(xr_shift_i64(XR_SHIFT_LEFT, 1, -1), INT64_MIN);
    ASSERT_EQ_INT(xr_shift_i64(XR_SHIFT_RIGHT_SIGNED, -8, 1), -4);
    ASSERT_EQ_INT(xr_shift_i64(XR_SHIFT_RIGHT_SIGNED, INT64_MIN, 63), -1);
    ASSERT_EQ_INT(xr_shift_i64(XR_SHIFT_RIGHT_UNSIGNED, INT64_MIN, 1),
                  INT64_C(4611686018427387904));
    ASSERT_EQ_INT(XR_SHIFT_OWNER_APPLY(
                      XR_SEM_OWNER_ID_SHARED_SHIFT_HI, XR_SEM_OWNER_ID_SHARED_SHIFT_LO,
                      XR_SEM_CONSUMER_VM, XR_SHIFT_LEFT, 1, 63),
                  INT64_MIN);
}

TEST(bitwise_binary_owner_freezes_scalar_and_bigint_edges) {
    ASSERT_EQ_INT(xr_bitwise_binary_i64(XR_BITWISE_BINARY_AND, -1, 0x55), 0x55);
    ASSERT_EQ_INT(xr_bitwise_binary_i64(XR_BITWISE_BINARY_OR, INT64_MIN, 1), INT64_MIN + 1);
    ASSERT_EQ_INT(xr_bitwise_binary_i64(XR_BITWISE_BINARY_XOR, -1, INT64_MIN), INT64_MAX);
    ASSERT_EQ_INT(XR_BITWISE_BINARY_OWNER_APPLY(
                      XR_SEM_OWNER_ID_SHARED_BITWISE_BINARY_HI,
                      XR_SEM_OWNER_ID_SHARED_BITWISE_BINARY_LO, XR_SEM_CONSUMER_VM,
                      XR_BITWISE_BINARY_XOR, 1, 1),
                  0);

    const uint32_t positive_2_32[2] = {0, 1};
    const uint32_t negative_3[1] = {3};
    uint32_t result[3] = {0};
    int8_t sign = 0;

    XrBigIntBitwisePlan plan = xr_bitwise_binary_bigint_plan(
        XR_BITWISE_BINARY_AND, 2, 1, 1, -1);
    ASSERT_EQ_INT(plan.status, XR_BITWISE_BINARY_STATUS_OK);
    ASSERT_EQ_INT(plan.capacity, 3);
    ASSERT_FALSE(plan.result_negative);
    ASSERT_EQ_INT(xr_bitwise_binary_bigint_apply(&plan, positive_2_32, 2, 1, negative_3, 1,
                                                 -1, result, &sign),
                  2);
    ASSERT_EQ_UINT(result[0], 0);
    ASSERT_EQ_UINT(result[1], 1);
    ASSERT_EQ_INT(sign, 1);

    memset(result, 0, sizeof(result));
    plan = xr_bitwise_binary_bigint_plan(XR_BITWISE_BINARY_OR, 2, 1, 1, -1);
    ASSERT_TRUE(plan.result_negative);
    ASSERT_EQ_INT(xr_bitwise_binary_bigint_apply(&plan, positive_2_32, 2, 1, negative_3, 1,
                                                 -1, result, &sign),
                  1);
    ASSERT_EQ_UINT(result[0], 3);
    ASSERT_EQ_INT(sign, -1);

    memset(result, 0, sizeof(result));
    plan = xr_bitwise_binary_bigint_plan(XR_BITWISE_BINARY_XOR, 2, 1, 1, -1);
    ASSERT_TRUE(plan.result_negative);
    ASSERT_EQ_INT(xr_bitwise_binary_bigint_apply(&plan, positive_2_32, 2, 1, negative_3, 1,
                                                 -1, result, &sign),
                  2);
    ASSERT_EQ_UINT(result[0], 3);
    ASSERT_EQ_UINT(result[1], 1);
    ASSERT_EQ_INT(sign, -1);

    plan = xr_bitwise_binary_bigint_plan((XrBitwiseBinaryKind) 99, 1, 1, 1, 1);
    ASSERT_EQ_INT(plan.status, XR_BITWISE_BINARY_STATUS_INVALID_KIND);
    plan = xr_bitwise_binary_bigint_plan(XR_BITWISE_BINARY_AND, UINT32_MAX, 1, 1, 1);
    ASSERT_EQ_INT(plan.status, XR_BITWISE_BINARY_STATUS_CAPACITY_OVERFLOW);
}

TEST(shift_owner_bigint_plan_and_limb_kernel) {
    uint32_t one[1] = {1};
    uint32_t wide[4] = {0};
    int8_t sign = 0;
    XrBigIntShiftPlan left = xr_shift_bigint_plan(XR_SHIFT_LEFT, 1, false, 64);
    ASSERT_EQ_INT(left.status, XR_SHIFT_STATUS_OK);
    ASSERT_EQ_INT(left.capacity, 4);
    ASSERT_EQ_INT(xr_shift_bigint_apply(&left, one, 1, 1, wide, &sign), 3);
    ASSERT_EQ_UINT(wide[0], 0);
    ASSERT_EQ_UINT(wide[1], 0);
    ASSERT_EQ_UINT(wide[2], 1);
    ASSERT_EQ_INT(sign, 1);

    uint32_t narrowed[3] = {0};
    XrBigIntShiftPlan right = xr_shift_bigint_plan(XR_SHIFT_RIGHT_SIGNED, 3, false, 64);
    ASSERT_EQ_INT(xr_shift_bigint_apply(&right, wide, 3, 1, narrowed, &sign), 1);
    ASSERT_EQ_UINT(narrowed[0], 1);
    ASSERT_EQ_INT(sign, 1);

    XrBigIntShiftPlan negative = xr_shift_bigint_plan(XR_SHIFT_LEFT, 1, false, -1);
    ASSERT_EQ_INT(negative.status, XR_SHIFT_STATUS_COUNT_RANGE);
}

TEST(numeric_conversion_integer_matrix_is_bit_defined) {
    ASSERT_EQ_INT(xr_numeric_int_convert_i64(-1, XR_NATIVE_I8, XR_NATIVE_U8, 64), 255);
    ASSERT_EQ_INT(xr_numeric_int_convert_i64(255, XR_NATIVE_U8, XR_NATIVE_I8, 64), -1);
    ASSERT_EQ_UINT(xr_numeric_i64_to_bits(xr_numeric_int_convert_i64(
                       xr_numeric_i64_from_bits(UINT64_MAX), XR_NATIVE_U64, XR_NATIVE_U32, 64)),
                   UINT32_MAX);
    ASSERT_EQ_INT(xr_numeric_int_convert_i64(INT64_C(0x100000001), XR_NATIVE_U64,
                                             XR_NATIVE_USIZE, 32),
                  1);
    ASSERT_EQ_UINT(xr_numeric_i64_to_bits(xr_numeric_int_convert_i64(
                       -1, XR_NATIVE_ISIZE, XR_NATIVE_USIZE, 64)),
                   UINT64_MAX);

    /* Target-sized conversions use the requested ABI width, never host sizeof(void *). */
    ASSERT_EQ_UINT(xr_numeric_i64_to_bits(xr_numeric_int_convert_i64(
                       xr_numeric_i64_from_bits(UINT64_C(0xffffffff80000000)), XR_NATIVE_U64,
                       XR_NATIVE_ISIZE, 32)),
                   UINT64_C(0xffffffff80000000));
    ASSERT_EQ_UINT(xr_numeric_i64_to_bits(xr_numeric_int_convert_i64(
                       xr_numeric_i64_from_bits(UINT64_C(0x100000000)), XR_NATIVE_U64,
                       XR_NATIVE_USIZE, 32)),
                   0);
    ASSERT_EQ_UINT(xr_numeric_i64_to_bits(xr_numeric_int_convert_i64(
                       xr_numeric_i64_from_bits(UINT64_C(0x100000000)), XR_NATIVE_U64,
                       XR_NATIVE_USIZE, 64)),
                   UINT64_C(0x100000000));
}

TEST(numeric_conversion_integer_to_float_is_round_to_even) {
    ASSERT(xr_numeric_int_to_float(INT64_C(16777217), XR_NATIVE_U64, XR_NATIVE_F32, 64) ==
           16777216.0);
    ASSERT(xr_numeric_int_to_float(INT64_C(16777219), XR_NATIVE_U64, XR_NATIVE_F32, 64) ==
           16777220.0);
    ASSERT(xr_numeric_int_to_float(xr_numeric_i64_from_bits(UINT64_MAX), XR_NATIVE_U64,
                                   XR_NATIVE_F64, 64) ==
           18446744073709551616.0);
    ASSERT(xr_numeric_int_to_float(INT64_MIN, XR_NATIVE_I64, XR_NATIVE_F64, 64) ==
           -9223372036854775808.0);
}

TEST(numeric_conversion_f64_to_f32_has_frozen_edges) {
    double min_subnormal = xr_numeric_double_from_bits(UINT64_C(874) << 52);
    double half_min_subnormal = xr_numeric_double_from_bits(UINT64_C(873) << 52);
    ASSERT(xr_numeric_f64_to_f32(min_subnormal) == min_subnormal);
    ASSERT_EQ_UINT(xr_numeric_double_to_bits(xr_numeric_f64_to_f32(half_min_subnormal)), 0);
    ASSERT_EQ_UINT(xr_numeric_double_to_bits(xr_numeric_f64_to_f32(
                       xr_numeric_double_from_bits(UINT64_C(0x7ff123456789abcd)))),
                   xr_numeric_double_to_bits(
                       (double) xr_numeric_float_from_bits(XR_NUMERIC_CANONICAL_F32_NAN)));
    ASSERT_EQ_UINT(xr_numeric_double_to_bits(xr_numeric_f64_to_f32(
                       xr_numeric_double_from_bits(UINT64_C(0x7ff0000000000000)))),
                   UINT64_C(0x7ff0000000000000));
}

TEST(numeric_width_owner_freezes_lowbits_sign_extension_rounding_nan_and_overflow) {
    ASSERT_EQ_INT(XR_NUMERIC_WIDTH_OWNER_APPLY(
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, XR_SEM_CONSUMER_VM,
                      xr_numeric_narrow_i8, INT64_C(0x1ff)),
                  -1);
    ASSERT_EQ_INT(XR_NUMERIC_WIDTH_OWNER_APPLY(
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, XR_SEM_CONSUMER_VM,
                      xr_numeric_narrow_u8, INT64_C(0x1ff)),
                  255);
    ASSERT_EQ_INT(XR_NUMERIC_WIDTH_OWNER_APPLY(
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, XR_SEM_CONSUMER_VM,
                      xr_numeric_narrow_i16, INT64_C(0x18000)),
                  INT16_MIN);
    ASSERT_EQ_INT(XR_NUMERIC_WIDTH_OWNER_APPLY(
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, XR_SEM_CONSUMER_VM,
                      xr_numeric_narrow_u16, INT64_C(0x18000)),
                  UINT16_C(0x8000));
    ASSERT_EQ_INT(XR_NUMERIC_WIDTH_OWNER_APPLY(
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, XR_SEM_CONSUMER_VM,
                      xr_numeric_narrow_i32, INT64_C(0x180000000)),
                  INT32_MIN);
    ASSERT_EQ_UINT(xr_numeric_i64_to_bits(XR_NUMERIC_WIDTH_OWNER_APPLY(
                       XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,
                       XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, XR_SEM_CONSUMER_VM,
                       xr_numeric_narrow_u32, INT64_C(0x180000000))),
                   UINT32_C(0x80000000));

    ASSERT_EQ_INT(XR_NUMERIC_WIDTH_OWNER_APPLY(
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, XR_SEM_CONSUMER_VM,
                      xr_numeric_widen_i8, INT64_C(0xff)),
                  -1);
    ASSERT_EQ_INT(XR_NUMERIC_WIDTH_OWNER_APPLY(
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, XR_SEM_CONSUMER_VM,
                      xr_numeric_widen_u8, INT64_C(0xff)),
                  255);
    ASSERT_EQ_INT(XR_NUMERIC_WIDTH_OWNER_APPLY(
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, XR_SEM_CONSUMER_VM,
                      xr_numeric_widen_i16, INT64_C(0xffff)),
                  -1);
    ASSERT_EQ_INT(XR_NUMERIC_WIDTH_OWNER_APPLY(
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, XR_SEM_CONSUMER_VM,
                      xr_numeric_widen_u16, INT64_C(0xffff)),
                  65535);
    ASSERT_EQ_INT(XR_NUMERIC_WIDTH_OWNER_APPLY(
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,
                      XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, XR_SEM_CONSUMER_VM,
                      xr_numeric_widen_i32, INT64_C(0xffffffff)),
                  -1);
    ASSERT_EQ_UINT(xr_numeric_i64_to_bits(XR_NUMERIC_WIDTH_OWNER_APPLY(
                       XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,
                       XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, XR_SEM_CONSUMER_VM,
                       xr_numeric_widen_u32, -INT64_C(1))),
                   UINT32_MAX);

    double rounded = XR_NUMERIC_WIDTH_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,
        XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, XR_SEM_CONSUMER_VM,
        xr_numeric_narrow_f32, 16777217.0);
    ASSERT(rounded == 16777216.0);
    double nan = XR_NUMERIC_WIDTH_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,
        XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, XR_SEM_CONSUMER_VM,
        xr_numeric_narrow_f32,
        xr_numeric_double_from_bits(UINT64_C(0x7ff123456789abcd)));
    ASSERT_EQ_UINT(xr_numeric_double_to_bits(nan),
                   xr_numeric_double_to_bits(
                       (double) xr_numeric_float_from_bits(XR_NUMERIC_CANONICAL_F32_NAN)));
    double overflow = XR_NUMERIC_WIDTH_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,
        XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, XR_SEM_CONSUMER_VM,
        xr_numeric_narrow_f32, xr_numeric_power_of_two(128));
    ASSERT_EQ_UINT(xr_numeric_double_to_bits(overflow), UINT64_C(0x7ff0000000000000));
    ASSERT(XR_NUMERIC_WIDTH_OWNER_APPLY(
               XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI,
               XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, XR_SEM_CONSUMER_VM,
               xr_numeric_widen_f32, 16777217.0) == 16777216.0);
}

TEST(numeric_conversion_float_to_integer_checks_before_cast) {
    int64_t out = 123;
    ASSERT_TRUE(xr_numeric_float_to_int(255.99, XR_NATIVE_U8, 64, &out));
    ASSERT_EQ_INT(out, 255);
    ASSERT_TRUE(xr_numeric_float_to_int(-0.99, XR_NATIVE_U8, 64, &out));
    ASSERT_EQ_INT(out, 0);
    ASSERT_FALSE(xr_numeric_float_to_int(256.0, XR_NATIVE_U8, 64, &out));
    ASSERT_TRUE(xr_numeric_float_to_int(-128.99, XR_NATIVE_I8, 64, &out));
    ASSERT_EQ_INT(out, -128);
    ASSERT_FALSE(xr_numeric_float_to_int(-129.0, XR_NATIVE_I8, 64, &out));
    ASSERT_FALSE(xr_numeric_float_to_int(
        xr_numeric_double_from_bits(UINT64_C(0x7ff8000000000000)), XR_NATIVE_I64, 64, &out));
    ASSERT_FALSE(xr_numeric_float_to_int(
        xr_numeric_double_from_bits(UINT64_C(0x7ff0000000000000)), XR_NATIVE_I64, 64, &out));
    ASSERT_TRUE(xr_numeric_float_to_int(18446744073709549568.0, XR_NATIVE_U64, 64, &out));
    ASSERT_EQ_UINT(xr_numeric_i64_to_bits(out), UINT64_C(0xfffffffffffff800));
    ASSERT_FALSE(xr_numeric_float_to_int(18446744073709551616.0, XR_NATIVE_U64, 64, &out));
}

TEST(bits_core_exact_width_queries) {
    ASSERT_EQ_INT(xr_bits_exact_popcount(-1, XR_NATIVE_I8), 8);
    ASSERT_EQ_INT(xr_bits_exact_popcount(-1, XR_NATIVE_I16), 16);
    ASSERT_EQ_INT(xr_bits_exact_popcount(-1, XR_NATIVE_I32), 32);
    ASSERT_EQ_INT(xr_bits_exact_popcount(-1, XR_NATIVE_I64), 64);
    ASSERT_EQ_INT(xr_bits_exact_leading_zeros(0, XR_NATIVE_U8), 8);
    ASSERT_EQ_INT(xr_bits_exact_leading_zeros(1, XR_NATIVE_U16), 15);
    ASSERT_EQ_INT(xr_bits_exact_trailing_zeros(0, XR_NATIVE_U32), 32);
    ASSERT_EQ_INT(xr_bits_exact_trailing_zeros(0x100, XR_NATIVE_U64), 8);
}

TEST(bits_not_owner_freezes_signed_64_bit_edges) {
    ASSERT_EQ_INT(xr_bits_not_i64(INT64_C(0)), -INT64_C(1));
    ASSERT_EQ_INT(xr_bits_not_i64(INT64_C(1)), -INT64_C(2));
    ASSERT_EQ_INT(xr_bits_not_i64(-INT64_C(1)), INT64_C(0));
    ASSERT_EQ_INT(xr_bits_not_i64(INT64_MAX), INT64_MIN);
    ASSERT_EQ_INT(xr_bits_not_i64(INT64_MIN), INT64_MAX);
    ASSERT_EQ_INT(XR_BITS_NOT_OWNER_APPLY(
                      XR_SEM_OWNER_ID_SHARED_BITS_NOT_HI,
                      XR_SEM_OWNER_ID_SHARED_BITS_NOT_LO, XR_SEM_CONSUMER_VM, INT64_MIN),
                  INT64_MAX);
}

TEST(bits_core_exact_width_preserves_type_pattern) {
    ASSERT_EQ_INT(xr_bits_exact_byteswap(0x12, XR_NATIVE_U8), 0x12);
    ASSERT_EQ_INT(xr_bits_exact_byteswap(0x1234, XR_NATIVE_U16), 0x3412);
    ASSERT_EQ_INT(xr_bits_exact_byteswap(0x80ff, XR_NATIVE_I16), -128);
    ASSERT_EQ_INT(xr_bits_exact_rotate_left(0x81, 1, XR_NATIVE_U8), 3);
    ASSERT_EQ_INT(xr_bits_exact_rotate_right(1, 1, XR_NATIVE_U8), 128);
    ASSERT_EQ_INT(xr_bits_exact_rotate_left(-128, -1, XR_NATIVE_I8), 64);
    ASSERT_EQ_INT(XR_BITS_EXACT_OWNER_APPLY(
                      XR_SEM_OWNER_ID_SHARED_BITS_HI, XR_SEM_OWNER_ID_SHARED_BITS_LO,
                      XR_SEM_CONSUMER_VM, xr_bits_exact_kernel_rotl, 0x81, 1, XR_NATIVE_U8),
                  3);
    ASSERT_EQ_INT(XR_BITS_EXACT_OWNER_APPLY(
                      XR_SEM_OWNER_ID_SHARED_BITS_HI, XR_SEM_OWNER_ID_SHARED_BITS_LO,
                      XR_SEM_CONSUMER_VM, xr_bits_exact_kernel_mul_high, 0xff, 2, XR_NATIVE_U8),
                  1);
}

TEST(bits_core_rotate_count_is_euclidean_mod_width) {
    static const uint8_t widths[] = {XR_NATIVE_I8,  XR_NATIVE_U16,   XR_NATIVE_I32,
                                     XR_NATIVE_U64, XR_NATIVE_ISIZE, XR_NATIVE_USIZE};
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        uint8_t native_type = widths[i];
        int64_t value = xr_bits_exact_restore(UINT64_C(0x81), native_type);
        uint8_t width = xr_bits_exact_width(native_type);
        ASSERT_EQ_INT(xr_bits_exact_rotate_left(value, -1, native_type),
                      xr_bits_exact_rotate_right(value, 1, native_type));
        ASSERT_EQ_INT(xr_bits_exact_rotate_left(value, width + 3, native_type),
                      xr_bits_exact_rotate_left(value, 3, native_type));
        ASSERT_EQ_INT(xr_bits_exact_rotate_right(xr_bits_exact_rotate_left(value, -65, native_type),
                                                 -65, native_type),
                      value);
    }
}

TEST(bits_core_aot_rotate_macros_match_exact_semantics) {
    ASSERT_EQ_INT(XR_BITS_ROTL8(UINT8_C(0x81), 1), UINT8_C(0x03));
    ASSERT_EQ_INT(XR_BITS_ROTR8(UINT8_C(0x01), 1), UINT8_C(0x80));
    ASSERT_EQ_INT(XR_BITS_ROTL16(UINT16_C(0x8001), -1), UINT16_C(0xc000));
    ASSERT_EQ_INT(XR_BITS_ROTR16(UINT16_C(0x0003), 17), UINT16_C(0x8001));
    ASSERT_EQ_INT(XR_BITS_ROTL32(UINT32_C(0x80000001), 33), UINT32_C(0x00000003));
    ASSERT_EQ_INT(XR_BITS_ROTR32(UINT32_C(0x00000003), -1), UINT32_C(0x00000006));
    ASSERT_EQ_INT(XR_BITS_ROTL64(UINT64_C(0x8000000000000001), 65), UINT64_C(3));
    ASSERT_EQ_INT(XR_BITS_ROTR64(UINT64_C(3), 1), UINT64_C(0x8000000000000001));
}

TEST(raw_scalar_core_unaligned_integer_access_preserves_bytes) {
    uint8_t bytes[32] = {0};
    uint8_t *p = bytes + 1;

    xr_raw_store_u8_unaligned(p, UINT8_C(0xa5));
    ASSERT_EQ_INT(xr_raw_load_u8_unaligned(p), UINT8_C(0xa5));

    xr_raw_store_u16_unaligned(p, xr_raw_u16_from_le(UINT16_C(0x1234)));
    ASSERT_EQ_INT(bytes[1], UINT8_C(0x34));
    ASSERT_EQ_INT(bytes[2], UINT8_C(0x12));
    ASSERT_EQ_INT(xr_raw_u16_from_le(xr_raw_load_u16_unaligned(p)), UINT16_C(0x1234));

    xr_raw_store_u32_unaligned(p, xr_raw_u32_from_be(UINT32_C(0x12345678)));
    ASSERT_EQ_INT(bytes[1], UINT8_C(0x12));
    ASSERT_EQ_INT(bytes[2], UINT8_C(0x34));
    ASSERT_EQ_INT(bytes[3], UINT8_C(0x56));
    ASSERT_EQ_INT(bytes[4], UINT8_C(0x78));
    ASSERT_EQ_INT(xr_raw_u32_from_be(xr_raw_load_u32_unaligned(p)), UINT32_C(0x12345678));

    xr_raw_store_u64_unaligned(p, xr_raw_u64_from_le(UINT64_C(0x0102030405060708)));
    ASSERT_EQ_INT(bytes[1], UINT8_C(0x08));
    ASSERT_EQ_INT(bytes[8], UINT8_C(0x01));
    ASSERT_EQ_INT(xr_raw_u64_from_le(xr_raw_load_u64_unaligned(p)), UINT64_C(0x0102030405060708));
}

TEST(raw_scalar_core_dynamic_endian_is_only_a_value_transform) {
    const uint64_t value = UINT64_C(0x0102030405060708);

    ASSERT_EQ_INT(xr_raw_u64_from_endian(value, XR_RAW_ENDIAN_NATIVE), value);
    ASSERT_EQ_INT(xr_raw_u64_from_endian(value, XR_RAW_ENDIAN_LE), xr_raw_u64_from_le(value));
    ASSERT_EQ_INT(xr_raw_u64_from_endian(value, XR_RAW_ENDIAN_BE), xr_raw_u64_from_be(value));
}

TEST(raw_scalar_core_float_and_pointer_access_preserve_bits) {
    uint8_t bytes[32] = {0};
    uint8_t *p = bytes + 1;
    const uint32_t f32_bits = UINT32_C(0x7fc01234);
    const uint64_t f64_bits = UINT64_C(0x7ff8000000001234);
    uint8_t target = 0;

    xr_raw_store_u32_unaligned(p, f32_bits);
    ASSERT_EQ_INT(xr_raw_f32_to_bits(xr_raw_f32_from_bits(xr_raw_load_u32_unaligned(p))), f32_bits);

    xr_raw_store_u64_unaligned(p, f64_bits);
    ASSERT_EQ_INT(xr_raw_f64_to_bits(xr_raw_f64_from_bits(xr_raw_load_u64_unaligned(p))), f64_bits);

    xr_raw_store_ptr_unaligned(p, &target);
    ASSERT(xr_raw_load_ptr_unaligned(p) == &target);
}

TEST(raw_scalar_access_owner_freezes_typed_load_store_matrix) {
    uint8_t bytes[40] = {0};
    uint8_t *p = bytes + 1;
    XrRawScalarValue value = {0};

    ASSERT(xr_semantic_owner_has_consumer(
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_VM));
    ASSERT_STR_EQ(xr_semantic_owner_cgen_adapter(
                      XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,
                      XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO),
                  "xrt_raw_scalar_access");

    value.bits = UINT64_C(0xffffffffffffedcc);
    ASSERT(XR_RAW_SCALAR_ACCESS_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_HI,
        XR_SEM_OWNER_ID_SHARED_RAW_SCALAR_ACCESS_LO, XR_SEM_CONSUMER_VM,
        xr_raw_scalar_store(p, XR_RAW_SCALAR_I16, sizeof(void *), XR_RAW_ENDIAN_LE, value)));
    ASSERT_EQ_INT(bytes[1], UINT8_C(0xcc));
    ASSERT_EQ_INT(bytes[2], UINT8_C(0xed));
    value.bits = 0;
    ASSERT(xr_raw_scalar_load(p, XR_RAW_SCALAR_I16, sizeof(void *), XR_RAW_ENDIAN_LE, &value));
    ASSERT_EQ_INT((int64_t) value.bits, -4660);

    value.bits = UINT64_C(0x12345678);
    ASSERT(xr_raw_scalar_store(p, XR_RAW_SCALAR_U32, sizeof(void *), XR_RAW_ENDIAN_BE, value));
    ASSERT_EQ_INT(bytes[1], UINT8_C(0x12));
    ASSERT_EQ_INT(bytes[4], UINT8_C(0x78));
    value.bits = 0;
    ASSERT(xr_raw_scalar_load(p, XR_RAW_SCALAR_U32, sizeof(void *), XR_RAW_ENDIAN_BE, &value));
    ASSERT_EQ_INT(value.bits, UINT64_C(0x12345678));

    ASSERT_FALSE(xr_raw_scalar_load(p, XR_RAW_SCALAR_VOID, sizeof(void *), XR_RAW_ENDIAN_NATIVE,
                                    &value));
    ASSERT_FALSE(xr_raw_scalar_store(p, XR_RAW_SCALAR_COUNT, sizeof(void *),
                                     XR_RAW_ENDIAN_NATIVE, value));
}

TEST(owner_forward_core_freezes_value_and_ownership_transfer) {
    XrOwnerForwardPlan vm_plan = XR_OWNER_FORWARD_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_OWNER_FORWARD_HI,
        XR_SEM_OWNER_ID_SHARED_OWNER_FORWARD_LO, XR_SEM_CONSUMER_VM);
    XrOwnerForwardPlan cgen_plan = XR_OWNER_FORWARD_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_OWNER_FORWARD_HI,
        XR_SEM_OWNER_ID_SHARED_OWNER_FORWARD_LO, XR_SEM_CONSUMER_CGEN);

    ASSERT(xr_owner_forward_plan_is_exact_core(vm_plan));
    ASSERT(xr_owner_forward_plan_is_exact_core(cgen_plan));
    ASSERT_STR_EQ(xr_semantic_owner_cgen_adapter(
                      XR_SEM_OWNER_ID_SHARED_OWNER_FORWARD_HI,
                      XR_SEM_OWNER_ID_SHARED_OWNER_FORWARD_LO),
                  "xr_owner_forward_plan_core");
}

TEST(codegen_opaque_core_freezes_value_and_optimizer_barrier) {
    XrCodegenOpaquePlan vm_plan = XR_CODEGEN_OPAQUE_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_CODEGEN_OPAQUE_HI,
        XR_SEM_OWNER_ID_SHARED_CODEGEN_OPAQUE_LO, XR_SEM_CONSUMER_VM,
        XR_CODEGEN_OPAQUE_VALUE);
    XrCodegenOpaquePlan cgen_plan = XR_CODEGEN_OPAQUE_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_CODEGEN_OPAQUE_HI,
        XR_SEM_OWNER_ID_SHARED_CODEGEN_OPAQUE_LO, XR_SEM_CONSUMER_CGEN,
        XR_CODEGEN_OPAQUE_CONST_POINTER);

    ASSERT(xr_codegen_opaque_plan_is_exact_core(vm_plan));
    ASSERT(xr_codegen_opaque_plan_is_exact_core(cgen_plan));
    ASSERT_EQ_INT(vm_plan.kind, XR_CODEGEN_OPAQUE_VALUE);
    ASSERT_EQ_INT(cgen_plan.kind, XR_CODEGEN_OPAQUE_CONST_POINTER);
    ASSERT_FALSE(xr_codegen_opaque_plan_is_exact_core(
        xr_codegen_opaque_plan_core(XR_CODEGEN_OPAQUE_KIND_COUNT)));
    ASSERT_STR_EQ(xr_semantic_owner_cgen_adapter(
                      XR_SEM_OWNER_ID_SHARED_CODEGEN_OPAQUE_HI,
                      XR_SEM_OWNER_ID_SHARED_CODEGEN_OPAQUE_LO),
                  "xr_codegen_opaque_plan_core");
}

TEST(codegen_compiler_fence_core_freezes_only_native_compiler_order) {
    XrCodegenFencePlan vm_plan = XR_CODEGEN_FENCE_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_CODEGEN_COMPILER_FENCE_HI,
        XR_SEM_OWNER_ID_SHARED_CODEGEN_COMPILER_FENCE_LO, XR_SEM_CONSUMER_VM);
    XrCodegenFencePlan cgen_plan = XR_CODEGEN_FENCE_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_CODEGEN_COMPILER_FENCE_HI,
        XR_SEM_OWNER_ID_SHARED_CODEGEN_COMPILER_FENCE_LO, XR_SEM_CONSUMER_CGEN);

    ASSERT(xr_codegen_fence_plan_is_exact_core(vm_plan));
    ASSERT(xr_codegen_fence_plan_is_exact_core(cgen_plan));
    ASSERT(vm_plan.preserves_program_state);
    ASSERT_FALSE(vm_plan.has_runtime_memory_effect);
    ASSERT(vm_plan.blocks_native_memory_reordering);
    vm_plan.has_runtime_memory_effect = true;
    ASSERT_FALSE(xr_codegen_fence_plan_is_exact_core(vm_plan));
    cgen_plan.blocks_native_memory_reordering = false;
    ASSERT_FALSE(xr_codegen_fence_plan_is_exact_core(cgen_plan));
    ASSERT_STR_EQ(xr_semantic_owner_cgen_adapter(
                      XR_SEM_OWNER_ID_SHARED_CODEGEN_COMPILER_FENCE_HI,
                      XR_SEM_OWNER_ID_SHARED_CODEGEN_COMPILER_FENCE_LO),
                  "xr_codegen_fence_plan_core");
}

TEST(copy_core_freezes_identity_clone_and_metadata_variants) {
    XrCopyPlan identity = XR_COPY_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_COPY_HI, XR_SEM_OWNER_ID_SHARED_COPY_LO,
        XR_SEM_CONSUMER_VM, XI_COPY_KIND_IDENTITY, false);
    XrCopyPlan clone = XR_COPY_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_COPY_HI, XR_SEM_OWNER_ID_SHARED_COPY_LO,
        XR_SEM_CONSUMER_CGEN, XI_COPY_KIND_VALUE_CLONE, false);
    XrCopyPlan metadata = xr_copy_plan_core(XI_COPY_KIND_IDENTITY, true);

    ASSERT(xr_copy_plan_is_exact_core(identity));
    ASSERT(xr_copy_plan_is_exact_core(clone));
    ASSERT(xr_copy_plan_is_exact_core(metadata));
    ASSERT_EQ_INT(identity.kind, XR_COPY_SEMANTIC_IDENTITY);
    ASSERT(identity.borrows_source && !identity.requires_independent_value);
    ASSERT_EQ_INT(clone.kind, XR_COPY_SEMANTIC_VALUE_CLONE);
    ASSERT(!clone.borrows_source && clone.requires_independent_value);
    ASSERT_EQ_INT(metadata.kind, XR_COPY_SEMANTIC_ENUM_METADATA_FORWARD);
    ASSERT_FALSE(xr_copy_plan_is_exact_core(xr_copy_plan_core(INT64_C(7), false)));
    ASSERT_FALSE(xr_copy_plan_is_exact_core(
        xr_copy_plan_core(XI_COPY_KIND_VALUE_CLONE, true)));
    ASSERT_STR_EQ(xr_semantic_owner_cgen_adapter(
                      XR_SEM_OWNER_ID_SHARED_COPY_HI, XR_SEM_OWNER_ID_SHARED_COPY_LO),
                  "xr_copy_plan_core");
}

TEST(static_address_core_freezes_stability_and_borrow_contract) {
    XrStaticAddressPlan readonly = XR_STATIC_ADDRESS_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_STATIC_ADDRESS_HI,
        XR_SEM_OWNER_ID_SHARED_STATIC_ADDRESS_LO, XR_SEM_CONSUMER_CGEN,
        XR_STATIC_ADDRESS_IDENTITY_MODULE, false);
    XrStaticAddressPlan mutable = xr_static_address_plan_core(
        XR_STATIC_ADDRESS_IDENTITY_SYSTEM, true);

    ASSERT(xr_static_address_plan_is_exact_core(readonly));
    ASSERT(xr_static_address_plan_is_exact_core(mutable));
    ASSERT(readonly.stable_escape && readonly.borrowed);
    ASSERT(readonly.requires_module_static_domain && !readonly.requires_mutable_storage);
    ASSERT(mutable.requires_mutable_storage && !mutable.requires_module_static_domain);
    ASSERT_FALSE(xr_static_address_plan_is_exact_core(
        xr_static_address_plan_core(XR_STATIC_ADDRESS_IDENTITY_INVALID, false)));
    ASSERT_STR_EQ(xr_semantic_owner_cgen_adapter(
                      XR_SEM_OWNER_ID_SHARED_STATIC_ADDRESS_HI,
                      XR_SEM_OWNER_ID_SHARED_STATIC_ADDRESS_LO),
                  "xr_static_address_plan_core");
}

TEST(reference_count_core_freezes_retain_and_release_contract) {
    XrReferenceCountPlan retain = XR_REFERENCE_COUNT_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_REFERENCE_COUNT_HI,
        XR_SEM_OWNER_ID_SHARED_REFERENCE_COUNT_LO, XR_SEM_CONSUMER_VM,
        XR_REFERENCE_COUNT_RETAIN);
    XrReferenceCountPlan release = XR_REFERENCE_COUNT_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_REFERENCE_COUNT_HI,
        XR_SEM_OWNER_ID_SHARED_REFERENCE_COUNT_LO, XR_SEM_CONSUMER_CGEN,
        XR_REFERENCE_COUNT_RELEASE);

    ASSERT(xr_reference_count_plan_is_exact_core(retain));
    ASSERT(xr_reference_count_plan_is_exact_core(release));
    ASSERT(retain.acquires_owner && !retain.relinquishes_owner);
    ASSERT(!release.acquires_owner && release.relinquishes_owner);
    ASSERT(release.destroys_on_last_release);
    ASSERT_FALSE(xr_reference_count_plan_is_exact_core(
        xr_reference_count_plan_core(XR_REFERENCE_COUNT_INVALID)));
    ASSERT_STR_EQ(xr_semantic_owner_cgen_adapter(
                      XR_SEM_OWNER_ID_SHARED_REFERENCE_COUNT_HI,
                      XR_SEM_OWNER_ID_SHARED_REFERENCE_COUNT_LO),
                  "xr_reference_count_plan_core");
}

TEST(atomic_load_core_freezes_ordering_without_aliases) {
    XrAtomicLoadPlan relaxed = XR_ATOMIC_LOAD_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_ATOMIC_LOAD_HI, XR_SEM_OWNER_ID_SHARED_ATOMIC_LOAD_LO,
        XR_SEM_CONSUMER_VM, 0);
    XrAtomicLoadPlan acquire = xr_atomic_load_plan_core(3);
    XrAtomicLoadPlan invalid = xr_atomic_load_plan_core(99);

    ASSERT(xr_atomic_load_plan_is_exact_core(relaxed));
    ASSERT(xr_atomic_load_plan_is_exact_core(acquire));
    ASSERT_EQ_INT(relaxed.order, XR_ATOMIC_LOAD_ORDER_RELAXED);
    ASSERT_EQ_INT(acquire.order, XR_ATOMIC_LOAD_ORDER_ACQUIRE);
    ASSERT_EQ_INT(xr_atomic_load_plan_core(4).order, XR_ATOMIC_LOAD_ORDER_SEQ_CST);
    ASSERT_FALSE(xr_atomic_load_plan_is_exact_core(invalid));
    ASSERT_EQ_INT(invalid.canonical_ordering, 4);
    ASSERT_STR_EQ(xr_semantic_owner_cgen_adapter(
                      XR_SEM_OWNER_ID_SHARED_ATOMIC_LOAD_HI,
                      XR_SEM_OWNER_ID_SHARED_ATOMIC_LOAD_LO),
                  "xr_atomic_load_plan_core");
}

TEST(atomic_store_core_freezes_ordering_without_aliases) {
    XrAtomicStorePlan relaxed = XR_ATOMIC_STORE_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_ATOMIC_STORE_HI, XR_SEM_OWNER_ID_SHARED_ATOMIC_STORE_LO,
        XR_SEM_CONSUMER_VM, 1);
    XrAtomicStorePlan release = xr_atomic_store_plan_core(3);
    XrAtomicStorePlan invalid = xr_atomic_store_plan_core(99);

    ASSERT(xr_atomic_store_plan_is_exact_core(relaxed));
    ASSERT(xr_atomic_store_plan_is_exact_core(release));
    ASSERT_EQ_INT(relaxed.order, XR_ATOMIC_STORE_ORDER_RELAXED);
    ASSERT_EQ_INT(release.order, XR_ATOMIC_STORE_ORDER_RELEASE);
    ASSERT_EQ_INT(xr_atomic_store_plan_core(4).order, XR_ATOMIC_STORE_ORDER_SEQ_CST);
    ASSERT_FALSE(xr_atomic_store_plan_is_exact_core(invalid));
    ASSERT_EQ_INT(invalid.canonical_ordering, 4);
    ASSERT_STR_EQ(xr_semantic_owner_cgen_adapter(
                      XR_SEM_OWNER_ID_SHARED_ATOMIC_STORE_HI,
                      XR_SEM_OWNER_ID_SHARED_ATOMIC_STORE_LO),
                  "xr_atomic_store_plan_core");
}

TEST(numeric_core_to_fixed_decimals_clamps) {
    ASSERT_EQ_INT(xr_numeric_core_to_fixed_decimals(-10), 0);
    ASSERT_EQ_INT(xr_numeric_core_to_fixed_decimals(3), 3);
    ASSERT_EQ_INT(xr_numeric_core_to_fixed_decimals(99), XR_TOFIXED_MAX_DECIMALS);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Numeric Core - Scalar Methods");
RUN_TEST(numeric_core_format_i64_handles_boundaries);
RUN_TEST(numeric_core_format_i64_rejects_short_buffer);
RUN_TEST(numeric_core_hex_matches_signed_magnitude_rule);
RUN_TEST(numeric_core_abs_wraps_int64_min);
RUN_TEST(numeric_core_integer_arithmetic_wraps);
RUN_TEST(numeric_neg_owner_freezes_scalar_bits_and_bigint_sign);
RUN_TEST(int_div_mod_owner_freezes_signed_unsigned_and_zero_edges);
RUN_TEST(numeric_core_shift_counts_are_mod64);
RUN_TEST(bitwise_binary_owner_freezes_scalar_and_bigint_edges);
RUN_TEST(numeric_conversion_integer_matrix_is_bit_defined);
RUN_TEST(numeric_conversion_integer_to_float_is_round_to_even);
RUN_TEST(numeric_conversion_f64_to_f32_has_frozen_edges);
RUN_TEST(numeric_conversion_float_to_integer_checks_before_cast);
RUN_TEST(bits_core_exact_width_queries);
RUN_TEST(bits_not_owner_freezes_signed_64_bit_edges);
RUN_TEST(bits_core_exact_width_preserves_type_pattern);
RUN_TEST(bits_core_rotate_count_is_euclidean_mod_width);
RUN_TEST(bits_core_aot_rotate_macros_match_exact_semantics);
RUN_TEST(raw_scalar_core_unaligned_integer_access_preserves_bytes);
RUN_TEST(raw_scalar_core_dynamic_endian_is_only_a_value_transform);
RUN_TEST(raw_scalar_core_float_and_pointer_access_preserve_bits);
RUN_TEST(raw_scalar_access_owner_freezes_typed_load_store_matrix);
RUN_TEST(owner_forward_core_freezes_value_and_ownership_transfer);
RUN_TEST(codegen_opaque_core_freezes_value_and_optimizer_barrier);
RUN_TEST(codegen_compiler_fence_core_freezes_only_native_compiler_order);
RUN_TEST(copy_core_freezes_identity_clone_and_metadata_variants);
RUN_TEST(static_address_core_freezes_stability_and_borrow_contract);
RUN_TEST(reference_count_core_freezes_retain_and_release_contract);
RUN_TEST(atomic_load_core_freezes_ordering_without_aliases);
RUN_TEST(atomic_store_core_freezes_ordering_without_aliases);
RUN_TEST(numeric_core_to_fixed_decimals_clamps);

TEST_MAIN_END()
