/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_dap_server.c - Unit tests for DAP message building/parsing
 *
 * Tests JSON message format correctness for DAP protocol.
 * Does not test actual server functionality (requires full runtime).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../src/base/xjson.h"
#include "../test_win_compat.h"

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
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_STR_EQ(a, b) ASSERT(strcmp((a), (b)) == 0)


// ============================================================================
// JSON Message Building Tests
// ============================================================================

TEST(build_initialize_response) {
    XrJsonValue *body = xjson_new_object();
    
    // Capabilities
    xjson_object_set(body, "supportsConfigurationDoneRequest", 
                          xjson_new_bool(true));
    xjson_object_set(body, "supportsFunctionBreakpoints",
                          xjson_new_bool(false));
    xjson_object_set(body, "supportsConditionalBreakpoints",
                          xjson_new_bool(true));
    xjson_object_set(body, "supportsEvaluateForHovers",
                          xjson_new_bool(true));
    
    size_t len;
    char *json = xjson_stringify(body, &len);
    ASSERT(json != NULL);
    ASSERT(strstr(json, "supportsConfigurationDoneRequest") != NULL);
    ASSERT(strstr(json, "true") != NULL);
    
    free(json);
    xjson_free(body);
}

TEST(build_stopped_event) {
    XrJsonValue *body = xjson_new_object();
    xjson_object_set(body, "reason", xjson_new_string("breakpoint"));
    xjson_object_set(body, "threadId", xjson_new_number(1));
    xjson_object_set(body, "allThreadsStopped", xjson_new_bool(true));
    
    size_t len;
    char *json = xjson_stringify(body, &len);
    ASSERT(json != NULL);
    ASSERT(strstr(json, "breakpoint") != NULL);
    ASSERT(strstr(json, "threadId") != NULL);
    
    free(json);
    xjson_free(body);
}

TEST(build_stack_frame) {
    XrJsonValue *frame = xjson_new_object();
    xjson_object_set(frame, "id", xjson_new_number(0));
    xjson_object_set(frame, "name", xjson_new_string("main"));
    
    XrJsonValue *source = xjson_new_object();
    xjson_object_set(source, "path", xjson_new_string("/test/file.xr"));
    xjson_object_set(frame, "source", source);
    
    xjson_object_set(frame, "line", xjson_new_number(10));
    xjson_object_set(frame, "column", xjson_new_number(0));
    
    size_t len;
    char *json = xjson_stringify(frame, &len);
    ASSERT(json != NULL);
    ASSERT(strstr(json, "main") != NULL);
    ASSERT(strstr(json, "/test/file.xr") != NULL);
    
    free(json);
    xjson_free(frame);
}

TEST(build_scope) {
    XrJsonValue *scope = xjson_new_object();
    xjson_object_set(scope, "name", xjson_new_string("Locals"));
    xjson_object_set(scope, "variablesReference", xjson_new_number(1000));
    xjson_object_set(scope, "expensive", xjson_new_bool(false));
    
    size_t len;
    char *json = xjson_stringify(scope, &len);
    ASSERT(json != NULL);
    ASSERT(strstr(json, "Locals") != NULL);
    ASSERT(strstr(json, "1000") != NULL);
    
    free(json);
    xjson_free(scope);
}

TEST(build_variable) {
    XrJsonValue *var = xjson_new_object();
    xjson_object_set(var, "name", xjson_new_string("count"));
    xjson_object_set(var, "value", xjson_new_string("42"));
    xjson_object_set(var, "type", xjson_new_string("int"));
    xjson_object_set(var, "variablesReference", xjson_new_number(0));
    
    size_t len;
    char *json = xjson_stringify(var, &len);
    ASSERT(json != NULL);
    ASSERT(strstr(json, "count") != NULL);
    ASSERT(strstr(json, "42") != NULL);
    ASSERT(strstr(json, "int") != NULL);
    
    free(json);
    xjson_free(var);
}

// ============================================================================
// DAP Message Parsing Tests
// ============================================================================

