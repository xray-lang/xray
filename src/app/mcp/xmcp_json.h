/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_json.h - Failure-atomic ownership for MCP JSON construction
 */
#ifndef XMCP_JSON_H
#define XMCP_JSON_H
#include "../../base/xjson.h"
XR_FUNC void xmcp_json_begin(void);
XR_FUNC void xmcp_json_fail(void);
XR_FUNC bool xmcp_json_failed(void);
/* Set/push consume the supplied value on every path. A failed insertion leaves
 * the logical container unchanged and makes this request's builder fail. */
XR_FUNC bool xmcp_json_set(XrJsonValue *object, const char *key, XrJsonValue *owned);
XR_FUNC bool xmcp_json_push(XrJsonValue *array, XrJsonValue *owned);
XR_FUNC XrJsonValue *xmcp_json_clone(const XrJsonValue *borrowed);
/* Finish consumes an owned response and rejects all partial publication.
 * Borrowed schema caches must publish only the non-NULL finished result. */
XR_FUNC XrJsonValue *xmcp_json_finish(XrJsonValue *owned);
#define XMCP_JSON_STRING(o,k,v) xmcp_json_set(o,k,xjson_new_string(v))
#define XMCP_JSON_INT(o,k,v) xmcp_json_set(o,k,xjson_new_number((double)(v)))
#define XMCP_JSON_BOOL(o,k,v) xmcp_json_set(o,k,xjson_new_bool(v))
#define XMCP_JSON_NULL(o,k) xmcp_json_set(o,k,xjson_new_null())
#endif // XMCP_JSON_H
