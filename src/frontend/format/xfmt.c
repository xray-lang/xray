/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfmt.c - AST-based code formatter implementation
 *
 * KEY CONCEPT:
 *   Parses source code to AST and regenerates formatted output.
 *   Handles space-sensitive generic syntax correctly.
 *   Supports all xray AST node types.
 */

#include "xfmt.h"
#include "../parser/xparse.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/value/xtype_names.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include "../../base/xmalloc.h"
/* Note: file moved from src/app/cli/ to src/frontend/format/ in CLI Phase 2 */

// Default configuration
XrFmtConfig xfmt_default_config = {
    .indent_size = 4,
    .use_tabs = 0,
    .max_line_length = 100,
    .trailing_newline = 1,
    .blank_lines_around_functions = 1,
    .blank_lines_around_classes = 1,
    .space_around_operators = 1,
    .space_after_comma = 1,
    .space_in_parentheses = 0,
    .brace_same_line = 1
};

// ============================================================================
// Buffer utilities
// ============================================================================

static void ensure_capacity(XrFmtContext *ctx, size_t additional) {
    if (ctx->length + additional >= ctx->capacity) {
        ctx->capacity = (ctx->capacity + additional) * 2;
        ctx->output = (char*)xr_realloc(ctx->output, ctx->capacity);
    }
}

static void write_char(XrFmtContext *ctx, char c) {
    ensure_capacity(ctx, 1);
    ctx->output[ctx->length++] = c;
    ctx->output[ctx->length] = '\0';

    if (c == '\n') {
        ctx->line_start = 1;
        ctx->column = 0;
    } else {
        ctx->column++;
    }
}

static void write_str(XrFmtContext *ctx, const char *str) {
    if (!str) return;
    size_t len = strlen(str);
    if (len == 0) return;
    ensure_capacity(ctx, len);
    memcpy(ctx->output + ctx->length, str, len);
    ctx->length += len;
    ctx->output[ctx->length] = '\0';
    // Update line/column tracking
    const char *last_nl = NULL;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\n') last_nl = str + i;
    }
    if (last_nl) {
        ctx->line_start = 1;
        ctx->column = (int)(str + len - 1 - last_nl);
    } else {
        ctx->column += (int)len;
    }
}

static void write_fmt(XrFmtContext *ctx, const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    write_str(ctx, buf);
}

static void write_indent(XrFmtContext *ctx) {
    if (!ctx->line_start) return;

    if (ctx->config->use_tabs) {
        for (int i = 0; i < ctx->indent_level; i++) {
            write_char(ctx, '\t');
        }
    } else {
        int spaces = ctx->indent_level * ctx->config->indent_size;
        for (int i = 0; i < spaces; i++) {
            write_char(ctx, ' ');
        }
    }
    ctx->line_start = 0;
}

static void write_newline(XrFmtContext *ctx) {
    write_char(ctx, '\n');
}

// ============================================================================
// Trivia (Comment) Output
// ============================================================================

// Write leading comments before a node
static void write_leading_comments(XrFmtContext *ctx, XrTrivia *trivia) {
    while (trivia) {
        write_indent(ctx);

        if (trivia->type == TRIVIA_LINE_COMMENT) {
            write_str(ctx, "//");
            // Write comment content (without delimiters)
            for (int i = 0; i < trivia->length; i++) {
                write_char(ctx, trivia->start[i]);
            }
            write_newline(ctx);
        } else if (trivia->type == TRIVIA_BLOCK_COMMENT) {
            write_str(ctx, "/*");
            // Write comment content
            for (int i = 0; i < trivia->length; i++) {
                write_char(ctx, trivia->start[i]);
            }
            write_str(ctx, "*/");
            write_newline(ctx);
        }

        trivia = trivia->next;
    }
}

static void write_space(XrFmtContext *ctx) {
    write_char(ctx, ' ');
}

// ============================================================================
// Initialize / Free
// ============================================================================

