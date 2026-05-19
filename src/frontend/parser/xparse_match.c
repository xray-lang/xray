/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_match.c - Match expression parsing
 *
 * KEY CONCEPT:
 *   Parses match expressions with pattern matching (literal, range, multi-value, wildcard).
 *   Extracted from xparse.c for maintainability.
 */

#include "xparse_internal.h"
#include "../../base/xchecks.h"

/*
 * Parse pattern
 * Supports:
 * - Literal pattern: 1, "hello", true, HttpStatus.OK
 * - Range pattern: 1..10
 * - Multi-value pattern (alternation): 1, 2, 3
 * - Wildcard pattern: _
 * - Tuple pattern: (a, b) / (0, _) / ((x, y), z)
 */
static AstNode *parse_pattern(Parser *parser);
static AstNode *parse_pattern_single(Parser *parser);

/* Parse a positional tuple pattern starting at the current `(` token.
 * `()`, `(p,)` and `(p1, p2, ...)` are all accepted; sub-patterns
 * recurse through parse_pattern_single (NOT the alternation form), so
 * a comma inside the tuple terminates the element instead of starting
 * a `1 | 2 | 3`-style alternation. */
static AstNode *parse_tuple_pattern(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_tuple_pattern: NULL parser");
    int line = parser->current.line;
    xr_parser_consume(parser, TK_LPAREN, "expected '(' to start tuple pattern");

    AstNode **patterns = NULL;
    int count = 0;
    int capacity = 0;

    while (!xr_parser_check(parser, TK_RPAREN) && !xr_parser_check(parser, TK_EOF)) {
        if (count >= capacity) {
            int old_capacity = capacity;
            capacity = (capacity == 0) ? 4 : capacity * 2;
            AstNode **_new =
                (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), (size_t) capacity);
            if (old_capacity > 0 && patterns)
                memcpy(_new, patterns, sizeof(AstNode *) * (size_t) old_capacity);
            patterns = _new;
        }

        AstNode *sub = parse_pattern_single(parser);
        if (!sub)
            return NULL;
        patterns[count++] = sub;

        if (xr_parser_check(parser, TK_RPAREN))
            break;
        if (!xr_parser_match(parser, TK_COMMA)) {
            xr_parser_error(parser, "expected ',' or ')' in tuple pattern");
            return NULL;
        }
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')' to close tuple pattern");
    return xr_ast_pattern_tuple(parser->X, patterns, count, line);
}

/* Parse exactly one pattern atom — wildcard, tuple destructure,
 * literal, range, or binding identifier. Crucially this does NOT
 * collapse a trailing `, …` into an alternation pattern; the caller
 * (a tuple-element loop or a match-arm head) decides how to interpret
 * the comma. */
static AstNode *parse_pattern_single(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_pattern_single: NULL parser");
    int line = parser->current.line;

    if (xr_parser_match(parser, TK_UNDERSCORE)) {
        return xr_ast_pattern_wildcard(parser->X, line);
    }

    if (xr_parser_check(parser, TK_LPAREN)) {
        return parse_tuple_pattern(parser);
    }

    AstNode *first = xr_parse_precedence(parser, PREC_CALL);
    if (!first) {
        xr_parser_error(parser, "expected pattern");
        return NULL;
    }

    if (xr_parser_match(parser, TK_RANGE)) {
        AstNode *end = xr_parse_precedence(parser, PREC_CALL);
        if (!end) {
            xr_parser_error(parser, "expected range end value");
            return NULL;
        }
        return xr_ast_pattern_range(parser->X, first, end, line);
    }

    /* ADT variant destructure: Shape.Circle(r, ...)
     * The Pratt parser already consumed `Shape.Circle(r)` as AST_CALL
     * whose callee is AST_MEMBER_ACCESS. Unwrap into AST_PATTERN_ADT
     * with the callee as variant and call args as sub-patterns. */
    if (first->type == AST_CALL_EXPR) {
        AstNode *callee = first->as.call_expr.callee;
        if (callee && (callee->type == AST_MEMBER_ACCESS || callee->type == AST_ENUM_ACCESS)) {
            int argc = first->as.call_expr.arg_count;
            AstNode **sub_pats = NULL;
            if (argc > 0) {
                sub_pats =
                    (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), (size_t) argc);
                for (int i = 0; i < argc; i++) {
                    AstNode *arg = first->as.call_expr.arguments[i];
                    /* Wrap each arg as a pattern node:
                     * variable → binding, wildcard stays, literal → literal */
                    if (arg->type == AST_VARIABLE && strcmp(arg->as.variable.name, "_") == 0)
                        sub_pats[i] = xr_ast_pattern_wildcard(parser->X, arg->line);
                    else if (arg->type == AST_VARIABLE)
                        sub_pats[i] = xr_ast_pattern_literal(parser->X, arg, arg->line);
                    else
                        sub_pats[i] = xr_ast_pattern_literal(parser->X, arg, arg->line);
                }
            }
            return xr_ast_pattern_adt(parser->X, callee, sub_pats, argc, line);
        }
    }

    return xr_ast_pattern_literal(parser->X, first, line);
}

/* Top-level match-arm pattern: parse one atom, then optionally fold
 * in further atoms separated by `,` into an alternation pattern up
 * to the `=>`. This is the only place where a top-level comma starts
 * an alternation; tuple sub-elements use parse_pattern_single. */
