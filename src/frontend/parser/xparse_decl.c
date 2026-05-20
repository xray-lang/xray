/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_decl.c - Function and container declaration parsing
 *
 * KEY CONCEPT:
 *   Parsing for function declarations, class declarations,
 *   array/map/set literals, destructuring patterns, and
 *   enum/type declarations.
 */

#include "xparse_internal.h"
#include "xtype_ref.h"
#include "../../base/xchecks.h"
#include "../../base/xarena.h"
#include "../../runtime/xisolate_api.h"
#include "xtype_scope.h"
#include "../xdiag_fmt.h"

/* ========== Local Cleanup Helpers ========== */

// All parser allocations go through the parse arena; these helpers are
// retained for API compatibility but are now no-ops. Arena destroy at
// parse end releases every buffer allocated here.
static inline void free_generic_params(XrGenericParam **type_params, int count) {
    (void) type_params;
    (void) count;
}

static inline void free_param_nodes(XrayIsolate *X, XrParamNode **params, int count) {
    (void) X;
    (void) params;
    (void) count;
}

/* ========== Function Parsing ========== */

// Parse single attribute: @test, @test(skip), @test(timeout: 30), etc.
static XrAttribute *xr_parse_single_attribute(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_single_attribute: NULL parser");
    xr_parser_advance(parser);  // Consume @

    xr_parser_consume(parser, TK_NAME, "expected attribute name");
    Token name_token = parser->previous;

    XrAttribute *attr = (XrAttribute *) ast_alloc(parser->X, sizeof(XrAttribute));
    attr->kind = ATTR_NONE;
    attr->timeout = 0;
    if (name_token.length == 4 && memcmp(name_token.start, "test", 4) == 0) {
        attr->kind = ATTR_TEST;

        // Check for params: @test(skip) or @test(timeout: 30)
        if (xr_parser_match(parser, TK_LPAREN)) {
            if (xr_parser_check(parser, TK_NAME)) {
                Token param = parser->current;
                xr_parser_advance(parser);

                if (param.length == 4 && memcmp(param.start, "skip", 4) == 0) {
                    attr->kind = ATTR_TEST_SKIP;
                } else if (param.length == 7 && memcmp(param.start, "timeout", 7) == 0) {
                    attr->kind = ATTR_TEST_TIMEOUT;
                    xr_parser_consume(parser, TK_COLON, "expected ':' after timeout");
                    xr_parser_consume(parser, TK_LITERAL_INT, "expected timeout seconds");
                    Token timeout_token = parser->previous;
                    char buf[32];
                    int len = timeout_token.length < 31 ? timeout_token.length : 31;
                    memcpy(buf, timeout_token.start, len);
                    buf[len] = '\0';
                    attr->timeout = atoi(buf);
                }
            }
            xr_parser_consume(parser, TK_RPAREN, "expected ')' to close attribute params");
        }
    } else if (name_token.length == 11 && memcmp(name_token.start, "before_each", 11) == 0) {
        attr->kind = ATTR_BEFORE_EACH;
    } else if (name_token.length == 10 && memcmp(name_token.start, "after_each", 10) == 0) {
        attr->kind = ATTR_AFTER_EACH;
    } else if (name_token.length == 10 && memcmp(name_token.start, "before_all", 10) == 0) {
        attr->kind = ATTR_BEFORE_ALL;
    } else if (name_token.length == 9 && memcmp(name_token.start, "after_all", 9) == 0) {
        attr->kind = ATTR_AFTER_ALL;
    } else if (name_token.length == 6 && memcmp(name_token.start, "native", 6) == 0) {
        attr->kind = ATTR_NATIVE;
    } else if (name_token.length == 10 && memcmp(name_token.start, "deprecated", 10) == 0) {
        attr->kind = ATTR_DEPRECATED;
        // Optional message: @deprecated("use X instead")
        if (xr_parser_match(parser, TK_LPAREN)) {
            // Consume and ignore the message string for now
            if (xr_parser_check(parser, TK_LITERAL_STRING)) {
                xr_parser_advance(parser);
            }
            xr_parser_consume(parser, TK_RPAREN, "expected ')' to close @deprecated");
        }
    } else {
        xr_parser_error(parser, "unknown attribute name");
        return NULL;
    }

    return attr;
}

// Check if any attribute in the list has the given kind.
static bool attrs_has(XrAttribute **attrs, int count, AttributeKind kind) {
    for (int i = 0; i < count; i++) {
        if (attrs[i] && attrs[i]->kind == kind)
            return true;
    }
    return false;
}

// Parse attributed declaration: @test fn ..., @native class ..., etc.
static AstNode *xr_parse_attributed_declaration(Parser *parser) {
    XrAttribute **attributes = NULL;
    int attr_count = 0;
    int attr_capacity = 0;

    while (xr_parser_check(parser, TK_AT)) {
        XrAttribute *attr = xr_parse_single_attribute(parser);
        if (!attr)
            return NULL;
        XR_PARSE_PUSH(parser, attributes, attr_count, attr_capacity, attr);
    }

    bool is_native = attrs_has(attributes, attr_count, ATTR_NATIVE);

    // @native class / @native final class
    if (xr_parser_check(parser, TK_CLASS) || xr_parser_check(parser, TK_FINAL)) {
        bool is_final = xr_parser_match(parser, TK_FINAL);
        if (is_final) {
            if (!xr_parser_match(parser, TK_CLASS)) {
                xr_parser_error_at_current(parser, "expected 'class' after 'final'");
                return NULL;
            }
        } else {
            xr_parser_advance(parser);  // consume TK_CLASS
        }
        // Set flag so method body parsing is skipped for @native classes
        parser->parsing_native_class = is_native;
        AstNode *cls = xr_parse_class_declaration(parser);
        parser->parsing_native_class = false;
        if (!cls)
            return NULL;
        cls->as.class_decl.is_final = is_final;
        cls->as.class_decl.is_native = is_native;
        return cls;
    }

    // @native struct
    if (xr_parser_match(parser, TK_STRUCT)) {
        parser->parsing_native_class = is_native;
        AstNode *st = xr_parse_struct_declaration(parser);
        parser->parsing_native_class = false;
        if (!st)
            return NULL;
        st->as.class_decl.is_native = is_native;
        return st;
    }

    // @test fn ..., @native fn ...
    if (xr_parser_match(parser, TK_FN)) {
        AstNode *func = xr_parse_function_declaration(parser);
        if (!func)
            return NULL;
        func->as.function_decl.attributes = attributes;
        func->as.function_decl.attr_count = attr_count;
        return func;
    }

    xr_parser_error_at_current(parser, "expected 'fn', 'class', or 'struct' after attribute");
    return NULL;
}

