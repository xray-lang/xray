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
 *   Flat (non-nested) destructuring only:
 *     - Array: let [a, b, c] = expr   (positional)
 *     - Object: let {x, y} = expr     (same-name fields, JSON object or class instance)
 *   No nesting, no renaming, no rest (...), no Map destructuring.
 */

#include "xparse_internal.h"
#include "../../base/xchecks.h"
#include <stdlib.h>
#include <string.h>

static char *copy_token_string(Parser *parser, Token *token) {
    char *str = (char *) ast_alloc(parser->X, (size_t) token->length + 1);
    memcpy(str, token->start, token->length);
    str[token->length] = '\0';
    return str;
}

// Parse flat array destructuring: [a, b, c] or [a, _, c]
XrDestructurePattern *xr_parse_array_pattern(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_array_pattern: NULL parser");
    // '[' already consumed
    XrDestructurePattern **elements = NULL;
    int count = 0;
    int capacity = 0;

    while (!xr_parser_check(parser, TK_RBRACKET) && !xr_parser_check(parser, TK_EOF)) {
        if (count >= capacity) {
            int old_capacity = capacity;
            capacity = (capacity == 0) ? 4 : capacity * 2;
            XrDestructurePattern **_new_elements = (XrDestructurePattern **) ast_alloc_array(
                parser->X, sizeof(XrDestructurePattern *), (size_t) capacity);
            if (old_capacity > 0 && elements)
                memcpy(_new_elements, elements,
                       sizeof(XrDestructurePattern *) * (size_t) old_capacity);
            elements = _new_elements;
        }

        // Skip element: comma without identifier
        if (xr_parser_check(parser, TK_COMMA)) {
            elements[count++] = xr_pattern_skip(parser->X);
            xr_parser_advance(parser);
            continue;
        }

        // Wildcard: _
        if (xr_parser_match(parser, TK_UNDERSCORE)) {
            elements[count++] = xr_pattern_skip(parser->X);
        }
        // Identifier
        else if (xr_parser_match(parser, TK_NAME)) {
            char *name = copy_token_string(parser, &parser->previous);
            elements[count++] = xr_pattern_identifier(parser->X, name, NULL);
        } else {
            xr_parser_error_expected_name(parser,
                                          "expected identifier or '_' in array destructuring");
            return NULL;
        }

        if (!xr_parser_check(parser, TK_RBRACKET)) {
            if (!xr_parser_match(parser, TK_COMMA)) {
                xr_parser_error(parser, "expected ',' or ']'");
                return NULL;
            }
        }
    }

    xr_parser_consume(parser, TK_RBRACKET, "expected ']'");
    return xr_pattern_array(parser->X, elements, count);
}

// Parse flat object destructuring: {name, age}
// Variable names must match field names exactly (no renaming, no defaults).
XrDestructurePattern *xr_parse_object_pattern(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_object_pattern: NULL parser");
    // '{' already consumed
    char **field_names = NULL;
    XrDestructurePattern **patterns = NULL;
    int count = 0;
    int capacity = 0;

    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        if (count >= capacity) {
            int old_capacity = capacity;
            capacity = (capacity == 0) ? 4 : capacity * 2;
            char **_new_field_names =
                (char **) ast_alloc_array(parser->X, sizeof(char *), (size_t) capacity);
            if (old_capacity > 0 && field_names)
                memcpy(_new_field_names, field_names, sizeof(char *) * (size_t) old_capacity);
            field_names = _new_field_names;

            XrDestructurePattern **_new_patterns = (XrDestructurePattern **) ast_alloc_array(
                parser->X, sizeof(XrDestructurePattern *), (size_t) capacity);
            if (old_capacity > 0 && patterns)
                memcpy(_new_patterns, patterns,
                       sizeof(XrDestructurePattern *) * (size_t) old_capacity);
            patterns = _new_patterns;
        }

        if (!xr_parser_match(parser, TK_NAME)) {
            xr_parser_error_expected_name(parser, "expected field name in object destructuring");
            return NULL;
        }

        char *field = copy_token_string(parser, &parser->previous);

        // Reject renaming syntax: {name: alias}
        if (xr_parser_check(parser, TK_COLON)) {
            xr_parser_error(parser, "renaming in destructuring is not supported; "
                                    "variable name must match field name");
            return NULL;
        }

        field_names[count] = field;
        patterns[count] = xr_pattern_identifier(parser->X, field, NULL);
        count++;

        if (!xr_parser_check(parser, TK_RBRACE)) {
            if (!xr_parser_match(parser, TK_COMMA)) {
                xr_parser_error(parser, "expected ',' or '}'");
                return NULL;
            }
        }
    }

    xr_parser_consume(parser, TK_RBRACE, "expected '}'");
    return xr_pattern_object(parser->X, field_names, patterns, count, true);
}

// Unified entry point
XrDestructurePattern *xr_parse_destructure_pattern(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_destructure_pattern: NULL parser");
    if (xr_parser_match(parser, TK_LBRACKET)) {
        return xr_parse_array_pattern(parser);
    } else if (xr_parser_match(parser, TK_LBRACE)) {
        return xr_parse_object_pattern(parser);
    } else {
        xr_parser_error(parser, "expected '[' or '{' for destructuring");
        return NULL;
    }
}

// Parse destructuring declaration: let [a, b] = expr  or  const {x, y} = expr
AstNode *xr_parse_destructure_declaration(Parser *parser, bool is_const) {
    XR_DCHECK(parser != NULL, "parse_destructure_declaration: NULL parser");
    int line = parser->previous.line;

    XrDestructurePattern *pattern = xr_parse_destructure_pattern(parser);
    if (!pattern)
        return NULL;

    if (!xr_parser_match(parser, TK_ASSIGN)) {
        xr_parser_error(parser, "destructuring declaration requires initializer");
        return NULL;
    }

    AstNode *initializer = xr_parse_expression(parser);
    if (!initializer) {
        xr_parser_error(parser, "expected initializer expression");
        return NULL;
    }

    return xr_ast_destructure_decl(parser->X, pattern, initializer, is_const, line);
}
