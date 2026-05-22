/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_tools.c - MCP tool implementations (table-driven)
 *
 * KEY CONCEPT:
 *   Table-driven tool registry. Adding a tool requires:
 *   1. Write handler function   2. Write schema builder
 *   3. Add one entry to TOOL_TABLE[]
 *   tools/list and tools/call are fully data-driven.
 */

#include "xmcp_tools.h"
#include "xmcp_protocol.h"
#include "xmcp_server.h"
#include "xmcp_knowledge.h"
#include "../../base/xjson.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../frontend/parser/xparse.h"
#include "../../frontend/parser/xast.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../frontend/format/xfmt.h"
#include "xray_isolate.h"
#include "../../base/xarena.h"
#include "../../os/os_fd.h"
#include <stdio.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

/* Maximum errors captured per check/diagnostics */
#define MAX_CHECK_ERRORS 20
#define CHECK_BUF_SIZE 4096

/* xray_run runtime quotas. The runner toolset is opt-in; these values are
 * intentionally tight to keep the MCP response stream short and to prevent a
 * pathological snippet from wedging the (sequential) server.
 *
 *   RUN_OUTPUT_DEFAULT — initial cap on stdout returned to the caller.
 *   RUN_OUTPUT_HARD_MAX — absolute upper bound the caller can request.
 *   RUN_TIMEOUT_DEFAULT — default wall-clock budget (ms).
 *   RUN_TIMEOUT_HARD_MAX — absolute upper bound the caller can request. */
#define RUN_OUTPUT_DEFAULT 8192
#define RUN_OUTPUT_HARD_MAX 65536
#define RUN_TIMEOUT_DEFAULT_MS 2000
#define RUN_TIMEOUT_HARD_MAX_MS 30000

/* --------------------------------------------------------------------------
 * Tool registration table type
 * -------------------------------------------------------------------------- */

/* Forward declarations for handlers and schema builders */
static XrJsonValue *tool_xray_analyze(XmcpServer *s, const XmcpCallContext *ctx, XrJsonValue *a);
static XrJsonValue *tool_xray_format(XmcpServer *s, const XmcpCallContext *ctx, XrJsonValue *a);
static XrJsonValue *tool_xray_run(XmcpServer *s, const XmcpCallContext *ctx, XrJsonValue *a);
static XrJsonValue *tool_xray_syntax_lookup(XmcpServer *s, const XmcpCallContext *ctx,
                                            XrJsonValue *a);
static XrJsonValue *tool_xray_stdlib_search(XmcpServer *s, const XmcpCallContext *ctx,
                                            XrJsonValue *a);
static XrJsonValue *tool_xray_definition(XmcpServer *s, const XmcpCallContext *ctx, XrJsonValue *a);

static XrJsonValue *schema_analyze(void);
static XrJsonValue *schema_analyze_output(void);
static XrJsonValue *schema_format(void);
static XrJsonValue *schema_format_output(void);
static XrJsonValue *schema_run(void);
static XrJsonValue *schema_run_output(void);
static XrJsonValue *schema_syntax(void);
static XrJsonValue *schema_syntax_output(void);
static XrJsonValue *schema_stdlib(void);
static XrJsonValue *schema_stdlib_output(void);
static XrJsonValue *schema_definition(void);
static XrJsonValue *schema_definition_output(void);

/* --------------------------------------------------------------------------
 * Tool registration table
 * -------------------------------------------------------------------------- */

