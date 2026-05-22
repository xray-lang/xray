/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_tools_schema.c - JSON Schema builders for every tool in TOOL_TABLE
 *
 * KEY CONCEPT:
 *   Each builder returns a JSON Schema object describing one tool's input
 *   or output contract.  Results are cached in a function-local static so
 *   tools/list and tools/call can read the same DOM repeatedly without
 *   rebuilding it; callers that hand the schema to a JSON-RPC response
 *   must xjson_clone() before transferring ownership.
 */

#include "xmcp_tools_internal.h"
#include "../../base/xchecks.h"
#include "../../base/xjson.h"
#include <stdint.h>

/* ---- Local schema-construction helpers --------------------------------- */

static XrJsonValue *schema_add_prop(XrJsonValue *props, const char *name, const char *type,
                                    const char *desc) {
    XrJsonValue *p = xjson_new_object();
    XJSON_SET_STRING(p, "type", type);
    XJSON_SET_STRING(p, "description", desc);
    xjson_object_set(props, name, p);
    return p;
}

static void schema_prop_set_int_range(XrJsonValue *prop, int64_t min, int64_t max) {
    XR_DCHECK(prop != NULL, "schema_prop_set_int_range: NULL prop");
    XJSON_SET_INT(prop, "minimum", min);
    XJSON_SET_INT(prop, "maximum", max);
}

static void schema_prop_set_string_enum(XrJsonValue *prop, const char *const *values, int count) {
    XR_DCHECK(prop != NULL, "schema_prop_set_string_enum: NULL prop");
    XR_DCHECK(values != NULL, "schema_prop_set_string_enum: NULL values");
    XrJsonValue *allowed = xjson_new_array();
    for (int i = 0; i < count; i++)
        xjson_array_push(allowed, xjson_new_string(values[i]));
    xjson_object_set(prop, "enum", allowed);
}

/* ---- Schema builders --------------------------------------------------- */

XR_FUNC XrJsonValue *xmcp_schema_analyze(void) {
    static XrJsonValue *cached = NULL;
    if (cached)
        return cached;
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "code", "string", "Xray source code to analyze");
    schema_add_prop(p, "filename", "string", "Optional filename used in diagnostics");
    XrJsonValue *mode =
        schema_add_prop(p, "mode", "string", "Analysis mode: syntax, semantic, or full");
    static const char *const modes[] = {"syntax", "semantic", "full"};
    schema_prop_set_string_enum(mode, modes, 3);
    xjson_object_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xjson_array_push(r, xjson_new_string("code"));
    xjson_object_set(s, "required", r);
    cached = s;
    return s;
}

XR_FUNC XrJsonValue *xmcp_schema_analyze_output(void) {
    static XrJsonValue *cached = NULL;
    if (cached)
        return cached;
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "ok", "boolean", "True when no diagnostics were produced");
    schema_add_prop(p, "mode", "string", "Effective analysis mode");
    schema_add_prop(p, "diagnosticCount", "integer", "Number of diagnostics returned");
    schema_add_prop(p, "truncated", "boolean", "True when diagnostics hit the result limit");
    schema_add_prop(p, "diagnostics", "array", "Structured diagnostics");
    xjson_object_set(s, "properties", p);
    cached = s;
    return s;
}

XR_FUNC XrJsonValue *xmcp_schema_format(void) {
    static XrJsonValue *cached = NULL;
    if (cached)
        return cached;
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "code", "string", "Xray source code to format");
    XrJsonValue *indent =
        schema_add_prop(p, "indentSize", "integer", "Indent size in spaces (default: 4)");
    schema_prop_set_int_range(indent, 1, 16);
    schema_add_prop(p, "useTabs", "boolean", "Use tabs instead of spaces (default: false)");
    xjson_object_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xjson_array_push(r, xjson_new_string("code"));
    xjson_object_set(s, "required", r);
    cached = s;
    return s;
}

