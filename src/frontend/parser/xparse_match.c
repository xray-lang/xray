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

/* In an unparenthesized catch clause the final brace group is the catch body.
 * A qualified variant path therefore owns a record-pattern brace only when the
 * matching brace is followed by another top-level pattern (`,`) or by the body
 * brace (`{`). This token-only delimiter rule never queries variant metadata
 * and leaves nested record patterns unchanged. */
static bool catch_brace_starts_record_pattern(Parser *parser) {
    XR_DCHECK(parser != NULL, "catch_brace_starts_record_pattern: NULL parser");
    XR_DCHECK(xr_parser_check(parser, TK_LBRACE),
              "catch_brace_starts_record_pattern: expected current '{'");

    XrParserStreamState saved = xr_parser_stream_save(parser);
    int brace_depth = 0;
    while (!xr_parser_check(parser, TK_EOF)) {
        if (xr_parser_check(parser, TK_LBRACE)) {
            brace_depth++;
            xr_parser_advance(parser);
            continue;
        }
        if (xr_parser_check(parser, TK_RBRACE)) {
            brace_depth--;
            xr_parser_advance(parser);
            if (brace_depth == 0)
                break;
            continue;
        }
        xr_parser_advance(parser);
    }
    bool is_record_pattern = brace_depth == 0 && (xr_parser_check(parser, TK_COMMA) ||
                                                  xr_parser_check(parser, TK_LBRACE));
    xr_parser_stream_restore(parser, &saved);
    return is_record_pattern;
}

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
            AstNode **_new = (AstNode **) ast_alloc_array(parser->compiler_session,
                                                          sizeof(AstNode *), (size_t) capacity);
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
    return xr_ast_pattern_tuple(parser->compiler_session, patterns, count, line);
}

/* Copy the identifier text of `tok` into an AST-arena string. */
static char *pattern_copy_token(Parser *parser, const Token *tok) {
    char *buf = (char *) ast_alloc(parser->compiler_session, (size_t) tok->length + 1);
    if (!buf)
        return NULL;
    memcpy(buf, tok->start, (size_t) tok->length);
    buf[tok->length] = '\0';
    return buf;
}

/* Parse an object match pattern: `{ x, y }` or `{ x: sub }`.
 * Shorthand `{ x }` binds field `x` to a local `x`; `{ x: sub }` matches the
 * field value against the sub-pattern (rename, literal, nested destructure). */
static AstNode *parse_object_pattern(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_object_pattern: NULL parser");
    int line = parser->current.line;
    xr_parser_consume(parser, TK_LBRACE, "expected '{' to start object pattern");

    char **field_names = NULL;
    AstNode **patterns = NULL;
    int count = 0;
    int capacity = 0;

    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        if (!xr_parser_check(parser, TK_NAME)) {
            xr_parser_error(parser, "expected field name in object pattern");
            return NULL;
        }
        Token name_tok = parser->current;
        xr_parser_advance(parser);
        char *field = pattern_copy_token(parser, &name_tok);

        AstNode *sub;
        if (xr_parser_match(parser, TK_COLON)) {
            sub = parse_pattern_single(parser);
            if (!sub)
                return NULL;
        } else {
            /* Shorthand `{ x }`: bind field `x` to a fresh local `x`. */
            AstNode *var = xr_ast_variable(parser->compiler_session, field, name_tok.line);
            sub = xr_ast_pattern_literal(parser->compiler_session, var, name_tok.line);
        }

        if (count >= capacity) {
            int old_capacity = capacity;
            capacity = (capacity == 0) ? 4 : capacity * 2;
            char **nf = (char **) ast_alloc_array(parser->compiler_session, sizeof(char *),
                                                  (size_t) capacity);
            AstNode **np = (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *),
                                                        (size_t) capacity);
            if (old_capacity > 0) {
                memcpy(nf, field_names, sizeof(char *) * (size_t) old_capacity);
                memcpy(np, patterns, sizeof(AstNode *) * (size_t) old_capacity);
            }
            field_names = nf;
            patterns = np;
        }
        field_names[count] = field;
        patterns[count] = sub;
        count++;

        if (!xr_parser_check(parser, TK_RBRACE)) {
            if (!xr_parser_match(parser, TK_COMMA)) {
                xr_parser_error(parser, "expected ',' or '}' in object pattern");
                return NULL;
            }
        }
    }

    xr_parser_consume(parser, TK_RBRACE, "expected '}' to close object pattern");
    return xr_ast_pattern_object(parser->compiler_session, field_names, patterns, count, line);
}

