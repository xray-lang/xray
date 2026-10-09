/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_enum.c - Enum payload and method parsing
 */
#include "xparse_internal.h"
#include "../../base/xchecks.h"
#include "xtype_ref.h"

/* ========== Enum Declaration Parsing ========== */

static void parse_enum_variant_fields(Parser *parser, char ***out_names,
                                      XrNameSpan **out_name_spans, XrTypeRef ***out_types,
                                      int *out_count) {
    if (!xr_parser_healthy(parser)) return;
    XR_DCHECK(parser != NULL, "parse_enum_variant_fields: NULL parser");

    char **names = NULL;
    XrNameSpan *name_spans = NULL;
    XrTypeRef **types = NULL;
    int count = 0;
    int capacity = 0;

    do {
        xr_parser_consume(parser, TK_LBRACE, "expected '{' to start enum payload fields");
        if (!xr_parser_healthy(parser)) return;
    } while (0);
    if (xr_parser_check(parser, TK_RBRACE))
        do {
            xr_parser_error(parser, "payload enum declarations require at least one named field");
            if (!xr_parser_healthy(parser)) return;
        } while (0);

    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        do {
            xr_parser_consume(parser, TK_NAME, "enum payload field requires a name");
            if (!xr_parser_healthy(parser)) return;
        } while (0);
        Token name_token = parser->previous;
        char *field_name = xr_parser_token_string(parser, &name_token);
        if (!xr_parser_healthy(parser)) return;
        do {
            xr_parser_consume(parser, TK_COLON, "expected ':' after enum payload field name");
            if (!xr_parser_healthy(parser)) return;
        } while (0);
        XrTypeRef *field_type = xr_parse_type_annotation(parser);
        if (!xr_parser_healthy(parser)) return;

        if (!field_type) {
            do {
                xr_parser_error(parser, "expected type after enum payload field name");
                if (!xr_parser_healthy(parser)) return;
            } while (0);
            break;
        }

        if (count >= capacity) {
            int old_capacity = capacity;
            do {
                capacity = xr_parser_grow_capacity(parser, capacity);
                if (!xr_parser_healthy(parser)) return;
            } while (0);
            char **new_names = (char **) ast_alloc_array(parser->compiler_session, sizeof(char *),
                                                         (size_t) capacity);
            if (!xr_parser_healthy(parser)) return;
            XrNameSpan *new_name_spans = (XrNameSpan *) ast_alloc_array(
                parser->compiler_session, sizeof(XrNameSpan), (size_t) capacity);
            if (!xr_parser_healthy(parser)) return;
            XrTypeRef **new_types = (XrTypeRef **) ast_alloc_array(
                parser->compiler_session, sizeof(XrTypeRef *), (size_t) capacity);
            if (!xr_parser_healthy(parser)) return;
            if (old_capacity > 0) {
                do { if (!ast_copy(parser->compiler_session, new_names, names, sizeof(char *) * (size_t) old_capacity)) return; } while (0);
                do { if (!ast_copy(parser->compiler_session, new_name_spans, name_spans, sizeof(XrNameSpan) * (size_t) old_capacity)) return; } while (0);
                do { if (!ast_copy(parser->compiler_session, new_types, types, sizeof(XrTypeRef *) * (size_t) old_capacity)) return; } while (0);
            }
            names = new_names;
            name_spans = new_name_spans;
            types = new_types;
        }
        names[count] = field_name;
        name_spans[count] = (XrNameSpan) {.line = name_token.line, .column = name_token.column};
        types[count] = field_type;
        count++;

        if (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_match(parser, TK_COMMA)) {
            do {
                xr_parser_error(parser, "expected ',' or '}' after enum payload field");
                if (!xr_parser_healthy(parser)) return;
            } while (0);
            break;
        }
    }
    do {
        xr_parser_consume(parser, TK_RBRACE, "expected '}' after enum payload fields");
        if (!xr_parser_healthy(parser)) return;
    } while (0);

    *out_names = names;
    *out_name_spans = name_spans;
    *out_types = types;
    *out_count = count;
}

static bool enum_name_starts_method(Parser *parser) {
    if (!xr_parser_healthy(parser)) return false;
    if (!parser || !xr_parser_check(parser, TK_NAME))
        return false;
    XrParserStreamState saved = xr_parser_stream_save(parser);
    if (!xr_parser_healthy(parser)) return false;
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return false;
    } while (0);
    if (xr_parser_check(parser, TK_LT)) {
        do {
            xr_parser_stream_restore(parser, &saved);
            if (!xr_parser_healthy(parser)) return false;
        } while (0);
        return true;
    }
    if (!xr_parser_match(parser, TK_LPAREN)) {
        do {
            xr_parser_stream_restore(parser, &saved);
            if (!xr_parser_healthy(parser)) return false;
        } while (0);
        return false;
    }

    int depth = 1;
    while (xr_parser_healthy(parser) && depth > 0 && !xr_parser_check(parser, TK_EOF)) {
        if (xr_parser_check(parser, TK_LPAREN))
            depth++;
        else if (xr_parser_check(parser, TK_RPAREN))
            depth--;
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return false;
        } while (0);
    }
    bool is_method =
        depth == 0 && (xr_parser_check(parser, TK_ARROW) || xr_parser_check(parser, TK_COLON) ||
                       xr_parser_check(parser, TK_LBRACE));
    if (!xr_parser_healthy(parser)) return false;
    do {
        xr_parser_stream_restore(parser, &saved);
        if (!xr_parser_healthy(parser)) return false;
    } while (0);
    return is_method;
}

