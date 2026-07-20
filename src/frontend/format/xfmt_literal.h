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
 *   The AST retains value lane, escape mode, and inline/block form. The
 *   formatter preserves those facts and chooses the smallest safe block fence.
 */

#ifndef XFMT_LITERAL_H
#define XFMT_LITERAL_H

#include "xfmt.h"
#include "../../base/xdefs.h"

// Callback used by xfmt_emit_template_string to recurse into
// interpolation expressions. The formatter passes its own fmt_expression.
typedef void (*XrFmtExprEmitter)(XrFmtContext *ctx, AstNode *expr);

XR_FUNC void xfmt_emit_escaped_inline_string(XrFmtContext *ctx, const char *value, int length);
XR_FUNC void xfmt_emit_string_literal(XrFmtContext *ctx, AstNode *node);
XR_FUNC void xfmt_emit_fixed_bytes_literal(XrFmtContext *ctx, AstNode *node);
XR_FUNC void xfmt_emit_template_string(XrFmtContext *ctx, AstNode *node,
                                       XrFmtExprEmitter emit_expr);

#endif  // XFMT_LITERAL_H
