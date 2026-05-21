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
 *   Table-driven tool registry with 6 built-in tools:
 *   xray_analyze, xray_format, xray_run,
 *   xray_syntax_lookup, xray_stdlib_search, xray_definition.
 *   Adding a tool = handler + schema + one TOOL_TABLE entry.
 */

#ifndef XMCP_TOOLS_H
#define XMCP_TOOLS_H

#include "../../base/xdefs.h"
#include "xmcp_registry.h"
#include <stddef.h>

/* Forward declarations */
typedef struct XrJsonValue XrJsonValue;
typedef struct XmcpServer XmcpServer;
typedef struct XmcpRpcError XmcpRpcError;

/* Handle "tools/list" request. Accepts params for pagination (cursor). */
XR_FUNC XrJsonValue *xmcp_handle_tools_list(XmcpServer *server, XrJsonValue *params,
                                            XmcpRpcError *error);

XR_FUNC size_t xmcp_tools_count(void);
XR_FUNC const XmcpToolDef *xmcp_tools_table(void);

/* Handle "tools/call" request. Populates `error` for protocol-level failures
 * (missing/unknown tool name, malformed arguments, schema validation errors).
 * Returns a tool result for handler-level failures. */
XR_FUNC XrJsonValue *xmcp_handle_tools_call(XmcpServer *server, XrJsonValue *params,
                                            XmcpRpcError *error);

#endif  // XMCP_TOOLS_H
