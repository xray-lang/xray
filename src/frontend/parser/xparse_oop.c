/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_oop.c - OOP syntax parsing (class, new, this, super)
 *
 * KEY CONCEPT:
 *   Parses class declarations, new expressions, and member access.
 */

#include "xparse_internal.h"
#include "../../base/xchecks.h"
#include "xast.h"
#include "../../runtime/value/xtype_names.h"
#include "../../runtime/value/xtype.h"
#include "../analyzer/xanalyzer_symbol.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Forward declarations
static AstNode *xr_parse_property_accessors(Parser *parser, const char *name,
                                            XrType *field_type,
                                            bool is_private, bool is_static, int line);

// Helper: copy Token text to string
static char* token_to_string(Token *token) {
    if (!token || token->length == 0) return NULL;

    char *str = (char*)xr_malloc(token->length + 1);
    memcpy(str, token->start, token->length);
    str[token->length] = '\0';
    return str;
}

/* ========== Class Declaration Parsing ========== */

// Parse class declaration
// Syntax: class Dog extends Animal { field_declarations... method_declarations... }
AstNode *xr_parse_class_declaration(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_class_declaration: NULL parser");
    int line = parser->previous.line;

    // 'class' keyword already consumed

    bool is_abstract = false;

    // Parse class name
    xr_parser_consume(parser, TK_NAME, "expected class name");
    char *class_name = token_to_string(&parser->previous);
    int name_column = parser->previous.column;

    // Parse generic type parameters <T, U: Constraint>
    XrGenericParam **type_params = NULL;
    int type_param_count = 0;
    int type_param_capacity = 0;

    if (xr_parser_match(parser, TK_LT)) {
        do {
            xr_parser_consume(parser, TK_NAME, "expected type parameter name");
            Token param_token = parser->previous;

            char *param_name = (char *)xr_malloc(param_token.length + 1);
            memcpy(param_name, param_token.start, param_token.length);
            param_name[param_token.length] = '\0';

            // Parse optional constraint <T: Interface>
            XrType *constraint = NULL;
            if (xr_parser_match(parser, TK_COLON)) {
                constraint = xr_parse_type_annotation(parser);
            }

            XrGenericParam *gp = (XrGenericParam *)xr_malloc(sizeof(XrGenericParam));
            gp->name = param_name;
            gp->constraint = constraint;
            XR_PARSE_PUSH(type_params, type_param_count, type_param_capacity, gp);

        } while (xr_parser_match(parser, TK_COMMA));

        xr_parser_consume(parser, TK_GT, "expected '>' to close generic params");
    }

    // Register generic type params in type_scope for field/method type parsing
    XaScope *saved_scope = parser->type_scope;
    if (type_param_count > 0) {
        XaScope *generic_scope = xa_scope_new(XA_SCOPE_CLASS, parser->type_scope);
        for (int i = 0; i < type_param_count; i++) {
            XrType *type_param = xr_type_new_type_param(type_params[i]->name, i);
            xa_scope_define_type_alias(generic_scope, type_params[i]->name, type_param);
        }
        parser->type_scope = generic_scope;
    }

    // Parse extends clause (optional)
    // Supports: extends Class or extends module.Class
    char *super_name = NULL;
    char *super_module = NULL;
    if (xr_parser_match(parser, TK_EXTENDS)) {
        xr_parser_consume(parser, TK_NAME, "expected superclass name");
        char *first_name = token_to_string(&parser->previous);

        // Check for module.Class form
        if (xr_parser_match(parser, TK_DOT)) {
            xr_parser_consume(parser, TK_NAME, "expected class name after '.'");
            super_module = first_name;
            super_name = token_to_string(&parser->previous);
        } else {
            super_name = first_name;
        }
    }


    char **interfaces = NULL;
    int interface_count = 0;
    int interface_capacity = 0;

    if (xr_parser_match(parser, TK_IMPLEMENTS)) {
        do {
            xr_parser_consume(parser, TK_NAME, "expected interface name");
            char *interface_name = token_to_string(&parser->previous);

            XR_PARSE_PUSH(interfaces, interface_count, interface_capacity, interface_name);

        } while (xr_parser_match(parser, TK_COMMA));
    }

    // Detect colon-style inheritance: class Dog : Animal
    if (xr_parser_check(parser, TK_COLON)) {
        xr_parser_error_at_current(parser,
            "use 'extends' instead of ':' for class inheritance, e.g. class Dog extends Animal");
        return NULL;
    }

    // Parse class body
    xr_parser_consume(parser, TK_LBRACE, "expected '{' to start class body");

    // Collect field and method declarations
    AstNode **fields = NULL;
    int field_count = 0;
    int field_capacity = 0;

    AstNode **methods = NULL;
    int method_count = 0;
    int method_capacity = 0;

    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        // Error recovery: skip to next valid token
        if (parser->panic_mode) {
            xr_parser_synchronize(parser);
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_EOF)) break;
            continue;
        }

        // Friendly hint: check common errors
        if (xr_parser_check(parser, TK_LET)) {
            xr_parser_error_at_current(parser,
                "'let' keyword not needed for field declarations in class body, write field name directly, e.g.: name: string\n"
                "'fn' keyword not needed for method definitions either, write method name directly, e.g.: greet() { ... }");
            xr_parser_advance(parser);
            continue;
        }

        if (xr_parser_check(parser, TK_FN)) {
            xr_parser_error_at_current(parser,
                "'fn' keyword not needed for method definitions in class body, write method name directly, e.g.: greet() { ... }");
            xr_parser_advance(parser);
            continue;
        }

        // Skip optional semicolons (Xray supports optional semicolons)
        if (xr_parser_check(parser, TK_SEMICOLON)) {
            xr_parser_advance(parser);
            continue;
        }

        // Skip unknown tokens to avoid infinite loop
        if (!xr_parser_check(parser, TK_NAME) && !xr_parser_check(parser, TK_PRIVATE) && !xr_parser_check(parser, TK_PUBLIC) &&
            !xr_parser_check(parser, TK_STATIC) && !xr_parser_check(parser, TK_CONSTRUCTOR) && !xr_parser_check(parser, TK_ABSTRACT) &&
            !xr_parser_check(parser, TK_OVERRIDE) && !xr_parser_check(parser, TK_FINAL) && !xr_parser_check(parser, TK_OPERATOR)) {
            xr_parser_error_expected_name(parser, "expected field or method name");
            xr_parser_advance(parser);
            continue;
        }

        // Determine if this is a method or field
        bool is_method = false;
        AstNode *member = xr_parse_field_declaration(parser, &is_method);

        if (is_method) {
            XR_PARSE_PUSH(methods, method_count, method_capacity, member);

            // Check for paired setter (property accessor block case)
            if (member->type == AST_METHOD_DECL &&
                member->as.method_decl.base_arg_count == -2) {
                // Has paired setter, extract from temporary storage and add
                AstNode *setter = (AstNode*)member->as.method_decl.base_args;
                member->as.method_decl.base_arg_count = 0;  // restore normal value
                member->as.method_decl.base_args = NULL;
                setter->as.method_decl.base_arg_count = 0;  // restore normal value

                XR_PARSE_PUSH(methods, method_count, method_capacity, setter);
            }
        } else {
            XR_PARSE_PUSH(fields, field_count, field_capacity, member);
        }
    }

    xr_parser_consume(parser, TK_RBRACE, "expected '}' to end class body");

    // Restore type_scope after parsing class body
    if (type_param_count > 0) {
        parser->type_scope = saved_scope;
    }

    // Create class declaration AST node
    AstNode *class_node = xr_ast_class_decl(parser->X, class_name, super_name,
                                             fields, field_count,
                                             methods, method_count, line);
    class_node->column = name_column;

    // Set superclass module (supports extends module.Class syntax)
    class_node->as.class_decl.super_module = super_module;
    class_node->as.class_decl.interfaces = interfaces;
    class_node->as.class_decl.interface_count = interface_count;
    class_node->as.class_decl.is_abstract = is_abstract;

    // Set generic type parameters
    class_node->as.class_decl.type_params = type_params;
    class_node->as.class_decl.type_param_count = type_param_count;

    return class_node;
}

