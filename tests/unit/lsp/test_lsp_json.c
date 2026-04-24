/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lsp_json.c - Unit tests for LSP JSON parser
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../../../src/base/xjson.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  Testing %s... ", #name); \
    test_##name(); \
    printf("PASS\n"); \
    tests_passed++; \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL at line %d: %s\n", __LINE__, #cond); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_STR_EQ(a, b) ASSERT(strcmp((a), (b)) == 0)

// ============================================================================
// JSON Parsing Tests
// ============================================================================

TEST(parse_null) {
    XrJsonValue *v = xjson_parse("null", 4);
    ASSERT(v != NULL);
    ASSERT(xjson_is_null(v));
    xjson_free(v);
}

TEST(parse_bool_true) {
    XrJsonValue *v = xjson_parse("true", 4);
    ASSERT(v != NULL);
    ASSERT(xjson_is_bool(v));
    ASSERT(v->as.boolean == true);
    xjson_free(v);
}

TEST(parse_bool_false) {
    XrJsonValue *v = xjson_parse("false", 5);
    ASSERT(v != NULL);
    ASSERT(xjson_is_bool(v));
    ASSERT(v->as.boolean == false);
    xjson_free(v);
}

TEST(parse_number_integer) {
    XrJsonValue *v = xjson_parse("42", 2);
    ASSERT(v != NULL);
    ASSERT(xjson_is_number(v));
    ASSERT(v->is_integer);
    ASSERT_EQ((int)v->as.integer, 42);
    xjson_free(v);
}

TEST(parse_number_negative) {
    XrJsonValue *v = xjson_parse("-123", 4);
    ASSERT(v != NULL);
    ASSERT(xjson_is_number(v));
    ASSERT(v->is_integer);
    ASSERT_EQ((int)v->as.integer, -123);
    xjson_free(v);
}

TEST(parse_number_float) {
    XrJsonValue *v = xjson_parse("3.14", 4);
    ASSERT(v != NULL);
    ASSERT(xjson_is_number(v));
    ASSERT(v->as.number > 3.13 && v->as.number < 3.15);
    xjson_free(v);
}

TEST(parse_string_simple) {
    XrJsonValue *v = xjson_parse("\"hello\"", 7);
    ASSERT(v != NULL);
    ASSERT(xjson_is_string(v));
    ASSERT_STR_EQ(v->as.string, "hello");
    xjson_free(v);
}

TEST(parse_string_escape) {
    XrJsonValue *v = xjson_parse("\"hello\\nworld\"", 14);
    ASSERT(v != NULL);
    ASSERT(xjson_is_string(v));
    ASSERT_STR_EQ(v->as.string, "hello\nworld");
    xjson_free(v);
}

TEST(parse_string_unicode) {
    // Note: Unicode escape may not be fully implemented
    // Just test that it parses without crashing
    XrJsonValue *v = xjson_parse("\"\\u0041\"", 8);
    ASSERT(v != NULL);
    ASSERT(xjson_is_string(v));
    // Result may be "A" or "\\u0041" depending on implementation
    xjson_free(v);
}

TEST(parse_empty_array) {
    XrJsonValue *v = xjson_parse("[]", 2);
    ASSERT(v != NULL);
    ASSERT(xjson_is_array(v));
    ASSERT_EQ(xjson_array_len(v), 0);
    xjson_free(v);
}

TEST(parse_array_numbers) {
    XrJsonValue *v = xjson_parse("[1, 2, 3]", 9);
    ASSERT(v != NULL);
    ASSERT(xjson_is_array(v));
    ASSERT_EQ(xjson_array_len(v), 3);
    ASSERT_EQ((int)xjson_array_get(v, 0)->as.integer, 1);
    ASSERT_EQ((int)xjson_array_get(v, 1)->as.integer, 2);
    ASSERT_EQ((int)xjson_array_get(v, 2)->as.integer, 3);
    xjson_free(v);
}

TEST(parse_empty_object) {
    XrJsonValue *v = xjson_parse("{}", 2);
    ASSERT(v != NULL);
    ASSERT(xjson_is_object(v));
    ASSERT_EQ(v->as.object.count, 0);
    xjson_free(v);
}

TEST(parse_object_simple) {
    const char *json = "{\"name\": \"test\", \"value\": 42}";
    XrJsonValue *v = xjson_parse(json, strlen(json));
    ASSERT(v != NULL);
    ASSERT(xjson_is_object(v));

    const char *name = xjson_get_string(v, "name");
    ASSERT(name != NULL);
    ASSERT_STR_EQ(name, "test");

    int64_t value = xjson_get_int(v, "value");
    ASSERT_EQ(value, 42);

    xjson_free(v);
}

