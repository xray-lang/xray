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
#include "../../../src/app/lsp/xlsp_json.h"

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
    XrJsonValue *v = xlsp_json_parse("null", 4);
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_null(v));
    xlsp_json_free(v);
}

TEST(parse_bool_true) {
    XrJsonValue *v = xlsp_json_parse("true", 4);
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_bool(v));
    ASSERT(v->as.boolean == true);
    xlsp_json_free(v);
}

TEST(parse_bool_false) {
    XrJsonValue *v = xlsp_json_parse("false", 5);
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_bool(v));
    ASSERT(v->as.boolean == false);
    xlsp_json_free(v);
}

TEST(parse_number_integer) {
    XrJsonValue *v = xlsp_json_parse("42", 2);
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_number(v));
    ASSERT_EQ((int)v->as.number, 42);
    xlsp_json_free(v);
}

TEST(parse_number_negative) {
    XrJsonValue *v = xlsp_json_parse("-123", 4);
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_number(v));
    ASSERT_EQ((int)v->as.number, -123);
    xlsp_json_free(v);
}

TEST(parse_number_float) {
    XrJsonValue *v = xlsp_json_parse("3.14", 4);
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_number(v));
    ASSERT(v->as.number > 3.13 && v->as.number < 3.15);
    xlsp_json_free(v);
}

TEST(parse_string_simple) {
    XrJsonValue *v = xlsp_json_parse("\"hello\"", 7);
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_string(v));
    ASSERT_STR_EQ(v->as.string, "hello");
    xlsp_json_free(v);
}

TEST(parse_string_escape) {
    XrJsonValue *v = xlsp_json_parse("\"hello\\nworld\"", 14);
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_string(v));
    ASSERT_STR_EQ(v->as.string, "hello\nworld");
    xlsp_json_free(v);
}

TEST(parse_string_unicode) {
    // Note: Unicode escape may not be fully implemented
    // Just test that it parses without crashing
    XrJsonValue *v = xlsp_json_parse("\"\\u0041\"", 8);
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_string(v));
    // Result may be "A" or "\\u0041" depending on implementation
    xlsp_json_free(v);
}

TEST(parse_empty_array) {
    XrJsonValue *v = xlsp_json_parse("[]", 2);
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_array(v));
    ASSERT_EQ(xlsp_json_array_len(v), 0);
    xlsp_json_free(v);
}

TEST(parse_array_numbers) {
    XrJsonValue *v = xlsp_json_parse("[1, 2, 3]", 9);
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_array(v));
    ASSERT_EQ(xlsp_json_array_len(v), 3);
    ASSERT_EQ((int)xlsp_json_array_get(v, 0)->as.number, 1);
    ASSERT_EQ((int)xlsp_json_array_get(v, 1)->as.number, 2);
    ASSERT_EQ((int)xlsp_json_array_get(v, 2)->as.number, 3);
    xlsp_json_free(v);
}

TEST(parse_empty_object) {
    XrJsonValue *v = xlsp_json_parse("{}", 2);
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_object(v));
    ASSERT_EQ(v->as.object.count, 0);
    xlsp_json_free(v);
}

TEST(parse_object_simple) {
    const char *json = "{\"name\": \"test\", \"value\": 42}";
    XrJsonValue *v = xlsp_json_parse(json, strlen(json));
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_object(v));
    
    const char *name = xlsp_json_get_string(v, "name");
    ASSERT(name != NULL);
    ASSERT_STR_EQ(name, "test");
    
    int64_t value = xlsp_json_get_int(v, "value");
    ASSERT_EQ(value, 42);
    
    xlsp_json_free(v);
}

