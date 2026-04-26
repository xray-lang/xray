/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfmt.h - AST-based code formatter
 *
 * KEY CONCEPT:
 *   Formats Xray source code by parsing to AST and regenerating
 *   with consistent style. Preserves semantic meaning including
 *   space-sensitive generic syntax.
 */

#ifndef XFMT_H
#define XFMT_H

#include "../parser/xast.h"

// Forward declaration: the formatter only stores an XrayIsolate pointer
// in XrFmtContext (used by xtype printing). Pulling in xray_isolate.h
// here would couple the frontend to the public API header.
typedef struct XrayIsolate XrayIsolate;

// Format configuration
typedef struct XrFmtConfig {
    int indent_size;        // Spaces per indent level (default: 4)
    int use_tabs;           // Use tabs instead of spaces
    int max_line_length;    // Max line length hint (default: 100)
    int trailing_newline;   // Ensure trailing newline at EOF
    int blank_lines_around_functions;  // Blank lines around functions
    int blank_lines_around_classes;    // Blank lines around classes
    int space_around_operators;        // Space around binary operators
    int space_after_comma;             // Space after comma
    int space_in_parentheses;          // Space inside parentheses
    int brace_same_line;               // Opening brace on same line
} XrFmtConfig;

// Default configuration
extern XrFmtConfig xfmt_default_config;

// Format context
typedef struct XrFmtContext {
    char *output;           // Output buffer
    size_t capacity;        // Buffer capacity
    size_t length;          // Current length
    int indent_level;       // Current indent level
    int line_start;         // At line start flag
    int column;             // Current column (for line length tracking)
    XrFmtConfig *config;    // Configuration
    XrayIsolate *X;         // Isolate for type printing
} XrFmtContext;

// Initialize formatter context
XR_FUNC void xfmt_init(XrFmtContext *ctx, XrFmtConfig *config, XrayIsolate *X);

// Free formatter context
XR_FUNC void xfmt_free(XrFmtContext *ctx);

// Format AST to string
// Returns newly allocated string (caller must free)
XR_FUNC char *xfmt_format_ast(AstNode *ast, XrFmtConfig *config, XrayIsolate *X);

// Format a single AST node
XR_FUNC void xfmt_node(XrFmtContext *ctx, AstNode *node);

// Format type annotation
XR_FUNC void xfmt_type(XrFmtContext *ctx, XrType *type);

#endif // XFMT_H
