/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_diag_fmt.c - Unit tests for diagnostic formatting helpers
 *
 * KEY CONCEPT:
 *   Tests line finding, digit counting, and source context helpers
 *   used by the compiler diagnostic display system.
 */

#include "../test_framework.h"
#include "frontend/xdiag_fmt.h"

/* ========== Find Line Start ========== */

TEST(diag_find_line_start_first_line) {
    const char *src = "hello world\nsecond line";
    const char *pos = src + 3;  // 'l' in "hello"
    const char *start = xr_diag_find_line_start(src, pos);
    ASSERT_EQ_PTR(start, src);
}

TEST(diag_find_line_start_second_line) {
    const char *src = "first\nsecond\nthird";
    const char *pos = src + 8;  // 'c' in "second"
    const char *start = xr_diag_find_line_start(src, pos);
    ASSERT_EQ_PTR(start, src + 6);  // after first '\n'
}

TEST(diag_find_line_start_third_line) {
    const char *src = "a\nb\ncdef";
    const char *pos = src + 5;  // 'd' in "cdef"
    const char *start = xr_diag_find_line_start(src, pos);
    ASSERT_EQ_PTR(start, src + 4);  // after second '\n'
}

TEST(diag_find_line_start_at_newline) {
    const char *src = "line1\nline2";
    const char *pos = src + 6;  // 'l' in "line2"
    const char *start = xr_diag_find_line_start(src, pos);
    ASSERT_EQ_PTR(start, src + 6);
}

/* ========== Find Line End ========== */

TEST(diag_find_line_end_first_line) {
    const char *src = "hello\nworld";
    const char *end = xr_diag_find_line_end(src);
    ASSERT_EQ_PTR(end, src + 5);  // points to '\n'
}

TEST(diag_find_line_end_last_line) {
    const char *src = "hello\nworld";
    const char *end = xr_diag_find_line_end(src + 6);  // "world"
    ASSERT_EQ_PTR(end, src + 11);  // points to '\0'
}

TEST(diag_find_line_end_empty) {
    const char *src = "";
    const char *end = xr_diag_find_line_end(src);
    ASSERT_EQ_PTR(end, src);  // points to '\0'
}

/* ========== Num Digits ========== */

TEST(diag_num_digits) {
    ASSERT_EQ_INT(xr_diag_num_digits(0), 1);
    ASSERT_EQ_INT(xr_diag_num_digits(1), 1);
    ASSERT_EQ_INT(xr_diag_num_digits(9), 1);
    ASSERT_EQ_INT(xr_diag_num_digits(10), 2);
    ASSERT_EQ_INT(xr_diag_num_digits(99), 2);
    ASSERT_EQ_INT(xr_diag_num_digits(100), 3);
    ASSERT_EQ_INT(xr_diag_num_digits(999), 3);
    ASSERT_EQ_INT(xr_diag_num_digits(1000), 4);
    ASSERT_EQ_INT(xr_diag_num_digits(9999), 4);
    ASSERT_EQ_INT(xr_diag_num_digits(10000), 5);
    ASSERT_EQ_INT(xr_diag_num_digits(99999), 5);
}

/* ========== Diag Level Enum ========== */

TEST(diag_level_values) {
    ASSERT_EQ_INT(XR_DIAG_ERROR, 0);
    ASSERT_EQ_INT(XR_DIAG_WARNING, 1);
    ASSERT_EQ_INT(XR_DIAG_NOTE, 2);
    ASSERT_TRUE(XR_DIAG_ERROR < XR_DIAG_WARNING);
    ASSERT_TRUE(XR_DIAG_WARNING < XR_DIAG_NOTE);
}

/* ========== Line Operations on Multi-line Source ========== */

TEST(diag_line_operations) {
    const char *src = "fn main() {\n    let x = 42\n    print(x)\n}";
    // Line 2: "    let x = 42"
    const char *line2_start = xr_diag_find_line_start(src, src + 16);
    const char *line2_end = xr_diag_find_line_end(line2_start);

    int line_len = (int)(line2_end - line2_start);
    ASSERT_EQ_INT(line_len, 14);  // "    let x = 42"
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

    RUN_TEST_SUITE("DiagFmt - Find Line Start");
    RUN_TEST(diag_find_line_start_first_line);
    RUN_TEST(diag_find_line_start_second_line);
    RUN_TEST(diag_find_line_start_third_line);
    RUN_TEST(diag_find_line_start_at_newline);

    RUN_TEST_SUITE("DiagFmt - Find Line End");
    RUN_TEST(diag_find_line_end_first_line);
    RUN_TEST(diag_find_line_end_last_line);
    RUN_TEST(diag_find_line_end_empty);

    RUN_TEST_SUITE("DiagFmt - Num Digits");
    RUN_TEST(diag_num_digits);

    RUN_TEST_SUITE("DiagFmt - Level Enum");
    RUN_TEST(diag_level_values);

    RUN_TEST_SUITE("DiagFmt - Line Operations");
    RUN_TEST(diag_line_operations);

TEST_MAIN_END()