// Parse function declaration: fn add(a, b) { return a + b }
AstNode *xr_parse_function_declaration(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_function_declaration: NULL parser");
    int line = parser->previous.line;

    xr_parser_consume(parser, TK_NAME, "expected function name");
    Token name_token = parser->previous;
    int name_column = name_token.column;

    char *func_name = (char *) ast_alloc(parser->X, (size_t) name_token.length + 1);
    memcpy(func_name, name_token.start, name_token.length);
    func_name[name_token.length] = '\0';

    // Parse generic type params <T: Constraint, U>
    XrGenericParam **type_params = NULL;
    int type_param_count = 0;
    int type_param_capacity = 0;

    if (xr_parser_match(parser, TK_LT)) {
        do {
            xr_parser_consume(parser, TK_NAME, "expected type parameter name");
            Token param_token = parser->previous;

            char *param_name = (char *) ast_alloc(parser->X, (size_t) param_token.length + 1);
            memcpy(param_name, param_token.start, param_token.length);
            param_name[param_token.length] = '\0';

            // Parse optional intersection constraint <T: Interface1 & Interface2 & ...>
            XrTypeRef **constraints = NULL;
            int constraint_count = 0;
            if (xr_parser_match(parser, TK_COLON)) {
                constraints = xr_parse_constraint_list(parser, &constraint_count);
            }

            XrGenericParam *gp = (XrGenericParam *) ast_alloc(parser->X, sizeof(XrGenericParam));
            gp->name = param_name;
            gp->constraints = constraints;
            gp->constraint_count = constraint_count;
            XR_PARSE_PUSH(parser, type_params, type_param_count, type_param_capacity, gp);

        } while (xr_parser_match(parser, TK_COMMA));

        xr_parser_consume(parser, TK_GT, "expected '>' to close generic params");
    }

    // Register generic type params in type_scope for type annotation parsing
    // This allows T in "fn identity<T>(x: T): T" to be recognised as type param.
    XrTypeScope *saved_scope = parser->type_scope;
    if (type_param_count > 0) {
        XrTypeScope *generic_scope = xr_type_scope_new(parser->type_scope);
        for (int i = 0; i < type_param_count; i++) {
            XrTypeRef *type_param = xr_tref_type_param(parser->X, type_params[i]->name);
            xr_type_scope_define(generic_scope, type_params[i]->name, type_param);
        }
        parser->type_scope = generic_scope;
    }

    xr_parser_consume(parser, TK_LPAREN, "expected '(' after function name");

    XrParamNode **params = NULL;
    int param_count = 0;
    int param_capacity = 0;
    int required_count = 0;
    bool seen_default = false;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            if (param_count >= param_capacity) {
                int _old_cap_param_capacity = (int) param_capacity;
                param_capacity = _old_cap_param_capacity == 0 ? 4 : _old_cap_param_capacity * 2;
                XrParamNode **_new_params = (XrParamNode **) ast_alloc_array(
                    parser->X, sizeof(XrParamNode *), (size_t) param_capacity);
                if (_old_cap_param_capacity > 0 && params)
                    memcpy(_new_params, params,
                           sizeof(XrParamNode *) * (size_t) _old_cap_param_capacity);
                params = _new_params;
            }

            // Check rest param: ...args
            if (xr_parser_match(parser, TK_DOT_DOT_DOT)) {
                xr_parser_consume(parser, TK_NAME, "expected parameter name after ...");
                Token rest_token = parser->previous;

                char temp_name[64];
                snprintf(temp_name, sizeof(temp_name), "%.*s", rest_token.length, rest_token.start);

                XrParamNode *rest_param =
                    xr_param_node_new(parser->X, temp_name, rest_token.line, rest_token.column);
                rest_param->is_rest = true;

                /* Optional element type annotation: ...args: int */
                if (xr_parser_match(parser, TK_COLON)) {
                    rest_param->type = xr_parse_type_annotation(parser);
                }
                params[param_count++] = rest_param;

                if (xr_parser_check(parser, TK_COMMA)) {
                    xr_parser_error(parser, "rest parameter must be last");
                    goto fail;
                }
                break;
            }

            // Check for destructure pattern param: [x, y], {x, y} or (x, y)
            if (xr_parser_check(parser, TK_LBRACKET) || xr_parser_check(parser, TK_LBRACE) ||
                xr_parser_check(parser, TK_LPAREN)) {
                XrDestructurePattern *pattern = xr_parse_destructure_pattern(parser);
                if (!pattern) {
                    xr_parser_error(parser, "failed to parse destructure parameter");
                    goto fail;
                }

                // Generate temp name for destructure param
                char temp_name[32];
                snprintf(temp_name, sizeof(temp_name), "__param%d", param_count);

                XrParamNode *param = xr_param_node_new(parser->X, temp_name, line, 0);
                param->pattern = pattern;

                /* A destructured parameter still needs a type annotation
                 * so the analyzer can infer the constituent types. The
                 * annotation lives on the outer XrParamNode and applies
                 * to the temp variable that the destructure binds to. */
                if (xr_parser_match(parser, TK_COLON)) {
                    param->type = xr_parse_type_annotation(parser);
                }

                params[param_count++] = param;
                required_count++;
            } else {
                // Regular parameter name
                xr_parser_consume(parser, TK_NAME,
                                  "expected parameter name or destructure pattern");
                Token param_token = parser->previous;

                char param_name[256];
                snprintf(param_name, sizeof(param_name), "%.*s", param_token.length,
                         param_token.start);

                XrParamNode *param =
                    xr_param_node_new(parser->X, param_name, param_token.line, param_token.column);

                // Parse optional type annotation with in/ref modifier
                if (xr_parser_match(parser, TK_COLON)) {
                    if (xr_parser_match(parser, TK_IN)) {
                        param->passing_mode = XR_PARAM_IN;
                    } else if (xr_parser_match_name(parser, "ref")) {
                        param->passing_mode = XR_PARAM_REF;
                    }
                    param->type = xr_parse_type_annotation(parser);
                }

                // Parse optional default value
                if (xr_parser_match(parser, TK_ASSIGN)) {
                    param->default_value = xr_parse_expression(parser);
                    seen_default = true;

                    // Infer type from default value if no explicit type annotation
                    if (param->type == NULL && param->default_value != NULL) {
                        AstNode *dv = param->default_value;
                        switch (dv->type) {
                            case AST_LITERAL_INT:
                                param->type = xr_tref_int(parser->X);
                                break;
                            case AST_LITERAL_FLOAT:
                                param->type = xr_tref_float(parser->X);
                                break;
                            case AST_LITERAL_STRING:
                            case AST_TEMPLATE_STRING:
                                param->type = xr_tref_string(parser->X);
                                break;
                            case AST_LITERAL_TRUE:
                            case AST_LITERAL_FALSE:
                                param->type = xr_tref_bool(parser->X);
                                break;
                            case AST_ARRAY_LITERAL:
                                param->type = xr_tref_named(parser->X, "Array");
                                break;
                            case AST_MAP_LITERAL:
                                param->type = xr_tref_named(parser->X, "Map");
                                break;
                            case AST_SET_LITERAL:
                                param->type = xr_tref_named(parser->X, "Set");
                                break;
                            case AST_OBJECT_LITERAL:
                                param->type = xr_tref_named(parser->X, "Json");
                                break;
                            default:
                                break;
                        }
                    }
                } else if (seen_default) {
                    xr_parser_error(parser, "required parameter cannot follow optional parameter");
                    params[param_count++] = param;
                    goto fail;
                } else {
                    required_count++;
                }

                params[param_count++] = param;
            }

        } while (xr_parser_match(parser, TK_COMMA));
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')' after parameter list");

    // Parse optional return type annotation: `fn foo(...) -> T { ... }`.
    // The unified arrow `->` is the only legal separator (see task 082).
    XrTypeRef *return_type = NULL;
    if (xr_parser_match(parser, TK_ARROW)) {
        return_type = xr_parse_type_annotation(parser);
    } else if (xr_parser_check(parser, TK_COLON)) {
        // Legacy syntax `fn foo(): T` is no longer accepted. Emit a clear
        // migration hint and recover by parsing the type so the rest of
        // the function still parses.
        xr_parser_advance(parser);  // consume ':'
        xr_parser_error(parser, "use '->' instead of ':' for function return type, "
                                "e.g. fn foo() -> int");
        parser->panic_mode = 0;
        return_type = xr_parse_type_annotation(parser);
    }

    // Parse function body (must be block)
    xr_parser_consume(parser, TK_LBRACE, "function body must use braces { }");
    AstNode *body = xr_parse_block(parser);

    // If has destructure params, insert destructure code at function body start
    for (int i = 0; i < param_count; i++) {
        if (params[i] && params[i]->pattern != NULL) {
            AstNode *param_var = xr_ast_variable(parser->X, params[i]->name, line);

            // Create destructure decl: let [x, y] = __param0
            AstNode *destructure_decl =
                xr_ast_destructure_decl(parser->X, params[i]->pattern, param_var, false, line);
            // Don't free pattern here, it's now owned by destructure_decl
            params[i]->pattern = NULL;

            BlockNode *block = &body->as.block;
            if (block->count >= block->capacity) {
                int _old_cap_block__capacity = (int) block->capacity;
                block->capacity = _old_cap_block__capacity == 0 ? 4 : _old_cap_block__capacity * 2;
                AstNode **_new_block_statements = (AstNode **) ast_alloc_array(
                    parser->X, sizeof(AstNode *), (size_t) block->capacity);
                if (_old_cap_block__capacity > 0 && block->statements)
                    memcpy(_new_block_statements, block->statements,
                           sizeof(AstNode *) * (size_t) _old_cap_block__capacity);
                block->statements = _new_block_statements;
            }

            // Shift existing statements back
            for (int j = block->count; j > 0; j--) {
                block->statements[j] = block->statements[j - 1];
            }

            block->statements[0] = destructure_decl;
            block->count++;
        }
    }

    AstNode *func_decl =
        xr_ast_function_decl(parser->X, func_name, params, param_count, body, line);
    func_decl->column = name_column;
    func_decl->end_line = body->end_line;
    func_decl->end_column = body->end_column;

    func_decl->as.function_decl.return_type = return_type;
    func_decl->as.function_decl.required_count = required_count;
    func_decl->as.function_decl.type_params = type_params;
    func_decl->as.function_decl.type_param_count = type_param_count;

    // Restore original type_scope after parsing generic function
    if (type_param_count > 0) {
        parser->type_scope = saved_scope;
    }

    return func_decl;

