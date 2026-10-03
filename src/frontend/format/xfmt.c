/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfmt.c - One metered formatter output owner
 */
#include "xfmt_internal.h"
#include "../../toolchain/xcompiler_arena_backing.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

XrFmtConfig xfmt_default_config = {.indent_size = 4,
                                   .use_tabs = 0,
                                   .max_line_length = 100,
                                   .trailing_newline = 1,
                                   .blank_lines_around_functions = 1,
                                   .blank_lines_around_classes = 1,
                                   .space_around_operators = 1,
                                   .space_after_comma = 1,
                                   .space_in_parentheses = 0,
                                   .brace_same_line = 1,
                                   .align_branch_arrows = 1,
                                   .align_enum_values = 0,
                                   .align_struct_fields = 0,
                                   .align_trailing_comments = 0,
                                   .wrap_long_lines = 0,
                                   .multiline_trailing_comma = 1};

XR_FUNC bool xfmt_healthy(const XrFmtContext *ctx) {
    return ctx && xr_compile_state_status(ctx->state) == XR_COMPILE_RESOURCE_OK;
}
XR_FUNC bool xfmt_work(XrFmtContext *ctx, uint64_t units) {
    return xr_compile_state_work(ctx->state, units) == XR_COMPILE_RESOURCE_OK;
}
XR_FUNC bool xfmt_step(XrFmtContext *ctx) { return xfmt_work(ctx, 1); }
XR_FUNC void xfmt_fail(XrFmtContext *ctx, XrCompileResourceStatus status) {
    (void)xr_compile_state_fail(ctx->state, status);
}
XR_FUNC void *xfmt_alloc_array(XrFmtContext *ctx, size_t count, size_t size) {
    void *memory = NULL;
    if (!count || !size || count > SIZE_MAX / size) {
        xfmt_fail(ctx, XR_COMPILE_RESOURCE_BUDGET);
        return NULL;
    }
    (void)xr_compile_state_alloc(ctx->state, count * size, &memory);
    return memory;
}
XR_FUNC size_t xfmt_length(XrFmtContext *ctx, const char *text) {
    size_t length = 0;
    if (text) (void)xr_compile_state_string_length(ctx->state, text, &length);
    /* Reserve the largest fixed declaration prefix before signed alignment math. */
    if (length > INT_MAX - 16u) {
        xfmt_fail(ctx, XR_COMPILE_RESOURCE_BUDGET);
        return 0;
    }
    return length;
}
XR_FUNC int xfmt_compare(XrFmtContext *ctx, const char *left, const char *right) {
    for (;;) {
        if (!xfmt_work(ctx, 2)) return 1;
        unsigned char a = (unsigned char)*left++, b = (unsigned char)*right++;
        if (a != b || !a) return (a > b) - (a < b);
    }
}
XR_FUNC bool xfmt_ensure_capacity(XrFmtContext *ctx, size_t additional) {
    if (!xfmt_healthy(ctx)) return false;
    if (additional > INT_MAX - 1u || ctx->length > INT_MAX - 1u - additional) {
        xfmt_fail(ctx, XR_COMPILE_RESOURCE_BUDGET);
        return false;
    }
    size_t required = ctx->length + additional + 1;
    if (required <= ctx->capacity) return true;
    size_t capacity = ctx->capacity ? ctx->capacity : 4096;
    while (capacity < required) {
        if (!xfmt_step(ctx)) return false;
        capacity = capacity > INT_MAX / 2u ? (size_t)INT_MAX : capacity * 2;
    }
    void *memory = ctx->output;
    if (xr_compile_state_resize(ctx->state, &memory, capacity) != XR_COMPILE_RESOURCE_OK) return false;
    ctx->output = memory;
    ctx->capacity = capacity;
    return true;
}
/* This raw span writer is shared by syntax and literal output. Prefix handling
 * belongs to the syntax wrappers; literal payload bytes must not be skipped. */
