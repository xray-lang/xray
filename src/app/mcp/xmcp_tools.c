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
#include "xmcp_server.h"
#include "xmcp_protocol.h"
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
#define RUN_OUTPUT_MAX 8192

/* --------------------------------------------------------------------------
 * Tool registration table type
 * -------------------------------------------------------------------------- */

/* Forward declarations for handlers and schema builders */
static XrJsonValue *tool_xray_analyze(XmcpServer *s, XrJsonValue *a);
static XrJsonValue *tool_xray_format(XmcpServer *s, XrJsonValue *a);
static XrJsonValue *tool_xray_run(XmcpServer *s, XrJsonValue *a);
static XrJsonValue *tool_xray_syntax_lookup(XmcpServer *s, XrJsonValue *a);
static XrJsonValue *tool_xray_stdlib_search(XmcpServer *s, XrJsonValue *a);
static XrJsonValue *tool_xray_definition(XmcpServer *s, XrJsonValue *a);

static XrJsonValue *schema_analyze(void);
static XrJsonValue *schema_analyze_output(void);
static XrJsonValue *schema_format(void);
static XrJsonValue *schema_run(void);
static XrJsonValue *schema_syntax(void);
static XrJsonValue *schema_stdlib(void);
static XrJsonValue *schema_definition(void);

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
     XMCP_TOOLSET_CORE, schema_format, NULL, tool_xray_format, true, false},
    {"xray_run", "Xray Code Runner",
     "Execute a small Xray code snippet and return its stdout output. "
     "Creates an isolated VM per execution. Max output: 8KB.",
     XMCP_TOOLSET_RUNNER, schema_run, NULL, tool_xray_run, false, true},
    {"xray_syntax_lookup", "Xray Syntax Reference",
     "Look up Xray language syntax by topic. Returns code examples. "
     "Topics: variables, types, functions, control_flow, class, struct, "
     "interface, enum, generics, collections, string, channel, coroutine, "
     "concurrency_rules, modules, testing, operators, builtin_functions.",
     XMCP_TOOLSET_KNOWLEDGE, schema_syntax, NULL, tool_xray_syntax_lookup, true, false},
    {"xray_stdlib_search", "Xray Stdlib Search",
     "Search the Xray standard library by module name or topic. "
     "Available modules: http, json, time, math, io, os, net, ws, "
     "crypto, csv, regex, cluster, compress, and more.",
     XMCP_TOOLSET_KNOWLEDGE, schema_stdlib, NULL, tool_xray_stdlib_search, true, false},
    {"xray_definition", "Xray Definition Lookup",
     "Find documentation for a symbol in the Xray language or stdlib. "
     "Searches syntax topics and standard library modules.",
     XMCP_TOOLSET_KNOWLEDGE, schema_definition, NULL, tool_xray_definition, true, false},
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

static XrJsonValue *schema_run(void) {
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "code", "string", "Xray code to execute");
    xjson_object_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xjson_array_push(r, xjson_new_string("code"));
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

XrJsonValue *xmcp_handle_tools_list(XmcpServer *server, XrJsonValue *params) {
    XR_DCHECK(server != NULL, "xmcp_handle_tools_list: NULL server");
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

static XrJsonValue *tool_xray_analyze(XmcpServer *server, XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "tool_xray_analyze: NULL server");
    XR_DCHECK(arguments != NULL, "tool_xray_analyze: NULL arguments");

    const char *code = xjson_get_string(arguments, "code");
    if (!code || code[0] == '\0') {
        return xmcp_make_error_result("Error: 'code' parameter is required");
    }
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

    int64_t ptok = server->current_progress_token;
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

static XrJsonValue *tool_xray_format(XmcpServer *server, XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "tool_xray_format: NULL server");
    XR_DCHECK(arguments != NULL, "tool_xray_format: NULL arguments");

    const char *code = xjson_get_string(arguments, "code");
    if (!code || code[0] == '\0') {
        return xmcp_make_error_result("Error: 'code' parameter is required");
    }

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

    XrJsonValue *result = xmcp_make_text_result(formatted, false);
    xr_free(formatted);
    return result;
}