/* ========== Struct Declaration Parsing ========== */

/*
 * Parse struct declaration (value type with copy semantics)
 *
 * Syntax: struct Point { x: float, y: float }
 *
 * Restrictions (enforced at parse time):
 *   - No inheritance (extends)
 *   - No interface implementation
 *   - No abstract/override modifiers
 *   - No constructor keyword (auto-generated)
 *   - Fields must have type annotations
 */
AstNode *xr_parse_struct_declaration(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_struct_declaration: NULL parser");
    int line = parser->previous.line;  // 'struct' already consumed

    // Parse struct name
    xr_parser_consume(parser, TK_NAME, "expected struct name");
    char *struct_name = token_to_string(&parser->previous);
    int name_column = parser->previous.column;

    // Parse generic type parameters <T, U: Constraint>
    XrGenericParam **type_params = NULL;
    int type_param_count = 0;
    int type_param_capacity = 0;

    if (xr_parser_match(parser, TK_LT)) {
        do {
            xr_parser_consume(parser, TK_NAME, "expected type parameter name");
            Token param_token = parser->previous;

            char *param_name = (char *)xr_malloc(param_token.length + 1);
            memcpy(param_name, param_token.start, param_token.length);
            param_name[param_token.length] = '\0';

            XrType *constraint = NULL;
            if (xr_parser_match(parser, TK_COLON)) {
                constraint = xr_parse_type_annotation(parser);
            }

            XrGenericParam *gp = (XrGenericParam *)xr_malloc(sizeof(XrGenericParam));
            gp->name = param_name;
            gp->constraint = constraint;
            XR_PARSE_PUSH(type_params, type_param_count, type_param_capacity, gp);

        } while (xr_parser_match(parser, TK_COMMA));

        xr_parser_consume(parser, TK_GT, "expected '>' to close generic params");
    }

    // Register generic type params in type_scope for field/method type parsing
    XaScope *saved_scope = parser->type_scope;
    if (type_param_count > 0) {
        XaScope *generic_scope = xa_scope_new(XA_SCOPE_CLASS, parser->type_scope);
        for (int i = 0; i < type_param_count; i++) {
            XrType *tp = xr_type_new_type_param(type_params[i]->name, i);
            xa_scope_define_type_alias(generic_scope, type_params[i]->name, tp);
        }
        parser->type_scope = generic_scope;
    }

    // Structs do not support extends (no inheritance)
    if (xr_parser_check(parser, TK_EXTENDS)) {
        xr_parser_error_at_current(parser, "structs cannot inherit from other types");
    }

    // Parse implements clause (structs can implement interfaces)
    char **interfaces = NULL;
    int interface_count = 0;
    int interface_capacity = 0;

    if (xr_parser_match(parser, TK_IMPLEMENTS)) {
        do {
            xr_parser_consume(parser, TK_NAME, "expected interface name");
            char *interface_name = token_to_string(&parser->previous);

            XR_PARSE_PUSH(interfaces, interface_count, interface_capacity, interface_name);

        } while (xr_parser_match(parser, TK_COMMA));
    }

    // Parse struct body
    xr_parser_consume(parser, TK_LBRACE, "expected '{' to start struct body");

    AstNode **fields = NULL;
    int field_count = 0;
    int field_capacity = 0;

    AstNode **methods = NULL;
    int method_count = 0;
    int method_capacity = 0;

    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        if (parser->panic_mode) {
            xr_parser_synchronize(parser);
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_EOF)) break;
            continue;
        }

        // Skip optional semicolons
        if (xr_parser_check(parser, TK_SEMICOLON)) {
            xr_parser_advance(parser);
            continue;
        }

        // Reject invalid modifiers for structs
        if (xr_parser_check(parser, TK_ABSTRACT)) {
            xr_parser_error_at_current(parser, "'abstract' is not allowed in struct declarations");
            xr_parser_advance(parser);
            continue;
        }
        if (xr_parser_check(parser, TK_OVERRIDE)) {
            xr_parser_error_at_current(parser, "'override' is not allowed in struct declarations");
            xr_parser_advance(parser);
            continue;
        }
        if (xr_parser_check(parser, TK_CONSTRUCTOR)) {
            xr_parser_error_at_current(parser, "structs cannot have explicit constructors; use struct literal syntax instead");
            xr_parser_advance(parser);
            continue;
        }
        if (xr_parser_check(parser, TK_FINAL)) {
            xr_parser_error_at_current(parser, "'final' is not allowed in struct declarations");
            xr_parser_advance(parser);
            continue;
        }

        // Reject 'let' and 'fn' hints
        if (xr_parser_check(parser, TK_LET)) {
            xr_parser_error_at_current(parser,
                "'let' keyword not needed for field declarations in struct body");
            xr_parser_advance(parser);
            continue;
        }
        if (xr_parser_check(parser, TK_FN)) {
            xr_parser_error_at_current(parser,
                "'fn' keyword not needed for method definitions in struct body");
            xr_parser_advance(parser);
            continue;
        }

        // Skip unknown tokens
        if (!xr_parser_check(parser, TK_NAME) && !xr_parser_check(parser, TK_PRIVATE) && !xr_parser_check(parser, TK_PUBLIC) &&
            !xr_parser_check(parser, TK_STATIC) && !xr_parser_check(parser, TK_OPERATOR)) {
            xr_parser_error_expected_name(parser, "expected field or method name in struct");
            xr_parser_advance(parser);
            continue;
        }

        bool is_method = false;
        AstNode *member = xr_parse_field_declaration(parser, &is_method);

        if (is_method) {
            XR_PARSE_PUSH(methods, method_count, method_capacity, member);

            // Check for paired setter (property accessor block case)
            if (member->type == AST_METHOD_DECL &&
                member->as.method_decl.base_arg_count == -2) {
                AstNode *setter = (AstNode*)member->as.method_decl.base_args;
                member->as.method_decl.base_arg_count = 0;
                member->as.method_decl.base_args = NULL;
                setter->as.method_decl.base_arg_count = 0;

                XR_PARSE_PUSH(methods, method_count, method_capacity, setter);
            }
        } else {
            XR_PARSE_PUSH(fields, field_count, field_capacity, member);
        }
    }

    xr_parser_consume(parser, TK_RBRACE, "expected '}' to end struct body");

    // Restore type_scope after parsing struct body
    if (type_param_count > 0) {
        parser->type_scope = saved_scope;
    }

    // Create struct declaration AST node
    AstNode *struct_node = xr_ast_struct_decl(parser->X, struct_name,
                                              fields, field_count,
                                              methods, method_count, line);
    struct_node->column = name_column;
    struct_node->as.struct_decl.interfaces = interfaces;
    struct_node->as.struct_decl.interface_count = interface_count;
    struct_node->as.struct_decl.type_params = type_params;
    struct_node->as.struct_decl.type_param_count = type_param_count;

    return struct_node;
}