XR_FUNC void xfmt_write_bytes(XrFmtContext *ctx, const char *bytes, size_t length) {
    if (!length || !xfmt_ensure_capacity(ctx, length)) return;
    int column = ctx->column, line_start = ctx->line_start;
    for (size_t i = 0; i < length; ++i) {
        if (!xfmt_step(ctx)) return;
        if (bytes[i] == '\n') { column = 0; line_start = 1; }
        else if (column == INT_MAX) { xfmt_fail(ctx, XR_COMPILE_RESOURCE_BUDGET); return; }
        else ++column;
    }
    if (xr_compile_state_copy(ctx->state, ctx->output + ctx->length, bytes, length) != XR_COMPILE_RESOURCE_OK ||
        !xfmt_step(ctx)) return;
    ctx->length += length;
    ctx->output[ctx->length] = 0;
    ctx->column = column;
    ctx->line_start = line_start;
}
XR_FUNC void xfmt_write_char(XrFmtContext *ctx, char c) {
    if (!xfmt_healthy(ctx)) return;
    if (ctx->block_literal_closed) {
        if (c == ' ' || c == '\t') return;
        ctx->block_literal_closed = false;
        if (c != '\n') { xfmt_write_char(ctx, '\n'); xfmt_write_indent(ctx); }
    }
    xfmt_write_bytes(ctx, &c, 1);
}
XR_FUNC void xfmt_write_str(XrFmtContext *ctx, const char *text) {
    if (!text || !xfmt_healthy(ctx)) return;
    size_t length = xfmt_length(ctx, text);
    if (!length || !xfmt_healthy(ctx)) return;
    if (ctx->block_literal_closed) {
        size_t skip = 0;
        while (skip < length) {
            if (!xfmt_step(ctx)) return;
            if (text[skip] != ' ' && text[skip] != '\t') break;
            ++skip;
        }
        if (skip == length) return;
        ctx->block_literal_closed = false;
        if (text[skip] != '\n') { xfmt_write_char(ctx, '\n'); xfmt_write_indent(ctx); }
        text += skip; length -= skip;
    }
    xfmt_write_bytes(ctx, text, length);
}
XR_FUNC void xfmt_write_fmt(XrFmtContext *ctx, const char *format, ...) {
    /* All callers supply fixed formats and bounded scalar arguments. Charge the
     * submitted stack capacity before invoking the C numeric conversion. */
    char buffer[1024];
    if (!xfmt_work(ctx, sizeof(buffer))) return;
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= sizeof(buffer)) {
        xfmt_fail(ctx, length < 0 ? XR_COMPILE_RESOURCE_BAD_ARGUMENT : XR_COMPILE_RESOURCE_BUDGET);
        return;
    }
    xfmt_write_str(ctx, buffer);
}
XR_FUNC int xfmt_indent_width(XrFmtContext *ctx) {
    if (!xfmt_healthy(ctx)) return 0;
    if (ctx->indent_level < 0 || ctx->config->indent_size < 0 ||
        (!ctx->config->use_tabs && ctx->config->indent_size &&
         ctx->indent_level > INT_MAX / ctx->config->indent_size)) {
        xfmt_fail(ctx, XR_COMPILE_RESOURCE_BUDGET); return 0;
    }
    return ctx->config->use_tabs ? ctx->indent_level : ctx->indent_level * ctx->config->indent_size;
}
XR_FUNC void xfmt_write_indent(XrFmtContext *ctx) {
    if (!xfmt_healthy(ctx) || !ctx->line_start) return;
    int count = xfmt_indent_width(ctx);
    for (int i = 0; i < count && xfmt_step(ctx); ++i)
        xfmt_write_char(ctx, ctx->config->use_tabs ? '\t' : ' ');
    if (xfmt_healthy(ctx)) ctx->line_start = 0;
}
XR_FUNC void xfmt_write_newline(XrFmtContext *ctx) { xfmt_write_char(ctx, '\n'); }
XR_FUNC void xfmt_write_space(XrFmtContext *ctx) { xfmt_write_char(ctx, ' '); }
XR_FUNC void xfmt_snapshot(XrFmtContext *ctx, XfmtSnapshot *snapshot) {
    *snapshot = (XfmtSnapshot){ctx->length,ctx->column,ctx->line_start,ctx->indent_level,ctx->block_literal_closed};
}
XR_FUNC void xfmt_rollback(XrFmtContext *ctx, const XfmtSnapshot *snapshot) {
    if (!xfmt_step(ctx)) return;
    ctx->length = snapshot->length;
    ctx->output[ctx->length] = 0;
    ctx->column = snapshot->column; ctx->line_start = snapshot->line_start;
    ctx->indent_level = snapshot->indent_level; ctx->block_literal_closed = snapshot->block_literal_closed;
}
XR_FUNC bool xfmt_fits_on_line(XrFmtContext *ctx, const XfmtSnapshot *snapshot) {
    for (size_t i = snapshot->length; i < ctx->length; ++i) {
        if (!xfmt_step(ctx) || ctx->output[i] == '\n') return false;
    }
    return xfmt_healthy(ctx) && ctx->column <= ctx->config->max_line_length;
}

