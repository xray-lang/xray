/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_text_kernel.c - Independent boundary and fault tests for the shared
 * typed text kernel
 *
 * The kernel is the one implementation of string/rune value semantics that
 * every executor compiles, so its correctness cannot be inferred from two
 * executors agreeing with each other.  These checks use answers written down
 * independently of any executor: UTF-8 forms from RFC 3629, the exact decimal
 * rendering of i64 extremes, UTF-8 encodings computed by hand, and the
 * measured/rendered group sizes of hostile operand lists.
 */

#include "runtime/core/xr_text_kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

typedef struct Utf8Case {
    const char *label;
    const uint8_t bytes[8];
    size_t size;
    int valid;
} Utf8Case;

static void test_utf8_admission(void) {
    static const Utf8Case cases[] = {
        {"empty", {0}, 0u, 1},
        {"ascii", {'a', 'b', 'c'}, 3u, 1},
        {"embedded nul", {'a', 0x00, 'b'}, 3u, 1},
        {"two-byte minimum U+0080", {0xC2, 0x80}, 2u, 1},
        {"two-byte maximum U+07FF", {0xDF, 0xBF}, 2u, 1},
        {"three-byte minimum U+0800", {0xE0, 0xA0, 0x80}, 3u, 1},
        {"three-byte below surrogates U+D7FF", {0xED, 0x9F, 0xBF}, 3u, 1},
        {"three-byte above surrogates U+E000", {0xEE, 0x80, 0x80}, 3u, 1},
        {"three-byte maximum U+FFFF", {0xEF, 0xBF, 0xBF}, 3u, 1},
        {"four-byte minimum U+10000", {0xF0, 0x90, 0x80, 0x80}, 4u, 1},
        {"four-byte maximum U+10FFFF", {0xF4, 0x8F, 0xBF, 0xBF}, 4u, 1},
        {"mixed", {'x', 0xC3, 0xA9, 0xF0, 0x9F, 0x98, 0x80, 'y'}, 8u, 1},
        {"stray continuation", {0x80}, 1u, 0},
        {"lead 0xC0 overlong", {0xC0, 0x80}, 2u, 0},
        {"lead 0xC1 overlong", {0xC1, 0xBF}, 2u, 0},
        {"three-byte overlong", {0xE0, 0x80, 0x80}, 3u, 0},
        {"four-byte overlong", {0xF0, 0x80, 0x80, 0x80}, 4u, 0},
        {"surrogate U+D800", {0xED, 0xA0, 0x80}, 3u, 0},
        {"surrogate U+DFFF", {0xED, 0xBF, 0xBF}, 3u, 0},
        {"beyond U+10FFFF", {0xF4, 0x90, 0x80, 0x80}, 4u, 0},
        {"lead 0xF5", {0xF5, 0x80, 0x80, 0x80}, 4u, 0},
        {"lead 0xFF", {0xFF}, 1u, 0},
        /* Truncated sequences are followed by a decoy continuation byte past
         * the declared size, so reading one byte too far admits the input
         * instead of being masked by zero padding. */
        {"truncated two-byte", {0xC3, 0xA9}, 1u, 0},
        {"truncated three-byte", {0xE4, 0xB8, 0x96}, 2u, 0},
        {"truncated four-byte", {0xF0, 0x9F, 0x98, 0x80}, 3u, 0},
        {"ascii in continuation slot", {0xE4, 0x41, 0x80}, 3u, 0},
        {"lead byte in continuation slot", {0xC3, 0xC3}, 2u, 0},
        {"0xFF in continuation slot", {0xE4, 0xFF, 0x80}, 3u, 0},
        {"trailing garbage after valid", {'a', 0xC3, 0xA9, 0xBF}, 4u, 0},
    };
    for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const Utf8Case *entry = &cases[index];
        int valid = xr_text_utf8_is_valid(entry->bytes, entry->size);
        if (valid != entry->valid)
            fprintf(stderr, "utf8 case %s: expected %d got %d\n", entry->label, entry->valid,
                    valid);
        REQUIRE(valid == entry->valid);
    }
    REQUIRE(xr_text_utf8_is_valid(NULL, 0u));
    REQUIRE(!xr_text_utf8_is_valid(NULL, 1u));
}

