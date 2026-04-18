/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_stmt.c - Control flow statement parsing
 *
 * KEY CONCEPT:
 *   Parses if/while/for/for-in/break/continue statements.
 *   Split from xparse.c to keep file sizes manageable.
 */

#include "xparse_internal.h"
#include "../../base/xchecks.h"

/* ========== Control Flow Parsing ========== */

// Helper: propagate `body->end_*` as the enclosing statement's end span.
static inline void inherit_block_end(AstNode *stmt, AstNode *body) {
    if (!stmt || !body) return;
    stmt->end_line   = body->end_line;
    stmt->end_column = body->end_column;
}

// Parse if statement
AstNode *xr_parse_if_statement(Parser *parser) {
    int line = parser->previous.line;
    xr_parser_advance(parser);

    xr_parser_consume(parser, TK_LPAREN, "expected '(' after if");
    AstNode *condition = xr_parse_expression(parser);
    xr_parser_consume(parser, TK_RPAREN, "expected ')' after if condition");

    if (!xr_parser_check(parser, TK_LBRACE)) {
        xr_parser_error_at_current(parser, "if statement requires braces { }");
        return NULL;
    }
    xr_parser_advance(parser);
    AstNode *then_branch = xr_parse_block(parser);

    AstNode *else_branch = NULL;
    // Detect 'elif' (Python habit) — must check before else
    if (xr_parser_check(parser, TK_NAME) &&
        parser->current.length == 4 &&
        memcmp(parser->current.start, "elif", 4) == 0) {
        xr_parser_error_at_current(parser,
            "unknown keyword 'elif'. Use 'else if' in Xray");
        AstNode *stmt = xr_ast_if_stmt(parser->X, condition, then_branch, NULL, line);
        inherit_block_end(stmt, then_branch);
        return stmt;
    }
    if (xr_parser_match(parser, TK_ELSE)) {
        if (xr_parser_check(parser, TK_IF)) {
            else_branch = xr_parse_if_statement(parser);
        } else {
            if (!xr_parser_check(parser, TK_LBRACE)) {
                xr_parser_error_at_current(parser, "else requires braces { } or if statement");
                return NULL;
            }
            xr_parser_advance(parser);
            else_branch = xr_parse_block(parser);
        }
    }

    AstNode *stmt = xr_ast_if_stmt(parser->X, condition, then_branch, else_branch, line);
    // End span = end of else branch if present, otherwise end of then branch.
    inherit_block_end(stmt, else_branch ? else_branch : then_branch);
    return stmt;
}

// Parse while loop
AstNode *xr_parse_while_statement(Parser *parser) {
    int line = parser->previous.line;
    xr_parser_advance(parser);

    xr_parser_consume(parser, TK_LPAREN, "expected '(' after while");
    AstNode *condition = xr_parse_expression(parser);
    xr_parser_consume(parser, TK_RPAREN, "expected ')' after while condition");

    if (!xr_parser_check(parser, TK_LBRACE)) {
        xr_parser_error_at_current(parser, "while statement requires braces { }");
        return NULL;
    }
    xr_parser_advance(parser);
    AstNode *body = xr_parse_block(parser);

    AstNode *stmt = xr_ast_while_stmt(parser->X, condition, body, line);
    inherit_block_end(stmt, body);
    return stmt;
}

