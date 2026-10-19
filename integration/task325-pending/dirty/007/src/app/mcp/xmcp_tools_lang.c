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
 *   semantic analyzer pass on top; format uses one owned trivia-aware
 *   parse, formats its AST, and surfaces parser diagnostics in the
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
#include "../../toolchain/xcompiler_session.h"
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

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(server->isolate);
    if (!session) {
        xr_arena_destroy(arena);
        xr_free(arena);
        return xmcp_make_error_result("Error: compiler session is required");
    }

    XrCompilerSessionScope parse_scope;
    if (!xr_compiler_session_push_arena(session, arena, filename, &parse_scope)) {
        xr_arena_destroy(arena);
        xr_free(arena);
        return xmcp_make_error_result("Error: failed to enter compiler session");
    }

    ErrorCapture cap = {.count = 0};
    Parser parser;
    xr_parser_init(&parser, session, code, filename, arena);
    xr_parser_set_error_callback(&parser, check_error_callback, &cap, XMCP_TOOLS_MAX_CHECK_ERRORS);
    AstNode *ast = xr_parse_recoverable(&parser);

    if (ptok)
        xmcp_send_progress_notification(server, ptok, 1, 2);

    XaAnalyzer *analyzer = NULL;
    XaDiagnostic *analyzer_diags = NULL;
    int analyzer_count = 0;
    if (ast && cap.count == 0 && run_analyzer) {
        analyzer = xa_analyzer_new(session);
        if (!analyzer) {
            xr_compiler_session_pop_arena(&parse_scope);
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
    xr_compiler_session_pop_arena(&parse_scope);
    xr_arena_destroy(arena);
    xr_free(arena);
    return result;
}

typedef struct FormatCapture {
    ErrorCapture *capture;
    XrCompileState *state;
} FormatCapture;
static void format_error_callback(void *data, int line, int column, int end_line,
                                   int end_column, const char *message) {
    FormatCapture *format = data;
    ErrorCapture *capture = format->capture;
    if (capture->count >= XMCP_TOOLS_MAX_CHECK_ERRORS) return;
    size_t length = 0;
    if (xr_compile_state_string_length(format->state, message, &length) != XR_COMPILE_RESOURCE_OK) return;
    size_t copied = length < sizeof(capture->messages[0])-1 ? length : sizeof(capture->messages[0])-1;
    int i=capture->count;
    if (xr_compile_state_copy(format->state,capture->messages[i],message,copied) != XR_COMPILE_RESOURCE_OK ||
        xr_compile_state_work(format->state,1) != XR_COMPILE_RESOURCE_OK) return;
    capture->messages[i][copied]=0;
    capture->lines[i]=line; capture->columns[i]=column;
    capture->end_lines[i]=end_line; capture->end_columns[i]=end_column;
    ++capture->count;
}

/* ---- Tool: xray_format ------------------------------------------------- */

XR_FUNC XrJsonValue *xmcp_tool_xray_format(XmcpServer *server, const XmcpCallContext *ctx,
                                           XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "xmcp_tool_xray_format: NULL server");
    XR_DCHECK(ctx != NULL, "xmcp_tool_xray_format: NULL ctx");
    XR_DCHECK(arguments != NULL, "xmcp_tool_xray_format: NULL arguments");
    (void) ctx;
    (void) server;

    const char *code = xjson_get_string(arguments, "code");
    if (!code || code[0] == '\0')
        return xmcp_make_error_result("Error: 'code' must not be empty");

    XrFmtConfig config = xfmt_default_config;
    int64_t indent = xjson_get_int_or(arguments, "indentSize", 0);
    if (indent > 0 && indent <= 16)
        config.indent_size = (int) indent;
    if (xjson_get_bool(arguments, "useTabs"))
        config.use_tabs = 1;

    XrCompileResourceLimits limits={UINT64_C(1073741824),UINT64_C(268435456),UINT64_C(8589934592)};
    XrCompileResources *resources=NULL;
    XrCompilerSession *session=NULL;
    AstNode *ast=NULL;
    XrFmtOutput output={0};
    XrJsonValue *result=NULL;
    unsigned stage=0, failure=0;
    XrCompileResourceStatus created=xr_compile_resources_new(&limits,&resources);
    if(created!=XR_COMPILE_RESOURCE_OK) { stage=1; failure=(unsigned)created; goto cleanup; }
    XrCompilerSessionStatus opened=xr_compile_session_new(resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK) { stage=2; failure=(unsigned)opened; goto cleanup; }
    XrCompileState *state=xr_compile_session_compile_state(session);
    ErrorCapture capture;
    if(xr_compile_state_zero(state,&capture,sizeof(capture))!=XR_COMPILE_RESOURCE_OK) {
        stage=3; failure=(unsigned)xr_compile_state_status(state); goto cleanup;
    }
    FormatCapture format={&capture,state};
    XrParseDiagnostics diagnostics={format_error_callback,&format,XMCP_TOOLS_MAX_CHECK_ERRORS};
    XrParseStatus parsed=xr_compile_parse_with_trivia(session,code,"<mcp-format>",&diagnostics,&ast);
    if(parsed==XR_PARSE_SYNTAX) {
        int count=0; bool truncated=false;
        XrJsonValue *items=make_parser_diagnostics(&capture,&count,&truncated);
        XrJsonValue *structured=make_format_result_content("",false,config.indent_size,config.use_tabs!=0,false,truncated,items);
        char message[128];
        snprintf(message,sizeof(message),"Cannot format code with %d syntax diagnostic(s).",count);
        result=xmcp_make_text_structured_result(message,structured,true);
        goto cleanup;
    }
    if(parsed!=XR_PARSE_OK) { stage=3; failure=(unsigned)parsed; goto cleanup; }
    XrFmtStatus formatted=xr_compile_format_ast(state,ast,&config,&output);
    if(formatted!=XR_FMT_OK) { stage=4; failure=(unsigned)formatted; goto cleanup; }
    size_t length=0;
    if(xr_compile_state_string_length(state,code,&length)!=XR_COMPILE_RESOURCE_OK ||
        (length==output.length && xr_compile_state_work(state,length)!=XR_COMPILE_RESOURCE_OK)) {
        stage=5; failure=(unsigned)xr_compile_state_status(state); goto cleanup;
    }
    bool changed=length!=output.length || memcmp(code,output.text,length)!=0;
    XrJsonValue *structured=make_format_result_content(output.text,changed,config.indent_size,
        config.use_tabs!=0,true,false,xjson_new_array());
    result=xmcp_make_text_result(output.text,false);
    xjson_object_set(result,"structuredContent",structured);
cleanup:
    xr_program_destroy(ast); xr_compile_session_free(session);
    xr_compile_resources_release(resources); xr_compile_format_output_free(&output);
    if(stage) {
        char message[128];
        snprintf(message,sizeof(message),"Error: formatting failed (stage=%u status=%u)",stage,failure);
        return xmcp_make_error_result(message);
    }
    return result;
}