fail:
    // Release everything the parser still owns locally. type_params and
    // params have not yet been transferred to the AST on this path.
    if (type_param_count > 0) {
        parser->type_scope = saved_scope;
    }
    free_generic_params(type_params, type_param_count);
    free_param_nodes(parser->X, params, param_count);
    return NULL;
}

// Parse one call argument, optionally a spread `...expr`. The spread
// source must be a tuple value; the analyzer expands its static arity
// into individual positional arguments.
//
// A bare `_` argument is accepted as a wildcard placeholder so that
// ADT pattern parsing (`R.Err(_)`) can later detect it; in normal call
// position the analyzer rejects it.
AstNode *xr_parse_call_argument(Parser *parser) {
    int line = parser->current.line;
    if (xr_parser_match(parser, TK_DOT_DOT_DOT)) {
        AstNode *inner = xr_parse_expression(parser);
        if (!inner)
            return NULL;
        return xr_ast_spread_expr(parser->X, inner, line);
    }
    if (xr_parser_match(parser, TK_UNDERSCORE)) {
        return xr_ast_variable(parser->X, "_", line);
    }
    return xr_parse_expression(parser);
}

// Parse function call: add(1, 2) or add(...t, 3)
AstNode *xr_parse_call_expr(Parser *parser, AstNode *callee) {
    XR_DCHECK(parser != NULL, "parse_call_expr: NULL parser");
    int line = parser->previous.line;

    AstNode **arguments = NULL;
    int arg_count = 0;
    int arg_capacity = 0;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            XR_PARSE_PUSH(parser, arguments, arg_count, arg_capacity,
                          xr_parse_call_argument(parser));
        } while (xr_parser_match(parser, TK_COMMA));
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')' after argument list");

    return xr_ast_call_expr(parser->X, callee, arguments, arg_count, line);
}

/* ========== Array Parsing ========== */

// Parse array literal or Map literal (smart detection)
// [1, 2, 3] -> array, ["key": value, ...] -> Map
AstNode *xr_parse_array_literal(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_array_literal: NULL parser");
    int line = parser->previous.line;

    if (xr_parser_match(parser, TK_RBRACKET)) {
        return xr_ast_array_literal(parser->X, NULL, 0, line);
    }

    // Parse first expression, then check ':' for Map or ',' for array
    AstNode *first_expr = xr_parse_expression(parser);
    if (xr_parser_match(parser, TK_COLON)) {
        // Map: ["key": value, ...]
        AstNode **keys = NULL;
        AstNode **values = NULL;
        int count = 0;
        int capacity = 4;

        keys = (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), (size_t) capacity);
        values = (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), (size_t) capacity);

        keys[0] = first_expr;
        values[0] = xr_parse_expression(parser);
        count = 1;

        while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RBRACKET)) {
            if (count >= capacity) {
                int old_capacity = capacity;
                capacity *= 2;

                AstNode **_new_keys =
                    (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), (size_t) capacity);
                if (old_capacity > 0 && keys)
                    memcpy(_new_keys, keys, sizeof(AstNode *) * (size_t) old_capacity);
                keys = _new_keys;

                AstNode **_new_values =
                    (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), (size_t) capacity);
                if (old_capacity > 0 && values)
                    memcpy(_new_values, values, sizeof(AstNode *) * (size_t) old_capacity);
                values = _new_values;
            }

            keys[count] = xr_parse_expression(parser);

            if (!xr_parser_match(parser, TK_COLON)) {
                xr_parser_error(parser, "expected ':' after Map key");
                return xr_ast_map_literal(parser->X, NULL, NULL, 0, line);
            }

            values[count] = xr_parse_expression(parser);
            count++;
        }

        xr_parser_consume(parser, TK_RBRACKET, "expected ']' at end of Map literal");

        return xr_ast_map_literal(parser->X, keys, values, count, line);

    } else {
        // Array: [1, 2, 3]
        AstNode **elements = NULL;
        int count = 0;
        int capacity = 4;

        elements = (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), (size_t) capacity);

        elements[0] = first_expr;
        count = 1;

        while (xr_parser_match(parser, TK_COMMA)) {
            if (xr_parser_check(parser, TK_RBRACKET)) {
                break;
            }

            XR_PARSE_PUSH(parser, elements, count, capacity, xr_parse_expression(parser));
        }

        xr_parser_consume(parser, TK_RBRACKET, "expected ']' at end of array");

        return xr_ast_array_literal(parser->X, elements, count, line);
    }
}

/*
 * Parse object literal {} or Map literal {key => value}
 * Distinguish by separator:
 *   - `:` separator -> Json object literal
 *   - `=>` separator -> Map literal
 * Supports computed property syntax: { [expr]: value } or { [expr] => value }
 */
