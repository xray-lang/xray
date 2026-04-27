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
#include "../../frontend/format/xfmt.h"
#include "xray_isolate.h"
#include "../../base/xarena.h"
#include "../../base/xfd.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
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

typedef XrJsonValue *(*XmcpToolHandler)(XmcpServer *server, XrJsonValue *args);
typedef XrJsonValue *(*XmcpSchemaBuilder)(void);

typedef struct {
    const char *name;
    const char *title;
    const char *description;
    XmcpSchemaBuilder build_schema;
    XmcpToolHandler handler;
    bool read_only;
    bool open_world;
} XmcpToolDef;

/* Forward declarations for handlers and schema builders */
static XrJsonValue *tool_xray_check(XmcpServer *s, XrJsonValue *a);
static XrJsonValue *tool_xray_format(XmcpServer *s, XrJsonValue *a);
static XrJsonValue *tool_xray_diagnostics(XmcpServer *s, XrJsonValue *a);
static XrJsonValue *tool_xray_run(XmcpServer *s, XrJsonValue *a);
static XrJsonValue *tool_xray_syntax_lookup(XmcpServer *s, XrJsonValue *a);
static XrJsonValue *tool_xray_stdlib_search(XmcpServer *s, XrJsonValue *a);
static XrJsonValue *tool_xray_definition(XmcpServer *s, XrJsonValue *a);

static XrJsonValue *schema_check(void);
static XrJsonValue *schema_format(void);
static XrJsonValue *schema_diagnostics(void);
static XrJsonValue *schema_run(void);
static XrJsonValue *schema_syntax(void);
static XrJsonValue *schema_stdlib(void);
static XrJsonValue *schema_definition(void);

/* --------------------------------------------------------------------------
 * Tool registration table
 * -------------------------------------------------------------------------- */

static const XmcpToolDef TOOL_TABLE[] = {
    {"xray_check", "Xray Code Checker",
     "Check Xray source code for syntax and type errors. "
     "Returns a list of diagnostics. Use this before suggesting code.",
     schema_check, tool_xray_check, true, false},
    {"xray_format", "Xray Code Formatter",
     "Format Xray source code according to standard style. "
     "Returns formatted code. Optionally set indent size or tabs.",
     schema_format, tool_xray_format, true, false},
    {"xray_diagnostics", "Xray Diagnostics",
     "Get structured diagnostic information (line, column, severity, "
     "message) for Xray source code as a markdown table.",
     schema_diagnostics, tool_xray_diagnostics, true, false},
    {"xray_run", "Xray Code Runner",
     "Execute a small Xray code snippet and return its stdout output. "
     "Creates an isolated VM per execution. Max output: 8KB.",
     schema_run, tool_xray_run, false, true},
    {"xray_syntax_lookup", "Xray Syntax Reference",
     "Look up Xray language syntax by topic. Returns code examples. "
     "Topics: variables, types, functions, control_flow, class, struct, "
     "interface, enum, generics, collections, string, channel, coroutine, "
     "concurrency_rules, modules, testing, operators, builtin_functions.",
     schema_syntax, tool_xray_syntax_lookup, true, false},
    {"xray_stdlib_search", "Xray Stdlib Search",
     "Search the Xray standard library by module name or topic. "
     "Available modules: http, json, time, math, io, os, net, ws, "
     "crypto, csv, regex, cluster, compress, and more.",
     schema_stdlib, tool_xray_stdlib_search, true, false},
    {"xray_definition", "Xray Definition Lookup",
     "Find documentation for a symbol in the Xray language or stdlib. "
     "Searches syntax topics and standard library modules.",
     schema_definition, tool_xray_definition, true, false},
    {NULL, NULL, NULL, NULL, NULL, false, false}};

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

/* --------------------------------------------------------------------------
 * Error capture (shared by xray_check and xray_diagnostics)
 * -------------------------------------------------------------------------- */