/* ========== Field Declaration Parsing ========== */

// Parse field or method declaration
// Syntax:
//   Field: name: string or private age: int = 0
//   Method: greet() { ... } or constructor(name) { ... }
// @param is_method_out output parameter, true for method, false for field
AstNode *xr_parse_field_declaration(Parser *parser, bool *is_method_out) {
    XR_DCHECK(parser != NULL, "parse_field_declaration: NULL parser");
    int line = parser->current.line;

    // Parse access modifiers (optional)
    bool is_private = false;
    bool is_static = false;
    bool is_getter = false;
    (void)is_getter;
    bool is_setter = false;
    (void)is_setter;
    bool is_abstract = false;
    bool is_override = false;
    bool is_final = false;


    if (xr_parser_match(parser, TK_OVERRIDE)) {
        is_override = true;
    }

    if (xr_parser_match(parser, TK_ABSTRACT)) {
        is_abstract = true;
    }

    if (xr_parser_match(parser, TK_PRIVATE)) {
        is_private = true;
    } else if (xr_parser_match(parser, TK_PUBLIC)) {
        is_private = false;  // explicit public
    }

    if (xr_parser_match(parser, TK_STATIC)) {
        is_static = true;

        // Check for static constructor: static constructor()
        if (xr_parser_check(parser, TK_CONSTRUCTOR)) {
            *is_method_out = true;
            xr_parser_advance(parser);  // consume 'constructor'
            return xr_parse_static_constructor(parser, is_private);
        }
    }

    if (xr_parser_match(parser, TK_FINAL)) {
        is_final = true;
    }

    if (xr_parser_match(parser, TK_OPERATOR)) {
        *is_method_out = true;
        return xr_parse_operator_method(parser, is_private, is_static);
    }

    // Parse member name (may be 'constructor' keyword)
    char *name = NULL;
    bool is_constructor = false;

    if (xr_parser_match(parser, TK_CONSTRUCTOR)) {
        // 'constructor' keyword
        is_constructor = true;
        name = (char*)xr_malloc(sizeof(XR_KEYWORD_CONSTRUCTOR));
        strcpy(name, XR_KEYWORD_CONSTRUCTOR);
    } else {
        // Normal name
        xr_parser_consume(parser, TK_NAME, "expected field or method name");
        name = token_to_string(&parser->previous);
    }

    // Distinguish field and method: check for '(' or '<' (generic) or override modifier
    if (xr_parser_check(parser, TK_LPAREN) || xr_parser_check(parser, TK_LT) || is_constructor || is_override) {
        // Method: has parameter list or generic type params or override modifier
        *is_method_out = true;
        AstNode *method = xr_parse_method_declaration(parser, name, is_private, is_static, is_abstract);
        if (method) method->as.method_decl.is_final = is_final;
        return method;
    } else {
        // Field: has type annotation or initializer
        *is_method_out = false;

        // 'override' can only be used for methods
        if (is_override) {
            xr_parser_error_at_current(parser, "'override' modifier can only be used for methods");
        }

        // Parse type annotation (optional)
        XrType *field_type = NULL;
        if (xr_parser_match(parser, TK_COLON)) {
            // Use type annotation parser, supports all types and generic syntax
            field_type = xr_parse_type_annotation(parser);
        }

        // Check for property block { fn() {} fn(v) {} }
        if (xr_parser_check(parser, TK_LBRACE)) {
            // This is a property definition with getter/setter
            *is_method_out = true;
            return xr_parse_property_accessors(parser, name, field_type, is_private, is_static, line);
        }

        // Parse initializer expression (optional)
        AstNode *initializer = NULL;
        if (xr_parser_match(parser, TK_ASSIGN)) {
            initializer = xr_parse_expression(parser);
        }

        // Field declaration doesn't need semicolon (Xray doesn't use semicolons)

        AstNode *field = xr_ast_field_decl(parser->X, name, field_type,
                                is_private, is_static,
                                initializer, line);
        if (field) field->as.field_decl.is_final = is_final;
        return field;
    }
}

/* ========== Method Declaration Parsing ========== */