/* Parse an array match pattern: `[a, b, ..rest]`. Positional elements use
 * ordinary sub-patterns; an optional trailing `..rest` (or bare `..`) binds the
 * remaining elements as a new array. The rest element must be last. */
static AstNode *parse_array_pattern(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_array_pattern: NULL parser");
    int line = parser->current.line;
    xr_parser_consume(parser, TK_LBRACKET, "expected '[' to start array pattern");

    AstNode **patterns = NULL;
    int count = 0;
    int capacity = 0;
    bool has_rest = false;
    char *rest_name = NULL;

    while (!xr_parser_check(parser, TK_RBRACKET) && !xr_parser_check(parser, TK_EOF)) {
        if (xr_parser_check(parser, TK_RANGE)) {
            xr_parser_advance(parser);  // consume '..'
            if (xr_parser_check(parser, TK_NAME)) {
                Token rt = parser->current;
                xr_parser_advance(parser);
                rest_name = pattern_copy_token(parser, &rt);
            }
            has_rest = true;
            break;
        }

        AstNode *sub = parse_pattern_single(parser);
        if (!sub)
            return NULL;

        if (count >= capacity) {
            int old_capacity = capacity;
            capacity = (capacity == 0) ? 4 : capacity * 2;
            AstNode **np = (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *),
                                                        (size_t) capacity);
            if (old_capacity > 0)
                memcpy(np, patterns, sizeof(AstNode *) * (size_t) old_capacity);
            patterns = np;
        }
        patterns[count++] = sub;

        if (!xr_parser_check(parser, TK_RBRACKET)) {
            if (!xr_parser_match(parser, TK_COMMA)) {
                xr_parser_error(parser, "expected ',' or ']' in array pattern");
                return NULL;
            }
        }
    }

    if (has_rest && xr_parser_check(parser, TK_COMMA))
        xr_parser_advance(parser);

    xr_parser_consume(parser, TK_RBRACKET, "expected ']' to close array pattern");
    return xr_ast_pattern_array(parser->compiler_session, patterns, count, has_rest, rest_name,
                                line);
}

