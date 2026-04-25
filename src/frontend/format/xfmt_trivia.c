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
 *   Emits leading comments attached to AST nodes by the trivia-aware
 *   parser. Block comments preserve their /-star ... star-/ form;
 *   line comments preserve their // form. Each comment is followed by
 *   a newline so it never bleeds into the following statement.
 */

#include "xfmt_internal.h"

void xfmt_write_leading_comments(XrFmtContext *ctx, XrTrivia *trivia) {
    while (trivia) {
        xfmt_write_indent(ctx);

        if (trivia->type == TRIVIA_LINE_COMMENT) {
            xfmt_write_str(ctx, "//");
            for (int i = 0; i < trivia->length; i++) {
                xfmt_write_char(ctx, trivia->start[i]);
            }
            xfmt_write_newline(ctx);
        } else if (trivia->type == TRIVIA_BLOCK_COMMENT) {
            xfmt_write_str(ctx, "/*");
            for (int i = 0; i < trivia->length; i++) {
                xfmt_write_char(ctx, trivia->start[i]);
            }
            xfmt_write_str(ctx, "*/");
            xfmt_write_newline(ctx);
        }

        trivia = trivia->next;
    }
}
