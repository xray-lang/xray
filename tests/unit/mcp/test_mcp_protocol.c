/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_mcp_protocol.c - Unit tests for MCP protocol, tools, resources, prompts
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../../../src/app/mcp/xmcp_protocol.h"
#include "../../../src/app/mcp/xmcp_server.h"
#include "../../../src/app/mcp/xmcp_tools.h"
#include "../../../src/app/mcp/xmcp_resources.h"
#include "../../../src/app/mcp/xmcp_prompts.h"
#include "../../../src/app/mcp/xmcp_knowledge.h"
#include "../../../src/app/lsp/xlsp_json.h"
#include "../../../src/base/xmalloc.h"

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
#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)

/* =========================================================================
 * Protocol error codes
 * ========================================================================= */

TEST(error_codes_standard_range) {
    /* JSON-RPC 2.0 standard error codes */
    ASSERT_EQ(XMCP_ERR_PARSE, -32700);
    ASSERT_EQ(XMCP_ERR_INVALID_REQ, -32600);
    ASSERT_EQ(XMCP_ERR_METHOD_NOT_FOUND, -32601);
    ASSERT_EQ(XMCP_ERR_INVALID_PARAMS, -32602);
    ASSERT_EQ(XMCP_ERR_INTERNAL, -32603);
}

TEST(error_codes_mcp_range) {
    /* MCP-specific codes in -32000 to -32099 range */
    ASSERT_EQ(XMCP_ERR_NOT_INITIALIZED, -32002);
    ASSERT_EQ(XMCP_ERR_ALREADY_INIT, -32003);
    ASSERT(XMCP_ERR_NOT_INITIALIZED >= -32099);
    ASSERT(XMCP_ERR_NOT_INITIALIZED <= -32000);
}

/* =========================================================================
 * Protocol constants
 * ========================================================================= */

TEST(protocol_version) {
    ASSERT_STR_EQ(XMCP_PROTOCOL_VERSION, "2025-03-26");
}

TEST(server_name) {
    ASSERT_STR_EQ(XMCP_SERVER_NAME, "xray-mcp-server");
}

TEST(server_version_defined) {
    ASSERT_NOT_NULL(XMCP_SERVER_VERSION);
    /* Version string should be non-empty */
    ASSERT(strlen(XMCP_SERVER_VERSION) > 0);
}

/* =========================================================================
 * Initialize handler
 * ========================================================================= */

TEST(initialize_returns_protocol_version) {
    /* Create a minimal server struct for testing */
    XmcpServer server = {
        .has_tools = true,
        .has_resources = true,
        .has_prompts = true,
        .initialized = false,
    };

    XrJsonValue *result = xmcp_handle_initialize(&server, NULL);
    ASSERT_NOT_NULL(result);

    const char *version = xlsp_json_get_string(result, "protocolVersion");
    ASSERT_NOT_NULL(version);
    ASSERT_STR_EQ(version, "2025-03-26");

    xlsp_json_free(result);
}

TEST(initialize_returns_server_info) {
    XmcpServer server = {
        .has_tools = true,
        .has_resources = true,
        .has_prompts = false,
    };

    XrJsonValue *result = xmcp_handle_initialize(&server, NULL);
    ASSERT_NOT_NULL(result);

    XrJsonValue *info = xlsp_json_get_object(result, "serverInfo");
    ASSERT_NOT_NULL(info);

    const char *name = xlsp_json_get_string(info, "name");
    ASSERT_NOT_NULL(name);
    ASSERT_STR_EQ(name, "xray-mcp-server");

    const char *ver = xlsp_json_get_string(info, "version");
    ASSERT_NOT_NULL(ver);
    ASSERT(strlen(ver) > 0);

    xlsp_json_free(result);
}

