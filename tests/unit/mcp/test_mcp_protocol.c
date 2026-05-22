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
#include "../../../src/app/mcp/xmcp_jsonrpc.h"
#include "../../../src/app/mcp/xmcp_protocol.h"
#include "../../../src/app/mcp/xmcp_server.h"
#include "../../../src/app/mcp/xmcp_tools.h"
#include "../../../src/app/mcp/xmcp_resources.h"
#include "../../../src/app/mcp/xmcp_prompts.h"
#include "../../../src/app/mcp/xmcp_knowledge.h"
#include "../../../src/base/xjson.h"
#include "../../../src/base/xmalloc.h"
#include "../../../include/xray_isolate.h"
#include "../test_win_compat.h"

/* Stubs for notification functions (implemented in xmcp_server.c, not linked
 * into this test binary). Tools may call these; they are no-ops here. */
void xmcp_send_notification(XmcpServer *s, const char *m, XrJsonValue *p) {
    (void) s;
    (void) m;
    (void) p;
}
void xmcp_send_log_notification(XmcpServer *s, const char *l, const char *m) {
    (void) s;
    (void) l;
    (void) m;
}
void xmcp_send_progress_notification(XmcpServer *s, int64_t t, int p, int tot) {
    (void) s;
    (void) t;
    (void) p;
    (void) tot;
}

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name)                                                                             \
    do {                                                                                           \
        printf("  Testing %s... ", #name);                                                         \
        test_##name();                                                                             \
        printf("PASS\n");                                                                          \
        tests_passed++;                                                                            \
    } while (0)

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL at line %d: %s\n", __LINE__, #cond);                                      \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_STR_EQ(a, b) ASSERT(strcmp((a), (b)) == 0)
#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)

static XrJsonValue *parse_json(const char *json) {
    return xjson_parse(json, strlen(json));
}

static XmcpRegistry test_registry(size_t tools, size_t resources, size_t templates,
                                  size_t prompts) {
    XmcpRegistry registry = {0};
    const XmcpToolDef *tool_table = xmcp_tools_table();
    for (size_t i = 0; i < tools && i < XMCP_REGISTRY_MAX_TOOLS; i++)
        registry.tools[i] = &tool_table[i];
    registry.resources = xmcp_resources_table();
    registry.resource_templates = xmcp_resource_templates_table();
    registry.prompts = xmcp_prompts_table();
    registry.tool_count = tools;
    registry.resource_count = resources;
    registry.resource_template_count = templates;
    registry.prompt_count = prompts;
    return registry;
}

static XmcpServer test_server_with_runner(bool enable_runner) {
    XmcpServer server = {0};
    XmcpRegistryOptions options;
    xmcp_registry_options_default(&options);
    options.enable_runner = enable_runner;
    xmcp_registry_init(&server.registry, &options);
    return server;
}

static XmcpServer test_server(void) {
    return test_server_with_runner(false);
}

static void test_server_load_knowledge(XmcpServer *server) {
    server->knowledge = xmcp_knowledge_new();
    ASSERT_NOT_NULL(server->knowledge);
    xmcp_knowledge_load(server->knowledge);
}

/* Test wrappers for handlers that now return JSON-RPC errors via XmcpRpcError.
 * Tests that don't focus on the RPC error path use these wrappers and let any
 * error pass through to assertions on `g_test_rpc_err`. */
static XmcpRpcError g_test_rpc_err;

static XrJsonValue *call_initialize(XmcpServer *s, XrJsonValue *p) {
    g_test_rpc_err.code = 0;
    g_test_rpc_err.message[0] = '\0';
    return xmcp_handle_initialize(s, p, &g_test_rpc_err);
}

static XrJsonValue *call_tools_list(XmcpServer *s, XrJsonValue *p) {
    g_test_rpc_err.code = 0;
    g_test_rpc_err.message[0] = '\0';
    return xmcp_handle_tools_list(s, p, &g_test_rpc_err);
}

static XrJsonValue *call_tools_call(XmcpServer *s, XrJsonValue *p) {
    g_test_rpc_err.code = 0;
    g_test_rpc_err.message[0] = '\0';
    return xmcp_handle_tools_call(s, p, &g_test_rpc_err);
}

static XrJsonValue *call_resources_list(XmcpServer *s) {
    g_test_rpc_err.code = 0;
    g_test_rpc_err.message[0] = '\0';
    return xmcp_handle_resources_list(s, &g_test_rpc_err);
}

static XrJsonValue *call_resources_read(XmcpServer *s, XrJsonValue *p) {
    g_test_rpc_err.code = 0;
    g_test_rpc_err.message[0] = '\0';
    return xmcp_handle_resources_read(s, p, &g_test_rpc_err);
}

static XrJsonValue *call_resource_templates_list(XmcpServer *s) {
    g_test_rpc_err.code = 0;
    g_test_rpc_err.message[0] = '\0';
    return xmcp_handle_resource_templates_list(s, &g_test_rpc_err);
}

static XrJsonValue *call_prompts_list(XmcpServer *s) {
    g_test_rpc_err.code = 0;
    g_test_rpc_err.message[0] = '\0';
    return xmcp_handle_prompts_list(s, &g_test_rpc_err);
}

static XrJsonValue *call_prompts_get(XmcpServer *s, XrJsonValue *p) {
    g_test_rpc_err.code = 0;
    g_test_rpc_err.message[0] = '\0';
    return xmcp_handle_prompts_get(s, p, &g_test_rpc_err);
}

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
        .registry = test_registry(1, 1, 1, 1),
        .lifecycle_state = XMCP_LIFECYCLE_CREATED,
    };

    XrJsonValue *result = call_initialize(&server, NULL);
    ASSERT_NOT_NULL(result);

    const char *version = xjson_get_string(result, "protocolVersion");
    ASSERT_NOT_NULL(version);
    ASSERT_STR_EQ(version, "2025-03-26");
    ASSERT_EQ(server.lifecycle_state, XMCP_LIFECYCLE_INITIALIZE_SENT);

    xjson_free(result);
}

TEST(initialize_does_not_rewind_ready_state) {
    XmcpServer server = {
        .registry = test_registry(1, 1, 1, 1),
        .lifecycle_state = XMCP_LIFECYCLE_READY,
    };

    XrJsonValue *result = call_initialize(&server, NULL);
    ASSERT_NOT_NULL(result);
    ASSERT_EQ(server.lifecycle_state, XMCP_LIFECYCLE_READY);

    xjson_free(result);
}

/* =========================================================================
 * JSON-RPC validation
 * ========================================================================= */

TEST(jsonrpc_valid_request) {
    XrJsonValue *msg =
        parse_json("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":{}}");
    ASSERT_NOT_NULL(msg);

    XmcpJsonRpcMessage req;
    XrJsonValue *error_id = NULL;
    int error_code = 0;
    const char *error_message = NULL;
    ASSERT(xmcp_jsonrpc_validate_message(msg, &req, &error_id, &error_code, &error_message));
    ASSERT_STR_EQ(req.method, "ping");
    ASSERT_NOT_NULL(req.id);
    ASSERT(req.params != NULL);
    ASSERT(req.is_notification == false);
    ASSERT(error_id == NULL);
    ASSERT_EQ(error_code, 0);
    ASSERT(error_message == NULL);

    xjson_free(msg);
}