void xfmt_init(XrFmtContext *ctx, XrFmtConfig *config, XrayIsolate *X) {
    ctx->capacity = 4096;
    ctx->output = (char*)xr_malloc(ctx->capacity);
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
// Forward declarations
// ============================================================================

static void fmt_expression(XrFmtContext *ctx, AstNode *node);
static void fmt_statement(XrFmtContext *ctx, AstNode *node);
static void fmt_block(XrFmtContext *ctx, AstNode *node);
static void fmt_type(XrFmtContext *ctx, XrType *type);
static void fmt_pattern(XrFmtContext *ctx, XrDestructurePattern *pattern);

// ============================================================================
// Operators
// ============================================================================

static const char *get_binary_op(AstNodeType type) {
    switch (type) {
        case AST_BINARY_ADD: return "+";
        case AST_BINARY_SUB: return "-";
        case AST_BINARY_MUL: return "*";
        case AST_BINARY_DIV: return "/";
        case AST_BINARY_MOD: return "%";
        case AST_BINARY_BAND: return "&";
        case AST_BINARY_BOR: return "|";
        case AST_BINARY_BXOR: return "^";
        case AST_BINARY_LSHIFT: return "<<";
        case AST_BINARY_RSHIFT: return ">>";
        case AST_BINARY_EQ: return "==";
        case AST_BINARY_NE: return "!=";
        case AST_BINARY_EQ_STRICT: return "===";
        case AST_BINARY_NE_STRICT: return "!==";
        case AST_BINARY_LT: return "<";
        case AST_BINARY_LE: return "<=";
        case AST_BINARY_GT: return ">";
        case AST_BINARY_GE: return ">=";
        case AST_BINARY_AND: return "&&";
        case AST_BINARY_OR: return "||";
        default: return "?";
    }
}

static const char *get_compound_op(TokenType type) {
    switch (type) {
        case TK_PLUS_ASSIGN: return "+=";
        case TK_MINUS_ASSIGN: return "-=";
        case TK_MUL_ASSIGN: return "*=";
        case TK_DIV_ASSIGN: return "/=";
        case TK_MOD_ASSIGN: return "%=";
        case TK_AND_ASSIGN: return "&=";
        case TK_OR_ASSIGN: return "|=";
        case TK_XOR_ASSIGN: return "^=";
        case TK_LSHIFT_ASSIGN: return "<<=";
        case TK_RSHIFT_ASSIGN: return ">>=";
        default: return "?=";
    }
}

// ============================================================================
// Type formatting
// ============================================================================

static void fmt_type(XrFmtContext *ctx, XrType *type) {
    if (!type) return;
    const char *type_str = xr_type_to_string(type);
    if (type_str) {
        write_str(ctx, type_str);
    } else {
        write_str(ctx, "unknown");
    }
}

// Format generic type parameters <T, U: Constraint>
static void fmt_generic_params(XrFmtContext *ctx, XrGenericParam **params, int count) {
    if (count <= 0) return;

    write_char(ctx, '<');
    for (int i = 0; i < count; i++) {
        if (i > 0) write_str(ctx, ", ");
        write_str(ctx, params[i]->name);
        if (params[i]->constraint) {
            write_str(ctx, ": ");
            fmt_type(ctx, params[i]->constraint);
        }
    }
    write_char(ctx, '>');
}

// Format generic type arguments <int, string>
static void fmt_generic_args(XrFmtContext *ctx, XrType **args, int count) {
    if (count <= 0) return;

    write_char(ctx, '<');
    for (int i = 0; i < count; i++) {
        if (i > 0) write_str(ctx, ", ");
        fmt_type(ctx, args[i]);
    }
    write_char(ctx, '>');
}

// ============================================================================
// Pattern formatting (for destructuring)
// ============================================================================

static void fmt_pattern(XrFmtContext *ctx, XrDestructurePattern *pattern) {
    if (!pattern) return;

    switch (pattern->type) {
        case PATTERN_IDENTIFIER:
            write_str(ctx, pattern->as.identifier.name);
            if (pattern->as.identifier.type) {
                write_str(ctx, ": ");
                fmt_type(ctx, pattern->as.identifier.type);
            }
            break;

        case PATTERN_ARRAY:
            write_char(ctx, '[');
            for (int i = 0; i < pattern->as.array.element_count; i++) {
                if (i > 0) write_str(ctx, ", ");
                fmt_pattern(ctx, pattern->as.array.elements[i]);
            }
            write_char(ctx, ']');
            break;

        case PATTERN_OBJECT:
            write_char(ctx, '{');
            for (int i = 0; i < pattern->as.object.field_count; i++) {
                if (i > 0) write_str(ctx, ", ");
                fmt_pattern(ctx, pattern->as.object.patterns[i]);
            }
            write_char(ctx, '}');
            break;

        case PATTERN_SKIP:
            write_char(ctx, '_');
            break;
    }
}

// ============================================================================
// Expression formatting
// ============================================================================

static void fmt_literal(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);

    switch (node->type) {
        case AST_LITERAL_INT:
            write_fmt(ctx, "%lld", (long long)node->as.literal.raw_value.int_val);
            break;
        case AST_LITERAL_FLOAT:
            write_fmt(ctx, "%g", node->as.literal.raw_value.float_val);
            break;
        case AST_LITERAL_BIGINT:
            write_str(ctx, node->as.literal.raw_value.bigint_val);
            write_char(ctx, 'n');
            break;
        case AST_LITERAL_STRING:
            write_char(ctx, '"');
            write_str(ctx, node->as.literal.raw_value.string_val);
            write_char(ctx, '"');
            break;
        case AST_LITERAL_REGEX:
            write_char(ctx, '/');
            write_str(ctx, node->as.literal.raw_value.regex.pattern);
            write_char(ctx, '/');
            if (node->as.literal.raw_value.regex.flags) {
                write_str(ctx, node->as.literal.raw_value.regex.flags);
            }
            break;
        case AST_LITERAL_NULL:
            write_str(ctx, "null");
            break;
        case AST_LITERAL_TRUE:
            write_str(ctx, "true");
            break;
        case AST_LITERAL_FALSE:
            write_str(ctx, "false");
            break;
        default:
            break;
    }
}

static void fmt_binary(XrFmtContext *ctx, AstNode *node) {
    fmt_expression(ctx, node->as.binary.left);
    write_space(ctx);
    write_str(ctx, get_binary_op(node->type));
    write_space(ctx);
    fmt_expression(ctx, node->as.binary.right);
}

static void fmt_unary(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    switch (node->type) {
        case AST_UNARY_NEG: write_char(ctx, '-'); break;
        case AST_UNARY_NOT: write_char(ctx, '!'); break;
        case AST_UNARY_BNOT: write_char(ctx, '~'); break;
        default: break;
    }
    fmt_expression(ctx, node->as.unary.operand);
}

static void fmt_call(XrFmtContext *ctx, AstNode *node) {
    CallExprNode *call = &node->as.call_expr;
    fmt_expression(ctx, call->callee);
    fmt_generic_args(ctx, call->type_args, call->type_arg_count);
    write_char(ctx, '(');
    for (int i = 0; i < call->arg_count; i++) {
        if (i > 0) write_str(ctx, ", ");
        fmt_expression(ctx, call->arguments[i]);
    }
    write_char(ctx, ')');
}

static void fmt_new_expr(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    write_str(ctx, "new ");
    NewExprNode *new_expr = &node->as.new_expr;
    if (new_expr->module_name) {
        write_str(ctx, new_expr->module_name);
        write_char(ctx, '.');
    }
    write_str(ctx, new_expr->class_name);
    fmt_generic_args(ctx, new_expr->type_args, new_expr->type_arg_count);
    write_char(ctx, '(');
    for (int i = 0; i < new_expr->arg_count; i++) {
        if (i > 0) write_str(ctx, ", ");
        fmt_expression(ctx, new_expr->arguments[i]);
    }
    write_char(ctx, ')');
}

static void fmt_template_string(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    write_char(ctx, '`');
    TemplateStringNode *tmpl = &node->as.template_str;
    for (int i = 0; i < tmpl->part_count; i++) {
        AstNode *part = tmpl->parts[i];
        if (part->type == AST_LITERAL_STRING) {
            write_str(ctx, part->as.literal.raw_value.string_val);
        } else {
            write_str(ctx, "${");
            fmt_expression(ctx, part);
            write_char(ctx, '}');
        }
    }
    write_char(ctx, '`');
}

