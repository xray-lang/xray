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
    int n = xr_error_core_format_type_mismatch(buf, sizeof(buf), "int", "float");

    ASSERT_TRUE(n > 0);
    ASSERT_STR_EQ(buf, "TypeError: expected 'int', got 'float'");
}

TEST(error_core_defines_arithmetic_messages) {
    ASSERT_STR_EQ(XR_ERROR_CORE_DIVISION_BY_ZERO_MSG, "division by zero");
    ASSERT_STR_EQ(XR_ERROR_CORE_MODULO_BY_ZERO_MSG, "modulo by zero");
    ASSERT_STR_EQ(XR_ERROR_CORE_MODULO_REQUIRES_INTEGER_MSG, "modulo requires integer types");
}

TEST(error_core_defines_bytes_messages) {
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_CONSTRUCTOR_EXPECTS_MSG,
                  "Bytes(n): n must be integer or array");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_CONSTRUCTOR_FILL_EXPECTS_MSG,
                  "Bytes(n, value): both args must be integers");
    ASSERT_STR_EQ(XR_ERROR_CORE_SLICE_BOUNDS_EXPECTS_MSG, "slice bounds must be integers");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_LOAD_U16_EXPECTS_MSG,
                  "Bytes.loadU16LE(offset) expects Bytes and integer");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_LOAD_U16_RECEIVER_MSG,
                  "Bytes.loadU16LE receiver must be Bytes");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_LOAD_U16_OOB_MSG, "Bytes.loadU16LE offset out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_LOAD_U32_EXPECTS_MSG,
                  "Bytes.loadU32LE(offset) expects Bytes and integer");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_LOAD_U32_RECEIVER_MSG,
                  "Bytes.loadU32LE receiver must be Bytes");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_LOAD_U32_OOB_MSG, "Bytes.loadU32LE offset out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_LOAD_U64_EXPECTS_MSG,
                  "Bytes.loadU64LE(offset) expects Bytes and integer");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_LOAD_U64_RECEIVER_MSG,
                  "Bytes.loadU64LE receiver must be Bytes");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_LOAD_U64_OOB_MSG, "Bytes.loadU64LE offset out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_COPY_WITHIN_EXPECTS_MSG,
                  "Bytes.copyWithin expects integer offsets and count");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_COPY_WITHIN_RECEIVER_MSG,
                  "Bytes.copyWithin receiver must be Bytes");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_COPY_WITHIN_OOB_MSG, "Bytes.copyWithin range out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_COPY_FROM_EXPECTS_MSG,
                  "Bytes.copyFrom expects Bytes and integer ranges");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_COPY_FROM_OPERANDS_MSG,
                  "Bytes.copyFrom operands must be Bytes");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_COPY_FROM_OOB_MSG, "Bytes.copyFrom range out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_REPEAT_FROM_EXPECTS_MSG,
                  "Bytes.repeatFrom expects integer offsets and count");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_REPEAT_FROM_RECEIVER_MSG,
                  "Bytes.repeatFrom receiver must be Bytes");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_REPEAT_FROM_OOB_MSG, "Bytes.repeatFrom range out of bounds");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_APPEND_FROM_UNCHECKED_EXPECTS_MSG,
                  "Bytes.appendFromUnchecked(src, srcOffset, count) expects ByteSpan and integers");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_APPEND_FROM_UNCHECKED_OPERANDS_MSG,
                  "Bytes.appendFromUnchecked receiver/source must use byte storage");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_APPEND_FROM_UNCHECKED_OOB_MSG,
                  "Bytes.appendFromUnchecked range/capacity precondition failed");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_REPEAT_FROM_UNCHECKED_EXPECTS_MSG,
                  "Bytes.repeatFromUnchecked(distance, count) expects integer distance and count");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_REPEAT_FROM_UNCHECKED_RECEIVER_MSG,
                  "Bytes.repeatFromUnchecked receiver must be Bytes");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_REPEAT_FROM_UNCHECKED_OOB_MSG,
                  "Bytes.repeatFromUnchecked range/capacity precondition failed");
    ASSERT_STR_EQ(
        XR_ERROR_CORE_BYTES_WRITE_FROM_UNCHECKED_EXPECTS_MSG,
        "Bytes.writeFromUnchecked(dstOffset, src, srcOffset, count) expects ByteSpan and integers");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_WRITE_FROM_UNCHECKED_OPERANDS_MSG,
                  "Bytes.writeFromUnchecked receiver/source must use byte storage");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_WRITE_FROM_UNCHECKED_OOB_MSG,
                  "Bytes.writeFromUnchecked range/capacity precondition failed");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_REPEAT_AT_UNCHECKED_EXPECTS_MSG,
                  "Bytes.repeatAtUnchecked(dstOffset, distance, count) expects integers");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_REPEAT_AT_UNCHECKED_RECEIVER_MSG,
                  "Bytes.repeatAtUnchecked receiver must be Bytes");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_REPEAT_AT_UNCHECKED_OOB_MSG,
                  "Bytes.repeatAtUnchecked range/capacity precondition failed");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_SET_LENGTH_UNCHECKED_EXPECTS_MSG,
                  "Bytes.setLengthUnchecked(length) expects integer length");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_SET_LENGTH_UNCHECKED_RECEIVER_MSG,
                  "Bytes.setLengthUnchecked receiver must be Bytes");
    ASSERT_STR_EQ(XR_ERROR_CORE_BYTES_SET_LENGTH_UNCHECKED_OOB_MSG,
                  "Bytes.setLengthUnchecked range/capacity precondition failed");
}

TEST(error_core_defines_array_messages) {
    ASSERT_STR_EQ(XR_ERROR_CORE_ARRAY_CAPACITY_EXPECTS_MSG, "Array capacity must be an integer");
    ASSERT_STR_EQ(XR_ERROR_CORE_ARRAY_RESERVE_EXPECTS_MSG,
                  "Array.reserve(capacity) expects an integer");
    ASSERT_STR_EQ(XR_ERROR_CORE_ARRAY_RESERVE_FAILED_MSG, "Array.reserve failed");
    ASSERT_STR_EQ(XR_ERROR_CORE_ARRAY_RESIZE_EXPECTS_MSG,
                  "Array.resize(length, fill) expects integer length");
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
