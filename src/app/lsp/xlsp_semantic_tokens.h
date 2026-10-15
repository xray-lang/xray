/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_semantic_tokens.h - Semantic tokens for syntax highlighting
 *
 * KEY CONCEPT:
 *   Provides rich syntax highlighting based on semantic analysis.
 *   Token types: variable, function, class, parameter, property, etc.
 */

#ifndef XLSP_SEMANTIC_TOKENS_H
#define XLSP_SEMANTIC_TOKENS_H

#include "xlsp_server.h"
#include "../../base/xjson.h"

#include "xlsp_semantic_legend.h"

// Semantic token entry
typedef struct XlspSemanticToken {
    int line;        // 0-indexed
    int start_char;  // 0-indexed
    int length;
    XlspSemanticTokenType type;
    int modifiers;  // Bit flags
} XlspSemanticToken;

// Semantic tokens result
typedef struct XlspSemanticTokensResult {
    XlspSemanticToken *tokens;
    int count;
    int capacity;
} XlspSemanticTokensResult;

// Get semantic tokens legend (for capabilities)
XR_FUNC XrJsonValue *xlsp_semantic_tokens_legend(void);

// Analyze document for semantic tokens
XR_FUNC XlspSemanticTokensResult *xlsp_analyze_semantic_tokens(XrLspDocument *doc);

// Free semantic tokens result
XR_FUNC void xlsp_semantic_tokens_free(XlspSemanticTokensResult *result);

// Encode tokens to LSP format (delta encoding)
XR_FUNC XrJsonValue *xlsp_semantic_tokens_encode(XlspSemanticTokensResult *result);

// Encode tokens to raw uint32_t array (for caching/delta comparison)
// Returns malloc'd array, caller owns. Sets *out_count to number of uint32_t values.
XR_FUNC uint32_t *xlsp_semantic_tokens_encode_raw(XlspSemanticTokensResult *result, int *out_count);

#endif  // XLSP_SEMANTIC_TOKENS_H