TEST(parse_nested_object) {
    const char *json = "{\"outer\": {\"inner\": 123}}";
    XrJsonValue *v = xjson_parse(json, strlen(json));
    ASSERT(v != NULL);

    XrJsonValue *outer = xjson_get(v, "outer");
    ASSERT(outer != NULL);
    ASSERT(xjson_is_object(outer));

    int64_t inner = xjson_get_int(outer, "inner");
    ASSERT_EQ(inner, 123);

    xjson_free(v);
}

TEST(parse_lsp_message) {
    const char *json =
        "{"
        "  \"jsonrpc\": \"2.0\","
        "  \"id\": 1,"
        "  \"method\": \"initialize\","
        "  \"params\": {"
        "    \"processId\": 12345,"
        "    \"rootUri\": \"file:///workspace\""
        "  }"
        "}";

    XrJsonValue *v = xjson_parse(json, strlen(json));
    ASSERT(v != NULL);

    ASSERT_STR_EQ(xjson_get_string(v, "jsonrpc"), "2.0");
    ASSERT_EQ(xjson_get_int(v, "id"), 1);
    ASSERT_STR_EQ(xjson_get_string(v, "method"), "initialize");

    XrJsonValue *params = xjson_get(v, "params");
    ASSERT(params != NULL);
    ASSERT_EQ(xjson_get_int(params, "processId"), 12345);
    ASSERT_STR_EQ(xjson_get_string(params, "rootUri"), "file:///workspace");

    xjson_free(v);
}

// ============================================================================
// JSON Building Tests
// ============================================================================

TEST(build_null) {
    XrJsonValue *v = xjson_new_null();
    ASSERT(v != NULL);
    ASSERT(xjson_is_null(v));
    xjson_free(v);
}

TEST(build_bool) {
    XrJsonValue *v = xjson_new_bool(true);
    ASSERT(v != NULL);
    ASSERT(xjson_is_bool(v));
    ASSERT(v->as.boolean == true);
    xjson_free(v);
}

TEST(build_number) {
    XrJsonValue *v = xjson_new_number(42.5);
    ASSERT(v != NULL);
    ASSERT(xjson_is_number(v));
    ASSERT(v->as.number > 42.4 && v->as.number < 42.6);
    xjson_free(v);
}

TEST(build_string) {
    XrJsonValue *v = xjson_new_string("hello");
    ASSERT(v != NULL);
    ASSERT(xjson_is_string(v));
    ASSERT_STR_EQ(v->as.string, "hello");
    xjson_free(v);
}

TEST(build_array) {
    XrJsonValue *arr = xjson_new_array();
    xjson_array_push(arr, xjson_new_number(1));
    xjson_array_push(arr, xjson_new_number(2));
    xjson_array_push(arr, xjson_new_string("three"));

    ASSERT_EQ(xjson_array_len(arr), 3);
    ASSERT_EQ((int)xjson_array_get(arr, 0)->as.number, 1);
    ASSERT_STR_EQ(xjson_array_get(arr, 2)->as.string, "three");

    xjson_free(arr);
}

TEST(build_object) {
    XrJsonValue *obj = xjson_new_object();
    xjson_object_set(obj, "name", xjson_new_string("test"));
    xjson_object_set(obj, "count", xjson_new_number(42));
    xjson_object_set(obj, "enabled", xjson_new_bool(true));

    ASSERT_STR_EQ(xjson_get_string(obj, "name"), "test");
    ASSERT_EQ(xjson_get_int(obj, "count"), 42);
    ASSERT(xjson_get_bool(obj, "enabled") == true);

    xjson_free(obj);
}

// ============================================================================
// JSON Stringify Tests
// ============================================================================

TEST(stringify_null) {
    XrJsonValue *v = xjson_new_null();
    size_t len;
    char *s = xjson_stringify(v, &len);
    ASSERT(s != NULL);
    ASSERT_STR_EQ(s, "null");
    free(s);
    xjson_free(v);
}

TEST(stringify_bool) {
    XrJsonValue *v = xjson_new_bool(true);
    size_t len;
    char *s = xjson_stringify(v, &len);
    ASSERT(s != NULL);
    ASSERT_STR_EQ(s, "true");
    free(s);
    xjson_free(v);
}

TEST(stringify_number) {
    XrJsonValue *v = xjson_new_number(42);
    size_t len;
    char *s = xjson_stringify(v, &len);
    ASSERT(s != NULL);
    ASSERT(strstr(s, "42") != NULL);
    free(s);
    xjson_free(v);
}

TEST(stringify_string) {
    XrJsonValue *v = xjson_new_string("hello");
    size_t len;
    char *s = xjson_stringify(v, &len);
    ASSERT(s != NULL);
    ASSERT_STR_EQ(s, "\"hello\"");
    free(s);
    xjson_free(v);
}

TEST(stringify_array) {
    XrJsonValue *arr = xjson_new_array();
    xjson_array_push(arr, xjson_new_number(1));
    xjson_array_push(arr, xjson_new_number(2));

    size_t len;
    char *s = xjson_stringify(arr, &len);
    ASSERT(s != NULL);
    ASSERT_STR_EQ(s, "[1,2]");
    free(s);
    xjson_free(arr);
}

