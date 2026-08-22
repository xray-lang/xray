/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_error_core.c - Unit tests for runtime-neutral error formatting helpers
 */

#include "../test_framework.h"
#include "shared/xr_error_core.h"

#include <string.h>

TEST(error_core_formats_array_index_oob) {
    char buf[XR_ERROR_CORE_INDEX_OOB_BUFSZ];
    int n = xr_error_core_format_array_index_oob(buf, sizeof(buf), -1, 3);

    ASSERT_TRUE(n > 0);
    ASSERT_STR_EQ(buf, "array index out of range: -1 (length 3)");
}

TEST(error_core_formats_type_mismatch) {
    char buf[XR_ERROR_CORE_TYPE_MISMATCH_BUFSZ];
    int n = xr_error_core_format_type_mismatch(buf, sizeof(buf), "i64", "f64");

    ASSERT_TRUE(n > 0);
    ASSERT_STR_EQ(buf, "TypeError: expected 'i64', got 'f64'");
}

TEST(error_core_defines_arithmetic_messages) {
    ASSERT_STR_EQ(XR_ERROR_CORE_DIVISION_BY_ZERO_MSG, "division by zero");
    ASSERT_STR_EQ(XR_ERROR_CORE_MODULO_BY_ZERO_MSG, "modulo by zero");
    ASSERT_STR_EQ(XR_ERROR_CORE_MODULO_REQUIRES_INTEGER_MSG, "modulo requires integer types");
}

