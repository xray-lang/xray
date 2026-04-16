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
 * - Multi-value pattern: 1, 2, 3
 * - Wildcard pattern: _
 */
static AstNode *parse_pattern(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_pattern: NULL parser");
    int line = parser->current.line;
    
    // Wildcard pattern
    if (xr_parser_match(parser, TK_UNDERSCORE)) {
            return xr_ast_pattern_wildcard(parser->X, line);
    }
    
    // Parse first value (may be literal or enum access)
    // Use PREC_CALL to support member access expressions (e.g. HttpStatus.OK)
    AstNode *first = xr_parse_precedence(parser, PREC_CALL);
    if (!first) {
        xr_parser_error(parser, "expected pattern");
        return NULL;
    }
    
    // Check if range pattern
    if (xr_parser_match(parser, TK_RANGE)) {
        // Range pattern: 1..10
        AstNode *end = xr_parse_precedence(parser, PREC_CALL);
        if (!end) {
            xr_parser_error(parser, "expected range end value");
            return NULL;
        }
        return xr_ast_pattern_range(parser->X, first, end, line);
    }
    
    // Check if multi-value pattern
    if (xr_parser_check(parser, TK_COMMA)) {
        // Multi-value pattern: 1, 2, 3
        AstNode **patterns = (AstNode **)xr_malloc(sizeof(AstNode *) * 16);
        int count = 0;
        int capacity = 16;
        
        // First pattern
        patterns[count++] = xr_ast_pattern_literal(parser->X, first, line);
        
        // Subsequent patterns
        while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_ARROW)) {
            if (count >= capacity) {
                capacity *= 2;
                AstNode ** _new_patterns = (AstNode **)xr_realloc(patterns, sizeof(AstNode *) * capacity);
                if (!_new_patterns) return NULL;
                patterns = _new_patterns;

            }
            
            AstNode *value = xr_parse_precedence(parser, PREC_CALL);
            if (!value) {
                xr_parser_error(parser, "expected pattern value");
                break;
            }
            patterns[count++] = xr_ast_pattern_literal(parser->X, value, line);
        }
        
        return xr_ast_pattern_multi(parser->X, patterns, count, line);
    }
    
    // Single literal pattern
    return xr_ast_pattern_literal(parser->X, first, line);
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
 * match x {
 *     1 => "one",
 *     2 => "two",
 *     _ => "other"
 * }
 */
AstNode *xr_parse_match_expr(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_match_expr: NULL parser");
    int line = parser->previous.line;  // match keyword already consumed
    
    // Parse match expression
    AstNode *expr = xr_parse_expression(parser);
    if (!expr) {
        xr_parser_error(parser, "expected match expression");
        return NULL;
    }
    
    // Consume '{'
    xr_parser_consume(parser, TK_LBRACE, "expected '{' after match expression");
    
    // Parse all arms
    AstNode **arms = (AstNode **)xr_malloc(sizeof(AstNode *) * 16);
    int arm_count = 0;
    int capacity = 16;
    
    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        // Expand capacity
        if (arm_count >= capacity) {
            capacity *= 2;
            AstNode ** _new_arms = (AstNode **)xr_realloc(arms, sizeof(AstNode *) * capacity);
            if (!_new_arms) return NULL;
            arms = _new_arms;

        }
        
        // Parse one arm
        AstNode *arm = parse_match_arm(parser);
        if (!arm) {
            // Error recovery: skip to next arm or }
            while (!xr_parser_check(parser, TK_COMMA) && 
                   !xr_parser_check(parser, TK_RBRACE) &&
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
    
    // Check if at least one arm
    if (arm_count == 0) {
        xr_parser_error(parser, "match expression requires at least one arm");
        xr_free(arms);
        return NULL;
    }
    
    return xr_ast_match_expr(parser->X, expr, arms, arm_count, line);
}