static AstNode *parse_adt_record_pattern(Parser *parser, AstNode *variant, int line) {
    XR_DCHECK(parser != NULL, "parse_adt_record_pattern: NULL parser");
    XR_DCHECK(variant != NULL, "parse_adt_record_pattern: NULL variant");

    xr_parser_consume(parser, TK_LBRACE, "expected '{' to start payload enum pattern");
    char **field_names = NULL;
    XrNameSpan *field_name_spans = NULL;
    AstNode **patterns = NULL;
    int count = 0;
    int capacity = 0;

    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        if (xr_parser_check(parser, TK_RANGE) || xr_parser_check(parser, TK_DOT_DOT_DOT)) {
            xr_parser_error(parser, "payload enum patterns ignore omitted fields; remove '..'");
            return NULL;
        }
        xr_parser_consume(parser, TK_NAME, "expected field name in payload enum pattern");
        Token field_token = parser->previous;
        char *field_name = pattern_copy_token(parser, &field_token);
        AstNode *subpattern = NULL;
        if (xr_parser_match(parser, TK_COLON)) {
            subpattern = parse_pattern_single(parser);
        } else {
            AstNode *binding =
                xr_ast_variable(parser->compiler_session, field_name, field_token.line);
            subpattern =
                xr_ast_pattern_literal(parser->compiler_session, binding, field_token.line);
        }

        if (count >= capacity) {
            int old_capacity = capacity;
            capacity = capacity == 0 ? 4 : capacity * 2;
            char **new_names = (char **) ast_alloc_array(parser->compiler_session, sizeof(char *),
                                                         (size_t) capacity);
            XrNameSpan *new_name_spans = (XrNameSpan *) ast_alloc_array(
                parser->compiler_session, sizeof(XrNameSpan), (size_t) capacity);
            AstNode **new_patterns = (AstNode **) ast_alloc_array(
                parser->compiler_session, sizeof(AstNode *), (size_t) capacity);
            if (old_capacity > 0) {
                memcpy(new_names, field_names, sizeof(char *) * (size_t) old_capacity);
                memcpy(new_name_spans, field_name_spans,
                       sizeof(XrNameSpan) * (size_t) old_capacity);
                memcpy(new_patterns, patterns, sizeof(AstNode *) * (size_t) old_capacity);
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
            xr_parser_error(parser, "expected ',' or '}' in payload enum pattern");
            return NULL;
        }
    }

    xr_parser_consume(parser, TK_RBRACE, "expected '}' after payload enum pattern");
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
static AstNode *parse_pattern_single_inner(Parser *parser, bool unparenthesized_catch_header) {
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
        if (!type) {
            xr_parser_error(parser, "expected type after 'is'");
            return NULL;
        }
        const char *binding_name = NULL;
        if (xr_parser_check(parser, TK_NAME)) {
            Token name_tok = parser->current;
            xr_parser_advance(parser);
            char *buf = (char *) ast_alloc(parser->compiler_session, (size_t) name_tok.length + 1);
            if (!buf)
                return NULL;
            memcpy(buf, name_tok.start, name_tok.length);
            buf[name_tok.length] = '\0';
            binding_name = buf;
        }
        return xr_ast_pattern_type(parser->compiler_session, type, binding_name, line);
    }

    bool saved_pattern_mode = parser->parsing_pattern;
    parser->parsing_pattern = true;
    AstNode *first = xr_parse_precedence(parser, PREC_CALL);
    parser->parsing_pattern = saved_pattern_mode;
    if (!first) {
        xr_parser_error(parser, "expected pattern");
        return NULL;
    }

    if (xr_parser_check(parser, TK_RANGE) || xr_parser_check(parser, TK_RANGE_INCLUSIVE)) {
        bool inclusive_end = parser->current.type == TK_RANGE_INCLUSIVE;
        xr_parser_advance(parser);
        AstNode *end = xr_parse_precedence(parser, PREC_CALL);
        if (!end) {
            xr_parser_error(parser, "expected range end value");
            return NULL;
        }
        return xr_ast_pattern_range(parser->compiler_session, first, end, inclusive_end, line);
    }

    if ((first->type == AST_MEMBER_ACCESS || first->type == AST_ENUM_ACCESS) &&
        xr_parser_check(parser, TK_LBRACE) &&
        (!unparenthesized_catch_header || catch_brace_starts_record_pattern(parser))) {
        return parse_adt_record_pattern(parser, first, line);
    }

    if (first->type == AST_CALL_EXPR) {
        AstNode *callee = first->as.call_expr.callee;
        if (callee && (callee->type == AST_MEMBER_ACCESS || callee->type == AST_ENUM_ACCESS)) {
            xr_parser_error(parser, "payload enum patterns use named record fields, not '(...)'");
            return NULL;
        }
    }

    return xr_ast_pattern_literal(parser->compiler_session, first, line);
}

// Public pattern-atom entry: recursion-depth guard around
// parse_pattern_single_inner. Nested tuple/object/array patterns recurse
// through here, so this bounds pattern nesting depth.
static AstNode *parse_pattern_single_with_context(Parser *parser,
                                                  bool unparenthesized_catch_header) {
    XR_DCHECK(parser != NULL, "parse_pattern_single: NULL parser");
    if (++parser->recursion_depth > XR_PARSER_MAX_DEPTH) {
        parser->recursion_depth--;
        xr_parser_error(parser, "pattern nesting too deep (max 1000 levels)");
        return NULL;
    }
    AstNode *result = parse_pattern_single_inner(parser, unparenthesized_catch_header);
    parser->recursion_depth--;
    return result;
}

static AstNode *parse_pattern_single(Parser *parser) {
    return parse_pattern_single_with_context(parser, false);
}

/* Top-level match-arm pattern: parse one atom, then optionally fold
 * in further atoms separated by `,` into an alternation pattern up
 * to the `->`. This is the only place where a top-level comma starts
 * an alternation; tuple sub-elements use parse_pattern_single. */