AstNode *xr_parse_object_literal(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_object_literal: NULL parser");
    int line = parser->previous.line;

    // '{' already consumed

    // Empty object {} -> Json
    if (xr_parser_match(parser, TK_RBRACE)) {
        return xr_ast_object_literal(parser->X, NULL, NULL, NULL, 0, line);
    }

    // Collect key-value pairs
    AstNode **keys = NULL;
    AstNode **values = NULL;
    bool *computed = NULL;
    bool has_computed = false;
    int count = 0;
    int capacity = 0;
    bool is_map = false;                // Whether it's a Map (determined by => separator)
    bool separator_determined = false;  // Whether separator has been determined

    do {
        // Expand capacity
        if (count >= capacity) {
            int _old_cap_capacity = (int) capacity;
            capacity = _old_cap_capacity == 0 ? 4 : _old_cap_capacity * 2;
            AstNode **_new_keys =
                (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), (size_t) capacity);
            if (_old_cap_capacity > 0 && keys)
                memcpy(_new_keys, keys, sizeof(AstNode *) * (size_t) _old_cap_capacity);
            keys = _new_keys;

            AstNode **_new_values =
                (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), (size_t) capacity);
            if (_old_cap_capacity > 0 && values)
                memcpy(_new_values, values, sizeof(AstNode *) * (size_t) _old_cap_capacity);
            values = _new_values;

            bool *_new_computed =
                (bool *) ast_alloc_array(parser->X, sizeof(bool), (size_t) capacity);
            if (_old_cap_capacity > 0 && computed)
                memcpy(_new_computed, computed, sizeof(bool) * (size_t) _old_cap_capacity);
            computed = _new_computed;
        }

        // Parse key
        AstNode *key = NULL;
        bool is_computed = false;
        const char *shorthand_name = NULL;

        // Computed property syntax: [expr]
        if (xr_parser_match(parser, TK_LBRACKET)) {
            key = xr_parse_expression(parser);
            xr_parser_consume(parser, TK_RBRACKET, "expected ']' after computed property");
            is_computed = true;
            has_computed = true;
        }
        // String literal as key
        else if (xr_parser_check(parser, TK_LITERAL_STRING)) {
            Token key_token = parser->current;
            xr_parser_advance(parser);

            char *key_str = (char *) ast_alloc(parser->X, (size_t) key_token.length - 1);
            memcpy(key_str, key_token.start + 1, key_token.length - 2);
            key_str[key_token.length - 2] = '\0';
            key = xr_ast_literal_string(parser->X, key_str, line);
            is_computed = false;
        }
        // Numeric literal as key: only Map allows this, and Map literals must
        // use the `#{ ... }` prefix form (see task 082).
        else if (xr_parser_check(parser, TK_LITERAL_INT) ||
                 xr_parser_check(parser, TK_LITERAL_FLOAT)) {
            xr_parser_error(
                parser, "Json object does not support numeric keys; use `#{ key: value }` for Map");
            return xr_ast_literal_null(parser->X, line);
        }
        // Identifier or keyword as key (allow keywords like 'type', 'int', etc.)
        else if (xr_parser_check(parser, TK_NAME) ||
                 // Type keywords
                 xr_parser_check(parser, TK_TYPE_ALIAS) || xr_parser_check(parser, TK_INT) ||
                 xr_parser_check(parser, TK_FLOAT) || xr_parser_check(parser, TK_STRING) ||
                 xr_parser_check(parser, TK_BOOL) ||
                 // Common keywords
                 xr_parser_check(parser, TK_CLASS) || xr_parser_check(parser, TK_ENUM) ||
                 xr_parser_check(parser, TK_STATIC) || xr_parser_check(parser, TK_AS) ||
                 xr_parser_check(parser, TK_IN) || xr_parser_check(parser, TK_IS)) {
            Token key_token = parser->current;
            xr_parser_advance(parser);

            char *key_str = (char *) ast_alloc(parser->X, (size_t) key_token.length + 1);
            memcpy(key_str, key_token.start, key_token.length);
            key_str[key_token.length] = '\0';
            key = xr_ast_literal_string(parser->X, key_str, line);
            is_computed = false;
            if (key_token.type == TK_NAME)
                shorthand_name = key_str;
        } else {
            xr_parser_error(
                parser,
                "literal key must be identifier, string, number or [expr] computed property");
            return xr_ast_literal_null(parser->X, line);
        }

        keys[count] = key;
        computed[count] = is_computed;

        // `{ ... }` is always a Json/Object literal in xray (task 082).
        // The only legal key-value separator is `:`. Map literals must use
        // the `#{ k: v }` prefix form. The unified arrow `->` is reserved
        // for function / branch arrows and is rejected here with a hint.
        if (xr_parser_match(parser, TK_COLON)) {
            separator_determined = true;
        } else if (xr_parser_check(parser, TK_ARROW)) {
            xr_parser_error(
                parser,
                "`->` is not a valid separator in Json literal; use `#{ key: value }` for Map");
            return xr_ast_literal_null(parser->X, line);
        } else if (shorthand_name &&
                   (xr_parser_check(parser, TK_COMMA) || xr_parser_check(parser, TK_RBRACE))) {
            separator_determined = true;
            values[count] = xr_ast_variable(parser->X, shorthand_name, line);
            count++;
            continue;
        } else {
            xr_parser_error(parser, "expected ':' after key in Json literal");
            return xr_ast_literal_null(parser->X, line);
        }

        // Parse value expression
        values[count] = xr_parse_expression(parser);
        count++;

    } while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RBRACE));

    // Expect closing brace
    xr_parser_consume(parser, TK_RBRACE, "expected '}' at end of literal");

    // `{ ... }` always produces a Json/Object literal under task 082;
    // the legacy `is_map` branch is unreachable but the local is kept to
    // minimise diff churn against historical compiles.
    (void) is_map;
    AstNode *result =
        xr_ast_object_literal(parser->X, keys, values, has_computed ? computed : NULL, count, line);

    // Free temporary array

    return result;
}

/*
 * Parse Map literal (new unified syntax)
 * Syntax: #{} or #{"key" => value, ...}
 */
AstNode *xr_parse_empty_map_literal(Parser *parser) {
    int line = parser->previous.line;

    // '#{' already consumed

    // Empty Map: #{}
    if (xr_parser_match(parser, TK_RBRACE)) {
        return xr_ast_map_literal(parser->X, NULL, NULL, 0, line);
    }

    // Non-empty Map: #{"key" => value, ...}
    AstNode **keys = NULL;
    AstNode **values = NULL;
    int count = 0;
    int capacity = 0;

    do {
        // Expand capacity
        if (count >= capacity) {
            int _old_cap_capacity = (int) capacity;
            capacity = _old_cap_capacity == 0 ? 4 : _old_cap_capacity * 2;
            AstNode **_new_keys =
                (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), (size_t) capacity);
            if (_old_cap_capacity > 0 && keys)
                memcpy(_new_keys, keys, sizeof(AstNode *) * (size_t) _old_cap_capacity);
            keys = _new_keys;

            AstNode **_new_values =
                (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), (size_t) capacity);
            if (_old_cap_capacity > 0 && values)
                memcpy(_new_values, values, sizeof(AstNode *) * (size_t) _old_cap_capacity);
            values = _new_values;
        }

        // Parse key expression
        keys[count] = xr_parse_expression(parser);

        // Expect ':' as the key-value separator inside `#{ ... }` Map literal.
        // (Old `=>` separator was removed; the `#` prefix already disambiguates
        // a Map from a Json/Object literal — see task 082.)
        xr_parser_consume(parser, TK_COLON, "expected ':' after Map key in #{...}");

        // Parse value expression
        values[count] = xr_parse_expression(parser);
        count++;

    } while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RBRACE));

    // Expect '}'
    xr_parser_consume(parser, TK_RBRACE, "expected '}' at end of Map literal");

    // Create Map literal node
    return xr_ast_map_literal(parser->X, keys, values, count, line);
}

