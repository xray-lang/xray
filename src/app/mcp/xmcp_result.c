/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_result.c - Owned tool response construction
 *
 * KEY CONCEPT:
 *   Tool response ownership is independent of compiler and execution state.
 */
#include "xmcp_json.h"
#include "xmcp_tools_internal.h"
#include "../../base/xchecks.h"

XR_FUNC XrJsonValue *xmcp_make_error_result(const char *message) {
    XR_DCHECK(message != NULL, "xmcp_make_error_result: NULL message");
    XrJsonValue *r = xjson_new_object();
    XrJsonValue *c = xjson_new_array();
    XrJsonValue *item = xjson_new_object();
    XMCP_JSON_STRING(item, "type", "text");
    XMCP_JSON_STRING(item, "text", message);
    xmcp_json_push(c, item);
    xmcp_json_set(r, "content", c);
    XMCP_JSON_BOOL(r, "isError", true);
    return xmcp_json_finish(r);
}

XR_FUNC XrJsonValue *xmcp_make_text_result(const char *text, bool is_error) {
    XR_DCHECK(text != NULL, "xmcp_make_text_result: NULL text");
    XrJsonValue *result = xjson_new_object();
    XrJsonValue *arr = xjson_new_array();
    XrJsonValue *item = xjson_new_object();
    XMCP_JSON_STRING(item, "type", "text");
    xmcp_json_set(item, "text", xjson_new_string(text));
    xmcp_json_push(arr, item);
    xmcp_json_set(result, "content", arr);
    if (is_error) {
        XMCP_JSON_BOOL(result, "isError", true);
    }
    return xmcp_json_finish(result);
}

XR_FUNC XrJsonValue *xmcp_make_text_structured_result(const char *text, XrJsonValue *structured,
                                                      bool is_error) {
    if (!structured) xmcp_json_fail();
    XrJsonValue *result = xmcp_make_text_result(text, is_error);
    xmcp_json_set(result, "structuredContent", structured);
    return xmcp_json_finish(result);
}
