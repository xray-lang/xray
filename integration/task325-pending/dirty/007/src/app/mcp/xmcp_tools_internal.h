/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_tools_internal.h - Shared private API for xmcp_tools_*.c
 *
 * KEY CONCEPT:
 *   Plumbing shared between the tool-table file (xmcp_tools.c) and the
 *   per-domain handler files (xmcp_tools_schema/lang/run/kb).  Not part of
 *   the public MCP server API; do not include from outside src/app/mcp/.
 */

#ifndef XMCP_TOOLS_INTERNAL_H
#define XMCP_TOOLS_INTERNAL_H

#include "../../base/xdefs.h"
#include "../../base/xjson.h"
#include "xmcp_registry.h"
#include <stdbool.h>

struct XmcpServer;

/* Maximum errors captured per check/diagnostics run.  Shared between the
 * parser/analyzer plumbing in xmcp_tools_lang.c and the dispatch layer that
 * sizes intermediate buffers. */
#define XMCP_TOOLS_MAX_CHECK_ERRORS 20
#define XMCP_TOOLS_CHECK_BUF_SIZE 4096

/* Quotas for the xray_run sandbox.  Declared here because the schema builder
 * (xmcp_tools_schema.c) and the runner itself (xmcp_tools_run.c) must agree
 * on the advertised range for `timeoutMs` and `outputLimit`. */
#define XMCP_TOOLS_RUN_OUTPUT_DEFAULT 8192
#define XMCP_TOOLS_RUN_OUTPUT_HARD_MAX 65536
#define XMCP_TOOLS_RUN_TIMEOUT_DEFAULT_MS 2000
#define XMCP_TOOLS_RUN_TIMEOUT_HARD_MAX_MS 30000

/* ---- Result helpers ---------------------------------------------------- */

/* All three return a freshly-allocated XrJsonValue object the caller must
 * eventually free (handled by the tools/call dispatch).  The structured
 * variant takes ownership of `structured`. */
XR_FUNC XrJsonValue *xmcp_make_error_result(const char *message);
XR_FUNC XrJsonValue *xmcp_make_text_result(const char *text, bool is_error);
XR_FUNC XrJsonValue *xmcp_make_text_structured_result(const char *text, XrJsonValue *structured,
                                                      bool is_error);

/* ---- Tool handlers (forward decls used by TOOL_TABLE) ------------------ */

XR_FUNC XrJsonValue *xmcp_tool_xray_analyze(struct XmcpServer *s, const XmcpCallContext *ctx,
                                            XrJsonValue *args);
XR_FUNC XrJsonValue *xmcp_tool_xray_format(struct XmcpServer *s, const XmcpCallContext *ctx,
                                           XrJsonValue *args);
XR_FUNC XrJsonValue *xmcp_tool_xray_run(struct XmcpServer *s, const XmcpCallContext *ctx,
                                        XrJsonValue *args);
XR_FUNC XrJsonValue *xmcp_tool_xray_syntax_lookup(struct XmcpServer *s, const XmcpCallContext *ctx,
                                                  XrJsonValue *args);
XR_FUNC XrJsonValue *xmcp_tool_xray_stdlib_search(struct XmcpServer *s, const XmcpCallContext *ctx,
                                                  XrJsonValue *args);
XR_FUNC XrJsonValue *xmcp_tool_xray_definition(struct XmcpServer *s, const XmcpCallContext *ctx,
                                               XrJsonValue *args);

/* ---- Schema builders (forward decls used by TOOL_TABLE) ---------------- */

XR_FUNC XrJsonValue *xmcp_schema_analyze(void);
XR_FUNC XrJsonValue *xmcp_schema_analyze_output(void);
XR_FUNC XrJsonValue *xmcp_schema_format(void);
XR_FUNC XrJsonValue *xmcp_schema_format_output(void);
XR_FUNC XrJsonValue *xmcp_schema_run(void);
XR_FUNC XrJsonValue *xmcp_schema_run_output(void);
XR_FUNC XrJsonValue *xmcp_schema_syntax(void);
XR_FUNC XrJsonValue *xmcp_schema_syntax_output(void);
XR_FUNC XrJsonValue *xmcp_schema_stdlib(void);
XR_FUNC XrJsonValue *xmcp_schema_stdlib_output(void);
XR_FUNC XrJsonValue *xmcp_schema_definition(void);
XR_FUNC XrJsonValue *xmcp_schema_definition_output(void);

#endif  // XMCP_TOOLS_INTERNAL_H