TEST(jsonrpc_valid_notification) {
    XrJsonValue *msg = parse_json("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\"}");
    ASSERT_NOT_NULL(msg);

    XmcpJsonRpcMessage req;
    ASSERT(xmcp_jsonrpc_validate_message(msg, &req, NULL, NULL, NULL));
    ASSERT_STR_EQ(req.method, "notifications/cancelled");
    ASSERT(req.id == NULL);
    ASSERT(req.is_notification == true);

    xjson_free(msg);
}

TEST(jsonrpc_rejects_missing_version) {
    XrJsonValue *msg = parse_json("{\"id\":\"abc\",\"method\":\"ping\"}");
    ASSERT_NOT_NULL(msg);

    XmcpJsonRpcMessage req;
    XrJsonValue *error_id = NULL;
    int error_code = 0;
    const char *error_message = NULL;
    ASSERT(!xmcp_jsonrpc_validate_message(msg, &req, &error_id, &error_code, &error_message));
    ASSERT(error_id == xjson_get(msg, "id"));
    ASSERT_EQ(error_code, XMCP_ERR_INVALID_REQ);
    ASSERT_NOT_NULL(error_message);

    xjson_free(msg);
}

TEST(jsonrpc_rejects_invalid_id) {
    XrJsonValue *msg = parse_json("{\"jsonrpc\":\"2.0\",\"id\":{},\"method\":\"ping\"}");
    ASSERT_NOT_NULL(msg);

    XmcpJsonRpcMessage req;
    XrJsonValue *error_id = NULL;
    int error_code = 0;
    ASSERT(!xmcp_jsonrpc_validate_message(msg, &req, &error_id, &error_code, NULL));
    ASSERT(error_id == NULL);
    ASSERT_EQ(error_code, XMCP_ERR_INVALID_REQ);

    xjson_free(msg);
}

TEST(jsonrpc_rejects_missing_method) {
    XrJsonValue *msg = parse_json("{\"jsonrpc\":\"2.0\",\"id\":7}");
    ASSERT_NOT_NULL(msg);

    XmcpJsonRpcMessage req;
    XrJsonValue *error_id = NULL;
    int error_code = 0;
    ASSERT(!xmcp_jsonrpc_validate_message(msg, &req, &error_id, &error_code, NULL));
    ASSERT(error_id == xjson_get(msg, "id"));
    ASSERT_EQ(error_code, XMCP_ERR_INVALID_REQ);

    xjson_free(msg);
}

TEST(jsonrpc_rejects_scalar_params) {
    XrJsonValue *msg =
        parse_json("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\",\"params\":1}");
    ASSERT_NOT_NULL(msg);

    XmcpJsonRpcMessage req;
    int error_code = 0;
    ASSERT(!xmcp_jsonrpc_validate_message(msg, &req, NULL, &error_code, NULL));
    ASSERT_EQ(error_code, XMCP_ERR_INVALID_REQ);

    xjson_free(msg);
}

TEST(jsonrpc_rejects_null_params) {
    XrJsonValue *msg =
        parse_json("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\",\"params\":null}");
    ASSERT_NOT_NULL(msg);

    XmcpJsonRpcMessage req;
    int error_code = 0;
    ASSERT(!xmcp_jsonrpc_validate_message(msg, &req, NULL, &error_code, NULL));
    ASSERT_EQ(error_code, XMCP_ERR_INVALID_REQ);

    xjson_free(msg);
}

TEST(initialize_returns_server_info) {
    XmcpServer server = {
        .registry = test_registry(1, 1, 1, 0),
    };

    XrJsonValue *result = call_initialize(&server, NULL);
    ASSERT_NOT_NULL(result);

    XrJsonValue *info = xjson_get_object(result, "serverInfo");
    ASSERT_NOT_NULL(info);

    const char *name = xjson_get_string(info, "name");
    ASSERT_NOT_NULL(name);
    ASSERT_STR_EQ(name, "xray-mcp-server");

    const char *ver = xjson_get_string(info, "version");
    ASSERT_NOT_NULL(ver);
    ASSERT(strlen(ver) > 0);

    xjson_free(result);
}

TEST(initialize_capabilities_with_all_features) {
    XmcpServer server = {
        .registry = test_registry(1, 1, 1, 1),
    };

    XrJsonValue *result = call_initialize(&server, NULL);
    ASSERT_NOT_NULL(result);

    XrJsonValue *caps = xjson_get_object(result, "capabilities");
    ASSERT_NOT_NULL(caps);

    /* tools capability present */
    XrJsonValue *tools = xjson_get_object(caps, "tools");
    ASSERT_NOT_NULL(tools);

    /* resources capability present */
    XrJsonValue *resources = xjson_get_object(caps, "resources");
    ASSERT_NOT_NULL(resources);

    /* prompts capability present */
    XrJsonValue *prompts = xjson_get_object(caps, "prompts");
    ASSERT_NOT_NULL(prompts);

    /* logging always present */
    XrJsonValue *logging = xjson_get_object(caps, "logging");
    ASSERT_NOT_NULL(logging);

    xjson_free(result);
}

TEST(initialize_capabilities_without_prompts) {
    XmcpServer server = {
        .registry = test_registry(1, 1, 1, 0),
    };

    XrJsonValue *result = call_initialize(&server, NULL);
    ASSERT_NOT_NULL(result);

    XrJsonValue *caps = xjson_get_object(result, "capabilities");
    ASSERT_NOT_NULL(caps);

    /* tools and resources present */
    ASSERT_NOT_NULL(xjson_get_object(caps, "tools"));
    ASSERT_NOT_NULL(xjson_get_object(caps, "resources"));

    /* prompts should NOT be present */
    XrJsonValue *prompts = xjson_get_object(caps, "prompts");
    ASSERT(prompts == NULL);

    /* logging always present */
    ASSERT_NOT_NULL(xjson_get_object(caps, "logging"));

    xjson_free(result);
}

TEST(initialize_capabilities_minimal) {
    XmcpServer server = {
        .registry = test_registry(0, 0, 0, 0),
    };

    XrJsonValue *result = call_initialize(&server, NULL);
    ASSERT_NOT_NULL(result);

    XrJsonValue *caps = xjson_get_object(result, "capabilities");
    ASSERT_NOT_NULL(caps);

    /* Only logging should be present */
    ASSERT(xjson_get_object(caps, "tools") == NULL);
    ASSERT(xjson_get_object(caps, "resources") == NULL);
    ASSERT(xjson_get_object(caps, "prompts") == NULL);
    ASSERT_NOT_NULL(xjson_get_object(caps, "logging"));

    xjson_free(result);
}