/* --------------------------------------------------------------------------
 * Tool: xray_run (execute snippet, capture stdout)
 * -------------------------------------------------------------------------- */

static XrJsonValue *tool_xray_run(XmcpServer *server, XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "tool_xray_run: NULL server");
    XR_DCHECK(arguments != NULL, "tool_xray_run: NULL arguments");

    const char *code = xjson_get_string(arguments, "code");
    if (!code || code[0] == '\0') {
        return xmcp_make_error_result("Error: 'code' parameter is required");
    }

    /* Redirect stdout to a temp file to capture print() output */
    int saved_stdout = dup(xr_stdout_fd());
    if (saved_stdout < 0) {
        return xmcp_make_error_result("Error: failed to save stdout");
    }

    FILE *tmp = tmpfile();
    if (!tmp) {
        close(saved_stdout);
        return xmcp_make_error_result("Error: failed to create capture buffer");
    }
    fflush(stdout);
    dup2(fileno(tmp), xr_stdout_fd());

    /* Create a full isolate for execution */
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    XrayIsolate *iso = xray_isolate_new(&params);

    int exec_result = -1;
    if (iso) {
        exec_result = xray_isolate_dostring(iso, code);
        xray_isolate_delete(iso);
    }

    /* Restore stdout and read captured output */
    fflush(stdout);
    dup2(saved_stdout, xr_stdout_fd());
    close(saved_stdout);

    /* Read captured output */
    long out_size = ftell(tmp);
    if (out_size < 0)
        out_size = 0;
    if (out_size > RUN_OUTPUT_MAX)
        out_size = RUN_OUTPUT_MAX;

    char *output = xr_malloc((size_t) out_size + 256);
    if (!output) {
        fclose(tmp);
        return xmcp_make_error_result("Error: out of memory");
    }

    int total = 0;
    if (out_size > 0) {
        fseek(tmp, 0, SEEK_SET);
        size_t nread = fread(output, 1, (size_t) out_size, tmp);
        output[nread] = '\0';
        total = (int) nread;
    }
    fclose(tmp);

    /* Append execution status */
    if (exec_result != 0) {
        total +=
            snprintf(output + total, 256, "%s[exit code: %d]", total > 0 ? "\n" : "", exec_result);
    } else if (total == 0) {
        total = snprintf(output, 256, "(no output)");
    }

    XrJsonValue *result = xmcp_make_text_result(output, exec_result != 0);
    xr_free(output);
    return result;
}

/* --------------------------------------------------------------------------
 * Tool: xray_syntax_lookup
 * -------------------------------------------------------------------------- */

static XrJsonValue *tool_xray_syntax_lookup(XmcpServer *server, XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "tool_xray_syntax_lookup: NULL server");
    XR_DCHECK(arguments != NULL, "tool_xray_syntax_lookup: NULL arguments");

    const char *topic = xjson_get_string(arguments, "topic");
    if (!topic || topic[0] == '\0') {
        return xmcp_make_error_result("Error: 'topic' parameter is required");
    }

    const char *content = xmcp_knowledge_lookup_topic(server->knowledge, topic);
    if (content)
        return xmcp_make_text_result(content, false);

    char msg[512];
    snprintf(msg, sizeof(msg),
             "No syntax documentation found for topic \"%s\".\n\n"
             "Available topics: variables, types, functions, control_flow, "
             "class, struct, interface, enum, generics, collections, string, "
             "channel, coroutine, concurrency_rules, modules, testing, "
             "operators, builtin_functions.",
             topic);
    return xmcp_make_text_result(msg, false);
}

/* --------------------------------------------------------------------------
 * Tool: xray_stdlib_search
 * -------------------------------------------------------------------------- */