static AstNode *parse_pattern(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_pattern: NULL parser");
    int line = parser->current.line;

    AstNode *first = parse_pattern_single(parser);
    if (!first)
        return NULL;

    if (!xr_parser_check(parser, TK_COMMA))
        return first;

    /* Alternation: `p1, p2, p3 => ...` — gather until the arrow.
     * The first atom may itself be a tuple/range/wildcard pattern. */
    AstNode **patterns = (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), 16);
    int count = 0;
    int capacity = 16;
    patterns[count++] = first;

    while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_ARROW) &&
           !xr_parser_check(parser, TK_IF)) {
        if (count >= capacity) {
            int old_capacity = capacity;
            capacity *= 2;
            AstNode **_new_patterns =
                (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), (size_t) capacity);
            if (old_capacity > 0 && patterns)
                memcpy(_new_patterns, patterns, sizeof(AstNode *) * (size_t) old_capacity);
            patterns = _new_patterns;
        }

        AstNode *next = parse_pattern_single(parser);
        if (!next) {
            xr_parser_error(parser, "expected pattern value");
            break;
        }
        patterns[count++] = next;
    }

    return xr_ast_pattern_multi(parser->X, patterns, count, line);
}

/*
 * Parse match arm
 * pattern => expression
 * pattern if guard => expression
 * pattern => { block }
 */
static AstNode *parse_match_arm(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_match_arm: NULL parser");
    int line = parser->current.line;

    // Parse pattern
    AstNode *pattern = parse_pattern(parser);
    if (!pattern) {
        return NULL;
    }

    // Optional guard condition: if (expr)
    AstNode *guard = NULL;
    if (xr_parser_match(parser, TK_IF)) {
        // Consume left paren
        xr_parser_consume(parser, TK_LPAREN, "expected '(' after if");

        // Parse guard condition expression
        guard = xr_parse_expression(parser);
        if (!guard) {
            xr_parser_error(parser, "expected guard condition expression");
            return NULL;
        }

        // Consume right paren
        xr_parser_consume(parser, TK_RPAREN, "expected ')' after guard condition");
    }

    // Consume arrow
    xr_parser_consume(parser, TK_ARROW, "expected '=>' after pattern");

    // Parse arm body
    AstNode *body = NULL;
    if (xr_parser_match(parser, TK_LBRACE)) {
        // Code block
        body = xr_parse_block(parser);
    } else {
        // Single expression
        body = xr_parse_expression(parser);
    }

    if (!body) {
        xr_parser_error(parser, "expected expression or code block");
        return NULL;
    }

    return xr_ast_match_arm(parser->X, pattern, guard, body, line);
}

/*
 * Parse match expression (prefix)
 * match (x) {
 *     1 => "one",
 *     2 => "two",
 *     _ => "other"
 * }
 */
AstNode *xr_parse_match_expr(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_match_expr: NULL parser");
    int line = parser->previous.line;  // match keyword already consumed

    // Scrutinee must be parenthesised, mirroring `if (...)`, `for (...)`,
    // `while (...)`. Required parens also disambiguate tuple scrutinees
    // (`match (x, y) { (a, b) => ... }`) from a sequence of bare names.
    xr_parser_consume(parser, TK_LPAREN, "expected '(' after 'match'");

    AstNode *expr = xr_parse_expression(parser);
    if (!expr) {
        xr_parser_error(parser, "expected match expression");
        return NULL;
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')' after match scrutinee");
    xr_parser_consume(parser, TK_LBRACE, "expected '{' after match scrutinee");

    // Parse all arms
    AstNode **arms = (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), 16);
    int arm_count = 0;
    int capacity = 16;

    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        // Expand capacity
        if (arm_count >= capacity) {
            int old_capacity = capacity;
            capacity *= 2;
            AstNode **_new_arms =
                (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), (size_t) capacity);
            if (old_capacity > 0 && arms)
                memcpy(_new_arms, arms, sizeof(AstNode *) * (size_t) old_capacity);
            arms = _new_arms;
        }

        // Parse one arm
        AstNode *arm = parse_match_arm(parser);
        if (!arm) {
            // Error recovery: skip to next arm or }
            while (!xr_parser_check(parser, TK_COMMA) && !xr_parser_check(parser, TK_RBRACE) &&
                   !xr_parser_check(parser, TK_EOF)) {
                xr_parser_advance(parser);
            }
            if (xr_parser_check(parser, TK_COMMA)) {
                xr_parser_advance(parser);
            }
            continue;
        }

        arms[arm_count++] = arm;

        // Optional comma
        if (xr_parser_check(parser, TK_COMMA)) {
            xr_parser_advance(parser);
        }
    }

    // Consume '}'
    xr_parser_consume(parser, TK_RBRACE, "expected '}' at end of match expression");
    int match_end_line = parser->previous.line;
    int match_end_column = parser->previous.column + 1;

    // Check if at least one arm
    if (arm_count == 0) {
        xr_parser_error(parser, "match expression requires at least one arm");
        return NULL;
    }

    AstNode *node = xr_ast_match_expr(parser->X, expr, arms, arm_count, line);
    node->end_line = match_end_line;
    node->end_column = match_end_column;
    return node;
}
