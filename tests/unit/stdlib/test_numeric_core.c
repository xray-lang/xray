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
#include "shared/xr_numeric_conversion_core.h"
#include "shared/xr_numeric_core.h"
#include "shared/xr_raw_scalar_core.h"
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

TEST(numeric_core_math_abs_preserves_int_or_promotes_min) {
    XrNumericCoreI64AbsResult zero = xr_numeric_core_i64_math_abs(0);
    ASSERT_FALSE(zero.is_float);
    ASSERT_EQ_INT(zero.int_value, 0);

    XrNumericCoreI64AbsResult neg = xr_numeric_core_i64_math_abs(-42);
    ASSERT_FALSE(neg.is_float);
    ASSERT_EQ_INT(neg.int_value, 42);

    XrNumericCoreI64AbsResult min = xr_numeric_core_i64_math_abs(INT64_MIN);
    ASSERT_TRUE(min.is_float);
    ASSERT(min.float_value == (double) INT64_MAX + 1.0);
}

TEST(numeric_core_integer_arithmetic_wraps) {
    ASSERT_EQ_INT(xr_numeric_core_i64_add_wrap(INT64_MAX, 1), INT64_MIN);
    ASSERT_EQ_INT(xr_numeric_core_i64_sub_wrap(INT64_MIN, 1), INT64_MAX);
    ASSERT_EQ_INT(xr_numeric_core_i64_mul_wrap(INT64_MAX, 2), -2);
    ASSERT_EQ_INT(xr_numeric_core_i64_neg_wrap(INT64_MIN), INT64_MIN);
}

TEST(numeric_core_integer_div_mod_edges_match_language) {
    ASSERT_EQ_INT(xr_numeric_core_i64_div_wrap(INT64_MIN, -1), INT64_MIN);
    ASSERT_EQ_INT(xr_numeric_core_i64_mod_wrap(INT64_MIN, -1), 0);
    ASSERT_EQ_INT(xr_numeric_core_i64_div_wrap(7, -3), -2);
    ASSERT_EQ_INT(xr_numeric_core_i64_mod_wrap(7, -3), 1);
}

TEST(numeric_core_shift_counts_are_mod64) {
    ASSERT_EQ_INT(xr_numeric_core_i64_shl_wrap(12345, 64), 12345);
    ASSERT_EQ_INT(xr_numeric_core_i64_shl_wrap(12345, 65), 24690);
    ASSERT_EQ_INT(xr_numeric_core_i64_shr_wrap(12345, 70), 192);
    ASSERT_EQ_INT(xr_numeric_core_i64_shl_wrap(1, -1), INT64_MIN);
    ASSERT_EQ_INT(xr_numeric_core_i64_shr_wrap(-8, 1), -4);
    ASSERT_EQ_INT(xr_numeric_core_i64_shr_wrap(INT64_MIN, 63), -1);
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
RUN_TEST(numeric_core_math_abs_preserves_int_or_promotes_min);
RUN_TEST(numeric_core_integer_arithmetic_wraps);
RUN_TEST(numeric_core_integer_div_mod_edges_match_language);
RUN_TEST(numeric_core_shift_counts_are_mod64);
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
RUN_TEST(numeric_core_to_fixed_decimals_clamps);

TEST_MAIN_END()