// Parse method declaration
// Syntax:
//   greet(name: string): void { ... }
//   constructor(x: int, y: int) { ... }
// @param name method name (already parsed)
// @param is_private whether private
// @param is_static whether static
AstNode *xr_parse_method_declaration(Parser *parser, const char *name,
                                     bool is_private, bool is_static, bool is_abstract) {
    XR_DCHECK(parser != NULL, "parse_method_declaration: NULL parser");
    int line = parser->previous.line;

    // Check if this is a constructor
    bool is_constructor = (strcmp(name, XR_KEYWORD_CONSTRUCTOR) == 0);

    // Parse optional generic type parameters: methodName<T, U>(...)
    char **type_param_names = NULL;
    int type_param_count = 0;
    int type_param_capacity = 0;

    if (xr_parser_match(parser, TK_LT)) {
        do {
            // Parse type parameter name
            xr_parser_consume(parser, TK_NAME, "expected type parameter name");
            XR_PARSE_PUSH(type_param_names, type_param_count, type_param_capacity, token_to_string(&parser->previous));

            // Skip optional constraint <T: Interface> (for now)
            if (xr_parser_match(parser, TK_COLON)) {
                xr_parse_type_annotation(parser);  // Parse but ignore constraint
            }
        } while (xr_parser_match(parser, TK_COMMA));

        // Consume closing '>'
        xr_parser_consume(parser, TK_GT, "expected '>' after type parameters");
    }

    // Parse parameter list
    xr_parser_consume(parser, TK_LPAREN, "expected '(' to start parameter list");

    char **parameters = NULL;
    XrType **param_types = NULL;
    uint8_t *param_passing_modes = NULL;
    int param_count = 0;
    int param_capacity = 0;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            // Extend arrays
            if (param_count >= param_capacity) {
                param_capacity = param_capacity == 0 ? 4 : param_capacity * 2;
                char** _new_parameters = (char**)xr_realloc(parameters, sizeof(XrType*) * param_capacity);
                if (!_new_parameters) return NULL;
                parameters = _new_parameters;

                XrType** _new_param_types = (XrType**)xr_realloc(param_types, sizeof(XrType*) * param_capacity);
                if (!_new_param_types) return NULL;
                param_types = _new_param_types;

                uint8_t* _new_modes = (uint8_t*)xr_realloc(param_passing_modes, sizeof(uint8_t) * param_capacity);
                if (!_new_modes) return NULL;
                param_passing_modes = _new_modes;

            }

            // Parse parameter name
            xr_parser_consume(parser, TK_NAME, "expected parameter name");
            parameters[param_count] = token_to_string(&parser->previous);
            param_passing_modes[param_count] = XR_PARAM_VALUE;

            // Parse parameter type with optional in/ref modifier
            if (xr_parser_match(parser, TK_COLON)) {
                if (xr_parser_match(parser, TK_IN)) {
                    param_passing_modes[param_count] = XR_PARAM_IN;
                } else if (xr_parser_match_name(parser, "ref")) {
                    param_passing_modes[param_count] = XR_PARAM_REF;
                }
                param_types[param_count] = xr_parse_type_annotation(parser);
            } else {
                param_types[param_count] = NULL;
            }

            param_count++;
        } while (xr_parser_match(parser, TK_COMMA));
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')' to end parameter list");

    // Parse return type (optional)
    XrType *return_type = NULL;
    if (xr_parser_match(parser, TK_COLON)) {
        return_type = xr_parse_type_annotation(parser);
    }


    AstNode *body = NULL;
    if (is_abstract) {
        // Abstract method has no body, semicolon optional (Xray syntax)
        xr_parser_match(parser, TK_SEMICOLON);  // optional semicolon
    } else {
        // Normal method has body
        xr_parser_consume(parser, TK_LBRACE, "expected '{' to start method body");
        body = xr_parse_block(parser);
    }

    // Create method declaration node
    AstNode *method_node = xr_ast_method_decl(parser->X, name,
                              parameters, param_types, param_count,
                              return_type, body,
                              is_constructor, is_static, is_private,
                              false, false, line);          // is_getter, is_setter

    // Set whether this is an abstract method
    method_node->as.method_decl.is_abstract = is_abstract;
    method_node->as.method_decl.param_passing_modes = param_passing_modes;

    // Set generic type parameters
    method_node->as.method_decl.type_param_names = type_param_names;
    method_node->as.method_decl.type_param_count = type_param_count;

    return method_node;
}

/* ========== New Expression Parsing ========== */

// Parse new expression
// Syntax: new Dog("Rex", "Labrador")
// Also supports: new Box<int>(42)
AstNode *xr_parse_new_expression(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_new_expression: NULL parser");
    int line = parser->previous.line;

    // 'new' keyword already consumed

    // Parse class name (can be class name or type keyword like Map, Array)
    // Supports two forms:
    //   new ClassName()
    //   new module.ClassName()
    char *module_name = NULL;
    char *class_name = NULL;

    if (xr_parser_match(parser, TK_NAME)) {
        char *first_name = token_to_string(&parser->previous);

        // Check if it's module.Class form
        if (xr_parser_match(parser, TK_DOT)) {
            // Is module.Class form
            module_name = first_name;

            if (xr_parser_match(parser, TK_NAME)) {
                class_name = token_to_string(&parser->previous);
            } else {
                xr_parser_error_expected_name(parser, "expected class name");
                class_name = strdup(TYPE_NAME_NULL);
            }
        } else {
            // Just a normal class name
            class_name = first_name;
        }
    } else if (xr_parser_match(parser, TK_TYPE_ARRAY)) {
        class_name = (char*)xr_malloc(6);
        strcpy(class_name, "Array");
    } else if (xr_parser_match(parser, TK_TYPE_MAP)) {
        class_name = (char*)xr_malloc(4);
        strcpy(class_name, "Map");
    } else if (xr_parser_match(parser, TK_TYPE_SET)) {
        class_name = (char*)xr_malloc(4);
        strcpy(class_name, "Set");
    } else if (xr_parser_match(parser, TK_TYPE_CHANNEL)) {
        class_name = (char*)xr_malloc(8);
        strcpy(class_name, "Channel");
    } else {
        // Support other built-in type names as new target
        xr_parser_error_expected_name(parser, "expected class name or type name");
        class_name = strdup(TYPE_NAME_NULL);
    }

    // Parse optional generic type parameters: new Box<int>(...)
    XrType *type_args[16];  // Max 16 type args
    int type_arg_count = 0;

    if (xr_parser_match(parser, TK_LT)) {
        do {
            if (type_arg_count >= 16) break;

            XrType *type = xr_parse_type_annotation(parser);
            if (!type) {
                xr_parser_error(parser, "expected type in generic type arguments");
                break;
            }
            type_args[type_arg_count++] = type;
        } while (xr_parser_match(parser, TK_COMMA));

        // Consume closing '>' (handle >> as two > for nested generics like Box<Array<int>>)
        if (xr_parser_check(parser, TK_GT)) {
            xr_parser_advance(parser);
        } else if (xr_parser_check(parser, TK_RSHIFT)) {
            // >> is treated as two >, consume only the first one
            // Transform '>>' to single '>' and leave the second '>' for outer generic
            parser->previous = parser->current;
            parser->previous.type = TK_GT;
            parser->previous.length = 1;
            // Transform current token from '>>' to '>'
            parser->current.type = TK_GT;
            parser->current.start++;
            parser->current.length = 1;
        } else {
            xr_parser_error(parser, "expected '>' after generic type arguments");
        }
    }

    // Parse constructor arguments
    xr_parser_consume(parser, TK_LPAREN, "expected '(' to start argument list");

    AstNode **arguments = NULL;
    int arg_count = 0;
    int arg_capacity = 0;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            XR_PARSE_PUSH(arguments, arg_count, arg_capacity, xr_parse_expression(parser));
        } while (xr_parser_match(parser, TK_COMMA));
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')' to end argument list");

    // Create new expression node
    return xr_ast_new_expr(parser->X, module_name, class_name, arguments, arg_count,
                           type_args, type_arg_count, line);
}

