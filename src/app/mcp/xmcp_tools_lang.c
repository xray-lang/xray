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
 *   owned Source semantic check on top; format uses one owned trivia-aware
 *   parse, formats its AST, and surfaces parser diagnostics in the
 *   structured result.
 */

#include "xmcp_tools_internal.h"
#include "xmcp_protocol.h"
#include "xmcp_server.h"
#include "xmcp_source.h"
#include "../../base/xjson.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../frontend/parser/xparse.h"
#include "../../frontend/parser/xast.h"
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

static void analysis_error_callback(void *data, int line, int column, int end_line,
    int end_column, const char *message) {
    ErrorCapture *capture = data;
    if (capture->count >= XMCP_TOOLS_MAX_CHECK_ERRORS) return;
    int i = capture->count++;
    capture->lines[i] = line; capture->columns[i] = column;
    capture->end_lines[i] = end_line; capture->end_columns[i] = end_column;
    snprintf(capture->messages[i], sizeof(capture->messages[i]), "%s", message);
}

static XrJsonValue *analysis_result(const char *mode, bool ok, bool truncated, XrJsonValue *diagnostics) {
    int count = xjson_array_len(diagnostics);
    XrJsonValue *structured = xjson_new_object();
    XJSON_SET_BOOL(structured, "ok", ok);
    XJSON_SET_STRING(structured, "mode", mode);
    XJSON_SET_INT(structured, "diagnosticCount", count);
    XJSON_SET_BOOL(structured, "truncated", truncated);
    xjson_object_set(structured, "diagnostics", diagnostics);
    char text[128];
    snprintf(text, sizeof(text), ok ? "OK: no diagnostics found." : "Found %d diagnostic(s).", count);
    return xmcp_make_text_structured_result(text, structured, !ok);
}

static void analysis_failure(XrJsonValue *items, int line, int column, int status,
    const char *message, const char *source) {
    xjson_array_push(items, make_diagnostic(line, column, line, column, "error", status,
        message && message[0] ? message : "Source checking failed", source));
}

XR_FUNC XrJsonValue *xmcp_tool_xray_analyze(XmcpServer *server, const XmcpCallContext *ctx,
    XrJsonValue *arguments) {
    if (!server || !ctx || !arguments) return NULL;
    XrJsonValue *input = xjson_get(arguments, "code");
    if (!xjson_is_string(input) || !input->string_len)
        return xmcp_make_error_result("Error: 'code' must not be empty");
    const char *mode = xjson_get_string(arguments, "mode");
    if (!mode || !mode[0]) mode = "full";
    if (strcmp(mode, "syntax") && strcmp(mode, "semantic") && strcmp(mode, "full"))
        return xmcp_make_error_result("Error: invalid analysis mode");
    const char *filename = xjson_get_string(arguments, "filename");
    if (!filename || !filename[0]) filename = "<mcp-analyze>";
    if (ctx->progress_token) xmcp_send_progress_notification(server, ctx->progress_token, 0, 2);
    XrJsonValue *items = xjson_new_array();
    if (!items) return NULL;
    bool ok = false, truncated = false;
    if (!strcmp(mode, "syntax")) {
        XrCompileResources *resources = NULL;
        XrCompilerSession *session = NULL;
        AstNode *ast = NULL;
        XrCompileResourceLimits limits = xmcp_source_default_limits();
        XrCompileResourceStatus created = xr_compile_resources_new(&limits, &resources);
        XrCompilerSessionStatus opened = created == XR_COMPILE_RESOURCE_OK ?
            xr_compile_session_new(resources, &session) : XR_COMPILER_SESSION_OUT_OF_MEMORY;
        ErrorCapture capture = {0};
        XrParseStatus status = created == XR_COMPILE_RESOURCE_BUDGET || opened == XR_COMPILER_SESSION_BUDGET ?
            XR_PARSE_BUDGET : opened == XR_COMPILER_SESSION_OK ? XR_PARSE_OK : XR_PARSE_OUT_OF_MEMORY;
        if (status == XR_PARSE_OK && memchr(input->as.string, 0, input->string_len)) {
            analysis_failure(items, 0, 0, XR_PARSE_SYNTAX, "Source contains a NUL byte", "parser");
            status = XR_PARSE_SYNTAX;
        } else if (status == XR_PARSE_OK) {
            XrParseDiagnostics diagnostics = {analysis_error_callback, &capture, XMCP_TOOLS_MAX_CHECK_ERRORS};
            status = xr_compile_parse_with_trivia(session, input->as.string, filename, &diagnostics, &ast);
            int count = 0;
            xjson_free(items);
            items = make_parser_diagnostics(&capture, &count, &truncated);
        }
        if (status != XR_PARSE_OK && xjson_array_len(items) == 0) {
            analysis_failure(items, 0, 0, status, "Parser resource or input failure", "parser");
        }
        xr_program_destroy(ast);
        xr_compile_session_free(session);
        xr_compile_resources_release(resources);
        ok = status == XR_PARSE_OK;
    } else {
        XmcpSourceOwner owner = {0};
        XrCompileResourceLimits limits = xmcp_source_default_limits();
        XrXirCompileContext context = {NULL, xr_xir_compile_default_limits()};
        XrCompileResourceStatus created = xr_compile_resources_new(&limits, &context.resources);
        XmcpSourceRequest request = {input->as.string, input->string_len, "mcp-analyze", &context};
        XrXirStatus status = created == XR_COMPILE_RESOURCE_OK ? xmcp_source_check(&request, &owner) :
            created == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
        const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(owner.result.snapshot);
        ok = status == XR_XIR_OK && view && view->complete;
        if (!ok) analysis_failure(items, owner.diagnostic.line, owner.diagnostic.column,
            (int)status, owner.diagnostic.message, "source");
        xmcp_source_free(&owner);
        xr_compile_resources_release(context.resources);
    }
    XrJsonValue *result = analysis_result(mode, ok, truncated, items);
    if (ctx->progress_token) xmcp_send_progress_notification(server, ctx->progress_token, 2, 2);
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
