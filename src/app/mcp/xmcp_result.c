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
#include "xmcp_tools_internal.h"
#include "../../base/xchecks.h"

XR_FUNC XrJsonValue *xmcp_make_error_result(const char *message) {
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

XR_FUNC XrJsonValue *xmcp_make_text_result(const char *text, bool is_error) {
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

XR_FUNC XrJsonValue *xmcp_make_text_structured_result(const char *text, XrJsonValue *structured,
                                                      bool is_error) {
    XR_DCHECK(structured != NULL, "xmcp_make_text_structured_result: NULL structured");
    XrJsonValue *result = xmcp_make_text_result(text, is_error);
    xjson_object_set(result, "structuredContent", structured);
    return result;
}
