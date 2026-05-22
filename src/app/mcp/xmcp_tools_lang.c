/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_tools_lang.c - Frontend-driven MCP tools (analyze, format)
 *
 * KEY CONCEPT:
 *   Both tools share the parser-error capture pipeline.  analyze adds the
 *   semantic analyzer pass on top; format runs an additional trivia-aware
 *   parse, hands the AST to xfmt, and surfaces parser diagnostics in the
 *   structured result.
 */

#include "xmcp_tools_internal.h"
#include "xmcp_protocol.h"
#include "xmcp_server.h"
#include "../../base/xjson.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../base/xarena.h"
#include "../../frontend/parser/xparse.h"
#include "../../frontend/parser/xast.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../frontend/format/xfmt.h"
#include <stdio.h>
#include <string.h>

/* ---- Parser error capture ---------------------------------------------- */

typedef struct {
    int lines[XMCP_TOOLS_MAX_CHECK_ERRORS];
    int columns[XMCP_TOOLS_MAX_CHECK_ERRORS];
    int end_lines[XMCP_TOOLS_MAX_CHECK_ERRORS];
    int end_columns[XMCP_TOOLS_MAX_CHECK_ERRORS];
    char messages[XMCP_TOOLS_MAX_CHECK_ERRORS][512];
    int count;
} ErrorCapture;

static void check_error_callback(void *user_data, int line, int column, int end_line,
                                 int end_column, const char *message) {
    ErrorCapture *cap = (ErrorCapture *) user_data;
    XR_DCHECK(cap != NULL, "check_error_callback: NULL capture");
    if (cap->count >= XMCP_TOOLS_MAX_CHECK_ERRORS)
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

static XrJsonValue *make_parser_diagnostics(const ErrorCapture *cap, int *out_count,
                                            bool *out_truncated) {
    XR_DCHECK(cap != NULL, "make_parser_diagnostics: NULL capture");
    XR_DCHECK(out_count != NULL, "make_parser_diagnostics: NULL count");
    XR_DCHECK(out_truncated != NULL, "make_parser_diagnostics: NULL truncated");

    XrJsonValue *diagnostics = xjson_new_array();
    int emitted = 0;
    for (int i = 0; i < cap->count && emitted < XMCP_TOOLS_MAX_CHECK_ERRORS; i++, emitted++) {
        xjson_array_push(diagnostics, make_diagnostic(cap->lines[i], cap->columns[i],
                                                      cap->end_lines[i], cap->end_columns[i],
                                                      "error", 0, cap->messages[i], "parser"));
    }
    *out_count = emitted;
    *out_truncated = cap->count >= XMCP_TOOLS_MAX_CHECK_ERRORS;
    return diagnostics;
}

static XrJsonValue *make_format_result_content(const char *formatted, bool changed, int indent_size,
                                               bool use_tabs, bool ok, bool truncated,
                                               XrJsonValue *diagnostics) {
    XR_DCHECK(formatted != NULL, "make_format_result_content: NULL formatted");
    XR_DCHECK(diagnostics != NULL, "make_format_result_content: NULL diagnostics");
    XrJsonValue *structured = xjson_new_object();
    XJSON_SET_BOOL(structured, "ok", ok);
    XJSON_SET_STRING(structured, "formattedCode", formatted);
    XJSON_SET_BOOL(structured, "changed", changed);
    XJSON_SET_INT(structured, "indentSize", indent_size);
    XJSON_SET_BOOL(structured, "useTabs", use_tabs);
    XJSON_SET_INT(structured, "diagnosticCount", xjson_array_len(diagnostics));
    XJSON_SET_BOOL(structured, "truncated", truncated);
    xjson_object_set(structured, "diagnostics", diagnostics);
    return structured;
}

/* ---- Tool: xray_analyze ------------------------------------------------ */

XR_FUNC XrJsonValue *xmcp_tool_xray_analyze(XmcpServer *server, const XmcpCallContext *ctx,
                                            XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "xmcp_tool_xray_analyze: NULL server");
    XR_DCHECK(ctx != NULL, "xmcp_tool_xray_analyze: NULL ctx");
    XR_DCHECK(arguments != NULL, "xmcp_tool_xray_analyze: NULL arguments");

    const char *code = xjson_get_string(arguments, "code");
    if (!code || code[0] == '\0')
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

    const XrJsonValue *ptok = ctx->progress_token;
    if (ptok)
        xmcp_send_progress_notification(server, ptok, 0, 2);

    ErrorCapture cap = {.count = 0};
    Parser parser;
    xr_parser_init(&parser, server->isolate, code, filename, arena);
    xr_parser_set_error_callback(&parser, check_error_callback, &cap, XMCP_TOOLS_MAX_CHECK_ERRORS);
    AstNode *ast = xr_parse_recoverable(&parser);

    if (ptok)
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
    for (int i = 0; i < cap.count && emitted < XMCP_TOOLS_MAX_CHECK_ERRORS; i++, emitted++) {
        xjson_array_push(diagnostics, make_diagnostic(cap.lines[i], cap.columns[i],
                                                      cap.end_lines[i], cap.end_columns[i], "error",
                                                      0, cap.messages[i], "parser"));
    }
    for (XaDiagnostic *d = analyzer_diags; d && emitted < XMCP_TOOLS_MAX_CHECK_ERRORS;
         d = d->next, emitted++) {
        xjson_array_push(diagnostics,
                         make_diagnostic((int) d->location.line, (int) d->location.column,
                                         (int) d->location.end_line, (int) d->location.end_column,
                                         diag_severity_name(d->severity), d->code, d->message,
                                         "analyzer"));
    }
    if (cap.count >= XMCP_TOOLS_MAX_CHECK_ERRORS ||
        analyzer_count > XMCP_TOOLS_MAX_CHECK_ERRORS - cap.count)
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
    if (ptok)
        xmcp_send_progress_notification(server, ptok, 2, 2);

    if (analyzer)
        xa_analyzer_free(analyzer);
    xr_arena_destroy(arena);
    xr_free(arena);
    return result;
}