static void test_rune_admission_and_encoding(void) {
    static const struct {
        uint32_t rune;
        int scalar;
        size_t size;
        uint8_t encoded[4];
    } cases[] = {
        {0x0u, 1, 1u, {0x00}},
        {0x41u, 1, 1u, {0x41}},
        {0x7Fu, 1, 1u, {0x7F}},
        {0x80u, 1, 2u, {0xC2, 0x80}},
        {0x7FFu, 1, 2u, {0xDF, 0xBF}},
        {0x800u, 1, 3u, {0xE0, 0xA0, 0x80}},
        {0xD7FFu, 1, 3u, {0xED, 0x9F, 0xBF}},
        {0xD800u, 0, 0u, {0}},
        {0xDBFFu, 0, 0u, {0}},
        {0xDC00u, 0, 0u, {0}},
        {0xDFFFu, 0, 0u, {0}},
        {0xE000u, 1, 3u, {0xEE, 0x80, 0x80}},
        {0xFFFFu, 1, 3u, {0xEF, 0xBF, 0xBF}},
        {0x10000u, 1, 4u, {0xF0, 0x90, 0x80, 0x80}},
        {0x1F600u, 1, 4u, {0xF0, 0x9F, 0x98, 0x80}},
        {0x10FFFFu, 1, 4u, {0xF4, 0x8F, 0xBF, 0xBF}},
        {0x110000u, 0, 0u, {0}},
        {0xFFFFFFFFu, 0, 0u, {0}},
    };
    for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        uint8_t out[XR_TEXT_RUNE_UTF8_MAX] = {0xAA, 0xAA, 0xAA, 0xAA};
        REQUIRE(xr_text_rune_is_scalar(cases[index].rune) == cases[index].scalar);
        REQUIRE(xr_text_encode_rune(cases[index].rune, NULL) == cases[index].size);
        REQUIRE(xr_text_encode_rune(cases[index].rune, out) == cases[index].size);
        REQUIRE(memcmp(out, cases[index].encoded, cases[index].size) == 0);
        /* Every encoding the kernel produces is admitted by its own validator. */
        if (cases[index].scalar)
            REQUIRE(xr_text_utf8_is_valid(out, cases[index].size));
    }
}

static void test_i64_display(void) {
    static const struct {
        int64_t value;
        const char *text;
    } cases[] = {
        {0, "0"},
        {7, "7"},
        {-1, "-1"},
        {10, "10"},
        {-10, "-10"},
        {INT64_MAX, "9223372036854775807"},
        {INT64_MIN, "-9223372036854775808"},
        {INT64_MIN + 1, "-9223372036854775807"},
        {1000000000000000000, "1000000000000000000"},
    };
    for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        uint8_t out[XR_TEXT_I64_DISPLAY_MAX + 1u];
        size_t expected = strlen(cases[index].text);
        memset(out, 0xAA, sizeof(out));
        REQUIRE(xr_text_display_i64(cases[index].value, NULL) == expected);
        REQUIRE(xr_text_display_i64(cases[index].value, out) == expected);
        REQUIRE(memcmp(out, cases[index].text, expected) == 0);
        REQUIRE(out[expected] == 0xAA);
    }
    uint8_t out[XR_TEXT_BOOL_DISPLAY_MAX];
    REQUIRE(xr_text_display_bool(1, NULL) == 4u);
    REQUIRE(xr_text_display_bool(0, NULL) == 5u);
    REQUIRE(xr_text_display_bool(1, out) == 4u && memcmp(out, "true", 4u) == 0);
    REQUIRE(xr_text_display_bool(0, out) == 5u && memcmp(out, "false", 5u) == 0);
    REQUIRE(xr_text_display_bool(42, out) == 4u && memcmp(out, "true", 4u) == 0);
}