static AstNode *parse_top_level_pattern(Parser *parser, bool unparenthesized_catch_header) {
    XR_DCHECK(parser != NULL, "parse_pattern: NULL parser");
    int line = parser->current.line;

    AstNode *first = parse_pattern_single_with_context(parser, unparenthesized_catch_header);
    if (!first)
        return NULL;

    if (!xr_parser_check(parser, TK_COMMA))
        return first;

    /* Alternation: `p1, p2, p3 -> ...` — gather until the arrow.
     * The first atom may itself be a tuple/range/wildcard pattern. */
    AstNode **patterns =
        (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *), 16);
    int count = 0;
    int capacity = 16;
    patterns[count++] = first;

    while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_ARROW) &&
           !xr_parser_check(parser, TK_IF)) {
        if (count >= capacity) {
            int old_capacity = capacity;
            capacity *= 2;
            AstNode **_new_patterns = (AstNode **) ast_alloc_array(
                parser->compiler_session, sizeof(AstNode *), (size_t) capacity);
            if (old_capacity > 0 && patterns)
                memcpy(_new_patterns, patterns, sizeof(AstNode *) * (size_t) old_capacity);
            patterns = _new_patterns;
        }

        AstNode *next = parse_pattern_single_with_context(parser, unparenthesized_catch_header);
        if (!next) {
            xr_parser_error(parser, "expected pattern value");
            break;
        }
        patterns[count++] = next;
    }

    return xr_ast_pattern_multi(parser->compiler_session, patterns, count, line);
}

XR_FUNC AstNode *xr_parse_match_pattern(Parser *parser) {
    return parse_top_level_pattern(parser, false);
}

XR_FUNC AstNode *xr_parse_unparenthesized_catch_pattern(Parser *parser) {
    return parse_top_level_pattern(parser, true);
}

/*
 * Parse match arm
 * pattern -> expression
 * pattern if guard -> expression
 * pattern -> { block }
 */
static AstNode *parse_match_arm(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_match_arm: NULL parser");
    int line = parser->current.line;

    // Parse pattern
    AstNode *pattern = xr_parse_match_pattern(parser);
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
    xr_parser_consume(parser, TK_ARROW, "expected '->' after pattern");

    // Parse arm body
    AstNode *body = NULL;
    if (xr_parser_match(parser, TK_LBRACE)) {
        // Code block
        body = xr_parse_block(parser);
    } else {
        // Single expression; a line break followed by `is` ends the body and
        // starts the next arm's type pattern (see line_break_ends_expr).
        parser->match_arm_body_depth++;
        body = xr_parse_expression(parser);
        parser->match_arm_body_depth--;
    }

    if (!body) {
        xr_parser_error(parser, "expected expression or code block");
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
    XR_DCHECK(parser != NULL, "parse_match_expr: NULL parser");
    int line = parser->previous.line;  // match keyword already consumed

    // Scrutinee must be parenthesised, mirroring `if (...)`, `for (...)`,
    // `while (...)`. Required parens also disambiguate tuple scrutinees
    // (`match (x, y) { (a, b) -> ... }`) from a sequence of bare names.
    xr_parser_consume(parser, TK_LPAREN, "expected '(' after 'match'");

    AstNode *expr = xr_parse_expression(parser);
    if (!expr) {
        xr_parser_error(parser, "expected match expression");
        return NULL;
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')' after match scrutinee");
    xr_parser_consume(parser, TK_LBRACE, "expected '{' after match scrutinee");

    // Parse all arms
    AstNode **arms = (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *), 16);
    int arm_count = 0;
    int capacity = 16;

    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        // Expand capacity
        if (arm_count >= capacity) {
            int old_capacity = capacity;
            capacity *= 2;
            AstNode **_new_arms = (AstNode **) ast_alloc_array(
                parser->compiler_session, sizeof(AstNode *), (size_t) capacity);
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

    AstNode *node = xr_ast_match_expr(parser->compiler_session, expr, arms, arm_count, line);
    node->end_line = match_end_line;
    node->end_column = match_end_column;
    return node;
}
