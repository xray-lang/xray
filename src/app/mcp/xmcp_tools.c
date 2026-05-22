/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_tools.c - Tool registration table, argument validator, and the
 *                tools/list / tools/call dispatchers.
 *
 * KEY CONCEPT:
 *   Per-tool implementations live in sibling files (xmcp_tools_schema /
 *   _lang / _run / _kb).  This file owns the static TOOL_TABLE that wires
 *   them together, the JSON-Schema-driven argument validator used before
 *   every dispatch, and the JSON-RPC entry points consumed by the
 *   xmcp_server dispatcher.  Adding a tool requires:
 *     1. Implement handler + schema in the matching domain file.
 *     2. Declare them in xmcp_tools_internal.h.
 *     3. Add one row to TOOL_TABLE.
 */

#include "xmcp_tools.h"
#include "xmcp_tools_internal.h"
#include "xmcp_protocol.h"
#include "xmcp_server.h"
#include "../../base/xjson.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Tool registration table
 * -------------------------------------------------------------------------- */

static const XmcpToolDef TOOL_TABLE[] = {
    {"xray_analyze", "Xray Code Analyzer",
     "Analyze Xray source code for syntax and semantic diagnostics. "
     "Returns a text summary and structured diagnostics.",
     XMCP_TOOLSET_CORE, xmcp_schema_analyze, xmcp_schema_analyze_output, xmcp_tool_xray_analyze,
     true, false},
    {"xray_format", "Xray Code Formatter",
     "Format Xray source code according to standard style. "
     "Returns formatted code. Optionally set indent size or tabs.",
     XMCP_TOOLSET_CORE, xmcp_schema_format, xmcp_schema_format_output, xmcp_tool_xray_format, true,
     false},
    {"xray_run", "Xray Code Runner",
     "Execute a short Xray snippet in an isolated VM and return structured "
     "results (ok, exitCode, stdout, timedOut, truncated, outputBytes). "
     "Bounded by a wall-clock timeout and output limit; dangerous stdlib "
     "modules (net, http, io, os, ...) are not importable.",
     XMCP_TOOLSET_RUNNER, xmcp_schema_run, xmcp_schema_run_output, xmcp_tool_xray_run, false, true},
    {"xray_syntax_lookup", "Xray Syntax Reference",
     "Look up Xray language syntax by topic. Returns code examples. "
     "Topics: variables, types, functions, control_flow, class, struct, "
     "interface, enum, generics, collections, string, channel, coroutine, "
     "concurrency_rules, modules, testing, operators, builtin_functions, result.",
     XMCP_TOOLSET_KNOWLEDGE, xmcp_schema_syntax, xmcp_schema_syntax_output,
     xmcp_tool_xray_syntax_lookup, true, false},
    {"xray_stdlib_search", "Xray Stdlib Search",
     "Search the Xray standard library by module name or topic. "
     "Available modules: http, json, time, math, io, os, net, ws, "
     "crypto, csv, regex, cluster, compress, and more.",
     XMCP_TOOLSET_KNOWLEDGE, xmcp_schema_stdlib, xmcp_schema_stdlib_output,
     xmcp_tool_xray_stdlib_search, true, false},
    {"xray_definition", "Xray Definition Lookup",
     "Find documentation for a symbol in the Xray language or stdlib. "
     "Searches syntax topics and standard library modules.",
     XMCP_TOOLSET_KNOWLEDGE, xmcp_schema_definition, xmcp_schema_definition_output,
     xmcp_tool_xray_definition, true, false},
    {NULL, NULL, NULL, XMCP_TOOLSET_CORE, NULL, NULL, NULL, false, false}};

/* --------------------------------------------------------------------------
 * Result helpers (visible to the per-domain handler files)
 * -------------------------------------------------------------------------- */

XR_FUNC XrJsonValue *xmcp_make_error_result(const char *message) {
    XR_DCHECK(message != NULL, "xmcp_make_error_result: NULL message");
    XrJsonValue *r = xjson_new_object();
    XrJsonValue *c = xjson_new_array();
    XrJsonValue *item = xjson_new_object();
    XJSON_SET_STRING(item, "type", "text");
    XJSON_SET_STRING(item, "text", message);
    xjson_array_push(c, item);
    xjson_object_set(r, "content", c);
    XJSON_SET_BOOL(r, "isError", true);
    return r;
}