TEST(registry_init_counts_features) {
    XmcpRegistry registry;
    XmcpRegistryOptions options;
    xmcp_registry_options_default(&options);
    xmcp_registry_init(&registry, &options);

    ASSERT_NOT_NULL(xmcp_registry_tool_at(&registry, 0));
    ASSERT_NOT_NULL(registry.resources);
    ASSERT_NOT_NULL(registry.resource_templates);
    ASSERT_NOT_NULL(registry.prompts);
    ASSERT_EQ(registry.tool_count, 5);
    ASSERT_EQ(registry.resource_count, 3);
    ASSERT_EQ(registry.resource_template_count, 2);
    ASSERT_EQ(registry.prompt_count, 5);
    ASSERT(xmcp_registry_has_tools(&registry));
    ASSERT(xmcp_registry_has_resources(&registry));
    ASSERT(xmcp_registry_has_prompts(&registry));
}

TEST(registry_finds_tools_by_name) {
    XmcpServer server = test_server();

    const XmcpToolDef *format = xmcp_registry_find_tool(&server.registry, "xray_format");
    ASSERT_NOT_NULL(format);
    ASSERT_STR_EQ(format->name, "xray_format");
    ASSERT_NOT_NULL(format->handler);
    ASSERT(format == xmcp_registry_tool_at(&server.registry, 1));
    ASSERT(xmcp_registry_find_tool(&server.registry, "missing_tool") == NULL);
}

TEST(registry_indexes_resources_and_prompts) {
    XmcpServer server = test_server();

    const XmcpResourceDef *resource = xmcp_registry_resource_at(&server.registry, 0);
    ASSERT_STR_EQ(resource->uri, "xray://spec/cheatsheet");

    const XmcpResourceTemplateDef *resource_template =
        xmcp_registry_resource_template_at(&server.registry, 0);
    ASSERT_STR_EQ(resource_template->uri_template, "xray://spec/topic/{name}");

    const XmcpPromptDef *prompt = xmcp_registry_find_prompt(&server.registry, "code-review");
    ASSERT_NOT_NULL(prompt);
    ASSERT_STR_EQ(prompt->name, "code-review");
    ASSERT(prompt == xmcp_registry_prompt_at(&server.registry, 0));
    ASSERT(xmcp_registry_find_prompt(&server.registry, "missing-prompt") == NULL);
}

/* =========================================================================
 * Tools: tools/list
 * ========================================================================= */

TEST(tools_list_returns_default_tools) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_tools_list(&server, NULL);
    ASSERT_NOT_NULL(result);

    XrJsonValue *tools = xjson_get_array(result, "tools");
    ASSERT_NOT_NULL(tools);
    ASSERT_EQ(xjson_array_len(tools), 5);

    xjson_free(result);
}

TEST(tools_list_has_required_fields) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_tools_list(&server, NULL);
    ASSERT_NOT_NULL(result);

    XrJsonValue *tools = xjson_get_array(result, "tools");
    for (int i = 0; i < xjson_array_len(tools); i++) {
        XrJsonValue *tool = xjson_array_get(tools, i);
        ASSERT_NOT_NULL(xjson_get_string(tool, "name"));
        ASSERT_NOT_NULL(xjson_get_string(tool, "description"));
        ASSERT_NOT_NULL(xjson_get_object(tool, "inputSchema"));
    }

    xjson_free(result);
}

TEST(tools_list_has_annotations) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_tools_list(&server, NULL);
    ASSERT_NOT_NULL(result);

    XrJsonValue *tools = xjson_get_array(result, "tools");
    for (int i = 0; i < xjson_array_len(tools); i++) {
        XrJsonValue *tool = xjson_array_get(tools, i);
        XrJsonValue *ann = xjson_get_object(tool, "annotations");
        ASSERT_NOT_NULL(ann);
        ASSERT_NOT_NULL(xjson_get_string(ann, "title"));
        ASSERT(xjson_get_bool(ann, "readOnlyHint") == true);
        ASSERT(xjson_get_bool(ann, "destructiveHint") == false);
    }

    xjson_free(result);
}

TEST(tools_list_tool_names) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_tools_list(&server, NULL);
    ASSERT_NOT_NULL(result);

    XrJsonValue *tools = xjson_get_array(result, "tools");
    const char *expected[] = {"xray_analyze", "xray_format", "xray_syntax_lookup",
                              "xray_stdlib_search", "xray_definition"};

    for (int i = 0; i < 5; i++) {
        XrJsonValue *tool = xjson_array_get(tools, i);
        ASSERT_STR_EQ(xjson_get_string(tool, "name"), expected[i]);
    }

    xjson_free(result);
}

TEST(tools_list_default_tools_have_output_schema) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_tools_list(&server, NULL);
    XrJsonValue *tools = xjson_get_array(result, "tools");

    for (int i = 0; i < xjson_array_len(tools); i++) {
        XrJsonValue *tool = xjson_array_get(tools, i);
        ASSERT_NOT_NULL(xjson_get_object(tool, "outputSchema"));
    }

    xjson_free(result);
}

/* =========================================================================
 * Tools: tools/call
 * ========================================================================= */

TEST(tools_call_unknown_tool) {
    XmcpServer server = test_server();
    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "nonexistent_tool");

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT(result == NULL);
    ASSERT_EQ(g_test_rpc_err.code, XMCP_ERR_INVALID_PARAMS);
    ASSERT(strstr(g_test_rpc_err.message, "unknown tool") != NULL);

    xjson_free(params);
}

TEST(tools_call_missing_name) {
    XmcpServer server = test_server();
    XrJsonValue *params = xjson_new_object();

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT(result == NULL);
    ASSERT_EQ(g_test_rpc_err.code, XMCP_ERR_INVALID_PARAMS);
    ASSERT(strstr(g_test_rpc_err.message, "'name' is required") != NULL);

    xjson_free(params);
}

TEST(tools_call_rejects_non_object_arguments) {
    XmcpServer server = test_server();
    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "xray_format");
    XJSON_SET_STRING(params, "arguments", "not-an-object");

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT(result == NULL);
    ASSERT_EQ(g_test_rpc_err.code, XMCP_ERR_INVALID_PARAMS);
    ASSERT(strstr(g_test_rpc_err.message, "'arguments' must be an object") != NULL);

    xjson_free(params);
}

TEST(tools_call_validates_required_arguments) {
    XmcpServer server = test_server();
    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "xray_format");
    xjson_object_set(params, "arguments", xjson_new_object());

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT(result == NULL);
    ASSERT_EQ(g_test_rpc_err.code, XMCP_ERR_INVALID_PARAMS);
    ASSERT(strstr(g_test_rpc_err.message, "missing required parameter 'code'") != NULL);

    xjson_free(params);
}

