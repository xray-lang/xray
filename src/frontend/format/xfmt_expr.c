/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfmt_expr.c - Expression formatting
 *
 * KEY CONCEPT:
 *   xfmt_emit_expression() is the single dispatch entry that other
 *   formatter modules (stmt, decl) call into. Literal strings and
 *   template strings delegate to xfmt_literal.c so re-escaping stays
 *   in one place.
 */

#include "xfmt_internal.h"
#include "xfmt_literal.h"
#include "../../base/xmalloc.h"
#include <string.h>

// ----------------------------------------------------------------------------
// Leaf-shaped expressions
// ----------------------------------------------------------------------------

static void fmt_literal(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);

    switch (node->type) {
        case AST_LITERAL_INT:
            xfmt_write_fmt(ctx, "%lld", (long long) node->as.literal.raw_value.int_val);
            break;
        case AST_LITERAL_FLOAT:
            xfmt_write_fmt(ctx, "%g", node->as.literal.raw_value.float_val);
            break;
        case AST_LITERAL_BIGINT:
            xfmt_write_str(ctx, node->as.literal.raw_value.bigint_val);
            xfmt_write_char(ctx, 'n');
            break;
        case AST_LITERAL_STRING: {
            // Re-escape via xfmt_emit_string so quotes / backslashes /
            // control bytes round-trip through the parser.
            const char *s = node->as.literal.raw_value.string_val;
            xfmt_emit_string(ctx, s, s ? (int) strlen(s) : 0);
            break;
        }
        case AST_LITERAL_REGEX:
            xfmt_write_char(ctx, '/');
            xfmt_write_str(ctx, node->as.literal.raw_value.regex.pattern);
            xfmt_write_char(ctx, '/');
            if (node->as.literal.raw_value.regex.flags) {
                xfmt_write_str(ctx, node->as.literal.raw_value.regex.flags);
            }
            break;
        case AST_LITERAL_NULL:
            xfmt_write_str(ctx, "null");
            break;
        case AST_LITERAL_TRUE:
            xfmt_write_str(ctx, "true");
            break;
        case AST_LITERAL_FALSE:
            xfmt_write_str(ctx, "false");
            break;
        default:
            break;
    }
}

static void fmt_binary(XrFmtContext *ctx, AstNode *node) {
    xfmt_emit_expression(ctx, node->as.binary.left);
    xfmt_write_space(ctx);
    xfmt_write_str(ctx, xfmt_binary_op(node->type));
    xfmt_write_space(ctx);
    xfmt_emit_expression(ctx, node->as.binary.right);
}

static void fmt_unary(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);
    switch (node->type) {
        case AST_UNARY_NEG:
            xfmt_write_char(ctx, '-');
            break;
        case AST_UNARY_NOT:
            xfmt_write_char(ctx, '!');
            break;
        case AST_UNARY_BNOT:
            xfmt_write_char(ctx, '~');
            break;
        default:
            break;
    }
    xfmt_emit_expression(ctx, node->as.unary.operand);
}

static void fmt_call(XrFmtContext *ctx, AstNode *node) {
    CallExprNode *call = &node->as.call_expr;
    xfmt_emit_expression(ctx, call->callee);
    xfmt_emit_generic_args(ctx, call->type_args, call->type_arg_count);

    bool wrap = ctx->config && ctx->config->wrap_long_lines && call->arg_count > 0;
    XfmtSnapshot snap;
    if (wrap)
        xfmt_snapshot(ctx, &snap);

    // Try single-line: foo(a, b, c)
    xfmt_write_char(ctx, '(');
    for (int i = 0; i < call->arg_count; i++) {
        if (i > 0)
            xfmt_write_str(ctx, ", ");
        xfmt_emit_expression(ctx, call->arguments[i]);
    }
    xfmt_write_char(ctx, ')');

    if (!wrap || xfmt_fits_on_line(ctx, &snap))
        return;

    // Multi-line:
    //   foo(
    //       a,
    //       b,
    //   )
    xfmt_rollback(ctx, &snap);
    xfmt_write_char(ctx, '(');
    xfmt_write_newline(ctx);
    ctx->indent_level++;
    for (int i = 0; i < call->arg_count; i++) {
        xfmt_write_indent(ctx);
        xfmt_emit_expression(ctx, call->arguments[i]);
        if (ctx->config->multiline_trailing_comma || i < call->arg_count - 1)
            xfmt_write_char(ctx, ',');
        xfmt_write_newline(ctx);
    }
    ctx->indent_level--;
    xfmt_write_indent(ctx);
    xfmt_write_char(ctx, ')');
}