static const XmcpToolDef TOOL_TABLE[] = {
    {"xray_analyze", "Xray Code Analyzer",
     "Analyze Xray source code for syntax and semantic diagnostics. "
     "Returns a text summary and structured diagnostics.",
     XMCP_TOOLSET_CORE, schema_analyze, schema_analyze_output, tool_xray_analyze, true, false},
    {"xray_format", "Xray Code Formatter",
     "Format Xray source code according to standard style. "
     "Returns formatted code. Optionally set indent size or tabs.",
     XMCP_TOOLSET_CORE, schema_format, schema_format_output, tool_xray_format, true, false},
    {"xray_run", "Xray Code Runner",
     "Execute a short Xray snippet in an isolated VM and return structured "
     "results (ok, exitCode, stdout, timedOut, truncated, outputBytes). "
     "Bounded by a wall-clock timeout and output limit; dangerous stdlib "
     "modules (net, http, io, os, ...) are not importable.",
     XMCP_TOOLSET_RUNNER, schema_run, schema_run_output, tool_xray_run, false, true},
    {"xray_syntax_lookup", "Xray Syntax Reference",
     "Look up Xray language syntax by topic. Returns code examples. "
     "Topics: variables, types, functions, control_flow, class, struct, "
     "interface, enum, generics, collections, string, channel, coroutine, "
     "concurrency_rules, modules, testing, operators, builtin_functions, result.",
     XMCP_TOOLSET_KNOWLEDGE, schema_syntax, schema_syntax_output, tool_xray_syntax_lookup, true,
     false},
    {"xray_stdlib_search", "Xray Stdlib Search",
     "Search the Xray standard library by module name or topic. "
     "Available modules: http, json, time, math, io, os, net, ws, "
     "crypto, csv, regex, cluster, compress, and more.",
     XMCP_TOOLSET_KNOWLEDGE, schema_stdlib, schema_stdlib_output, tool_xray_stdlib_search, true,
     false},
    {"xray_definition", "Xray Definition Lookup",
     "Find documentation for a symbol in the Xray language or stdlib. "
     "Searches syntax topics and standard library modules.",
     XMCP_TOOLSET_KNOWLEDGE, schema_definition, schema_definition_output, tool_xray_definition,
     true, false},
    {NULL, NULL, NULL, XMCP_TOOLSET_CORE, NULL, NULL, NULL, false, false}};

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static XrJsonValue *xmcp_make_error_result(const char *message) {
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

static XrJsonValue *xmcp_make_text_result(const char *text, bool is_error) {
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

static XrJsonValue *xmcp_make_text_structured_result(const char *text, XrJsonValue *structured,
                                                     bool is_error) {
    XR_DCHECK(structured != NULL, "xmcp_make_text_structured_result: NULL structured");
    XrJsonValue *result = xmcp_make_text_result(text, is_error);
    xjson_object_set(result, "structuredContent", structured);
    return result;
}

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

static bool xmcp_value_matches_schema_type(XrJsonValue *value, const char *type) {
    XR_DCHECK(value != NULL, "xmcp_value_matches_schema_type: NULL value");
    XR_DCHECK(type != NULL, "xmcp_value_matches_schema_type: NULL type");
    if (strcmp(type, "string") == 0)
        return xjson_is_string(value);
    if (strcmp(type, "integer") == 0 || strcmp(type, "number") == 0)
        return xjson_is_number(value);
    if (strcmp(type, "boolean") == 0)
        return xjson_is_bool(value);
    if (strcmp(type, "array") == 0)
        return xjson_is_array(value);
    if (strcmp(type, "object") == 0)
        return xjson_is_object(value);
    return false;
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
            xjson_free(schema);
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
            if (!type || xmcp_value_matches_schema_type(value, type))
                continue;
            error->code = XMCP_ERR_INVALID_PARAMS;
            snprintf(error->message, sizeof(error->message),
                     "invalid type for parameter '%s' of tool '%s': expected %s, got %s",
                     member->key, tool->name, type, xmcp_json_type_name(value));
            xjson_free(schema);
            return false;
        }
    }

    xjson_free(schema);
    return true;
}

/* Build a simple schema: { type:"object", properties:{<name>:{type,desc},...}, required:[...] } */
static void schema_add_prop(XrJsonValue *props, const char *name, const char *type,
                            const char *desc) {
    XrJsonValue *p = xjson_new_object();
    XJSON_SET_STRING(p, "type", type);
    XJSON_SET_STRING(p, "description", desc);
    xjson_object_set(props, name, p);
}

typedef struct {
    int lines[MAX_CHECK_ERRORS];
    int columns[MAX_CHECK_ERRORS];
    int end_lines[MAX_CHECK_ERRORS];
    int end_columns[MAX_CHECK_ERRORS];
    char messages[MAX_CHECK_ERRORS][512];
    int count;
} ErrorCapture;

static void check_error_callback(void *user_data, int line, int column, int end_line,
                                 int end_column, const char *message) {
    ErrorCapture *cap = (ErrorCapture *) user_data;
    XR_DCHECK(cap != NULL, "check_error_callback: NULL capture");
    if (cap->count >= MAX_CHECK_ERRORS)
        return;
    int i = cap->count;
    cap->lines[i] = line;
    cap->columns[i] = column;
    cap->end_lines[i] = end_line;
    cap->end_columns[i] = end_column;
    snprintf(cap->messages[i], sizeof(cap->messages[0]), "%s", message);
    cap->count++;
}

static const char *diag_severity_name(XrDiagSeverity severity) {
    switch (severity) {
        case XR_DIAG_SEV_WARNING:
            return "warning";
        case XR_DIAG_SEV_INFO:
            return "info";
        case XR_DIAG_SEV_HINT:
            return "hint";
        case XR_DIAG_SEV_ERROR:
        default:
            return "error";
    }
}

static XrJsonValue *make_diagnostic(int line, int column, int end_line, int end_column,
                                    const char *severity, int code, const char *message,
                                    const char *source) {
    XR_DCHECK(severity != NULL, "make_diagnostic: NULL severity");
    XR_DCHECK(message != NULL, "make_diagnostic: NULL message");
    XR_DCHECK(source != NULL, "make_diagnostic: NULL source");

    XrJsonValue *diag = xjson_new_object();
    XJSON_SET_INT(diag, "line", line);
    XJSON_SET_INT(diag, "column", column);
    XJSON_SET_INT(diag, "endLine", end_line);
    XJSON_SET_INT(diag, "endColumn", end_column);
    XJSON_SET_STRING(diag, "severity", severity);
    XJSON_SET_INT(diag, "code", code);
    XJSON_SET_STRING(diag, "message", message);
    XJSON_SET_STRING(diag, "source", source);
    return diag;
}

