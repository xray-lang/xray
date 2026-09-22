/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_value_format_core.c - Unit tests for shared value formatting limits
 */

#include "../test_framework.h"
#include "shared/xr_value_format_core.h"

TEST(value_format_limits_are_stable) {
    ASSERT_EQ_INT(XR_VALUE_FORMAT_MAX_DEPTH, 3);
    ASSERT_EQ_INT(XR_VALUE_FORMAT_MAX_ELEMENTS, 32);
}

TEST(value_format_core_clamps_counts_and_depth) {
    ASSERT_EQ_INT(xr_value_format_depth_exceeded(3), 0);
    ASSERT_EQ_INT(xr_value_format_depth_exceeded(4), 1);
    ASSERT_EQ_INT((int) xr_value_format_limit_count(-1), 0);
    ASSERT_EQ_INT((int) xr_value_format_limit_count(8), 8);
    ASSERT_EQ_INT((int) xr_value_format_limit_count(40), 32);
    ASSERT_EQ_INT((int) xr_value_format_remaining_count(40, 32), 8);
    ASSERT_EQ_INT((int) xr_value_format_remaining_count(12, 32), 0);
}

TEST(value_format_core_writes_more_suffix) {
    char buf[32];
    int n = xr_value_format_more_suffix(buf, sizeof(buf), 40, 32);
    ASSERT_STR_EQ(buf, ", ...(8 more)");
    ASSERT_EQ_INT(n, 13);

    n = xr_value_format_more_suffix(buf, sizeof(buf), 12, 12);
    ASSERT_EQ_INT(n, 0);
    ASSERT_STR_EQ(buf, "");
}

typedef struct FormatFixture {
    XrValueFormatView view;
    const struct FormatFixture *children;
} FormatFixture;

typedef struct FormatCapture {
    char bytes[8192];
    size_t size;
    size_t capacity;
} FormatCapture;

static int read_fixture(const void *context, XrValueFormatNode node, XrValueFormatView *view) {
    (void) context;
    if (!node.value)
        return 0;
    *view = ((const FormatFixture *) node.value)->view;
    return 1;
}

static int child_fixture(const void *context, XrValueFormatNode node, uint32_t index,
                          XrValueFormatNode *child) {
    (void) context;
    const FormatFixture *parent = node.value;
    if (!parent || !parent->children || index >= parent->view.children)
        return 0;
    child->value = &parent->children[index];
    return 1;
}

static int capture_bytes(void *context, const void *bytes, size_t size) {
    FormatCapture *capture = context;
    if (size >= capture->capacity - capture->size)
        return 0;
    if (size)
        memcpy(capture->bytes + capture->size, bytes, size);
    capture->size += size;
    capture->bytes[capture->size] = '\0';
    return 1;
}

TEST(value_format_preserves_typed_error_payload) {
    const FormatFixture payload[] = {
        {.view = {.kind = XR_VALUE_FORMAT_SIGNED, .signed_value = INT64_MIN}},
        {.view = {.kind = XR_VALUE_FORMAT_UNSIGNED, .unsigned_value = UINT64_MAX}},
        {.view = {.kind = XR_VALUE_FORMAT_BOOL, .unsigned_value = 1}},
        {.view = {.kind = XR_VALUE_FORMAT_BYTES, .bytes = (const unsigned char *) "detail",
                  .size = 6u, .quote = '"'}},
        {.view = {.kind = XR_VALUE_FORMAT_ENUM, .name = "Nested", .member = "Empty"}},
    };
    const FormatFixture root = {
        .view = {.kind = XR_VALUE_FORMAT_ENUM, .name = "Failure", .member = "Failed", .children = 5},
        .children = payload,
    };
    XrValueFormatReader reader = {NULL, read_fixture, child_fixture};
    FormatCapture capture = {.capacity = sizeof(capture.bytes)};
    XrValueFormatSink sink = {&capture, capture_bytes};
    ASSERT_TRUE(xr_value_format_uncaught(reader, (XrValueFormatNode) {&root, 0}, sink, 0));
    ASSERT_STR_EQ(capture.bytes,
        "[Uncaught Error] Failure.Failed(-9223372036854775808, 18446744073709551615, true, "
        "\"detail\", Nested.Empty)\n");
    capture.size = 0;
    ASSERT_TRUE(xr_value_format_value(reader, (XrValueFormatNode) {&root, 0}, sink, 4));
    ASSERT_STR_EQ(capture.bytes, "...");
    capture.size = 0;
    capture.capacity = 8;
    ASSERT_FALSE(xr_value_format_uncaught(reader, (XrValueFormatNode) {&root, 0}, sink, 0));
    ASSERT_EQ_INT(capture.size, 0);
    ASSERT_FALSE(xr_value_format_value(reader, (XrValueFormatNode) {NULL, 0}, sink, 0));
}

TEST(value_format_streams_long_and_embedded_zero_strings) {
    unsigned char bytes[4096];
    memset(bytes, 'x', sizeof(bytes));
    bytes[24] = 0;
    FormatFixture root = {.view = {.kind = XR_VALUE_FORMAT_BYTES,
        .bytes = bytes, .size = sizeof(bytes), .quote = '"'}};
    FormatCapture capture = {.capacity = sizeof(capture.bytes)};
    XrValueFormatReader reader = {NULL, read_fixture, child_fixture};
    XrValueFormatSink sink = {&capture, capture_bytes};
    ASSERT_TRUE(xr_value_format_value(reader, (XrValueFormatNode) {&root, 0}, sink, 0));
    ASSERT_EQ_INT(capture.size, sizeof(bytes));
    ASSERT_TRUE(memcmp(capture.bytes, bytes, sizeof(bytes)) == 0);
    capture.size = 0;
    ASSERT_TRUE(xr_value_format_value(reader, (XrValueFormatNode) {&root, 0}, sink, 1));
    ASSERT_EQ_INT(capture.size, sizeof(bytes) + 2);
    ASSERT_TRUE(capture.bytes[0] == '"' && capture.bytes[sizeof(bytes) + 1] == '"');
    ASSERT_TRUE(memcmp(capture.bytes + 1, bytes, sizeof(bytes)) == 0);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Value Format Core");
RUN_TEST(value_format_limits_are_stable);
RUN_TEST(value_format_core_clamps_counts_and_depth);
RUN_TEST(value_format_core_writes_more_suffix);
RUN_TEST(value_format_preserves_typed_error_payload);
RUN_TEST(value_format_streams_long_and_embedded_zero_strings);

TEST_MAIN_END()