TEST(tools_call_validates_argument_types) {
    XmcpServer server = test_server();
    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "xray_format");
    XrJsonValue *args = xjson_new_object();
    XJSON_SET_STRING(args, "code", "let x = 1\n");
    XJSON_SET_STRING(args, "indentSize", "two");
    xjson_object_set(params, "arguments", args);

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT(result == NULL);
    ASSERT_EQ(g_test_rpc_err.code, XMCP_ERR_INVALID_PARAMS);
    ASSERT(strstr(g_test_rpc_err.message, "invalid type for parameter 'indentSize'") != NULL);
    ASSERT(strstr(g_test_rpc_err.message, "expected integer") != NULL);

    xjson_free(params);
}

/* =========================================================================
 * Tools: xray_format
 * ========================================================================= */

TEST(tools_call_format_missing_code) {
    XmcpServer server = test_server();
    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "xray_format");
    XrJsonValue *args = xjson_new_object();
    xjson_object_set(params, "arguments", args);

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT(result == NULL);
    ASSERT_EQ(g_test_rpc_err.code, XMCP_ERR_INVALID_PARAMS);

    xjson_free(params);
}

TEST(tools_call_format_schema_has_optional_params) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_tools_list(&server, NULL);
    ASSERT_NOT_NULL(result);

    XrJsonValue *tools = xjson_get_array(result, "tools");
    /* xray_format is at index 1 */
    XrJsonValue *fmt_tool = xjson_array_get(tools, 1);
    ASSERT_STR_EQ(xjson_get_string(fmt_tool, "name"), "xray_format");

    XrJsonValue *schema = xjson_get_object(fmt_tool, "inputSchema");
    ASSERT_NOT_NULL(schema);
    XrJsonValue *props = xjson_get_object(schema, "properties");
    ASSERT_NOT_NULL(props);

    /* code is required, indentSize and useTabs are optional */
    ASSERT_NOT_NULL(xjson_get_object(props, "code"));
    ASSERT_NOT_NULL(xjson_get_object(props, "indentSize"));
    ASSERT_NOT_NULL(xjson_get_object(props, "useTabs"));

    XrJsonValue *output_schema = xjson_get_object(fmt_tool, "outputSchema");
    ASSERT_NOT_NULL(output_schema);
    XrJsonValue *output_props = xjson_get_object(output_schema, "properties");
    ASSERT_NOT_NULL(xjson_get_object(output_props, "formattedCode"));
    ASSERT_NOT_NULL(xjson_get_object(output_props, "changed"));
    ASSERT_NOT_NULL(xjson_get_object(output_props, "indentSize"));
    ASSERT_NOT_NULL(xjson_get_object(output_props, "useTabs"));

    xjson_free(result);
}

TEST(tools_call_format_returns_structured_content) {
    XmcpServer server = test_server();
    XrayIsolateParams iso_params;
    xray_isolate_params_init(&iso_params);
    server.isolate = xray_isolate_new(&iso_params);
    ASSERT_NOT_NULL(server.isolate);

    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "xray_format");
    XrJsonValue *args = xjson_new_object();
    XJSON_SET_STRING(args, "code", "let x=1\n");
    XJSON_SET_INT(args, "indentSize", 2);
    xjson_object_set(params, "arguments", args);

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT_NOT_NULL(result);
    ASSERT(xjson_get_bool(result, "isError") == false);
    XrJsonValue *structured = xjson_get_object(result, "structuredContent");
    ASSERT_NOT_NULL(structured);
    ASSERT_NOT_NULL(xjson_get_string(structured, "formattedCode"));
    ASSERT(xjson_get_bool(structured, "changed") == true);
    ASSERT_EQ(xjson_get_int_or(structured, "indentSize", 0), 2);
    ASSERT(xjson_get_bool(structured, "useTabs") == false);
    XrJsonValue *content = xjson_get_array(result, "content");
    XrJsonValue *item = xjson_array_get(content, 0);
    ASSERT_STR_EQ(xjson_get_string(item, "text"), xjson_get_string(structured, "formattedCode"));

    xjson_free(params);
    xjson_free(result);
    xray_isolate_delete(server.isolate);
}

/* =========================================================================
 * Tools: xray_analyze
 * ========================================================================= */

TEST(tools_call_analyze_missing_code) {
    XmcpServer server = test_server();
    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "xray_analyze");
    XrJsonValue *args = xjson_new_object();
    xjson_object_set(params, "arguments", args);

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT(result == NULL);
    ASSERT_EQ(g_test_rpc_err.code, XMCP_ERR_INVALID_PARAMS);

    xjson_free(params);
}

TEST(tools_call_analyze_schema) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_tools_list(&server, NULL);
    XrJsonValue *tools = xjson_get_array(result, "tools");
    XrJsonValue *diag_tool = xjson_array_get(tools, 0);
    ASSERT_STR_EQ(xjson_get_string(diag_tool, "name"), "xray_analyze");

    XrJsonValue *schema = xjson_get_object(diag_tool, "inputSchema");
    ASSERT_NOT_NULL(schema);
    XrJsonValue *props = xjson_get_object(schema, "properties");
    ASSERT_NOT_NULL(xjson_get_object(props, "code"));
    ASSERT_NOT_NULL(xjson_get_object(props, "filename"));
    ASSERT_NOT_NULL(xjson_get_object(props, "mode"));
    ASSERT_NOT_NULL(xjson_get_object(diag_tool, "outputSchema"));

    xjson_free(result);
}

TEST(tools_call_analyze_returns_structured_diagnostics) {
    XmcpServer server = test_server();
    XrayIsolateParams iso_params;
    xray_isolate_params_init(&iso_params);
    xray_isolate_setup_full(&iso_params);
    server.isolate = xray_isolate_new(&iso_params);
    ASSERT_NOT_NULL(server.isolate);

    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "xray_analyze");
    XrJsonValue *args = xjson_new_object();
    XJSON_SET_STRING(args, "code", "let x = ;");
    XJSON_SET_STRING(args, "mode", "syntax");
    xjson_object_set(params, "arguments", args);

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT_NOT_NULL(result);
    ASSERT(xjson_get_bool(result, "isError") == true);
    XrJsonValue *structured = xjson_get_object(result, "structuredContent");
    ASSERT_NOT_NULL(structured);
    ASSERT(xjson_get_bool(structured, "ok") == false);
    ASSERT(xjson_get_int_or(structured, "diagnosticCount", 0) > 0);
    XrJsonValue *diagnostics = xjson_get_array(structured, "diagnostics");
    ASSERT_NOT_NULL(diagnostics);
    ASSERT(xjson_array_len(diagnostics) > 0);

    xjson_free(params);
    xjson_free(result);
    xray_isolate_delete(server.isolate);
}

/* =========================================================================
 * Tools: xray_run
 * ========================================================================= */

TEST(tools_call_run_disabled_by_default) {
    XmcpServer server = test_server();
    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "xray_run");
    XrJsonValue *args = xjson_new_object();
    xjson_object_set(params, "arguments", args);

    /* runner toolset is disabled by default, so xray_run is an unknown tool. */
    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT(result == NULL);
    ASSERT_EQ(g_test_rpc_err.code, XMCP_ERR_INVALID_PARAMS);
    ASSERT(strstr(g_test_rpc_err.message, "unknown tool") != NULL);

    xjson_free(params);
}