/* ========== This Expression Parsing ========== */

// Parse this expression
// Syntax: this
AstNode *xr_parse_this_expression(Parser *parser) {
    int line = parser->previous.line;

    // 'this' keyword already consumed

    // Create this expression node
    return xr_ast_this_expr(parser->X, line);
}

/* ========== Super Expression Parsing ========== */

// Parse super call
// Syntax:
//   super.greet(args)  - call superclass method
//   super(args)        - call superclass constructor
AstNode *xr_parse_super_expression(Parser *parser) {
    int line = parser->previous.line;

    // 'super' keyword already consumed

    char *method_name = NULL;

    // Check if super() or super.method()
    if (xr_parser_match(parser, TK_DOT)) {
        // super.method()
        xr_parser_consume(parser, TK_NAME, "expected method name");
        method_name = token_to_string(&parser->previous);

        // Parse parameter list
        xr_parser_consume(parser, TK_LPAREN, "expected '(' to start argument list");
    } else if (xr_parser_match(parser, TK_LPAREN)) {
        // super(args) - call superclass constructor
        method_name = NULL;  // NULL means constructor
    } else {
        xr_parser_error(parser, "expected '.' or '(' after super");
        return NULL;
    }

    // Parse arguments
    AstNode **arguments = NULL;
    int arg_count = 0;
    int arg_capacity = 0;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            XR_PARSE_PUSH(arguments, arg_count, arg_capacity, xr_parse_expression(parser));
        } while (xr_parser_match(parser, TK_COMMA));
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')' to end argument list");

    // Create super call node
    return xr_ast_super_call(parser->X, method_name, arguments, arg_count, line);
}

/* ========== Operator Method Parsing ========== */