TEST(initialize_capabilities_with_all_features) {
    XmcpServer server = {
        .has_tools = true,
        .has_resources = true,
        .has_prompts = true,
    };

    XrJsonValue *result = xmcp_handle_initialize(&server, NULL);
    ASSERT_NOT_NULL(result);

    XrJsonValue *caps = xlsp_json_get_object(result, "capabilities");
    ASSERT_NOT_NULL(caps);

    /* tools capability present */
    XrJsonValue *tools = xlsp_json_get_object(caps, "tools");
    ASSERT_NOT_NULL(tools);

    /* resources capability present */
    XrJsonValue *resources = xlsp_json_get_object(caps, "resources");
    ASSERT_NOT_NULL(resources);

    /* prompts capability present */
    XrJsonValue *prompts = xlsp_json_get_object(caps, "prompts");
    ASSERT_NOT_NULL(prompts);

    /* logging always present */
    XrJsonValue *logging = xlsp_json_get_object(caps, "logging");
    ASSERT_NOT_NULL(logging);

    xlsp_json_free(result);
}

TEST(initialize_capabilities_without_prompts) {
    XmcpServer server = {
        .has_tools = true,
        .has_resources = true,
        .has_prompts = false,
    };

    XrJsonValue *result = xmcp_handle_initialize(&server, NULL);
    ASSERT_NOT_NULL(result);

    XrJsonValue *caps = xlsp_json_get_object(result, "capabilities");
    ASSERT_NOT_NULL(caps);

    /* tools and resources present */
    ASSERT_NOT_NULL(xlsp_json_get_object(caps, "tools"));
    ASSERT_NOT_NULL(xlsp_json_get_object(caps, "resources"));

    /* prompts should NOT be present */
    XrJsonValue *prompts = xlsp_json_get_object(caps, "prompts");
    ASSERT(prompts == NULL);

    /* logging always present */
    ASSERT_NOT_NULL(xlsp_json_get_object(caps, "logging"));

    xlsp_json_free(result);
}

TEST(initialize_capabilities_minimal) {
    XmcpServer server = {
        .has_tools = false,
        .has_resources = false,
        .has_prompts = false,
    };

    XrJsonValue *result = xmcp_handle_initialize(&server, NULL);
    ASSERT_NOT_NULL(result);

    XrJsonValue *caps = xlsp_json_get_object(result, "capabilities");
    ASSERT_NOT_NULL(caps);

    /* Only logging should be present */
    ASSERT(xlsp_json_get_object(caps, "tools") == NULL);
    ASSERT(xlsp_json_get_object(caps, "resources") == NULL);
    ASSERT(xlsp_json_get_object(caps, "prompts") == NULL);
    ASSERT_NOT_NULL(xlsp_json_get_object(caps, "logging"));

    xlsp_json_free(result);
}

/* =========================================================================
 * Tools: tools/list
 * ========================================================================= */

TEST(tools_list_returns_three_tools) {
    XrJsonValue *result = xmcp_handle_tools_list();
    ASSERT_NOT_NULL(result);

    XrJsonValue *tools = xlsp_json_get_array(result, "tools");
    ASSERT_NOT_NULL(tools);
    ASSERT_EQ(xlsp_json_array_len(tools), 3);

    xlsp_json_free(result);
}

TEST(tools_list_has_required_fields) {
    XrJsonValue *result = xmcp_handle_tools_list();
    ASSERT_NOT_NULL(result);

    XrJsonValue *tools = xlsp_json_get_array(result, "tools");
    for (int i = 0; i < xlsp_json_array_len(tools); i++) {
        XrJsonValue *tool = xlsp_json_array_get(tools, i);
        ASSERT_NOT_NULL(xlsp_json_get_string(tool, "name"));
        ASSERT_NOT_NULL(xlsp_json_get_string(tool, "description"));
        ASSERT_NOT_NULL(xlsp_json_get_object(tool, "inputSchema"));
    }

    xlsp_json_free(result);
}

TEST(tools_list_has_annotations) {
    XrJsonValue *result = xmcp_handle_tools_list();
    ASSERT_NOT_NULL(result);

    XrJsonValue *tools = xlsp_json_get_array(result, "tools");
    for (int i = 0; i < xlsp_json_array_len(tools); i++) {
        XrJsonValue *tool = xlsp_json_array_get(tools, i);
        XrJsonValue *ann = xlsp_json_get_object(tool, "annotations");
        ASSERT_NOT_NULL(ann);
        ASSERT_NOT_NULL(xlsp_json_get_string(ann, "title"));
        /* All current tools are read-only */
        ASSERT(xlsp_json_get_bool(ann, "readOnlyHint") == true);
        ASSERT(xlsp_json_get_bool(ann, "destructiveHint") == false);
    }

    xlsp_json_free(result);
}