static void fmt_match_expr(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    write_str(ctx, "match ");
    fmt_expression(ctx, node->as.match_expr.expr);
    write_str(ctx, " {");
    write_newline(ctx);
    ctx->indent_level++;

    for (int i = 0; i < node->as.match_expr.arm_count; i++) {
        AstNode *arm = node->as.match_expr.arms[i];
        MatchArmNode *ma = &arm->as.match_arm;

        write_indent(ctx);
        fmt_expression(ctx, ma->pattern);
        if (ma->guard) {
            write_str(ctx, " if ");
            fmt_expression(ctx, ma->guard);
        }
        write_str(ctx, " => ");

        if (ma->body->type == AST_BLOCK) {
            fmt_block(ctx, ma->body);
        } else {
            fmt_expression(ctx, ma->body);
        }
        write_newline(ctx);
    }

    ctx->indent_level--;
    write_indent(ctx);
    write_char(ctx, '}');
}

static void fmt_expression(XrFmtContext *ctx, AstNode *node) {
    if (!node) return;

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
            write_indent(ctx);
            write_char(ctx, '(');
            fmt_expression(ctx, node->as.grouping);
            write_char(ctx, ')');
            break;

        // Variable
        case AST_VARIABLE:
            write_indent(ctx);
            write_str(ctx, node->as.variable.name);
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
            write_indent(ctx);
            write_str(ctx, "this");
            break;

        // Super call
        case AST_SUPER_CALL: {
            write_indent(ctx);
            SuperCallNode *sc = &node->as.super_call;
            if (sc->method_name) {
                write_str(ctx, "super.");
                write_str(ctx, sc->method_name);
            } else {
                write_str(ctx, "super");
            }
            write_char(ctx, '(');
            for (int i = 0; i < sc->arg_count; i++) {
                if (i > 0) write_str(ctx, ", ");
                fmt_expression(ctx, sc->arguments[i]);
            }
            write_char(ctx, ')');
            break;
        }

        // Array literal
        case AST_ARRAY_LITERAL: {
            write_indent(ctx);
            write_char(ctx, '[');
            ArrayLiteralNode *arr = &node->as.array_literal;
            for (int i = 0; i < arr->count; i++) {
                if (i > 0) write_str(ctx, ", ");
                fmt_expression(ctx, arr->elements[i]);
            }
            write_char(ctx, ']');
            break;
        }

        // Object literal
        case AST_OBJECT_LITERAL: {
            write_indent(ctx);
            ObjectLiteralNode *obj = &node->as.object_literal;
            if (obj->count == 0) {
                write_str(ctx, "{}");
            } else {
                write_str(ctx, "{ ");
                for (int i = 0; i < obj->count; i++) {
                    if (i > 0) write_str(ctx, ", ");
                    fmt_expression(ctx, obj->keys[i]);
                    write_str(ctx, ": ");
                    fmt_expression(ctx, obj->values[i]);
                }
                write_str(ctx, " }");
            }
            break;
        }

        // Map literal
        case AST_MAP_LITERAL: {
            write_indent(ctx);
            MapLiteralNode *map = &node->as.map_literal;
            if (map->count == 0) {
                write_str(ctx, "#{}");
            } else {
                write_str(ctx, "{ ");
                for (int i = 0; i < map->count; i++) {
                    if (i > 0) write_str(ctx, ", ");
                    fmt_expression(ctx, map->keys[i]);
                    write_str(ctx, " => ");
                    fmt_expression(ctx, map->values[i]);
                }
                write_str(ctx, " }");
            }
            break;
        }

        // Set literal
        case AST_SET_LITERAL: {
            write_indent(ctx);
            write_str(ctx, "#[");
            SetLiteralNode *set = &node->as.set_literal;
            for (int i = 0; i < set->count; i++) {
                if (i > 0) write_str(ctx, ", ");
                fmt_expression(ctx, set->elements[i]);
            }
            write_char(ctx, ']');
            break;
        }

        // Index access
        case AST_INDEX_GET:
            fmt_expression(ctx, node->as.index_get.array);
            write_char(ctx, '[');
            fmt_expression(ctx, node->as.index_get.index);
            write_char(ctx, ']');
            break;

        // Index set
        case AST_INDEX_SET:
            write_indent(ctx);
            fmt_expression(ctx, node->as.index_set.array);
            write_char(ctx, '[');
            fmt_expression(ctx, node->as.index_set.index);
            write_str(ctx, "] = ");
            fmt_expression(ctx, node->as.index_set.value);
            break;

        // Slice expression
        case AST_SLICE_EXPR:
            fmt_expression(ctx, node->as.slice_expr.source);
            write_char(ctx, '[');
            if (node->as.slice_expr.start) {
                fmt_expression(ctx, node->as.slice_expr.start);
            }
            write_char(ctx, ':');
            if (node->as.slice_expr.end) {
                fmt_expression(ctx, node->as.slice_expr.end);
            }
            write_char(ctx, ']');
            break;

        // Member access
        case AST_MEMBER_ACCESS:
            fmt_expression(ctx, node->as.member_access.object);
            write_char(ctx, '.');
            write_str(ctx, node->as.member_access.name);
            break;

        // Member set
        case AST_MEMBER_SET:
            write_indent(ctx);
            fmt_expression(ctx, node->as.member_set.object);
            write_char(ctx, '.');
            write_str(ctx, node->as.member_set.member);
            write_str(ctx, " = ");
            fmt_expression(ctx, node->as.member_set.value);
            break;

        // Ternary
        case AST_TERNARY:
            fmt_expression(ctx, node->as.ternary.condition);
            write_str(ctx, " ? ");
            fmt_expression(ctx, node->as.ternary.true_expr);
            write_str(ctx, " : ");
            fmt_expression(ctx, node->as.ternary.false_expr);
            break;

        // Nullish coalesce
        case AST_NULLISH_COALESCE:
            fmt_expression(ctx, node->as.binary.left);
            write_str(ctx, " ?? ");
            fmt_expression(ctx, node->as.binary.right);
            break;

        // Optional chain
        case AST_OPTIONAL_CHAIN: {
            OptionalChainNode *oc = &node->as.optional_chain;
            fmt_expression(ctx, oc->object);
            write_str(ctx, "?.");
            if (oc->name) {
                write_str(ctx, oc->name);
            } else if (oc->index) {
                write_char(ctx, '[');
                fmt_expression(ctx, oc->index);
                write_char(ctx, ']');
            }
            break;
        }

        // Range
        case AST_RANGE:
            fmt_expression(ctx, node->as.range.start);
            write_str(ctx, "..");
            fmt_expression(ctx, node->as.range.end);
            break;

        // Is expression
        case AST_IS_EXPR:
            fmt_expression(ctx, node->as.is_expr.expr);
            write_str(ctx, " is ");
            fmt_type(ctx, node->as.is_expr.type);
            break;

        // Assignment
        case AST_ASSIGNMENT:
            write_indent(ctx);
            write_str(ctx, node->as.assignment.name);
            write_str(ctx, " = ");
            fmt_expression(ctx, node->as.assignment.value);
            break;

        // Compound assignment
        case AST_COMPOUND_ASSIGNMENT: {
            write_indent(ctx);
            CompoundAssignmentNode *ca = &node->as.compound_assignment;
            if (ca->object) {
                fmt_expression(ctx, ca->object);
                write_char(ctx, '.');
            }
            write_str(ctx, ca->name);
            write_space(ctx);
            write_str(ctx, get_compound_op(ca->op));
            write_space(ctx);
            fmt_expression(ctx, ca->value);
            break;
        }

        // Increment
        case AST_INC:
            write_indent(ctx);
            write_str(ctx, node->as.inc.name);
            write_str(ctx, "++");
            break;

        // Decrement
        case AST_DEC:
            write_indent(ctx);
            write_str(ctx, node->as.dec.name);
            write_str(ctx, "--");
            break;

        // Function expression
        case AST_FUNCTION_EXPR: {
            FunctionDeclNode *fn = &node->as.function_expr;
            write_str(ctx, "fn");
            fmt_generic_params(ctx, fn->type_params, fn->type_param_count);
            write_char(ctx, '(');
            for (int i = 0; i < fn->param_count; i++) {
                if (i > 0) write_str(ctx, ", ");
                write_str(ctx, fn->params[i]->name);
                if (fn->params[i]->type) {
                    write_str(ctx, ": ");
                    fmt_type(ctx, fn->params[i]->type);
                }
            }
            write_char(ctx, ')');
            if (fn->return_type) {
                write_str(ctx, ": ");
                fmt_type(ctx, fn->return_type);
            }
            write_space(ctx);
            fmt_block(ctx, fn->body);
            break;
        }

        // Match expression
        case AST_MATCH_EXPR:
            fmt_match_expr(ctx, node);
            break;

        // Enum access
        case AST_ENUM_ACCESS:
            write_indent(ctx);
            write_str(ctx, node->as.enum_access.enum_name);
            write_char(ctx, '.');
            write_str(ctx, node->as.enum_access.member_name);
            break;

        // Enum convert
        case AST_ENUM_CONVERT:
            write_indent(ctx);
            write_str(ctx, node->as.enum_convert.enum_name);
            write_char(ctx, '(');
            fmt_expression(ctx, node->as.enum_convert.value_expr);
            write_char(ctx, ')');
            break;

        // Go expression
        case AST_GO_EXPR: {
            write_indent(ctx);
            GoExprNode *go = &node->as.go_expr;
            write_str(ctx, "go");
            if (go->name || go->priority) {
                write_char(ctx, '(');
                int has_prev = 0;
                if (go->name) {
                    write_str(ctx, "name: \"");
                    write_str(ctx, go->name);
                    write_char(ctx, '"');
                    has_prev = 1;
                }
                if (go->priority) {
                    if (has_prev) write_str(ctx, ", ");
                    write_str(ctx, "priority: ");
                    fmt_expression(ctx, go->priority);
                }
                write_str(ctx, ") ");
            } else {
                write_space(ctx);
            }
            fmt_expression(ctx, go->expr);
            break;
        }

        // Await expression
        case AST_AWAIT_EXPR:
        case AST_AWAIT_ALL_EXPR:
        case AST_AWAIT_ANY_EXPR: {
            write_indent(ctx);
            AwaitExprNode *aw = &node->as.await_expr;
            write_str(ctx, "await");
            if (aw->is_any) write_str(ctx, ".any");
            if (aw->is_all) write_str(ctx, ".all");
            write_space(ctx);
            fmt_expression(ctx, aw->expr);
            if (aw->timeout) {
                write_str(ctx, " timeout ");
                fmt_expression(ctx, aw->timeout);
            }
            break;
        }

        // Channel
        case AST_CHANNEL_NEW:
            write_indent(ctx);
            write_str(ctx, "Channel(");
            if (node->as.channel_new.buffer_size) {
                fmt_expression(ctx, node->as.channel_new.buffer_size);
            }
            write_char(ctx, ')');
            break;

        // Cancelled
        case AST_CANCELLED_EXPR:
            write_indent(ctx);
            write_str(ctx, "cancelled()");
            break;

        // Yield expression
        case AST_YIELD_EXPR:
            write_indent(ctx);
            write_str(ctx, "yield");
            if (node->as.yield_expr.value) {
                write_space(ctx);
                fmt_expression(ctx, node->as.yield_expr.value);
            }
            break;

        // Pattern nodes (for match)
        case AST_PATTERN_LITERAL:
            fmt_expression(ctx, node->as.pattern_literal.value);
            break;

        case AST_PATTERN_RANGE:
            fmt_expression(ctx, node->as.pattern_range.start);
            write_str(ctx, "..");
            fmt_expression(ctx, node->as.pattern_range.end);
            break;

        case AST_PATTERN_WILDCARD:
            write_indent(ctx);
            write_char(ctx, '_');
            break;

        case AST_PATTERN_MULTI: {
            PatternMultiNode *pm = &node->as.pattern_multi;
            for (int i = 0; i < pm->count; i++) {
                if (i > 0) write_str(ctx, ", ");
                fmt_expression(ctx, pm->patterns[i]);
            }
            break;
        }

        default:
            write_indent(ctx);
            write_fmt(ctx, "/* unsupported expr: %d */", node->type);
            break;
    }
}