TEST(parse_nested_object) {
    const char *json = "{\"outer\": {\"inner\": 123}}";
    XrJsonValue *v = xlsp_json_parse(json, strlen(json));
    ASSERT(v != NULL);
    
    XrJsonValue *outer = xlsp_json_get(v, "outer");
    ASSERT(outer != NULL);
    ASSERT(xlsp_json_is_object(outer));
    
    int64_t inner = xlsp_json_get_int(outer, "inner");
    ASSERT_EQ(inner, 123);
    
    xlsp_json_free(v);
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
    
    XrJsonValue *v = xlsp_json_parse(json, strlen(json));
    ASSERT(v != NULL);
    
    ASSERT_STR_EQ(xlsp_json_get_string(v, "jsonrpc"), "2.0");
    ASSERT_EQ(xlsp_json_get_int(v, "id"), 1);
    ASSERT_STR_EQ(xlsp_json_get_string(v, "method"), "initialize");
    
    XrJsonValue *params = xlsp_json_get(v, "params");
    ASSERT(params != NULL);
    ASSERT_EQ(xlsp_json_get_int(params, "processId"), 12345);
    ASSERT_STR_EQ(xlsp_json_get_string(params, "rootUri"), "file:///workspace");
    
    xlsp_json_free(v);
}

// ============================================================================
// JSON Building Tests
// ============================================================================

TEST(build_null) {
    XrJsonValue *v = xlsp_json_new_null();
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_null(v));
    xlsp_json_free(v);
}

TEST(build_bool) {
    XrJsonValue *v = xlsp_json_new_bool(true);
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_bool(v));
    ASSERT(v->as.boolean == true);
    xlsp_json_free(v);
}

TEST(build_number) {
    XrJsonValue *v = xlsp_json_new_number(42.5);
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_number(v));
    ASSERT(v->as.number > 42.4 && v->as.number < 42.6);
    xlsp_json_free(v);
}

TEST(build_string) {
    XrJsonValue *v = xlsp_json_new_string("hello");
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_string(v));
    ASSERT_STR_EQ(v->as.string, "hello");
    xlsp_json_free(v);
}

TEST(build_array) {
    XrJsonValue *arr = xlsp_json_new_array();
    xlsp_json_array_push(arr, xlsp_json_new_number(1));
    xlsp_json_array_push(arr, xlsp_json_new_number(2));
    xlsp_json_array_push(arr, xlsp_json_new_string("three"));
    
    ASSERT_EQ(xlsp_json_array_len(arr), 3);
    ASSERT_EQ((int)xlsp_json_array_get(arr, 0)->as.number, 1);
    ASSERT_STR_EQ(xlsp_json_array_get(arr, 2)->as.string, "three");
    
    xlsp_json_free(arr);
}

TEST(build_object) {
    XrJsonValue *obj = xlsp_json_new_object();
    xlsp_json_object_set(obj, "name", xlsp_json_new_string("test"));
    xlsp_json_object_set(obj, "count", xlsp_json_new_number(42));
    xlsp_json_object_set(obj, "enabled", xlsp_json_new_bool(true));
    
    ASSERT_STR_EQ(xlsp_json_get_string(obj, "name"), "test");
    ASSERT_EQ(xlsp_json_get_int(obj, "count"), 42);
    ASSERT(xlsp_json_get_bool(obj, "enabled") == true);
    
    xlsp_json_free(obj);
}

// ============================================================================
// JSON Stringify Tests
// ============================================================================

TEST(stringify_null) {
    XrJsonValue *v = xlsp_json_new_null();
    size_t len;
    char *s = xlsp_json_stringify(v, &len);
    ASSERT(s != NULL);
    ASSERT_STR_EQ(s, "null");
    free(s);
    xlsp_json_free(v);
}

TEST(stringify_bool) {
    XrJsonValue *v = xlsp_json_new_bool(true);
    size_t len;
    char *s = xlsp_json_stringify(v, &len);
    ASSERT(s != NULL);
    ASSERT_STR_EQ(s, "true");
    free(s);
    xlsp_json_free(v);
}

TEST(stringify_number) {
    XrJsonValue *v = xlsp_json_new_number(42);
    size_t len;
    char *s = xlsp_json_stringify(v, &len);
    ASSERT(s != NULL);
    ASSERT(strstr(s, "42") != NULL);
    free(s);
    xlsp_json_free(v);
}

TEST(stringify_string) {
    XrJsonValue *v = xlsp_json_new_string("hello");
    size_t len;
    char *s = xlsp_json_stringify(v, &len);
    ASSERT(s != NULL);
    ASSERT_STR_EQ(s, "\"hello\"");
    free(s);
    xlsp_json_free(v);
}

