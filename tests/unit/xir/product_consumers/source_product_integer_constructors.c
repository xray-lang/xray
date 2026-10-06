/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_integer_constructors.c - Original integer source producers
 *
 * KEY CONCEPT:
 *   Exact original C loops independently produce fixture bytes without retired runtime APIs.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ASSERT_TRUE(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void emit_integer_division(FILE *output) {

    static const struct {
        const char *type, *left, *right, *quotient, *remainder;
    } cases[] = {
        {"i8", "-128", "-1", "-128", "0"},
        {"i16", "-32768", "-1", "-32768", "0"},
        {"i32", "-2147483648", "-1", "-2147483648", "0"},
        {"i64", "-9223372036854775808", "-1", "-9223372036854775808", "0"},
        {"u8", "255", "3", "85", "0"},
        {"u16", "65535", "3", "21845", "0"},
        {"u32", "4294967295", "3", "1431655765", "0"},
        {"u64", "18446744073709551615", "10", "1844674407370955161", "5"},
    };
    char source[16384];
    size_t used = 0u;
    for (unsigned index = 0u; index < 8u; ++index) {
        const char *type = cases[index].type;
        int count = snprintf(source + used, sizeof(source) - used,
                             "fn div_%s(a: %s, b: %s) -> %s { return a / b }\n"
                             "fn rem_%s(a: %s, b: %s) -> %s { return a %% b }\n",
                             type, type, type, type, type, type, type, type);
        ASSERT_TRUE(count > 0 && (size_t) count < sizeof(source) - used);
        used += (size_t) count;
    }
    const char prefix[] = "fn answer() -> i64 {\n";
    memcpy(source + used, prefix, sizeof(prefix));
    used += sizeof(prefix) - 1u;
    for (unsigned index = 0u; index < 8u; ++index) {
        int count =
            snprintf(source + used, sizeof(source) - used,
                     "if ((div_%s(%s, %s) as i64) != %s) { return %u }\n"
                     "if ((rem_%s(%s, %s) as i64) != %s) { return %u }\n",
                     cases[index].type, cases[index].left, cases[index].right,
                     cases[index].quotient, index * 2u + 1u, cases[index].type, cases[index].left,
                     cases[index].right, cases[index].remainder, index * 2u + 2u);
        ASSERT_TRUE(count > 0 && (size_t) count < sizeof(source) - used);
        used += (size_t) count;
    }
    const char suffix[] = "if (div_i64(-7, 3) != -2) { return 17 }\n"
                          "if (rem_i64(-7, 3) != -1) { return 18 }\n"
                          "if (div_i64(7, -3) != -2) { return 19 }\n"
                          "if (rem_i64(7, -3) != 1) { return 20 }\n"
                          "return 0\n}\n";
    ASSERT_TRUE(sizeof(suffix) <= sizeof(source) - used);
    memcpy(source + used, suffix, sizeof(suffix));
    ASSERT_TRUE(fwrite(source, 1, strlen(source), output) == strlen(source));
}
static void emit_integer_comparisons(FILE *output) {

    static const struct {
        const char *type, *low, *high;
    } bounds[] = {
        {"i8", "-128", "127"},
        {"u8", "0", "255"},
        {"i16", "-32768", "32767"},
        {"u16", "0", "65535"},
        {"i32", "-2147483648", "2147483647"},
        {"u32", "0", "4294967295"},
        {"i64", "-9223372036854775808", "9223372036854775807"},
        {"u64", "0", "18446744073709551615"},
    };
    static const struct {
        const char *symbol;
        bool expected[3];
    } predicates[] = {
        {"==", {false, false, true}}, {"!=", {true, true, false}}, {"<", {true, false, false}},
        {"<=", {true, false, true}},  {">", {false, true, false}}, {">=", {false, true, true}},
    };
    char source[32768];
    size_t used = 0u;
    for (unsigned type = 0u; type < 8u; ++type) {
        for (unsigned predicate = 0u; predicate < 6u; ++predicate) {
            int count = snprintf(source + used, sizeof(source) - used,
                                 "fn op%u_%s(a: %s, b: %s) -> bool { return a %s b }\n", predicate,
                                 bounds[type].type, bounds[type].type, bounds[type].type,
                                 predicates[predicate].symbol);
            ASSERT_TRUE(count > 0 && (size_t) count < sizeof(source) - used);
            used += (size_t) count;
        }
    }
    const char prefix[] = "fn mixed(a: u32, b: u64) -> bool { return a < b }\n"
                          "fn signedMixed(a: i8, b: i16) -> bool { return a < b }\n"
                          "fn answer() -> i64 {\n";
    ASSERT_TRUE(sizeof(prefix) <= sizeof(source) - used);
    memcpy(source + used, prefix, sizeof(prefix));
    used += sizeof(prefix) - 1u;
    for (unsigned type = 0u; type < 8u; ++type) {
        for (unsigned predicate = 0u; predicate < 6u; ++predicate) {
            for (unsigned order = 0u; order < 3u; ++order) {
                const char *left = order == 0u ? bounds[type].low : bounds[type].high;
                const char *right = order == 1u ? bounds[type].low : bounds[type].high;
                int count = snprintf(
                    source + used, sizeof(source) - used, "if (%sop%u_%s(%s, %s)) { return %u }\n",
                    predicates[predicate].expected[order] ? "!" : "", predicate, bounds[type].type,
                    left, right, 1u + type * 18u + predicate * 3u + order);
                ASSERT_TRUE(count > 0 && (size_t) count < sizeof(source) - used);
                used += (size_t) count;
            }
        }
    }
    const char suffix[] = "if (!mixed(4294967295, 18446744073709551615)) { return 145 }\n"
                          "if (!signedMixed(-128, 32767)) { return 146 }\n"
                          "if (!op2_u64(9223372036854775807, 9223372036854775808)) { return 147 }\n"
                          "if (op2_u64(9223372036854775808, 9223372036854775807)) { return 148 }\n"
                          "return 0\n}\n";
    ASSERT_TRUE(sizeof(suffix) <= sizeof(source) - used);
    memcpy(source + used, suffix, sizeof(suffix));
    ASSERT_TRUE(fwrite(source, 1, strlen(source), output) == strlen(source));
}
static void emit_integer_arithmetic(FILE *output) {

    static const struct {
        const char *type;
        const char *minimum;
        const char *maximum;
        const char *add_result;
        const char *subtract_result;
        const char *multiply_result;
    } cases[] = {
        {"i8", "-128", "127", "-128", "127", "-2"},
        {"u8", "0", "255", "0", "255", "254"},
        {"i16", "-32768", "32767", "-32768", "32767", "-2"},
        {"u16", "0", "65535", "0", "65535", "65534"},
        {"i32", "-2147483648", "2147483647", "-2147483648", "2147483647", "-2"},
        {"u32", "0", "4294967295", "0", "4294967295", "4294967294"},
        {"i64", "-9223372036854775808", "9223372036854775807", "-9223372036854775808",
         "9223372036854775807", "-2"},
        {"u64", "0", "18446744073709551615", "0", "-1", "-2"},
    };
    char source[16384];
    size_t used = 0u;
    for (unsigned index = 0u; index < 8u; ++index) {
        const char *type = cases[index].type;
        int count =
            snprintf(source + used, sizeof(source) - used,
                     "fn add_%s(a: %s, b: %s) -> %s { return a + b }\n"
                     "fn sub_%s(a: %s, b: %s) -> %s { return a - b }\n"
                     "fn mul_%s(a: %s, b: %s) -> %s { return a * b }\n",
                     type, type, type, type, type, type, type, type, type, type, type, type);
        ASSERT_TRUE(count > 0 && (size_t) count < sizeof(source) - used);
        used += (size_t) count;
    }
    const char prefix[] = "fn mixed(a: u8, b: u16) -> u16 { return a + b }\n"
                          "fn answer() -> i64 {\n";
    ASSERT_TRUE(sizeof(prefix) <= sizeof(source) - used);
    memcpy(source + used, prefix, sizeof(prefix));
    used += sizeof(prefix) - 1u;
    for (unsigned index = 0u; index < 8u; ++index) {
        int count = snprintf(source + used, sizeof(source) - used,
                             "if ((add_%s(%s, 1) as i64) != %s) { return %u }\n"
                             "if ((sub_%s(%s, 1) as i64) != %s) { return %u }\n"
                             "if ((mul_%s(%s, 2) as i64) != %s) { return %u }\n",
                             cases[index].type, cases[index].maximum, cases[index].add_result,
                             index * 3u + 1u, cases[index].type, cases[index].minimum,
                             cases[index].subtract_result, index * 3u + 2u, cases[index].type,
                             cases[index].maximum, cases[index].multiply_result, index * 3u + 3u);
        ASSERT_TRUE(count > 0 && (size_t) count < sizeof(source) - used);
        used += (size_t) count;
    }
    const char suffix[] = "if ((mixed(255, 1) as i64) != 256) { return 25 }\nreturn 0\n}\n";
    ASSERT_TRUE(sizeof(suffix) <= sizeof(source) - used);
    memcpy(source + used, suffix, sizeof(suffix));
    ASSERT_TRUE(fwrite(source, 1, strlen(source), output) == strlen(source));
}
int main(int argc, char **argv) {
    ASSERT_TRUE(argc == 3);
    FILE *output = fopen(argv[2], "wb");
    ASSERT_TRUE(output);
    if (!strcmp(argv[1], "integer_division")) emit_integer_division(output);
    else if (!strcmp(argv[1], "integer_comparisons")) emit_integer_comparisons(output);
    else if (!strcmp(argv[1], "integer_arithmetic")) emit_integer_arithmetic(output);
    else ASSERT_TRUE(false);
    ASSERT_TRUE(!fclose(output));
    return 0;
}