/*
 * Parse new Set literal #[]
 * Syntax: #[] or #[element, ...]
 */
AstNode *xr_parse_set_literal_new(Parser *parser) {
    int line = parser->previous.line;

    // '#[' already consumed

    // Empty Set: #[]
    if (xr_parser_match(parser, TK_RBRACKET)) {
        // Create empty Set literal
        return xr_ast_set_literal(parser->X, NULL, 0, line);
    }

    // Collect elements
    AstNode **elements = NULL;
    int count = 0;
    int capacity = 0;

    do {
        // Expand capacity
        if (count >= capacity) {
            int _old_cap_capacity = (int) capacity;
            capacity = _old_cap_capacity == 0 ? 4 : _old_cap_capacity * 2;
            AstNode **_new_elements =
                (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), (size_t) capacity);
            if (_old_cap_capacity > 0 && elements)
                memcpy(_new_elements, elements, sizeof(AstNode *) * (size_t) _old_cap_capacity);
            elements = _new_elements;
        }

        // Parse element expression
        elements[count++] = xr_parse_expression(parser);

    } while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RBRACKET));

    // Expect ']'
    xr_parser_consume(parser, TK_RBRACKET, "expected ']' at end of Set literal");

    // Create Set literal node
    return xr_ast_set_literal(parser->X, elements, count, line);
}

/*
 * Parse index access or slice expression (infix)
 * Index access: arr[0]
 * Slice syntax: arr[start:end], arr[:end], arr[start:], arr[:]
 */
AstNode *xr_parse_index_access(Parser *parser, AstNode *array) {
    XR_DCHECK(parser != NULL, "parse_index_access: NULL parser");
    int line = parser->previous.line;

    // '[' already consumed

    AstNode *start = NULL;
    AstNode *end = NULL;
    bool is_slice = false;

    // Check if [:...] form (omitted start index)
    if (parser->current.type == TK_COLON) {
        is_slice = true;
        xr_parser_advance(parser);  // Consume ':'

        // Check if there's an end index
        if (parser->current.type != TK_RBRACKET) {
            end = xr_parse_expression(parser);
        }
    } else {
        // Parse start index or regular index
        start = xr_parse_expression(parser);

        // Check if slice syntax
        if (parser->current.type == TK_COLON) {
            is_slice = true;
            xr_parser_advance(parser);  // Consume ':'

            // Check if there's an end index
            if (parser->current.type != TK_RBRACKET) {
                end = xr_parse_expression(parser);
            }
        }
    }

    // Expect ']'
    xr_parser_consume(parser, TK_RBRACKET, "expected ']' after index or slice");

    if (is_slice) {
        // Create slice expression node
        return xr_ast_slice_expr(parser->X, array, start, end, line);
    } else {
        // Create index access node
        return xr_ast_index_get(parser->X, array, start, line);
    }
}

/*
 * Parse member access (infix)
 * arr.length, arr.push
 *
 * Note: Keywords are allowed as member names in member access (e.g. .get, .set, .new)
 */
AstNode *xr_parse_member_access(Parser *parser, AstNode *object) {
    XR_DCHECK(parser != NULL, "parse_member_access: NULL parser");
    int line = parser->previous.line;

    // '.' already consumed

    // In member access, all keywords are allowed as member names (no restrictions)
    // This supports JSON-style key names like j.float, j.int, j.class
    const char *name = NULL;
    int name_len = 0;

    XrTokenType t = parser->current.type;

    // Accept identifier, any keyword (keyword range TK_AND..TK_UNKNOWN),
    // or an integer literal for tuple field access (`tuple.0`, `tuple.1`).
    // The integer text is stored verbatim as the member name; the analyzer
    // recognises digit-only names on tuple-typed receivers.
    if (t == TK_NAME || t == TK_LITERAL_INT || (t >= TK_AND && t <= TK_UNKNOWN)) {
        xr_parser_advance(parser);
        name = parser->previous.start;
        name_len = parser->previous.length;
    } else {
        xr_parser_error(parser, "expected member name");
        return NULL;
    }

    // Copy member name (xr_ast_member_access deep-copies, so release our copy)
    char *member_name = (char *) ast_alloc(parser->X, (size_t) name_len + 1);
    strncpy(member_name, name, name_len);
    member_name[name_len] = '\0';

    AstNode *node = xr_ast_member_access(parser->X, object, member_name, line);
    return node;
}

/*
 * Parse return statement. Multi-value returns are no longer
 * supported: a function returns at most one value, and that value
 * may be a tuple expression (`return (a, b)`) when multiple results
 * are needed.
 */
AstNode *xr_parse_return_statement(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_return_statement: NULL parser");
    int line = parser->previous.line;
    xr_parser_advance(parser);  // Consume 'return'

    // Bare `return` or `return` followed by block-close: void return.
    if (parser->current.type == TK_RBRACE ||  // Block end
        parser->current.type == TK_EOF) {     // File end
        return xr_ast_return_stmt(parser->X, NULL, 0, line);
    }

    AstNode *value = xr_parse_expression(parser);

    // Comma after the return expression is the obsolete multi-value
    // form. Redirect users to the tuple equivalent.
    if (xr_parser_check(parser, TK_COMMA)) {
        xr_parser_error(parser, "[E0801] multi-value return is not supported; "
                                "use a tuple: `return (a, b)`");
        return NULL;
    }

    AstNode **values = (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), 1);
    values[0] = value;
    return xr_ast_return_stmt(parser->X, values, 1, line);
}

/*
 * Parse type alias declaration
 * type Point = { x: float, y: float }
 * type BinaryOp = (int, int) => int
 * type Points = Array<Point>
 *
 * Type aliases are registered in parser's type alias table for parse-time resolution.
 * Also generates AST_TYPE_ALIAS node so analyzer/LSP can see the declaration.
 */
AstNode *xr_parse_type_alias_declaration(Parser *parser) {
    int line = parser->previous.line;

    // Parse alias name
    xr_parser_consume(parser, TK_NAME, "expected type name after 'type'");
    char *alias_name = xr_strndup(parser->previous.start, parser->previous.length);

    // Expect '='
    xr_parser_consume(parser, TK_ASSIGN, "expected '=' in type alias definition");

    // Pre-register with NULL to block recursive self-reference (type A = A).
    // We retain the entry pointer so we can patch its `type` field below
    // once the RHS is fully parsed.
    XrTypeAlias *alias_entry = xr_type_scope_define(parser->type_scope, alias_name, NULL);
    if (!alias_entry) {
        xr_parser_error(parser, "duplicate type alias definition");
        xr_free(alias_name);
        return NULL;
    }

    // Parse type definition (self-reference will resolve to NULL → named type fallback)
    XrTypeRef *type_definition = xr_parse_type_annotation(parser);
    if (!type_definition) {
        xr_parser_error(parser, "invalid type definition");
        xr_free(alias_name);
        return NULL;
    }

    // Patch the placeholder with the actual type definition.
    alias_entry->type_ref = type_definition;
    if (type_definition->kind == XR_TREF_OBJECT) {
        char *type_name = (char *) ast_alloc(parser->X, strlen(alias_name) + 1);
        strcpy(type_name, alias_name);
        type_definition->name = type_name;
    }

    // Create AST node so analyzer/LSP can see the declaration.
    // Stash the resolved type in TypeAliasNode::resolved_type so that
    // the analyzer can read it without going through any backchannel
    // on AstNode itself.
    AstNode *node = xr_ast_type_alias(parser->X, alias_name, NULL, NULL, NULL, 0, line);
    xr_free(alias_name);  // xr_ast_type_alias copies the name, so we can free the original
    if (!node) {
        return NULL;
    }
    node->as.type_alias.resolved_type = type_definition;

    return node;
}