// Parse operator method declaration
// Syntax: operator +(other: Type): Type { ... }
// @param is_private whether private
// @param is_static whether static
AstNode *xr_parse_operator_method(Parser *parser, bool is_private, bool is_static) {
    XR_DCHECK(parser != NULL, "parse_operator_method: NULL parser");
    int line = parser->previous.line;

    // Parse operator symbol
    TokenType op_token = parser->current.type;

    char *name = NULL;
    int expected_params = 1;  // Most are binary operators, need 1 parameter (the other operand)

    // Determine operator and op_type based on token type
    OperatorType op_type_val;
    switch (op_token) {
        // Arithmetic operators
        case TK_PLUS:
            name = (char*)xr_malloc(2);
            strcpy(name, "+");
            op_type_val = OPTYPE_ADD;
            break;
        case TK_MINUS:
            name = (char*)xr_malloc(2);
            strcpy(name, "-");
            op_type_val = OPTYPE_SUB;  // default to binary, adjusted later based on param count
            break;
        case TK_STAR:
            name = (char*)xr_malloc(2);
            strcpy(name, "*");
            op_type_val = OPTYPE_MUL;
            break;
        case TK_SLASH:
            name = (char*)xr_malloc(2);
            strcpy(name, "/");
            op_type_val = OPTYPE_DIV;
            break;
        case TK_PERCENT:
            name = (char*)xr_malloc(2);
            strcpy(name, "%");
            op_type_val = OPTYPE_MOD;
            break;
        // Bitwise operators
        case TK_AMP:
            name = (char*)xr_malloc(2);
            strcpy(name, "&");
            op_type_val = OPTYPE_BAND;
            break;
        case TK_PIPE:
            name = (char*)xr_malloc(2);
            strcpy(name, "|");
            op_type_val = OPTYPE_BOR;
            break;
        case TK_CARET:
            name = (char*)xr_malloc(2);
            strcpy(name, "^");
            op_type_val = OPTYPE_BXOR;
            break;
        case TK_TILDE:
            name = (char*)xr_malloc(2);
            strcpy(name, "~");
            op_type_val = OPTYPE_UNARY;  // unary operator
            expected_params = 0;  // unary operator needs no extra params
            break;
        case TK_NOT:
            name = (char*)xr_malloc(2);
            strcpy(name, "!");
            op_type_val = OPTYPE_UNARY;  // unary operator
            expected_params = 0;  // unary operator needs no extra params
            break;
        // Comparison operators
        case TK_EQ:
            name = (char*)xr_malloc(3);
            strcpy(name, "==");
            op_type_val = OPTYPE_EQ;
            break;
        case TK_NE:
            name = (char*)xr_malloc(3);
            strcpy(name, "!=");
            op_type_val = OPTYPE_NE;
            break;
        case TK_LT:
            name = (char*)xr_malloc(2);
            strcpy(name, "<");
            op_type_val = OPTYPE_LT;
            break;
        case TK_LE:
            name = (char*)xr_malloc(3);
            strcpy(name, "<=");
            op_type_val = OPTYPE_LE;
            break;
        case TK_GT:
            name = (char*)xr_malloc(2);
            strcpy(name, ">");
            op_type_val = OPTYPE_GT;
            break;
        case TK_GE:
            name = (char*)xr_malloc(3);
            strcpy(name, ">=");
            op_type_val = OPTYPE_GE;
            break;

        // Subscript operator
        case TK_LBRACKET:
            xr_parser_advance(parser);
            if (!xr_parser_match(parser, TK_RBRACKET)) {
                xr_parser_error(parser, "expected ']' in operator []");
                return NULL;
            }
            // Check for = sign (operator []=)
            if (xr_parser_match(parser, TK_ASSIGN)) {
                name = (char*)xr_malloc(4);
                strcpy(name, "[]=");
                expected_params = 2;  // []= needs 2 params: index + value
                op_type_val = OPTYPE_SUBSCRIPT_SET;
            } else {
                name = (char*)xr_malloc(3);
                strcpy(name, "[]");
                expected_params = 1;  // [] needs 1 param: index
                op_type_val = OPTYPE_SUBSCRIPT;
            }
            break;

        // Shift operators
        case TK_LSHIFT:
            name = (char*)xr_malloc(3);
            strcpy(name, "<<");
            op_type_val = OPTYPE_BAND;  // reuse bitwise type
            break;
        case TK_RSHIFT:
            name = (char*)xr_malloc(3);
            strcpy(name, ">>");
            op_type_val = OPTYPE_BAND;  // reuse bitwise type
            break;

        // Compound assignment operators
        case TK_PLUS_ASSIGN:
            name = (char*)xr_malloc(3);
            strcpy(name, "+=");
            op_type_val = OPTYPE_ADD;  // reuse add type
            break;
        case TK_MINUS_ASSIGN:
            name = (char*)xr_malloc(3);
            strcpy(name, "-=");
            op_type_val = OPTYPE_SUB;  // reuse sub type
            break;
        case TK_MUL_ASSIGN:
            name = (char*)xr_malloc(3);
            strcpy(name, "*=");
            op_type_val = OPTYPE_MUL;  // reuse mul type
            break;
        case TK_DIV_ASSIGN:
            name = (char*)xr_malloc(3);
            strcpy(name, "/=");
            op_type_val = OPTYPE_DIV;  // reuse div type
            break;
        case TK_MOD_ASSIGN:
            name = (char*)xr_malloc(3);
            strcpy(name, "%=");
            op_type_val = OPTYPE_MOD;  // reuse mod type
            break;
        case TK_AND_ASSIGN:
            name = (char*)xr_malloc(3);
            strcpy(name, "&=");
            op_type_val = OPTYPE_BAND;  // reuse bitwise and type
            break;
        case TK_OR_ASSIGN:
            name = (char*)xr_malloc(3);
            strcpy(name, "|=");
            op_type_val = OPTYPE_BOR;  // reuse bitwise or type
            break;
        case TK_XOR_ASSIGN:
            name = (char*)xr_malloc(3);
            strcpy(name, "^=");
            op_type_val = OPTYPE_BXOR;  // reuse bitwise xor type
            break;
        case TK_LSHIFT_ASSIGN:
            name = (char*)xr_malloc(4);
            strcpy(name, "<<=");
            op_type_val = OPTYPE_BAND;  // reuse bitwise type
            break;
        case TK_RSHIFT_ASSIGN:
            name = (char*)xr_malloc(4);
            strcpy(name, ">>=");
            op_type_val = OPTYPE_BAND;  // reuse bitwise type
            break;

        // Increment/decrement operators
        case TK_INC:
            name = (char*)xr_malloc(3);
            strcpy(name, "++");
            op_type_val = OPTYPE_UNARY;  // treat as unary operator
            break;
        case TK_DEC:
            name = (char*)xr_malloc(3);
            strcpy(name, "--");
            op_type_val = OPTYPE_UNARY;  // treat as unary operator
            break;

        default:
            xr_parser_error(parser, "unsupported operator type");
            return NULL;
    }

    // Consume operator token (except [] already consumed)
    if (op_token != TK_LBRACKET) {
        xr_parser_advance(parser);
    }

    // Parse parameter list
    xr_parser_consume(parser, TK_LPAREN, "expected '(' to start parameter list");

    char **parameters = NULL;
    XrType **param_types = NULL;
    int param_count = 0;

    // Parse parameters: most operators need 1, []= needs 2, unary operators need 0
    if (xr_parser_check(parser, TK_RPAREN)) {
        if (expected_params > 0 && op_token != TK_MINUS && op_token != TK_NOT && op_token != TK_INC && op_token != TK_DEC) {
            xr_parser_error(parser, "operator method requires parameters");
            return NULL;
        }
        // Unary operators (expected_params == 0) can have no parameters
        xr_parser_consume(parser, TK_RPAREN, "expected ')' to end parameter list");
        parameters = NULL;
        param_types = NULL;
        param_count = 0;
        // Go directly to return type parsing, no goto needed
    } else {
        // Allocate parameter arrays
        parameters = (char**)xr_malloc(sizeof(char*) * expected_params);
        param_types = (XrType**)xr_malloc(sizeof(XrType*) * expected_params);

    // Parse first parameter
    xr_parser_consume(parser, TK_NAME, "expected parameter name");
    parameters[0] = token_to_string(&parser->previous);

    // Parse parameter type (optional) - use full type annotation parser
    if (xr_parser_match(parser, TK_COLON)) {
        param_types[0] = xr_parse_type_annotation(parser);
    } else {
        param_types[0] = NULL;
    }

    param_count = 1;

    // operator []= needs second parameter (value)
    if (expected_params == 2) {
        if (!xr_parser_match(parser, TK_COMMA)) {
            xr_parser_error(parser, "operator []= requires 2 parameters");
            return NULL;
        }

        // Parse second parameter
        xr_parser_consume(parser, TK_NAME, "expected parameter name");
        parameters[1] = token_to_string(&parser->previous);

        // Parse second parameter type (optional) - use full type annotation parser
        if (xr_parser_match(parser, TK_COLON)) {
            param_types[1] = xr_parse_type_annotation(parser);
        } else {
            param_types[1] = NULL;
        }

        param_count = 2;
    }

        // Should not have more parameters
        if (xr_parser_match(parser, TK_COMMA)) {
            xr_parser_error(parser, "too many parameters for operator method");
            return NULL;
        }

        xr_parser_consume(parser, TK_RPAREN, "expected ')' to end parameter list");
    }

    // Adjust operator type based on parameter count (handle unary/binary ambiguity)
    if (op_token == TK_MINUS && param_count == 0) {
        op_type_val = OPTYPE_UNARY;  // no params - is unary negation
    }
    if (op_token == TK_NOT && param_count == 0) {
        op_type_val = OPTYPE_UNARY;  // no params ! is unary logical not
    }

    // Parse return type (optional)
    XrType *return_type = NULL;
    if (xr_parser_match(parser, TK_COLON)) {
        return_type = xr_parse_type_annotation(parser);
    }

    // Parse method body
    xr_parser_consume(parser, TK_LBRACE, "expected '{' to start method body");
    AstNode *body = xr_parse_block(parser);

    // Create method node
    AstNode *method = xr_ast_method_decl(parser->X, name,
                                        parameters, param_types, param_count,
                                        return_type, body,
                                        false,  // is_constructor
                                        is_static,
                                        is_private,
                                        false,  // is_getter
                                        false,  // is_setter
                                        line);

    // Set operator flags
    method->as.method_decl.is_operator = true;  // mark as operator method
    method->as.method_decl.op_type = op_type_val;  // set specific operator type

    return method;
}

/* ========== Property Accessor Parsing (New Syntax) ========== */

