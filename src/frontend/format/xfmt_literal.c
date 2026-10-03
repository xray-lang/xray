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
#include <limits.h>
#include "../../base/xmalloc.h"
#include <stdio.h>
#include <string.h>

/* Literals share the same allocation and raw-span writer as syntax. */
static void lit_byte(XrFmtContext *ctx, char c) {
    if (!xfmt_step(ctx)) return; xfmt_write_char(ctx, c); }
static void lit_bytes(XrFmtContext *ctx, const char *bytes, size_t length) {
    if (!xfmt_step(ctx)) return;
    xfmt_write_bytes(ctx, bytes, length);
}
static void lit_str(XrFmtContext *ctx, const char *text) {
    if (!xfmt_step(ctx)) return;
    if (text) xfmt_write_bytes(ctx, text, xfmt_length(ctx, text));
}
static void lit_indent(XrFmtContext *ctx) {
    if (!xfmt_step(ctx)) return; xfmt_write_indent(ctx); }

static void emit_escaped_byte(XrFmtContext *ctx, unsigned char c, bool escape_dollar, bool block,
                              bool binary) {
    if (!xfmt_step(ctx)) return;
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
        if (!xfmt_work(ctx, sizeof(buf))) return;
        snprintf(buf, sizeof buf, "\\x%02X", c);
        lit_bytes(ctx, buf, 4);
        return;
    }
    lit_byte(ctx, (char) c);
}