/*
 * Parse declaration (variable declaration, constant declaration, class declaration or statement)
 */
AstNode *xr_parse_declaration(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_declaration: NULL parser");
    // Module system
    if (xr_parser_match(parser, TK_IMPORT)) {
        return xr_parse_import_declaration(parser);
    }

    if (xr_parser_match(parser, TK_EXPORT)) {
        return xr_parse_export_declaration(parser);
    }

    // Class declaration (with optional 'final' prefix)
    if (xr_parser_match(parser, TK_FINAL)) {
        if (!xr_parser_match(parser, TK_CLASS)) {
            xr_parser_error_at_current(parser,
                                       "'final' can only be used before 'class' at top level");
            return NULL;
        }
        AstNode *cls = xr_parse_class_declaration(parser);
        if (cls)
            cls->as.class_decl.is_final = true;
        return cls;
    }

    if (xr_parser_match(parser, TK_CLASS)) {
        return xr_parse_class_declaration(parser);
    }

    // Struct declaration
    if (xr_parser_match(parser, TK_STRUCT)) {
        return xr_parse_struct_declaration(parser);
    }

    // Interface declaration
    if (xr_parser_match(parser, TK_INTERFACE)) {
        return xr_parse_interface_declaration(parser);
    }

    // Enum declaration
    if (xr_parser_match(parser, TK_ENUM)) {
        return xr_parse_enum_declaration(parser);
    }

    // Type alias declaration: type Point = { x: float, y: float }
    if (xr_parser_match(parser, TK_TYPE_ALIAS)) {
        return xr_parse_type_alias_declaration(parser);
    }

    // ========== Coroutine syntax ==========

    // defer statement: defer fn() or defer { block }
    if (xr_parser_match(parser, TK_DEFER)) {
        return xr_parse_defer_statement(parser);
    }

    // select statement: select { ... }
    if (xr_parser_match(parser, TK_SELECT)) {
        return xr_parse_select_statement(parser);
    }

    // scope block: scope { ... }
    if (xr_parser_match(parser, TK_SCOPE)) {
        return xr_parse_scope_block(parser);
    }

    // yield statement: cooperatively yield execution to the scheduler
    if (xr_parser_match(parser, TK_YIELD)) {
        int line = parser->previous.line;
        return xr_ast_yield_stmt(parser->X, line);
    }

    // Attributed declaration: @test fn ..., @native class ..., etc.
    if (xr_parser_check(parser, TK_AT)) {
        return xr_parse_attributed_declaration(parser);
    }

    // Function declaration: only fn keyword supported
    if (xr_parser_match(parser, TK_FN)) {
        return xr_parse_function_declaration(parser);
    }

    // Context keywords: linked/supervisor before go/scope
    // These are identifiers that act as modifiers only when followed by go or scope.
    // Note: "monitored" prefix was removed — use task.monitor() API instead.
    if (xr_parser_check(parser, TK_NAME)) {
        Token name_token = parser->current;

        // "linked" go ... | "linked" scope ...
        if (name_token.length == 6 && memcmp(name_token.start, "linked", 6) == 0) {
            Scanner saved = parser->scanner;
            Token peek = xr_scanner_scan(&saved);
            if (peek.type == TK_GO) {
                xr_parser_advance(parser);  // consume "linked"
                xr_parser_advance(parser);  // consume "go"
                return xr_parse_go_expr_with_link(parser, XR_LINK_LINKED);
            }
            if (peek.type == TK_SCOPE) {
                xr_parser_advance(parser);  // consume "linked"
                xr_parser_advance(parser);  // consume "scope"
                return xr_parse_scope_block_with_mode(parser, XR_SCOPE_LINKED);
            }
        }

        // "supervisor" scope ...
        if (name_token.length == 10 && memcmp(name_token.start, "supervisor", 10) == 0) {
            Scanner saved = parser->scanner;
            Token peek = xr_scanner_scan(&saved);
            if (peek.type == TK_SCOPE) {
                xr_parser_advance(parser);  // consume "supervisor"
                xr_parser_advance(parser);  // consume "scope"
                return xr_parse_scope_block_with_mode(parser, XR_SCOPE_SUPERVISOR);
            }
        }
    }

    // Detect unsupported 'from ... import' syntax
    if (xr_parser_check_name(parser, "from")) {
        xr_parser_error_at_current(parser, "Python-style 'from ... import' is not supported. "
                                           "Use 'import { name } from \"module\"' in Xray");
        return NULL;
    }

    // Detect common wrong keywords from other languages.
    // Without this, ASI causes misleading "semicolon" errors instead of
    // reporting the actual problem (unknown keyword).
    if (xr_parser_check(parser, TK_NAME)) {
        Token name_token = parser->current;
        // Detect walrus-style short declaration: x := 1
        {
            Parser checkpoint = *parser;
            xr_parser_advance(parser);
            if (xr_parser_check(parser, TK_COLON)) {
                xr_parser_advance(parser);
                if (xr_parser_check(parser, TK_ASSIGN)) {
                    *parser = checkpoint;
                    char buf[128];
                    snprintf(
                        buf, sizeof(buf),
                        "':=' is not supported. Use 'let %.*s = ...' to declare variables in Xray",
                        name_token.length, name_token.start);
                    xr_parser_error_at_current(parser, buf);
                    return NULL;
                }
            }
            *parser = checkpoint;
        }
        if (name_token.length == 8 && memcmp(name_token.start, "function", 8) == 0) {
            xr_parser_error_at_current(
                parser, "unknown keyword 'function'. Use 'fn' to define functions in Xray");
            return NULL;
        }
        if (name_token.length == 3 && memcmp(name_token.start, "var", 3) == 0) {
            xr_parser_error_at_current(
                parser, "unknown keyword 'var'. Use 'let' to declare variables in Xray");
            return NULL;
        }
        if (name_token.length == 3 && memcmp(name_token.start, "def", 3) == 0) {
            xr_parser_error_at_current(
                parser, "unknown keyword 'def'. Use 'fn' to define functions in Xray");
            return NULL;
        }
        if (name_token.length == 4 && memcmp(name_token.start, "func", 4) == 0) {
            xr_parser_error_at_current(
                parser, "unknown keyword 'func'. Use 'fn' to define functions in Xray");
            return NULL;
        }
        if (name_token.length == 4 && memcmp(name_token.start, "elif", 4) == 0) {
            xr_parser_error_at_current(parser, "unknown keyword 'elif'. Use 'else if' in Xray");
            return NULL;
        }
        if (name_token.length == 6 && memcmp(name_token.start, "switch", 6) == 0) {
            xr_parser_error_at_current(
                parser, "unknown keyword 'switch'. Use 'match' for pattern matching in Xray");
            return NULL;
        }
        if (name_token.length == 7 && memcmp(name_token.start, "foreach", 7) == 0) {
            xr_parser_error_at_current(
                parser, "unknown keyword 'foreach'. Use 'for (x in collection) { }' in Xray");
            return NULL;
        }
        if (name_token.length == 2 && memcmp(name_token.start, "do", 2) == 0) {
            xr_parser_error_at_current(
                parser, "'do...while' is not supported. Use 'while (condition) { }' in Xray");
            return NULL;
        }
        if (name_token.length == 6 && memcmp(name_token.start, "lambda", 6) == 0) {
            xr_parser_error_at_current(
                parser,
                "'lambda' is not supported. Use 'fn(params) { }' or '(params) => expr' in Xray");
            return NULL;
        }
    }

    // Detect type-first declarations: int x = 5, string name = "hello"
    // Type keyword followed by identifier means user intended a declaration
    {
        XrTokenType t = parser->current.type;
        if (t == TK_INT || t == TK_INT8 || t == TK_INT16 || t == TK_INT32 || t == TK_INT64 ||
            t == TK_UINT8 || t == TK_UINT16 || t == TK_UINT32 || t == TK_UINT64 || t == TK_FLOAT ||
            t == TK_FLOAT32 || t == TK_FLOAT64 || t == TK_STRING || t == TK_BOOL) {
            // Peek ahead: if next token is TK_NAME, this is a C-style declaration
            Token saved = parser->current;
            Parser checkpoint = *parser;
            xr_parser_advance(parser);
            if (xr_parser_check(parser, TK_NAME)) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "C-style declaration not supported. Use 'let %.*s: %.*s = ...' in Xray",
                         parser->current.length, parser->current.start, saved.length, saved.start);
                *parser = checkpoint;
                xr_parser_error_at_current(parser, buf);
                return NULL;
            }
            *parser = checkpoint;
        }
    }

    /* Variable declaration
     *
     * Syntax design:
     * 1. let a, b, c          - No initialization, all null
     * 2. let a, b = 1, 2      - Multi-value declaration, counts must match
     * 3. let a, b = foo()     - Multi-value declaration, receive function multi-return
     * 4. let a = 1            - Single variable declaration
     * 5. let a=1, b=2         - Forbidden! Must write on separate lines
     */
    if (xr_parser_match(parser, TK_LET)) {
        // Check if destructure declaration
        if (xr_parser_check(parser, TK_LBRACKET) || xr_parser_check(parser, TK_LBRACE) ||
            xr_parser_check(parser, TK_LPAREN)) {
            return xr_parse_destructure_declaration(parser, false);
        }

        if (xr_parser_check(parser, TK_NAME)) {
            int saved_line = parser->current.line;
            int saved_column = parser->current.column;

            // Copy first identifier name
            char *first_name = (char *) ast_alloc(parser->X, (size_t) parser->current.length + 1);
            memcpy(first_name, parser->current.start, parser->current.length);
            first_name[parser->current.length] = '\0';
            xr_parser_advance(parser);

            // `let a, b = ...` (bare multi-variable) is the obsolete
            // multi-value form. Tuple destructure `let (a, b) = ...`
            // is the canonical replacement.
            if (xr_parser_check(parser, TK_COMMA)) {
                xr_parser_error(parser, "multi-variable declaration is not supported; "
                                        "use tuple destructure: `let (a, b) = ...`");
                return NULL;
            }
            {
                // Single variable declaration: let a or let a = expr or let a: Type = expr
                // Single variable declaration: let a or let a = expr or let a: Type = expr
                XrType *var_type = NULL;
                if (xr_parser_match(parser, TK_COLON)) {
                    var_type = xr_parse_type_annotation(parser);
                }

                // Optional initialization expression
                AstNode *initializer = NULL;
                if (xr_parser_match(parser, TK_ASSIGN)) {
                    initializer = xr_parse_expression(parser);
                }

                AstNode *decl =
                    xr_ast_var_decl(parser->X, first_name, initializer, false, saved_line);
                decl->column = saved_column;
                if (initializer && initializer->end_line > 0) {
                    decl->end_line = initializer->end_line;
                    decl->end_column = initializer->end_column;
                } else {
                    decl->end_line = saved_line;
                    decl->end_column = saved_column + (int) strlen(first_name);
                }
                decl->as.var_decl.type_annotation = var_type;  // Set type annotation
                return decl;
            }
        }

        xr_parser_error_expected_name(parser, "expected variable name");
        return NULL;
    }

    // Constant declaration (supports comma separation)
    if (xr_parser_match(parser, TK_CONST)) {
        // Check if destructure declaration
        if (xr_parser_check(parser, TK_LBRACKET) || xr_parser_check(parser, TK_LBRACE) ||
            xr_parser_check(parser, TK_LPAREN)) {
            return xr_parse_destructure_declaration(parser, true);
        }

        AstNode *first_decl = xr_parse_single_var_declaration(parser, 1);  // 1 means constant

        // Check if there are comma-separated following declarations
        if (!xr_parser_check(parser, TK_COMMA)) {
            return first_decl;
        }

        // Multiple declarations, create sequence node
        AstNode **declarations =
            (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), (size_t) 16);
        int decl_count = 0;
        int decl_capacity = 16;
        declarations[decl_count++] = first_decl;

        while (xr_parser_match(parser, TK_COMMA)) {
            XR_PARSE_PUSH(parser, declarations, decl_count, decl_capacity,
                          xr_parse_single_var_declaration(parser, 1));
        }

        // Create sequence node
        AstNode *seq = xr_ast_program(parser->X);
        for (int i = 0; i < decl_count; i++) {
            xr_ast_program_add(parser->X, seq, declarations[i]);
        }
        return seq;
    }

    /* shared variable declaration (stored in global heap, pass by reference)
     * Syntax: shared let x = value or shared const x = value
     *
     * shared const: can be read concurrently by coroutine closures
     * shared let: can only be accessed serially via Channel
     */
    if (xr_parser_match(parser, TK_SHARED)) {
        bool is_const = false;
        if (xr_parser_match(parser, TK_CONST)) {
            is_const = true;
        } else if (!xr_parser_match(parser, TK_LET)) {
            xr_parser_error(parser, "expected 'let' or 'const' after 'shared'");
            return NULL;
        }

        // Parse variable name
        xr_parser_consume(parser, TK_NAME, "expected variable name");
        char *name = (char *) ast_alloc(parser->X, (size_t) parser->previous.length + 1);
        memcpy(name, parser->previous.start, parser->previous.length);
        name[parser->previous.length] = '\0';
        int line = parser->previous.line;
        int column = parser->previous.column;
        int name_length = parser->previous.length;

        // Parse optional type annotation (: Type)
        XrType *type_annotation = NULL;
        if (xr_parser_match(parser, TK_COLON)) {
            type_annotation = xr_parse_type_annotation(parser);
        }

        // shared must have initialization expression
        xr_parser_consume(parser, TK_ASSIGN, "shared variable must have initializer");
        AstNode *initializer = xr_parse_precedence(parser, PREC_TERNARY);

        AstNode *decl = xr_ast_var_decl_with_mode(parser->X, name, initializer, is_const,
                                                  XR_STORAGE_SHARED, line);
        decl->column = column;
        if (initializer && initializer->end_line > 0) {
            decl->end_line = initializer->end_line;
            decl->end_column = initializer->end_column;
        } else {
            decl->end_line = line;
            decl->end_column = column + name_length;
        }
        decl->as.var_decl.type_annotation = type_annotation;
        return decl;
    }

    // Code block
    if (xr_parser_match(parser, TK_LBRACE)) {
        return xr_parse_block(parser);
    }

    // Otherwise parse as statement
    return xr_parse_statement(parser);
}