// Parse property accessor block
// New syntax:
//   x: int {
//       fn() { return this._x }         // getter (no params)
//       fn(v) { this._x = v }           // setter (has params)
//   }
// @param name property name
// @param field_type property type (optional)
// @param is_private whether private
// @param is_static whether static
// @param line line number
// @return returns list node of multiple method declaration nodes
static AstNode *xr_parse_property_accessors(Parser *parser, const char *name,
                                            XrType *field_type,
                                            bool is_private, bool is_static, int line) {
    xr_parser_consume(parser, TK_LBRACE, "expected '{' to start property accessor block");

    AstNode *getter_node = NULL;
    AstNode *setter_node = NULL;

    // Parse fn definitions in property block
    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        if (!xr_parser_match(parser, TK_FN)) {
            xr_parser_error(parser, "property accessor block can only contain fn() definitions");
            break;
        }

        // Parse parameter list
        xr_parser_consume(parser, TK_LPAREN, "expected '(' to start parameter list");

        char **parameters = NULL;
        XrType **param_types = NULL;
        int param_count = 0;

        if (!xr_parser_check(parser, TK_RPAREN)) {
            // Has parameters = setter
            parameters = (char**)xr_malloc(sizeof(char*));
            param_types = (XrType**)xr_malloc(sizeof(XrType*));

            xr_parser_consume(parser, TK_NAME, "expected parameter name");
            parameters[0] = token_to_string(&parser->previous);

            // Parse parameter type (optional)
            if (xr_parser_match(parser, TK_COLON)) {
                param_types[0] = xr_parse_type_annotation(parser);
            } else {
                param_types[0] = field_type;  // use property type as default param type
            }

            param_count = 1;

            if (xr_parser_match(parser, TK_COMMA)) {
                xr_parser_error(parser, "setter can only have one parameter");
            }
        }

        xr_parser_consume(parser, TK_RPAREN, "expected ')' to end parameter list");

        // Parse return type (optional)
        XrType *return_type = NULL;
        if (xr_parser_match(parser, TK_COLON)) {
            return_type = xr_parse_type_annotation(parser);
        } else if (param_count == 0) {
            // getter defaults to property type
            return_type = field_type;
        }

        // Parse method body
        xr_parser_consume(parser, TK_LBRACE, "expected '{' to start method body");
        AstNode *body = xr_parse_block(parser);

        // Construct method name: get:xxx or set:xxx
        bool is_getter = (param_count == 0);
        size_t name_len = strlen(name) + 5;
        char *method_name = (char*)xr_malloc(name_len);
        snprintf(method_name, name_len, "%s:%s", is_getter ? "get" : "set", name);

        // Create method declaration node
        AstNode *method_node = xr_ast_method_decl(parser->X, method_name,
                                                   parameters, param_types, param_count,
                                                   return_type, body,
                                                   false, is_static, is_private,
                                                   is_getter, !is_getter, line);

        if (is_getter) {
            if (getter_node != NULL) {
                xr_parser_error(parser, "property can only have one getter (fn with no params)");
            }
            getter_node = method_node;
        } else {
            if (setter_node != NULL) {
                xr_parser_error(parser, "property can only have one setter (fn with params)");
            }
            setter_node = method_node;
        }
    }

    xr_parser_consume(parser, TK_RBRACE, "expected '}' to end property accessor block");

    if (getter_node == NULL && setter_node == NULL) {
        xr_parser_error(parser, "property accessor block cannot be empty");
        return NULL;
    }

    // If only one, return directly
    if (getter_node == NULL) return setter_node;
    if (setter_node == NULL) return getter_node;

    // Both present, link setter to getter's next pointer
    // Note: this needs special handling in class parsing
    getter_node->as.method_decl.base_arg_count = -2;  // special mark: has paired setter
    setter_node->as.method_decl.base_arg_count = -1;  // special mark: this is paired setter

    // Store setter in getter's base_args (temporarily borrowed)
    getter_node->as.method_decl.base_args = (AstNode**)setter_node;

    return getter_node;
}

/* ========== Interface Declaration Parsing ========== */

// Parse interface declaration
// Syntax:
//   interface Drawable [extends Shape, Colorful] {
//       draw(): void;
//       getZIndex(): int;
//   }
AstNode *xr_parse_interface_declaration(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_interface_declaration: NULL parser");
    int line = parser->previous.line;

    // 'interface' keyword already consumed

    // Parse interface name
    xr_parser_consume(parser, TK_NAME, "expected interface name");
    char *interface_name = token_to_string(&parser->previous);

    // Parse extends clause (optional, interface can extend multiple interfaces)
    char **extends = NULL;
    int extends_count = 0;
    int extends_capacity = 0;

    if (xr_parser_match(parser, TK_EXTENDS)) {
        do {
            xr_parser_consume(parser, TK_NAME, "expected interface name");
            char *parent_name = token_to_string(&parser->previous);

            XR_PARSE_PUSH(extends, extends_count, extends_capacity, parent_name);

        } while (xr_parser_match(parser, TK_COMMA));
    }

    // Parse interface body
    xr_parser_consume(parser, TK_LBRACE, "expected '{' to start interface body");

    // Collect method signatures
    AstNode **methods = NULL;
    int method_count = 0;
    int method_capacity = 0;

    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        // Error recovery to avoid infinite loop
        if (parser->panic_mode) {
            xr_parser_synchronize(parser);
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_EOF)) break;
            continue;
        }

        // Parse method signature
        AstNode *method = xr_parse_interface_method(parser);
        if (!method) {
            // Skip to next method or end of interface
            if (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
                xr_parser_advance(parser);
            }
            continue;
        }

        XR_PARSE_PUSH(methods, method_count, method_capacity, method);
    }

    xr_parser_consume(parser, TK_RBRACE, "expected '}' to end interface body");

    // Create interface declaration AST node
    return xr_ast_interface_decl(parser->X, interface_name,
                                 extends, extends_count,
                                 methods, method_count, line);
}

// Parse interface method signature
// Syntax:
//   draw(): void;
//   area(precision: int): float;
AstNode *xr_parse_interface_method(Parser *parser) {
    int line = parser->current.line;

    // Parse method name
    xr_parser_consume(parser, TK_NAME, "expected method name");
    char *method_name = token_to_string(&parser->previous);

    // Parse parameter list
    xr_parser_consume(parser, TK_LPAREN, "expected '('");

    char **parameters = NULL;
    XrType **param_types = NULL;
    int param_count = 0;
    int param_capacity = 0;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            // Parameter name
            xr_parser_consume(parser, TK_NAME, "expected parameter name");
            char *param_name = token_to_string(&parser->previous);

            // Parameter type (required in interface method signatures)
            xr_parser_consume(parser, TK_COLON, "expected ':'");

            XrType *param_type = NULL;
            if (xr_parser_match(parser, TK_INT)) {
                param_type = xr_type_new_int();
            } else if (xr_parser_match(parser, TK_FLOAT)) {
                param_type = xr_type_new_float();
            } else if (xr_parser_match(parser, TK_STRING)) {
                param_type = xr_type_new_string();
            } else if (xr_parser_match(parser, TK_BOOL)) {
                param_type = xr_type_new_bool();
            } else if (xr_parser_match(parser, TK_NAME)) {
                param_type = xr_type_new_class(token_to_string(&parser->previous));
            } else {
                xr_parser_error(parser, "expected parameter type");
                param_type = xr_type_new_unknown();
            }

            // Add to parameters array
            if (param_count >= param_capacity) {
                param_capacity = param_capacity == 0 ? 4 : param_capacity * 2;
                char** _new_parameters = (char**)xr_realloc(parameters, sizeof(XrType*) * param_capacity);
                if (!_new_parameters) return NULL;
                parameters = _new_parameters;

                XrType** _new_param_types = (XrType**)xr_realloc(param_types, sizeof(XrType*) * param_capacity);
                if (!_new_param_types) return NULL;
                param_types = _new_param_types;

            }
            parameters[param_count] = param_name;
            param_types[param_count] = param_type;
            param_count++;

        } while (xr_parser_match(parser, TK_COMMA));
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')'");

    // Parse return type (optional)
    XrType *return_type = NULL;
    if (xr_parser_match(parser, TK_COLON)) {
        return_type = xr_parse_type_annotation(parser);
    }

    // Interface method signature ends with semicolon (optional)
    xr_parser_match(parser, TK_SEMICOLON);  // optional semicolon

    // Create interface method signature node
    return xr_ast_interface_method(parser->X, method_name,
                                    parameters, param_types, param_count,
                                    return_type, line);
}