// ============================================================================
// Statement formatting
// ============================================================================

static void fmt_var_decl(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    VarDeclNode *decl = &node->as.var_decl;

    if (decl->storage_mode == XR_STORAGE_SHARED) {
        write_str(ctx, "shared ");
    }
    write_str(ctx, decl->is_const ? "const " : "let ");
    write_str(ctx, decl->name);

    if (decl->type_annotation) {
        write_str(ctx, ": ");
        fmt_type(ctx, decl->type_annotation);
    }
    if (decl->initializer) {
        write_str(ctx, " = ");
        fmt_expression(ctx, decl->initializer);
    }
    write_newline(ctx);
}

static void fmt_multi_var_decl(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    MultiVarDeclNode *decl = &node->as.multi_var_decl;

    write_str(ctx, decl->is_const ? "const " : "let ");
    for (int i = 0; i < decl->name_count; i++) {
        if (i > 0) write_str(ctx, ", ");
        write_str(ctx, decl->names[i]);
    }
    write_str(ctx, " = ");
    for (int i = 0; i < decl->value_count; i++) {
        if (i > 0) write_str(ctx, ", ");
        fmt_expression(ctx, decl->values[i]);
    }
    write_newline(ctx);
}

static void fmt_destructure_decl(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    DestructureDeclNode *decl = &node->as.destructure_decl;

    write_str(ctx, decl->is_const ? "const " : "let ");
    fmt_pattern(ctx, decl->pattern);
    write_str(ctx, " = ");
    fmt_expression(ctx, decl->initializer);
    write_newline(ctx);
}

