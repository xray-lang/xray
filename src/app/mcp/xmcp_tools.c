/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_tools.c - MCP tool implementations (Phase 1)
 *
 * KEY CONCEPT:
 *   Three tools for Phase 1:
 *   - xray_check: compile-check a code snippet
 *   - xray_syntax_lookup: look up Xray syntax by topic
 *   - xray_stdlib_search: search standard library modules
 */

#include "xmcp_tools.h"
#include "xmcp_server.h"
#include "xmcp_knowledge.h"
#include "../lsp/xlsp_json.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../frontend/parser/xparse.h"
#include "../../frontend/parser/xast.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "xray_isolate.h"
#include <stdio.h>
#include <string.h>

/* Maximum errors captured per check */
#define MAX_CHECK_ERRORS 20

/* --------------------------------------------------------------------------
 * Error capture for xray_check
 * -------------------------------------------------------------------------- */

typedef struct {
    char messages[MAX_CHECK_ERRORS][512];
    int count;
} ErrorCapture;

static void check_error_callback(void *user_data, int line, int column,
                                  int end_line, int end_column,
                                  const char *message) {
    (void)end_line;
    (void)end_column;
    ErrorCapture *cap = (ErrorCapture *)user_data;
    XR_DCHECK(cap != NULL, "check_error_callback: NULL capture");
    if (cap->count >= MAX_CHECK_ERRORS) return;
    snprintf(cap->messages[cap->count], sizeof(cap->messages[0]),
             "line %d:%d: %s", line, column, message);
    cap->count++;
}

/* --------------------------------------------------------------------------
 * Tool definitions (for tools/list)
 * -------------------------------------------------------------------------- */

/* Build the input schema JSON for xray_check. */
static XrJsonValue *build_check_schema(void) {
    XrJsonValue *schema = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(schema, "type", "object");

    XrJsonValue *props = xlsp_json_new_object();

    XrJsonValue *code_prop = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(code_prop, "type", "string");
    XLSP_JSON_SET_STRING(code_prop, "description",
        "Xray source code to check for syntax and type errors");
    xlsp_json_object_set(props, "code", code_prop);

    xlsp_json_object_set(schema, "properties", props);

    XrJsonValue *req = xlsp_json_new_array();
    xlsp_json_array_push(req, xlsp_json_new_string("code"));
    xlsp_json_object_set(schema, "required", req);

    return schema;
}

/* Build the input schema JSON for xray_syntax_lookup. */
static XrJsonValue *build_syntax_schema(void) {
    XrJsonValue *schema = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(schema, "type", "object");

    XrJsonValue *props = xlsp_json_new_object();

    XrJsonValue *topic_prop = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(topic_prop, "type", "string");
    XLSP_JSON_SET_STRING(topic_prop, "description",
        "Syntax topic to look up. Examples: channel, coroutine, "
        "match, generics, class, enum, collections, string, testing");
    xlsp_json_object_set(props, "topic", topic_prop);

    xlsp_json_object_set(schema, "properties", props);

    XrJsonValue *req = xlsp_json_new_array();
    xlsp_json_array_push(req, xlsp_json_new_string("topic"));
    xlsp_json_object_set(schema, "required", req);

    return schema;
}

/* Build the input schema JSON for xray_stdlib_search. */
static XrJsonValue *build_stdlib_schema(void) {
    XrJsonValue *schema = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(schema, "type", "object");

    XrJsonValue *props = xlsp_json_new_object();

    XrJsonValue *q_prop = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(q_prop, "type", "string");
    XLSP_JSON_SET_STRING(q_prop, "description",
        "Search query for standard library modules");
    xlsp_json_object_set(props, "query", q_prop);

    XrJsonValue *m_prop = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(m_prop, "type", "string");
    XLSP_JSON_SET_STRING(m_prop, "description",
        "Optional: filter by specific module name (e.g., http, json, time)");
    xlsp_json_object_set(props, "module", m_prop);

    xlsp_json_object_set(schema, "properties", props);

    XrJsonValue *req = xlsp_json_new_array();
    xlsp_json_array_push(req, xlsp_json_new_string("query"));
    xlsp_json_object_set(schema, "required", req);

    return schema;
}

/* --------------------------------------------------------------------------
 * tools/list handler
 * -------------------------------------------------------------------------- */