static void test_cached_scalar_count(void) {
    static const struct { const char *bytes; size_t size; size_t count; } cases[] = {
        {"", 0u, 0u}, {"abc", 3u, 3u}, {"a\0b", 3u, 3u},
        {"\xC3\xA9", 2u, 1u}, {"\xE4\xB8\x96", 3u, 1u},
        {"\xF0\x9F\x98\x80", 4u, 1u}, {"e\xCC\x81", 3u, 2u},
    };
    REQUIRE(xr_text_scalar_count(NULL, 0u) == 0u);
    for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const uint8_t *bytes = (const uint8_t *)cases[index].bytes;
        REQUIRE(xr_text_utf8_is_valid(bytes, cases[index].size));
        REQUIRE(xr_text_scalar_count(bytes, cases[index].size) == cases[index].count);
    }
}

static void test_u64_display(void) {
    static const struct { uint64_t value; const char *text; } cases[] = {
        {0u, "0"}, {9u, "9"}, {10u, "10"}, {UINT32_MAX, "4294967295"},
        {UINT64_C(9223372036854775808), "9223372036854775808"},
        {UINT64_MAX, "18446744073709551615"},
    };
    for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        uint8_t out[XR_TEXT_U64_DISPLAY_MAX + 2u];
        size_t expected = strlen(cases[index].text);
        memset(out, 0xAA, sizeof(out));
        REQUIRE(xr_text_display_u64(cases[index].value, NULL) == expected);
        REQUIRE(xr_text_display_u64(cases[index].value, out + 1u) == expected);
        REQUIRE(memcmp(out + 1u, cases[index].text, expected) == 0);
        REQUIRE(out[0] == 0xAA && out[expected + 1u] == 0xAA);
    }
    XrTextDisplayOperand operands[2] = {
        {.kind = XR_TEXT_DISPLAY_I64, .i64 = INT64_MIN},
        {.kind = XR_TEXT_DISPLAY_U64, .u64 = UINT64_MAX},
    };
    static const char expected[] = "-9223372036854775808 18446744073709551615\n";
    uint8_t line[sizeof(expected)] = {0};
    int ok = 0;
    REQUIRE(xr_text_group_size(operands, 2u, &ok) == sizeof(expected) - 1u && ok);
    REQUIRE(xr_text_group_render(operands, 2u, line) == sizeof(expected) - 1u);
    REQUIRE(memcmp(line, expected, sizeof(expected)) == 0);
}

static void test_compare_and_predicates(void) {
    static const uint8_t a[] = {'a'};
    static const uint8_t ab[] = {'a', 'b'};
    static const uint8_t abc[] = {'a', 'b', 'c'};
    static const uint8_t b[] = {'b'};
    static const uint8_t e_acute[] = {0xC3, 0xA9};
    static const uint8_t z[] = {'z'};
    static const uint8_t nul_a[] = {0x00, 'a'};
    REQUIRE(xr_text_compare(NULL, 0u, NULL, 0u) == 0);
    REQUIRE(xr_text_compare(NULL, 0u, a, 1u) == -1);
    REQUIRE(xr_text_compare(a, 1u, NULL, 0u) == 1);
    REQUIRE(xr_text_compare(a, 1u, a, 1u) == 0);
    REQUIRE(xr_text_compare(ab, 2u, abc, 3u) == -1);
    REQUIRE(xr_text_compare(abc, 3u, ab, 2u) == 1);
    REQUIRE(xr_text_compare(a, 1u, b, 1u) == -1);
    /* Byte order equals scalar order: U+00E9 sorts after 'z'. */
    REQUIRE(xr_text_compare(e_acute, 2u, z, 1u) == 1);
    /* An embedded NUL is an ordinary byte, not a terminator. */
    REQUIRE(xr_text_compare(nul_a, 2u, a, 1u) == -1);
    REQUIRE(xr_text_compare(nul_a, 2u, nul_a, 2u) == 0);
    static const int orders[] = {-1, 0, 1};
    static const int expected[3][6] = {
        /* eq ne lt le gt ge */
        {0, 1, 1, 1, 0, 0},
        {1, 0, 0, 1, 0, 1},
        {0, 1, 0, 0, 1, 1},
    };
    for (size_t order = 0u; order < 3u; ++order)
        for (uint32_t predicate = 0u; predicate < 6u; ++predicate)
            REQUIRE(xr_text_predicate(orders[order], predicate) == expected[order][predicate]);
    REQUIRE(xr_text_predicate(0, 6u) == 0);
    REQUIRE(xr_text_predicate(0, UINT32_MAX) == 0);
}

