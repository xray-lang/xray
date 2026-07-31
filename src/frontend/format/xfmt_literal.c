/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfmt_literal.c - String / template-string serialisation
 *
 * See xfmt_literal.h for the rationale.
 */

#include "xfmt_literal.h"
#include "xfmt_internal.h"
#include "../../base/xmalloc.h"
#include <stdio.h>
#include <string.h>

/* ========== Local buffer helpers ==========
 *
 * Mirror xfmt.c's static write_char/write_str behaviour so this file
 * stays self-contained; when xfmt.c is split, these helpers may move
 * into a shared xfmt_internal.h.
 */

static void lit_ensure(XrFmtContext *ctx, size_t additional) {
    if (ctx->length + additional >= ctx->capacity) {
        ctx->capacity = (ctx->capacity + additional) * 2;
        XR_REALLOC_OR_ABORT(ctx->output, ctx->capacity, "fmt_literal buffer grow");
    }
}

static void lit_byte(XrFmtContext *ctx, char c) {
    xfmt_write_char(ctx, c);
}

static void lit_bytes(XrFmtContext *ctx, const char *bytes, size_t n) {
    if (n == 0)
        return;
    lit_ensure(ctx, n);
    memcpy(ctx->output + ctx->length, bytes, n);
    ctx->length += n;
    ctx->output[ctx->length] = '\0';
    const char *last_nl = NULL;
    for (size_t i = 0; i < n; i++) {
        if (bytes[i] == '\n')
            last_nl = bytes + i;
    }
    if (last_nl) {
        ctx->line_start = 1;
        ctx->column = (int) (bytes + n - 1 - last_nl);
    } else {
        ctx->column += (int) n;
    }
}

static void lit_str(XrFmtContext *ctx, const char *s) {
    if (!s)
        return;
    lit_bytes(ctx, s, strlen(s));
}

static void lit_indent(XrFmtContext *ctx) {
    if (!ctx->line_start)
        return;
    if (ctx->config->use_tabs) {
        for (int i = 0; i < ctx->indent_level; i++)
            lit_byte(ctx, '\t');
    } else {
        int spaces = ctx->indent_level * ctx->config->indent_size;
        for (int i = 0; i < spaces; i++)
            lit_byte(ctx, ' ');
    }
    ctx->line_start = 0;
}

static void emit_escaped_byte(XrFmtContext *ctx, unsigned char c, bool escape_dollar, bool block,
                              bool binary) {
    switch (c) {
        case '"':
            if (block)
                lit_byte(ctx, '"');
            else
                lit_bytes(ctx, "\\\"", 2);
            return;
        case '\\':
            lit_bytes(ctx, "\\\\", 2);
            return;
        case '\n':
            if (block)
                lit_byte(ctx, '\n');
            else
                lit_bytes(ctx, "\\n", 2);
            return;
        case '\r':
            lit_bytes(ctx, "\\r", 2);
            return;
        case '\t':
            lit_bytes(ctx, "\\t", 2);
            return;
        case '\b':
            lit_bytes(ctx, "\\b", 2);
            return;
        case '\f':
            lit_bytes(ctx, "\\f", 2);
            return;
        case '\0':
            lit_bytes(ctx, "\\0", 2);
            return;
        case '$':
            if (escape_dollar) {
                lit_bytes(ctx, "\\$", 2);
                return;
            }
            break;
        default:
            break;
    }
    if (c < 0x20 || (binary && c >= 0x80)) {
        char buf[5];
        snprintf(buf, sizeof buf, "\\x%02X", c);
        lit_bytes(ctx, buf, 4);
        return;
    }
    lit_byte(ctx, (char) c);
}

static void emit_payload(XrFmtContext *ctx, const uint8_t *value, size_t len,
                         XrLiteralEscapeMode escape_mode, bool block, bool binary,
                         bool escape_dollar) {
    if (!value || len <= 0)
        return;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = value[i];
        if (escape_mode == XR_LITERAL_RAW) {
            lit_byte(ctx, (char) c);
        } else {
            emit_escaped_byte(ctx, c, escape_dollar, block, binary);
        }
        if (block && c == '\n' && i + 1 < len)
            lit_indent(ctx);
    }
}

static bool quote_line_collision(const uint8_t *value, size_t length, int quote_count) {
    size_t line_start = 0;
    for (size_t i = 0; i <= length; i++) {
        if (i < length && value[i] != '\n')
            continue;
        size_t line_length = i - line_start;
        if (line_length == (size_t) quote_count) {
            bool only_quotes = true;
            for (size_t j = line_start; j < i; j++) {
                if (value[j] != '"') {
                    only_quotes = false;
                    break;
                }
            }
            if (only_quotes)
                return true;
        }
        line_start = i + 1;
    }
    return false;
}

static int safe_quote_count(const uint8_t *value, size_t length) {
    int quote_count = 3;
    while (quote_line_collision(value, length, quote_count))
        quote_count++;
    return quote_count;
}

static void emit_quotes(XrFmtContext *ctx, int quote_count) {
    for (int i = 0; i < quote_count; i++)
        lit_byte(ctx, '"');
}

