/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_errors.c - Error signature and propagation expression syntax
 */
#include "xparse_internal.h"

XR_FUNC bool xr_parse_throws_clause(Parser *parser, XrThrowsClause *clause) {
    if (!xr_parser_healthy(parser) || !clause) return false;
    *clause = (XrThrowsClause){0};
    if (!xr_parser_check_name(parser, "throws")) return xr_parser_healthy(parser);
    Token keyword = parser->current;
    xr_parser_advance(parser);
    if (!xr_parser_healthy(parser)) return false;
    clause->syntax = XR_THROWS_ERASED;
    clause->line = keyword.line;
    clause->column = keyword.column;
    if (xr_parser_match(parser, TK_LPAREN)) {
        clause->syntax = XR_THROWS_TYPED;
        clause->type = xr_parse_type_annotation(parser);
        if (!xr_parser_healthy(parser) || !clause->type) return false;
        xr_parser_consume(parser, TK_RPAREN, "expected ')' after throws type");
    }
    return xr_parser_healthy(parser);
}

XR_FUNC bool xr_parse_starts_try_block(Parser *parser) {
    if (!xr_parser_healthy(parser)) return false;
    Scanner lookahead;
    if (xr_compile_state_copy(parser->state, &lookahead, &parser->scanner,
                              sizeof(lookahead)) != XR_COMPILE_RESOURCE_OK) return false;
    Token next = xr_scanner_scan(&lookahead);
    return xr_parser_healthy(parser) && next.type == TK_LBRACE;
}

XR_FUNC AstNode *xr_parse_try_expression(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    Token keyword = parser->previous;
    XrTryMode mode = XR_TRY_PROPAGATE;
    if (xr_parser_match(parser, TK_QUESTION)) mode = XR_TRY_OPTIONAL;
    else if (xr_parser_match(parser, TK_NOT)) mode = XR_TRY_ASSERT;
    bool outer_optional = parser->optional_try_operand;
    int outer_depth = parser->optional_try_bracket_depth;
    if (mode == XR_TRY_OPTIONAL) {
        parser->optional_try_operand = true;
        parser->optional_try_bracket_depth = parser->bracket_depth;
    }
    AstNode *operand = xr_parse_precedence(parser, PREC_TERNARY);
    parser->optional_try_operand = outer_optional;
    parser->optional_try_bracket_depth = outer_depth;
    if (!xr_parser_healthy(parser) || !operand) return NULL;
    AstNode *node = xr_ast_unary(parser->compiler_session, AST_TRY_EXPR, operand,
                                 keyword.line);
    if (!xr_parser_healthy(parser) || !node) return NULL;
    node->as.try_expr.mode = mode;
    node->column = keyword.column;
    node->end_line = parser->previous.line;
    node->end_column = parser->previous.column + parser->previous.length;
    return node;
}
