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
RUN_TEST(error_core_parses_prefixed_message);
RUN_TEST(error_core_leaves_unprefixed_message_intact);

TEST_MAIN_END()