static XrJsonValue *make_syntax_result_content(const char *topic, bool found, const char *content) {
    XR_DCHECK(topic != NULL, "make_syntax_result_content: NULL topic");
    XR_DCHECK(content != NULL, "make_syntax_result_content: NULL content");
    XrJsonValue *structured = xjson_new_object();
    XJSON_SET_STRING(structured, "topic", topic);
    XJSON_SET_BOOL(structured, "found", found);
    XJSON_SET_STRING(structured, "content", content);
    return structured;
}

static XrJsonValue *make_stdlib_match_json(const XmcpStdlibMatch *match) {
    XR_DCHECK(match != NULL, "make_stdlib_match_json: NULL match");
    XR_DCHECK(match->module != NULL, "make_stdlib_match_json: NULL module");
    XrJsonValue *item = xjson_new_object();
    XJSON_SET_STRING(item, "module", match->module->name);
    XJSON_SET_STRING(item, "summary", match->module->summary);
    XJSON_SET_INT(item, "score", match->score);
    if (match->symbol) {
        XJSON_SET_STRING(item, "symbol", match->symbol->name);
        XJSON_SET_STRING(item, "signature", match->symbol->signature);
        XJSON_SET_STRING(item, "symbolSummary", match->symbol->summary);
    }
    return item;
}

static XrJsonValue *make_stdlib_result_content(const char *query, const char *module,
                                               const XmcpStdlibSearchResult *search,
                                               const char *content) {
    XR_DCHECK(query != NULL, "make_stdlib_result_content: NULL query");
    XR_DCHECK(search != NULL, "make_stdlib_result_content: NULL search");
    XR_DCHECK(content != NULL, "make_stdlib_result_content: NULL content");
    XrJsonValue *structured = xjson_new_object();
    XJSON_SET_STRING(structured, "query", query);
    XJSON_SET_STRING(structured, "module", module ? module : "");
    XJSON_SET_INT(structured, "matchCount", search->match_count);
    XJSON_SET_BOOL(structured, "found", search->match_count > 0);
    XJSON_SET_STRING(structured, "content", content);
    XrJsonValue *matches = xjson_new_array();
    for (int i = 0; i < search->match_count; i++)
        xjson_array_push(matches, make_stdlib_match_json(&search->matches[i]));
    xjson_object_set(structured, "matches", matches);
    return structured;
}

static XrJsonValue *make_definition_result_content(const char *symbol, const char *kind, bool found,
                                                   const char *content) {
    XR_DCHECK(symbol != NULL, "make_definition_result_content: NULL symbol");
    XR_DCHECK(kind != NULL, "make_definition_result_content: NULL kind");
    XR_DCHECK(content != NULL, "make_definition_result_content: NULL content");
    XrJsonValue *structured = xjson_new_object();
    XJSON_SET_STRING(structured, "symbol", symbol);
    XJSON_SET_STRING(structured, "kind", kind);
    XJSON_SET_BOOL(structured, "found", found);
    XJSON_SET_STRING(structured, "content", content);
    return structured;
}

/* --------------------------------------------------------------------------
 * Schema builders
 * -------------------------------------------------------------------------- */

static XrJsonValue *schema_analyze(void) {
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "code", "string", "Xray source code to analyze");
    schema_add_prop(p, "filename", "string", "Optional filename used in diagnostics");
    schema_add_prop(p, "mode", "string", "Analysis mode: syntax, semantic, or full");
    xjson_object_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xjson_array_push(r, xjson_new_string("code"));
    xjson_object_set(s, "required", r);
    return s;
}

static XrJsonValue *schema_analyze_output(void) {
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "ok", "boolean", "True when no diagnostics were produced");
    schema_add_prop(p, "mode", "string", "Effective analysis mode");
    schema_add_prop(p, "diagnosticCount", "integer", "Number of diagnostics returned");
    schema_add_prop(p, "truncated", "boolean", "True when diagnostics hit the result limit");
    schema_add_prop(p, "diagnostics", "array", "Structured diagnostics");
    xjson_object_set(s, "properties", p);
    return s;
}

static XrJsonValue *schema_format(void) {
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "code", "string", "Xray source code to format");
    schema_add_prop(p, "indentSize", "integer", "Indent size in spaces (default: 4)");
    schema_add_prop(p, "useTabs", "boolean", "Use tabs instead of spaces (default: false)");
    xjson_object_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xjson_array_push(r, xjson_new_string("code"));
    xjson_object_set(s, "required", r);
    return s;
}

static XrJsonValue *schema_format_output(void) {
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "formattedCode", "string", "Formatted Xray source code");
    schema_add_prop(p, "changed", "boolean", "True when formatting changed the input");
    schema_add_prop(p, "indentSize", "integer", "Effective indent size");
    schema_add_prop(p, "useTabs", "boolean", "Effective tab indentation flag");
    xjson_object_set(s, "properties", p);
    return s;
}