TEST(tools_list_runner_enabled_includes_run) {
    XmcpServer server = test_server_with_runner(true);
    XrJsonValue *result = call_tools_list(&server, NULL);
    XrJsonValue *tools = xjson_get_array(result, "tools");
    ASSERT_EQ(xjson_array_len(tools), 6);
    XrJsonValue *run_tool = xjson_array_get(tools, 2);
    ASSERT_STR_EQ(xjson_get_string(run_tool, "name"), "xray_run");

    XrJsonValue *ann = xjson_get_object(run_tool, "annotations");
    ASSERT(xjson_get_bool(ann, "readOnlyHint") == false);
    ASSERT(xjson_get_bool(ann, "openWorldHint") == true);

    xjson_free(result);
}

/* Build a tools/call params object for xray_run. Ownership of `code` stays
 * with the caller; the returned JSON owns its copy. */
static XrJsonValue *make_run_params(const char *code) {
    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "xray_run");
    XrJsonValue *args = xjson_new_object();
    XJSON_SET_STRING(args, "code", code);
    xjson_object_set(params, "arguments", args);
    return params;
}

TEST(tools_call_run_basic_print_returns_structured) {
    XmcpServer server = test_server_with_runner(true);
    XrJsonValue *params = make_run_params("print(\"hello\")\n");

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT_NOT_NULL(result);
    ASSERT(xjson_get_bool(result, "isError") == false);

    XrJsonValue *structured = xjson_get_object(result, "structuredContent");
    ASSERT_NOT_NULL(structured);
    ASSERT(xjson_get_bool(structured, "ok") == true);
    ASSERT_EQ(xjson_get_int_or(structured, "exitCode", -1), 0);
    ASSERT(xjson_get_bool(structured, "timedOut") == false);
    ASSERT(xjson_get_bool(structured, "truncated") == false);
    ASSERT_STR_EQ(xjson_get_string(structured, "stdout"), "hello\n");
    ASSERT_EQ(xjson_get_int_or(structured, "outputBytes", -1), 6);

    xjson_free(params);
    xjson_free(result);
}

TEST(tools_call_run_output_truncated) {
    /* Print a 50-byte line and clamp outputLimit to 10 bytes. */
    XmcpServer server = test_server_with_runner(true);
    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "xray_run");
    XrJsonValue *args = xjson_new_object();
    XJSON_SET_STRING(args, "code", "print(\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\")\n");
    XJSON_SET_INT(args, "outputLimit", 10);
    xjson_object_set(params, "arguments", args);

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT_NOT_NULL(result);

    XrJsonValue *structured = xjson_get_object(result, "structuredContent");
    ASSERT_NOT_NULL(structured);
    ASSERT(xjson_get_bool(structured, "truncated") == true);
    ASSERT_EQ(xjson_get_int_or(structured, "outputBytes", -1), 10);

    xjson_free(params);
    xjson_free(result);
}

TEST(tools_call_run_deadline_exceeded) {
    /* Tight infinite loop, capped at 50ms — the VM back-edge check should
     * abort within a couple of reductions. */
    XmcpServer server = test_server_with_runner(true);
    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "xray_run");
    XrJsonValue *args = xjson_new_object();
    XJSON_SET_STRING(args, "code", "let i = 0\nwhile (i == 0) { let x = 1 }\n");
    XJSON_SET_INT(args, "timeoutMs", 50);
    xjson_object_set(params, "arguments", args);

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT_NOT_NULL(result);
    ASSERT(xjson_get_bool(result, "isError") == true);

    XrJsonValue *structured = xjson_get_object(result, "structuredContent");
    ASSERT_NOT_NULL(structured);
    ASSERT(xjson_get_bool(structured, "ok") == false);
    ASSERT(xjson_get_bool(structured, "timedOut") == true);

    xjson_free(params);
    xjson_free(result);
}

TEST(tools_call_run_blocks_dangerous_import) {
    /* `net` is not in the runner allowlist; the import must fail so the
     * snippet exits non-zero. The structured payload signals !ok. */
    XmcpServer server = test_server_with_runner(true);
    XrJsonValue *params = make_run_params("import net\n");

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT_NOT_NULL(result);
    ASSERT(xjson_get_bool(result, "isError") == true);

    XrJsonValue *structured = xjson_get_object(result, "structuredContent");
    ASSERT_NOT_NULL(structured);
    ASSERT(xjson_get_bool(structured, "ok") == false);
    ASSERT(xjson_get_bool(structured, "timedOut") == false);
    ASSERT(xjson_get_int_or(structured, "exitCode", 0) != 0);

    xjson_free(params);
    xjson_free(result);
}

TEST(tools_call_run_missing_code_is_error_result) {
    /* Empty `code` is a tool-level error, not a JSON-RPC error. */
    XmcpServer server = test_server_with_runner(true);
    XrJsonValue *params = make_run_params("");

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT_NOT_NULL(result);
    ASSERT(xjson_get_bool(result, "isError") == true);

    XrJsonValue *structured = xjson_get_object(result, "structuredContent");
    ASSERT_NOT_NULL(structured);
    ASSERT(xjson_get_bool(structured, "ok") == false);

    xjson_free(params);
    xjson_free(result);
}

/* =========================================================================
 * Tools: knowledge tools
 * ========================================================================= */

TEST(tools_call_syntax_lookup_returns_structured_content) {
    XmcpServer server = test_server();
    test_server_load_knowledge(&server);

    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "xray_syntax_lookup");
    XrJsonValue *args = xjson_new_object();
    XJSON_SET_STRING(args, "topic", "class");
    xjson_object_set(params, "arguments", args);

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT_NOT_NULL(result);
    ASSERT(xjson_get_bool(result, "isError") == false);
    XrJsonValue *structured = xjson_get_object(result, "structuredContent");
    ASSERT_NOT_NULL(structured);
    ASSERT_STR_EQ(xjson_get_string(structured, "topic"), "class");
    ASSERT(xjson_get_bool(structured, "found") == true);
    ASSERT_NOT_NULL(strstr(xjson_get_string(structured, "content"), "Classes"));

    xjson_free(params);
    xjson_free(result);
    xmcp_knowledge_free(server.knowledge);
}

