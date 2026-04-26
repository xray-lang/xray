/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfmt_literal.h - String / template-string serialisation
 *
 * KEY CONCEPT:
 *   Round-trippable formatter output for the three string-bearing AST
 *   shapes:
 *
 *     AST_LITERAL_STRING        decoded payload  -> "..." (re-escaped)
 *     AST_TEMPLATE_STRING       parts[]          -> "..." with ${expr}
 *     (raw style — see note)    decoded payload  -> "..." (canonical)
 *
 *   Background:
 *     The lexer accepts `"..."` and `'...'` (regular) and `r"..."`
 *     (raw); backtick-template strings are deprecated. Once parsed,
 *     the AST does NOT distinguish raw from non-raw — the parser
 *     decodes escape sequences and stores the canonical decoded
 *     bytes in LiteralNode.raw_value.string_val. Therefore raw style
 *     CANNOT be preserved by the formatter, and per the project
 *     principle (canonical form > lexeme preservation) raw strings
 *     are emitted as ordinary double-quoted strings.
 *
 *   An earlier formatter wrote raw_value verbatim between two
 *   `"` characters and emitted templates between backticks. Both
 *   produced source the lexer rejects: a string containing a quote,
 *   a backslash, a newline, or a control byte became a syntax error,
 *   and backtick templates simply do not lex any more. This module
 *   replaces that with proper re-escaping.
 */

#ifndef XFMT_LITERAL_H
#define XFMT_LITERAL_H

#include "xfmt.h"
#include "../../base/xdefs.h"

// Callback used by xfmt_emit_template_string to recurse into
// interpolation expressions. The formatter passes its own fmt_expression.
typedef void (*XrFmtExprEmitter)(XrFmtContext *ctx, AstNode *expr);

// Emit `value` (`len` bytes) as a properly-escaped double-quoted string
// literal. `len` is honoured even past embedded NUL bytes.
//
// Re-escape rules (matches xr_process_escapes):
//   '"'   -> \"
//   '\\'  -> \\
//   '\n'  -> \n     '\r' -> \r     '\t' -> \t
//   '\b'  -> \b     '\f' -> \f
//   '\0'  -> \0
//   other byte < 0x20 -> \xHH      (hex escape)
//   everything else                -> verbatim
XR_FUNC void xfmt_emit_string(XrFmtContext *ctx, const char *value, int len);

// Emit an AST_TEMPLATE_STRING node as a canonical double-quoted
// template, e.g. "Hello, ${name}!".
//
// Literal parts are re-escaped via xfmt_emit_string's escape table AND
// every `$` byte is escaped as `\$` so that no literal substring can
// accidentally form a `${` opener at re-parse time.
//
// `emit_expr` is invoked for each non-literal part (the interpolation
// expression). It must emit the expression source on `ctx`.
XR_FUNC void xfmt_emit_template_string(XrFmtContext *ctx, AstNode *node,
                                       XrFmtExprEmitter emit_expr);

// Emit `value` as a properly-escaped ordinary double-quoted string.
// Currently a thin wrapper around xfmt_emit_string because the AST
// has no raw flag; kept as a separate API so callers convey intent
// (and so the canonicalisation can change implementation later
// without disturbing call sites).
XR_FUNC void xfmt_emit_raw_string(XrFmtContext *ctx, const char *value, int len);

#endif  // XFMT_LITERAL_H
