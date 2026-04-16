/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_analysis.h - Code analysis for LSP features
 *
 * KEY CONCEPT:
 *   Provides diagnostics, completion, hover by analyzing xray source code.
 */

#ifndef XLSP_ANALYSIS_H
#define XLSP_ANALYSIS_H

#include "xlsp_types.h"
#include "xlsp_server.h"
#include "xlsp_json.h"
#include "xlsp_completion.h"

// Generate diagnostics for a document
XR_FUNC XrJsonValue *xlsp_analyze_diagnostics(XrLspDocument *doc);

// Generate hover info at position
XR_FUNC XrJsonValue *xlsp_analyze_hover(XrLspServer *server, XrLspDocument *doc, XrLspPosition pos);

// Generate document symbols (outline)
XR_FUNC XrJsonValue *xlsp_analyze_document_symbols(XrLspDocument *doc);

// Go to definition
XR_FUNC XrJsonValue *xlsp_analyze_definition(XrLspServer *server, XrLspDocument *doc, XrLspPosition pos);

// Find all references (cross-file)
XR_FUNC XrJsonValue *xlsp_analyze_references(XrLspServer *server, XrLspDocument *doc, XrLspPosition pos);

// Document highlight (scope-aware, single-file)
XR_FUNC XrJsonValue *xlsp_analyze_document_highlight(XrLspServer *server, XrLspDocument *doc, XrLspPosition pos);

// Parse document and cache AST (call after document open/change)
// Uses server->workspace_analyzer for cross-file analysis
XR_FUNC void xlsp_parse_document(XrLspDocument *doc, XrLspServer *server);

// Rename symbol at position (cross-file)
// Returns WorkspaceEdit JSON or NULL if rename not possible
XR_FUNC XrJsonValue *xlsp_analyze_rename(XrLspServer *server, XrLspDocument *doc, 
                                  XrLspPosition pos, const char *new_name);

// Prepare rename (check if rename is valid at position)
// Returns Range JSON or NULL if rename not valid
XR_FUNC XrJsonValue *xlsp_analyze_prepare_rename(XrLspDocument *doc, XrLspPosition pos);

// Format document
// Returns array of TextEdit
XR_FUNC XrJsonValue *xlsp_analyze_format(XrLspDocument *doc);

// Format range in document
XR_FUNC XrJsonValue *xlsp_analyze_format_range(XrLspDocument *doc, XrLspRange range);

// Signature help at position
XR_FUNC XrJsonValue *xlsp_analyze_signature_help(XrLspDocument *doc, XrLspPosition pos);

// Word at cursor position (shared utility to avoid code duplication)
// Returns allocated word string, caller must free. Sets start/end offsets.
// Returns NULL if no word at position.
XR_FUNC char *xlsp_word_at_position(XrLspDocument *doc, XrLspPosition pos,
                            uint32_t *out_start, uint32_t *out_end);

#endif // XLSP_ANALYSIS_H
