/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfmt.c - AST-based code formatter (entry, config, buffer, public API)
 *
 * KEY CONCEPT:
 *   The formatter is split into thematic translation units; this file
 *   keeps only the per-context buffer plumbing and the three public
 *   entry points (xfmt_init / xfmt_free / xfmt_format_ast plus the
 *   xfmt_node and xfmt_type dispatchers). All AST-shape formatting
 *   lives in xfmt_expr.c / xfmt_stmt.c / xfmt_decl.c / xfmt_type.c /
 *   xfmt_trivia.c / xfmt_literal.c. Cross-TU helpers are declared in
 *   xfmt_internal.h.
 */

#include "xfmt.h"
#include "xfmt_internal.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// Default configuration
// ============================================================================

XrFmtConfig xfmt_default_config = {.indent_size = 4,
                                   .use_tabs = 0,
                                   .max_line_length = 100,
                                   .trailing_newline = 1,
                                   .blank_lines_around_functions = 1,
                                   .blank_lines_around_classes = 1,
                                   .space_around_operators = 1,
                                   .space_after_comma = 1,
                                   .space_in_parentheses = 0,
                                   .brace_same_line = 1};

// ============================================================================
// Buffer helpers (cross-TU; declared in xfmt_internal.h)
// ============================================================================

void xfmt_ensure_capacity(XrFmtContext *ctx, size_t additional) {
    if (ctx->length + additional >= ctx->capacity) {
        ctx->capacity = (ctx->capacity + additional) * 2;
        ctx->output = (char *) xr_realloc(ctx->output, ctx->capacity);
    }
}

void xfmt_write_char(XrFmtContext *ctx, char c) {
    xfmt_ensure_capacity(ctx, 1);
    ctx->output[ctx->length++] = c;
    ctx->output[ctx->length] = '\0';

    if (c == '\n') {
        ctx->line_start = 1;
        ctx->column = 0;
    } else {
        ctx->column++;
    }
}

void xfmt_write_str(XrFmtContext *ctx, const char *str) {
    if (!str)
        return;
    size_t len = strlen(str);
    if (len == 0)
        return;
    xfmt_ensure_capacity(ctx, len);
    memcpy(ctx->output + ctx->length, str, len);
    ctx->length += len;
    ctx->output[ctx->length] = '\0';
    // Update line/column tracking
    const char *last_nl = NULL;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\n')
            last_nl = str + i;
    }
    if (last_nl) {
        ctx->line_start = 1;
        ctx->column = (int) (str + len - 1 - last_nl);
    } else {
        ctx->column += (int) len;
    }
}

void xfmt_write_fmt(XrFmtContext *ctx, const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    xfmt_write_str(ctx, buf);
}

void xfmt_write_indent(XrFmtContext *ctx) {
    if (!ctx->line_start)
        return;

    if (ctx->config->use_tabs) {
        for (int i = 0; i < ctx->indent_level; i++) {
            xfmt_write_char(ctx, '\t');
        }
    } else {
        int spaces = ctx->indent_level * ctx->config->indent_size;
        for (int i = 0; i < spaces; i++) {
            xfmt_write_char(ctx, ' ');
        }
    }
    ctx->line_start = 0;
}

void xfmt_write_newline(XrFmtContext *ctx) {
    xfmt_write_char(ctx, '\n');
}

void xfmt_write_space(XrFmtContext *ctx) {
    xfmt_write_char(ctx, ' ');
}

// ============================================================================
// Initialize / Free
// ============================================================================

void xfmt_init(XrFmtContext *ctx, XrFmtConfig *config, XrayIsolate *X) {
    ctx->capacity = 4096;
    ctx->output = (char *) xr_malloc(ctx->capacity);
    XR_DCHECK(ctx->output != NULL, "xfmt: output buffer allocation failed");
    ctx->output[0] = '\0';
    ctx->length = 0;
    ctx->indent_level = 0;
    ctx->line_start = 1;
    ctx->column = 0;
    ctx->config = config ? config : &xfmt_default_config;
    ctx->X = X;
}

void xfmt_free(XrFmtContext *ctx) {
    if (ctx->output) {
        xr_free(ctx->output);
        ctx->output = NULL;
    }
}

// ============================================================================
// Public API
// ============================================================================

void xfmt_node(XrFmtContext *ctx, AstNode *node) {
    if (!node)
        return;

    if (node->type == AST_PROGRAM) {
        xfmt_emit_program(ctx, node);
    } else {
        xfmt_emit_statement(ctx, node);
    }
}

void xfmt_type(XrFmtContext *ctx, XrType *type) {
    xfmt_emit_type(ctx, type);
}

char *xfmt_format_ast(AstNode *ast, XrFmtConfig *config, XrayIsolate *X) {
    XrFmtContext ctx;
    xfmt_init(&ctx, config, X);

    xfmt_node(&ctx, ast);

    if (config && config->trailing_newline && ctx.length > 0 &&
        ctx.output[ctx.length - 1] != '\n') {
        xfmt_write_char(&ctx, '\n');
    }

    char *result = ctx.output;
    ctx.output = NULL;
    return result;
}