TEST(error_core_defines_bytes_messages) {
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_ARRAY_CONSTRUCTOR_EXPECTS_MSG,
                  "Array<u8>(length, fill) expects integers");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_ARRAY_CONSTRUCTOR_FILL_EXPECTS_MSG,
                  "Array<u8>(length, fill): both args must be integers");
    ASSERT_STR_EQ(XR_ERROR_CORE_SLICE_BOUNDS_EXPECTS_MSG, "slice bounds must be integers");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_ENDIAN_EXPECTS_MSG,
                  "Slice<u8> load/store expects Endian");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_LOAD_OFFSET_EXPECTS_MSG,
                  "Slice<u8>.load<T>() expects integer offset");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_LOAD_U16_EXPECTS_MSG,
                  "Slice<u8>.load<u16>(offset) expects Slice<u8> and integer");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_LOAD_U16_RECEIVER_MSG,
                  "Slice<u8>.load<u16>() receiver must be Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_LOAD_U16_OOB_MSG,
                  "Slice<u8>.load<u16>() offset out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_LOAD_U32_EXPECTS_MSG,
                  "Slice<u8>.load<u32>(offset) expects Slice<u8> and integer");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_LOAD_U32_RECEIVER_MSG,
                  "Slice<u8>.load<u32>() receiver must be Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_LOAD_U32_OOB_MSG,
                  "Slice<u8>.load<u32>() offset out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_LOAD_U64_EXPECTS_MSG,
                  "Slice<u8>.load<u64>(offset) expects Slice<u8> and integer");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_LOAD_U64_RECEIVER_MSG,
                  "Slice<u8>.load<u64>() receiver must be Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_LOAD_U64_OOB_MSG,
                  "Slice<u8>.load<u64>() offset out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_LOAD_F32_RECEIVER_MSG,
                  "Slice<u8>.load<f32>() receiver must be Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_LOAD_F32_OOB_MSG,
                  "Slice<u8>.load<f32>() offset out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_LOAD_F64_RECEIVER_MSG,
                  "Slice<u8>.load<f64>() receiver must be Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_LOAD_F64_OOB_MSG,
                  "Slice<u8>.load<f64>() offset out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_STORE_VALUE_EXPECTS_MSG,
                  "Slice<u8>.store<T>() expects integer offset and value");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_STORE_FLOAT_VALUE_EXPECTS_MSG,
                  "Slice<u8>.store<T>() expects integer offset and f64 value");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_STORE_U16_RECEIVER_MSG,
                  "Slice<u8>.store<u16>() receiver must be Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_STORE_U16_OOB_MSG,
                  "Slice<u8>.store<u16>() offset out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_STORE_U32_RECEIVER_MSG,
                  "Slice<u8>.store<u32>() receiver must be Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_STORE_U32_OOB_MSG,
                  "Slice<u8>.store<u32>() offset out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_STORE_U64_RECEIVER_MSG,
                  "Slice<u8>.store<u64>() receiver must be Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_STORE_U64_OOB_MSG,
                  "Slice<u8>.store<u64>() offset out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_STORE_F32_RECEIVER_MSG,
                  "Slice<u8>.store<f32>() receiver must be Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_STORE_F32_OOB_MSG,
                  "Slice<u8>.store<f32>() offset out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_STORE_F64_RECEIVER_MSG,
                  "Slice<u8>.store<f64>() receiver must be Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_STORE_F64_OOB_MSG,
                  "Slice<u8>.store<f64>() offset out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_READONLY_MSG,
                  "cannot write through readonly Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_FILL_RECEIVER_MSG,
                  "Slice<u8>.fill(value) expects Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_FILL_VALUE_EXPECTS_MSG,
                  "Slice<u8>.fill(value) expects integer byte value");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_FILL_OOB_MSG,
                  "Slice<u8>.fill(value) range out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_COPY_RECEIVER_MSG,
                  "Slice<u8>.copyFrom(src) receiver must be Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_COPY_SOURCE_MSG,
                  "Slice<u8>.copyFrom(src) source must be Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_COPY_OOB_MSG,
                  "Slice<u8>.copyFrom(src) range out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_COMPARE_RECEIVER_MSG,
                  "Slice<u8>.compare(other) receiver must be Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_COMPARE_OPERAND_MSG,
                  "Slice<u8>.compare(other) operand must be Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_COMPARE_NO_DATA_MSG,
                  "Slice<u8>.compare(other) span has no data");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_COMMON_PREFIX_RECEIVER_MSG,
                  "Slice<u8>.commonPrefix(other) receiver must be Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_COMMON_PREFIX_OPERAND_MSG,
                  "Slice<u8>.commonPrefix(other) operand must be Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_COMMON_PREFIX_NO_DATA_MSG,
                  "Slice<u8>.commonPrefix(other) span has no data");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_REPEAT_RECEIVER_MSG,
                  "Slice<u8>.repeatFrom(dstOffset, distance, count) expects Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_REPEAT_INTS_EXPECTS_MSG,
                  "Slice<u8>.repeatFrom(dstOffset, distance, count) expects integers");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_REPEAT_OOB_MSG,
                  "Slice<u8>.repeatFrom(dstOffset, distance, count) range out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_ARG_EXPECTS_MSG,
                  "Slice<u8> argument expects Array<u8> or Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_MISSING_METADATA_MSG,
                  "Slice<u8>.reinterpret<T>() missing metadata");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_REQUIRES_POD_MSG,
                  "Slice<u8>.reinterpret<T>() requires POD target type");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_METADATA_MISMATCH_MSG,
                  "Slice<u8>.reinterpret<T>() target metadata mismatch");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_EXPECTS_MSG,
                  "Slice<u8>.reinterpret<T>() expects Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_RECEIVER_MSG,
                  "Slice<u8>.reinterpret<T>() expects Slice<u8> receiver");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_OVERFLOW_MSG,
                  "Slice<u8>.reinterpret<T>() byte length overflow");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_DIVISIBLE_MSG,
                  "Slice<u8>.reinterpret<T>() length is not divisible by target size");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_ARRAY_COPY_WITHIN_EXPECTS_MSG,
                  "Array<u8> copy-within expects integer offsets and count");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_ARRAY_COPY_WITHIN_RECEIVER_MSG,
                  "Array<u8> copy-within receiver must be Array<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_ARRAY_COPY_WITHIN_OOB_MSG,
                  "Array<u8> copy-within range out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_ARRAY_COPY_FROM_EXPECTS_MSG,
                  "Array<u8> copy range expects Array<u8> operands and integer ranges");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_ARRAY_COPY_FROM_OPERANDS_MSG,
                  "Array<u8> copy range operands must be Array<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_ARRAY_COPY_FROM_OOB_MSG,
                  "Array<u8> copy range out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_ARRAY_APPEND_FROM_EXPECTS_MSG,
                  "Array<u8>.appendFrom(src) expects Slice<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_ARRAY_APPEND_FROM_OPERANDS_MSG,
                  "Array<u8>.appendFrom receiver/source must use byte storage");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_ARRAY_APPEND_FROM_OOB_MSG,
                  "Array<u8>.appendFrom range/grow failed");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_EXPECTS_MSG,
                  "Array<u8>.repeatFrom(distance, count) expects integers");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_RECEIVER_MSG,
                  "Array<u8>.repeatFrom receiver must be Array<u8>");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_OOB_MSG,
                  "Array<u8>.repeatFrom range/grow failed");
}