static void test_concat(void) {
    static const uint8_t left[] = {'a', 'b'};
    static const uint8_t right[] = {0xE4, 0xB8, 0x96};
    uint8_t out[5];
    int ok = 0;
    REQUIRE(xr_text_concat_size(2u, 3u, &ok) == 5u && ok == 1);
    REQUIRE(xr_text_concat_size(0u, 0u, &ok) == 0u && ok == 1);
    REQUIRE(xr_text_concat_size(SIZE_MAX, 1u, &ok) == 0u && ok == 0);
    REQUIRE(xr_text_concat_size(SIZE_MAX - 1u, 1u, &ok) == SIZE_MAX && ok == 1);
    xr_text_concat(left, 2u, right, 3u, out);
    REQUIRE(memcmp(out, "ab\xE4\xB8\x96", 5u) == 0);
    xr_text_concat(NULL, 0u, right, 3u, out);
    REQUIRE(memcmp(out, right, 3u) == 0);
    xr_text_concat(left, 2u, NULL, 0u, out);
    REQUIRE(memcmp(out, left, 2u) == 0);
}

static void test_output_group(void) {
    static const uint8_t text[] = {'x', ' ', 'y'};
    static const char expected[] = "-5 true x y \xF0\x9F\x98\x80 \n";
    XrTextDisplayOperand operands[5];
    XrTextDisplayOperand hostile;
    XrTextDisplayOperand huge[2];
    uint8_t line[64];
    size_t size;
    int ok = 0;
    memset(operands, 0, sizeof(operands));
    operands[0].kind = XR_TEXT_DISPLAY_I64;
    operands[0].i64 = -5;
    operands[1].kind = XR_TEXT_DISPLAY_BOOL;
    operands[1].boolean = 1;
    operands[2].kind = XR_TEXT_DISPLAY_STRING;
    operands[2].bytes = text;
    operands[2].size = sizeof(text);
    operands[3].kind = XR_TEXT_DISPLAY_RUNE;
    operands[3].rune = 0x1F600u;
    operands[4].kind = XR_TEXT_DISPLAY_STRING;
    operands[4].bytes = NULL;
    operands[4].size = 0u;
    size = xr_text_group_size(operands, 5u, &ok);
    REQUIRE(ok == 1 && size == sizeof(expected) - 1u);
    REQUIRE(xr_text_group_render(operands, 5u, line) == size);
    REQUIRE(memcmp(line, expected, size) == 0);

    /* An empty group is exactly one line feed. */
    REQUIRE(xr_text_group_size(NULL, 0u, &ok) == 1u && ok == 1);
    REQUIRE(xr_text_group_render(NULL, 0u, line) == 1u && line[0] == '\n');

    /* A single empty string renders as an empty line, not as nothing. */
    REQUIRE(xr_text_group_size(&operands[4], 1u, &ok) == 1u && ok == 1);

    /* Undisplayable kinds and non-scalar runes are refused, not rendered. */
    memset(&hostile, 0, sizeof(hostile));
    hostile.kind = 99u;
    REQUIRE(xr_text_group_size(&hostile, 1u, &ok) == 0u && ok == 0);
    hostile.kind = XR_TEXT_DISPLAY_RUNE;
    hostile.rune = 0xD800u;
    REQUIRE(xr_text_group_size(&hostile, 1u, &ok) == 0u && ok == 0);
    hostile.rune = 0x110000u;
    REQUIRE(xr_text_group_size(&hostile, 1u, &ok) == 0u && ok == 0);

    /* Size accounting refuses overflow instead of wrapping: the measured
     * total may reach SIZE_MAX exactly, and one byte more is refused.  Only
     * sizes are inspected here; measuring never touches operand bytes. */
    memset(huge, 0, sizeof(huge));
    huge[0].kind = XR_TEXT_DISPLAY_STRING;
    huge[0].bytes = text;
    huge[0].size = SIZE_MAX - 3u;
    huge[1].kind = XR_TEXT_DISPLAY_STRING;
    huge[1].bytes = text;
    huge[1].size = 1u;
    REQUIRE(xr_text_group_size(huge, 2u, &ok) == SIZE_MAX && ok == 1);
    huge[1].size = 2u;
    REQUIRE(xr_text_group_size(huge, 2u, &ok) == 0u && ok == 0);
    huge[0].size = SIZE_MAX;
    REQUIRE(xr_text_group_size(huge, 1u, &ok) == 0u && ok == 0);
}