XR_FUNC XrJsonValue *xmcp_schema_format_output(void) {
    static XrJsonValue *cached = NULL;
    if (cached)
        return cached;
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "ok", "boolean", "True when formatting succeeded");
    schema_add_prop(p, "formattedCode", "string", "Formatted Xray source code");
    schema_add_prop(p, "changed", "boolean", "True when formatting changed the input");
    schema_add_prop(p, "indentSize", "integer", "Effective indent size");
    schema_add_prop(p, "useTabs", "boolean", "Effective tab indentation flag");
    schema_add_prop(p, "diagnosticCount", "integer", "Number of parser diagnostics returned");
    schema_add_prop(p, "truncated", "boolean", "True when diagnostics hit the result limit");
    schema_add_prop(p, "diagnostics", "array", "Structured parser diagnostics");
    xjson_object_set(s, "properties", p);
    cached = s;
    return s;
}

XR_FUNC XrJsonValue *xmcp_schema_run(void) {
    static XrJsonValue *cached = NULL;
    if (cached)
        return cached;
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "code", "string", "Xray source code to execute (top-level statements).");
    XrJsonValue *timeout =
        schema_add_prop(p, "timeoutMs", "integer",
                        "Wall-clock time budget in milliseconds (default 2000, max 30000).");
    schema_prop_set_int_range(timeout, 1, XMCP_TOOLS_RUN_TIMEOUT_HARD_MAX_MS);
    XrJsonValue *output_limit = schema_add_prop(
        p, "outputLimit", "integer", "Maximum captured stdout bytes (default 8192, max 65536).");
    schema_prop_set_int_range(output_limit, 1, XMCP_TOOLS_RUN_OUTPUT_HARD_MAX);
    xjson_object_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xjson_array_push(r, xjson_new_string("code"));
    xjson_object_set(s, "required", r);
    cached = s;
    return s;
}

XR_FUNC XrJsonValue *xmcp_schema_run_output(void) {
    static XrJsonValue *cached = NULL;
    if (cached)
        return cached;
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
    cached = s;
    return s;
}

XR_FUNC XrJsonValue *xmcp_schema_syntax(void) {
    static XrJsonValue *cached = NULL;
    if (cached)
        return cached;
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "topic", "string",
                    "Syntax topic to look up (e.g., channel, coroutine, class, enum)");
    xjson_object_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xjson_array_push(r, xjson_new_string("topic"));
    xjson_object_set(s, "required", r);
    cached = s;
    return s;
}

XR_FUNC XrJsonValue *xmcp_schema_syntax_output(void) {
    static XrJsonValue *cached = NULL;
    if (cached)
        return cached;
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "topic", "string", "Requested syntax topic");
    schema_add_prop(p, "found", "boolean", "True when the topic was found");
    schema_add_prop(p, "content", "string", "Syntax reference content");
    xjson_object_set(s, "properties", p);
    cached = s;
    return s;
}

XR_FUNC XrJsonValue *xmcp_schema_stdlib(void) {
    static XrJsonValue *cached = NULL;
    if (cached)
        return cached;
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
    cached = s;
    return s;
}

XR_FUNC XrJsonValue *xmcp_schema_stdlib_output(void) {
    static XrJsonValue *cached = NULL;
    if (cached)
        return cached;
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
    cached = s;
    return s;
}

XR_FUNC XrJsonValue *xmcp_schema_definition(void) {
    static XrJsonValue *cached = NULL;
    if (cached)
        return cached;
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "symbol", "string",
                    "Symbol name to look up (e.g., 'http.Server', 'print', 'chan')");
    xjson_object_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xjson_array_push(r, xjson_new_string("symbol"));
    xjson_object_set(s, "required", r);
    cached = s;
    return s;
}

XR_FUNC XrJsonValue *xmcp_schema_definition_output(void) {
    static XrJsonValue *cached = NULL;
    if (cached)
        return cached;
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "symbol", "string", "Requested symbol");
    schema_add_prop(p, "kind", "string", "Definition source: syntax, stdlib, or none");
    schema_add_prop(p, "found", "boolean", "True when a definition was found");
    schema_add_prop(p, "content", "string", "Definition content or not-found message");
    xjson_object_set(s, "properties", p);
    cached = s;
    return s;
}