static void fmt_function_decl(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    FunctionDeclNode *fn = &node->as.function_decl;

    write_str(ctx, "fn ");
    write_str(ctx, fn->name);
    fmt_generic_params(ctx, fn->type_params, fn->type_param_count);

    write_char(ctx, '(');
    for (int i = 0; i < fn->param_count; i++) {
        if (i > 0) write_str(ctx, ", ");
        XrParamNode *param = fn->params[i];
        write_str(ctx, param->name);
        if (param->type) {
            write_str(ctx, ": ");
            fmt_type(ctx, param->type);
        }
        if (param->default_value) {
            write_str(ctx, " = ");
            fmt_expression(ctx, param->default_value);
        }
    }
    write_char(ctx, ')');

    if (fn->return_type) {
        write_str(ctx, ": ");
        fmt_type(ctx, fn->return_type);
    }

    write_space(ctx);
    fmt_block(ctx, fn->body);
    write_newline(ctx);
}

static void fmt_class_decl(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    ClassDeclNode *cls = &node->as.class_decl;

    if (cls->is_abstract) write_str(ctx, "abstract ");
    write_str(ctx, "class ");
    write_str(ctx, cls->name);
    fmt_generic_params(ctx, cls->type_params, cls->type_param_count);

    if (cls->super_name) {
        write_str(ctx, " extends ");
        if (cls->super_module) {
            write_str(ctx, cls->super_module);
            write_char(ctx, '.');
        }
        write_str(ctx, cls->super_name);
    }

    if (cls->interface_count > 0) {
        write_str(ctx, " implements ");
        for (int i = 0; i < cls->interface_count; i++) {
            if (i > 0) write_str(ctx, ", ");
            write_str(ctx, cls->interfaces[i]);
        }
    }

    write_str(ctx, " {");
    write_newline(ctx);
    ctx->indent_level++;

    // Fields
    for (int i = 0; i < cls->field_count; i++) {
        AstNode *field = cls->fields[i];
        FieldDeclNode *f = &field->as.field_decl;

        write_indent(ctx);
        if (f->is_private) write_str(ctx, "private ");
        if (f->is_static) write_str(ctx, "static ");
        write_str(ctx, f->name);
        if (f->field_type) {
            write_str(ctx, ": ");
            fmt_type(ctx, f->field_type);
        }
        if (f->initializer) {
            write_str(ctx, " = ");
            fmt_expression(ctx, f->initializer);
        }
        write_newline(ctx);
    }

    if (cls->field_count > 0 && cls->method_count > 0) {
        write_newline(ctx);
    }

    // Methods
    for (int i = 0; i < cls->method_count; i++) {
        AstNode *method = cls->methods[i];
        MethodDeclNode *m = &method->as.method_decl;

        write_indent(ctx);
        if (m->is_private) write_str(ctx, "private ");
        if (m->is_static) write_str(ctx, "static ");
        if (m->is_abstract) write_str(ctx, "abstract ");
        if (m->is_getter) write_str(ctx, "get ");
        if (m->is_setter) write_str(ctx, "set ");

        if (m->is_constructor) {
            write_str(ctx, XR_KEYWORD_CONSTRUCTOR);
        } else {
            write_str(ctx, m->name);
        }

        if (m->type_param_count > 0) {
            write_char(ctx, '<');
            for (int j = 0; j < m->type_param_count; j++) {
                if (j > 0) write_str(ctx, ", ");
                write_str(ctx, m->type_param_names[j]);
            }
            write_char(ctx, '>');
        }

        write_char(ctx, '(');
        for (int j = 0; j < m->param_count; j++) {
            if (j > 0) write_str(ctx, ", ");
            write_str(ctx, m->parameters[j]);
            if (m->param_types && m->param_types[j]) {
                write_str(ctx, ": ");
                fmt_type(ctx, m->param_types[j]);
            }
        }
        write_char(ctx, ')');

        if (m->return_type) {
            write_str(ctx, ": ");
            fmt_type(ctx, m->return_type);
        }

        if (m->is_abstract) {
            write_newline(ctx);
        } else if (m->body) {
            write_space(ctx);
            fmt_block(ctx, m->body);
            write_newline(ctx);
        }

        if (i < cls->method_count - 1) {
            write_newline(ctx);
        }
    }

    ctx->indent_level--;
    write_indent(ctx);
    write_char(ctx, '}');
    write_newline(ctx);
}

