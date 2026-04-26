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
        ctx->output = (char *)xr_realloc(ctx->output, ctx->capacity);
    }
}

static void lit_byte(XrFmtContext *ctx, char c) {
    lit_ensure(ctx, 1);
    ctx->output[ctx->length++] = c;
    ctx->output[ctx->length] = '\0';
    if (c == '\n') {
        ctx->line_start = 1;
        ctx->column = 0;
    } else {
        ctx->column++;
    }
}

static void lit_bytes(XrFmtContext *ctx, const char *bytes, size_t n) {
    if (n == 0) return;
    lit_ensure(ctx, n);
    memcpy(ctx->output + ctx->length, bytes, n);
    ctx->length += n;
    ctx->output[ctx->length] = '\0';
    const char *last_nl = NULL;
    for (size_t i = 0; i < n; i++) {
        if (bytes[i] == '\n') last_nl = bytes + i;
    }
    if (last_nl) {
        ctx->line_start = 1;
        ctx->column = (int)(bytes + n - 1 - last_nl);
    } else {
        ctx->column += (int)n;
    }
}

static void lit_str(XrFmtContext *ctx, const char *s) {
    if (!s) return;
    lit_bytes(ctx, s, strlen(s));
}

/* ========== Escape table ==========
 *
 * `escape_in_template` flips on `$` re-escaping for template literal
 * parts only. Outside of templates, `$` is just text and needs no
 * escaping.
 */

static void emit_escaped_byte(XrFmtContext *ctx, unsigned char c, bool escape_dollar) {
    switch (c) {
        case '"':  lit_bytes(ctx, "\\\"", 2); return;
        case '\\': lit_bytes(ctx, "\\\\", 2); return;
        case '\n': lit_bytes(ctx, "\\n",  2); return;
        case '\r': lit_bytes(ctx, "\\r",  2); return;
        case '\t': lit_bytes(ctx, "\\t",  2); return;
        case '\b': lit_bytes(ctx, "\\b",  2); return;
        case '\f': lit_bytes(ctx, "\\f",  2); return;
        case '\0': lit_bytes(ctx, "\\0",  2); return;
        case '$':
            if (escape_dollar) {
                lit_bytes(ctx, "\\$", 2);
                return;
            }
            break;
        default:
            break;
    }
    if (c < 0x20) {
        char buf[5];
        // \xHH escape for any other control byte. The lexer's escape
        // table does not currently recognise \xHH; once it does this
        // will round-trip exactly. Until then control bytes are rare
        // enough that this branch is the safe fallback.
        snprintf(buf, sizeof buf, "\\x%02X", c);
        lit_bytes(ctx, buf, 4);
        return;
    }
    lit_byte(ctx, (char)c);
}

static void emit_payload(XrFmtContext *ctx, const char *value, int len, bool escape_dollar) {
    if (!value || len <= 0) return;
    for (int i = 0; i < len; i++) {
        emit_escaped_byte(ctx, (unsigned char)value[i], escape_dollar);
    }
}

/* ========== Public API ========== */

void xfmt_emit_string(XrFmtContext *ctx, const char *value, int len) {
    lit_byte(ctx, '"');
    emit_payload(ctx, value, len, /*escape_dollar=*/false);
    lit_byte(ctx, '"');
}

void xfmt_emit_raw_string(XrFmtContext *ctx, const char *value, int len) {
    // Project principle: canonical form > lexeme preservation. Raw
    // style is dropped at parse time, so this just emits the canonical
    // double-quoted form.
    xfmt_emit_string(ctx, value, len);
}

void xfmt_emit_template_string(XrFmtContext *ctx, AstNode *node,
                               XrFmtExprEmitter emit_expr) {
    lit_byte(ctx, '"');
    TemplateStringNode *tmpl = &node->as.template_str;
    for (int i = 0; i < tmpl->part_count; i++) {
        AstNode *part = tmpl->parts[i];
        if (part->type == AST_LITERAL_STRING) {
            const char *s = part->as.literal.raw_value.string_val;
            int n = s ? (int)strlen(s) : 0;
            // escape_dollar=true: prevent literal "${" inside parts
            // from being misread as an interpolation opener at re-parse.
            emit_payload(ctx, s, n, /*escape_dollar=*/true);
        } else {
            lit_str(ctx, "${");
            if (emit_expr) emit_expr(ctx, part);
            lit_byte(ctx, '}');
        }
    }
    lit_byte(ctx, '"');
}