static XrJsonValue *schema_run(void) {
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "code", "string", "Xray source code to execute (top-level statements).");
    schema_add_prop(p, "timeoutMs", "integer",
                    "Wall-clock time budget in milliseconds (default 2000, max 30000).");
    schema_add_prop(p, "outputLimit", "integer",
                    "Maximum captured stdout bytes (default 8192, max 65536).");
    xjson_object_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xjson_array_push(r, xjson_new_string("code"));
    xjson_object_set(s, "required", r);
    return s;
}

static XrJsonValue *schema_run_output(void) {
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "ok", "boolean", "True iff exitCode==0 and timedOut==false.");
    schema_add_prop(p, "exitCode", "integer",
                    "Return value from xray_isolate_dostring (0 on success).");
    schema_add_prop(p, "stdout", "string", "Captured print() output, truncated to outputLimit.");
    schema_add_prop(p, "timedOut", "boolean", "True if execution exceeded timeoutMs.");
    schema_add_prop(p, "truncated", "boolean", "True if stdout was clipped to outputLimit.");
    schema_add_prop(p, "outputBytes", "integer",
                    "Number of bytes actually returned in the stdout field.");
    xjson_object_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xjson_array_push(r, xjson_new_string("ok"));
    xjson_array_push(r, xjson_new_string("exitCode"));
    xjson_array_push(r, xjson_new_string("stdout"));
    xjson_array_push(r, xjson_new_string("timedOut"));
    xjson_array_push(r, xjson_new_string("truncated"));
    xjson_array_push(r, xjson_new_string("outputBytes"));
    xjson_object_set(s, "required", r);
    return s;
}

static XrJsonValue *schema_syntax(void) {
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "topic", "string",
                    "Syntax topic to look up (e.g., channel, coroutine, class, enum)");
    xjson_object_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xjson_array_push(r, xjson_new_string("topic"));
    xjson_object_set(s, "required", r);
    return s;
}

static XrJsonValue *schema_syntax_output(void) {
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "topic", "string", "Requested syntax topic");
    schema_add_prop(p, "found", "boolean", "True when the topic was found");
    schema_add_prop(p, "content", "string", "Syntax reference content");
    xjson_object_set(s, "properties", p);
    return s;
}

static XrJsonValue *schema_stdlib(void) {
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "query", "string", "Search query for standard library modules");
    schema_add_prop(p, "module", "string",
                    "Optional: filter by specific module name (e.g., http, json, time)");
    xjson_object_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xjson_array_push(r, xjson_new_string("query"));
    xjson_object_set(s, "required", r);
    return s;
}

static XrJsonValue *schema_stdlib_output(void) {
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "query", "string", "Requested stdlib search query");
    schema_add_prop(p, "module", "string", "Effective module filter");
    schema_add_prop(p, "matchCount", "integer", "Number of ranked matches");
    schema_add_prop(p, "found", "boolean", "True when at least one ranked match exists");
    schema_add_prop(p, "content", "string", "Formatted stdlib search result");
    schema_add_prop(p, "matches", "array", "Ranked matches with module, summary, and score");
    xjson_object_set(s, "properties", p);
    return s;
}

static XrJsonValue *schema_definition(void) {
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "symbol", "string",
                    "Symbol name to look up (e.g., 'http.Server', 'print', 'chan')");
    xjson_object_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xjson_array_push(r, xjson_new_string("symbol"));
    xjson_object_set(s, "required", r);
    return s;
}

static XrJsonValue *schema_definition_output(void) {
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "symbol", "string", "Requested symbol");
    schema_add_prop(p, "kind", "string", "Definition source: syntax, stdlib, or none");
    schema_add_prop(p, "found", "boolean", "True when a definition was found");
    schema_add_prop(p, "content", "string", "Definition content or not-found message");
    xjson_object_set(s, "properties", p);
    return s;
}

/* --------------------------------------------------------------------------
 * tools/list handler (table-driven)
 * -------------------------------------------------------------------------- */

/* Default page size for list endpoints (MCP convention: 1000) */
#define XMCP_PAGE_SIZE 1000

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
    xjson_object_set(t, "inputSchema", tool->build_schema());
    if (tool->build_output_schema)
        xjson_object_set(t, "outputSchema", tool->build_output_schema());

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
    (void) error;
    XrJsonValue *result = xjson_new_object();
    XrJsonValue *tools = xjson_new_array();

    /* Parse cursor: skip entries whose name <= cursor (alphabetical) */
    const char *cursor = params ? xjson_get_string(params, "cursor") : NULL;
    int count = 0;

    for (size_t i = 0; i < server->registry.tool_count; i++) {
        const XmcpToolDef *td = xmcp_registry_tool_at(&server->registry, i);
        /* Skip entries at or before cursor position */
        if (cursor && strcmp(td->name, cursor) <= 0)
            continue;

        xjson_array_push(tools, xmcp_tool_to_json(td));
        count++;
        if (count >= XMCP_PAGE_SIZE) {
            /* Set nextCursor to the last emitted tool name */
            XJSON_SET_STRING(result, "nextCursor", td->name);
            break;
        }
    }

    xjson_object_set(result, "tools", tools);
    return result;
}