XR_FUNC XrJsonValue *xmcp_make_text_result(const char *text, bool is_error) {
    XR_DCHECK(text != NULL, "xmcp_make_text_result: NULL text");
    XrJsonValue *result = xjson_new_object();
    XrJsonValue *arr = xjson_new_array();
    XrJsonValue *item = xjson_new_object();
    XJSON_SET_STRING(item, "type", "text");
    xjson_object_set(item, "text", xjson_new_string(text));
    xjson_array_push(arr, item);
    xjson_object_set(result, "content", arr);
    if (is_error) {
        XJSON_SET_BOOL(result, "isError", true);
    }
    return result;
}

XR_FUNC XrJsonValue *xmcp_make_text_structured_result(const char *text, XrJsonValue *structured,
                                                      bool is_error) {
    XR_DCHECK(structured != NULL, "xmcp_make_text_structured_result: NULL structured");
    XrJsonValue *result = xmcp_make_text_result(text, is_error);
    xjson_object_set(result, "structuredContent", structured);
    return result;
}

/* --------------------------------------------------------------------------
 * Validator helpers (file-local; used only by xmcp_validate_tool_arguments)
 * -------------------------------------------------------------------------- */

static const char *xmcp_json_type_name(const XrJsonValue *value) {
    XR_DCHECK(value != NULL, "xmcp_json_type_name: NULL value");
    switch (value->type) {
        case XR_JSON_NULL:
            return "null";
        case XR_JSON_BOOL:
            return "boolean";
        case XR_JSON_NUMBER:
            return "number";
        case XR_JSON_STRING:
            return "string";
        case XR_JSON_ARRAY:
            return "array";
        case XR_JSON_OBJECT:
            return "object";
        default:
            return "unknown";
    }
}

static bool xmcp_number_is_integer_value(XrJsonValue *value) {
    XR_DCHECK(value != NULL, "xmcp_number_is_integer_value: NULL value");
    if (!xjson_is_number(value))
        return false;
    if (value->is_integer)
        return true;
    if (value->as.number < -9223372036854775808.0 || value->as.number > 9223372036854775807.0)
        return false;
    int64_t as_int = (int64_t) value->as.number;
    return (double) as_int == value->as.number;
}

static int64_t xmcp_number_to_int(XrJsonValue *value) {
    XR_DCHECK(value != NULL, "xmcp_number_to_int: NULL value");
    XR_DCHECK(xjson_is_number(value), "xmcp_number_to_int: value is not a number");
    return value->is_integer ? value->as.integer : (int64_t) value->as.number;
}

static bool xmcp_value_matches_schema_type(XrJsonValue *value, const char *type) {
    XR_DCHECK(value != NULL, "xmcp_value_matches_schema_type: NULL value");
    XR_DCHECK(type != NULL, "xmcp_value_matches_schema_type: NULL type");
    if (strcmp(type, "string") == 0)
        return xjson_is_string(value);
    if (strcmp(type, "integer") == 0)
        return xmcp_number_is_integer_value(value);
    if (strcmp(type, "number") == 0)
        return xjson_is_number(value);
    if (strcmp(type, "boolean") == 0)
        return xjson_is_bool(value);
    if (strcmp(type, "array") == 0)
        return xjson_is_array(value);
    if (strcmp(type, "object") == 0)
        return xjson_is_object(value);
    return false;
}

static bool xmcp_schema_enum_contains(XrJsonValue *allowed, XrJsonValue *value) {
    if (!allowed)
        return true;
    if (!xjson_is_string(value))
        return false;
    for (int i = 0; i < xjson_array_len(allowed); i++) {
        XrJsonValue *item = xjson_array_get(allowed, i);
        if (xjson_is_string(item) && strcmp(item->as.string, value->as.string) == 0)
            return true;
    }
    return false;
}

static void xmcp_format_string_enum(XrJsonValue *allowed, char *buf, size_t cap) {
    XR_DCHECK(buf != NULL, "xmcp_format_string_enum: NULL buf");
    if (cap == 0)
        return;
    size_t len = 0;
    buf[0] = '\0';
    for (int i = 0; allowed && i < xjson_array_len(allowed); i++) {
        XrJsonValue *item = xjson_array_get(allowed, i);
        if (!xjson_is_string(item))
            continue;
        if (len > 0 && len < cap)
            len += (size_t) snprintf(buf + len, cap - len, ", ");
        if (len < cap)
            len += (size_t) snprintf(buf + len, cap - len, "%s", item->as.string);
    }
}