/* ---- Tool: xray_format ------------------------------------------------- */

XR_FUNC XrJsonValue *xmcp_tool_xray_format(XmcpServer *server, const XmcpCallContext *ctx,
                                           XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "xmcp_tool_xray_format: NULL server");
    XR_DCHECK(ctx != NULL, "xmcp_tool_xray_format: NULL ctx");
    XR_DCHECK(arguments != NULL, "xmcp_tool_xray_format: NULL arguments");
    (void) ctx;

    const char *code = xjson_get_string(arguments, "code");
    if (!code || code[0] == '\0')
        return xmcp_make_error_result("Error: 'code' must not be empty");

    XrFmtConfig config = xfmt_default_config;
    int64_t indent = xjson_get_int_or(arguments, "indentSize", 0);
    if (indent > 0 && indent <= 16)
        config.indent_size = (int) indent;
    if (xjson_get_bool(arguments, "useTabs"))
        config.use_tabs = 1;

    XrArena *arena = xr_malloc(sizeof(XrArena));
    if (!arena)
        return xmcp_make_error_result("Error: out of memory");
    xr_arena_init(arena, 0);

    ErrorCapture cap = {.count = 0};
    Parser parser;
    xr_parser_init(&parser, server->isolate, code, "<mcp-format>", arena);
    xr_parser_set_error_callback(&parser, check_error_callback, &cap, XMCP_TOOLS_MAX_CHECK_ERRORS);
    AstNode *syntax_ast = xr_parse_recoverable(&parser);
    (void) syntax_ast;

    if (cap.count > 0) {
        int diagnostic_count = 0;
        bool truncated = false;
        XrJsonValue *diagnostics = make_parser_diagnostics(&cap, &diagnostic_count, &truncated);
        XrJsonValue *structured = make_format_result_content(
            "", false, config.indent_size, config.use_tabs != 0, false, truncated, diagnostics);
        char text[128];
        snprintf(text, sizeof(text), "Cannot format code with %d syntax diagnostic(s).",
                 diagnostic_count);
        XrJsonValue *result = xmcp_make_text_structured_result(text, structured, true);
        xr_arena_destroy(arena);
        xr_free(arena);
        return result;
    }
    xr_arena_destroy(arena);
    xr_free(arena);

    AstNode *ast = xr_parse_with_trivia(server->isolate, code, "<mcp-format>");
    if (!ast) {
        XrJsonValue *diagnostics = xjson_new_array();
        XrJsonValue *structured = make_format_result_content(
            "", false, config.indent_size, config.use_tabs != 0, false, false, diagnostics);
        return xmcp_make_text_structured_result("Error: formatting parser failed", structured,
                                                true);
    }

    char *formatted = xfmt_format_ast(ast, &config, server->isolate);
    xr_program_destroy(ast);
    if (!formatted)
        return xmcp_make_error_result("Error: formatting failed");

    XrJsonValue *structured =
        make_format_result_content(formatted, strcmp(code, formatted) != 0, config.indent_size,
                                   config.use_tabs != 0, true, false, xjson_new_array());

    XrJsonValue *result = xmcp_make_text_result(formatted, false);
    xjson_object_set(result, "structuredContent", structured);
    xr_free(formatted);
    return result;
}