TEST(tools_list_tool_names) {
    XrJsonValue *result = xmcp_handle_tools_list();
    ASSERT_NOT_NULL(result);

    XrJsonValue *tools = xlsp_json_get_array(result, "tools");
    const char *expected[] = {"xray_check", "xray_syntax_lookup", "xray_stdlib_search"};

    for (int i = 0; i < 3; i++) {
        XrJsonValue *tool = xlsp_json_array_get(tools, i);
        ASSERT_STR_EQ(xlsp_json_get_string(tool, "name"), expected[i]);
    }

    xlsp_json_free(result);
}

/* =========================================================================
 * Tools: tools/call
 * ========================================================================= */

TEST(tools_call_unknown_tool) {
    XmcpServer server = {0};
    XrJsonValue *params = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(params, "name", "nonexistent_tool");

    XrJsonValue *result = xmcp_handle_tools_call(&server, params);
    ASSERT_NOT_NULL(result);

    /* Should have isError=true */
    ASSERT(xlsp_json_get_bool(result, "isError") == true);

    xlsp_json_free(params);
    xlsp_json_free(result);
}

TEST(tools_call_missing_name) {
    XmcpServer server = {0};
    XrJsonValue *params = xlsp_json_new_object();

    XrJsonValue *result = xmcp_handle_tools_call(&server, params);
    ASSERT_NOT_NULL(result);
    ASSERT(xlsp_json_get_bool(result, "isError") == true);

    xlsp_json_free(params);
    xlsp_json_free(result);
}

/* =========================================================================
 * Resources: resources/list
 * ========================================================================= */

TEST(resources_list_returns_three) {
    XmcpServer server = {0};
    XrJsonValue *result = xmcp_handle_resources_list(&server);
    ASSERT_NOT_NULL(result);

    XrJsonValue *resources = xlsp_json_get_array(result, "resources");
    ASSERT_NOT_NULL(resources);
    ASSERT_EQ(xlsp_json_array_len(resources), 3);

    xlsp_json_free(result);
}

TEST(resources_list_has_required_fields) {
    XmcpServer server = {0};
    XrJsonValue *result = xmcp_handle_resources_list(&server);
    ASSERT_NOT_NULL(result);

    XrJsonValue *resources = xlsp_json_get_array(result, "resources");
    for (int i = 0; i < xlsp_json_array_len(resources); i++) {
        XrJsonValue *res = xlsp_json_array_get(resources, i);
        ASSERT_NOT_NULL(xlsp_json_get_string(res, "uri"));
        ASSERT_NOT_NULL(xlsp_json_get_string(res, "name"));
        ASSERT_NOT_NULL(xlsp_json_get_string(res, "mimeType"));
    }

    xlsp_json_free(result);
}

TEST(resources_list_uris) {
    XmcpServer server = {0};
    XrJsonValue *result = xmcp_handle_resources_list(&server);
    ASSERT_NOT_NULL(result);

    XrJsonValue *resources = xlsp_json_get_array(result, "resources");
    const char *expected[] = {
        "xray://spec/cheatsheet",
        "xray://spec/concurrency",
        "xray://stdlib/modules"
    };

    for (int i = 0; i < 3; i++) {
        XrJsonValue *res = xlsp_json_array_get(resources, i);
        ASSERT_STR_EQ(xlsp_json_get_string(res, "uri"), expected[i]);
    }

    xlsp_json_free(result);
}

/* =========================================================================
 * Resources: resources/read
 * ========================================================================= */

