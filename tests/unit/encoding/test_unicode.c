/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_unicode.c - Unit tests for Unicode character classification
 *
 * KEY CONCEPT:
 *   Tests Unicode property lookup, character classification (letter, number,
 *   whitespace, punctuation), case conversion, and CJK/script detection.
 */

#include "../test_framework.h"
#include "base/xunicode.h"

/* ========== Property Lookup ========== */

TEST(unicode_property_lookup_valid) {
    ASSERT_EQ_INT(xr_unicode_property_lookup("L", 1), XR_UP_L);
    ASSERT_EQ_INT(xr_unicode_property_lookup("Lu", 2), XR_UP_Lu);
    ASSERT_EQ_INT(xr_unicode_property_lookup("Ll", 2), XR_UP_Ll);
    ASSERT_EQ_INT(xr_unicode_property_lookup("N", 1), XR_UP_N);
    ASSERT_EQ_INT(xr_unicode_property_lookup("Nd", 2), XR_UP_Nd);
    ASSERT_EQ_INT(xr_unicode_property_lookup("P", 1), XR_UP_P);
    ASSERT_EQ_INT(xr_unicode_property_lookup("S", 1), XR_UP_S);
    ASSERT_EQ_INT(xr_unicode_property_lookup("Z", 1), XR_UP_Z);
}

TEST(unicode_property_lookup_scripts) {
    ASSERT_EQ_INT(xr_unicode_property_lookup("Han", 3), XR_UP_Han);
    ASSERT_EQ_INT(xr_unicode_property_lookup("Latin", 5), XR_UP_Latin);
    ASSERT_EQ_INT(xr_unicode_property_lookup("Greek", 5), XR_UP_Greek);
    ASSERT_EQ_INT(xr_unicode_property_lookup("Cyrillic", 8), XR_UP_Cyrillic);
    ASSERT_EQ_INT(xr_unicode_property_lookup("ASCII", 5), XR_UP_ASCII);
    ASSERT_EQ_INT(xr_unicode_property_lookup("Any", 3), XR_UP_Any);
}

TEST(unicode_property_lookup_invalid) {
    ASSERT_EQ_INT(xr_unicode_property_lookup("Unknown", 7), XR_UP_INVALID);
    ASSERT_EQ_INT(xr_unicode_property_lookup("", 0), XR_UP_INVALID);
}

TEST(unicode_property_name) {
    ASSERT_NOT_NULL(xr_unicode_property_name(XR_UP_L));
    ASSERT_NOT_NULL(xr_unicode_property_name(XR_UP_Han));
    ASSERT_NOT_NULL(xr_unicode_property_name(XR_UP_ASCII));
}

/* ========== ASCII Classification ========== */

TEST(unicode_is_letter_ascii) {
    ASSERT_TRUE(xr_unicode_is_letter('A'));
    ASSERT_TRUE(xr_unicode_is_letter('Z'));
    ASSERT_TRUE(xr_unicode_is_letter('a'));
    ASSERT_TRUE(xr_unicode_is_letter('z'));
    ASSERT_FALSE(xr_unicode_is_letter('0'));
    ASSERT_FALSE(xr_unicode_is_letter(' '));
    ASSERT_FALSE(xr_unicode_is_letter('!'));
}

TEST(unicode_is_upper_ascii) {
    ASSERT_TRUE(xr_unicode_is_upper('A'));
    ASSERT_TRUE(xr_unicode_is_upper('Z'));
    ASSERT_FALSE(xr_unicode_is_upper('a'));
    ASSERT_FALSE(xr_unicode_is_upper('0'));
}

TEST(unicode_is_lower_ascii) {
    ASSERT_TRUE(xr_unicode_is_lower('a'));
    ASSERT_TRUE(xr_unicode_is_lower('z'));
    ASSERT_FALSE(xr_unicode_is_lower('A'));
    ASSERT_FALSE(xr_unicode_is_lower('0'));
}

TEST(unicode_is_number_ascii) {
    ASSERT_TRUE(xr_unicode_is_number('0'));
    ASSERT_TRUE(xr_unicode_is_number('9'));
    ASSERT_FALSE(xr_unicode_is_number('a'));
    ASSERT_FALSE(xr_unicode_is_number(' '));
}

TEST(unicode_is_alnum_ascii) {
    ASSERT_TRUE(xr_unicode_is_alnum('A'));
    ASSERT_TRUE(xr_unicode_is_alnum('z'));
    ASSERT_TRUE(xr_unicode_is_alnum('0'));
    ASSERT_TRUE(xr_unicode_is_alnum('9'));
    ASSERT_FALSE(xr_unicode_is_alnum(' '));
    ASSERT_FALSE(xr_unicode_is_alnum('!'));
}

TEST(unicode_is_whitespace_ascii) {
    ASSERT_TRUE(xr_unicode_is_whitespace(' '));
    ASSERT_TRUE(xr_unicode_is_whitespace('\t'));
    ASSERT_TRUE(xr_unicode_is_whitespace('\n'));
    ASSERT_TRUE(xr_unicode_is_whitespace('\r'));
    ASSERT_FALSE(xr_unicode_is_whitespace('a'));
    ASSERT_FALSE(xr_unicode_is_whitespace('0'));
}