static void fmt_interface_decl(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    InterfaceDeclNode *iface = &node->as.interface_decl;

    write_str(ctx, "interface ");
    write_str(ctx, iface->name);

    if (iface->extends_count > 0) {
        write_str(ctx, " extends ");
        for (int i = 0; i < iface->extends_count; i++) {
            if (i > 0) write_str(ctx, ", ");
            write_str(ctx, iface->extends[i]);
        }
    }

    write_str(ctx, " {");
    write_newline(ctx);
    ctx->indent_level++;

    for (int i = 0; i < iface->method_count; i++) {
        AstNode *method = iface->methods[i];
        InterfaceMethodNode *m = &method->as.interface_method;

        write_indent(ctx);
        write_str(ctx, m->name);
        write_char(ctx, '(');
        for (int j = 0; j < m->param_count; j++) {
            if (j > 0) write_str(ctx, ", ");
            write_str(ctx, m->parameters[j]);
            if (m->param_types && m->param_types[j]) {
                write_str(ctx, ": ");
                fmt_type(ctx, m->param_types[j]);
            }
        }
        write_char(ctx, ')');
        if (m->return_type) {
            write_str(ctx, ": ");
            fmt_type(ctx, m->return_type);
        }
        write_newline(ctx);
    }

    ctx->indent_level--;
    write_indent(ctx);
    write_char(ctx, '}');
    write_newline(ctx);
}

static void fmt_enum_decl(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    EnumDeclNode *en = &node->as.enum_decl;

    write_str(ctx, "enum ");
    write_str(ctx, en->name);
    if (en->type_hint) {
        write_str(ctx, ": ");
        write_str(ctx, en->type_hint);
    }
    write_str(ctx, " {");
    write_newline(ctx);
    ctx->indent_level++;

    for (int i = 0; i < en->member_count; i++) {
        AstNode *member = en->members[i];
        EnumMemberNode *m = &member->as.enum_member;

        write_indent(ctx);
        write_str(ctx, m->name);
        if (m->value) {
            write_str(ctx, " = ");
            fmt_expression(ctx, m->value);
        }
        if (i < en->member_count - 1) {
            write_char(ctx, ',');
        }
        write_newline(ctx);
    }

    ctx->indent_level--;
    write_indent(ctx);
    write_char(ctx, '}');
    write_newline(ctx);
}

static void fmt_type_alias(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    TypeAliasNode *ta = &node->as.type_alias;

    write_str(ctx, "type ");
    write_str(ctx, ta->name);
    write_str(ctx, " = { ");

    for (int i = 0; i < ta->field_count; i++) {
        if (i > 0) write_str(ctx, ", ");
        write_str(ctx, ta->field_names[i]);
        if (ta->field_optional && ta->field_optional[i]) {
            write_char(ctx, '?');
        }
        write_str(ctx, ": ");
        fmt_type(ctx, ta->field_types[i]);
    }

    write_str(ctx, " }");
    write_newline(ctx);
}

static void fmt_if_stmt(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    write_str(ctx, "if (");
    fmt_expression(ctx, node->as.if_stmt.condition);
    write_str(ctx, ") ");
    fmt_block(ctx, node->as.if_stmt.then_branch);

    if (node->as.if_stmt.else_branch) {
        write_str(ctx, " else ");
        if (node->as.if_stmt.else_branch->type == AST_IF_STMT) {
            ctx->line_start = 0;
            fmt_if_stmt(ctx, node->as.if_stmt.else_branch);
            return;
        } else {
            fmt_block(ctx, node->as.if_stmt.else_branch);
        }
    }
    write_newline(ctx);
}

static void fmt_while_stmt(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    write_str(ctx, "while (");
    fmt_expression(ctx, node->as.while_stmt.condition);
    write_str(ctx, ") ");
    fmt_block(ctx, node->as.while_stmt.body);
    write_newline(ctx);
}

static void fmt_for_stmt(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    write_str(ctx, "for (");

    ForStmtNode *f = &node->as.for_stmt;
    if (f->initializer) {
        // Don't add newline for initializer
        int old_line_start = ctx->line_start;
        ctx->line_start = 0;
        if (f->initializer->type == AST_VAR_DECL || f->initializer->type == AST_CONST_DECL) {
            VarDeclNode *decl = &f->initializer->as.var_decl;
            write_str(ctx, decl->is_const ? "const " : "let ");
            write_str(ctx, decl->name);
            if (decl->initializer) {
                write_str(ctx, " = ");
                fmt_expression(ctx, decl->initializer);
            }
        } else {
            fmt_expression(ctx, f->initializer);
        }
        ctx->line_start = old_line_start;
    }
    write_str(ctx, "; ");
    if (f->condition) {
        fmt_expression(ctx, f->condition);
    }
    write_str(ctx, "; ");
    if (f->increment) {
        fmt_expression(ctx, f->increment);
    }
    write_str(ctx, ") ");
    fmt_block(ctx, f->body);
    write_newline(ctx);
}

static void fmt_for_in_stmt(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    write_str(ctx, "for (");

    ForInStmtNode *f = &node->as.for_in_stmt;
    if (f->is_keyvalue) {
        write_str(ctx, f->item_name);
        write_str(ctx, ", ");
        write_str(ctx, f->value_name);
    } else {
        write_str(ctx, f->item_name);
    }

    write_str(ctx, " in ");
    fmt_expression(ctx, f->collection);
    write_str(ctx, ") ");
    fmt_block(ctx, f->body);
    write_newline(ctx);
}