TEST(resources_read_cheatsheet) {
    XmcpServer server = {0};
    XrJsonValue *params = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(params, "uri", "xray://spec/cheatsheet");

    XrJsonValue *result = xmcp_handle_resources_read(&server, params);
    ASSERT_NOT_NULL(result);

    XrJsonValue *contents = xlsp_json_get_array(result, "contents");
    ASSERT_NOT_NULL(contents);
    ASSERT_EQ(xlsp_json_array_len(contents), 1);

    XrJsonValue *item = xlsp_json_array_get(contents, 0);
    ASSERT_STR_EQ(xlsp_json_get_string(item, "uri"), "xray://spec/cheatsheet");
    ASSERT_STR_EQ(xlsp_json_get_string(item, "mimeType"), "text/markdown");

    xlsp_json_free(params);
    xlsp_json_free(result);
}

TEST(resources_read_unknown_uri) {
    XmcpServer server = {0};
    XrJsonValue *params = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(params, "uri", "xray://nonexistent");

    XrJsonValue *result = xmcp_handle_resources_read(&server, params);
    ASSERT_NOT_NULL(result);

    XrJsonValue *contents = xlsp_json_get_array(result, "contents");
    ASSERT_NOT_NULL(contents);
    ASSERT_EQ(xlsp_json_array_len(contents), 0);

    xlsp_json_free(params);
    xlsp_json_free(result);
}

/* =========================================================================
 * Prompts: prompts/list
 * ========================================================================= */

TEST(prompts_list_returns_five) {
    XrJsonValue *result = xmcp_handle_prompts_list();
    ASSERT_NOT_NULL(result);

    XrJsonValue *prompts = xlsp_json_get_array(result, "prompts");
    ASSERT_NOT_NULL(prompts);
    ASSERT_EQ(xlsp_json_array_len(prompts), 5);

    xlsp_json_free(result);
}

TEST(prompts_list_has_required_fields) {
    XrJsonValue *result = xmcp_handle_prompts_list();
    ASSERT_NOT_NULL(result);

    XrJsonValue *prompts = xlsp_json_get_array(result, "prompts");
    for (int i = 0; i < xlsp_json_array_len(prompts); i++) {
        XrJsonValue *p = xlsp_json_array_get(prompts, i);
        ASSERT_NOT_NULL(xlsp_json_get_string(p, "name"));
        ASSERT_NOT_NULL(xlsp_json_get_string(p, "description"));
    }

    xlsp_json_free(result);
}

TEST(prompts_list_prompt_names) {
    XrJsonValue *result = xmcp_handle_prompts_list();
    ASSERT_NOT_NULL(result);

    XrJsonValue *prompts = xlsp_json_get_array(result, "prompts");
    const char *expected[] = {
        "code-review", "explain-error", "convert-to-xray",
        "concurrency-pattern", "write-test"
    };

    for (int i = 0; i < 5; i++) {
        XrJsonValue *p = xlsp_json_array_get(prompts, i);
        ASSERT_STR_EQ(xlsp_json_get_string(p, "name"), expected[i]);
    }

    xlsp_json_free(result);
}

TEST(prompts_list_has_arguments) {
    XrJsonValue *result = xmcp_handle_prompts_list();
    ASSERT_NOT_NULL(result);

    XrJsonValue *prompts = xlsp_json_get_array(result, "prompts");

    /* All prompts should have at least one argument */
    for (int i = 0; i < xlsp_json_array_len(prompts); i++) {
        XrJsonValue *p = xlsp_json_array_get(prompts, i);
        XrJsonValue *args = xlsp_json_get_array(p, "arguments");
        ASSERT_NOT_NULL(args);
        ASSERT(xlsp_json_array_len(args) >= 1);
    }

    xlsp_json_free(result);
}

/* =========================================================================
 * Prompts: prompts/get
 * ========================================================================= */