XrJsonValue *xmcp_handle_tools_list(void) {
    XrJsonValue *result = xlsp_json_new_object();
    XrJsonValue *tools = xlsp_json_new_array();

    /* Tool 1: xray_check */
    XrJsonValue *t1 = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(t1, "name", "xray_check");
    XLSP_JSON_SET_STRING(t1, "description",
        "Check Xray source code for syntax and type errors. "
        "Returns a list of diagnostics (errors, warnings). "
        "Use this before suggesting code to the user.");
    xlsp_json_object_set(t1, "inputSchema", build_check_schema());
    xlsp_json_array_push(tools, t1);

    /* Tool 2: xray_syntax_lookup */
    XrJsonValue *t2 = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(t2, "name", "xray_syntax_lookup");
    XLSP_JSON_SET_STRING(t2, "description",
        "Look up Xray language syntax by topic. "
        "Returns code examples and usage patterns. "
        "Topics: variables, types, functions, control_flow, class, "
        "struct, interface, enum, generics, collections, string, "
        "channel, coroutine, concurrency_rules, modules, testing, "
        "operators, builtin_functions.");
    xlsp_json_object_set(t2, "inputSchema", build_syntax_schema());
    xlsp_json_array_push(tools, t2);

    /* Tool 3: xray_stdlib_search */
    XrJsonValue *t3 = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(t3, "name", "xray_stdlib_search");
    XLSP_JSON_SET_STRING(t3, "description",
        "Search the Xray standard library by module name or topic. "
        "Returns available modules and their descriptions. "
        "Available modules: http, json, time, math, io, os, net, "
        "ws, crypto, csv, regex, cluster, compress, and more.");
    xlsp_json_object_set(t3, "inputSchema", build_stdlib_schema());
    xlsp_json_array_push(tools, t3);

    xlsp_json_object_set(result, "tools", tools);
    return result;
}

/* --------------------------------------------------------------------------
 * Tool: xray_check
 * -------------------------------------------------------------------------- */

static XrJsonValue *tool_xray_check(XmcpServer *server, XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "tool_xray_check: NULL server");

    const char *code = xlsp_json_get_string(arguments, "code");
    if (!code || code[0] == '\0') {
        XrJsonValue *r = xlsp_json_new_object();
        XrJsonValue *c = xlsp_json_new_array();
        XrJsonValue *item = xlsp_json_new_object();
        XLSP_JSON_SET_STRING(item, "type", "text");
        XLSP_JSON_SET_STRING(item, "text", "Error: 'code' parameter is required");
        xlsp_json_array_push(c, item);
        xlsp_json_object_set(r, "content", c);
        XLSP_JSON_SET_BOOL(r, "isError", true);
        return r;
    }

    /* Parse with error callback */
    ErrorCapture cap = {.count = 0};
    Parser parser;
    xr_parser_init(&parser, server->isolate, code, "<mcp-check>", NULL);
    xr_parser_set_error_callback(&parser, check_error_callback, &cap, MAX_CHECK_ERRORS);

    AstNode *ast = xr_parse_recoverable(&parser);

    /* Build result text */
    char text_buf[8192];
    int text_len = 0;

    if (cap.count == 0) {
        text_len = snprintf(text_buf, sizeof(text_buf),
                            "OK: no errors found.\n");
    } else {
        text_len = snprintf(text_buf, sizeof(text_buf),
                            "Found %d error(s):\n\n", cap.count);
        for (int i = 0; i < cap.count; i++) {
            text_len += snprintf(text_buf + text_len,
                                 sizeof(text_buf) - (size_t)text_len,
                                 "- %s\n", cap.messages[i]);
        }
    }

    if (ast) xr_program_destroy(ast);

    /* Build MCP content response */
    XrJsonValue *result = xlsp_json_new_object();
    XrJsonValue *content = xlsp_json_new_array();
    XrJsonValue *item = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(item, "type", "text");
    xlsp_json_object_set(item, "text", xlsp_json_new_string(text_buf));
    xlsp_json_array_push(content, item);
    xlsp_json_object_set(result, "content", content);

    if (cap.count > 0) {
        XLSP_JSON_SET_BOOL(result, "isError", true);
    }

    return result;
}

/* --------------------------------------------------------------------------
 * Tool: xray_syntax_lookup
 * -------------------------------------------------------------------------- */