static void test_f64_display(void) {
    static const struct { double value; const char *expected; } cases[] = {
        {0.0, "0.0"}, {-0.0, "-0.0"}, {1.0, "1.0"}, {1.5, "1.5"},
        {1e-7, "1e-07"}, {1e20, "1e+20"},
        {1.2345678901234567, "1.23456789012346"},
    };
    for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        XrTextDisplayOperand operand = {0};
        operand.kind = XR_TEXT_DISPLAY_F64;
        operand.f64 = cases[index].value;
        uint8_t output[34];
        memset(output, 0xa5, sizeof(output));
        size_t expected = strlen(cases[index].expected);
        REQUIRE(xr_format_float(NULL, 0u, cases[index].value) == (int)expected);
        for (size_t capacity = 0u; capacity <= expected + 2u; ++capacity) {
            unsigned char bounded[34];
            memset(bounded, 0xa5, sizeof(bounded));
            REQUIRE(xr_format_float((char *)bounded + 1u, capacity, cases[index].value) == (int)expected);
            REQUIRE(bounded[0] == 0xa5 && bounded[capacity + 1u] == 0xa5);
            if (capacity) {
                size_t copied = expected < capacity - 1u ? expected : capacity - 1u;
                REQUIRE(memcmp(bounded + 1u, cases[index].expected, copied) == 0);
                REQUIRE(bounded[copied + 1u] == 0);
            } else REQUIRE(bounded[1] == 0xa5);
        }
        REQUIRE(xr_text_display_operand(&operand, NULL) == expected);
        REQUIRE(xr_text_display_operand(&operand, output + 1u) == expected);
        REQUIRE(memcmp(output + 1u, cases[index].expected, expected) == 0);
        REQUIRE(output[0] == 0xa5 && output[expected + 1u] == 0xa5);
        int ok = 0;
        REQUIRE(xr_text_group_size(&operand, 1u, &ok) == expected + 1u && ok);
        REQUIRE(xr_text_group_render(&operand, 1u, output) == expected + 1u);
        REQUIRE(output[expected] == '\n');
    }
}

int main(void) {
    test_utf8_admission();
    test_rune_admission_and_encoding();
    test_i64_display();
    test_u64_display();
    test_cached_scalar_count();
    test_compare_and_predicates();
    test_concat();
    test_output_group();
    test_f64_display();
    puts("text kernel: PASS");
    return 0;
}