static void emit_payload(XrFmtContext *ctx, const uint8_t *value, size_t len,
                         XrLiteralEscapeMode escape_mode, bool block, bool binary,
                         bool escape_dollar) {
    if (!xfmt_step(ctx)) return;
    if (!value || len <= 0)
        return;
    for (size_t i = 0; xfmt_step(ctx) && (i < len); i++) {
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

static bool quote_line_collision(XrFmtContext *ctx, const uint8_t *value, size_t length, int quote_count) {
    if (!xfmt_step(ctx)) return false;
    size_t line_start = 0;
    for (size_t i = 0; xfmt_step(ctx) && (i <= length); i++) {
        if (i < length && value[i] != '\n')
            continue;
        size_t line_length = i - line_start;
        if (line_length == (size_t) quote_count) {
            bool only_quotes = true;
            for (size_t j = line_start; xfmt_step(ctx) && (j < i); j++) {
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

static int safe_quote_count(XrFmtContext *ctx, const uint8_t *value, size_t length) {
    if (!xfmt_step(ctx)) return 0;
    int quote_count = 3;
    while ( xfmt_step(ctx) && (quote_line_collision(ctx, value, length, quote_count))) {
        if (quote_count == INT_MAX) { xfmt_fail(ctx, XR_COMPILE_RESOURCE_BUDGET); return 0; }
        quote_count++;
    }
    return quote_count;
}

static void emit_quotes(XrFmtContext *ctx, int quote_count) {
    if (!xfmt_step(ctx)) return;
    for (int i = 0; xfmt_step(ctx) && (i < quote_count); i++)
        lit_byte(ctx, '"');
}

static void emit_quoted_payload(XrFmtContext *ctx, const char *prefix, const uint8_t *value,
                                size_t length, XrLiteralEscapeMode escape_mode,
                                XrLiteralSourceForm source_form, bool binary, bool escape_dollar) {
    if (!xfmt_step(ctx)) return;
    lit_str(ctx, prefix);
    if (source_form == XR_LITERAL_INLINE) {
        lit_byte(ctx, '"');
        emit_payload(ctx, value, length, escape_mode, false, binary, escape_dollar);
        lit_byte(ctx, '"');
        return;
    }
    int quote_count = safe_quote_count(ctx, value, length);
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

XR_FUNC void xfmt_emit_float_literal(XrFmtContext *ctx, double value) {
    if (!xfmt_step(ctx)) return;
    // %.17g round-trips every finite double exactly; shorter forms can change
    // the value. Then guarantee the result still LOOKS like a float: without a
    // '.', 'e' or 'E' the lexer would take `0` back as an integer literal and
    // the expression would silently change type.
    char buf[64];
    if (!xfmt_work(ctx, sizeof(buf))) return;
    int n = snprintf(buf, sizeof(buf), "%.17g", value);
    if (n <= 0 || (size_t) n >= sizeof(buf)) {
        xfmt_fail(ctx, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return;
    }
    bool marker = false;
    for (int i = 0; xfmt_step(ctx) && (i < n); ++i) {
        if (!xfmt_step(ctx)) return;
        char c = buf[i];
        if (c == '.' || c == 'e' || c == 'E' || c == 'n' || c == 'N') marker = true;
    }
    if (!marker) {  // n/N also catch inf/nan spellings
        if ((size_t) n + 2 < sizeof(buf)) {
            buf[n] = '.';
            buf[n + 1] = '0';
            buf[n + 2] = '\0';
        }
    }
    xfmt_write_str(ctx, buf);
}

XR_FUNC void xfmt_emit_escaped_inline_string(XrFmtContext *ctx, const char *value, int length) {
    if (!xfmt_step(ctx)) return;
    emit_quoted_payload(ctx, "", (const uint8_t *) value, length > 0 ? (size_t) length : 0,
                        XR_LITERAL_ESCAPED, XR_LITERAL_INLINE, false, false);
}

XR_FUNC void xfmt_emit_string_literal(XrFmtContext *ctx, AstNode *node) {
    if (!xfmt_step(ctx)) return;
    LiteralNode *literal = &node->as.literal;
    const char *value = literal->raw_value.string_val;
    const char *prefix = literal->escape_mode == XR_LITERAL_RAW ? "r" : "";
    emit_quoted_payload(ctx, prefix, (const uint8_t *) value, value ? literal->string_length : 0,
                        literal->escape_mode, literal->source_form, false, false);
}

XR_FUNC void xfmt_emit_fixed_bytes_literal(XrFmtContext *ctx, AstNode *node) {
    if (!xfmt_step(ctx)) return;
    FixedBytesLiteralNode *literal = &node->as.fixed_bytes_literal;
    const char *prefix = NULL;
    if (literal->append_nul)
        prefix = literal->escape_mode == XR_LITERAL_RAW ? "cr" : "c";
    else
        prefix = literal->escape_mode == XR_LITERAL_RAW ? "br" : "b";
    emit_quoted_payload(ctx, prefix, literal->payload, literal->payload_length,
                        literal->escape_mode, literal->source_form, true, false);
}

static int template_quote_count(XrFmtContext *ctx, const TemplateStringNode *tmpl) {
    if (!xfmt_step(ctx)) return 0;
    int quote_count = 3;
    bool collision = true;
    while ( xfmt_step(ctx) && (collision)) {
        collision = false;
        for (int i = 0; xfmt_step(ctx) && (i < tmpl->part_count); i++) {
            AstNode *part = tmpl->parts[i];
            if (part && part->type == AST_LITERAL_STRING) {
                const char *value = part->as.literal.raw_value.string_val;
                if (value &&
                    quote_line_collision(ctx, (const uint8_t *) value, part->as.literal.string_length, quote_count)) {
                    collision = true;
                    if (quote_count == INT_MAX) { xfmt_fail(ctx, XR_COMPILE_RESOURCE_BUDGET); return 0; }
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
    if (!xfmt_step(ctx)) return;
    for (int i = 0; xfmt_step(ctx) && (i < tmpl->part_count); i++) {
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
            emit_payload(ctx, (const uint8_t *) value, value ? part->as.literal.string_length : 0, tmpl->escape_mode,
                         block, false, true);
        } else {
            lit_str(ctx, "${");
            if (emit_expr)
                emit_expr(ctx, part);
            lit_byte(ctx, '}');
        }
    }
}

XR_FUNC void xfmt_emit_template_string(XrFmtContext *ctx, AstNode *node, XrFmtExprEmitter emit_expr) {
    if (!xfmt_step(ctx)) return;
    TemplateStringNode *tmpl = &node->as.template_str;
    if (tmpl->escape_mode == XR_LITERAL_RAW)
        lit_byte(ctx, 'r');
    if (tmpl->source_form == XR_LITERAL_INLINE) {
        lit_byte(ctx, '"');
        emit_template_parts(ctx, tmpl, false, emit_expr);
        lit_byte(ctx, '"');
        return;
    }
    int quote_count = template_quote_count(ctx, tmpl);
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
