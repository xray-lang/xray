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
 *   Each builder returns an owned JSON Schema describing one tool's input
 *   or output contract. Validation releases its schema; tools/list transfers
 *   it into the response. No process cache retains a request's allocations.
 */

#include "xmcp_json.h"
#include "xmcp_tools_internal.h"
#include "../../base/xchecks.h"
#include "../../base/xjson.h"
#include <stdint.h>

/* ---- Local schema-construction helpers --------------------------------- */

static XrJsonValue *schema_add_prop(XrJsonValue *props, const char *name, const char *type,
                                    const char *desc) {
    XrJsonValue *p = xjson_new_object();
    XMCP_JSON_STRING(p, "type", type);
    XMCP_JSON_STRING(p, "description", desc);
    if (!xmcp_json_set(props, name, p)) return NULL;
    return p;
}

static void schema_prop_set_int_range(XrJsonValue *prop, int64_t min, int64_t max) {
    if (!prop) { xmcp_json_fail(); return; }
    XMCP_JSON_INT(prop, "minimum", min);
    XMCP_JSON_INT(prop, "maximum", max);
}

static void schema_prop_set_string_enum(XrJsonValue *prop, const char *const *values, int count) {
    if (!prop) { xmcp_json_fail(); return; }
    XR_DCHECK(values != NULL, "schema_prop_set_string_enum: NULL values");
    XrJsonValue *allowed = xjson_new_array();
    for (int i = 0; i < count; i++)
        xmcp_json_push(allowed, xjson_new_string(values[i]));
    xmcp_json_set(prop, "enum", allowed);
}

/* ---- Schema builders --------------------------------------------------- */

XR_FUNC XrJsonValue *xmcp_schema_analyze(void) {
    XrJsonValue *s = xjson_new_object();
    XMCP_JSON_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "code", "string", "Xray source code to analyze");
    schema_add_prop(p, "filename", "string", "Optional filename used in diagnostics");
    XrJsonValue *mode =
        schema_add_prop(p, "mode", "string", "Analysis mode: syntax, semantic, or full");
    static const char *const modes[] = {"syntax", "semantic", "full"};
    schema_prop_set_string_enum(mode, modes, 3);
    xmcp_json_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xmcp_json_push(r, xjson_new_string("code"));
    xmcp_json_set(s, "required", r);
    return xmcp_json_finish(s);
}

XR_FUNC XrJsonValue *xmcp_schema_analyze_output(void) {
    XrJsonValue *s = xjson_new_object();
    XMCP_JSON_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "ok", "boolean", "True when no diagnostics were produced");
    schema_add_prop(p, "mode", "string", "Effective analysis mode");
    schema_add_prop(p, "diagnosticCount", "integer", "Number of diagnostics returned");
    schema_add_prop(p, "truncated", "boolean", "True when diagnostics hit the result limit");
    schema_add_prop(p, "diagnostics", "array", "Structured diagnostics");
    xmcp_json_set(s, "properties", p);
    return xmcp_json_finish(s);
}

XR_FUNC XrJsonValue *xmcp_schema_format(void) {
    XrJsonValue *s = xjson_new_object();
    XMCP_JSON_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "code", "string", "Xray source code to format");
    XrJsonValue *indent =
        schema_add_prop(p, "indentSize", "integer", "Indent size in spaces (default: 4)");
    schema_prop_set_int_range(indent, 1, 16);
    schema_add_prop(p, "useTabs", "boolean", "Use tabs instead of spaces (default: false)");
    xmcp_json_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xmcp_json_push(r, xjson_new_string("code"));
    xmcp_json_set(s, "required", r);
    return xmcp_json_finish(s);
}

XR_FUNC XrJsonValue *xmcp_schema_format_output(void) {
    XrJsonValue *s = xjson_new_object();
    XMCP_JSON_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "ok", "boolean", "True when formatting succeeded");
    schema_add_prop(p, "formattedCode", "string", "Formatted Xray source code");
    schema_add_prop(p, "changed", "boolean", "True when formatting changed the input");
    schema_add_prop(p, "indentSize", "integer", "Effective indent size");
    schema_add_prop(p, "useTabs", "boolean", "Effective tab indentation flag");
    schema_add_prop(p, "diagnosticCount", "integer", "Number of parser diagnostics returned");
    schema_add_prop(p, "truncated", "boolean", "True when diagnostics hit the result limit");
    schema_add_prop(p, "diagnostics", "array", "Structured parser diagnostics");
    xmcp_json_set(s, "properties", p);
    return xmcp_json_finish(s);
}

XR_FUNC XrJsonValue *xmcp_schema_run(void) {
    XrJsonValue *s = xjson_new_object();
    XMCP_JSON_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "code", "string", "Xray source code to execute (top-level statements).");
    schema_add_prop(p, "moduleId", "string",
                    "Explicit durable identity namespace for this memory module.");
    XrJsonValue *timeout =
        schema_add_prop(p, "timeoutMs", "integer",
                        "Wall-clock time budget in milliseconds (default 2000, max 30000).");
    schema_prop_set_int_range(timeout, 1, XMCP_TOOLS_RUN_TIMEOUT_HARD_MAX_MS);
    XrJsonValue *output_limit = schema_add_prop(
        p, "outputLimit", "integer", "Maximum captured stdout bytes (default 8192, max 65536).");
    schema_prop_set_int_range(output_limit, 1, XMCP_TOOLS_RUN_OUTPUT_HARD_MAX);
    xmcp_json_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xmcp_json_push(r, xjson_new_string("code"));
    xmcp_json_push(r, xjson_new_string("moduleId"));
    xmcp_json_set(s, "required", r);
    return xmcp_json_finish(s);
}