typedef struct {
    int lines[MAX_CHECK_ERRORS];
    int columns[MAX_CHECK_ERRORS];
    char messages[MAX_CHECK_ERRORS][512];
    int count;
} ErrorCapture;

static void check_error_callback(void *user_data, int line, int column, int end_line,
                                 int end_column, const char *message) {
    (void) end_line;
    (void) end_column;
    ErrorCapture *cap = (ErrorCapture *) user_data;
    XR_DCHECK(cap != NULL, "check_error_callback: NULL capture");
    if (cap->count >= MAX_CHECK_ERRORS)
        return;
    int i = cap->count;
    cap->lines[i] = line;
    cap->columns[i] = column;
    snprintf(cap->messages[i], sizeof(cap->messages[0]), "%s", message);
    cap->count++;
}

/* --------------------------------------------------------------------------
 * Schema builders
 * -------------------------------------------------------------------------- */

static XrJsonValue *schema_check(void) {
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "code", "string", "Xray source code to check for syntax and type errors");
    xjson_object_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xjson_array_push(r, xjson_new_string("code"));
    xjson_object_set(s, "required", r);
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

static XrJsonValue *schema_diagnostics(void) {
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "code", "string", "Xray source code to analyze");
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
                    "Symbol name to look up (e.g., 'http.Server', 'println', 'chan')");
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

XrJsonValue *xmcp_handle_tools_list(XrJsonValue *params) {
    XrJsonValue *result = xjson_new_object();
    XrJsonValue *tools = xjson_new_array();

    /* Parse cursor: skip entries whose name <= cursor (alphabetical) */
    const char *cursor = params ? xjson_get_string(params, "cursor") : NULL;
    int count = 0;

    for (int i = 0; TOOL_TABLE[i].name != NULL; i++) {
        const XmcpToolDef *td = &TOOL_TABLE[i];

        /* Skip entries at or before cursor position */
        if (cursor && strcmp(td->name, cursor) <= 0)
            continue;

        XrJsonValue *t = xjson_new_object();
        XJSON_SET_STRING(t, "name", td->name);
        XJSON_SET_STRING(t, "description", td->description);
        xjson_object_set(t, "inputSchema", td->build_schema());

        XrJsonValue *ann = xjson_new_object();
        XJSON_SET_STRING(ann, "title", td->title);
        XJSON_SET_BOOL(ann, "readOnlyHint", td->read_only);
        XJSON_SET_BOOL(ann, "destructiveHint", false);
        XJSON_SET_BOOL(ann, "openWorldHint", td->open_world);
        xjson_object_set(t, "annotations", ann);

        xjson_array_push(tools, t);
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

/* --------------------------------------------------------------------------
 * Tool: xray_check
 * -------------------------------------------------------------------------- */

static XrJsonValue *tool_xray_check(XmcpServer *server, XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "tool_xray_check: NULL server");
    XR_DCHECK(arguments != NULL, "tool_xray_check: NULL arguments");

    const char *code = xjson_get_string(arguments, "code");
    if (!code || code[0] == '\0') {
        return xmcp_make_error_result("Error: 'code' parameter is required");
    }

    XrArena *arena = xr_malloc(sizeof(XrArena));
    if (!arena)
        return xmcp_make_error_result("Error: out of memory");
    xr_arena_init(arena, 0);

    int64_t ptok = server->current_progress_token;
    if (ptok >= 0)
        xmcp_send_progress_notification(server, ptok, 0, 2);

    ErrorCapture cap = {.count = 0};
    Parser parser;
    xr_parser_init(&parser, server->isolate, code, "<mcp-check>", arena);
    xr_parser_set_error_callback(&parser, check_error_callback, &cap, MAX_CHECK_ERRORS);
    AstNode *ast = xr_parse_recoverable(&parser);

    if (ptok >= 0)
        xmcp_send_progress_notification(server, ptok, 1, 2);

    size_t buf_cap = CHECK_BUF_SIZE;
    char *text_buf = xr_malloc(buf_cap);
    if (!text_buf) {
        if (ast)
            xr_program_destroy(ast);
        xr_arena_destroy(arena);
        xr_free(arena);
        return xmcp_make_error_result("Error: out of memory");
    }
    int text_len = 0;
    if (cap.count == 0) {
        text_len = snprintf(text_buf, buf_cap, "OK: no errors found.\n");
    } else {
        text_len = snprintf(text_buf, buf_cap, "Found %d error(s):\n\n", cap.count);
        for (int i = 0; i < cap.count; i++) {
            text_len +=
                snprintf(text_buf + text_len, buf_cap - (size_t) text_len, "- line %d:%d: %s\n",
                         cap.lines[i], cap.columns[i], cap.messages[i]);
        }
    }
    (void) text_len;

    XrJsonValue *result = xmcp_make_text_result(text_buf, cap.count > 0);
    if (ptok >= 0)
        xmcp_send_progress_notification(server, ptok, 2, 2);

    xr_free(text_buf);
    if (ast)
        xr_program_destroy(ast);
    xr_arena_destroy(arena);
    xr_free(arena);
    return result;
}

/* --------------------------------------------------------------------------
 * Tool: xray_format
 * -------------------------------------------------------------------------- */

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
                                      "Use xray_check first to find and fix errors.");
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
 * Tool: xray_diagnostics (structured line/col/severity output)
 * -------------------------------------------------------------------------- */