TEST(tools_call_stdlib_search_returns_structured_content) {
    XmcpServer server = test_server();
    test_server_load_knowledge(&server);

    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "xray_stdlib_search");
    XrJsonValue *args = xjson_new_object();
    XJSON_SET_STRING(args, "query", "http");
    xjson_object_set(params, "arguments", args);

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT_NOT_NULL(result);
    ASSERT(xjson_get_bool(result, "isError") == false);
    XrJsonValue *structured = xjson_get_object(result, "structuredContent");
    ASSERT_NOT_NULL(structured);
    ASSERT_STR_EQ(xjson_get_string(structured, "query"), "http");
    ASSERT(xjson_get_bool(structured, "found") == true);
    ASSERT(xjson_get_int_or(structured, "matchCount", 0) > 0);
    ASSERT_NOT_NULL(strstr(xjson_get_string(structured, "content"), "Module: http"));
    XrJsonValue *matches = xjson_get_array(structured, "matches");
    ASSERT_NOT_NULL(matches);
    ASSERT(xjson_array_len(matches) > 0);
    XrJsonValue *first = xjson_array_get(matches, 0);
    ASSERT_NOT_NULL(xjson_get_string(first, "module"));
    ASSERT_NOT_NULL(xjson_get_string(first, "summary"));
    ASSERT(xjson_get_int_or(first, "score", 0) > 0);

    xjson_free(params);
    xjson_free(result);
    xmcp_knowledge_free(server.knowledge);
}

TEST(tools_call_definition_returns_structured_content) {
    XmcpServer server = test_server();
    test_server_load_knowledge(&server);

    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "xray_definition");
    XrJsonValue *args = xjson_new_object();
    XJSON_SET_STRING(args, "symbol", "class");
    xjson_object_set(params, "arguments", args);

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT_NOT_NULL(result);
    ASSERT(xjson_get_bool(result, "isError") == false);
    XrJsonValue *structured = xjson_get_object(result, "structuredContent");
    ASSERT_NOT_NULL(structured);
    ASSERT_STR_EQ(xjson_get_string(structured, "symbol"), "class");
    ASSERT_STR_EQ(xjson_get_string(structured, "kind"), "syntax");
    ASSERT(xjson_get_bool(structured, "found") == true);

    xjson_free(params);
    xjson_free(result);
    xmcp_knowledge_free(server.knowledge);
}

TEST(tools_call_definition_not_found_is_structured) {
    XmcpServer server = test_server();
    test_server_load_knowledge(&server);

    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "xray_definition");
    XrJsonValue *args = xjson_new_object();
    XJSON_SET_STRING(args, "symbol", "zzz_nonexistent_symbol");
    xjson_object_set(params, "arguments", args);

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT_NOT_NULL(result);
    ASSERT(xjson_get_bool(result, "isError") == false);
    XrJsonValue *structured = xjson_get_object(result, "structuredContent");
    ASSERT_NOT_NULL(structured);
    ASSERT_STR_EQ(xjson_get_string(structured, "kind"), "none");
    ASSERT(xjson_get_bool(structured, "found") == false);

    xjson_free(params);
    xjson_free(result);
    xmcp_knowledge_free(server.knowledge);
}

/* =========================================================================
 * Tools: xray_definition
 * ========================================================================= */

TEST(tools_call_definition_missing_symbol) {
    XmcpServer server = test_server();
    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "xray_definition");
    XrJsonValue *args = xjson_new_object();
    xjson_object_set(params, "arguments", args);

    XrJsonValue *result = call_tools_call(&server, params);
    ASSERT(result == NULL);
    ASSERT_EQ(g_test_rpc_err.code, XMCP_ERR_INVALID_PARAMS);

    xjson_free(params);
}

TEST(tools_call_definition_schema) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_tools_list(&server, NULL);
    XrJsonValue *tools = xjson_get_array(result, "tools");
    XrJsonValue *def_tool = xjson_array_get(tools, 4);
    ASSERT_STR_EQ(xjson_get_string(def_tool, "name"), "xray_definition");

    XrJsonValue *schema = xjson_get_object(def_tool, "inputSchema");
    ASSERT_NOT_NULL(schema);
    XrJsonValue *props = xjson_get_object(schema, "properties");
    ASSERT_NOT_NULL(xjson_get_object(props, "symbol"));
    XrJsonValue *output_schema = xjson_get_object(def_tool, "outputSchema");
    ASSERT_NOT_NULL(output_schema);
    XrJsonValue *output_props = xjson_get_object(output_schema, "properties");
    ASSERT_NOT_NULL(xjson_get_object(output_props, "symbol"));
    ASSERT_NOT_NULL(xjson_get_object(output_props, "kind"));
    ASSERT_NOT_NULL(xjson_get_object(output_props, "found"));
    ASSERT_NOT_NULL(xjson_get_object(output_props, "content"));

    xjson_free(result);
}

/* =========================================================================
 * Pagination: tools/list cursor support
 * ========================================================================= */

TEST(tools_list_pagination_no_cursor) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_tools_list(&server, NULL);
    XrJsonValue *tools = xjson_get_array(result, "tools");
    ASSERT_EQ(xjson_array_len(tools), 5);
    /* No nextCursor when all items fit */
    ASSERT(xjson_get_string(result, "nextCursor") == NULL);
    xjson_free(result);
}

/* =========================================================================
 * Resources: resources/list
 * ========================================================================= */

TEST(resources_list_returns_three) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_resources_list(&server);
    ASSERT_NOT_NULL(result);

    XrJsonValue *resources = xjson_get_array(result, "resources");
    ASSERT_NOT_NULL(resources);
    ASSERT_EQ(xjson_array_len(resources), 3);

    xjson_free(result);
}

TEST(resources_list_has_required_fields) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_resources_list(&server);
    ASSERT_NOT_NULL(result);

    XrJsonValue *resources = xjson_get_array(result, "resources");
    for (int i = 0; i < xjson_array_len(resources); i++) {
        XrJsonValue *res = xjson_array_get(resources, i);
        ASSERT_NOT_NULL(xjson_get_string(res, "uri"));
        ASSERT_NOT_NULL(xjson_get_string(res, "name"));
        ASSERT_NOT_NULL(xjson_get_string(res, "mimeType"));
    }

    xjson_free(result);
}

TEST(resources_list_uris) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_resources_list(&server);
    ASSERT_NOT_NULL(result);

    XrJsonValue *resources = xjson_get_array(result, "resources");
    const char *expected[] = {"xray://spec/cheatsheet", "xray://spec/concurrency",
                              "xray://stdlib/modules"};

    for (int i = 0; i < 3; i++) {
        XrJsonValue *res = xjson_array_get(resources, i);
        ASSERT_STR_EQ(xjson_get_string(res, "uri"), expected[i]);
    }

    xjson_free(result);
}

/* =========================================================================
 * Resources: resources/read
 * ========================================================================= */

TEST(resources_read_cheatsheet) {
    XmcpServer server = test_server();
    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "uri", "xray://spec/cheatsheet");

    XrJsonValue *result = call_resources_read(&server, params);
    ASSERT_NOT_NULL(result);

    XrJsonValue *contents = xjson_get_array(result, "contents");
    ASSERT_NOT_NULL(contents);
    ASSERT_EQ(xjson_array_len(contents), 1);

    XrJsonValue *item = xjson_array_get(contents, 0);
    ASSERT_STR_EQ(xjson_get_string(item, "uri"), "xray://spec/cheatsheet");
    ASSERT_STR_EQ(xjson_get_string(item, "mimeType"), "text/markdown");

    xjson_free(params);
    xjson_free(result);
}