static XrJsonValue *tool_xray_analyze(XmcpServer *server, const XmcpCallContext *ctx,
                                      XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "tool_xray_analyze: NULL server");
    XR_DCHECK(ctx != NULL, "tool_xray_analyze: NULL ctx");
    XR_DCHECK(arguments != NULL, "tool_xray_analyze: NULL arguments");

    const char *code = xjson_get_string(arguments, "code");
    if (code[0] == '\0')
        return xmcp_make_error_result("Error: 'code' must not be empty");
    if (!server->isolate)
        return xmcp_make_error_result("Error: analyzer isolate is not available");

    const char *filename = xjson_get_string(arguments, "filename");
    if (!filename || filename[0] == '\0')
        filename = "<mcp-analyze>";
    const char *mode = xjson_get_string(arguments, "mode");
    if (!mode || mode[0] == '\0')
        mode = "full";
    bool run_analyzer = strcmp(mode, "syntax") != 0;

    XrArena *arena = xr_malloc(sizeof(XrArena));
    if (!arena)
        return xmcp_make_error_result("Error: out of memory");
    xr_arena_init(arena, 0);

    int64_t ptok = ctx->progress_token;
    if (ptok >= 0)
        xmcp_send_progress_notification(server, ptok, 0, 2);

    ErrorCapture cap = {.count = 0};
    Parser parser;
    xr_parser_init(&parser, server->isolate, code, filename, arena);
    xr_parser_set_error_callback(&parser, check_error_callback, &cap, MAX_CHECK_ERRORS);
    AstNode *ast = xr_parse_recoverable(&parser);

    if (ptok >= 0)
        xmcp_send_progress_notification(server, ptok, 1, 2);

    XaAnalyzer *analyzer = NULL;
    XaDiagnostic *analyzer_diags = NULL;
    int analyzer_count = 0;
    if (ast && cap.count == 0 && run_analyzer) {
        analyzer = xa_analyzer_new(server->isolate);
        if (!analyzer) {
            xr_arena_destroy(arena);
            xr_free(arena);
            return xmcp_make_error_result("Error: out of memory");
        }
        if (strcmp(mode, "semantic") == 0 || strcmp(mode, "full") == 0)
            xa_analyzer_set_strict_mode(analyzer, true);
        xa_analyzer_analyze(analyzer, filename, (XrAstNode *) ast);
        analyzer_diags = xa_analyzer_get_diagnostics(analyzer, &analyzer_count);
    }

    XrJsonValue *diagnostics = xjson_new_array();
    int emitted = 0;
    bool truncated = false;
    for (int i = 0; i < cap.count && emitted < MAX_CHECK_ERRORS; i++, emitted++) {
        xjson_array_push(diagnostics, make_diagnostic(cap.lines[i], cap.columns[i],
                                                      cap.end_lines[i], cap.end_columns[i], "error",
                                                      0, cap.messages[i], "parser"));
    }
    for (XaDiagnostic *d = analyzer_diags; d && emitted < MAX_CHECK_ERRORS;
         d = d->next, emitted++) {
        xjson_array_push(diagnostics,
                         make_diagnostic((int) d->location.line, (int) d->location.column,
                                         (int) d->location.end_line, (int) d->location.end_column,
                                         diag_severity_name(d->severity), d->code, d->message,
                                         "analyzer"));
    }
    if (cap.count >= MAX_CHECK_ERRORS || analyzer_count > MAX_CHECK_ERRORS - cap.count)
        truncated = true;

    int diagnostic_count = xjson_array_len(diagnostics);
    char text[256];
    if (diagnostic_count == 0) {
        snprintf(text, sizeof(text), "OK: no diagnostics found.");
    } else {
        snprintf(text, sizeof(text), "Found %d diagnostic(s).", diagnostic_count);
    }

    XrJsonValue *structured = xjson_new_object();
    XJSON_SET_BOOL(structured, "ok", diagnostic_count == 0);
    XJSON_SET_STRING(structured, "mode", mode);
    XJSON_SET_INT(structured, "diagnosticCount", diagnostic_count);
    XJSON_SET_BOOL(structured, "truncated", truncated);
    xjson_object_set(structured, "diagnostics", diagnostics);

    XrJsonValue *result = xmcp_make_text_result(text, diagnostic_count > 0);
    xjson_object_set(result, "structuredContent", xjson_clone(structured));
    xjson_free(structured);
    if (ptok >= 0)
        xmcp_send_progress_notification(server, ptok, 2, 2);

    if (analyzer)
        xa_analyzer_free(analyzer);
    xr_arena_destroy(arena);
    xr_free(arena);
    return result;
}

