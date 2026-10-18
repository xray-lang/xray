/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_run_source.h - SourceProduct admission under the MCP import policy
 *
 * KEY CONCEPT:
 *   One caller ledger checks imports and builds the ordinary owned product.
 */
#ifndef XMCP_RUN_SOURCE_H
#define XMCP_RUN_SOURCE_H
#include "xmcp_source.h"
#include "../../program/xr_xir_source_product.h"

/* Synchronous input borrow. Output must be empty; failures never publish code.
 * Product/diagnostic ownership is exactly the ordinary SourceProduct contract. */
XR_FUNC XrXirStatus xmcp_run_source_product(const XmcpSourceRequest *request,
    XrXirSourceProduct **output, XrXirSourceProductDiagnostic *diagnostic);
#endif // XMCP_RUN_SOURCE_H