TEST(prompts_get_code_review) {
    XmcpServer server = {0};
    XrJsonValue *params = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(params, "name", "code-review");

    XrJsonValue *args = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(args, "code", "let x = 1\nprint(x)");
    xlsp_json_object_set(params, "arguments", args);

    XrJsonValue *result = xmcp_handle_prompts_get(&server, params);
    ASSERT_NOT_NULL(result);

    XrJsonValue *messages = xlsp_json_get_array(result, "messages");
    ASSERT_NOT_NULL(messages);
    ASSERT(xlsp_json_array_len(messages) >= 2);

    /* First message should have role */
    XrJsonValue *first = xlsp_json_array_get(messages, 0);
    ASSERT_NOT_NULL(xlsp_json_get_string(first, "role"));

    xlsp_json_free(params);
    xlsp_json_free(result);
}

TEST(prompts_get_unknown_prompt) {
    XmcpServer server = {0};
    XrJsonValue *params = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(params, "name", "nonexistent-prompt");

    XrJsonValue *result = xmcp_handle_prompts_get(&server, params);
    ASSERT_NOT_NULL(result);

    /* Should still have messages array (empty) */
    XrJsonValue *messages = xlsp_json_get_array(result, "messages");
    ASSERT_NOT_NULL(messages);
    ASSERT_EQ(xlsp_json_array_len(messages), 0);

    xlsp_json_free(params);
    xlsp_json_free(result);
}

TEST(prompts_get_missing_name) {
    XmcpServer server = {0};
    XrJsonValue *params = xlsp_json_new_object();

    XrJsonValue *result = xmcp_handle_prompts_get(&server, params);
    ASSERT_NOT_NULL(result);

    XrJsonValue *messages = xlsp_json_get_array(result, "messages");
    ASSERT_NOT_NULL(messages);
    ASSERT_EQ(xlsp_json_array_len(messages), 0);

    xlsp_json_free(params);
    xlsp_json_free(result);
}

/* =========================================================================
 * Knowledge base
 * ========================================================================= */

TEST(knowledge_new_and_load) {
    XmcpKnowledge *kb = xmcp_knowledge_new();
    ASSERT_NOT_NULL(kb);
    ASSERT_EQ(kb->topic_count, 0);
    ASSERT_EQ(kb->module_count, 0);

    xmcp_knowledge_load(kb);
    ASSERT(kb->topic_count > 0);
    ASSERT(kb->module_count > 0);

    xmcp_knowledge_free(kb);
}

TEST(knowledge_lookup_exact) {
    XmcpKnowledge *kb = xmcp_knowledge_new();
    ASSERT_NOT_NULL(kb);
    xmcp_knowledge_load(kb);

    const char *content = xmcp_knowledge_lookup_topic(kb, "channel");
    ASSERT_NOT_NULL(content);
    /* Content should mention Channel */
    ASSERT(strstr(content, "Channel") != NULL);

    xmcp_knowledge_free(kb);
}

TEST(knowledge_lookup_alias) {
    XmcpKnowledge *kb = xmcp_knowledge_new();
    ASSERT_NOT_NULL(kb);
    xmcp_knowledge_load(kb);

    /* "fn" is an alias for "functions" */
    const char *content = xmcp_knowledge_lookup_topic(kb, "fn");
    ASSERT_NOT_NULL(content);
    ASSERT(strstr(content, "Functions") != NULL || strstr(content, "fn") != NULL);

    xmcp_knowledge_free(kb);
}

TEST(knowledge_lookup_not_found) {
    XmcpKnowledge *kb = xmcp_knowledge_new();
    ASSERT_NOT_NULL(kb);
    xmcp_knowledge_load(kb);

    const char *content = xmcp_knowledge_lookup_topic(kb, "zzz_nonexistent_topic");
    ASSERT(content == NULL);

    xmcp_knowledge_free(kb);
}

TEST(knowledge_search_stdlib) {
    XmcpKnowledge *kb = xmcp_knowledge_new();
    ASSERT_NOT_NULL(kb);
    xmcp_knowledge_load(kb);

    char *result = xmcp_knowledge_search_stdlib(kb, "http", NULL);
    ASSERT_NOT_NULL(result);
    ASSERT(strstr(result, "http") != NULL);
    xr_free(result);

    xmcp_knowledge_free(kb);
}

