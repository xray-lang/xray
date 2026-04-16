/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_extra_handlers.h - Additional LSP handlers
 *
 * KEY CONCEPT:
 *   Handlers for document highlight, workspace symbol,
 *   selection range, and document link.
 */

#ifndef XLSP_EXTRA_HANDLERS_H
#define XLSP_EXTRA_HANDLERS_H

#include "xlsp_server.h"
#include "xlsp_json.h"

XR_FUNC XrJsonValue *xlsp_handle_document_highlight(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_workspace_symbol(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_selection_range(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_document_link(XrLspServer *server, XrJsonValue *params);

#endif // XLSP_EXTRA_HANDLERS_H