/* Validate `arguments` against `tool->build_schema()`. Populates `error` with
 * `XMCP_ERR_INVALID_PARAMS` on failure. Returns true when arguments pass. */
static bool xmcp_validate_tool_arguments(const XmcpToolDef *tool, XrJsonValue *arguments,
                                         XmcpRpcError *error) {
    XR_DCHECK(tool != NULL, "xmcp_validate_tool_arguments: NULL tool");
    XR_DCHECK(arguments != NULL, "xmcp_validate_tool_arguments: NULL arguments");
    XR_DCHECK(error != NULL, "xmcp_validate_tool_arguments: NULL error");

    XrJsonValue *schema = tool->build_schema ? tool->build_schema() : NULL;
    if (!schema)
        return true;

    XrJsonValue *required = xjson_get_array(schema, "required");
    for (int i = 0; i < xjson_array_len(required); i++) {
        XrJsonValue *item = xjson_array_get(required, i);
        if (!xjson_is_string(item))
            continue;
        const char *name = item->as.string;
        XrJsonValue *value = xjson_get(arguments, name);
        if (!value || xjson_is_null(value)) {
            error->code = XMCP_ERR_INVALID_PARAMS;
            snprintf(error->message, sizeof(error->message),
                     "missing required parameter '%s' for tool '%s'", name, tool->name);
            return false;
        }
    }

    XrJsonValue *properties = xjson_get_object(schema, "properties");
    if (properties) {
        for (int i = 0; i < properties->as.object.count; i++) {
            XrJsonMember *member = &properties->as.object.members[i];
            XrJsonValue *value = xjson_get(arguments, member->key);
            if (!value || xjson_is_null(value))
                continue;
            const char *type = xjson_get_string(member->value, "type");
            if (type && !xmcp_value_matches_schema_type(value, type)) {
                error->code = XMCP_ERR_INVALID_PARAMS;
                snprintf(error->message, sizeof(error->message),
                         "invalid type for parameter '%s' of tool '%s': expected %s, got %s",
                         member->key, tool->name, type, xmcp_json_type_name(value));
                return false;
            }

            XrJsonValue *allowed = xjson_get_array(member->value, "enum");
            if (allowed && !xmcp_schema_enum_contains(allowed, value)) {
                char allowed_text[128];
                xmcp_format_string_enum(allowed, allowed_text, sizeof(allowed_text));
                error->code = XMCP_ERR_INVALID_PARAMS;
                snprintf(error->message, sizeof(error->message),
                         "invalid value for parameter '%s' of tool '%s': expected one of %s",
                         member->key, tool->name, allowed_text);
                return false;
            }

            XrJsonValue *minimum = xjson_get(member->value, "minimum");
            XrJsonValue *maximum = xjson_get(member->value, "maximum");
            if (xjson_is_number(value) && (xjson_is_number(minimum) || xjson_is_number(maximum))) {
                int64_t actual = xmcp_number_to_int(value);
                if (xjson_is_number(minimum) && actual < xmcp_number_to_int(minimum)) {
                    error->code = XMCP_ERR_INVALID_PARAMS;
                    snprintf(error->message, sizeof(error->message),
                             "invalid value for parameter '%s' of tool '%s': must be >= %lld",
                             member->key, tool->name, (long long) xmcp_number_to_int(minimum));
                    return false;
                }
                if (xjson_is_number(maximum) && actual > xmcp_number_to_int(maximum)) {
                    error->code = XMCP_ERR_INVALID_PARAMS;
                    snprintf(error->message, sizeof(error->message),
                             "invalid value for parameter '%s' of tool '%s': must be <= %lld",
                             member->key, tool->name, (long long) xmcp_number_to_int(maximum));
                    return false;
                }
            }
        }
    }

    return true;
}

/* --------------------------------------------------------------------------
 * tools/list handler (table-driven)
 * -------------------------------------------------------------------------- */

XR_FUNC size_t xmcp_tools_count(void) {
    size_t count = 0;
    while (TOOL_TABLE[count].name != NULL)
        count++;
    return count;
}

XR_FUNC const XmcpToolDef *xmcp_tools_table(void) {
    return TOOL_TABLE;
}