static void fmt_new_expr(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);
    xfmt_write_str(ctx, "new ");
    NewExprNode *new_expr = &node->as.new_expr;
    if (new_expr->module_name) {
        xfmt_write_str(ctx, new_expr->module_name);
        xfmt_write_char(ctx, '.');
    }
    xfmt_write_str(ctx, new_expr->class_name);
    xfmt_emit_generic_args(ctx, new_expr->type_args, new_expr->type_arg_count);

    bool wrap = ctx->config && ctx->config->wrap_long_lines && new_expr->arg_count > 0;
    XfmtSnapshot snap;
    if (wrap)
        xfmt_snapshot(ctx, &snap);

    xfmt_write_char(ctx, '(');
    for (int i = 0; i < new_expr->arg_count; i++) {
        if (i > 0)
            xfmt_write_str(ctx, ", ");
        xfmt_emit_expression(ctx, new_expr->arguments[i]);
    }
    xfmt_write_char(ctx, ')');

    if (!wrap || xfmt_fits_on_line(ctx, &snap))
        return;

    xfmt_rollback(ctx, &snap);
    xfmt_write_char(ctx, '(');
    xfmt_write_newline(ctx);
    ctx->indent_level++;
    for (int i = 0; i < new_expr->arg_count; i++) {
        xfmt_write_indent(ctx);
        xfmt_emit_expression(ctx, new_expr->arguments[i]);
        if (ctx->config->multiline_trailing_comma || i < new_expr->arg_count - 1)
            xfmt_write_char(ctx, ',');
        xfmt_write_newline(ctx);
    }
    ctx->indent_level--;
    xfmt_write_indent(ctx);
    xfmt_write_char(ctx, ')');
}

static void fmt_template_string(XrFmtContext *ctx, AstNode *node) {
    // Backticks were dropped from the lexer; emit a canonical
    // double-quoted template via xfmt_emit_template_string. The
    // helper escapes literal parts and `$` to keep round-trip safe.
    xfmt_write_indent(ctx);
    xfmt_emit_template_string(ctx, node, xfmt_emit_expression);
}

// Emit `pattern (if (guard))?` for one match arm. Returns the number of
// characters written after the indent prefix on the current line, or -1
// if a newline was emitted (multi-line pattern -> cannot column-align).
static int fmt_match_arm_head(XrFmtContext *ctx, MatchArmNode *ma) {
    size_t saved_len = ctx->length;
    int indent_chars =
        ctx->config->use_tabs ? ctx->indent_level : ctx->indent_level * ctx->config->indent_size;

    xfmt_write_indent(ctx);
    xfmt_emit_expression(ctx, ma->pattern);
    if (ma->guard) {
        xfmt_write_str(ctx, " if (");
        xfmt_emit_expression(ctx, ma->guard);
        xfmt_write_char(ctx, ')');
    }

    for (size_t k = saved_len; k < ctx->length; k++) {
        if (ctx->output[k] == '\n')
            return -1;
    }
    int width = (int) (ctx->length - saved_len) - indent_chars;
    return width < 0 ? 0 : width;
}