TEST(resources_read_unknown_uri) {
    XmcpServer server = test_server();
    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "uri", "xray://nonexistent");

    XrJsonValue *result = call_resources_read(&server, params);
    ASSERT(result == NULL);
    ASSERT_EQ(g_test_rpc_err.code, XMCP_ERR_INVALID_PARAMS);
    ASSERT(strstr(g_test_rpc_err.message, "unknown uri") != NULL);

    xjson_free(params);
}

TEST(resources_read_missing_uri) {
    XmcpServer server = test_server();
    XrJsonValue *params = xjson_new_object();

    XrJsonValue *result = call_resources_read(&server, params);
    ASSERT(result == NULL);
    ASSERT_EQ(g_test_rpc_err.code, XMCP_ERR_INVALID_PARAMS);
    ASSERT(strstr(g_test_rpc_err.message, "'uri' is required") != NULL);

    xjson_free(params);
}

/* =========================================================================
 * Resources: resource templates
 * ========================================================================= */

TEST(resource_templates_list_returns_two) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_resource_templates_list(&server);
    ASSERT_NOT_NULL(result);

    XrJsonValue *templates = xjson_get_array(result, "resourceTemplates");
    ASSERT_NOT_NULL(templates);
    ASSERT_EQ(xjson_array_len(templates), 2);

    xjson_free(result);
}

TEST(resource_templates_have_required_fields) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_resource_templates_list(&server);
    ASSERT_NOT_NULL(result);

    XrJsonValue *templates = xjson_get_array(result, "resourceTemplates");
    for (int i = 0; i < xjson_array_len(templates); i++) {
        XrJsonValue *t = xjson_array_get(templates, i);
        ASSERT_NOT_NULL(xjson_get_string(t, "uriTemplate"));
        ASSERT_NOT_NULL(xjson_get_string(t, "name"));
        ASSERT_NOT_NULL(xjson_get_string(t, "description"));
        ASSERT_NOT_NULL(xjson_get_string(t, "mimeType"));
    }

    xjson_free(result);
}

TEST(resource_templates_uris) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_resource_templates_list(&server);
    ASSERT_NOT_NULL(result);

    XrJsonValue *templates = xjson_get_array(result, "resourceTemplates");
    XrJsonValue *t0 = xjson_array_get(templates, 0);
    ASSERT(strstr(xjson_get_string(t0, "uriTemplate"), "{name}") != NULL);

    XrJsonValue *t1 = xjson_array_get(templates, 1);
    ASSERT(strstr(xjson_get_string(t1, "uriTemplate"), "{module}") != NULL);

    xjson_free(result);
}

TEST(resources_read_topic_template) {
    /* Need knowledge base for template resources */
    XmcpServer server = test_server();
    server.knowledge = xmcp_knowledge_new();
    xmcp_knowledge_load(server.knowledge);

    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "uri", "xray://spec/topic/variables");

    XrJsonValue *result = call_resources_read(&server, params);
    ASSERT_NOT_NULL(result);

    XrJsonValue *contents = xjson_get_array(result, "contents");
    ASSERT_NOT_NULL(contents);
    ASSERT_EQ(xjson_array_len(contents), 1);

    XrJsonValue *item = xjson_array_get(contents, 0);
    ASSERT_STR_EQ(xjson_get_string(item, "uri"), "xray://spec/topic/variables");

    xmcp_knowledge_free(server.knowledge);
    xjson_free(params);
    xjson_free(result);
}

TEST(resources_read_stdlib_template) {
    XmcpServer server = test_server();
    server.knowledge = xmcp_knowledge_new();
    xmcp_knowledge_load(server.knowledge);

    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "uri", "xray://stdlib/http");

    XrJsonValue *result = call_resources_read(&server, params);
    ASSERT_NOT_NULL(result);

    XrJsonValue *contents = xjson_get_array(result, "contents");
    ASSERT_NOT_NULL(contents);
    /* Should find http module */
    ASSERT(xjson_array_len(contents) >= 1);

    xmcp_knowledge_free(server.knowledge);
    xjson_free(params);
    xjson_free(result);
}

/* =========================================================================
 * Prompts: prompts/list
 * ========================================================================= */

TEST(prompts_list_returns_five) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_prompts_list(&server);
    ASSERT_NOT_NULL(result);

    XrJsonValue *prompts = xjson_get_array(result, "prompts");
    ASSERT_NOT_NULL(prompts);
    ASSERT_EQ(xjson_array_len(prompts), 5);

    xjson_free(result);
}

TEST(prompts_list_has_required_fields) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_prompts_list(&server);
    ASSERT_NOT_NULL(result);

    XrJsonValue *prompts = xjson_get_array(result, "prompts");
    for (int i = 0; i < xjson_array_len(prompts); i++) {
        XrJsonValue *p = xjson_array_get(prompts, i);
        ASSERT_NOT_NULL(xjson_get_string(p, "name"));
        ASSERT_NOT_NULL(xjson_get_string(p, "description"));
    }

    xjson_free(result);
}

TEST(prompts_list_prompt_names) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_prompts_list(&server);
    ASSERT_NOT_NULL(result);

    XrJsonValue *prompts = xjson_get_array(result, "prompts");
    const char *expected[] = {"code-review", "explain-error", "convert-to-xray",
                              "concurrency-pattern", "write-test"};

    for (int i = 0; i < 5; i++) {
        XrJsonValue *p = xjson_array_get(prompts, i);
        ASSERT_STR_EQ(xjson_get_string(p, "name"), expected[i]);
    }

    xjson_free(result);
}

TEST(prompts_list_has_arguments) {
    XmcpServer server = test_server();
    XrJsonValue *result = call_prompts_list(&server);
    ASSERT_NOT_NULL(result);

    XrJsonValue *prompts = xjson_get_array(result, "prompts");

    /* All prompts should have at least one argument */
    for (int i = 0; i < xjson_array_len(prompts); i++) {
        XrJsonValue *p = xjson_array_get(prompts, i);
        XrJsonValue *args = xjson_get_array(p, "arguments");
        ASSERT_NOT_NULL(args);
        ASSERT(xjson_array_len(args) >= 1);
    }

    xjson_free(result);
}

/* =========================================================================
 * Prompts: prompts/get
 * ========================================================================= */

TEST(prompts_get_code_review) {
    XmcpServer server = test_server();
    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "code-review");

    XrJsonValue *args = xjson_new_object();
    XJSON_SET_STRING(args, "code", "let x = 1\nprint(x)");
    xjson_object_set(params, "arguments", args);

    XrJsonValue *result = call_prompts_get(&server, params);
    ASSERT_NOT_NULL(result);

    XrJsonValue *messages = xjson_get_array(result, "messages");
    ASSERT_NOT_NULL(messages);
    ASSERT(xjson_array_len(messages) >= 2);

    /* First message should have role */
    XrJsonValue *first = xjson_array_get(messages, 0);
    ASSERT_NOT_NULL(xjson_get_string(first, "role"));

    xjson_free(params);
    xjson_free(result);
}

