/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_utf8_diagnostic.c - Shared strict and maximal-subpart UTF-8 core tests
 */

#include "../test_framework.h"
#include "base/xutf8.h"
#include <string.h>

static void assert_scan(const uint8_t *data, size_t len, XrUtf8ErrorKind error, size_t offset,
                        size_t invalid_length, size_t rune_count) {
    XrUtf8ScanResult scan = xr_utf8_scan_strict(data, len);
    ASSERT_EQ_INT(scan.error, error);
    ASSERT_EQ_UINT(scan.byte_offset, offset);
    ASSERT_EQ_UINT(scan.invalid_length, invalid_length);
    ASSERT_EQ_UINT(scan.rune_count, rune_count);
}

TEST(utf8_diagnostic_valid_and_count) {
    static const uint8_t valid[] = {'A', 0xC3, 0xA9, 0xE4, 0xB8, 0xAD, 0xF0, 0x9F, 0x98, 0x80};
    assert_scan(NULL, 0, XR_UTF8_OK, 0, 0, 0);
    assert_scan(valid, sizeof(valid), XR_UTF8_OK, sizeof(valid), 0, 4);
}

TEST(utf8_diagnostic_error_payloads) {
    static const uint8_t overlong2[] = {0xC0, 0xAF};
    static const uint8_t overlong3[] = {0xE0, 0x80, 0xBF};
    static const uint8_t overlong4[] = {0xF0, 0x81, 0x82, 0x83};
    static const uint8_t surrogate[] = {0xED, 0xA0, 0x80};
    static const uint8_t out_of_range4[] = {0xF4, 0x91, 0x92, 0x93};
    static const uint8_t out_of_range_lead[] = {0xFF};
    static const uint8_t stray[] = {'x', 0x80};

    assert_scan(overlong2, sizeof(overlong2), XR_UTF8_OVERLONG, 0, 2, 0);
    assert_scan(overlong3, sizeof(overlong3), XR_UTF8_OVERLONG, 0, 3, 0);
    assert_scan(overlong4, sizeof(overlong4), XR_UTF8_OVERLONG, 0, 4, 0);
    assert_scan(surrogate, sizeof(surrogate), XR_UTF8_SURROGATE, 0, 3, 0);
    assert_scan(out_of_range4, sizeof(out_of_range4), XR_UTF8_OUT_OF_RANGE, 0, 4, 0);
    assert_scan(out_of_range_lead, sizeof(out_of_range_lead), XR_UTF8_OUT_OF_RANGE, 0, 1, 0);
    assert_scan(stray, sizeof(stray), XR_UTF8_STRAY_CONTINUATION, 1, 1, 1);
}

TEST(utf8_diagnostic_truncated_prefixes) {
    static const uint8_t two[] = {0xC2};
    static const uint8_t three[] = {0xE1, 0x80};
    static const uint8_t four[] = {0xF1, 0x80, 0x80};
    static const uint8_t bad_third[] = {0xE1, 0x80, 'A'};

    assert_scan(two, sizeof(two), XR_UTF8_TRUNCATED, 0, 1, 0);
    assert_scan(three, sizeof(three), XR_UTF8_TRUNCATED, 0, 2, 0);
    assert_scan(four, sizeof(four), XR_UTF8_TRUNCATED, 0, 3, 0);
    assert_scan(bad_third, sizeof(bad_third), XR_UTF8_TRUNCATED, 0, 2, 0);
}

static void assert_steps(const uint8_t *data, size_t len, const size_t *consumed, size_t count) {
    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        ASSERT_TRUE(pos < len);
        XrUtf8Step step = xr_utf8_decode_step(data + pos, len - pos);
        ASSERT_EQ_UINT(step.consumed, consumed[i]);
        if (i + 1 == count)
            ASSERT_EQ_INT(step.error, XR_UTF8_OK);
        else
            ASSERT_TRUE(step.error != XR_UTF8_OK);
        pos += step.consumed;
    }
    ASSERT_EQ_UINT(pos, len);
}

TEST(utf8_maximal_subpart_unicode_tables) {
    /* Unicode 17, Tables 3-8 through 3-11. */
    static const uint8_t non_shortest[] = {0xC0, 0xAF, 0xE0, 0x80, 0xBF, 0xF0, 0x81, 0x82, 0x41};
    static const size_t non_shortest_steps[] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    static const uint8_t surrogate[] = {0xED, 0xA0, 0x80, 0x41};
    static const size_t surrogate_steps[] = {1, 1, 1, 1};
    static const uint8_t truncated[] = {0xE1, 0x80, 0xE2, 0xF0, 0x91, 0x92, 0xF1, 0xBF, 0x41};
    static const size_t truncated_steps[] = {2, 1, 3, 2, 1};

    assert_steps(non_shortest, sizeof(non_shortest), non_shortest_steps,
                 sizeof(non_shortest_steps) / sizeof(non_shortest_steps[0]));
    assert_steps(surrogate, sizeof(surrogate), surrogate_steps,
                 sizeof(surrogate_steps) / sizeof(surrogate_steps[0]));
    assert_steps(truncated, sizeof(truncated), truncated_steps,
                 sizeof(truncated_steps) / sizeof(truncated_steps[0]));
}

static void assert_lossy(const uint8_t *data, size_t len, const char *expected,
                         size_t expected_runes) {
    XrUtf8LossyPlan plan = xr_utf8_core_lossy_plan(data, len);
    ASSERT_FALSE(plan.overflow);
    ASSERT_EQ_UINT(plan.output_length, strlen(expected));
    ASSERT_EQ_UINT(plan.rune_count, expected_runes);

    char out[64];
    ASSERT_TRUE(plan.output_length < sizeof(out));
    size_t written = xr_utf8_core_lossy_write(out, data, len);
    ASSERT_EQ_UINT(written, plan.output_length);
    out[written] = '\0';
    ASSERT(strcmp(out, expected) == 0);
}

TEST(utf8_lossy_plan_and_write) {
    static const uint8_t valid[] = {'A', 0xC3, 0xA9, 0xE4, 0xB8, 0xAD};
    static const uint8_t mixed[] = {'A', 0xFF, 'B'};
    static const uint8_t truncated[] = {0xE1, 0x80, 0xE2, 0xF0, 0x91, 0x92, 0xF1, 0xBF, 'A'};

    assert_lossy(valid, sizeof(valid), "Aé中", 3);
    assert_lossy(mixed, sizeof(mixed), "A�B", 3);
    assert_lossy(truncated, sizeof(truncated), "����A", 5);
}

TEST(utf8_validate_uses_diagnostic_core) {
    static const char valid[] = "A\xC3\xA9\xF0\x9F\x98\x80";
    static const char invalid[] = "\xED\xA0\x80";
    ASSERT_TRUE(xr_utf8_validate(valid, sizeof(valid) - 1));
    ASSERT_FALSE(xr_utf8_validate(invalid, sizeof(invalid) - 1));
}

static void run_all_tests(void) {
    RUN_TEST(utf8_diagnostic_valid_and_count);
    RUN_TEST(utf8_diagnostic_error_payloads);
    RUN_TEST(utf8_diagnostic_truncated_prefixes);
    RUN_TEST(utf8_maximal_subpart_unicode_tables);
    RUN_TEST(utf8_lossy_plan_and_write);
    RUN_TEST(utf8_validate_uses_diagnostic_core);
}

TEST_MAIN_BEGIN()
printf("=== xray UTF-8 Diagnostic Core Tests ===\n");
run_all_tests();
TEST_MAIN_END()