TEST(error_core_defines_array_messages) {
    ASSERT_STR_EQ(XR_ERROR_CORE_ARRAY_CAPACITY_EXPECTS_MSG, "Array capacity must be an integer");
    ASSERT_STR_EQ(XR_ERROR_CORE_ARRAY_RESERVE_EXPECTS_MSG,
                  "Array.reserve(capacity) expects an integer");
    ASSERT_STR_EQ(XR_ERROR_CORE_ARRAY_RESERVE_FAILED_MSG, "Array.reserve failed");
    ASSERT_STR_EQ(XR_ERROR_CORE_ARRAY_RESIZE_EXPECTS_MSG,
                  "Array.resize(length, fill) expects integer length");
    ASSERT_STR_EQ(XR_ERROR_CORE_ARRAY_RESIZE_REQUIRES_FILL_MSG,
                  "Array.resize(length, fill) requires fill value");
    ASSERT_STR_EQ(XR_ERROR_CORE_ARRAY_RESIZE_FAILED_MSG, "Array.resize failed");
    ASSERT_STR_EQ(XR_ERROR_CORE_ARRAY_SLICE_PUSH_MSG, "cannot push to array slice");
}

TEST(error_core_defines_range_messages) {
    ASSERT_STR_EQ(XR_ERROR_CORE_RANGE_TO_ARRAY_TOO_LARGE_MSG, "Range.toArray range too large");
}

TEST(error_core_parses_prefixed_message) {
    const char *text = "E0430: array index out of range: 5 (length 3)";
    const char *expected = "array index out of range: 5 (length 3)";
    XrErrorCoreMessageView view = xr_error_core_parse_prefixed(text, strlen(text));

    ASSERT_TRUE(view.has_code);
    ASSERT_EQ_INT(view.code, 430);
    ASSERT_EQ_INT((int) view.message_len, (int) strlen(expected));
    ASSERT_TRUE(strncmp(view.message, expected, view.message_len) == 0);
}

TEST(error_core_leaves_unprefixed_message_intact) {
    const char *text = "division by zero";
    XrErrorCoreMessageView view = xr_error_core_parse_prefixed(text, strlen(text));

    ASSERT_FALSE(view.has_code);
    ASSERT_EQ_INT(view.code, 0);
    ASSERT_EQ_INT((int) view.message_len, 16);
    ASSERT_TRUE(view.message == text);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Error Core");
RUN_TEST(error_core_formats_array_index_oob);
RUN_TEST(error_core_formats_type_mismatch);
RUN_TEST(error_core_defines_arithmetic_messages);
RUN_TEST(error_core_defines_bytes_messages);
RUN_TEST(error_core_defines_array_messages);
RUN_TEST(error_core_defines_range_messages);
RUN_TEST(error_core_parses_prefixed_message);
RUN_TEST(error_core_leaves_unprefixed_message_intact);

TEST_MAIN_END()