static void fmt_match_expr(XrFmtContext *ctx, AstNode *node) {
    xfmt_write_indent(ctx);
    xfmt_write_str(ctx, "match (");
    xfmt_emit_expression(ctx, node->as.match_expr.expr);
    xfmt_write_str(ctx, ") {");
    xfmt_write_newline(ctx);
    ctx->indent_level++;

    int arm_count = node->as.match_expr.arm_count;
    AstNode **arms = node->as.match_expr.arms;

    // Pass 1: when alignment is enabled and there is more than one arm,
    // dry-run each arm's `pattern (guard)?` head into the output buffer,
    // measure its on-line width, then truncate the buffer back. Patterns
    // that emit a newline (e.g. attached leading comments, multi-line
    // destructure) report width=-1 and are not aligned individually.
    int *widths = NULL;
    int max_width = 0;
    bool align = ctx->config && ctx->config->align_match_arms && arm_count > 1;
    if (align) {
        widths = (int *) xr_malloc(sizeof(int) * (size_t) arm_count);
        for (int i = 0; i < arm_count; i++) {
            MatchArmNode *ma = &arms[i]->as.match_arm;
            size_t saved_len = ctx->length;
            int saved_col = ctx->column;
            int saved_line_start = ctx->line_start;

            int w = fmt_match_arm_head(ctx, ma);
            widths[i] = w;
            if (w > max_width)
                max_width = w;

            // Rollback. Trivia data is AST-attached and re-emitting it
            // in pass 2 is deterministic, so this dry-run is idempotent.
            ctx->length = saved_len;
            ctx->output[ctx->length] = '\0';
            ctx->column = saved_col;
            ctx->line_start = saved_line_start;
        }
    }

    // Pass 2: real emission. Pad each arm's head to max_width before
    // writing " -> body".
    for (int i = 0; i < arm_count; i++) {
        MatchArmNode *ma = &arms[i]->as.match_arm;

        int w = fmt_match_arm_head(ctx, ma);

        if (align && widths && widths[i] >= 0 && w >= 0) {
            int pad = max_width - w;
            for (int j = 0; j < pad; j++)
                xfmt_write_char(ctx, ' ');
        }
        xfmt_write_str(ctx, " -> ");

        if (ma->body->type == AST_BLOCK) {
            xfmt_emit_block(ctx, ma->body);
        } else {
            xfmt_emit_expression(ctx, ma->body);
        }
        xfmt_write_newline(ctx);
    }

    if (widths)
        xr_free(widths);

    ctx->indent_level--;
    xfmt_write_indent(ctx);
    xfmt_write_char(ctx, '}');
}

// ----------------------------------------------------------------------------
// Dispatch
// ----------------------------------------------------------------------------