static XrJsonValue *tool_xray_format(XmcpServer *server, const XmcpCallContext *ctx,
                                     XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "tool_xray_format: NULL server");
    XR_DCHECK(ctx != NULL, "tool_xray_format: NULL ctx");
    XR_DCHECK(arguments != NULL, "tool_xray_format: NULL arguments");
    (void) ctx;

    const char *code = xjson_get_string(arguments, "code");
    if (code[0] == '\0')
        return xmcp_make_error_result("Error: 'code' must not be empty");

    XrFmtConfig config = xfmt_default_config;
    int64_t indent = xjson_get_int_or(arguments, "indentSize", 0);
    if (indent > 0 && indent <= 16)
        config.indent_size = (int) indent;
    if (xjson_get_bool(arguments, "useTabs"))
        config.use_tabs = 1;

    AstNode *ast = xr_parse_with_trivia(server->isolate, code, "<mcp-format>");
    if (!ast) {
        return xmcp_make_error_result("Error: cannot format code with syntax errors. "
                                      "Use xray_analyze first to find and fix errors.");
    }

    char *formatted = xfmt_format_ast(ast, &config, server->isolate);
    xr_program_destroy(ast);
    if (!formatted)
        return xmcp_make_error_result("Error: formatting failed");

    XrJsonValue *structured = xjson_new_object();
    XJSON_SET_STRING(structured, "formattedCode", formatted);
    XJSON_SET_BOOL(structured, "changed", strcmp(code, formatted) != 0);
    XJSON_SET_INT(structured, "indentSize", config.indent_size);
    XJSON_SET_BOOL(structured, "useTabs", config.use_tabs != 0);

    XrJsonValue *result = xmcp_make_text_result(formatted, false);
    xjson_object_set(result, "structuredContent", structured);
    xr_free(formatted);
    return result;
}

/* --------------------------------------------------------------------------
 * Tool: xray_run (execute snippet, capture stdout)
 * -------------------------------------------------------------------------- */

/* Modules user snippets may import. Anything not on this list is rejected at
 * the module loader and surfaces inside the script as a runtime import
 * failure. The list is intentionally narrow: pure data / formatting / regex
 * modules only. Networking (`net`/`http`/`ws`), local I/O (`io`/`os`/`path`),
 * cluster orchestration, and dlopen-based packages are off the table because
 * the MCP runner runs in the same process as the server and cannot give them
 * up cleanly on timeout.
 *
 * `prelude` is included so that an explicit `import prelude` in user code
 * still succeeds; the isolate's own bootstrap import (issued by
 * isolate_init_full() before the allowlist is configured) is not subject
 * to filtering. */
static const char *const RUN_ALLOWED_MODULES[] = {
    "prelude", "math", "json", "string", "regex", "encoding", "base64", "url",        "datetime",
    "time",    "log",  "csv",  "toml",   "xml",   "yaml",     "types",  "test_yield",
};
#define RUN_ALLOWED_MODULES_COUNT (sizeof(RUN_ALLOWED_MODULES) / sizeof(RUN_ALLOWED_MODULES[0]))

/* Build the structured tool result described by schema_run_output(). The
 * structured object is also rendered as a human-readable summary for the
 * `content[].text` field so model UIs that don't yet consume structuredContent
 * still see something useful. */
static XrJsonValue *build_run_structured(bool ok, int exit_code, const char *stdout_text,
                                         int output_bytes, bool timed_out, bool truncated) {
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_BOOL(s, "ok", ok);
    XJSON_SET_INT(s, "exitCode", exit_code);
    xjson_object_set(s, "stdout", xjson_new_string(stdout_text ? stdout_text : ""));
    XJSON_SET_BOOL(s, "timedOut", timed_out);
    XJSON_SET_BOOL(s, "truncated", truncated);
    XJSON_SET_INT(s, "outputBytes", output_bytes);
    return s;
}

