/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_ast_utils.h - Shared AST utility functions for LSP
 *
 * KEY CONCEPT:
 *   Common AST traversal helpers used by multiple LSP modules
 *   (analysis, folding, call hierarchy, etc.)
 */

#ifndef XLSP_AST_UTILS_H
#define XLSP_AST_UTILS_H

#include <stdbool.h>
#include "../../base/xdefs.h"

typedef struct AstNode AstNode;

// Find the last line of an AST node recursively
// Handles: block, function, class, method, if, while, for, try/catch, match
XR_FUNC int xlsp_get_node_end_line(AstNode *node);

// Check if character is valid in an identifier
static inline bool xlsp_is_ident_char(char c) {
    return c == '_' ||
           (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

#endif // XLSP_AST_UTILS_H