TEST(prompts_get_unknown_prompt) {
    XmcpServer server = test_server();
    XrJsonValue *params = xjson_new_object();
    XJSON_SET_STRING(params, "name", "nonexistent-prompt");

    XrJsonValue *result = call_prompts_get(&server, params);
    ASSERT(result == NULL);
    ASSERT_EQ(g_test_rpc_err.code, XMCP_ERR_INVALID_PARAMS);
    ASSERT(strstr(g_test_rpc_err.message, "unknown prompt") != NULL);

    xjson_free(params);
}

TEST(prompts_get_missing_name) {
    XmcpServer server = test_server();
    XrJsonValue *params = xjson_new_object();

    XrJsonValue *result = call_prompts_get(&server, params);
    ASSERT(result == NULL);
    ASSERT_EQ(g_test_rpc_err.code, XMCP_ERR_INVALID_PARAMS);
    ASSERT(strstr(g_test_rpc_err.message, "'name' is required") != NULL);

    xjson_free(params);
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

    int match_count = 0;
    char *result = xmcp_knowledge_search_stdlib(kb, "http", NULL, &match_count);
    ASSERT_NOT_NULL(result);
    ASSERT(match_count > 0);
    ASSERT(strstr(result, "http") != NULL);
    xr_free(result);

    xmcp_knowledge_free(kb);
}

TEST(knowledge_search_stdlib_ranks_symbols) {
    XmcpKnowledge *kb = xmcp_knowledge_new();
    ASSERT_NOT_NULL(kb);
    xmcp_knowledge_load(kb);

    XmcpStdlibSearchResult result;
    xmcp_knowledge_search_stdlib_matches(kb, "parse", "csv", &result);
    ASSERT(result.match_count > 0);
    ASSERT_NOT_NULL(result.matches[0].module);
    ASSERT_STR_EQ(result.matches[0].module->name, "csv");
    ASSERT_NOT_NULL(result.matches[0].symbol);
    ASSERT_STR_EQ(result.matches[0].symbol->name, "parse");
    ASSERT(result.matches[0].score > 0);

    xmcp_knowledge_free(kb);
}

TEST(knowledge_search_stdlib_no_match) {
    XmcpKnowledge *kb = xmcp_knowledge_new();
    ASSERT_NOT_NULL(kb);
    xmcp_knowledge_load(kb);

    int match_count = 0;
    char *result = xmcp_knowledge_search_stdlib(kb, "zzz_nonexistent", NULL, &match_count);
    ASSERT_NOT_NULL(result);
    ASSERT_EQ(match_count, 0);
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
        .method = "test/method", .handler = NULL, .is_notification = false, .needs_init = true};
    ASSERT_STR_EQ(entry.method, "test/method");
    ASSERT(entry.handler == NULL);
    ASSERT(entry.is_notification == false);
    ASSERT(entry.needs_init == true);
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
    xr_test_suppress_dialogs();
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
    RUN_TEST(initialize_does_not_rewind_ready_state);

    /* JSON-RPC validation */
    RUN_TEST(jsonrpc_valid_request);
    RUN_TEST(jsonrpc_valid_notification);
    RUN_TEST(jsonrpc_rejects_missing_version);
    RUN_TEST(jsonrpc_rejects_invalid_id);
    RUN_TEST(jsonrpc_rejects_missing_method);
    RUN_TEST(jsonrpc_rejects_scalar_params);
    RUN_TEST(jsonrpc_rejects_null_params);

    /* Initialize result shape */
    RUN_TEST(initialize_returns_server_info);
    RUN_TEST(initialize_capabilities_with_all_features);
    RUN_TEST(initialize_capabilities_without_prompts);
    RUN_TEST(initialize_capabilities_minimal);
    RUN_TEST(registry_init_counts_features);
    RUN_TEST(registry_finds_tools_by_name);
    RUN_TEST(registry_indexes_resources_and_prompts);

    /* Tools */
    RUN_TEST(tools_list_returns_default_tools);
    RUN_TEST(tools_list_has_required_fields);
    RUN_TEST(tools_list_has_annotations);
    RUN_TEST(tools_list_tool_names);
    RUN_TEST(tools_list_default_tools_have_output_schema);
    RUN_TEST(tools_call_unknown_tool);
    RUN_TEST(tools_call_missing_name);
    RUN_TEST(tools_call_rejects_non_object_arguments);
    RUN_TEST(tools_call_validates_required_arguments);
    RUN_TEST(tools_call_validates_argument_types);

    /* Format tool */
    RUN_TEST(tools_call_format_missing_code);
    RUN_TEST(tools_call_format_schema_has_optional_params);
    RUN_TEST(tools_call_format_returns_structured_content);

    /* Diagnostics tool */
    RUN_TEST(tools_call_analyze_missing_code);
    RUN_TEST(tools_call_analyze_schema);
    RUN_TEST(tools_call_analyze_returns_structured_diagnostics);

    /* Run tool */
    RUN_TEST(tools_call_run_disabled_by_default);
    RUN_TEST(tools_list_runner_enabled_includes_run);
    RUN_TEST(tools_call_run_basic_print_returns_structured);
    RUN_TEST(tools_call_run_output_truncated);
    RUN_TEST(tools_call_run_deadline_exceeded);
    RUN_TEST(tools_call_run_blocks_dangerous_import);
    RUN_TEST(tools_call_run_missing_code_is_error_result);

    /* Knowledge tools */
    RUN_TEST(tools_call_syntax_lookup_returns_structured_content);
    RUN_TEST(tools_call_stdlib_search_returns_structured_content);
    RUN_TEST(tools_call_definition_returns_structured_content);
    RUN_TEST(tools_call_definition_not_found_is_structured);

    /* Definition tool */
    RUN_TEST(tools_call_definition_missing_symbol);
    RUN_TEST(tools_call_definition_schema);

    /* Pagination */
    RUN_TEST(tools_list_pagination_no_cursor);

    /* Resources */
    RUN_TEST(resources_list_returns_three);
    RUN_TEST(resources_list_has_required_fields);
    RUN_TEST(resources_list_uris);
    RUN_TEST(resources_read_cheatsheet);
    RUN_TEST(resources_read_unknown_uri);
    RUN_TEST(resources_read_missing_uri);

    /* Resource templates */
    RUN_TEST(resource_templates_list_returns_two);
    RUN_TEST(resource_templates_have_required_fields);
    RUN_TEST(resource_templates_uris);
    RUN_TEST(resources_read_topic_template);
    RUN_TEST(resources_read_stdlib_template);

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
    RUN_TEST(knowledge_search_stdlib_ranks_symbols);
    RUN_TEST(knowledge_search_stdlib_no_match);
    RUN_TEST(knowledge_get_cheatsheet);
    RUN_TEST(knowledge_get_concurrency);
    RUN_TEST(knowledge_get_stdlib_list);

    /* Dispatch table */
    RUN_TEST(method_entry_struct_size);

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
