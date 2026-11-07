/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_tools_run.c - Report the availability of the MCP execution tool
 */
#include "xmcp_tools_internal.h"
#include "xmcp_server.h"

XR_FUNC XrJsonValue *xmcp_tool_xray_run(XmcpServer *server, const XmcpCallContext *ctx,
    XrJsonValue *arguments) {
    if (!server || !ctx || !arguments) return NULL;
    return xmcp_make_error_result("MCP execution is temporarily unavailable");
}
