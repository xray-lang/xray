/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfmt_type.c - Type, generic, destructure-pattern, operator-string output
 *
 * KEY CONCEPT:
 *   The non-recursive structural helpers used by both expr and decl
 *   modules. Type printing uses the single metered syntax-type printer.
 */

#include "xfmt_internal.h"
#include <limits.h>
#include "../parser/xtype_ref.h"
#include <string.h>

// ----------------------------------------------------------------------------
// Operator strings
// ----------------------------------------------------------------------------

const char *xfmt_binary_op(AstNodeType type) {
    switch (type) {
        case AST_BINARY_ADD:
            return "+";
        case AST_BINARY_SUB:
            return "-";
        case AST_BINARY_MUL:
            return "*";
        case AST_BINARY_DIV:
            return "/";
        case AST_BINARY_MOD:
            return "%";
        case AST_BINARY_BAND:
            return "&";
        case AST_BINARY_BOR:
            return "|";
        case AST_BINARY_BXOR:
            return "^";
        case AST_BINARY_LSHIFT:
            return "<<";
        case AST_BINARY_RSHIFT:
            return ">>";
        case AST_BINARY_EQ:
            return "==";
        case AST_BINARY_NE:
            return "!=";
        case AST_BINARY_LT:
            return "<";
        case AST_BINARY_LE:
            return "<=";
        case AST_BINARY_GT:
            return ">";
        case AST_BINARY_GE:
            return ">=";
        case AST_BINARY_AND:
            return "&&";
        case AST_BINARY_OR:
            return "||";
        default:
            return "?";
    }
}

const char *xfmt_compound_op(XrTokenType type) {
    switch (type) {
        case TK_PLUS_ASSIGN:
            return "+=";
        case TK_MINUS_ASSIGN:
            return "-=";
        case TK_MUL_ASSIGN:
            return "*=";
        case TK_DIV_ASSIGN:
            return "/=";
        case TK_MOD_ASSIGN:
            return "%=";
        case TK_AND_ASSIGN:
            return "&=";
        case TK_OR_ASSIGN:
            return "|=";
        case TK_XOR_ASSIGN:
            return "^=";
        case TK_LSHIFT_ASSIGN:
            return "<<=";
        case TK_RSHIFT_ASSIGN:
            return ">>=";
        default:
            return "?=";
    }
}

// ----------------------------------------------------------------------------
// Type
// ----------------------------------------------------------------------------

static void fmt_type_text(XrFmtContext *ctx, XrTypeRef *type, bool structural) {
    if (!xfmt_step(ctx)) return;
    if (!type || !xfmt_healthy(ctx)) return;
    size_t capacity = 256;
    void *scratch = NULL;
    while ( xfmt_step(ctx) && (xfmt_healthy(ctx))) {
        if (xr_compile_state_resize(ctx->state, &scratch, capacity) != XR_COMPILE_RESOURCE_OK) break;
        int length = structural ? xr_compile_tref_to_string_structural(ctx->state, type, scratch, (int)capacity) :
            xr_compile_tref_to_string_buf(ctx->state, type, scratch, (int)capacity);
        if (length < 0 || !xfmt_healthy(ctx)) break;
        if ((size_t)length < capacity - 1) {
            xfmt_write_bytes(ctx, scratch, (size_t)length);
            break;
        }
        if (capacity > INT_MAX / 2u) { xfmt_fail(ctx, XR_COMPILE_RESOURCE_BUDGET); break; }
        capacity *= 2;
    }
    xr_compile_state_free(scratch);
}
XR_FUNC void xfmt_emit_type(XrFmtContext *ctx, XrTypeRef *type) {
    if (!xfmt_step(ctx)) return; fmt_type_text(ctx, type, false); }
XR_FUNC void xfmt_emit_type_structural(XrFmtContext *ctx, XrTypeRef *type) {
    if (!xfmt_step(ctx)) return; fmt_type_text(ctx, type, true); }

XR_FUNC void xfmt_emit_param_annotation(XrFmtContext *ctx, XrParamMode mode, XrTypeRef *tref) {
    if (!xfmt_step(ctx)) return;
    if (!tref)
        return;
    xfmt_write_str(ctx, ": ");
    if (mode != XR_PARAM_READ && xr_param_mode_is_valid(mode)) {
        xfmt_write_str(ctx, xr_param_mode_label(mode));
        xfmt_write_char(ctx, ' ');
    }
    xfmt_emit_type(ctx, tref);
}