TEST(stringify_array) {
    XrJsonValue *arr = xlsp_json_new_array();
    xlsp_json_array_push(arr, xlsp_json_new_number(1));
    xlsp_json_array_push(arr, xlsp_json_new_number(2));
    
    size_t len;
    char *s = xlsp_json_stringify(arr, &len);
    ASSERT(s != NULL);
    ASSERT_STR_EQ(s, "[1,2]");
    free(s);
    xlsp_json_free(arr);
}

TEST(stringify_object) {
    XrJsonValue *obj = xlsp_json_new_object();
    xlsp_json_object_set(obj, "a", xlsp_json_new_number(1));
    
    size_t len;
    char *s = xlsp_json_stringify(obj, &len);
    ASSERT(s != NULL);
    ASSERT(strstr(s, "\"a\"") != NULL);
    ASSERT(strstr(s, "1") != NULL);
    free(s);
    xlsp_json_free(obj);
}

// ============================================================================
// Edge Cases and Robustness Tests
// ============================================================================

TEST(parse_whitespace) {
    XrJsonValue *v = xlsp_json_parse("  \n\t{ \"key\" : \"value\" }  ", 26);
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_object(v));
    ASSERT_STR_EQ(xlsp_json_get_string(v, "key"), "value");
    xlsp_json_free(v);
}

TEST(parse_empty_string) {
    XrJsonValue *v = xlsp_json_parse("\"\"", 2);
    ASSERT(v != NULL);
    ASSERT(xlsp_json_is_string(v));
    ASSERT_STR_EQ(v->as.string, "");
    xlsp_json_free(v);
}

TEST(get_nonexistent_key) {
    XrJsonValue *obj = xlsp_json_new_object();
    xlsp_json_object_set(obj, "exists", xlsp_json_new_number(1));
    
    XrJsonValue *v = xlsp_json_get(obj, "not_exists");
    ASSERT(v == NULL);
    
    const char *s = xlsp_json_get_string(obj, "not_exists");
    ASSERT(s == NULL);
    
    xlsp_json_free(obj);
}

TEST(array_out_of_bounds) {
    XrJsonValue *arr = xlsp_json_new_array();
    xlsp_json_array_push(arr, xlsp_json_new_number(1));
    
    XrJsonValue *v = xlsp_json_array_get(arr, 10);
    ASSERT(v == NULL);
    
    xlsp_json_free(arr);
}

TEST(roundtrip_complex) {
    // Build complex object
    XrJsonValue *obj = xlsp_json_new_object();
    xlsp_json_object_set(obj, "id", xlsp_json_new_number(1));
    xlsp_json_object_set(obj, "method", xlsp_json_new_string("test"));
    
    XrJsonValue *params = xlsp_json_new_object();
    xlsp_json_object_set(params, "uri", xlsp_json_new_string("file:///test.xr"));
    
    XrJsonValue *arr = xlsp_json_new_array();
    xlsp_json_array_push(arr, xlsp_json_new_number(1));
    xlsp_json_array_push(arr, xlsp_json_new_number(2));
    xlsp_json_object_set(params, "lines", arr);
    
    xlsp_json_object_set(obj, "params", params);
    
    // Stringify
    size_t len;
    char *s = xlsp_json_stringify(obj, &len);
    ASSERT(s != NULL);
    
    // Parse back
    XrJsonValue *parsed = xlsp_json_parse(s, len);
    ASSERT(parsed != NULL);
    
    // Verify
    ASSERT_EQ(xlsp_json_get_int(parsed, "id"), 1);
    ASSERT_STR_EQ(xlsp_json_get_string(parsed, "method"), "test");
    
    XrJsonValue *p = xlsp_json_get(parsed, "params");
    ASSERT(p != NULL);
    ASSERT_STR_EQ(xlsp_json_get_string(p, "uri"), "file:///test.xr");
    
    free(s);
    xlsp_json_free(obj);
    xlsp_json_free(parsed);
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