// ========== Exception handling parse functions ==========

/*
 * Parse try-catch-finally statement
 * try { ... } catch (e) { ... } finally { ... }
 */
AstNode *xr_parse_try_statement(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_try_statement: NULL parser");
    int line = parser->current.line;

    // Consume 'try'
    xr_parser_advance(parser);

    // Parse try block
    xr_parser_consume(parser, TK_LBRACE, "expected '{' after try");
    AstNode *try_body = xr_parse_block(parser);

    // Optional catch block
    const char *catch_var = NULL;
    int catch_var_line = 0;
    int catch_var_column = 0;
    AstNode *catch_body = NULL;
    if (xr_parser_match(parser, TK_CATCH)) {
        // Parse catch variable
        xr_parser_consume(parser, TK_LPAREN, "expected '(' after catch");

        if (parser->current.type != TK_NAME) {
            xr_parser_error_expected_name(parser, "expected exception variable name");
            return NULL;
        }

        // Save variable name and position
        char *var_name = (char *) ast_alloc(parser->X, (size_t) parser->current.length + 1);
        memcpy(var_name, parser->current.start, parser->current.length);
        var_name[parser->current.length] = '\0';
        catch_var = var_name;
        catch_var_line = parser->current.line;
        catch_var_column = parser->current.column;

        xr_parser_advance(parser);  // Consume variable name
        xr_parser_consume(parser, TK_RPAREN, "expected ')' after exception variable");

        // Parse catch block
        xr_parser_consume(parser, TK_LBRACE, "expected '{' after catch");
        catch_body = xr_parse_block(parser);
    }

    // Optional finally block
    AstNode *finally_body = NULL;
    if (xr_parser_match(parser, TK_FINALLY)) {
        xr_parser_consume(parser, TK_LBRACE, "expected '{' after finally");
        finally_body = xr_parse_block(parser);
    }

    // Need at least catch or finally
    if (!catch_body && !finally_body) {
        xr_parser_error(parser, "try statement requires catch or finally block");
        return NULL;
    }

    // xr_ast_try_catch deep-copies catch_var via ast_strdup.
    AstNode *node = xr_ast_try_catch(parser->X, try_body, catch_var, catch_var_line,
                                     catch_var_column, catch_body, finally_body, line);
    // Span ends at the last block present (finally > catch > try).
    AstNode *last_block = finally_body ? finally_body : (catch_body ? catch_body : try_body);
    if (last_block) {
        node->end_line = last_block->end_line;
        node->end_column = last_block->end_column;
    }
    return node;
}