void xfmt_emit_expression(XrFmtContext *ctx, AstNode *node) {
    if (!node)
        return;

    switch (node->type) {
        // Literals
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_BIGINT:
        case AST_LITERAL_STRING:
        case AST_LITERAL_REGEX:
        case AST_LITERAL_NULL:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
            fmt_literal(ctx, node);
            break;

        // Template string
        case AST_TEMPLATE_STRING:
            fmt_template_string(ctx, node);
            break;

        // Binary operators
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_EQ_STRICT:
        case AST_BINARY_NE_STRICT:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
            fmt_binary(ctx, node);
            break;

        // Unary operators
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            fmt_unary(ctx, node);
            break;

        // Grouping
        case AST_GROUPING:
            xfmt_write_indent(ctx);
            xfmt_write_char(ctx, '(');
            xfmt_emit_expression(ctx, node->as.grouping);
            xfmt_write_char(ctx, ')');
            break;

        // Variable
        case AST_VARIABLE:
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, node->as.variable.name);
            break;

        // Call
        case AST_CALL_EXPR:
            fmt_call(ctx, node);
            break;

        // New
        case AST_NEW_EXPR:
            fmt_new_expr(ctx, node);
            break;

        // This
        case AST_THIS_EXPR:
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, "this");
            break;

        // Super call
        case AST_SUPER_CALL: {
            xfmt_write_indent(ctx);
            SuperCallNode *sc = &node->as.super_call;
            if (sc->method_name) {
                xfmt_write_str(ctx, "super.");
                xfmt_write_str(ctx, sc->method_name);
            } else {
                xfmt_write_str(ctx, "super");
            }
            bool sc_wrap = ctx->config && ctx->config->wrap_long_lines && sc->arg_count > 0;
            XfmtSnapshot sc_snap;
            if (sc_wrap)
                xfmt_snapshot(ctx, &sc_snap);
            xfmt_write_char(ctx, '(');
            for (int i = 0; i < sc->arg_count; i++) {
                if (i > 0)
                    xfmt_write_str(ctx, ", ");
                xfmt_emit_expression(ctx, sc->arguments[i]);
            }
            xfmt_write_char(ctx, ')');
            if (sc_wrap && !xfmt_fits_on_line(ctx, &sc_snap)) {
                xfmt_rollback(ctx, &sc_snap);
                xfmt_write_char(ctx, '(');
                xfmt_write_newline(ctx);
                ctx->indent_level++;
                for (int i = 0; i < sc->arg_count; i++) {
                    xfmt_write_indent(ctx);
                    xfmt_emit_expression(ctx, sc->arguments[i]);
                    if (ctx->config->multiline_trailing_comma || i < sc->arg_count - 1)
                        xfmt_write_char(ctx, ',');
                    xfmt_write_newline(ctx);
                }
                ctx->indent_level--;
                xfmt_write_indent(ctx);
                xfmt_write_char(ctx, ')');
            }
            break;
        }

        // Array literal
        case AST_ARRAY_LITERAL: {
            xfmt_write_indent(ctx);
            ArrayLiteralNode *arr = &node->as.array_literal;
            bool arr_wrap = ctx->config && ctx->config->wrap_long_lines && arr->count > 0;
            XfmtSnapshot arr_snap;
            if (arr_wrap)
                xfmt_snapshot(ctx, &arr_snap);
            xfmt_write_char(ctx, '[');
            for (int i = 0; i < arr->count; i++) {
                if (i > 0)
                    xfmt_write_str(ctx, ", ");
                xfmt_emit_expression(ctx, arr->elements[i]);
            }
            xfmt_write_char(ctx, ']');
            if (arr_wrap && !xfmt_fits_on_line(ctx, &arr_snap)) {
                xfmt_rollback(ctx, &arr_snap);
                xfmt_write_char(ctx, '[');
                xfmt_write_newline(ctx);
                ctx->indent_level++;
                for (int i = 0; i < arr->count; i++) {
                    xfmt_write_indent(ctx);
                    xfmt_emit_expression(ctx, arr->elements[i]);
                    if (ctx->config->multiline_trailing_comma || i < arr->count - 1)
                        xfmt_write_char(ctx, ',');
                    xfmt_write_newline(ctx);
                }
                ctx->indent_level--;
                xfmt_write_indent(ctx);
                xfmt_write_char(ctx, ']');
            }
            break;
        }

        // Tuple literal — `()`, `(x,)`, `(a, b, ...)`. The trailing
        // comma on the arity-1 form is mandatory for roundtrip, since
        // `(x)` is a grouping, not a 1-tuple.
        case AST_TUPLE_LITERAL: {
            xfmt_write_indent(ctx);
            TupleLiteralNode *tup = &node->as.tuple_literal;
            bool tup_wrap = ctx->config && ctx->config->wrap_long_lines && tup->count > 0;
            XfmtSnapshot tup_snap;
            if (tup_wrap)
                xfmt_snapshot(ctx, &tup_snap);
            xfmt_write_char(ctx, '(');
            for (int i = 0; i < tup->count; i++) {
                if (i > 0)
                    xfmt_write_str(ctx, ", ");
                xfmt_emit_expression(ctx, tup->elements[i]);
            }
            if (tup->count == 1)
                xfmt_write_char(ctx, ',');
            xfmt_write_char(ctx, ')');
            if (tup_wrap && !xfmt_fits_on_line(ctx, &tup_snap)) {
                xfmt_rollback(ctx, &tup_snap);
                xfmt_write_char(ctx, '(');
                xfmt_write_newline(ctx);
                ctx->indent_level++;
                for (int i = 0; i < tup->count; i++) {
                    xfmt_write_indent(ctx);
                    xfmt_emit_expression(ctx, tup->elements[i]);
                    // Tuples always need a trailing comma in multi-line form;
                    // the arity-1 form requires it for parser disambiguation
                    // and the rest follows the same rule for consistency.
                    xfmt_write_char(ctx, ',');
                    xfmt_write_newline(ctx);
                }
                ctx->indent_level--;
                xfmt_write_indent(ctx);
                xfmt_write_char(ctx, ')');
            }
            break;
        }

        // Object literal
        case AST_OBJECT_LITERAL: {
            xfmt_write_indent(ctx);
            ObjectLiteralNode *obj = &node->as.object_literal;
            if (obj->count == 0) {
                xfmt_write_str(ctx, "{}");
                break;
            }
            bool obj_wrap = ctx->config && ctx->config->wrap_long_lines;
            XfmtSnapshot obj_snap;
            if (obj_wrap)
                xfmt_snapshot(ctx, &obj_snap);
            xfmt_write_str(ctx, "{ ");
            for (int i = 0; i < obj->count; i++) {
                if (i > 0)
                    xfmt_write_str(ctx, ", ");
                if (obj->computed && obj->computed[i]) {
                    xfmt_write_char(ctx, '[');
                    xfmt_emit_expression(ctx, obj->keys[i]);
                    xfmt_write_char(ctx, ']');
                } else {
                    xfmt_emit_expression(ctx, obj->keys[i]);
                }
                xfmt_write_str(ctx, ": ");
                xfmt_emit_expression(ctx, obj->values[i]);
            }
            xfmt_write_str(ctx, " }");
            if (obj_wrap && !xfmt_fits_on_line(ctx, &obj_snap)) {
                xfmt_rollback(ctx, &obj_snap);
                xfmt_write_char(ctx, '{');
                xfmt_write_newline(ctx);
                ctx->indent_level++;
                for (int i = 0; i < obj->count; i++) {
                    xfmt_write_indent(ctx);
                    if (obj->computed && obj->computed[i]) {
                        xfmt_write_char(ctx, '[');
                        xfmt_emit_expression(ctx, obj->keys[i]);
                        xfmt_write_char(ctx, ']');
                    } else {
                        xfmt_emit_expression(ctx, obj->keys[i]);
                    }
                    xfmt_write_str(ctx, ": ");
                    xfmt_emit_expression(ctx, obj->values[i]);
                    if (ctx->config->multiline_trailing_comma || i < obj->count - 1)
                        xfmt_write_char(ctx, ',');
                    xfmt_write_newline(ctx);
                }
                ctx->indent_level--;
                xfmt_write_indent(ctx);
                xfmt_write_char(ctx, '}');
            }
            break;
        }

        // Map literal — always emit with `#{ ... }` prefix and `: ` separator
        // so Json and Map are visually consistent while remaining
        // unambiguous through the leading `#` (task 082).
        case AST_MAP_LITERAL: {
            xfmt_write_indent(ctx);
            MapLiteralNode *map = &node->as.map_literal;
            if (map->count == 0) {
                xfmt_write_str(ctx, "#{}");
                break;
            }
            bool map_wrap = ctx->config && ctx->config->wrap_long_lines;
            XfmtSnapshot map_snap;
            if (map_wrap)
                xfmt_snapshot(ctx, &map_snap);
            xfmt_write_str(ctx, "#{ ");
            for (int i = 0; i < map->count; i++) {
                if (i > 0)
                    xfmt_write_str(ctx, ", ");
                xfmt_emit_expression(ctx, map->keys[i]);
                xfmt_write_str(ctx, ": ");
                xfmt_emit_expression(ctx, map->values[i]);
            }
            xfmt_write_str(ctx, " }");
            if (map_wrap && !xfmt_fits_on_line(ctx, &map_snap)) {
                xfmt_rollback(ctx, &map_snap);
                xfmt_write_str(ctx, "#{");
                xfmt_write_newline(ctx);
                ctx->indent_level++;
                for (int i = 0; i < map->count; i++) {
                    xfmt_write_indent(ctx);
                    xfmt_emit_expression(ctx, map->keys[i]);
                    xfmt_write_str(ctx, ": ");
                    xfmt_emit_expression(ctx, map->values[i]);
                    if (ctx->config->multiline_trailing_comma || i < map->count - 1)
                        xfmt_write_char(ctx, ',');
                    xfmt_write_newline(ctx);
                }
                ctx->indent_level--;
                xfmt_write_indent(ctx);
                xfmt_write_char(ctx, '}');
            }
            break;
        }

        // Set literal
        case AST_SET_LITERAL: {
            xfmt_write_indent(ctx);
            SetLiteralNode *set = &node->as.set_literal;
            bool set_wrap = ctx->config && ctx->config->wrap_long_lines && set->count > 0;
            XfmtSnapshot set_snap;
            if (set_wrap)
                xfmt_snapshot(ctx, &set_snap);
            xfmt_write_str(ctx, "#[");
            for (int i = 0; i < set->count; i++) {
                if (i > 0)
                    xfmt_write_str(ctx, ", ");
                xfmt_emit_expression(ctx, set->elements[i]);
            }
            xfmt_write_char(ctx, ']');
            if (set_wrap && !xfmt_fits_on_line(ctx, &set_snap)) {
                xfmt_rollback(ctx, &set_snap);
                xfmt_write_str(ctx, "#[");
                xfmt_write_newline(ctx);
                ctx->indent_level++;
                for (int i = 0; i < set->count; i++) {
                    xfmt_write_indent(ctx);
                    xfmt_emit_expression(ctx, set->elements[i]);
                    if (ctx->config->multiline_trailing_comma || i < set->count - 1)
                        xfmt_write_char(ctx, ',');
                    xfmt_write_newline(ctx);
                }
                ctx->indent_level--;
                xfmt_write_indent(ctx);
                xfmt_write_char(ctx, ']');
            }
            break;
        }

        // Index access
        case AST_INDEX_GET:
            xfmt_emit_expression(ctx, node->as.index_get.array);
            xfmt_write_char(ctx, '[');
            xfmt_emit_expression(ctx, node->as.index_get.index);
            xfmt_write_char(ctx, ']');
            break;

        // Index set
        case AST_INDEX_SET:
            xfmt_write_indent(ctx);
            xfmt_emit_expression(ctx, node->as.index_set.array);
            xfmt_write_char(ctx, '[');
            xfmt_emit_expression(ctx, node->as.index_set.index);
            xfmt_write_str(ctx, "] = ");
            xfmt_emit_expression(ctx, node->as.index_set.value);
            break;

        // Slice expression
        case AST_SLICE_EXPR:
            xfmt_emit_expression(ctx, node->as.slice_expr.source);
            xfmt_write_char(ctx, '[');
            if (node->as.slice_expr.start) {
                xfmt_emit_expression(ctx, node->as.slice_expr.start);
            }
            xfmt_write_char(ctx, ':');
            if (node->as.slice_expr.end) {
                xfmt_emit_expression(ctx, node->as.slice_expr.end);
            }
            xfmt_write_char(ctx, ']');
            break;

        // Member access
        case AST_MEMBER_ACCESS:
            xfmt_emit_expression(ctx, node->as.member_access.object);
            xfmt_write_char(ctx, '.');
            xfmt_write_str(ctx, node->as.member_access.name);
            break;

        // Member set
        case AST_MEMBER_SET:
            xfmt_write_indent(ctx);
            xfmt_emit_expression(ctx, node->as.member_set.object);
            xfmt_write_char(ctx, '.');
            xfmt_write_str(ctx, node->as.member_set.member);
            xfmt_write_str(ctx, " = ");
            xfmt_emit_expression(ctx, node->as.member_set.value);
            break;

        // Ternary
        case AST_TERNARY:
            xfmt_emit_expression(ctx, node->as.ternary.condition);
            xfmt_write_str(ctx, " ? ");
            xfmt_emit_expression(ctx, node->as.ternary.true_expr);
            xfmt_write_str(ctx, " : ");
            xfmt_emit_expression(ctx, node->as.ternary.false_expr);
            break;

        // Nullish coalesce
        case AST_NULLISH_COALESCE:
            xfmt_emit_expression(ctx, node->as.binary.left);
            xfmt_write_str(ctx, " ?? ");
            xfmt_emit_expression(ctx, node->as.binary.right);
            break;

        // Optional chain
        case AST_OPTIONAL_CHAIN: {
            OptionalChainNode *oc = &node->as.optional_chain;
            xfmt_emit_expression(ctx, oc->object);
            xfmt_write_str(ctx, "?.");
            if (oc->name) {
                xfmt_write_str(ctx, oc->name);
            } else if (oc->index) {
                xfmt_write_char(ctx, '[');
                xfmt_emit_expression(ctx, oc->index);
                xfmt_write_char(ctx, ']');
            }
            break;
        }

        // Range
        case AST_RANGE:
            xfmt_emit_expression(ctx, node->as.range.start);
            xfmt_write_str(ctx, "..");
            xfmt_emit_expression(ctx, node->as.range.end);
            break;

        // Is expression
        case AST_IS_EXPR:
            xfmt_emit_expression(ctx, node->as.is_expr.expr);
            xfmt_write_str(ctx, " is ");
            xfmt_emit_type(ctx, node->as.is_expr.type);
            break;

        // Assignment
        case AST_ASSIGNMENT:
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, node->as.assignment.name);
            xfmt_write_str(ctx, " = ");
            xfmt_emit_expression(ctx, node->as.assignment.value);
            break;

        // Compound assignment
        case AST_COMPOUND_ASSIGNMENT: {
            xfmt_write_indent(ctx);
            CompoundAssignmentNode *ca = &node->as.compound_assignment;
            if (ca->object) {
                xfmt_emit_expression(ctx, ca->object);
                xfmt_write_char(ctx, '.');
            }
            xfmt_write_str(ctx, ca->name);
            xfmt_write_space(ctx);
            xfmt_write_str(ctx, xfmt_compound_op(ca->op));
            xfmt_write_space(ctx);
            xfmt_emit_expression(ctx, ca->value);
            break;
        }

        // Increment
        case AST_INC:
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, node->as.inc.name);
            xfmt_write_str(ctx, "++");
            break;

        // Decrement
        case AST_DEC:
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, node->as.dec.name);
            xfmt_write_str(ctx, "--");
            break;

        // Function expression
        case AST_FUNCTION_EXPR: {
            FunctionDeclNode *fn = &node->as.function_expr;
            xfmt_write_str(ctx, "fn");
            xfmt_emit_generic_params(ctx, fn->type_params, fn->type_param_count);
            xfmt_write_char(ctx, '(');
            for (int i = 0; i < fn->param_count; i++) {
                if (i > 0)
                    xfmt_write_str(ctx, ", ");
                xfmt_write_str(ctx, fn->params[i]->name);
                if (fn->params[i]->type) {
                    xfmt_write_str(ctx, ": ");
                    xfmt_emit_type(ctx, fn->params[i]->type);
                }
            }
            xfmt_write_char(ctx, ')');
            if (fn->return_type) {
                xfmt_write_str(ctx, " -> ");
                xfmt_emit_type(ctx, fn->return_type);
            }
            xfmt_write_space(ctx);
            xfmt_emit_block(ctx, fn->body);
            break;
        }

        // Match expression
        case AST_MATCH_EXPR:
            fmt_match_expr(ctx, node);
            break;

        // Enum access
        case AST_ENUM_ACCESS:
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, node->as.enum_access.enum_name);
            xfmt_write_char(ctx, '.');
            xfmt_write_str(ctx, node->as.enum_access.member_name);
            break;

        // Enum convert
        case AST_ENUM_CONVERT:
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, node->as.enum_convert.enum_name);
            xfmt_write_char(ctx, '(');
            xfmt_emit_expression(ctx, node->as.enum_convert.value_expr);
            xfmt_write_char(ctx, ')');
            break;

        // Go expression
        case AST_GO_EXPR: {
            xfmt_write_indent(ctx);
            GoExprNode *go = &node->as.go_expr;
            xfmt_write_str(ctx, "go");
            if (go->name || go->priority) {
                xfmt_write_char(ctx, '(');
                int has_prev = 0;
                if (go->name) {
                    xfmt_write_str(ctx, "name: \"");
                    xfmt_write_str(ctx, go->name);
                    xfmt_write_char(ctx, '"');
                    has_prev = 1;
                }
                if (go->priority) {
                    if (has_prev)
                        xfmt_write_str(ctx, ", ");
                    xfmt_write_str(ctx, "priority: ");
                    xfmt_emit_expression(ctx, go->priority);
                }
                xfmt_write_str(ctx, ") ");
            } else {
                xfmt_write_space(ctx);
            }
            xfmt_emit_expression(ctx, go->expr);
            break;
        }

        // Await expression
        case AST_AWAIT_EXPR: {
            xfmt_write_indent(ctx);
            AwaitExprNode *aw = &node->as.await_expr;
            xfmt_write_str(ctx, "await");
            if (aw->is_any)
                xfmt_write_str(ctx, " any");
            if (aw->is_all)
                xfmt_write_str(ctx, " all");
            if (aw->timeout) {
                xfmt_write_str(ctx, "(timeout: ");
                xfmt_emit_expression(ctx, aw->timeout);
                xfmt_write_char(ctx, ')');
            }
            xfmt_write_space(ctx);
            xfmt_emit_expression(ctx, aw->expr);
            break;
        }

        // Channel
        case AST_CHANNEL_NEW:
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, "Channel(");
            if (node->as.channel_new.buffer_size) {
                xfmt_emit_expression(ctx, node->as.channel_new.buffer_size);
            }
            xfmt_write_char(ctx, ')');
            break;

        // Cancelled
        case AST_CANCELLED_EXPR:
            xfmt_write_indent(ctx);
            xfmt_write_str(ctx, "cancelled()");
            break;

        // Pattern nodes (for match)
        case AST_PATTERN_LITERAL:
            xfmt_emit_expression(ctx, node->as.pattern_literal.value);
            break;

        case AST_PATTERN_RANGE:
            xfmt_emit_expression(ctx, node->as.pattern_range.start);
            xfmt_write_str(ctx, "..");
            xfmt_emit_expression(ctx, node->as.pattern_range.end);
            break;

        case AST_PATTERN_WILDCARD:
            xfmt_write_indent(ctx);
            xfmt_write_char(ctx, '_');
            break;

        case AST_PATTERN_MULTI: {
            PatternMultiNode *pm = &node->as.pattern_multi;
            for (int i = 0; i < pm->count; i++) {
                if (i > 0)
                    xfmt_write_str(ctx, ", ");
                xfmt_emit_expression(ctx, pm->patterns[i]);
            }
            break;
        }

        // Force unwrap: expr!
        case AST_FORCE_UNWRAP:
            xfmt_emit_expression(ctx, node->as.unary.operand);
            xfmt_write_char(ctx, '!');
            break;

        // Try-modified expressions: try? expr / try! expr
        case AST_TRY_OPTIONAL:
            xfmt_write_str(ctx, "try? ");
            xfmt_emit_expression(ctx, node->as.unary.operand);
            break;
        case AST_TRY_FORCE:
            xfmt_write_str(ctx, "try! ");
            xfmt_emit_expression(ctx, node->as.unary.operand);
            break;

        // As expression: expr as Type / expr as Type?
        // The ? for safe cast is already part of the stored type ref.
        case AST_AS_EXPR:
            xfmt_emit_expression(ctx, node->as.as_expr.expr);
            xfmt_write_str(ctx, " as ");
            xfmt_emit_type(ctx, node->as.as_expr.type);
            break;

        // Struct literal: Name{field: val, ...}
        case AST_STRUCT_LITERAL: {
            StructLiteralNode *sl = &node->as.struct_literal;
            xfmt_write_str(ctx, sl->struct_name);
            xfmt_emit_generic_args(ctx, sl->type_args, sl->type_arg_count);
            bool sl_wrap = ctx->config && ctx->config->wrap_long_lines && sl->field_count > 0;
            XfmtSnapshot sl_snap;
            if (sl_wrap)
                xfmt_snapshot(ctx, &sl_snap);
            xfmt_write_char(ctx, '{');
            for (int i = 0; i < sl->field_count; i++) {
                if (i > 0)
                    xfmt_write_str(ctx, ", ");
                xfmt_write_str(ctx, sl->field_names[i]);
                xfmt_write_str(ctx, ": ");
                xfmt_emit_expression(ctx, sl->field_values[i]);
            }
            xfmt_write_char(ctx, '}');
            if (sl_wrap && !xfmt_fits_on_line(ctx, &sl_snap)) {
                xfmt_rollback(ctx, &sl_snap);
                xfmt_write_char(ctx, '{');
                xfmt_write_newline(ctx);
                ctx->indent_level++;
                for (int i = 0; i < sl->field_count; i++) {
                    xfmt_write_indent(ctx);
                    xfmt_write_str(ctx, sl->field_names[i]);
                    xfmt_write_str(ctx, ": ");
                    xfmt_emit_expression(ctx, sl->field_values[i]);
                    if (ctx->config->multiline_trailing_comma || i < sl->field_count - 1)
                        xfmt_write_char(ctx, ',');
                    xfmt_write_newline(ctx);
                }
                ctx->indent_level--;
                xfmt_write_indent(ctx);
                xfmt_write_char(ctx, '}');
            }
            break;
        }

        // Move expression: move var
        case AST_MOVE_EXPR:
            xfmt_write_str(ctx, "move ");
            xfmt_emit_expression(ctx, node->as.move_expr.expr);
            break;

        // Scope block used as expression
        case AST_SCOPE_BLOCK: {
            ScopeBlockNode *sb = &node->as.scope_block;
            if (sb->scope_mode == XR_SCOPE_LINKED)
                xfmt_write_str(ctx, "linked ");
            else if (sb->scope_mode == XR_SCOPE_SUPERVISOR)
                xfmt_write_str(ctx, "supervisor ");
            xfmt_write_str(ctx, "scope ");
            xfmt_emit_block(ctx, sb->body);
            break;
        }

        default:
            xfmt_write_indent(ctx);
            xfmt_write_fmt(ctx, "/* unsupported expr: %d */", node->type);
            break;
    }
}
