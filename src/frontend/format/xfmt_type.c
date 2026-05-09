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
 *   modules. Type printing delegates to xr_type_to_string (the runtime
 *   type printer) so the formatter does not need to mirror the type
 *   grammar.
 */

#include "xfmt_internal.h"
#include "../../runtime/value/xtype.h"
#include "../parser/xtype_ref.h"

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
        case AST_BINARY_EQ_STRICT:
            return "===";
        case AST_BINARY_NE_STRICT:
            return "!==";
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

void xfmt_emit_type(XrFmtContext *ctx, XrTypeRef *tref) {
    if (!tref)
        return;
    /* Use the buffer variant — no arena required at format time. */
    char buf[256];
    int n = xr_tref_to_string_buf(tref, buf, (int) sizeof(buf));
    if (n > 0) {
        xfmt_write_str(ctx, buf);
    } else {
        xfmt_write_str(ctx, "unknown");
    }
}

// Format generic type parameters <T, U: Constraint>
void xfmt_emit_generic_params(XrFmtContext *ctx, XrGenericParam **params, int count) {
    if (count <= 0)
        return;

    xfmt_write_char(ctx, '<');
    for (int i = 0; i < count; i++) {
        if (i > 0)
            xfmt_write_str(ctx, ", ");
        xfmt_write_str(ctx, params[i]->name);
        if (params[i]->constraint) {
            xfmt_write_str(ctx, ": ");
            xfmt_emit_type(ctx, params[i]->constraint);
        }
    }
    xfmt_write_char(ctx, '>');
}

// Format generic type arguments <int, string>
void xfmt_emit_generic_args(XrFmtContext *ctx, XrTypeRef **args, int count) {
    if (count <= 0)
        return;

    xfmt_write_char(ctx, '<');
    for (int i = 0; i < count; i++) {
        if (i > 0)
            xfmt_write_str(ctx, ", ");
        xfmt_emit_type(ctx, args[i]);
    }
    xfmt_write_char(ctx, '>');
}

// ----------------------------------------------------------------------------
// Destructure patterns (used by let / multi-assign)
// ----------------------------------------------------------------------------

void xfmt_emit_pattern(XrFmtContext *ctx, XrDestructurePattern *pattern) {
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
            for (int i = 0; i < pattern->as.array.element_count; i++) {
                if (i > 0)
                    xfmt_write_str(ctx, ", ");
                xfmt_emit_pattern(ctx, pattern->as.array.elements[i]);
            }
            xfmt_write_char(ctx, ']');
            break;

        case PATTERN_OBJECT:
            xfmt_write_char(ctx, '{');
            for (int i = 0; i < pattern->as.object.field_count; i++) {
                if (i > 0)
                    xfmt_write_str(ctx, ", ");
                xfmt_emit_pattern(ctx, pattern->as.object.patterns[i]);
            }
            xfmt_write_char(ctx, '}');
            break;

        case PATTERN_SKIP:
            xfmt_write_char(ctx, '_');
            break;
    }
}
