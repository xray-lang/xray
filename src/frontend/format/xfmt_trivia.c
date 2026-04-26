/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfmt_trivia.c - Comment / trivia output for the formatter
 *
 * KEY CONCEPT:
 *   Emits leading and trailing comments attached to AST nodes by the
 *   trivia-aware parser.
 *
 *     - Leading comments  : block / line comments that appeared on
 *                           lines preceding the node. Emitted before
 *                           the node, each followed by a newline.
 *     - Trailing comments : a single inline comment on the same line
 *                           as the node's closing token (L-06). The
 *                           formatter rewinds the node's terminating
 *                           newline, appends "  // ..." (two-space
 *                           gap), and re-emits the newline.
 *
 *   Block comments preserve their /-star ... star-/ form; line
 *   comments preserve their // form.
 */

#include "xfmt_internal.h"

static void xfmt_write_trivia_body(XrFmtContext *ctx, XrTrivia *trivia) {
    if (trivia->type == TRIVIA_LINE_COMMENT) {
        xfmt_write_str(ctx, "//");
        for (int i = 0; i < trivia->length; i++) {
            xfmt_write_char(ctx, trivia->start[i]);
        }
    } else if (trivia->type == TRIVIA_BLOCK_COMMENT) {
        xfmt_write_str(ctx, "/*");
        for (int i = 0; i < trivia->length; i++) {
            xfmt_write_char(ctx, trivia->start[i]);
        }
        xfmt_write_str(ctx, "*/");
    }
}

void xfmt_write_leading_comments(XrFmtContext *ctx, XrTrivia *trivia) {
    while (trivia) {
        xfmt_write_indent(ctx);
        xfmt_write_trivia_body(ctx, trivia);
        xfmt_write_newline(ctx);
        trivia = trivia->next;
    }
}

void xfmt_write_trailing_comment(XrFmtContext *ctx, XrTrivia *trivia) {
    XR_DCHECK(ctx != NULL, "xfmt_write_trailing_comment: NULL ctx");
    if (!trivia)
        return;

    // L-06: the lexer attaches at most one inline comment per token; we
    // intentionally emit only the first entry as trailing. A chain
    // longer than one would mean the lexer started returning multiple
    // -- treat that as a contract violation worth catching.
    XR_DCHECK(trivia->next == NULL, "xfmt_write_trailing_comment: lexer returned more than one "
                                    "inline trailing trivia entry");

    // Rewind the just-emitted terminating newline so the inline
    // comment lives on the same source line as the node body. If the
    // caller's emit path didn't end in '\n' (rare -- e.g. a one-line
    // statement at EOF without trailing newline) we just append the
    // comment in place.
    bool had_newline = ctx->length > 0 && ctx->output[ctx->length - 1] == '\n';
    if (had_newline) {
        ctx->length--;
        ctx->output[ctx->length] = '\0';
        ctx->line_start = 0;
    }

    // Two-space gap matches Rust / Go convention; using a constant
    // keeps the formatter idempotent (round-trip stable).
    xfmt_write_str(ctx, "  ");
    xfmt_write_trivia_body(ctx, trivia);

    if (had_newline)
        xfmt_write_newline(ctx);
}
