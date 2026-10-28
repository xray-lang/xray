/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_source.h - Per-request owned Source facts for MCP consumers
 *
 * KEY CONCEPT:
 *   Memory source checking uses the ordinary semantic owner and explicit controlled
 *   memory owner. Query facts never grant execution authority.
 */
#ifndef XMCP_SOURCE_H
#define XMCP_SOURCE_H
#include "../../xir/xxir_source.h"
#include "../../base/xcompile_request.h"

typedef struct XmcpSourceRequest {
    const char *text;
    size_t length;
    const char *module_id;
    const XrXirCompileContext *context;
    XrCompileRequestControl *control; /* Synchronous borrow; never retained in query facts. */
} XmcpSourceRequest;
typedef struct XmcpSourceOwner {
    XrXirSourceResult result;
    XrXirSourceDiagnosticFact diagnostic;
    XrXirSourceFailure failure;
} XmcpSourceOwner;

XR_FUNC XrCompileResourceLimits xmcp_source_default_limits(void);
XR_FUNC XrXirStatus xmcp_source_resource_status(XrCompileResourceStatus status);
/* The caller supplies an empty owner. Valid checks retain failure diagnostics
 * until free; successful facts outlive the temporary Session and input bytes. */
XR_FUNC XrXirStatus xmcp_source_check(const XmcpSourceRequest *request, XmcpSourceOwner *output);
XR_FUNC void xmcp_source_free(XmcpSourceOwner *owner);
#endif // XMCP_SOURCE_H