static XrJsonValue *tool_xray_syntax_lookup(XmcpServer *server, XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "tool_xray_syntax_lookup: NULL server");

    const char *topic = xlsp_json_get_string(arguments, "topic");
    if (!topic || topic[0] == '\0') {
        XrJsonValue *r = xlsp_json_new_object();
        XrJsonValue *c = xlsp_json_new_array();
        XrJsonValue *item = xlsp_json_new_object();
        XLSP_JSON_SET_STRING(item, "type", "text");
        XLSP_JSON_SET_STRING(item, "text", "Error: 'topic' parameter is required");
        xlsp_json_array_push(c, item);
        xlsp_json_object_set(r, "content", c);
        XLSP_JSON_SET_BOOL(r, "isError", true);
        return r;
    }

    const char *content = xmcp_knowledge_lookup_topic(server->knowledge, topic);

    XrJsonValue *result = xlsp_json_new_object();
    XrJsonValue *arr = xlsp_json_new_array();
    XrJsonValue *item = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(item, "type", "text");

    if (content) {
        xlsp_json_object_set(item, "text", xlsp_json_new_string(content));
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "No syntax documentation found for topic \"%s\".\n\n"
                 "Available topics: variables, types, functions, control_flow, "
                 "class, struct, interface, enum, generics, collections, string, "
                 "channel, coroutine, concurrency_rules, modules, testing, "
                 "operators, builtin_functions.", topic);
        xlsp_json_object_set(item, "text", xlsp_json_new_string(msg));
    }

    xlsp_json_array_push(arr, item);
    xlsp_json_object_set(result, "content", arr);
    return result;
}

/* --------------------------------------------------------------------------
 * Tool: xray_stdlib_search
 * -------------------------------------------------------------------------- */

static XrJsonValue *tool_xray_stdlib_search(XmcpServer *server, XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "tool_xray_stdlib_search: NULL server");

    const char *query = xlsp_json_get_string(arguments, "query");
    const char *module = xlsp_json_get_string(arguments, "module");

    if (!query || query[0] == '\0') {
        XrJsonValue *r = xlsp_json_new_object();
        XrJsonValue *c = xlsp_json_new_array();
        XrJsonValue *item = xlsp_json_new_object();
        XLSP_JSON_SET_STRING(item, "type", "text");
        XLSP_JSON_SET_STRING(item, "text", "Error: 'query' parameter is required");
        xlsp_json_array_push(c, item);
        xlsp_json_object_set(r, "content", c);
        XLSP_JSON_SET_BOOL(r, "isError", true);
        return r;
    }

    char *text = xmcp_knowledge_search_stdlib(server->knowledge, query, module);

    XrJsonValue *result = xlsp_json_new_object();
    XrJsonValue *arr = xlsp_json_new_array();
    XrJsonValue *item = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(item, "type", "text");
    xlsp_json_object_set(item, "text",
        xlsp_json_new_string(text ? text : "No results found"));
    xlsp_json_array_push(arr, item);
    xlsp_json_object_set(result, "content", arr);

    xr_free(text);
    return result;
}

/* --------------------------------------------------------------------------
 * tools/call dispatch
 * -------------------------------------------------------------------------- */

XrJsonValue *xmcp_handle_tools_call(XmcpServer *server, XrJsonValue *params) {
    XR_DCHECK(server != NULL, "xmcp_handle_tools_call: NULL server");

    const char *name = xlsp_json_get_string(params, "name");
    XrJsonValue *arguments = xlsp_json_get_object(params, "arguments");

    if (!name) {
        XrJsonValue *r = xlsp_json_new_object();
        XrJsonValue *c = xlsp_json_new_array();
        XrJsonValue *item = xlsp_json_new_object();
        XLSP_JSON_SET_STRING(item, "type", "text");
        XLSP_JSON_SET_STRING(item, "text", "Error: tool 'name' is required");
        xlsp_json_array_push(c, item);
        xlsp_json_object_set(r, "content", c);
        XLSP_JSON_SET_BOOL(r, "isError", true);
        return r;
    }

    if (!arguments) {
        arguments = xlsp_json_new_object();
    }

    if (strcmp(name, "xray_check") == 0) {
        return tool_xray_check(server, arguments);
    }
    if (strcmp(name, "xray_syntax_lookup") == 0) {
        return tool_xray_syntax_lookup(server, arguments);
    }
    if (strcmp(name, "xray_stdlib_search") == 0) {
        return tool_xray_stdlib_search(server, arguments);
    }

    /* Unknown tool */
    XrJsonValue *r = xlsp_json_new_object();
    XrJsonValue *c = xlsp_json_new_array();
    XrJsonValue *item = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(item, "type", "text");
    char msg[256];
    snprintf(msg, sizeof(msg), "Unknown tool: %s", name);
    xlsp_json_object_set(item, "text", xlsp_json_new_string(msg));
    xlsp_json_array_push(c, item);
    xlsp_json_object_set(r, "content", c);
    XLSP_JSON_SET_BOOL(r, "isError", true);
    return r;
}