static XrJsonValue *tool_xray_run(XmcpServer *server, const XmcpCallContext *ctx,
                                  XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "tool_xray_run: NULL server");
    XR_DCHECK(ctx != NULL, "tool_xray_run: NULL ctx");
    XR_DCHECK(arguments != NULL, "tool_xray_run: NULL arguments");
    (void) server;
    (void) ctx;

    const char *code = xjson_get_string(arguments, "code");
    if (!code || code[0] == '\0') {
        XrJsonValue *structured = build_run_structured(false, -1, "", 0, false, false);
        return xmcp_make_text_structured_result("Error: 'code' must not be empty", structured,
                                                true);
    }

    /* Resolve quotas. Negative / zero / oversized values fall back to the
     * default rather than getting silently clamped to a confusing minimum. */
    int64_t timeout_ms = xjson_get_int_or(arguments, "timeoutMs", RUN_TIMEOUT_DEFAULT_MS);
    if (timeout_ms <= 0)
        timeout_ms = RUN_TIMEOUT_DEFAULT_MS;
    if (timeout_ms > RUN_TIMEOUT_HARD_MAX_MS)
        timeout_ms = RUN_TIMEOUT_HARD_MAX_MS;

    int64_t output_limit = xjson_get_int_or(arguments, "outputLimit", RUN_OUTPUT_DEFAULT);
    if (output_limit <= 0)
        output_limit = RUN_OUTPUT_DEFAULT;
    if (output_limit > RUN_OUTPUT_HARD_MAX)
        output_limit = RUN_OUTPUT_HARD_MAX;

    /* Per-call capture buffer; written to via xray_isolate_set_stdout() so we
     * never touch the process-wide fd 1 (which is the MCP transport). */
    FILE *capture = tmpfile();
    if (!capture) {
        XrJsonValue *structured = build_run_structured(false, -1, "", 0, false, false);
        return xmcp_make_text_structured_result("Error: failed to create capture buffer",
                                                structured, true);
    }

    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    XrayIsolate *iso = xray_isolate_new(&params);
    if (!iso) {
        fclose(capture);
        XrJsonValue *structured = build_run_structured(false, -1, "", 0, false, false);
        return xmcp_make_text_structured_result("Error: failed to create isolate", structured,
                                                true);
    }

    /* Sandbox configuration. Order matters: the allowlist applies to user
     * imports issued by xray_isolate_dostring(); the isolate's own prelude
     * bootstrap already ran during xray_isolate_new() and is not affected. */
    xray_isolate_set_stdout(iso, capture);
    xray_isolate_set_module_allowlist(iso, RUN_ALLOWED_MODULES, RUN_ALLOWED_MODULES_COUNT);
    xray_isolate_set_deadline_ms(iso, timeout_ms);

    /* No multicore runtime: xr_execute() falls back to the in-place
     * interpreter, which (since the VM back-edge fallback) keeps refilling
     * reductions instead of yielding to a non-existent scheduler. The
     * wall-clock deadline above remains the sole termination guarantee for
     * tight loops. Skipping the runtime saves the per-call thread spin-up. */
    int exec_result = xray_isolate_dostring(iso, code);
    bool timed_out = xray_isolate_timed_out(iso);
    fflush(capture);

    xray_isolate_delete(iso);

    /* Read captured output (clamped to output_limit). The buffer is sized one
     * byte larger so we can NUL-terminate; `output_bytes` reports the real
     * payload size before that terminator. */
    long out_size = ftell(capture);
    if (out_size < 0)
        out_size = 0;
    bool truncated = false;
    if ((int64_t) out_size > output_limit) {
        out_size = (long) output_limit;
        truncated = true;
    }

    char *output = xr_malloc((size_t) out_size + 1);
    if (!output) {
        fclose(capture);
        XrJsonValue *structured = build_run_structured(false, -1, "", 0, timed_out, truncated);
        return xmcp_make_text_structured_result("Error: out of memory", structured, true);
    }
    int output_bytes = 0;
    if (out_size > 0) {
        fseek(capture, 0, SEEK_SET);
        size_t nread = fread(output, 1, (size_t) out_size, capture);
        output[nread] = '\0';
        output_bytes = (int) nread;
    } else {
        output[0] = '\0';
    }
    fclose(capture);

    bool ok = (exec_result == 0) && !timed_out;
    XrJsonValue *structured =
        build_run_structured(ok, exec_result, output, output_bytes, timed_out, truncated);

    /* Compose a text summary so plain-text MCP clients see the outcome. */
    char summary[1024];
    if (timed_out) {
        snprintf(summary, sizeof(summary), "[timed out after %lldms] %s%s%d byte%s captured%s",
                 (long long) timeout_ms, output_bytes > 0 ? "\n" : "",
                 output_bytes > 0 ? output : "", output_bytes, output_bytes == 1 ? "" : "s",
                 truncated ? " (truncated)" : "");
    } else if (exec_result != 0) {
        snprintf(summary, sizeof(summary), "[exit code %d] %s%s%s", exec_result,
                 output_bytes > 0 ? "\n" : "", output_bytes > 0 ? output : "",
                 truncated ? "\n(truncated)" : "");
    } else if (output_bytes == 0) {
        snprintf(summary, sizeof(summary), "(no output)");
    } else {
        snprintf(summary, sizeof(summary), "%s%s", output, truncated ? "\n(truncated)" : "");
    }

    XrJsonValue *result = xmcp_make_text_structured_result(summary, structured, !ok);
    xr_free(output);
    return result;
}

/* --------------------------------------------------------------------------
 * Tool: xray_syntax_lookup
 * -------------------------------------------------------------------------- */

static XrJsonValue *tool_xray_syntax_lookup(XmcpServer *server, const XmcpCallContext *ctx,
                                            XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "tool_xray_syntax_lookup: NULL server");
    XR_DCHECK(ctx != NULL, "tool_xray_syntax_lookup: NULL ctx");
    XR_DCHECK(arguments != NULL, "tool_xray_syntax_lookup: NULL arguments");
    (void) ctx;

    const char *topic = xjson_get_string(arguments, "topic");
    if (topic[0] == '\0')
        return xmcp_make_error_result("Error: 'topic' must not be empty");
    if (!server->knowledge)
        return xmcp_make_error_result("Error: knowledge base is not available");

    const char *content = xmcp_knowledge_lookup_topic(server->knowledge, topic);
    if (content) {
        return xmcp_make_text_structured_result(
            content, make_syntax_result_content(topic, true, content), false);
    }

    char *available = xmcp_knowledge_list_topics(server->knowledge);
    char msg[1024];
    snprintf(msg, sizeof(msg),
             "No syntax documentation found for topic \"%s\".\n\n"
             "Available topics: %s.",
             topic, available ? available : "");
    if (available)
        xr_free(available);
    return xmcp_make_text_structured_result(msg, make_syntax_result_content(topic, false, msg),
                                            false);
}