/*
 * Parse throw statement
 * throw expr
 */
AstNode *xr_parse_throw_statement(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_throw_statement: NULL parser");
    int line = parser->current.line;

    // Consume 'throw'
    xr_parser_advance(parser);

    // Parse expression to throw
    AstNode *expression = xr_parse_expression(parser);

    if (!expression) {
        xr_parser_error(parser, "throw statement requires an expression");
        return NULL;
    }

    return xr_ast_throw_stmt(parser->X, expression, line);
}

// ========== Destructuring assignment helpers ==========

/*
 * Convert array literal to destructure pattern (for destructuring assignment)
 * [a, b, c] -> destructure pattern
 * Can only convert when all elements are variable references
 */
XrDestructurePattern *convert_array_literal_to_pattern(XrayIsolate *X, AstNode *array_literal) {
    if (array_literal->type != AST_ARRAY_LITERAL) {
        return NULL;
    }

    int count = array_literal->as.array_literal.count;
    XrDestructurePattern **elements = (XrDestructurePattern **) ast_alloc_array(
        X, sizeof(XrDestructurePattern *), (size_t) count);

    for (int i = 0; i < count; i++) {
        AstNode *element = array_literal->as.array_literal.elements[i];

        // Check if element is variable reference
        if (element->type == AST_VARIABLE) {
            // Create identifier pattern
            elements[i] = xr_pattern_identifier(X, element->as.variable.name, NULL);
        } else {
            // Not a variable reference, cannot convert to destructure pattern
            return NULL;
        }
    }

    return xr_pattern_array(X, elements, count);
}

/*
 * Convert tuple literal to destructure pattern (for destructuring assignment)
 * (a, b, c) -> tuple destructure pattern
 * Mirrors convert_array_literal_to_pattern; only succeeds when every element
 * is a bare variable reference, since assignment targets cannot evaluate
 * sub-expressions.
 */
XrDestructurePattern *convert_tuple_literal_to_pattern(XrayIsolate *X, AstNode *tuple_literal) {
    if (tuple_literal->type != AST_TUPLE_LITERAL) {
        return NULL;
    }

    int count = tuple_literal->as.tuple_literal.count;
    XrDestructurePattern **elements = (XrDestructurePattern **) ast_alloc_array(
        X, sizeof(XrDestructurePattern *), (size_t) count);

    for (int i = 0; i < count; i++) {
        AstNode *element = tuple_literal->as.tuple_literal.elements[i];
        if (element->type == AST_VARIABLE) {
            elements[i] = xr_pattern_identifier(X, element->as.variable.name, NULL);
        } else {
            return NULL;
        }
    }

    return xr_pattern_tuple(X, elements, count);
}

/*
 * Convert object literal to destructure pattern (for destructuring assignment)
 * {a, b, c} -> destructure pattern
 * Can only convert when all field keys and values are variable references and key name equals
 * variable name
 */
XrDestructurePattern *convert_object_literal_to_pattern(XrayIsolate *X, AstNode *object_literal) {
    if (object_literal->type != AST_OBJECT_LITERAL) {
        return NULL;
    }

    int count = object_literal->as.object_literal.count;
    char **field_names = (char **) ast_alloc_array(X, sizeof(char *), (size_t) count);
    XrDestructurePattern **patterns = (XrDestructurePattern **) ast_alloc_array(
        X, sizeof(XrDestructurePattern *), (size_t) count);

    for (int i = 0; i < count; i++) {
        AstNode *key_node = object_literal->as.object_literal.keys[i];
        AstNode *value_node = object_literal->as.object_literal.values[i];

        // Check if key is string literal or variable reference
        char *field_name = NULL;
        if (key_node->type == AST_LITERAL_STRING) {
            // Key is string literal
            field_name = ast_strdup(X, key_node->as.literal.raw_value.string_val);
        } else if (key_node->type == AST_VARIABLE) {
            // Key is variable reference (shorthand syntax: {x, y})
            field_name = ast_strdup(X, key_node->as.variable.name);
        } else {
            // Key is not string or variable, cannot convert
            return NULL;
        }

        // Check if value is variable reference
        if (value_node->type == AST_VARIABLE) {
            // For shorthand syntax {x, y}, key and value should be same variable
            if (key_node->type == AST_VARIABLE &&
                strcmp(key_node->as.variable.name, value_node->as.variable.name) != 0) {
                // Key and value don't match, not shorthand syntax
                return NULL;
            }

            field_names[i] = field_name;
            // Create identifier pattern
            patterns[i] = xr_pattern_identifier(X, value_node->as.variable.name, NULL);
        } else {
            // Value is not variable reference, cannot convert to destructure pattern
            return NULL;
        }
    }

    return xr_pattern_object(X, field_names, patterns, count, true);
}

// Destructuring declaration/pattern parsing moved to xparse_destructure.c
