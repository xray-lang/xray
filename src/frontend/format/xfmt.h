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

#include "../../base/xcompile_state.h"

// Format configuration
typedef struct XrFmtConfig {
    int indent_size;                   // Spaces per indent level (default: 4)
    int use_tabs;                      // Use tabs instead of spaces
    int max_line_length;               // Max line length hint (default: 100)
    int trailing_newline;              // Ensure trailing newline at EOF
    int blank_lines_around_functions;  // Blank lines around functions
    int blank_lines_around_classes;    // Blank lines around classes
    int space_around_operators;        // Space around binary operators
    int space_after_comma;             // Space after comma
    int space_in_parentheses;          // Space inside parentheses
    int brace_same_line;               // Opening brace on same line

    // ---- Column alignment ----
    int align_branch_arrows;      // Column-align `->` of match/select branch arms
    int align_enum_values;        // Column-align `=` of enum members
    int align_struct_fields;      // Column-align `:` of class/struct/iface fields
    int align_trailing_comments;  // Column-align `//` of consecutive trailing line comments

    // ---- Long-line wrapping (off by default; turn on to enforce max_line_length) ----
    int wrap_long_lines;           // Break literals/calls that exceed max_line_length
    int multiline_trailing_comma;  // When wrapping to multi-line, emit trailing `,`
} XrFmtConfig;

// Default configuration
extern XrFmtConfig xfmt_default_config;

typedef enum XrFmtStatus {
    XR_FMT_OK, XR_FMT_BAD_ARGUMENT, XR_FMT_BUDGET, XR_FMT_OUT_OF_MEMORY
} XrFmtStatus;
typedef struct XrFmtOutput { char *text; size_t length; } XrFmtOutput;

/* Synchronously borrows syntax and configuration. Only success publishes an
 * independent ledger allocation into an empty output. A program arena must
 * belong to the exact state; borrowed non-program nodes remain caller-owned. */
XR_FUNC XrFmtStatus xr_compile_format_ast(XrCompileState *state, AstNode *ast,
    const XrFmtConfig *config, XrFmtOutput *output);
/* Unconditional physical cleanup; no work admission or allocation. */
XR_FUNC void xr_compile_format_output_free(XrFmtOutput *output);

#endif // XFMT_H