TEST(knowledge_search_stdlib_no_match) {
    XmcpKnowledge *kb = xmcp_knowledge_new();
    ASSERT_NOT_NULL(kb);
    xmcp_knowledge_load(kb);

    char *result = xmcp_knowledge_search_stdlib(kb, "zzz_nonexistent", NULL);
    ASSERT_NOT_NULL(result);
    /* Should contain "No modules found" or module list */
    ASSERT(strstr(result, "No modules found") != NULL ||
           strstr(result, "Available modules") != NULL);
    xr_free(result);

    xmcp_knowledge_free(kb);
}

TEST(knowledge_get_cheatsheet) {
    const char *cs = xmcp_knowledge_get_cheatsheet();
    ASSERT_NOT_NULL(cs);
    ASSERT(strstr(cs, "Xray") != NULL);
    ASSERT(strlen(cs) > 100);
}

TEST(knowledge_get_concurrency) {
    const char *cm = xmcp_knowledge_get_concurrency();
    ASSERT_NOT_NULL(cm);
    ASSERT(strstr(cm, "Concurrency") != NULL);
}

TEST(knowledge_get_stdlib_list) {
    const char *sl = xmcp_knowledge_get_stdlib_list();
    ASSERT_NOT_NULL(sl);
    ASSERT(strstr(sl, "http") != NULL);
    ASSERT(strstr(sl, "json") != NULL);
}

/* =========================================================================
 * Method dispatch table structure
 * ========================================================================= */

TEST(method_entry_struct_size) {
    /* Verify the struct layout compiles correctly */
    XmcpMethodEntry entry = {
        .method = "test/method",
        .handler = NULL,
        .is_notification = false,
        .needs_init = true
    };
    ASSERT_STR_EQ(entry.method, "test/method");
    ASSERT(entry.handler == NULL);
    ASSERT(entry.is_notification == false);
    ASSERT(entry.needs_init == true);
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
    printf("=== MCP Protocol Tests ===\n");

    /* Error codes */
    RUN_TEST(error_codes_standard_range);
    RUN_TEST(error_codes_mcp_range);

    /* Protocol constants */
    RUN_TEST(protocol_version);
    RUN_TEST(server_name);
    RUN_TEST(server_version_defined);

    /* Initialize */
    RUN_TEST(initialize_returns_protocol_version);
    RUN_TEST(initialize_returns_server_info);
    RUN_TEST(initialize_capabilities_with_all_features);
    RUN_TEST(initialize_capabilities_without_prompts);
    RUN_TEST(initialize_capabilities_minimal);

    /* Tools */
    RUN_TEST(tools_list_returns_three_tools);
    RUN_TEST(tools_list_has_required_fields);
    RUN_TEST(tools_list_has_annotations);
    RUN_TEST(tools_list_tool_names);
    RUN_TEST(tools_call_unknown_tool);
    RUN_TEST(tools_call_missing_name);

    /* Resources */
    RUN_TEST(resources_list_returns_three);
    RUN_TEST(resources_list_has_required_fields);
    RUN_TEST(resources_list_uris);
    RUN_TEST(resources_read_cheatsheet);
    RUN_TEST(resources_read_unknown_uri);

    /* Prompts */
    RUN_TEST(prompts_list_returns_five);
    RUN_TEST(prompts_list_has_required_fields);
    RUN_TEST(prompts_list_prompt_names);
    RUN_TEST(prompts_list_has_arguments);
    RUN_TEST(prompts_get_code_review);
    RUN_TEST(prompts_get_unknown_prompt);
    RUN_TEST(prompts_get_missing_name);

    /* Knowledge base */
    RUN_TEST(knowledge_new_and_load);
    RUN_TEST(knowledge_lookup_exact);
    RUN_TEST(knowledge_lookup_alias);
    RUN_TEST(knowledge_lookup_not_found);
    RUN_TEST(knowledge_search_stdlib);
    RUN_TEST(knowledge_search_stdlib_no_match);
    RUN_TEST(knowledge_get_cheatsheet);
    RUN_TEST(knowledge_get_concurrency);
    RUN_TEST(knowledge_get_stdlib_list);

    /* Dispatch table */
    RUN_TEST(method_entry_struct_size);

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