static XrJsonValue *xmcp_tool_to_json(const XmcpToolDef *tool) {
    XR_DCHECK(tool != NULL, "xmcp_tool_to_json: NULL tool");
    XrJsonValue *t = xjson_new_object();
    XJSON_SET_STRING(t, "name", tool->name);
    XJSON_SET_STRING(t, "description", tool->description);
    xjson_object_set(t, "inputSchema", xjson_clone(tool->build_schema()));
    if (tool->build_output_schema)
        xjson_object_set(t, "outputSchema", xjson_clone(tool->build_output_schema()));

    XrJsonValue *ann = xjson_new_object();
    XJSON_SET_STRING(ann, "title", tool->title);
    XJSON_SET_BOOL(ann, "readOnlyHint", tool->read_only);
    XJSON_SET_BOOL(ann, "destructiveHint", false);
    XJSON_SET_BOOL(ann, "openWorldHint", tool->open_world);
    xjson_object_set(t, "annotations", ann);
    return t;
}

XR_FUNC XrJsonValue *xmcp_handle_tools_list(XmcpServer *server, XrJsonValue *params,
                                            XmcpRpcError *error) {
    XR_DCHECK(server != NULL, "xmcp_handle_tools_list: NULL server");
    (void) params;
    (void) error;
    XrJsonValue *result = xjson_new_object();
    XrJsonValue *tools = xjson_new_array();

    for (size_t i = 0; i < server->registry.tool_count; i++) {
        const XmcpToolDef *td = xmcp_registry_tool_at(&server->registry, i);
        xjson_array_push(tools, xmcp_tool_to_json(td));
    }

    xjson_object_set(result, "tools", tools);
    return result;
}

/* --------------------------------------------------------------------------
 * tools/call dispatch (table-driven)
 * -------------------------------------------------------------------------- */

XR_FUNC XrJsonValue *xmcp_handle_tools_call(XmcpServer *server, XrJsonValue *params,
                                            XmcpRpcError *error) {
    XR_DCHECK(server != NULL, "xmcp_handle_tools_call: NULL server");
    XR_DCHECK(error != NULL, "xmcp_handle_tools_call: NULL error");

    if (!params || !xjson_is_object(params)) {
        error->code = XMCP_ERR_INVALID_PARAMS;
        snprintf(error->message, sizeof(error->message), "tools/call: params must be an object");
        return NULL;
    }

    const char *name = xjson_get_string(params, "name");
    if (!name || name[0] == '\0') {
        error->code = XMCP_ERR_INVALID_PARAMS;
        snprintf(error->message, sizeof(error->message), "tools/call: 'name' is required");
        return NULL;
    }

    const XmcpToolDef *tool = xmcp_registry_find_tool(&server->registry, name);
    if (!tool) {
        error->code = XMCP_ERR_INVALID_PARAMS;
        snprintf(error->message, sizeof(error->message), "tools/call: unknown tool '%s'", name);
        return NULL;
    }

    XrJsonValue *arguments_value = xjson_get(params, "arguments");
    if (arguments_value && !xjson_is_object(arguments_value)) {
        error->code = XMCP_ERR_INVALID_PARAMS;
        snprintf(error->message, sizeof(error->message),
                 "tools/call: 'arguments' must be an object");
        return NULL;
    }

    /* Build per-call context. progress_token (string|number) from _meta. */
    XmcpCallContext ctx = {.progress_token = NULL};
    XrJsonValue *meta = xjson_get_object(params, "_meta");
    if (meta) {
        XrJsonValue *tok = xjson_get(meta, "progressToken");
        if (tok)
            ctx.progress_token = xjson_clone(tok);
    }

    XrJsonValue *empty_args = NULL;
    XrJsonValue *arguments = arguments_value;
    if (!arguments) {
        empty_args = xjson_new_object();
        arguments = empty_args;
    }

    if (!xmcp_validate_tool_arguments(tool, arguments, error)) {
        if (empty_args)
            xjson_free(empty_args);
        return NULL;
    }

    XrJsonValue *result = tool->handler(server, &ctx, arguments);
    if (!result) {
        error->code = XMCP_ERR_INTERNAL;
        snprintf(error->message, sizeof(error->message), "tools/call: handler '%s' returned NULL",
                 name);
    }

    if (ctx.progress_token)
        xjson_free(ctx.progress_token);
    if (empty_args)
        xjson_free(empty_args);
    return result;
}