/* ========== Enum Declaration Parsing ========== */

// Parse enum declaration
// Syntax:
//   enum Status : int { Success = 200, Error = 500 }
//   enum Color { Red, Green, Blue }
AstNode *xr_parse_enum_declaration(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_enum_declaration: NULL parser");
    int line = parser->previous.line;

    // 'enum' keyword already consumed

    // Parse enum name
    xr_parser_consume(parser, TK_NAME, "expected enum name");
    char *enum_name = token_to_string(&parser->previous);

    // Parse type hint (optional): int, string, float, bool
    char *type_hint = NULL;
    if (xr_parser_match(parser, TK_COLON)) {
        // Support type keywords (uppercase) and plain identifiers (lowercase)
        if (xr_parser_match(parser, TK_INT)) {
            type_hint = strdup(TYPE_NAME_INT);
        } else if (xr_parser_match(parser, TK_STRING)) {
            type_hint = strdup(TYPE_NAME_STRING);
        } else if (xr_parser_match(parser, TK_FLOAT)) {
            type_hint = strdup(TYPE_NAME_FLOAT);
        } else if (xr_parser_match(parser, TK_BOOL)) {
            type_hint = strdup(TYPE_NAME_BOOL);
        } else if (xr_parser_match(parser, TK_NAME)) {
            // Support lowercase type names
            Token t = parser->previous;
            if (t.length == 3 && memcmp(t.start, "int", 3) == 0) {
                type_hint = strdup(TYPE_NAME_INT);
            } else if (t.length == 6 && memcmp(t.start, "string", 6) == 0) {
                type_hint = strdup(TYPE_NAME_STRING);
            } else if (t.length == 5 && memcmp(t.start, "float", 5) == 0) {
                type_hint = strdup(TYPE_NAME_FLOAT);
            } else if (t.length == 4 && memcmp(t.start, "bool", 4) == 0) {
                type_hint = strdup(TYPE_NAME_BOOL);
            } else {
                xr_parser_error(parser, "enum type must be int, string, float or bool");
            }
        } else {
            xr_parser_error(parser, "enum type must be int, string, float or bool");
            type_hint = NULL;
        }
    }

    // Parse enum body
    xr_parser_consume(parser, TK_LBRACE, "expected '{' to start enum body");

    // Collect enum members
    AstNode **members = NULL;
    int member_count = 0;
    int member_capacity = 0;

    // At least one member required
    if (xr_parser_check(parser, TK_RBRACE)) {
        xr_parser_error(parser, "enum requires at least one member");
    }

    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        // Error recovery to avoid infinite loop
        if (parser->panic_mode) {
            xr_parser_synchronize(parser);
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_EOF)) break;
            continue;
        }

        // Skip unknown tokens
        if (!xr_parser_check(parser, TK_NAME)) {
            xr_parser_error_expected_name(parser, "expected enum member name");
            xr_parser_advance(parser);
            continue;
        }

        // Parse member name
        xr_parser_consume(parser, TK_NAME, "expected enum member name");
        char *member_name = token_to_string(&parser->previous);

        // Parse member value (optional)
        AstNode *member_value = NULL;
        if (xr_parser_match(parser, TK_ASSIGN)) {
            // Parse constant expression
            member_value = xr_parse_expression(parser);
        }

        // Create enum member node
        AstNode *member = xr_ast_enum_member(parser->X, member_name,
                                             member_value, parser->previous.line);

        XR_PARSE_PUSH(members, member_count, member_capacity, member);

        // Comma separator (optional, can be omitted after last member)
        if (!xr_parser_check(parser, TK_RBRACE)) {
            if (!xr_parser_match(parser, TK_COMMA)) {
                xr_parser_error(parser, "expected ',' to separate enum members");
                break;
            }
            // Allow trailing comma
            if (xr_parser_check(parser, TK_RBRACE)) {
                break;
            }
        }
    }

    xr_parser_consume(parser, TK_RBRACE, "expected '}' to end enum body");

    // Create enum declaration AST node
    return xr_ast_enum_decl(parser->X, enum_name, type_hint,
                            members, member_count, line);
}

/* ========== Static Constructor Parsing ========== */

// Parse static constructor
// Syntax:
//   static constructor() {
//       // class-level initialization code
//   }
// @param is_private whether private (usually not allowed for static constructor, but parameter kept)
AstNode *xr_parse_static_constructor(Parser *parser, bool is_private) {
    int line = parser->previous.line;

    // Static constructor cannot have parameters
    xr_parser_consume(parser, TK_LPAREN, "expected '('");

    if (!xr_parser_check(parser, TK_RPAREN)) {
        xr_parser_error(parser, "static constructor cannot have parameters");
        // Skip all parameters until ')'
        while (!xr_parser_check(parser, TK_RPAREN) && !xr_parser_check(parser, TK_EOF)) {
            xr_parser_advance(parser);
        }
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')'");

    // Static constructor cannot have return type
    if (xr_parser_check(parser, TK_COLON)) {
        xr_parser_error(parser, "static constructor cannot have return type");
        xr_parser_advance(parser);  // skip :
        // Skip type annotation
        if (xr_parser_check(parser, TK_NAME) || xr_parser_check(parser, TK_INT) ||
            xr_parser_check(parser, TK_STRING) || xr_parser_check(parser, TK_FLOAT) ||
            xr_parser_check(parser, TK_BOOL) || xr_parser_check(parser, TK_VOID)) {
            xr_parser_advance(parser);
        }
    }

    // Parse method body
    xr_parser_consume(parser, TK_LBRACE, "expected '{' to start static constructor body");
    AstNode *body = xr_parse_block(parser);

    // Create method declaration node
    AstNode *method_node = xr_ast_method_decl(parser->X, "<clinit>",
                              NULL, NULL, 0,  // no parameters
                              NULL,  // no return type
                              body,
                              false,  // not a regular constructor
                              true,   // is static
                              is_private,
                              false, false, line);

    // Mark as static constructor
    method_node->as.method_decl.is_static_constructor = true;

    return method_node;
}