static bool enum_method_starts(Parser *parser) {
    if (!xr_parser_healthy(parser)) return false;
    return parser && (xr_parser_check(parser, TK_AT) || xr_parser_check(parser, TK_STATIC) ||
                      xr_parser_check(parser, TK_REF) || xr_parser_check_name(parser, "ref") ||
                      xr_parser_check(parser, TK_MOVE) || xr_parser_check_name(parser, "move") ||
                      xr_parser_check(parser, TK_FN) || enum_name_starts_method(parser));
}

static AstNode *parse_enum_method(Parser *parser, bool is_static, XrParamMode receiver_mode) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_enum_method: NULL parser");

    do {
        xr_parser_consume(parser, TK_NAME, "expected enum method name");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    char *name = xr_parser_token_string(parser, &parser->previous);
    if (!xr_parser_healthy(parser)) return NULL;
    int name_line = parser->previous.line;
    int name_col = parser->previous.column;

    AstNode *method = xr_parse_method_declaration(parser, name, name_line, name_col,
                                                  /* is_private */ false,
                                                  /* is_static */ is_static);
    if (!xr_parser_healthy(parser)) return NULL;
    if (method)
        method->as.method_decl.receiver_mode = receiver_mode;
    return method;
}

// Parse enum declaration (safe tagged aggregate)
// Syntax:
//   enum Color { Red, Green, Blue }
//   enum Result<T, E> { Ok { value: T }, Err { error: E } }
//   enum Shape { Circle { radius: f64 }, Rect { width: f64, height: f64 } }
AstNode *xr_parse_enum_declaration(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_enum_declaration: NULL parser");
    int line = parser->previous.line;

    // 'enum' keyword already consumed

    // Parse enum name
    do {
        xr_parser_consume(parser, TK_NAME, "expected enum name");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    char *enum_name = xr_parser_token_string(parser, &parser->previous);
    if (!xr_parser_healthy(parser)) return NULL;
    int name_column = parser->previous.column;

    /* Parse optional generic type parameters <T, E: Constraint, F = int>. The
     * helper installs a scope holding them so payload types resolve. */
    XrTypeScope *saved_scope = parser->type_scope;
    int type_param_count = 0;
    XrGenericParam **type_params = xr_parse_generic_params(parser, &type_param_count);
    if (!xr_parser_healthy(parser)) return NULL;
    XrTypeScope *generic_scope = type_param_count > 0 ? parser->type_scope : NULL;

    if (xr_parser_match(parser, TK_COLON)) {
        do {
            xr_parser_error(parser, "enum backing types have been removed");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_LBRACE) && !xr_parser_check(parser, TK_IMPLEMENTS) &&
               !xr_parser_check(parser, TK_EOF)) {
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
    }

    // Parse optional 'implements' clause
    XrTypeRef **interfaces = NULL;
    int interface_count = 0;
    int interface_capacity = 0;

    if (xr_parser_match(parser, TK_IMPLEMENTS)) {
        do {
            XrTypeRef *iface_ref = xr_parse_type_annotation(parser);
            if (!xr_parser_healthy(parser)) return NULL;
            if (!iface_ref)
                break;
            do {
                XR_PARSE_PUSH(parser, interfaces, interface_count, interface_capacity, iface_ref);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA));
    }

    // Parse enum body
    /* `enum Tree<T> where T: Comparable { ... }` */
    do {
        xr_parse_where_clause(parser, type_params, type_param_count);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    do {
        xr_parser_consume(parser, TK_LBRACE, "expected '{' to start enum body");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    // Collect variants and methods
    AstNode **members = NULL;
    int member_count = 0;
    int member_capacity = 0;

    AstNode **methods = NULL;
    int method_count = 0;
    int method_capacity = 0;

    if (xr_parser_check(parser, TK_RBRACE)) {
        do {
            xr_parser_error(parser, "enum requires at least one variant");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    /* Variant and method syntax are disjoint after one bounded lookahead:
     * variants end at a comma/brace, while methods continue to a body. */
    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF) &&
           !enum_method_starts(parser)) {
        if (parser->panic_mode) {
            do {
                xr_parser_synchronize(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_EOF))
                break;
            continue;
        }

        if (!xr_parser_check(parser, TK_NAME)) {
            do {
                xr_parser_error_expected_name(parser, "expected enum variant name");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            continue;
        }

        do {
            xr_parser_consume(parser, TK_NAME, "expected enum variant name");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        char *member_name = xr_parser_token_string(parser, &parser->previous);
        if (!xr_parser_healthy(parser)) return NULL;
        int member_line = parser->previous.line;
        int member_col = parser->previous.column;
        int member_name_len = parser->previous.length;

        char **payload_names = NULL;
        XrNameSpan *payload_name_spans = NULL;
        XrTypeRef **payload_types = NULL;
        int payload_count = 0;

        if (xr_parser_check(parser, TK_LBRACE)) {
            parse_enum_variant_fields(parser, &payload_names, &payload_name_spans, &payload_types,
                                      &payload_count);
        } else if (xr_parser_check(parser, TK_LPAREN)) {
            do {
                xr_parser_error(parser,
                            "payload enum declarations use named record fields, not '(...)'");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            int depth = 0;
            do {
                if (xr_parser_check(parser, TK_LPAREN))
                    depth++;
                else if (xr_parser_check(parser, TK_RPAREN))
                    depth--;
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            } while (xr_parser_healthy(parser) && depth > 0 && !xr_parser_check(parser, TK_EOF));
            parser->panic_mode = 0;
        } else if (xr_parser_match(parser, TK_ASSIGN)) {
            do {
                xr_parser_error(parser, "enum backing values have been removed");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            (void) xr_parse_expression(parser);
        }

        AstNode *member =
            xr_ast_enum_member(parser->compiler_session, member_name, payload_names,
                               payload_name_spans, payload_types, payload_count, member_line);
        if (!xr_parser_healthy(parser)) return NULL;
        member->column = member_col;
        member->end_line = member_line;
        member->end_column = member_col + member_name_len;

        do {
            XR_PARSE_PUSH(parser, members, member_count, member_capacity, member);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);

        if (!xr_parser_check(parser, TK_RBRACE) && !enum_method_starts(parser)) {
            if (!xr_parser_match(parser, TK_COMMA)) {
                do {
                    xr_parser_error(parser, "expected ',' between enum variants");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                break;
            }
            if (xr_parser_check(parser, TK_RBRACE) || enum_method_starts(parser))
                break;
        }
    }

    while (xr_parser_healthy(parser) && enum_method_starts(parser) && !xr_parser_check(parser, TK_RBRACE)) {
        if (parser->panic_mode) {
            do {
                xr_parser_synchronize(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_EOF))
                break;
            continue;
        }

        XrAttribute **attributes = NULL;
        int attr_count = 0;
        int attr_capacity = 0;
        while (xr_parser_healthy(parser) && xr_parser_check(parser, TK_AT)) {
            XrAttribute *attribute = xr_parse_single_attribute(parser);
            if (!xr_parser_healthy(parser)) return NULL;
            if (!attribute)
                break;
            if (!xr_parser_validate_enum_method_attr(parser, attribute))
                break;
            do {
                XR_PARSE_PUSH(parser, attributes, attr_count, attr_capacity, attribute);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
        if (!xr_parser_reject_duplicate_assertion_attrs(parser, attributes, attr_count))
            break;

        if (xr_parser_check(parser, TK_FN)) {
            do {
                xr_parser_error(parser, "enum methods do not use the 'fn' keyword");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            break;
        }

        bool is_static = false;
        XrParamMode receiver_mode = XR_PARAM_READ;
        XrNameSpan receiver_mode_span = {0};
        if (xr_parser_match(parser, TK_STATIC)) {
            is_static = true;
        } else if (xr_parser_match(parser, TK_REF) || xr_parser_match_name(parser, "ref")) {
            receiver_mode = XR_PARAM_REF;
            receiver_mode_span = (XrNameSpan){parser->previous.line, parser->previous.column};
        } else if (xr_parser_match(parser, TK_MOVE) || xr_parser_match_name(parser, "move")) {
            receiver_mode = XR_PARAM_MOVE;
            receiver_mode_span = (XrNameSpan){parser->previous.line, parser->previous.column};
        }

        AstNode *method = parse_enum_method(parser, is_static, receiver_mode);
        if (!xr_parser_healthy(parser)) return NULL;
        if (method) {
            if (!ast_work(parser->compiler_session, sizeof(XrNameSpan))) return NULL;
            method->as.method_decl.receiver_mode_span = receiver_mode_span;
            method->as.method_decl.attributes = attributes;
            method->as.method_decl.attr_count = attr_count;
            do {
                XR_PARSE_PUSH(parser, methods, method_count, method_capacity, method);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
    }

    do {
        xr_parser_consume(parser, TK_RBRACE, "expected '}' to end enum body");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    int enum_end_line = parser->previous.line;
    int enum_end_column = parser->previous.column + 1;

    // Restore type_scope after parsing enum body
    if (type_param_count > 0) {
        parser->type_scope = saved_scope;
        do {
            xr_parser_type_scope_free(parser, generic_scope);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    AstNode *node = xr_ast_enum_decl(parser->compiler_session, enum_name, members, member_count,
                                     methods, method_count, type_params, type_param_count,
                                     interfaces, interface_count, line);
    if (!xr_parser_healthy(parser)) return NULL;
    node->column = name_column;
    node->end_line = enum_end_line;
    node->end_column = enum_end_column;
    return node;
}