/* --------------------------------------------------------------------------
 * Tool: xray_stdlib_search
 * -------------------------------------------------------------------------- */

static XrJsonValue *tool_xray_stdlib_search(XmcpServer *server, const XmcpCallContext *ctx,
                                            XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "tool_xray_stdlib_search: NULL server");
    XR_DCHECK(ctx != NULL, "tool_xray_stdlib_search: NULL ctx");
    XR_DCHECK(arguments != NULL, "tool_xray_stdlib_search: NULL arguments");
    (void) ctx;

    const char *query = xjson_get_string(arguments, "query");
    const char *module = xjson_get_string(arguments, "module");
    if (query[0] == '\0')
        return xmcp_make_error_result("Error: 'query' must not be empty");
    if (!server->knowledge)
        return xmcp_make_error_result("Error: knowledge base is not available");

    XmcpStdlibSearchResult search;
    xmcp_knowledge_search_stdlib_matches(server->knowledge, query, module, &search);
    char *text = xmcp_knowledge_search_stdlib(server->knowledge, query, module, NULL);
    if (!text)
        return xmcp_make_error_result("Error: stdlib search failed");
    XrJsonValue *result = xmcp_make_text_structured_result(
        text, make_stdlib_result_content(query, module, &search, text), false);
    xr_free(text);
    return result;
}

/* --------------------------------------------------------------------------
 * Tool: xray_definition (symbol lookup in knowledge base)
 * -------------------------------------------------------------------------- */

static XrJsonValue *tool_xray_definition(XmcpServer *server, const XmcpCallContext *ctx,
                                         XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "tool_xray_definition: NULL server");
    XR_DCHECK(ctx != NULL, "tool_xray_definition: NULL ctx");
    XR_DCHECK(arguments != NULL, "tool_xray_definition: NULL arguments");
    (void) ctx;

    const char *symbol = xjson_get_string(arguments, "symbol");
    if (symbol[0] == '\0')
        return xmcp_make_error_result("Error: 'symbol' must not be empty");
    if (!server->knowledge)
        return xmcp_make_error_result("Error: knowledge base is not available");

    /* Try syntax topic first (e.g., "chan", "class", "enum") */
    const char *topic_content = xmcp_knowledge_lookup_topic(server->knowledge, symbol);
    if (topic_content) {
        return xmcp_make_text_structured_result(
            topic_content, make_definition_result_content(symbol, "syntax", true, topic_content),
            false);
    }

    /* Try stdlib search (e.g., "http.Server", "json.parse") */
    int match_count = 0;
    char *stdlib_text = xmcp_knowledge_search_stdlib(server->knowledge, symbol, NULL, &match_count);
    if (stdlib_text) {
        if (match_count == 0) {
            xr_free(stdlib_text);
        } else {
            XrJsonValue *result = xmcp_make_text_structured_result(
                stdlib_text, make_definition_result_content(symbol, "stdlib", true, stdlib_text),
                false);
            xr_free(stdlib_text);
            return result;
        }
    } else {
        return xmcp_make_error_result("Error: stdlib search failed");
    }

    /* Try splitting "module.symbol" */
    const char *dot = strchr(symbol, '.');
    if (dot && dot > symbol) {
        char mod[128];
        size_t mod_len = (size_t) (dot - symbol);
        if (mod_len < sizeof(mod)) {
            memcpy(mod, symbol, mod_len);
            mod[mod_len] = '\0';
            int module_match_count = 0;
            char *found =
                xmcp_knowledge_search_stdlib(server->knowledge, dot + 1, mod, &module_match_count);
            if (found && module_match_count > 0) {
                XrJsonValue *result = xmcp_make_text_structured_result(
                    found, make_definition_result_content(symbol, "stdlib", true, found), false);
                xr_free(found);
                return result;
            }
            if (found) {
                xr_free(found);
            } else {
                return xmcp_make_error_result("Error: stdlib search failed");
            }
        }
    }

    char msg[512];
    snprintf(msg, sizeof(msg),
             "No definition found for \"%s\".\n\n"
             "Try: language keywords (class, chan, enum), stdlib modules "
             "(http, json), or module.symbol format (http.Server).",
             symbol);
    return xmcp_make_text_structured_result(
        msg, make_definition_result_content(symbol, "none", false, msg), false);
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

    /* Build per-call context. progress_token comes from params._meta if present. */
    XmcpCallContext ctx = {.progress_token = -1};
    XrJsonValue *meta = xjson_get_object(params, "_meta");
    if (meta)
        ctx.progress_token = xjson_get_int_or(meta, "progressToken", -1);

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

    if (empty_args)
        xjson_free(empty_args);
    return result;
}