static bool fmt_trailing_comment(XrFmtContext *ctx, const char *bytes, size_t begin, size_t end,
                                  size_t *code_width, size_t *comment) {
    bool in_double = false, in_single = false;
    size_t pos = begin;
    while (pos < end) {
        if (!xfmt_step(ctx)) return false;
        char c = bytes[pos];
        if (in_double || in_single) {
            if (c == '\\' && pos + 1 < end) { pos += 2; continue; }
            if (in_double && c == '"') in_double = false;
            if (in_single && c == '\'') in_single = false;
            ++pos;
        } else if (c == '"') { in_double = true; ++pos; }
        else if (c == '\'') { in_single = true; ++pos; }
        else if (c == '/' && pos + 1 < end) {
            if (!xfmt_step(ctx)) return false;
            char next = bytes[pos + 1];
            if (next == '/') {
                size_t code_end = pos;
                while (code_end > begin) {
                    if (!xfmt_step(ctx)) return false;
                    if (bytes[code_end - 1] != ' ') break;
                    --code_end;
                }
                if (code_end == begin) return false;
                *code_width = code_end - begin; *comment = pos;
                return true;
            }
            if (next == '*') {
                pos += 2;
                while (pos + 1 < end) {
                    if (!xfmt_work(ctx, 2)) return false;
                    if (bytes[pos] == '*' && bytes[pos + 1] == '/') break;
                    ++pos;
                }
                if (pos + 1 < end) pos += 2;
            } else ++pos;
        } else ++pos;
    }
    return false;
}
typedef struct XfmtLine { size_t begin, end, width, comment; bool trailing; } XfmtLine;