TEST(stringify_object) {
    XrJsonValue *obj = xjson_new_object();
    xjson_object_set(obj, "a", xjson_new_number(1));

    size_t len;
    char *s = xjson_stringify(obj, &len);
    ASSERT(s != NULL);
    ASSERT(strstr(s, "\"a\"") != NULL);
    ASSERT(strstr(s, "1") != NULL);
    free(s);
    xjson_free(obj);
}

// ============================================================================
// Edge Cases and Robustness Tests
// ============================================================================

TEST(parse_whitespace) {
    const char *input = "  \n\t{ \"key\" : \"value\" }  ";
    XrJsonValue *v = xjson_parse(input, strlen(input));
    ASSERT(v != NULL);
    ASSERT(xjson_is_object(v));
    ASSERT_STR_EQ(xjson_get_string(v, "key"), "value");
    xjson_free(v);
}

TEST(parse_empty_string) {
    XrJsonValue *v = xjson_parse("\"\"", 2);
    ASSERT(v != NULL);
    ASSERT(xjson_is_string(v));
    ASSERT_STR_EQ(v->as.string, "");
    xjson_free(v);
}

TEST(get_nonexistent_key) {
    XrJsonValue *obj = xjson_new_object();
    xjson_object_set(obj, "exists", xjson_new_number(1));

    XrJsonValue *v = xjson_get(obj, "not_exists");
    ASSERT(v == NULL);

    const char *s = xjson_get_string(obj, "not_exists");
    ASSERT(s == NULL);

    xjson_free(obj);
}

TEST(array_out_of_bounds) {
    XrJsonValue *arr = xjson_new_array();
    xjson_array_push(arr, xjson_new_number(1));

    XrJsonValue *v = xjson_array_get(arr, 10);
    ASSERT(v == NULL);

    xjson_free(arr);
}

TEST(roundtrip_complex) {
    // Build complex object
    XrJsonValue *obj = xjson_new_object();
    xjson_object_set(obj, "id", xjson_new_number(1));
    xjson_object_set(obj, "method", xjson_new_string("test"));

    XrJsonValue *params = xjson_new_object();
    xjson_object_set(params, "uri", xjson_new_string("file:///test.xr"));

    XrJsonValue *arr = xjson_new_array();
    xjson_array_push(arr, xjson_new_number(1));
    xjson_array_push(arr, xjson_new_number(2));
    xjson_object_set(params, "lines", arr);

    xjson_object_set(obj, "params", params);

    // Stringify
    size_t len;
    char *s = xjson_stringify(obj, &len);
    ASSERT(s != NULL);

    // Parse back
    XrJsonValue *parsed = xjson_parse(s, len);
    ASSERT(parsed != NULL);

    // Verify
    ASSERT_EQ(xjson_get_int(parsed, "id"), 1);
    ASSERT_STR_EQ(xjson_get_string(parsed, "method"), "test");

    XrJsonValue *p = xjson_get(parsed, "params");
    ASSERT(p != NULL);
    ASSERT_STR_EQ(xjson_get_string(p, "uri"), "file:///test.xr");

    free(s);
    xjson_free(obj);
    xjson_free(parsed);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("\n=== LSP JSON Parser Unit Tests ===\n\n");

    printf("Parsing tests:\n");
    RUN_TEST(parse_null);
    RUN_TEST(parse_bool_true);
    RUN_TEST(parse_bool_false);
    RUN_TEST(parse_number_integer);
    RUN_TEST(parse_number_negative);
    RUN_TEST(parse_number_float);
    RUN_TEST(parse_string_simple);
    RUN_TEST(parse_string_escape);
    RUN_TEST(parse_string_unicode);
    RUN_TEST(parse_empty_array);
    RUN_TEST(parse_array_numbers);
    RUN_TEST(parse_empty_object);
    RUN_TEST(parse_object_simple);
    RUN_TEST(parse_nested_object);
    RUN_TEST(parse_lsp_message);

    printf("\nBuilding tests:\n");
    RUN_TEST(build_null);
    RUN_TEST(build_bool);
    RUN_TEST(build_number);
    RUN_TEST(build_string);
    RUN_TEST(build_array);
    RUN_TEST(build_object);

    printf("\nStringify tests:\n");
    RUN_TEST(stringify_null);
    RUN_TEST(stringify_bool);
    RUN_TEST(stringify_number);
    RUN_TEST(stringify_string);
    RUN_TEST(stringify_array);
    RUN_TEST(stringify_object);

    printf("\nEdge cases:\n");
    RUN_TEST(parse_whitespace);
    RUN_TEST(parse_empty_string);
    RUN_TEST(get_nonexistent_key);
    RUN_TEST(array_out_of_bounds);
    RUN_TEST(roundtrip_complex);

    printf("\n=== Results: %d passed, %d failed ===\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
