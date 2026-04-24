/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_tools.h - MCP tool handlers
 *
 * KEY CONCEPT:
 *   Table-driven tool registry with 7 built-in tools:
 *   xray_check, xray_format, xray_diagnostics, xray_run,
 *   xray_syntax_lookup, xray_stdlib_search, xray_definition.
 *   Adding a tool = handler + schema + one TOOL_TABLE entry.
 */

#ifndef XMCP_TOOLS_H
#define XMCP_TOOLS_H

#include "../../base/xdefs.h"

/* Forward declarations */
typedef struct XrJsonValue XrJsonValue;
typedef struct XmcpServer XmcpServer;

/* Handle "tools/list" request. Accepts params for pagination (cursor). */
XR_FUNC XrJsonValue *xmcp_handle_tools_list(XrJsonValue *params);

/* Handle "tools/call" request. */
XR_FUNC XrJsonValue *xmcp_handle_tools_call(XmcpServer *server, XrJsonValue *params);

#endif // XMCP_TOOLS_H
