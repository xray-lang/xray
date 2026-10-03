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
static AstNode *parse_pattern_single(Parser *parser);

/* Parse a positional tuple pattern starting at the current `(` token.
 * `()`, `(p,)` and `(p1, p2, ...)` are all accepted; sub-patterns
 * recurse through parse_pattern_single (NOT the alternation form), so
 * a comma inside the tuple terminates the element instead of starting
 * a `1 | 2 | 3`-style alternation. */
static AstNode *parse_tuple_pattern(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_tuple_pattern: NULL parser");
    int line = parser->current.line;
    do {
        xr_parser_consume(parser, TK_LPAREN, "expected '(' to start tuple pattern");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    AstNode **patterns = NULL;
    int count = 0;
    int capacity = 0;

    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RPAREN) && !xr_parser_check(parser, TK_EOF)) {
        if (count >= capacity) {
            int old_capacity = capacity;
            do {
                capacity = xr_parser_grow_capacity(parser, capacity);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            AstNode **_new = (AstNode **) ast_alloc_array(parser->compiler_session,
                                                          sizeof(AstNode *), (size_t) capacity);
            if (!xr_parser_healthy(parser)) return NULL;
            if (old_capacity > 0 && patterns)
                do { if (!ast_copy(parser->compiler_session, _new, patterns, sizeof(AstNode *) * (size_t) old_capacity)) return NULL; } while (0);
            patterns = _new;
        }

        AstNode *sub = parse_pattern_single(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!sub)
            return NULL;
        patterns[count++] = sub;

        if (xr_parser_check(parser, TK_RPAREN))
            break;
        if (!xr_parser_match(parser, TK_COMMA)) {
            do {
                xr_parser_error(parser, "expected ',' or ')' in tuple pattern");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
    }

    do {
        xr_parser_consume(parser, TK_RPAREN, "expected ')' to close tuple pattern");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    return xr_ast_pattern_tuple(parser->compiler_session, patterns, count, line);
}

/* Copy the identifier text of `tok` into an AST-arena string. */
static char *pattern_copy_token(Parser *parser, const Token *tok) {
    if (!xr_parser_healthy(parser)) return NULL;
    char *buf = (char *) ast_alloc(parser->compiler_session, (size_t) tok->length + 1);
    if (!xr_parser_healthy(parser)) return NULL;
    if (!buf)
        return NULL;
    do { if (!ast_copy(parser->compiler_session, buf, tok->start, (size_t) tok->length)) return NULL; } while (0);
    do { if (!ast_work(parser->compiler_session, 1)) return NULL; buf[tok->length] = '\0'; } while (0);
    return buf;
}

/* Parse an object match pattern: `{ x, y }` or `{ x: sub }`.
 * Shorthand `{ x }` binds field `x` to a local `x`; `{ x: sub }` matches the
 * field value against the sub-pattern (rename, literal, nested destructure). */
static AstNode *parse_object_pattern(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_object_pattern: NULL parser");
    int line = parser->current.line;
    do {
        xr_parser_consume(parser, TK_LBRACE, "expected '{' to start object pattern");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    char **field_names = NULL;
    AstNode **patterns = NULL;
    int count = 0;
    int capacity = 0;

    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        if (!xr_parser_check(parser, TK_NAME)) {
            do {
                xr_parser_error(parser, "expected field name in object pattern");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        Token name_tok = parser->current;
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        char *field = pattern_copy_token(parser, &name_tok);
        if (!xr_parser_healthy(parser)) return NULL;

        AstNode *sub;
        if (xr_parser_match(parser, TK_COLON)) {
            do {
                sub = parse_pattern_single(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            if (!sub)
                return NULL;
        } else {
            /* Shorthand `{ x }`: bind field `x` to a fresh local `x`. */
            AstNode *var = xr_ast_variable(parser->compiler_session, field, name_tok.line);
            if (!xr_parser_healthy(parser)) return NULL;
            do {
                sub = xr_ast_pattern_literal(parser->compiler_session, var, name_tok.line);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }

        if (count >= capacity) {
            int old_capacity = capacity;
            do {
                capacity = xr_parser_grow_capacity(parser, capacity);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            char **nf = (char **) ast_alloc_array(parser->compiler_session, sizeof(char *),
                                                  (size_t) capacity);
            if (!xr_parser_healthy(parser)) return NULL;
            AstNode **np = (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *),
                                                        (size_t) capacity);
            if (!xr_parser_healthy(parser)) return NULL;
            if (old_capacity > 0) {
                do { if (!ast_copy(parser->compiler_session, nf, field_names, sizeof(char *) * (size_t) old_capacity)) return NULL; } while (0);
                do { if (!ast_copy(parser->compiler_session, np, patterns, sizeof(AstNode *) * (size_t) old_capacity)) return NULL; } while (0);
            }
            field_names = nf;
            patterns = np;
        }
        field_names[count] = field;
        patterns[count] = sub;
        count++;

        if (!xr_parser_check(parser, TK_RBRACE)) {
            if (!xr_parser_match(parser, TK_COMMA)) {
                do {
                    xr_parser_error(parser, "expected ',' or '}' in object pattern");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
        }
    }

    do {
        xr_parser_consume(parser, TK_RBRACE, "expected '}' to close object pattern");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    return xr_ast_pattern_object(parser->compiler_session, field_names, patterns, count, line);
}

/* Parse an array match pattern: `[a, b, ..rest]`. Positional elements use
 * ordinary sub-patterns; an optional trailing `..rest` (or bare `..`) binds the
 * remaining elements as a new array. The rest element must be last. */
static AstNode *parse_array_pattern(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_array_pattern: NULL parser");
    int line = parser->current.line;
    do {
        xr_parser_consume(parser, TK_LBRACKET, "expected '[' to start array pattern");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    AstNode **patterns = NULL;
    int count = 0;
    int capacity = 0;
    bool has_rest = false;
    char *rest_name = NULL;

    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RBRACKET) && !xr_parser_check(parser, TK_EOF)) {
        if (xr_parser_check(parser, TK_RANGE)) {
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);  // consume '..'
            if (xr_parser_check(parser, TK_NAME)) {
                Token rt = parser->current;
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                do {
                    rest_name = pattern_copy_token(parser, &rt);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
            has_rest = true;
            break;
        }

        AstNode *sub = parse_pattern_single(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!sub)
            return NULL;

        if (count >= capacity) {
            int old_capacity = capacity;
            do {
                capacity = xr_parser_grow_capacity(parser, capacity);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            AstNode **np = (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *),
                                                        (size_t) capacity);
            if (!xr_parser_healthy(parser)) return NULL;
            if (old_capacity > 0)
                do { if (!ast_copy(parser->compiler_session, np, patterns, sizeof(AstNode *) * (size_t) old_capacity)) return NULL; } while (0);
            patterns = np;
        }
        patterns[count++] = sub;

        if (!xr_parser_check(parser, TK_RBRACKET)) {
            if (!xr_parser_match(parser, TK_COMMA)) {
                do {
                    xr_parser_error(parser, "expected ',' or ']' in array pattern");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
        }
    }

    if (has_rest && xr_parser_check(parser, TK_COMMA))
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);

    do {
        xr_parser_consume(parser, TK_RBRACKET, "expected ']' to close array pattern");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    return xr_ast_pattern_array(parser->compiler_session, patterns, count, has_rest, rest_name,
                                line);
}

static AstNode *parse_adt_record_pattern(Parser *parser, AstNode *variant, int line) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_adt_record_pattern: NULL parser");
    XR_DCHECK(variant != NULL, "parse_adt_record_pattern: NULL variant");

    do {
        xr_parser_consume(parser, TK_LBRACE, "expected '{' to start payload enum pattern");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    char **field_names = NULL;
    XrNameSpan *field_name_spans = NULL;
    AstNode **patterns = NULL;
    int count = 0;
    int capacity = 0;

    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        if (xr_parser_check(parser, TK_RANGE) || xr_parser_check(parser, TK_DOT_DOT_DOT)) {
            do {
                xr_parser_error(parser, "payload enum patterns ignore omitted fields; remove '..'");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        do {
            xr_parser_consume(parser, TK_NAME, "expected field name in payload enum pattern");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        Token field_token = parser->previous;
        char *field_name = pattern_copy_token(parser, &field_token);
        if (!xr_parser_healthy(parser)) return NULL;
        AstNode *subpattern = NULL;
        if (xr_parser_match(parser, TK_COLON)) {
            do {
                subpattern = parse_pattern_single(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } else {
            AstNode *binding =
                xr_ast_variable(parser->compiler_session, field_name, field_token.line);
            if (!xr_parser_healthy(parser)) return NULL;
            do {
                subpattern =
                xr_ast_pattern_literal(parser->compiler_session, binding, field_token.line);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }

        if (count >= capacity) {
            int old_capacity = capacity;
            do {
                capacity = xr_parser_grow_capacity(parser, capacity);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            char **new_names = (char **) ast_alloc_array(parser->compiler_session, sizeof(char *),
                                                         (size_t) capacity);
            if (!xr_parser_healthy(parser)) return NULL;
            XrNameSpan *new_name_spans = (XrNameSpan *) ast_alloc_array(
                parser->compiler_session, sizeof(XrNameSpan), (size_t) capacity);
            if (!xr_parser_healthy(parser)) return NULL;
            AstNode **new_patterns = (AstNode **) ast_alloc_array(
                parser->compiler_session, sizeof(AstNode *), (size_t) capacity);
            if (!xr_parser_healthy(parser)) return NULL;
            if (old_capacity > 0) {
                do { if (!ast_copy(parser->compiler_session, new_names, field_names, sizeof(char *) * (size_t) old_capacity)) return NULL; } while (0);
                do { if (!ast_copy(parser->compiler_session, new_name_spans, field_name_spans,
                       sizeof(XrNameSpan) * (size_t) old_capacity)) return NULL; } while (0);
                do { if (!ast_copy(parser->compiler_session, new_patterns, patterns, sizeof(AstNode *) * (size_t) old_capacity)) return NULL; } while (0);
            }
            field_names = new_names;
            field_name_spans = new_name_spans;
            patterns = new_patterns;
        }
        field_names[count] = field_name;
        field_name_spans[count] =
            (XrNameSpan) {.line = field_token.line, .column = field_token.column};
        patterns[count] = subpattern;
        count++;

        if (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_match(parser, TK_COMMA)) {
            do {
                xr_parser_error(parser, "expected ',' or '}' in payload enum pattern");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
    }

    do {
        xr_parser_consume(parser, TK_RBRACE, "expected '}' after payload enum pattern");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    return xr_ast_pattern_adt(parser->compiler_session, variant, field_names, field_name_spans,
                              patterns, count, line);
}

/* Parse exactly one pattern atom — wildcard, tuple destructure,
 * literal, range, or binding identifier. Crucially this does NOT
 * collapse a trailing `, …` into an alternation pattern; the caller
 * (a tuple-element loop or a match-arm head) decides how to interpret
 * the comma. */
// Inner implementation; parse_pattern_single wraps this with the recursion-depth
// guard. Nested tuple/object/array patterns recurse through the public
// wrapper, so the guard bounds pattern nesting depth.
static AstNode *parse_pattern_single_inner(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_pattern_single: NULL parser");
    int line = parser->current.line;

    if (xr_parser_match(parser, TK_UNDERSCORE)) {
        return xr_ast_pattern_wildcard(parser->compiler_session, line);
    }

    if (xr_parser_check(parser, TK_LPAREN)) {
        return parse_tuple_pattern(parser);
    }

    if (xr_parser_check(parser, TK_LBRACE)) {
        return parse_object_pattern(parser);
    }

    if (xr_parser_check(parser, TK_LBRACKET)) {
        return parse_array_pattern(parser);
    }

    /* Type pattern: `is T` or `is T name`.
     * Must be detected before generic expression parsing so the `is`
     * keyword is interpreted as a pattern prefix rather than an
     * (illegal) binary operator. */
    if (xr_parser_match(parser, TK_IS)) {
        XrTypeRef *type = xr_parse_type_annotation(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!type) {
            do {
                xr_parser_error(parser, "expected type after 'is'");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        const char *binding_name = NULL;
        if (xr_parser_check(parser, TK_NAME)) {
            Token name_tok = parser->current;
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            char *buf = (char *) ast_alloc(parser->compiler_session, (size_t) name_tok.length + 1);
            if (!xr_parser_healthy(parser)) return NULL;
            if (!buf)
                return NULL;
            do { if (!ast_copy(parser->compiler_session, buf, name_tok.start, name_tok.length)) return NULL; } while (0);
            do { if (!ast_work(parser->compiler_session, 1)) return NULL; buf[name_tok.length] = '\0'; } while (0);
            binding_name = buf;
        }
        return xr_ast_pattern_type(parser->compiler_session, type, binding_name, line);
    }

    bool saved_pattern_mode = parser->parsing_pattern;
    parser->parsing_pattern = true;
    AstNode *first = xr_parse_precedence(parser, PREC_CALL);
    if (!xr_parser_healthy(parser)) return NULL;
    parser->parsing_pattern = saved_pattern_mode;
    if (!first) {
        do {
            xr_parser_error(parser, "expected pattern");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    if (xr_parser_check(parser, TK_RANGE) || xr_parser_check(parser, TK_RANGE_INCLUSIVE)) {
        bool inclusive_end = parser->current.type == TK_RANGE_INCLUSIVE;
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        AstNode *end = xr_parse_precedence(parser, PREC_CALL);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!end) {
            do {
                xr_parser_error(parser, "expected range end value");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        return xr_ast_pattern_range(parser->compiler_session, first, end, inclusive_end, line);
    }

    if ((first->type == AST_MEMBER_ACCESS || first->type == AST_ENUM_ACCESS) &&
        xr_parser_check(parser, TK_LBRACE)) {
        return parse_adt_record_pattern(parser, first, line);
    }

    if (first->type == AST_CALL_EXPR) {
        AstNode *callee = first->as.call_expr.callee;
        if (callee && (callee->type == AST_MEMBER_ACCESS || callee->type == AST_ENUM_ACCESS)) {
            do {
                xr_parser_error(parser, "payload enum patterns use named record fields, not '(...)'");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
    }

    return xr_ast_pattern_literal(parser->compiler_session, first, line);
}

// Public pattern-atom entry: recursion-depth guard around
// parse_pattern_single_inner. Nested tuple/object/array patterns recurse
// through here, so this bounds pattern nesting depth.
static AstNode *parse_pattern_single(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_pattern_single: NULL parser");
    if (++parser->recursion_depth > XR_PARSER_MAX_DEPTH) {
        parser->recursion_depth--;
        do {
            xr_parser_error(parser, "pattern nesting too deep (max 1000 levels)");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    AstNode *result = parse_pattern_single_inner(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    parser->recursion_depth--;
    return result;
}

/* Top-level pattern of a match arm or parenthesized catch header: parse one
 * atom, then optionally fold further atoms separated by `,` into an
 * alternation pattern. This is the only place where a top-level comma starts
 * an alternation; tuple sub-elements use parse_pattern_single. */
static AstNode *parse_top_level_pattern(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_pattern: NULL parser");
    int line = parser->current.line;

    AstNode *first = parse_pattern_single(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    if (!first)
        return NULL;

    if (!xr_parser_check(parser, TK_COMMA))
        return first;

    /* Alternation: `p1, p2, p3 -> ...` — gather until the arrow.
     * The first atom may itself be a tuple/range/wildcard pattern. */
    AstNode **patterns =
        (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *), 16);
    if (!xr_parser_healthy(parser)) return NULL;
    int count = 0;
    int capacity = 16;
    patterns[count++] = first;

    while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_ARROW) &&
           !xr_parser_check(parser, TK_IF)) {
        if (count >= capacity) {
            int old_capacity = capacity;
            capacity *= 2;
            AstNode **_new_patterns = (AstNode **) ast_alloc_array(
                parser->compiler_session, sizeof(AstNode *), (size_t) capacity);
            if (!xr_parser_healthy(parser)) return NULL;
            if (old_capacity > 0 && patterns)
                do { if (!ast_copy(parser->compiler_session, _new_patterns, patterns, sizeof(AstNode *) * (size_t) old_capacity)) return NULL; } while (0);
            patterns = _new_patterns;
        }

        AstNode *next = parse_pattern_single(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!next) {
            do {
                xr_parser_error(parser, "expected pattern value");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            break;
        }
        patterns[count++] = next;
    }

    return xr_ast_pattern_multi(parser->compiler_session, patterns, count, line);
}

XR_FUNC AstNode *xr_parse_match_pattern(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    return parse_top_level_pattern(parser);
}

/*
 * Parse match arm
 * pattern -> expression
 * pattern if guard -> expression
 * pattern -> { block }
 */
static AstNode *parse_match_arm(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_match_arm: NULL parser");
    int line = parser->current.line;

    // Parse pattern
    AstNode *pattern = xr_parse_match_pattern(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    if (!pattern) {
        return NULL;
    }

    // Optional guard condition: if (expr)
    AstNode *guard = NULL;
    if (xr_parser_match(parser, TK_IF)) {
        // Consume left paren
        do {
            xr_parser_consume(parser, TK_LPAREN, "expected '(' after if");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);

        // Parse guard condition expression
        do {
            guard = xr_parse_expression(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        if (!guard) {
            do {
                xr_parser_error(parser, "expected guard condition expression");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }

        // Consume right paren
        do {
            xr_parser_consume(parser, TK_RPAREN, "expected ')' after guard condition");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    // Consume arrow
    do {
        xr_parser_consume(parser, TK_ARROW, "expected '->' after pattern");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    // Parse arm body
    AstNode *body = NULL;
    if (xr_parser_match(parser, TK_LBRACE)) {
        // Only the direct arm block consumes its tail expression.
        parser->match_value_block_pending = true;
        do {
            body = xr_parse_block(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    } else {
        // Single expression; a line break followed by `is` ends the body and
        // starts the next arm's type pattern (see line_break_ends_expr).
        parser->match_arm_body_depth++;
        do {
            body = xr_parse_expression(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        parser->match_arm_body_depth--;
    }

    if (!body) {
        do {
            xr_parser_error(parser, "expected expression or code block");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    return xr_ast_match_arm(parser->compiler_session, pattern, guard, body, line);
}

/*
 * Parse match expression (prefix)
 * match (x) {
 *     1 -> "one",
 *     2 -> "two",
 *     _ -> "other"
 * }
 */
AstNode *xr_parse_match_expr(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_match_expr: NULL parser");
    int line = parser->previous.line;  // match keyword already consumed

    // Scrutinee must be parenthesised, mirroring `if (...)`, `for (...)`,
    // `while (...)`. Required parens also disambiguate tuple scrutinees
    // (`match (x, y) { (a, b) -> ... }`) from a sequence of bare names.
    do {
        xr_parser_consume(parser, TK_LPAREN, "expected '(' after 'match'");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    AstNode *expr = xr_parse_expression(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    if (!expr) {
        do {
            xr_parser_error(parser, "expected match expression");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    do {
        xr_parser_consume(parser, TK_RPAREN, "expected ')' after match scrutinee");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    do {
        xr_parser_consume(parser, TK_LBRACE, "expected '{' after match scrutinee");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    // Parse all arms
    AstNode **arms = (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *), 16);
    if (!xr_parser_healthy(parser)) return NULL;
    int arm_count = 0;
    int capacity = 16;

    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        // Expand capacity
        if (arm_count >= capacity) {
            int old_capacity = capacity;
            capacity *= 2;
            AstNode **_new_arms = (AstNode **) ast_alloc_array(
                parser->compiler_session, sizeof(AstNode *), (size_t) capacity);
            if (!xr_parser_healthy(parser)) return NULL;
            if (old_capacity > 0 && arms)
                do { if (!ast_copy(parser->compiler_session, _new_arms, arms, sizeof(AstNode *) * (size_t) old_capacity)) return NULL; } while (0);
            arms = _new_arms;
        }

        // Parse one arm
        AstNode *arm = parse_match_arm(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!arm) {
            // Error recovery: skip to next arm or }
            while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_COMMA) && !xr_parser_check(parser, TK_RBRACE) &&
                   !xr_parser_check(parser, TK_EOF)) {
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
            if (xr_parser_check(parser, TK_COMMA)) {
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
            continue;
        }

        arms[arm_count++] = arm;

        // Optional comma
        if (xr_parser_check(parser, TK_COMMA)) {
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
    }

    // Consume '}'
    do {
        xr_parser_consume(parser, TK_RBRACE, "expected '}' at end of match expression");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    int match_end_line = parser->previous.line;
    int match_end_column = parser->previous.column + 1;

    // Check if at least one arm
    if (arm_count == 0) {
        do {
            xr_parser_error(parser, "match expression requires at least one arm");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    AstNode *node = xr_ast_match_expr(parser->compiler_session, expr, arms, arm_count, line);
    if (!xr_parser_healthy(parser)) return NULL;
    node->end_line = match_end_line;
    node->end_column = match_end_column;
    return node;
}