static XrJsonValue *tool_xray_stdlib_search(XmcpServer *server, XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "tool_xray_stdlib_search: NULL server");
    XR_DCHECK(arguments != NULL, "tool_xray_stdlib_search: NULL arguments");

    const char *query = xjson_get_string(arguments, "query");
    const char *module = xjson_get_string(arguments, "module");
    if (!query || query[0] == '\0') {
        return xmcp_make_error_result("Error: 'query' parameter is required");
    }

    char *text = xmcp_knowledge_search_stdlib(server->knowledge, query, module);
    XrJsonValue *result = xmcp_make_text_result(text ? text : "No results found", false);
    xr_free(text);
    return result;
}

/* --------------------------------------------------------------------------
 * Tool: xray_definition (symbol lookup in knowledge base)
 * -------------------------------------------------------------------------- */

static XrJsonValue *tool_xray_definition(XmcpServer *server, XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "tool_xray_definition: NULL server");
    XR_DCHECK(arguments != NULL, "tool_xray_definition: NULL arguments");

    const char *symbol = xjson_get_string(arguments, "symbol");
    if (!symbol || symbol[0] == '\0') {
        return xmcp_make_error_result("Error: 'symbol' parameter is required");
    }

    /* Try syntax topic first (e.g., "chan", "class", "enum") */
    const char *topic_content = xmcp_knowledge_lookup_topic(server->knowledge, symbol);
    if (topic_content)
        return xmcp_make_text_result(topic_content, false);

    /* Try stdlib search (e.g., "http.Server", "json.parse") */
    char *stdlib_text = xmcp_knowledge_search_stdlib(server->knowledge, symbol, NULL);
    if (stdlib_text) {
        XrJsonValue *result = xmcp_make_text_result(stdlib_text, false);
        xr_free(stdlib_text);
        return result;
    }

    /* Try splitting "module.symbol" */
    const char *dot = strchr(symbol, '.');
    if (dot && dot > symbol) {
        char mod[128];
        size_t mod_len = (size_t) (dot - symbol);
        if (mod_len < sizeof(mod)) {
            memcpy(mod, symbol, mod_len);
            mod[mod_len] = '\0';
            char *found = xmcp_knowledge_search_stdlib(server->knowledge, dot + 1, mod);
            if (found) {
                XrJsonValue *result = xmcp_make_text_result(found, false);
                xr_free(found);
                return result;
            }
        }
    }

    char msg[512];
    snprintf(msg, sizeof(msg),
             "No definition found for \"%s\".\n\n"
             "Try: language keywords (class, chan, enum), stdlib modules "
             "(http, json), or module.symbol format (http.Server).",
             symbol);
    return xmcp_make_text_result(msg, false);
}

/* --------------------------------------------------------------------------
 * tools/call dispatch (table-driven)
 * -------------------------------------------------------------------------- */

XrJsonValue *xmcp_handle_tools_call(XmcpServer *server, XrJsonValue *params) {
    XR_DCHECK(server != NULL, "xmcp_handle_tools_call: NULL server");

    const char *name = xjson_get_string(params, "name");
    if (!name)
        return xmcp_make_error_result("Error: tool 'name' is required");

    /* Extract progress token from _meta if present */
    server->current_progress_token = -1;
    XrJsonValue *meta = xjson_get_object(params, "_meta");
    if (meta) {
        server->current_progress_token = xjson_get_int_or(meta, "progressToken", -1);
    }

    XrJsonValue *arguments = xjson_get_object(params, "arguments");
    XrJsonValue *empty_args = NULL;
    if (!arguments) {
        empty_args = xjson_new_object();
        arguments = empty_args;
    }

    const XmcpToolDef *tool = xmcp_registry_find_tool(&server->registry, name);
    XrJsonValue *result = tool ? tool->handler(server, arguments) : NULL;

    if (!result) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Unknown tool: %s", name);
        result = xmcp_make_error_result(msg);
    }

    if (empty_args)
        xjson_free(empty_args);
    return result;
}