// Parse for loop: for (init; condition; increment) { ... }
AstNode *xr_parse_for_statement(Parser *parser) {
    int line = parser->previous.line;
    xr_parser_advance(parser);

    xr_parser_consume(parser, TK_LPAREN, "expected '(' after for");

    AstNode *initializer = NULL;
    if (xr_parser_match(parser, TK_SEMICOLON)) {
        initializer = NULL;
    } else if (xr_parser_match(parser, TK_LET)) {
        initializer = xr_parse_single_var_declaration(parser, 0);
        xr_parser_consume(parser, TK_SEMICOLON, "expected ';' after for initializer");
    } else {
        initializer = xr_parse_expr_statement(parser);
        xr_parser_consume(parser, TK_SEMICOLON, "expected ';' after for initializer");
    }

    AstNode *condition = NULL;
    if (!xr_parser_check(parser, TK_SEMICOLON)) {
        condition = xr_parse_expression(parser);
    }
    xr_parser_consume(parser, TK_SEMICOLON, "expected ';' after for condition");

    AstNode *increment = NULL;
    if (!xr_parser_check(parser, TK_RPAREN)) {
        increment = xr_parse_expression(parser);
    }
    xr_parser_consume(parser, TK_RPAREN, "expected ')' after for header");

    if (!xr_parser_check(parser, TK_LBRACE)) {
        xr_parser_error_at_current(parser, "for statement requires braces { }");
        return NULL;
    }
    xr_parser_advance(parser);
    AstNode *body = xr_parse_block(parser);

    AstNode *stmt = xr_ast_for_stmt(parser->X, initializer, condition, increment, body, line);
    inherit_block_end(stmt, body);
    return stmt;
}

// Parse for-in loop: for (item in collection) { ... }
AstNode *xr_parse_for_in_statement(Parser *parser) {
    int line = parser->previous.line;
    xr_parser_advance(parser);

    xr_parser_consume(parser, TK_LPAREN, "expected '(' after for");

    // Support _ as blank identifier (discards value)
    if (parser->current.type != TK_NAME && parser->current.type != TK_UNDERSCORE) {
        xr_parser_error_expected_name(parser, "expected loop variable name");
        return NULL;
    }
    xr_parser_advance(parser);
    char *first_name = (char *)xr_malloc(parser->previous.length + 1);
    memcpy(first_name, parser->previous.start, parser->previous.length);
    first_name[parser->previous.length] = '\0';

    // Check for comma (key-value pattern)
    char *second_name = NULL;
    bool is_keyvalue = false;

    if (xr_parser_match(parser, TK_COMMA)) {
        is_keyvalue = true;

        // Support _ as blank identifier in key-value pattern
        if (parser->current.type != TK_NAME && parser->current.type != TK_UNDERSCORE) {
            xr_parser_error_expected_name(parser, "expected value variable name");
            xr_free(first_name);
            return NULL;
        }
        xr_parser_advance(parser);

        second_name = (char *)xr_malloc(parser->previous.length + 1);
        memcpy(second_name, parser->previous.start, parser->previous.length);
        second_name[parser->previous.length] = '\0';
    }

    // Optional type annotation (not supported yet)
    XrType *item_type = NULL;
    if (xr_parser_match(parser, TK_COLON)) {
        while (!xr_parser_check(parser, TK_IN) &&
               !xr_parser_check(parser, TK_EOF)) {
            xr_parser_advance(parser);
        }
    }

    xr_parser_consume(parser, TK_IN, "expected 'in' after loop variable");

    AstNode *collection = xr_parse_expression(parser);

    xr_parser_consume(parser, TK_RPAREN, "expected ')' after for-in header");

    if (!xr_parser_check(parser, TK_LBRACE)) {
        xr_parser_error_at_current(parser, "for-in statement requires braces { }");
        xr_free(first_name);
        if (second_name) xr_free(second_name);
        return NULL;
    }
    xr_parser_advance(parser);
    AstNode *body = xr_parse_block(parser);

    AstNode *stmt = is_keyvalue
        ? xr_ast_for_in_keyvalue_stmt(parser->X, first_name, second_name,
                                      item_type, collection, body, line)
        : xr_ast_for_in_stmt(parser->X, first_name, item_type, collection, body, line);
    inherit_block_end(stmt, body);
    return stmt;
}

// Parse break statement
AstNode *xr_parse_break_statement(Parser *parser) {
    int line = parser->previous.line;
    xr_parser_advance(parser);
    return xr_ast_break_stmt(parser->X, line);
}

// Parse continue statement
AstNode *xr_parse_continue_statement(Parser *parser) {
    int line = parser->previous.line;
    xr_parser_advance(parser);
    return xr_ast_continue_stmt(parser->X, line);
}