XR_FUNC XrJsonValue *xmcp_schema_run_output(void) {
    XrJsonValue *s = xjson_new_object();
    XMCP_JSON_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "ok", "boolean", "True iff exitCode==0 and timedOut==false.");
    schema_add_prop(p, "exitCode", "integer", "Return value from source execution (0 on success).");
    schema_add_prop(p, "stdout", "string", "Captured print() output, truncated to outputLimit.");
    schema_add_prop(p, "stderr", "string", "Captured error output and owned terminal diagnostics.");
    schema_add_prop(p, "stderrTruncated", "boolean", "True when captured error output was clipped.");
    schema_add_prop(p, "sourceStatus", "integer", "SourceProduct admission status.");
    schema_add_prop(p, "runtimeStatus", "integer", "HostCall execution or cleanup status.");
    schema_add_prop(p, "timedOut", "boolean", "True if execution exceeded timeoutMs.");
    schema_add_prop(p, "truncated", "boolean", "True if stdout was clipped to outputLimit.");
    schema_add_prop(p, "outputBytes", "integer",
                    "Number of bytes actually returned in the stdout field.");
    xmcp_json_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xmcp_json_push(r, xjson_new_string("ok"));
    xmcp_json_push(r, xjson_new_string("exitCode"));
    xmcp_json_push(r, xjson_new_string("stdout"));
    xmcp_json_push(r, xjson_new_string("timedOut"));
    xmcp_json_push(r, xjson_new_string("truncated"));
    xmcp_json_push(r, xjson_new_string("outputBytes"));
    xmcp_json_set(s, "required", r);
    return xmcp_json_finish(s);
}

XR_FUNC XrJsonValue *xmcp_schema_syntax(void) {
    XrJsonValue *s = xjson_new_object();
    XMCP_JSON_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "topic", "string",
                    "Syntax topic to look up (e.g., channel, coroutine, class, enum)");
    xmcp_json_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xmcp_json_push(r, xjson_new_string("topic"));
    xmcp_json_set(s, "required", r);
    return xmcp_json_finish(s);
}

XR_FUNC XrJsonValue *xmcp_schema_syntax_output(void) {
    XrJsonValue *s = xjson_new_object();
    XMCP_JSON_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "topic", "string", "Requested syntax topic");
    schema_add_prop(p, "found", "boolean", "True when the topic was found");
    schema_add_prop(p, "content", "string", "Syntax reference content");
    xmcp_json_set(s, "properties", p);
    return xmcp_json_finish(s);
}

XR_FUNC XrJsonValue *xmcp_schema_stdlib(void) {
    XrJsonValue *s = xjson_new_object();
    XMCP_JSON_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "query", "string", "Search query for standard library modules");
    schema_add_prop(p, "module", "string",
                    "Optional: filter by specific module name (e.g., http, json, time)");
    xmcp_json_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xmcp_json_push(r, xjson_new_string("query"));
    xmcp_json_set(s, "required", r);
    return xmcp_json_finish(s);
}

XR_FUNC XrJsonValue *xmcp_schema_stdlib_output(void) {
    XrJsonValue *s = xjson_new_object();
    XMCP_JSON_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "query", "string", "Requested stdlib search query");
    schema_add_prop(p, "module", "string", "Effective module filter");
    schema_add_prop(p, "matchCount", "integer", "Number of ranked matches");
    schema_add_prop(p, "found", "boolean", "True when at least one ranked match exists");
    schema_add_prop(p, "content", "string", "Formatted stdlib search result");
    schema_add_prop(p, "matches", "array", "Ranked matches with module, summary, and score");
    xmcp_json_set(s, "properties", p);
    return xmcp_json_finish(s);
}

XR_FUNC XrJsonValue *xmcp_schema_definition(void) {
    XrJsonValue *s = xjson_new_object();
    XMCP_JSON_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "symbol", "string",
                    "Symbol name to look up (e.g., 'http.Server', 'print', 'chan')");
    schema_add_prop(p, "code", "string", "Optional source text for checked definition lookup");
    schema_add_prop(p, "moduleId", "string", "Explicit memory-module identity required when code is supplied");
    xmcp_json_set(s, "properties", p);
    XrJsonValue *r = xjson_new_array();
    xmcp_json_push(r, xjson_new_string("symbol"));
    xmcp_json_set(s, "required", r);
    return xmcp_json_finish(s);
}

XR_FUNC XrJsonValue *xmcp_schema_definition_output(void) {
    XrJsonValue *s = xjson_new_object();
    XMCP_JSON_STRING(s, "type", "object");
    XrJsonValue *p = xjson_new_object();
    schema_add_prop(p, "symbol", "string", "Requested symbol");
    schema_add_prop(p, "kind", "string", "Definition source: syntax, stdlib, source, or none");
    schema_add_prop(p, "found", "boolean", "True when a definition was found");
    schema_add_prop(p, "content", "string", "Definition content or not-found message");
    xmcp_json_set(s, "properties", p);
    return xmcp_json_finish(s);
}
