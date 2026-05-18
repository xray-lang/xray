/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfmt_internal.h - Cross-TU helpers shared by formatter modules
 *
 * KEY CONCEPT:
 *   xfmt.c was previously a 1700+ line single-file implementation.
 *   It is now split into thematic modules:
 *
 *       xfmt.c         entry / config / init / public API
 *       xfmt_trivia.c  leading / trailing comment output
 *       xfmt_type.c    type, generic-args, destructure-pattern, op strings
 *       xfmt_expr.c    expression-shaped AST nodes
 *       xfmt_stmt.c    statement / control-flow / program
 *       xfmt_decl.c    var / func / class / interface / enum / type alias
 *       xfmt_literal.c re-escaping string / template emission
 *
 *   This header declares the few helpers that need to cross those
 *   translation units. All declarations carry XR_FUNC and use the
 *   xfmt_ prefix. It is NOT a public formatter API and must not be
 *   included from outside src/frontend/format/.
 */

#ifndef XFMT_INTERNAL_H
#define XFMT_INTERNAL_H

#include "xfmt.h"
#include "../parser/xast.h"
#include "../../base/xdefs.h"

// ---------------------------------------------------------------------------
// Buffer helpers (defined in xfmt.c)
// ---------------------------------------------------------------------------

XR_FUNC void xfmt_ensure_capacity(XrFmtContext *ctx, size_t additional);
XR_FUNC void xfmt_write_char(XrFmtContext *ctx, char c);
XR_FUNC void xfmt_write_str(XrFmtContext *ctx, const char *str);
XR_FUNC void xfmt_write_fmt(XrFmtContext *ctx, const char *fmt, ...) XR_PRINTF_FMT(2, 3);
XR_FUNC void xfmt_write_indent(XrFmtContext *ctx);
XR_FUNC void xfmt_write_newline(XrFmtContext *ctx);
XR_FUNC void xfmt_write_space(XrFmtContext *ctx);

// ---------------------------------------------------------------------------
// Snapshot / rollback (for long-line wrapping: try single-line emit, fall
// back to multi-line when it overflows or contains a newline).
// ---------------------------------------------------------------------------

typedef struct XfmtSnapshot {
    size_t length;
    int column;
    int line_start;
    int indent_level;
    int in_template_expr;
} XfmtSnapshot;

XR_FUNC void xfmt_snapshot(XrFmtContext *ctx, XfmtSnapshot *snap);
XR_FUNC void xfmt_rollback(XrFmtContext *ctx, const XfmtSnapshot *snap);

// Returns true if the region written since `snap` contains no newline AND
// the current column is within max_line_length. Use to decide whether the
// just-emitted single-line attempt fits, or must rollback to multi-line.
XR_FUNC bool xfmt_fits_on_line(XrFmtContext *ctx, const XfmtSnapshot *snap);

// ---------------------------------------------------------------------------
// Trivia (defined in xfmt_trivia.c)
// ---------------------------------------------------------------------------

XR_FUNC void xfmt_write_leading_comments(XrFmtContext *ctx, XrTrivia *trivia);

// L-06: emit a single inline trailing comment by rewinding the
// just-written terminating newline and re-emitting it after the
// comment body. Caller must invoke this AFTER the node's content
// (and its terminating newline, if any) has been written.
XR_FUNC void xfmt_write_trailing_comment(XrFmtContext *ctx, XrTrivia *trivia);

// ---------------------------------------------------------------------------
// Type / generic / destructure-pattern / operator-string helpers
// (defined in xfmt_type.c)
// ---------------------------------------------------------------------------

XR_FUNC void xfmt_emit_type(XrFmtContext *ctx, XrTypeRef *tref);
XR_FUNC void xfmt_emit_generic_params(XrFmtContext *ctx, XrGenericParam **params, int count);
XR_FUNC void xfmt_emit_generic_args(XrFmtContext *ctx, XrTypeRef **args, int count);
XR_FUNC void xfmt_emit_pattern(XrFmtContext *ctx, XrDestructurePattern *pattern);
XR_FUNC const char *xfmt_binary_op(AstNodeType type);
XR_FUNC const char *xfmt_compound_op(XrTokenType type);

// ---------------------------------------------------------------------------
// AST dispatchers (cross-TU recursion targets)
// ---------------------------------------------------------------------------

// xfmt_expr.c
XR_FUNC void xfmt_emit_expression(XrFmtContext *ctx, AstNode *node);

// xfmt_stmt.c
XR_FUNC void xfmt_emit_statement(XrFmtContext *ctx, AstNode *node);
XR_FUNC void xfmt_emit_block(XrFmtContext *ctx, AstNode *node);
XR_FUNC void xfmt_emit_program(XrFmtContext *ctx, AstNode *node);

// xfmt_decl.c
XR_FUNC void xfmt_emit_var_decl(XrFmtContext *ctx, AstNode *node);
XR_FUNC void xfmt_emit_destructure_decl(XrFmtContext *ctx, AstNode *node);
XR_FUNC void xfmt_emit_function_decl(XrFmtContext *ctx, AstNode *node);
XR_FUNC void xfmt_emit_class_decl(XrFmtContext *ctx, AstNode *node);
XR_FUNC void xfmt_emit_interface_decl(XrFmtContext *ctx, AstNode *node);
XR_FUNC void xfmt_emit_enum_decl(XrFmtContext *ctx, AstNode *node);
XR_FUNC void xfmt_emit_type_alias(XrFmtContext *ctx, AstNode *node);

#endif  // XFMT_INTERNAL_H
