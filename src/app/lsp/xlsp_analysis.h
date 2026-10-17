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
#include "../../base/xjson.h"
#include "xlsp_completion.h"

typedef struct XrCompilerSession XrCompilerSession;
typedef struct XrModuleGraph XrModuleGraph;

/* Clear the analyzer's borrowed graph and release the server-owned graph and
 * authority. Safe to call during server teardown or analyzer replacement. */
XR_FUNC void xlsp_analysis_release_module_graph(XrLspServer *server);

// Generate diagnostics for a document
XR_FUNC XrJsonValue *xlsp_analyze_diagnostics(XrLspDocument *doc);

// Generate hover info at position
XR_FUNC XrJsonValue *xlsp_analyze_hover(XrLspServer *server, XrLspDocument *doc, XrLspPosition pos);

// Generate document symbols (outline)
XR_FUNC XrJsonValue *xlsp_analyze_document_symbols(XrLspDocument *doc);

// Navigation (definition, references, highlight) moved to xlsp_navigation.h
#include "xlsp_navigation.h"

// Parse document and cache AST (call after document open/change)
// Owns the actual Source syntax snapshot for this document version.
XR_FUNC void xlsp_parse_document(XrLspDocument *doc, XrLspServer *server);

/* Release the module graph built for the last parsed document, clearing the
 * analyzer's borrowed pointer to it first. Call before tearing the analyzer
 * down: the graph outlives a single parse on purpose, so nothing else frees
 * it at shutdown. */

// Format (moved to xlsp_format.h / xlsp_format.c)
#include "xlsp_format.h"

// Signature help at position
XR_FUNC XrJsonValue *xlsp_analyze_signature_help(XrLspDocument *doc, XrLspPosition pos);

// Word at cursor position (shared utility to avoid code duplication)
// Returns allocated word string, caller must free. Sets start/end offsets.
// Returns NULL if no word at position.
XR_FUNC char *xlsp_word_at_position(XrLspDocument *doc, XrLspPosition pos, uint32_t *out_start,
                                    uint32_t *out_end);

#endif  // XLSP_ANALYSIS_H
