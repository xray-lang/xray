/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_tools_source_query.c - Source-backed MCP definition lookup
 *
 * KEY CONCEPT:
 *   Definition replies copy immutable checked facts before releasing their
 *   Source owner. Ambiguous names never select an arbitrary declaration.
 */
#include "xmcp_json.h"
#include "xmcp_tools_internal.h"
#include "xmcp_source.h"
#include "xmcp_server.h"
#include <stdio.h>
#include <string.h>

static XrJsonValue *source_definition_result(const char *symbol, const char *content, bool found) {
    XrJsonValue *s = xjson_new_object();
    XMCP_JSON_STRING(s, "symbol", symbol);
    XMCP_JSON_STRING(s, "kind", "source");
    XMCP_JSON_BOOL(s, "found", found);
    XMCP_JSON_STRING(s, "content", content);
    return s;
}

XR_FUNC XrJsonValue *xmcp_tool_source_definition(XmcpServer *server, XrJsonValue *arguments) {
    XrJsonValue *text = xjson_get(arguments, "code");
    XrJsonValue *identity = xjson_get(arguments, "moduleId");
    XrJsonValue *name = xjson_get(arguments, "symbol");
    if (!xjson_is_string(text) || !text->string_len || !xjson_is_string(name) || !name->string_len)
        return xmcp_make_error_result("Error: source definition requires nonempty code and symbol");
    if (!xjson_is_string(identity) || !identity->string_len ||
        memchr(identity->as.string, 0, identity->string_len) || memchr(name->as.string, 0, name->string_len))
        return xmcp_make_error_result("Error: source definition requires a valid explicit moduleId and symbol");
    XrCompileResourceLimits limits = xmcp_source_default_limits();
    XrXirCompileContext context = {NULL, xr_xir_compile_default_limits()};
    XmcpSourceOwner owner = {0};
    XrCompileRequestControl control = {0};
    XrCompileResourceStatus created = xmcp_request_control_init(server, 0, &control);
    if (created == XR_COMPILE_RESOURCE_OK) created = xr_compile_request_control_poll(&control);
    if (created == XR_COMPILE_RESOURCE_OK) created = xr_compile_resources_new(&limits, &context.resources);
    XmcpSourceRequest request = {text->as.string, text->string_len, identity->as.string, &context, &control};
    XrXirStatus status = created == XR_COMPILE_RESOURCE_OK ? xmcp_source_check(&request, &owner) :
        xmcp_source_resource_status(created);
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(owner.result.snapshot);
    const XrXirSourceDeclaration *selected = NULL;
    uint32_t matches = 0;
    if (status == XR_XIR_OK && view && view->complete) {
        for (uint32_t i = 0; i < view->declaration_count; ++i) {
            const XrXirSourceDeclaration *d = &view->declarations[i];
            if (d->name && !strcmp(d->name, name->as.string)) { selected = d; ++matches; }
        }
    }
    if (status == XR_XIR_OK) status = xmcp_source_resource_status(xr_compile_request_control_poll(&control));
    bool complete = status == XR_XIR_OK && view && view->complete;
    bool found = complete && matches == 1;
    const char *message = !complete ? owner.diagnostic.message[0] ? owner.diagnostic.message :
        "Source checking failed" : matches > 1 ? "Source symbol is ambiguous" :
        !found ? "Source symbol was not found" : selected->signature ? selected->signature : selected->name;
    XrJsonValue *s = source_definition_result(name->as.string, message, found);
    XMCP_JSON_BOOL(s, "complete", complete);
    XMCP_JSON_INT(s, "sourceStatus", status);
    if (found) {
        const XrXirSourceSyntaxView *syntax = xr_xir_compile_source_snapshot_syntax(owner.result.snapshot);
        XrXirSourceRange range = selected->range;
        if (syntax && selected->id < syntax->declaration_count) range = syntax->declarations[selected->id].name;
        XMCP_JSON_INT(s, "line", range.line); XMCP_JSON_INT(s, "column", range.column);
        XMCP_JSON_INT(s, "endLine", range.end_line); XMCP_JSON_INT(s, "endColumn", range.end_column);
        XMCP_JSON_STRING(s, "moduleId", identity->as.string);
    }
    XrJsonValue *result = xmcp_make_text_structured_result(message, s, !complete || matches > 1);
    xmcp_source_free(&owner);
    xr_compile_resources_release(context.resources);
    return xmcp_json_finish(result);
}
