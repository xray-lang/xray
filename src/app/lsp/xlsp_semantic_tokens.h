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
#include "xlsp_json.h"

// LSP Semantic Token Types (indices into legend)
typedef enum {
    XLSP_TOKEN_NAMESPACE = 0,
    XLSP_TOKEN_TYPE,
    XLSP_TOKEN_CLASS,
    XLSP_TOKEN_ENUM,
    XLSP_TOKEN_INTERFACE,
    XLSP_TOKEN_STRUCT,
    XLSP_TOKEN_TYPE_PARAMETER,
    XLSP_TOKEN_PARAMETER,
    XLSP_TOKEN_VARIABLE,
    XLSP_TOKEN_PROPERTY,
    XLSP_TOKEN_ENUM_MEMBER,
    XLSP_TOKEN_EVENT,
    XLSP_TOKEN_FUNCTION,
    XLSP_TOKEN_METHOD,
    XLSP_TOKEN_MACRO,
    XLSP_TOKEN_KEYWORD,
    XLSP_TOKEN_MODIFIER,
    XLSP_TOKEN_COMMENT,
    XLSP_TOKEN_STRING,
    XLSP_TOKEN_NUMBER,
    XLSP_TOKEN_REGEXP,
    XLSP_TOKEN_OPERATOR,
    XLSP_TOKEN_COUNT
} XlspSemanticTokenType;

// LSP Semantic Token Modifiers (bit flags)
typedef enum {
    XLSP_MOD_DECLARATION = 1 << 0,
    XLSP_MOD_DEFINITION = 1 << 1,
    XLSP_MOD_READONLY = 1 << 2,
    XLSP_MOD_STATIC = 1 << 3,
    XLSP_MOD_DEPRECATED = 1 << 4,
    XLSP_MOD_ABSTRACT = 1 << 5,
    XLSP_MOD_ASYNC = 1 << 6,
    XLSP_MOD_MODIFICATION = 1 << 7,
    XLSP_MOD_DOCUMENTATION = 1 << 8,
    XLSP_MOD_DEFAULT_LIBRARY = 1 << 9
} XlspSemanticTokenModifier;

// Semantic token entry
typedef struct XlspSemanticToken {
    int line;           // 0-indexed
    int start_char;     // 0-indexed
    int length;
    XlspSemanticTokenType type;
    int modifiers;      // Bit flags
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

#endif // XLSP_SEMANTIC_TOKENS_H
