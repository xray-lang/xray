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
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_STR_EQ(a, b) ASSERT(strcmp((a), (b)) == 0)


// ============================================================================
// JSON Message Building Tests
// ============================================================================

TEST(build_initialize_response) {
    XrJsonValue *body = xlsp_json_new_object();
    
    // Capabilities
    xlsp_json_object_set(body, "supportsConfigurationDoneRequest", 
                          xlsp_json_new_bool(true));
    xlsp_json_object_set(body, "supportsFunctionBreakpoints",
                          xlsp_json_new_bool(false));
    xlsp_json_object_set(body, "supportsConditionalBreakpoints",
                          xlsp_json_new_bool(true));
    xlsp_json_object_set(body, "supportsEvaluateForHovers",
                          xlsp_json_new_bool(true));
    
    size_t len;
    char *json = xlsp_json_stringify(body, &len);
    ASSERT(json != NULL);
    ASSERT(strstr(json, "supportsConfigurationDoneRequest") != NULL);
    ASSERT(strstr(json, "true") != NULL);
    
    free(json);
    xlsp_json_free(body);
}

TEST(build_stopped_event) {
    XrJsonValue *body = xlsp_json_new_object();
    xlsp_json_object_set(body, "reason", xlsp_json_new_string("breakpoint"));
    xlsp_json_object_set(body, "threadId", xlsp_json_new_number(1));
    xlsp_json_object_set(body, "allThreadsStopped", xlsp_json_new_bool(true));
    
    size_t len;
    char *json = xlsp_json_stringify(body, &len);
    ASSERT(json != NULL);
    ASSERT(strstr(json, "breakpoint") != NULL);
    ASSERT(strstr(json, "threadId") != NULL);
    
    free(json);
    xlsp_json_free(body);
}

TEST(build_stack_frame) {
    XrJsonValue *frame = xlsp_json_new_object();
    xlsp_json_object_set(frame, "id", xlsp_json_new_number(0));
    xlsp_json_object_set(frame, "name", xlsp_json_new_string("main"));
    
    XrJsonValue *source = xlsp_json_new_object();
    xlsp_json_object_set(source, "path", xlsp_json_new_string("/test/file.xr"));
    xlsp_json_object_set(frame, "source", source);
    
    xlsp_json_object_set(frame, "line", xlsp_json_new_number(10));
    xlsp_json_object_set(frame, "column", xlsp_json_new_number(0));
    
    size_t len;
    char *json = xlsp_json_stringify(frame, &len);
    ASSERT(json != NULL);
    ASSERT(strstr(json, "main") != NULL);
    ASSERT(strstr(json, "/test/file.xr") != NULL);
    
    free(json);
    xlsp_json_free(frame);
}

TEST(build_scope) {
    XrJsonValue *scope = xlsp_json_new_object();
    xlsp_json_object_set(scope, "name", xlsp_json_new_string("Locals"));
    xlsp_json_object_set(scope, "variablesReference", xlsp_json_new_number(1000));
    xlsp_json_object_set(scope, "expensive", xlsp_json_new_bool(false));
    
    size_t len;
    char *json = xlsp_json_stringify(scope, &len);
    ASSERT(json != NULL);
    ASSERT(strstr(json, "Locals") != NULL);
    ASSERT(strstr(json, "1000") != NULL);
    
    free(json);
    xlsp_json_free(scope);
}

TEST(build_variable) {
    XrJsonValue *var = xlsp_json_new_object();
    xlsp_json_object_set(var, "name", xlsp_json_new_string("count"));
    xlsp_json_object_set(var, "value", xlsp_json_new_string("42"));
    xlsp_json_object_set(var, "type", xlsp_json_new_string("int"));
    xlsp_json_object_set(var, "variablesReference", xlsp_json_new_number(0));
    
    size_t len;
    char *json = xlsp_json_stringify(var, &len);
    ASSERT(json != NULL);
    ASSERT(strstr(json, "count") != NULL);
    ASSERT(strstr(json, "42") != NULL);
    ASSERT(strstr(json, "int") != NULL);
    
    free(json);
    xlsp_json_free(var);
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
    
    XrJsonValue *msg = xlsp_json_parse(json, strlen(json));
    ASSERT(msg != NULL);
    
    ASSERT_EQ(xlsp_json_get_int(msg, "seq"), 1);
    ASSERT_STR_EQ(xlsp_json_get_string(msg, "type"), "request");
    ASSERT_STR_EQ(xlsp_json_get_string(msg, "command"), "initialize");
    
    XrJsonValue *args = xlsp_json_get(msg, "arguments");
    ASSERT(args != NULL);
    ASSERT_STR_EQ(xlsp_json_get_string(args, "clientID"), "vscode");
    ASSERT(xlsp_json_get_bool(args, "linesStartAt1") == true);
    
    xlsp_json_free(msg);
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
    
    XrJsonValue *msg = xlsp_json_parse(json, strlen(json));
    ASSERT(msg != NULL);
    
    ASSERT_STR_EQ(xlsp_json_get_string(msg, "command"), "setBreakpoints");
    
    XrJsonValue *args = xlsp_json_get(msg, "arguments");
    XrJsonValue *source = xlsp_json_get(args, "source");
    ASSERT_STR_EQ(xlsp_json_get_string(source, "path"), "/test/file.xr");
    
    XrJsonValue *bps = xlsp_json_get(args, "breakpoints");
    ASSERT_EQ(xlsp_json_array_len(bps), 3);
    
    XrJsonValue *bp1 = xlsp_json_array_get(bps, 0);
    ASSERT_EQ(xlsp_json_get_int(bp1, "line"), 10);
    
    XrJsonValue *bp2 = xlsp_json_array_get(bps, 1);
    ASSERT_EQ(xlsp_json_get_int(bp2, "line"), 20);
    ASSERT_STR_EQ(xlsp_json_get_string(bp2, "condition"), "x > 5");
    
    XrJsonValue *bp3 = xlsp_json_array_get(bps, 2);
    ASSERT_STR_EQ(xlsp_json_get_string(bp3, "logMessage"), "value = {x}");
    
    xlsp_json_free(msg);
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
    
    XrJsonValue *msg = xlsp_json_parse(json, strlen(json));
    ASSERT(msg != NULL);
    
    XrJsonValue *args = xlsp_json_get(msg, "arguments");
    ASSERT_STR_EQ(xlsp_json_get_string(args, "expression"), "x + y * 2");
    ASSERT_EQ(xlsp_json_get_int(args, "frameId"), 0);
    ASSERT_STR_EQ(xlsp_json_get_string(args, "context"), "watch");
    
    xlsp_json_free(msg);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
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