TEST(unicode_is_punct_ascii) {
    ASSERT_TRUE(xr_unicode_is_punct('!'));
    ASSERT_TRUE(xr_unicode_is_punct('.'));
    ASSERT_TRUE(xr_unicode_is_punct(','));
    ASSERT_TRUE(xr_unicode_is_punct('{'));
    ASSERT_TRUE(xr_unicode_is_punct('~'));
    ASSERT_FALSE(xr_unicode_is_punct('a'));
    ASSERT_FALSE(xr_unicode_is_punct('0'));
    ASSERT_FALSE(xr_unicode_is_punct(' '));
}

/* ========== Non-ASCII Classification ========== */

TEST(unicode_is_letter_cjk) {
    // CJK Unified Ideographs range: U+4E00 - U+9FFF
    ASSERT_TRUE(xr_unicode_is_letter(0x4E2D));  // 中
    ASSERT_TRUE(xr_unicode_is_letter(0x6587));  // 文
}

TEST(unicode_is_letter_cyrillic) {
    // Cyrillic: U+0410 = А (Cyrillic Capital A)
    ASSERT_TRUE(xr_unicode_is_letter(0x0410));
    ASSERT_TRUE(xr_unicode_is_letter(0x0430));  // а (lowercase)
}

TEST(unicode_is_number_non_ascii) {
    // Arabic-Indic digits: U+0660 - U+0669
    ASSERT_TRUE(xr_unicode_is_number(0x0660));
}

/* ========== Case Conversion ========== */

TEST(unicode_toupper) {
    ASSERT_EQ_UINT(xr_unicode_toupper('a'), 'A');
    ASSERT_EQ_UINT(xr_unicode_toupper('z'), 'Z');
    ASSERT_EQ_UINT(xr_unicode_toupper('A'), 'A');  // already upper
    ASSERT_EQ_UINT(xr_unicode_toupper('0'), '0');  // non-letter unchanged
}

TEST(unicode_tolower) {
    ASSERT_EQ_UINT(xr_unicode_tolower('A'), 'a');
    ASSERT_EQ_UINT(xr_unicode_tolower('Z'), 'z');
    ASSERT_EQ_UINT(xr_unicode_tolower('a'), 'a');  // already lower
    ASSERT_EQ_UINT(xr_unicode_tolower('0'), '0');  // non-letter unchanged
}

/* ========== Property Ranges ========== */

TEST(unicode_property_ranges_valid) {
    const XrUnicodeRange *ranges;
    int count;

    ASSERT_TRUE(xr_unicode_property_ranges(XR_UP_L, &ranges, &count));
    ASSERT_NOT_NULL(ranges);
    ASSERT_GT(count, 0);

    ASSERT_TRUE(xr_unicode_property_ranges(XR_UP_Nd, &ranges, &count));
    ASSERT_NOT_NULL(ranges);
    ASSERT_GT(count, 0);
}

TEST(unicode_property_ranges_ascii) {
    const XrUnicodeRange *ranges;
    int count;

    ASSERT_TRUE(xr_unicode_property_ranges(XR_UP_ASCII, &ranges, &count));
    ASSERT_NOT_NULL(ranges);
    // ASCII range should cover 0x00-0x7F
    ASSERT_EQ_UINT(ranges[0].lo, 0x00);
    ASSERT_EQ_UINT(ranges[0].hi, 0x7F);
}

/* ========== is_property Direct ========== */

TEST(unicode_is_property_han) {
    ASSERT_TRUE(xr_unicode_is_property(0x4E2D, XR_UP_Han));  // 中
    ASSERT_FALSE(xr_unicode_is_property('A', XR_UP_Han));
}

TEST(unicode_is_property_latin) {
    ASSERT_TRUE(xr_unicode_is_property('A', XR_UP_Latin));
    ASSERT_TRUE(xr_unicode_is_property('z', XR_UP_Latin));
    ASSERT_FALSE(xr_unicode_is_property(0x4E2D, XR_UP_Latin));
}

TEST(unicode_is_property_any) {
    // Any should match everything
    ASSERT_TRUE(xr_unicode_is_property('A', XR_UP_Any));
    ASSERT_TRUE(xr_unicode_is_property(0x4E2D, XR_UP_Any));
    ASSERT_TRUE(xr_unicode_is_property(0, XR_UP_Any));
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Unicode - Property Lookup");
RUN_TEST(unicode_property_lookup_valid);
RUN_TEST(unicode_property_lookup_scripts);
RUN_TEST(unicode_property_lookup_invalid);
RUN_TEST(unicode_property_name);

RUN_TEST_SUITE("Unicode - ASCII Classification");
RUN_TEST(unicode_is_letter_ascii);
RUN_TEST(unicode_is_upper_ascii);
RUN_TEST(unicode_is_lower_ascii);
RUN_TEST(unicode_is_number_ascii);
RUN_TEST(unicode_is_alnum_ascii);
RUN_TEST(unicode_is_whitespace_ascii);
RUN_TEST(unicode_is_punct_ascii);

RUN_TEST_SUITE("Unicode - Non-ASCII");
RUN_TEST(unicode_is_letter_cjk);
RUN_TEST(unicode_is_letter_cyrillic);
RUN_TEST(unicode_is_number_non_ascii);

RUN_TEST_SUITE("Unicode - Case Conversion");
RUN_TEST(unicode_toupper);
RUN_TEST(unicode_tolower);

RUN_TEST_SUITE("Unicode - Property Ranges");
RUN_TEST(unicode_property_ranges_valid);
RUN_TEST(unicode_property_ranges_ascii);

RUN_TEST_SUITE("Unicode - is_property Direct");
RUN_TEST(unicode_is_property_han);
RUN_TEST(unicode_is_property_latin);
RUN_TEST(unicode_is_property_any);

TEST_MAIN_END()