TEST(parse_initialize_request) {
    const char *json = 
        "{"
        "  \"seq\": 1,"
        "  \"type\": \"request\","
        "  \"command\": \"initialize\","
        "  \"arguments\": {"
        "    \"clientID\": \"vscode\","
        "    \"adapterID\": \"xray\","
        "    \"linesStartAt1\": true,"
        "    \"columnsStartAt1\": true"
        "  }"
        "}";
    
    XrJsonValue *msg = xjson_parse(json, strlen(json));
    ASSERT(msg != NULL);
    
    ASSERT_EQ(xjson_get_int(msg, "seq"), 1);
    ASSERT_STR_EQ(xjson_get_string(msg, "type"), "request");
    ASSERT_STR_EQ(xjson_get_string(msg, "command"), "initialize");
    
    XrJsonValue *args = xjson_get(msg, "arguments");
    ASSERT(args != NULL);
    ASSERT_STR_EQ(xjson_get_string(args, "clientID"), "vscode");
    ASSERT(xjson_get_bool(args, "linesStartAt1") == true);
    
    xjson_free(msg);
}

TEST(parse_set_breakpoints_request) {
    const char *json = 
        "{"
        "  \"seq\": 3,"
        "  \"type\": \"request\","
        "  \"command\": \"setBreakpoints\","
        "  \"arguments\": {"
        "    \"source\": {\"path\": \"/test/file.xr\"},"
        "    \"breakpoints\": ["
        "      {\"line\": 10},"
        "      {\"line\": 20, \"condition\": \"x > 5\"},"
        "      {\"line\": 30, \"logMessage\": \"value = {x}\"}"
        "    ]"
        "  }"
        "}";
    
    XrJsonValue *msg = xjson_parse(json, strlen(json));
    ASSERT(msg != NULL);
    
    ASSERT_STR_EQ(xjson_get_string(msg, "command"), "setBreakpoints");
    
    XrJsonValue *args = xjson_get(msg, "arguments");
    XrJsonValue *source = xjson_get(args, "source");
    ASSERT_STR_EQ(xjson_get_string(source, "path"), "/test/file.xr");
    
    XrJsonValue *bps = xjson_get(args, "breakpoints");
    ASSERT_EQ(xjson_array_len(bps), 3);
    
    XrJsonValue *bp1 = xjson_array_get(bps, 0);
    ASSERT_EQ(xjson_get_int(bp1, "line"), 10);
    
    XrJsonValue *bp2 = xjson_array_get(bps, 1);
    ASSERT_EQ(xjson_get_int(bp2, "line"), 20);
    ASSERT_STR_EQ(xjson_get_string(bp2, "condition"), "x > 5");
    
    XrJsonValue *bp3 = xjson_array_get(bps, 2);
    ASSERT_STR_EQ(xjson_get_string(bp3, "logMessage"), "value = {x}");
    
    xjson_free(msg);
}

TEST(parse_evaluate_request) {
    const char *json = 
        "{"
        "  \"seq\": 10,"
        "  \"type\": \"request\","
        "  \"command\": \"evaluate\","
        "  \"arguments\": {"
        "    \"expression\": \"x + y * 2\","
        "    \"frameId\": 0,"
        "    \"context\": \"watch\""
        "  }"
        "}";
    
    XrJsonValue *msg = xjson_parse(json, strlen(json));
    ASSERT(msg != NULL);
    
    XrJsonValue *args = xjson_get(msg, "arguments");
    ASSERT_STR_EQ(xjson_get_string(args, "expression"), "x + y * 2");
    ASSERT_EQ(xjson_get_int(args, "frameId"), 0);
    ASSERT_STR_EQ(xjson_get_string(args, "context"), "watch");
    
    xjson_free(msg);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    xr_test_suppress_dialogs();
    (void)argc; (void)argv;
    
    printf("\n=== DAP Message Format Unit Tests ===\n\n");
    
    printf("JSON message building tests:\n");
    RUN_TEST(build_initialize_response);
    RUN_TEST(build_stopped_event);
    RUN_TEST(build_stack_frame);
    RUN_TEST(build_scope);
    RUN_TEST(build_variable);
    
    printf("\nDAP message parsing tests:\n");
    RUN_TEST(parse_initialize_request);
    RUN_TEST(parse_set_breakpoints_request);
    RUN_TEST(parse_evaluate_request);
    
    printf("\n=== Results: %d passed, %d failed ===\n\n", 
           tests_passed, tests_failed);
    
    return tests_failed > 0 ? 1 : 0;
}
