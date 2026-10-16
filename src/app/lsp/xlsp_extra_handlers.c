#include "xlsp_navigation.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_extra_handlers.c - Additional LSP handlers
 */

#include "xlsp_extra_handlers.h"
#include "xlsp_links.h"
#include "xlsp_source_index_store.h"
#include "xlsp_index_json.h"
#include "xlsp_hierarchy_request.h"
#include "xlsp_workspace_symbols.h"
#include "../../api/xsource_compile.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "../../base/xmalloc.h"

// ============================================================================
// Document Highlight
// ============================================================================

XrJsonValue *xlsp_handle_document_highlight(XrLspServer *server, XrJsonValue *params) {
    return xlsp_navigation_handle(server, params, 2);
}

// ============================================================================
// Workspace Symbol
// ============================================================================

XR_FUNC XrJsonValue *xlsp_handle_workspace_symbol(XrLspServer *server,XrJsonValue *params) {
    return xlsp_workspace_symbols_handle(server,params);
}

// ============================================================================
// Selection Range
// ============================================================================

XR_FUNC XrJsonValue *xlsp_handle_selection_range(XrLspServer *server,XrJsonValue *params) {
    return xlsp_source_scope_query(server,params,0,XLSP_SOURCE_SELECTION);
}

// ============================================================================
// Document Link
// ============================================================================

XrJsonValue *xlsp_handle_document_link(XrLspServer *server,XrJsonValue *params) {
    return xlsp_links_handle(server,params);
}
