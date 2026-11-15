/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_destructure.c - Destructuring pattern parsing
 *
 * KEY CONCEPT:
 *   Product children retain distinct binding and field-selection spans.
 *   Nested patterns share the parser recursion and allocation budgets.
 */

#include "xparse_internal.h"
#include "../../base/xchecks.h"
#include <stdlib.h>
#include <string.h>

static char *copy_token_string(Parser *parser, Token *token) {
    if (!xr_parser_healthy(parser)) return NULL;
    char *str = (char *) ast_alloc(parser->compiler_session, (size_t) token->length + 1);
    if (!xr_parser_healthy(parser)) return NULL;
    do { if (!ast_copy(parser->compiler_session, str, token->start, token->length)) return NULL; } while (0);
    do { str[token->length] = '\0'; } while (0);
    return str;
}

// Parse flat array destructuring: [a, b, c] or [a, _, c]
XrDestructurePattern *xr_parse_array_pattern(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_array_pattern: NULL parser");
    // '[' already consumed
    XrDestructurePattern **elements = NULL;
    int count = 0;
    int capacity = 0;

    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RBRACKET) && !xr_parser_check(parser, TK_EOF)) {
        if (count >= capacity) {
            int old_capacity = capacity;
            do {
                capacity = xr_parser_grow_capacity(parser, capacity);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            XrDestructurePattern **_new_elements = (XrDestructurePattern **) ast_alloc_array(
                parser->compiler_session, sizeof(XrDestructurePattern *), (size_t) capacity);
            if (!xr_parser_healthy(parser)) return NULL;
            if (old_capacity > 0 && elements)
                do { if (!ast_copy(parser->compiler_session, _new_elements, elements,
                       sizeof(XrDestructurePattern *) * (size_t) old_capacity)) return NULL; } while (0);
            elements = _new_elements;
        }

        // Skip element: comma without identifier
        if (xr_parser_check(parser, TK_COMMA)) {
            do {
                elements[count++] = xr_pattern_skip(parser->compiler_session);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            continue;
        }

        // Wildcard: _
        if (xr_parser_match(parser, TK_UNDERSCORE)) {
            do {
                elements[count++] = xr_pattern_skip(parser->compiler_session);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
        // Identifier
        else if (xr_parser_match(parser, TK_NAME)) {
            char *name = copy_token_string(parser, &parser->previous);
            if (!xr_parser_healthy(parser)) return NULL;
            do {
                elements[count++] = xr_pattern_identifier(parser->compiler_session, name, NULL);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } else {
            do {
                xr_parser_error_expected_name(parser,
                                          "expected identifier or '_' in array destructuring");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }

        if (!xr_parser_check(parser, TK_RBRACKET)) {
            if (!xr_parser_match(parser, TK_COMMA)) {
                do {
                    xr_parser_error(parser, "expected ',' or ']'");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
        }
    }

    do {
        xr_parser_consume(parser, TK_RBRACKET, "expected ']'");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    return xr_pattern_array(parser->compiler_session, elements, count);
}

// Parse flat tuple destructuring pattern: `()`, `(x,)`, `(a, b, ...)`.
//
// `(x)` without the trailing comma is *not* a 1-tuple pattern — it is
// just a parenthesised identifier, mirroring the rule for tuple
// expressions. Callers therefore only enter this helper when they
// have positively identified a tuple destructuring head (the dispatch
// logic in xr_parse_let_stmt / fn-param / for-in handles the
// disambiguation).

static XrDestructurePattern *parse_destructure_binding(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    Token token=parser->current;XrDestructurePattern *pattern=NULL;
    if (xr_parser_match(parser,TK_UNDERSCORE))
        pattern=xr_pattern_skip(parser->compiler_session);
    else if (xr_parser_match(parser,TK_NAME)) {
        char *name=copy_token_string(parser,&parser->previous);
        if (!xr_parser_healthy(parser)) return NULL;
        pattern=xr_pattern_identifier(parser->compiler_session,name,NULL);
    } else pattern=xr_parse_destructure_pattern(parser);
    if (!xr_parser_healthy(parser) || !pattern) return NULL;
    pattern->line=token.line;pattern->column=token.column;
    pattern->end_line=parser->previous.line;
    pattern->end_column=parser->previous.column+parser->previous.length;
    return pattern;
}

XrDestructurePattern *xr_parse_tuple_pattern(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_tuple_pattern: NULL parser");
    // '(' already consumed
    XrDestructurePattern **elements = NULL;
    int count = 0;
    int capacity = 0;
    bool had_comma = false;

    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RPAREN) && !xr_parser_check(parser, TK_EOF)) {
        if (count >= capacity) {
            int old_capacity = capacity;
            do {
                capacity = xr_parser_grow_capacity(parser, capacity);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            XrDestructurePattern **_new_elements = (XrDestructurePattern **) ast_alloc_array(
                parser->compiler_session, sizeof(XrDestructurePattern *), (size_t) capacity);
            if (!xr_parser_healthy(parser)) return NULL;
            if (old_capacity > 0 && elements)
                do { if (!ast_copy(parser->compiler_session, _new_elements, elements,
                       sizeof(XrDestructurePattern *) * (size_t) old_capacity)) return NULL; } while (0);
            elements = _new_elements;
        }

        elements[count]=parse_destructure_binding(parser);
        if (!elements[count]) return NULL;
        ++count;
        // A trailing comma at the end is allowed (and mandatory for
        // the unary form `(x,)`); otherwise comma separates elements.
        if (xr_parser_check(parser, TK_RPAREN))
            break;
        if (xr_parser_match(parser, TK_COMMA)) had_comma = true;
        else {
            do {
                xr_parser_error(parser, "expected ',' or ')'");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
    }

    if (count == 1 && !had_comma) {
        xr_parser_error(parser, "single-element tuple binding requires a trailing comma");
        return NULL;
    }
    do {
        xr_parser_consume(parser, TK_RPAREN, "expected ')'");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    return xr_pattern_tuple(parser->compiler_session, elements, count);
}

// Unified entry point
static XrDestructurePattern *parse_destructure_pattern_inner(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_destructure_pattern: NULL parser");
    if (xr_parser_match(parser, TK_LBRACKET)) {
        return xr_parse_array_pattern(parser);
    } else if (xr_parser_match(parser, TK_LPAREN)) {
        return xr_parse_tuple_pattern(parser);
    } else if (xr_parser_match(parser, TK_LBRACE)) {
        xr_parser_error_at_previous(parser, "object destructuring was removed; use named field access");
        return NULL;
    } else {
        do {
            xr_parser_error(parser, "expected '(' or '[' for destructuring");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
}

XrDestructurePattern *xr_parse_destructure_pattern(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    if (++parser->recursion_depth>XR_PARSER_MAX_DEPTH) {
        --parser->recursion_depth;
        xr_parser_error(parser,"destructure nesting too deep (max 1000 levels)");
        return NULL;
    }
    XrDestructurePattern *pattern=parse_destructure_pattern_inner(parser);
    --parser->recursion_depth;
    return xr_parser_healthy(parser) ? pattern : NULL;
}

// Parse destructuring declaration: var [a, b] = expr  or  const {x, y} = expr
AstNode *xr_parse_destructure_declaration(Parser *parser, bool is_const) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_destructure_declaration: NULL parser");
    int line = parser->previous.line;
    int column = parser->previous.column;

    XrDestructurePattern *pattern = xr_parse_destructure_pattern(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    if (!pattern)
        return NULL;

    if (!xr_parser_match(parser, TK_ASSIGN)) {
        do {
            xr_parser_error(parser, "destructuring declaration requires initializer");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    AstNode *initializer = xr_parse_expression(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    if (!initializer) {
        do {
            xr_parser_error(parser, "expected initializer expression");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    AstNode *node=xr_ast_destructure_decl(parser->compiler_session, pattern, initializer, is_const, line);
    if(!xr_parser_healthy(parser) || !node)return NULL;
    node->column=column;node->end_line=parser->previous.line;
    node->end_column=parser->previous.column+parser->previous.length;
    return node;
}

// ========== Destructuring assignment helpers ==========

/*
 * Convert array literal to destructure pattern (for destructuring assignment)
 * [a, b, c] -> destructure pattern
 * Can only convert when all elements are variable references
 */
XrDestructurePattern *convert_array_literal_to_pattern(Parser *parser,
                                                       AstNode *array_literal) {
    if (!xr_parser_healthy(parser)) return NULL;
    if (array_literal->type != AST_ARRAY_LITERAL) {
        return NULL;
    }
    if (array_literal->as.array_literal.is_repeat)
        return NULL;

    int count = array_literal->as.array_literal.count;
    XrDestructurePattern **elements = (XrDestructurePattern **) ast_alloc_array(
        parser->compiler_session, sizeof(XrDestructurePattern *), (size_t) count);
    if (!xr_parser_healthy(parser)) return NULL;

    for (int i = 0; xr_parser_healthy(parser) && (i < count); i++) {
        AstNode *element = array_literal->as.array_literal.elements[i];

        // Check if element is variable reference
        if (element->type == AST_VARIABLE) {
            // Create identifier pattern
            do {
                elements[i] = xr_pattern_identifier(parser->compiler_session, element->as.variable.name, NULL);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } else {
            // Not a variable reference, cannot convert to destructure pattern
            return NULL;
        }
    }

    return xr_pattern_array(parser->compiler_session, elements, count);
}

/*
 * Convert tuple literal to destructure pattern (for destructuring assignment)
 * (a, b, c) -> tuple destructure pattern
 * Mirrors convert_array_literal_to_pattern; only succeeds when every element
 * is a bare variable reference, since assignment targets cannot evaluate
 * sub-expressions.
 */
XrDestructurePattern *convert_tuple_literal_to_pattern(Parser *parser,
                                                       AstNode *tuple_literal) {
    if (!xr_parser_healthy(parser)) return NULL;
    if (!tuple_literal || tuple_literal->type != AST_TUPLE_LITERAL ||
        (tuple_literal->as.tuple_literal.count && !tuple_literal->as.tuple_literal.elements)) {
        return NULL;
    }

    int count = tuple_literal->as.tuple_literal.count;
    XrDestructurePattern **elements = (XrDestructurePattern **) ast_alloc_array(
        parser->compiler_session, sizeof(XrDestructurePattern *), (size_t) count);
    if (!xr_parser_healthy(parser)) return NULL;

    for (int i = 0; xr_parser_healthy(parser) && (i < count); i++) {
        AstNode *element = tuple_literal->as.tuple_literal.elements[i];
        if (!element) return NULL;
        if (element->type == AST_VARIABLE) {
            do {
                elements[i] = xr_pattern_identifier(parser->compiler_session, element->as.variable.name, NULL);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } else {
            return NULL;
        }
    }

    return xr_pattern_tuple(parser->compiler_session, elements, count);
}

// Destructuring declaration/pattern parsing moved to xparse_destructure.c