static void emit_quoted_payload(XrFmtContext *ctx, const char *prefix, const uint8_t *value,
                                size_t length, XrLiteralEscapeMode escape_mode,
                                XrLiteralSourceForm source_form, bool binary, bool escape_dollar) {
    lit_str(ctx, prefix);
    if (source_form == XR_LITERAL_INLINE) {
        lit_byte(ctx, '"');
        emit_payload(ctx, value, length, escape_mode, false, binary, escape_dollar);
        lit_byte(ctx, '"');
        return;
    }
    int quote_count = safe_quote_count(value, length);
    emit_quotes(ctx, quote_count);
    lit_byte(ctx, '\n');
    if (length > 0) {
        lit_indent(ctx);
        emit_payload(ctx, value, length, escape_mode, true, binary, escape_dollar);
        lit_byte(ctx, '\n');
    }
    lit_indent(ctx);
    emit_quotes(ctx, quote_count);
    ctx->block_literal_closed = true;
}

void xfmt_emit_float_literal(XrFmtContext *ctx, double value) {
    // %.17g round-trips every finite double exactly; shorter forms can change
    // the value. Then guarantee the result still LOOKS like a float: without a
    // '.', 'e' or 'E' the lexer would take `0` back as an integer literal and
    // the expression would silently change type.
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%.17g", value);
    if (n <= 0 || (size_t) n >= sizeof(buf)) {
        xfmt_write_str(ctx, "0.0");
        return;
    }
    if (!strpbrk(buf, ".eEnN")) {  // n/N also catch inf/nan spellings
        if ((size_t) n + 2 < sizeof(buf)) {
            buf[n] = '.';
            buf[n + 1] = '0';
            buf[n + 2] = '\0';
        }
    }
    xfmt_write_str(ctx, buf);
}

void xfmt_emit_escaped_inline_string(XrFmtContext *ctx, const char *value, int length) {
    emit_quoted_payload(ctx, "", (const uint8_t *) value, length > 0 ? (size_t) length : 0,
                        XR_LITERAL_ESCAPED, XR_LITERAL_INLINE, false, false);
}

void xfmt_emit_string_literal(XrFmtContext *ctx, AstNode *node) {
    LiteralNode *literal = &node->as.literal;
    const char *value = literal->raw_value.string_val;
    const char *prefix = literal->escape_mode == XR_LITERAL_RAW ? "r" : "";
    emit_quoted_payload(ctx, prefix, (const uint8_t *) value, value ? strlen(value) : 0,
                        literal->escape_mode, literal->source_form, false, false);
}

void xfmt_emit_fixed_bytes_literal(XrFmtContext *ctx, AstNode *node) {
    FixedBytesLiteralNode *literal = &node->as.fixed_bytes_literal;
    const char *prefix = NULL;
    if (literal->append_nul)
        prefix = literal->escape_mode == XR_LITERAL_RAW ? "cr" : "c";
    else
        prefix = literal->escape_mode == XR_LITERAL_RAW ? "br" : "b";
    emit_quoted_payload(ctx, prefix, literal->payload, literal->payload_length,
                        literal->escape_mode, literal->source_form, true, false);
}

static int template_quote_count(const TemplateStringNode *tmpl) {
    int quote_count = 3;
    bool collision = true;
    while (collision) {
        collision = false;
        for (int i = 0; i < tmpl->part_count; i++) {
            AstNode *part = tmpl->parts[i];
            if (part && part->type == AST_LITERAL_STRING) {
                const char *value = part->as.literal.raw_value.string_val;
                if (value &&
                    quote_line_collision((const uint8_t *) value, strlen(value), quote_count)) {
                    collision = true;
                    quote_count++;
                    break;
                }
            }
        }
    }
    return quote_count;
}

static void emit_template_parts(XrFmtContext *ctx, TemplateStringNode *tmpl, bool block,
                                XrFmtExprEmitter emit_expr) {
    for (int i = 0; i < tmpl->part_count; i++) {
        AstNode *part = tmpl->parts[i];
        /* A literal part may end exactly at a block newline. In that case the
         * next part owns the first bytes on the new source line. Establish
         * the closing-margin indent before emitting either a literal or `${`;
         * otherwise an expression emitter would place that indent inside the
         * interpolation and leave the block body under-indented. */
        if (block && ctx->line_start)
            lit_indent(ctx);
        if (part->type == AST_LITERAL_STRING && part->as.literal.is_template_chunk) {
            const char *value = part->as.literal.raw_value.string_val;
            emit_payload(ctx, (const uint8_t *) value, value ? strlen(value) : 0, tmpl->escape_mode,
                         block, false, true);
        } else {
            lit_str(ctx, "${");
            if (emit_expr)
                emit_expr(ctx, part);
            lit_byte(ctx, '}');
        }
    }
}

void xfmt_emit_template_string(XrFmtContext *ctx, AstNode *node, XrFmtExprEmitter emit_expr) {
    TemplateStringNode *tmpl = &node->as.template_str;
    if (tmpl->escape_mode == XR_LITERAL_RAW)
        lit_byte(ctx, 'r');
    if (tmpl->source_form == XR_LITERAL_INLINE) {
        lit_byte(ctx, '"');
        emit_template_parts(ctx, tmpl, false, emit_expr);
        lit_byte(ctx, '"');
        return;
    }
    int quote_count = template_quote_count(tmpl);
    emit_quotes(ctx, quote_count);
    lit_byte(ctx, '\n');
    if (tmpl->part_count > 0) {
        lit_indent(ctx);
        emit_template_parts(ctx, tmpl, true, emit_expr);
        lit_byte(ctx, '\n');
    }
    lit_indent(ctx);
    emit_quotes(ctx, quote_count);
    ctx->block_literal_closed = true;
}