static void fmt_try_catch(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    TryCatchNode *tc = &node->as.try_catch;

    write_str(ctx, "try ");
    fmt_block(ctx, tc->try_body);

    if (tc->catch_body) {
        write_str(ctx, " catch");
        if (tc->catch_var) {
            write_str(ctx, " (");
            write_str(ctx, tc->catch_var);
            write_char(ctx, ')');
        }
        write_space(ctx);
        fmt_block(ctx, tc->catch_body);
    }

    if (tc->finally_body) {
        write_str(ctx, " finally ");
        fmt_block(ctx, tc->finally_body);
    }

    write_newline(ctx);
}

static void fmt_select_stmt(XrFmtContext *ctx, AstNode *node) {
    write_indent(ctx);
    write_str(ctx, "select {");
    write_newline(ctx);
    ctx->indent_level++;

    SelectStmtNode *sel = &node->as.select_stmt;
    for (int i = 0; i < sel->case_count; i++) {
        AstNode *c = sel->cases[i];
        SelectCaseNode *sc = &c->as.select_case;

        write_indent(ctx);
        if (sc->is_default) {
            write_str(ctx, "default");
        } else if (sc->is_timeout) {
            write_str(ctx, "timeout ");
            fmt_expression(ctx, sc->value);
        } else if (sc->is_send) {
            fmt_expression(ctx, sc->value);
            write_str(ctx, " -> ");
            fmt_expression(ctx, sc->channel);
        } else {
            write_str(ctx, sc->var_name);
            write_str(ctx, " from ");
            fmt_expression(ctx, sc->channel);
        }
        write_str(ctx, " => ");

        if (sc->body->type == AST_BLOCK) {
            fmt_block(ctx, sc->body);
        } else {
            fmt_expression(ctx, sc->body);
        }
        write_newline(ctx);
    }

    ctx->indent_level--;
    write_indent(ctx);
    write_char(ctx, '}');
    write_newline(ctx);
}

static void fmt_block(XrFmtContext *ctx, AstNode *node) {
    if (!node || node->type != AST_BLOCK) {
        write_str(ctx, "{}");
        return;
    }

    write_char(ctx, '{');
    write_newline(ctx);
    ctx->indent_level++;

    BlockNode *block = &node->as.block;
    for (int i = 0; i < block->count; i++) {
        fmt_statement(ctx, block->statements[i]);
    }

    ctx->indent_level--;
    write_indent(ctx);
    write_char(ctx, '}');
}