static XrJsonValue *tool_xray_diagnostics(XmcpServer *server, XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "tool_xray_diagnostics: NULL server");
    XR_DCHECK(arguments != NULL, "tool_xray_diagnostics: NULL arguments");

    const char *code = xjson_get_string(arguments, "code");
    if (!code || code[0] == '\0') {
        return xmcp_make_error_result("Error: 'code' parameter is required");
    }

    XrArena *arena = xr_malloc(sizeof(XrArena));
    if (!arena)
        return xmcp_make_error_result("Error: out of memory");
    xr_arena_init(arena, 0);

    ErrorCapture cap = {.count = 0};
    Parser parser;
    xr_parser_init(&parser, server->isolate, code, "<mcp-diag>", arena);
    xr_parser_set_error_callback(&parser, check_error_callback, &cap, MAX_CHECK_ERRORS);
    AstNode *ast = xr_parse_recoverable(&parser);

    size_t buf_cap = CHECK_BUF_SIZE;
    char *buf = xr_malloc(buf_cap);
    if (!buf) {
        if (ast)
            xr_program_destroy(ast);
        xr_arena_destroy(arena);
        xr_free(arena);
        return xmcp_make_error_result("Error: out of memory");
    }

    int len = 0;
    if (cap.count == 0) {
        len = snprintf(buf, buf_cap, "No diagnostics. Code is clean.\n");
    } else {
        len = snprintf(buf, buf_cap,
                       "| Line | Column | Severity | Message |\n"
                       "|------|--------|----------|---------|\n");
        for (int i = 0; i < cap.count; i++) {
            len += snprintf(buf + len, buf_cap - (size_t) len, "| %d | %d | error | %s |\n",
                            cap.lines[i], cap.columns[i], cap.messages[i]);
        }
        len += snprintf(buf + len, buf_cap - (size_t) len, "\n**Total: %d error(s)**\n", cap.count);
    }
    (void) len;

    XrJsonValue *result = xmcp_make_text_result(buf, cap.count > 0);
    xr_free(buf);
    if (ast)
        xr_program_destroy(ast);
    xr_arena_destroy(arena);
    xr_free(arena);
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

    /* Table-driven dispatch */
    XrJsonValue *result = NULL;
    for (int i = 0; TOOL_TABLE[i].name != NULL; i++) {
        if (strcmp(name, TOOL_TABLE[i].name) == 0) {
            result = TOOL_TABLE[i].handler(server, arguments);
            break;
        }
    }

    if (!result) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Unknown tool: %s", name);
        result = xmcp_make_error_result(msg);
    }

    if (empty_args)
        xjson_free(empty_args);
    return result;
}