static void fmt_align_comments(XrFmtContext *ctx) {
    if (!ctx->length || !xfmt_healthy(ctx)) return;
    size_t count = 1;
    for (size_t i = 0; i < ctx->length; ++i) {
        if (!xfmt_step(ctx)) return;
        if (ctx->output[i] == '\n') ++count;
    }
    XfmtLine *lines = xfmt_alloc_array(ctx, count, sizeof(*lines));
    if (!lines) return;
    size_t begin = 0, used = 0;
    for (size_t i = 0; i <= ctx->length && xfmt_step(ctx); ++i) {
        if (i == ctx->length || ctx->output[i] == '\n') {
            XfmtLine line = {begin,i,0,0,false};
            line.trailing = fmt_trailing_comment(ctx, ctx->output, begin, i, &line.width, &line.comment);
            if (xr_compile_state_copy(ctx->state, &lines[used], &line, sizeof(line)) != XR_COMPILE_RESOURCE_OK) break;
            ++used; begin = i + 1;
        }
    }
    XrFmtContext out = {0};
    out.state = ctx->state; out.config = ctx->config;
    for (size_t i = 0; i < used && xfmt_step(ctx);) {
        if (!lines[i].trailing) {
            xfmt_write_bytes(&out, ctx->output + lines[i].begin, lines[i].end - lines[i].begin);
            if (lines[i].end < ctx->length) xfmt_write_bytes(&out, "\n", 1);
            ++i;
            continue;
        }
        size_t j = i + 1, width = lines[i].width;
        while (j < used && xfmt_step(ctx) && lines[j].trailing) {
            if (lines[j].width > width) width = lines[j].width;
            ++j;
        }
        for (size_t k = i; k < j && xfmt_step(ctx); ++k) {
            xfmt_write_bytes(&out, ctx->output + lines[k].begin, lines[k].width);
            size_t padding = width - lines[k].width + 2;
            for (size_t p = 0; p < padding && xfmt_step(ctx); ++p) xfmt_write_bytes(&out, " ", 1);
            xfmt_write_bytes(&out, ctx->output + lines[k].comment, lines[k].end - lines[k].comment);
            if (lines[k].end < ctx->length) xfmt_write_bytes(&out, "\n", 1);
        }
        i = j;
    }
    xr_compile_state_free(lines);
    if (xfmt_healthy(ctx)) {
        xr_compile_state_free(ctx->output);
        ctx->output = out.output; ctx->length = out.length; ctx->capacity = out.capacity;
    } else xr_compile_state_free(out.output);
}
XR_FUNC void xfmt_node(XrFmtContext *ctx, AstNode *node) {
    if (!node || !xfmt_step(ctx)) return;
    if (node->type == AST_PROGRAM) xfmt_emit_program(ctx, node);
    else xfmt_emit_statement(ctx, node);
}
XR_FUNC void xfmt_type(XrFmtContext *ctx, XrTypeRef *type) { xfmt_emit_type(ctx, type); }

static XrFmtStatus fmt_status(const XrCompileState *state) {
    switch (xr_compile_state_status(state)) {
        case XR_COMPILE_RESOURCE_OK: return XR_FMT_OK;
        case XR_COMPILE_RESOURCE_BUDGET: return XR_FMT_BUDGET;
        case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XR_FMT_OUT_OF_MEMORY;
        default: return XR_FMT_BAD_ARGUMENT;
    }
}
XR_FUNC XrFmtStatus xr_compile_format_ast(XrCompileState *state, AstNode *ast,
    const XrFmtConfig *config, XrFmtOutput *output) {
    if (!state || !ast || !output || output->text || output->length) return XR_FMT_BAD_ARGUMENT;
    if (fmt_status(state) != XR_FMT_OK) return fmt_status(state);
    if (ast->type == AST_PROGRAM && ast->as.program.arena &&
        !xr_compiler_arena_matches_state(ast->as.program.arena, state)) return XR_FMT_BAD_ARGUMENT;
    XrFmtContext ctx = {0};
    ctx.state = state; ctx.config = &ctx.configuration; ctx.line_start = 1;
    if (xr_compile_state_copy(state, &ctx.configuration, config ? config : &xfmt_default_config,
        sizeof(ctx.configuration)) != XR_COMPILE_RESOURCE_OK) return fmt_status(state);
    if (ctx.config->indent_size < 0 || ctx.config->max_line_length < 0) return XR_FMT_BAD_ARGUMENT;
    if (xfmt_ensure_capacity(&ctx, 0) && xfmt_step(&ctx)) ctx.output[0] = 0;
    xfmt_node(&ctx, ast);
    if (xfmt_healthy(&ctx) && config && config->trailing_newline && ctx.length &&
        ctx.output[ctx.length - 1] != '\n') xfmt_write_char(&ctx, '\n');
    if (xfmt_healthy(&ctx) && config && config->align_trailing_comments) fmt_align_comments(&ctx);
    XrFmtStatus status = fmt_status(state);
    if (status == XR_FMT_OK) *output = (XrFmtOutput){ctx.output,ctx.length};
    else xr_compile_state_free(ctx.output);
    return status;
}
XR_FUNC void xr_compile_format_output_free(XrFmtOutput *output) {
    if (!output) return;
    xr_compile_state_free(output->text);
    *output = (XrFmtOutput){0};
}