// Format generic type parameters <T, U: A & B & ...>
XR_FUNC void xfmt_emit_generic_params(XrFmtContext *ctx, XrGenericParam **params, int count) {
    if (!xfmt_step(ctx)) return;
    if (count <= 0)
        return;

    xfmt_write_char(ctx, '<');
    for (int i = 0; xfmt_step(ctx) && (i < count); i++) {
        if (i > 0)
            xfmt_write_str(ctx, ", ");
        xfmt_write_str(ctx, params[i]->name);
        if (params[i]->constraint_count > 0 && params[i]->constraints) {
            xfmt_write_str(ctx, ": ");
            for (int j = 0; xfmt_step(ctx) && (j < params[i]->constraint_count); j++) {
                if (j > 0)
                    xfmt_write_str(ctx, " & ");
                xfmt_emit_type(ctx, params[i]->constraints[j]);
            }
        }
    }
    xfmt_write_char(ctx, '>');
}

// Format generic type arguments <int, string>
XR_FUNC void xfmt_emit_generic_args(XrFmtContext *ctx, XrTypeRef **args, int count) {
    if (!xfmt_step(ctx)) return;
    if (count <= 0)
        return;

    xfmt_write_char(ctx, '<');
    for (int i = 0; xfmt_step(ctx) && (i < count); i++) {
        if (i > 0)
            xfmt_write_str(ctx, ", ");
        xfmt_emit_type(ctx, args[i]);
    }
    xfmt_write_char(ctx, '>');
}

// ----------------------------------------------------------------------------
// Destructure patterns (used by var/const declarations and multi-assign)
// ----------------------------------------------------------------------------

XR_FUNC void xfmt_emit_pattern(XrFmtContext *ctx, XrDestructurePattern *pattern) {
    if (!xfmt_step(ctx)) return;
    if (!pattern)
        return;

    switch (pattern->type) {
        case PATTERN_IDENTIFIER:
            xfmt_write_str(ctx, pattern->as.identifier.name);
            if (pattern->as.identifier.type) {
                xfmt_write_str(ctx, ": ");
                xfmt_emit_type(ctx, pattern->as.identifier.type);
            }
            break;

        case PATTERN_ARRAY:
            xfmt_write_char(ctx, '[');
            for (int i = 0; xfmt_step(ctx) && (i < pattern->as.array.element_count); i++) {
                if (i > 0)
                    xfmt_write_str(ctx, ", ");
                xfmt_emit_pattern(ctx, pattern->as.array.elements[i]);
            }
            xfmt_write_char(ctx, ']');
            break;

        case PATTERN_TUPLE:
            xfmt_write_char(ctx, '(');
            for (int i = 0; xfmt_step(ctx) && (i < pattern->as.array.element_count); i++) {
                if (i > 0)
                    xfmt_write_str(ctx, ", ");
                xfmt_emit_pattern(ctx, pattern->as.array.elements[i]);
            }
            /* Unary tuple pattern needs the trailing comma so the
             * roundtripped surface form does not collapse to a
             * grouping like `(x)`. */
            if (pattern->as.array.element_count == 1)
                xfmt_write_char(ctx, ',');
            xfmt_write_char(ctx, ')');
            break;

        case PATTERN_OBJECT:
            xfmt_write_char(ctx, '{');
            for (int i = 0; xfmt_step(ctx) && (i < pattern->as.object.field_count); i++) {
                if (i > 0)
                    xfmt_write_str(ctx, ", ");
                const char *field =
                    pattern->as.object.field_names ? pattern->as.object.field_names[i] : NULL;
                XrDestructurePattern *sub = pattern->as.object.patterns[i];
                if (field && sub && sub->type == PATTERN_IDENTIFIER && sub->as.identifier.name &&
                    xfmt_compare(ctx, field, sub->as.identifier.name) == 0) {
                    xfmt_write_str(ctx, field);
                } else {
                    if (field)
                        xfmt_write_str(ctx, field);
                    xfmt_write_str(ctx, ": ");
                    xfmt_emit_pattern(ctx, sub);
                }
            }
            xfmt_write_char(ctx, '}');
            break;

        case PATTERN_SKIP:
            xfmt_write_char(ctx, '_');
            break;
    }
}