static void fmt_statement(XrFmtContext *ctx, AstNode *node) {
    if (!node) return;

    // Output leading comments
    if (node->leading_comments) {
        write_leading_comments(ctx, node->leading_comments);
    }

    switch (node->type) {
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            fmt_var_decl(ctx, node);
            break;

        case AST_MULTI_VAR_DECL:
            fmt_multi_var_decl(ctx, node);
            break;

        case AST_DESTRUCTURE_DECL:
            fmt_destructure_decl(ctx, node);
            break;

        case AST_DESTRUCTURE_ASSIGN:
            write_indent(ctx);
            fmt_pattern(ctx, node->as.destructure_assign.pattern);
            write_str(ctx, " = ");
            fmt_expression(ctx, node->as.destructure_assign.value);
            write_newline(ctx);
            break;

        case AST_MULTI_ASSIGN: {
            write_indent(ctx);
            MultiAssignNode *ma = &node->as.multi_assign;
            for (int i = 0; i < ma->target_count; i++) {
                if (i > 0) write_str(ctx, ", ");
                fmt_expression(ctx, ma->targets[i]);
            }
            write_str(ctx, " = ");
            for (int i = 0; i < ma->value_count; i++) {
                if (i > 0) write_str(ctx, ", ");
                fmt_expression(ctx, ma->values[i]);
            }
            write_newline(ctx);
            break;
        }

        case AST_FUNCTION_DECL:
            fmt_function_decl(ctx, node);
            break;

        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
            fmt_class_decl(ctx, node);
            break;

        case AST_INTERFACE_DECL:
            fmt_interface_decl(ctx, node);
            break;

        case AST_ENUM_DECL:
            fmt_enum_decl(ctx, node);
            break;

        case AST_TYPE_ALIAS:
            fmt_type_alias(ctx, node);
            break;

        case AST_IF_STMT:
            fmt_if_stmt(ctx, node);
            break;

        case AST_WHILE_STMT:
            fmt_while_stmt(ctx, node);
            break;

        case AST_FOR_STMT:
            fmt_for_stmt(ctx, node);
            break;

        case AST_FOR_IN_STMT:
            fmt_for_in_stmt(ctx, node);
            break;

        case AST_RETURN_STMT: {
            write_indent(ctx);
            write_str(ctx, "return");
            ReturnStmtNode *ret = &node->as.return_stmt;
            if (ret->value_count > 0) {
                write_space(ctx);
                for (int i = 0; i < ret->value_count; i++) {
                    if (i > 0) write_str(ctx, ", ");
                    fmt_expression(ctx, ret->values[i]);
                }
            }
            write_newline(ctx);
            break;
        }

        case AST_BREAK_STMT:
            write_indent(ctx);
            write_str(ctx, "break");
            write_newline(ctx);
            break;

        case AST_CONTINUE_STMT:
            write_indent(ctx);
            write_str(ctx, "continue");
            write_newline(ctx);
            break;

        case AST_TRY_CATCH:
            fmt_try_catch(ctx, node);
            break;

        case AST_THROW_STMT:
            write_indent(ctx);
            write_str(ctx, "throw ");
            fmt_expression(ctx, node->as.throw_stmt.expression);
            write_newline(ctx);
            break;

        case AST_IMPORT_STMT: {
            write_indent(ctx);
            ImportStmtNode *imp = &node->as.import_stmt;

            if (imp->member_count > 0) {
                write_str(ctx, "import { ");
                for (int i = 0; i < imp->member_count; i++) {
                    if (i > 0) write_str(ctx, ", ");
                    write_str(ctx, imp->members[i].name);
                    if (imp->members[i].alias) {
                        write_str(ctx, " as ");
                        write_str(ctx, imp->members[i].alias);
                    }
                }
                write_str(ctx, " } from ");
            } else {
                write_str(ctx, "import ");
            }

            if (imp->import_type == IMPORT_FILE || imp->import_type == IMPORT_DIR) {
                write_char(ctx, '"');
                write_str(ctx, imp->module_name);
                write_char(ctx, '"');
            } else {
                write_str(ctx, imp->module_name);
            }

            // Only output "as alias" if alias is different from module name
            if (imp->alias && imp->member_count == 0 &&
                strcmp(imp->alias, imp->module_name) != 0) {
                write_str(ctx, " as ");
                write_str(ctx, imp->alias);
            }
            write_newline(ctx);
            break;
        }

        case AST_EXPORT_STMT: {
            write_indent(ctx);
            write_str(ctx, "export ");
            ExportStmtNode *exp = &node->as.export_stmt;

            if (exp->declaration) {
                ctx->line_start = 0;
                fmt_statement(ctx, exp->declaration);
            } else if (exp->export_count > 0) {
                for (int i = 0; i < exp->export_count; i++) {
                    if (i > 0) write_str(ctx, ", ");
                    write_str(ctx, exp->export_names[i]);
                }
                write_newline(ctx);
            }
            break;
        }

        case AST_SELECT_STMT:
            fmt_select_stmt(ctx, node);
            break;

        case AST_DEFER_STMT:
            write_indent(ctx);
            write_str(ctx, "defer ");
            fmt_expression(ctx, node->as.defer_stmt.expr);
            write_newline(ctx);
            break;

        case AST_SCOPE_BLOCK:
            write_indent(ctx);
            write_str(ctx, "scope ");
            fmt_block(ctx, node->as.scope_block.body);
            write_newline(ctx);
            break;

        case AST_YIELD_STMT:
            write_indent(ctx);
            write_str(ctx, "yield");
            write_newline(ctx);
            break;

        case AST_BLOCK:
            fmt_block(ctx, node);
            write_newline(ctx);
            break;

        case AST_EXPR_STMT:
            write_indent(ctx);
            fmt_expression(ctx, node->as.expr_stmt);
            write_newline(ctx);
            break;

        case AST_PRINT_STMT: {
            write_indent(ctx);
            write_str(ctx, "print(");
            PrintNode *p = &node->as.print_stmt;
            for (int i = 0; i < p->expr_count; i++) {
                if (i > 0) write_str(ctx, ", ");
                fmt_expression(ctx, p->exprs[i]);
            }
            write_str(ctx, ")");
            write_newline(ctx);
            break;
        }

        case AST_ASSIGNMENT:
            write_indent(ctx);
            write_str(ctx, node->as.assignment.name);
            write_str(ctx, " = ");
            fmt_expression(ctx, node->as.assignment.value);
            write_newline(ctx);
            break;

        case AST_MEMBER_SET:
            write_indent(ctx);
            fmt_expression(ctx, node->as.member_set.object);
            write_char(ctx, '.');
            write_str(ctx, node->as.member_set.member);
            write_str(ctx, " = ");
            fmt_expression(ctx, node->as.member_set.value);
            write_newline(ctx);
            break;

        case AST_INDEX_SET:
            write_indent(ctx);
            fmt_expression(ctx, node->as.index_set.array);
            write_char(ctx, '[');
            fmt_expression(ctx, node->as.index_set.index);
            write_str(ctx, "] = ");
            fmt_expression(ctx, node->as.index_set.value);
            write_newline(ctx);
            break;

        case AST_COMPOUND_ASSIGNMENT: {
            write_indent(ctx);
            CompoundAssignmentNode *ca = &node->as.compound_assignment;
            if (ca->object) {
                fmt_expression(ctx, ca->object);
                write_char(ctx, '.');
            }
            write_str(ctx, ca->name);
            write_space(ctx);
            write_str(ctx, get_compound_op(ca->op));
            write_space(ctx);
            fmt_expression(ctx, ca->value);
            write_newline(ctx);
            break;
        }

        case AST_INC:
            write_indent(ctx);
            write_str(ctx, node->as.inc.name);
            write_str(ctx, "++");
            write_newline(ctx);
            break;

        case AST_DEC:
            write_indent(ctx);
            write_str(ctx, node->as.dec.name);
            write_str(ctx, "--");
            write_newline(ctx);
            break;

        default:
            write_indent(ctx);
            write_fmt(ctx, "/* unsupported statement: %d */", node->type);
            write_newline(ctx);
            break;
    }
}

// ============================================================================
// Program formatting
// ============================================================================

static void fmt_program(XrFmtContext *ctx, AstNode *node) {
    if (!node || node->type != AST_PROGRAM) return;

    // Output file-level leading comments
    if (node->leading_comments) {
        write_leading_comments(ctx, node->leading_comments);
    }

    ProgramNode *prog = &node->as.program;
    int last_was_decl = 0;

    for (int i = 0; i < prog->count; i++) {
        AstNode *stmt = prog->statements[i];

        int is_decl = (stmt->type == AST_FUNCTION_DECL ||
                       stmt->type == AST_CLASS_DECL ||
                       stmt->type == AST_STRUCT_DECL ||
                       stmt->type == AST_INTERFACE_DECL ||
                       stmt->type == AST_ENUM_DECL);

        if (i > 0 && (is_decl || last_was_decl)) {
            write_newline(ctx);
        }

        fmt_statement(ctx, stmt);
        last_was_decl = is_decl;
    }
}

// ============================================================================
// Public API
// ============================================================================

void xfmt_node(XrFmtContext *ctx, AstNode *node) {
    if (!node) return;

    if (node->type == AST_PROGRAM) {
        fmt_program(ctx, node);
    } else {
        fmt_statement(ctx, node);
    }
}

void xfmt_type(XrFmtContext *ctx, XrType *type) {
    fmt_type(ctx, type);
}

char *xfmt_format_ast(AstNode *ast, XrFmtConfig *config, XrayIsolate *X) {
    XrFmtContext ctx;
    xfmt_init(&ctx, config, X);

    xfmt_node(&ctx, ast);

    if (config && config->trailing_newline && ctx.length > 0 &&
        ctx.output[ctx.length - 1] != '\n') {
        write_char(&ctx, '\n');
    }

    char *result = ctx.output;
    ctx.output = NULL;
    return result;
}
