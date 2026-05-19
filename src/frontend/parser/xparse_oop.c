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
#include "xtype_ref.h"
#include "../../runtime/value/xtype_names.h"
#include "../../runtime/value/xtype.h"
#include "xtype_scope.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Forward declarations
static AstNode *xr_parse_property_accessors(Parser *parser, const char *name, XrTypeRef *field_type,
                                            bool is_private, bool is_static, int line);

// Helper: copy Token text to a string allocated from the parser arena.
// The returned buffer lives for the full parse lifetime and is bulk-freed
// when the owning program node is destroyed.
static char *token_to_string(Parser *parser, Token *token) {
    if (!token || token->length == 0)
        return NULL;

    char *str = (char *) ast_alloc(parser->X, (size_t) token->length + 1);
    memcpy(str, token->start, token->length);
    str[token->length] = '\0';
    return str;
}

/* ========== Local Cleanup Helpers ========== */

// All parser allocations now go through the parse arena; individual frees
// are no-ops. Arena destroy at parse end (or on error via
// xr_parse_discard_arena) releases every string, param, and array
// allocated during parsing.

static inline void oop_free_generic_params(XrGenericParam **type_params, int count) {
    (void) type_params;
    (void) count;
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
    char *class_name = token_to_string(parser, &parser->previous);
    int name_column = parser->previous.column;

    // Parse generic type parameters <T, U: Constraint>
    XrGenericParam **type_params = NULL;
    int type_param_count = 0;
    int type_param_capacity = 0;

    if (xr_parser_match(parser, TK_LT)) {
        do {
            xr_parser_consume(parser, TK_NAME, "expected type parameter name");
            Token param_token = parser->previous;

            char *param_name = token_to_string(parser, &param_token);

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

    // Register generic type params in type_scope for field/method type parsing.
    XrTypeScope *saved_scope = parser->type_scope;
    if (type_param_count > 0) {
        XrTypeScope *generic_scope = xr_type_scope_new(parser->type_scope);
        for (int i = 0; i < type_param_count; i++) {
            XrTypeRef *type_param = xr_tref_type_param(parser->X, type_params[i]->name);
            xr_type_scope_define(generic_scope, type_params[i]->name, type_param);
        }
        parser->type_scope = generic_scope;
    }

    // Parse extends clause (optional)
    // Supports: extends Class or extends module.Class
    char *super_name = NULL;
    char *super_module = NULL;
    if (xr_parser_match(parser, TK_EXTENDS)) {
        xr_parser_consume(parser, TK_NAME, "expected superclass name");
        char *first_name = token_to_string(parser, &parser->previous);

        // Check for module.Class form
        if (xr_parser_match(parser, TK_DOT)) {
            xr_parser_consume(parser, TK_NAME, "expected class name after '.'");
            super_module = first_name;
            super_name = token_to_string(parser, &parser->previous);
        } else {
            super_name = first_name;
        }
    }

    // Implemented interfaces are full type references so that
    // `class IntBox implements Container<int>` and the bare-name form
    // `class Dog implements Comparable` go through the same path.
    XrTypeRef **interfaces = NULL;
    int interface_count = 0;
    int interface_capacity = 0;

    if (xr_parser_match(parser, TK_IMPLEMENTS)) {
        do {
            XrTypeRef *iface_ref = xr_parse_type_annotation(parser);
            if (!iface_ref)
                break;
            XR_PARSE_PUSH(parser, interfaces, interface_count, interface_capacity, iface_ref);
        } while (xr_parser_match(parser, TK_COMMA));
    }

    // Detect colon-style inheritance: class Dog : Animal
    if (xr_parser_check(parser, TK_COLON)) {
        xr_parser_error_at_current(
            parser,
            "use 'extends' instead of ':' for class inheritance, e.g. class Dog extends Animal");
        // Release every local heap state we own: AST is not built on this
        // path so ownership is not transferred. Also restore type_scope so
        // the rest of the file continues to parse in the outer scope.
        if (type_param_count > 0) {
            parser->type_scope = saved_scope;
        }
        oop_free_generic_params(type_params, type_param_count);
        // interfaces[] entries are arena-owned XrTypeRefs; nothing to free.
        (void) interfaces;
        (void) interface_count;
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

    // Save and restore native flag — the flag is set by the caller
    // (xr_parse_attributed_declaration) after this function returns,
    // but we also need it during body parsing. Use a pre-set approach:
    // the caller will set is_native on the returned node, but we need
    // to propagate it into method parsing. We use parser->parsing_native_class.
    bool saved_native = parser->parsing_native_class;

    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        // Error recovery: skip to next valid token
        if (parser->panic_mode) {
            xr_parser_synchronize(parser);
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_EOF))
                break;
            continue;
        }

        // Friendly hint: check common errors
        if (xr_parser_check(parser, TK_LET)) {
            xr_parser_error_at_current(parser,
                                       "'let' keyword not needed for field declarations in class "
                                       "body, write field name directly, e.g.: name: string\n"
                                       "'fn' keyword not needed for method definitions either, "
                                       "write method name directly, e.g.: greet() { ... }");
            xr_parser_advance(parser);
            continue;
        }

        if (xr_parser_check(parser, TK_FN)) {
            xr_parser_error_at_current(parser,
                                       "'fn' keyword not needed for method definitions in class "
                                       "body, write method name directly, e.g.: greet() { ... }");
            xr_parser_advance(parser);
            continue;
        }

        // Skip optional semicolons (Xray supports optional semicolons)
        if (xr_parser_check(parser, TK_SEMICOLON)) {
            xr_parser_advance(parser);
            continue;
        }

        // Skip unknown tokens to avoid infinite loop
        if (!xr_parser_check(parser, TK_NAME) && !xr_parser_check(parser, TK_PRIVATE) &&
            !xr_parser_check(parser, TK_PUBLIC) && !xr_parser_check(parser, TK_STATIC) &&
            !xr_parser_check(parser, TK_CONSTRUCTOR) && !xr_parser_check(parser, TK_ABSTRACT) &&
            !xr_parser_check(parser, TK_OVERRIDE) && !xr_parser_check(parser, TK_FINAL) &&
            !xr_parser_check(parser, TK_OPERATOR)) {
            xr_parser_error_expected_name(parser, "expected field or method name");
            xr_parser_advance(parser);
            continue;
        }

        // Determine if this is a method or field
        bool is_method = false;
        AstNode *member = xr_parse_field_declaration(parser, &is_method);

        if (is_method) {
            XR_PARSE_PUSH(parser, methods, method_count, method_capacity, member);

            // Check for paired setter (property accessor block case)
            if (member->type == AST_METHOD_DECL && member->as.method_decl.base_arg_count == -2) {
                // Has paired setter, extract from temporary storage and add
                AstNode *setter = (AstNode *) member->as.method_decl.base_args;
                member->as.method_decl.base_arg_count = 0;  // restore normal value
                member->as.method_decl.base_args = NULL;
                setter->as.method_decl.base_arg_count = 0;  // restore normal value

                XR_PARSE_PUSH(parser, methods, method_count, method_capacity, setter);
            }
        } else {
            XR_PARSE_PUSH(parser, fields, field_count, field_capacity, member);
        }
    }

    parser->parsing_native_class = saved_native;

    xr_parser_consume(parser, TK_RBRACE, "expected '}' to end class body");
    int end_line = parser->previous.line;
    int end_column = parser->previous.column + 1;  // exclusive, past '}'

    // Restore type_scope after parsing class body
    if (type_param_count > 0) {
        parser->type_scope = saved_scope;
    }

    // Create class declaration AST node
    AstNode *class_node = xr_ast_class_decl(parser->X, class_name, super_name, fields, field_count,
                                            methods, method_count, line);
    class_node->column = name_column;
    class_node->end_line = end_line;
    class_node->end_column = end_column;

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
    char *struct_name = token_to_string(parser, &parser->previous);
    int name_column = parser->previous.column;

    // Parse generic type parameters <T, U: Constraint>
    XrGenericParam **type_params = NULL;
    int type_param_count = 0;
    int type_param_capacity = 0;

    if (xr_parser_match(parser, TK_LT)) {
        do {
            xr_parser_consume(parser, TK_NAME, "expected type parameter name");
            Token param_token = parser->previous;

            char *param_name = token_to_string(parser, &param_token);

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

    // Register generic type params in type_scope for field/method type parsing.
    XrTypeScope *saved_scope = parser->type_scope;
    if (type_param_count > 0) {
        XrTypeScope *generic_scope = xr_type_scope_new(parser->type_scope);
        for (int i = 0; i < type_param_count; i++) {
            XrTypeRef *tp = xr_tref_type_param(parser->X, type_params[i]->name);
            xr_type_scope_define(generic_scope, type_params[i]->name, tp);
        }
        parser->type_scope = generic_scope;
    }

    // Structs do not support extends (no inheritance)
    if (xr_parser_check(parser, TK_EXTENDS)) {
        xr_parser_error_at_current(parser, "structs cannot inherit from other types");
    }

    // Parse implements clause (structs can implement interfaces).
    // Use full type-reference parser so `implements Container<int>` works.
    XrTypeRef **interfaces = NULL;
    int interface_count = 0;
    int interface_capacity = 0;

    if (xr_parser_match(parser, TK_IMPLEMENTS)) {
        do {
            XrTypeRef *iface_ref = xr_parse_type_annotation(parser);
            if (!iface_ref)
                break;
            XR_PARSE_PUSH(parser, interfaces, interface_count, interface_capacity, iface_ref);
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
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_EOF))
                break;
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
            xr_parser_error_at_current(
                parser,
                "structs cannot have explicit constructors; use struct literal syntax instead");
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
            xr_parser_error_at_current(
                parser, "'let' keyword not needed for field declarations in struct body");
            xr_parser_advance(parser);
            continue;
        }
        if (xr_parser_check(parser, TK_FN)) {
            xr_parser_error_at_current(
                parser, "'fn' keyword not needed for method definitions in struct body");
            xr_parser_advance(parser);
            continue;
        }

        // Skip unknown tokens
        if (!xr_parser_check(parser, TK_NAME) && !xr_parser_check(parser, TK_PRIVATE) &&
            !xr_parser_check(parser, TK_PUBLIC) && !xr_parser_check(parser, TK_STATIC) &&
            !xr_parser_check(parser, TK_OPERATOR)) {
            xr_parser_error_expected_name(parser, "expected field or method name in struct");
            xr_parser_advance(parser);
            continue;
        }

        bool is_method = false;
        AstNode *member = xr_parse_field_declaration(parser, &is_method);

        if (is_method) {
            XR_PARSE_PUSH(parser, methods, method_count, method_capacity, member);

            // Check for paired setter (property accessor block case)
            if (member->type == AST_METHOD_DECL && member->as.method_decl.base_arg_count == -2) {
                AstNode *setter = (AstNode *) member->as.method_decl.base_args;
                member->as.method_decl.base_arg_count = 0;
                member->as.method_decl.base_args = NULL;
                setter->as.method_decl.base_arg_count = 0;

                XR_PARSE_PUSH(parser, methods, method_count, method_capacity, setter);
            }
        } else {
            XR_PARSE_PUSH(parser, fields, field_count, field_capacity, member);
        }
    }

    xr_parser_consume(parser, TK_RBRACE, "expected '}' to end struct body");
    int struct_end_line = parser->previous.line;
    int struct_end_column = parser->previous.column + 1;

    // Restore type_scope after parsing struct body
    if (type_param_count > 0) {
        parser->type_scope = saved_scope;
    }

    // Create struct declaration AST node
    AstNode *struct_node = xr_ast_struct_decl(parser->X, struct_name, fields, field_count, methods,
                                              method_count, line);
    struct_node->column = name_column;
    struct_node->end_line = struct_end_line;
    struct_node->end_column = struct_end_column;
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
    (void) is_getter;
    bool is_setter = false;
    (void) is_setter;
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

    int name_line = 0;
    int name_column = 0;
    int name_length = 0;

    if (xr_parser_match(parser, TK_CONSTRUCTOR)) {
        // 'constructor' keyword
        is_constructor = true;
        name = (char *) ast_alloc(parser->X, sizeof(XR_KEYWORD_CONSTRUCTOR));
        strcpy(name, XR_KEYWORD_CONSTRUCTOR);
        name_line = parser->previous.line;
        name_column = parser->previous.column;
        name_length = (int) (sizeof(XR_KEYWORD_CONSTRUCTOR) - 1);
    } else {
        // Normal name
        xr_parser_consume(parser, TK_NAME, "expected field or method name");
        name = token_to_string(parser, &parser->previous);
        name_line = parser->previous.line;
        name_column = parser->previous.column;
        name_length = parser->previous.length;
    }

    // Distinguish field and method: check for '(' or '<' (generic) or override modifier
    if (xr_parser_check(parser, TK_LPAREN) || xr_parser_check(parser, TK_LT) || is_constructor ||
        is_override) {
        // Method: has parameter list or generic type params or override modifier
        *is_method_out = true;
        AstNode *method = xr_parse_method_declaration(parser, name, name_line, name_column,
                                                      is_private, is_static, is_abstract);
        if (method)
            method->as.method_decl.is_final = is_final;
        return method;
    } else {
        // Field: has type annotation or initializer
        *is_method_out = false;

        // 'override' can only be used for methods
        if (is_override) {
            xr_parser_error_at_current(parser, "'override' modifier can only be used for methods");
        }

        // Parse type annotation (optional)
        XrTypeRef *field_type = NULL;
        if (xr_parser_match(parser, TK_COLON)) {
            // Use type annotation parser, supports all types and generic syntax
            field_type = xr_parse_type_annotation(parser);
        }

        // Check for property block { fn() {} fn(v) {} }
        if (xr_parser_check(parser, TK_LBRACE)) {
            // This is a property definition with getter/setter
            *is_method_out = true;
            return xr_parse_property_accessors(parser, name, field_type, is_private, is_static,
                                               line);
        }

        // Parse initializer expression (optional)
        AstNode *initializer = NULL;
        if (xr_parser_match(parser, TK_ASSIGN)) {
            initializer = xr_parse_expression(parser);
        }

        // Field declaration doesn't need semicolon (Xray doesn't use semicolons)

        AstNode *field = xr_ast_field_decl(parser->X, name, field_type, is_private, is_static,
                                           initializer, name_line);
        if (field) {
            field->as.field_decl.is_final = is_final;
            field->column = name_column;
            // End span: to end of initializer if present, else just the name.
            if (initializer && initializer->end_line > 0) {
                field->end_line = initializer->end_line;
                field->end_column = initializer->end_column;
            } else {
                field->end_line = name_line;
                field->end_column = name_column + name_length;
            }
        }
        return field;
    }
}

/* ========== Method Declaration Parsing ========== */

// Parse method declaration
// Syntax:
//   greet(name: string): void { ... }
//   constructor(x: int, y: int) { ... }
// @param name method name (already parsed by caller)
// @param name_line  1-indexed line of the identifier token
// @param name_column 1-indexed column of the identifier token
// @param is_private whether private
// @param is_static whether static
AstNode *xr_parse_method_declaration(Parser *parser, const char *name, int name_line,
                                     int name_column, bool is_private, bool is_static,
                                     bool is_abstract) {
    XR_DCHECK(parser != NULL, "parse_method_declaration: NULL parser");
    int line = name_line;

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
            XR_PARSE_PUSH(parser, type_param_names, type_param_count, type_param_capacity,
                          token_to_string(parser, &parser->previous));

            // Skip optional intersection constraint <T: Interface1 & Interface2 ...>
            // Method-level type-param constraints are not yet stored in the
            // method-decl AST, so we parse and discard for forward-compat parsing.
            if (xr_parser_match(parser, TK_COLON)) {
                int dummy = 0;
                xr_parse_constraint_list(parser, &dummy);
            }
        } while (xr_parser_match(parser, TK_COMMA));

        // Consume closing '>'
        xr_parser_consume(parser, TK_GT, "expected '>' after type parameters");
    }

    // Parse parameter list
    xr_parser_consume(parser, TK_LPAREN, "expected '(' to start parameter list");

    char **parameters = NULL;
    XrTypeRef **param_types = NULL;
    uint8_t *param_passing_modes = NULL;
    int param_count = 0;
    int param_capacity = 0;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            // Extend arrays (arena grow - old buffers are bulk-released)
            if (param_count >= param_capacity) {
                int old_capacity = param_capacity;
                param_capacity = param_capacity == 0 ? 4 : param_capacity * 2;

                char **_new_parameters =
                    (char **) ast_alloc_array(parser->X, sizeof(char *), (size_t) param_capacity);
                if (old_capacity > 0 && parameters) {
                    memcpy(_new_parameters, parameters, sizeof(char *) * (size_t) old_capacity);
                }
                parameters = _new_parameters;

                XrTypeRef **_new_param_types = (XrTypeRef **) ast_alloc_array(
                    parser->X, sizeof(XrTypeRef *), (size_t) param_capacity);
                if (old_capacity > 0 && param_types) {
                    memcpy(_new_param_types, param_types,
                           sizeof(XrTypeRef *) * (size_t) old_capacity);
                }
                param_types = _new_param_types;

                uint8_t *_new_modes = (uint8_t *) ast_alloc_array(parser->X, sizeof(uint8_t),
                                                                  (size_t) param_capacity);
                if (old_capacity > 0 && param_passing_modes) {
                    memcpy(_new_modes, param_passing_modes,
                           sizeof(uint8_t) * (size_t) old_capacity);
                }
                param_passing_modes = _new_modes;
            }

            // Parse parameter name
            xr_parser_consume(parser, TK_NAME, "expected parameter name");
            parameters[param_count] = token_to_string(parser, &parser->previous);
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

    // Parse return type (optional) — unified arrow `->` (task 082).
    XrTypeRef *return_type = NULL;
    if (xr_parser_match(parser, TK_ARROW)) {
        return_type = xr_parse_type_annotation(parser);
    } else if (xr_parser_check(parser, TK_COLON)) {
        xr_parser_advance(parser);
        xr_parser_error(parser, "use '->' instead of ':' for method return type");
        parser->panic_mode = 0;
        return_type = xr_parse_type_annotation(parser);
    }

    AstNode *body = NULL;
    if (is_abstract || parser->parsing_native_class) {
        // Abstract or @native method has no body, semicolon optional
        xr_parser_match(parser, TK_SEMICOLON);
    } else {
        // Normal method has body
        xr_parser_consume(parser, TK_LBRACE, "expected '{' to start method body");
        body = xr_parse_block(parser);
    }

    // Create method declaration node
    AstNode *method_node = xr_ast_method_decl(
        parser->X, name, parameters, param_types, param_count, return_type, body, is_constructor,
        is_static, is_private, false, false, line);  // is_getter, is_setter

    method_node->column = name_column;
    if (body && body->end_line > 0) {
        // Normal method: end is body's closing brace.
        method_node->end_line = body->end_line;
        method_node->end_column = body->end_column;
    } else {
        // Abstract method: no body — span is the identifier token.
        method_node->end_line = name_line;
        method_node->end_column = name_column + (int) strlen(name);
    }

    // Set whether this is an abstract method
    method_node->as.method_decl.is_abstract = is_abstract;
    method_node->as.method_decl.param_passing_modes = param_passing_modes;

    // Set generic type parameters
    method_node->as.method_decl.type_param_names = type_param_names;
    method_node->as.method_decl.type_param_count = type_param_count;

    return method_node;

fail:
    // Release the heap state we still own locally. XrType pointers come
    // from the type pool and are not freed here. `name` was heap-allocated
    // by our caller (xr_parse_field_declaration) but ownership is only
    // transferred on success; on failure we consume it here.
    if (type_param_names) {
        for (int i = 0; i < type_param_count; i++) {
        }
    }
    if (parameters) {
        for (int i = 0; i < param_count; i++) {
        }
    }
    return NULL;
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
        char *first_name = token_to_string(parser, &parser->previous);

        // Check if it's module.Class form
        if (xr_parser_match(parser, TK_DOT)) {
            // Is module.Class form
            module_name = first_name;

            if (xr_parser_match(parser, TK_NAME)) {
                class_name = token_to_string(parser, &parser->previous);
            } else {
                xr_parser_error_expected_name(parser, "expected class name");
                class_name = ast_strdup(parser->X, TYPE_NAME_NULL);
            }
        } else {
            // Just a normal class name
            class_name = first_name;
        }
    } else {
        // Support other built-in type names as new target
        xr_parser_error_expected_name(parser, "expected class name or type name");
        class_name = ast_strdup(parser->X, TYPE_NAME_NULL);
    }

    // Parse optional generic type parameters: new Box<int>(...)
    XrTypeRef *type_args[16];  // Max 16 type args
    int type_arg_count = 0;

    if (xr_parser_match(parser, TK_LT)) {
        do {
            if (type_arg_count >= 16)
                break;

            XrTypeRef *type = xr_parse_type_annotation(parser);
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
            XR_PARSE_PUSH(parser, arguments, arg_count, arg_capacity, xr_parse_expression(parser));
        } while (xr_parser_match(parser, TK_COMMA));
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')' to end argument list");

    // Create new expression node. xr_ast_new_expr takes ownership of
    // class_name / arguments / type_args, but deep-copies module_name,
    // so release our copy of module_name here.
    AstNode *node = xr_ast_new_expr(parser->X, module_name, class_name, arguments, arg_count,
                                    type_args, type_arg_count, line);
    return node;
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
        method_name = token_to_string(parser, &parser->previous);

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
            XR_PARSE_PUSH(parser, arguments, arg_count, arg_capacity, xr_parse_expression(parser));
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

    // Capture operator-token position for later LSP ranges.
    int name_line = parser->current.line;
    int name_column = parser->current.column;

    // Parse operator symbol
    XrTokenType op_token = parser->current.type;

    char *name = NULL;
    int expected_params = 1;  // Most are binary operators, need 1 parameter (the other operand)

    // Determine operator and op_type based on token type
    OperatorType op_type_val;
    switch (op_token) {
        // Arithmetic operators
        case TK_PLUS:
            name = ast_strdup(parser->X, "+");
            op_type_val = OPTYPE_ADD;
            break;
        case TK_MINUS:
            name = ast_strdup(parser->X, "-");
            op_type_val = OPTYPE_SUB;  // default to binary, adjusted later based on param count
            break;
        case TK_STAR:
            name = ast_strdup(parser->X, "*");
            op_type_val = OPTYPE_MUL;
            break;
        case TK_SLASH:
            name = ast_strdup(parser->X, "/");
            op_type_val = OPTYPE_DIV;
            break;
        case TK_PERCENT:
            name = ast_strdup(parser->X, "%");
            op_type_val = OPTYPE_MOD;
            break;
        // Bitwise operators
        case TK_AMP:
            name = ast_strdup(parser->X, "&");
            op_type_val = OPTYPE_BAND;
            break;
        case TK_PIPE:
            name = ast_strdup(parser->X, "|");
            op_type_val = OPTYPE_BOR;
            break;
        case TK_CARET:
            name = ast_strdup(parser->X, "^");
            op_type_val = OPTYPE_BXOR;
            break;
        case TK_TILDE:
            name = ast_strdup(parser->X, "~");
            op_type_val = OPTYPE_UNARY;  // unary operator
            expected_params = 0;         // unary operator needs no extra params
            break;
        case TK_NOT:
            name = ast_strdup(parser->X, "!");
            op_type_val = OPTYPE_UNARY;  // unary operator
            expected_params = 0;         // unary operator needs no extra params
            break;
        // Comparison operators
        case TK_EQ:
            name = ast_strdup(parser->X, "==");
            op_type_val = OPTYPE_EQ;
            break;
        case TK_NE:
            name = ast_strdup(parser->X, "!=");
            op_type_val = OPTYPE_NE;
            break;
        case TK_LT:
            name = ast_strdup(parser->X, "<");
            op_type_val = OPTYPE_LT;
            break;
        case TK_LE:
            name = ast_strdup(parser->X, "<=");
            op_type_val = OPTYPE_LE;
            break;
        case TK_GT:
            name = ast_strdup(parser->X, ">");
            op_type_val = OPTYPE_GT;
            break;
        case TK_GE:
            name = ast_strdup(parser->X, ">=");
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
                name = ast_strdup(parser->X, "[]=");
                expected_params = 2;  // []= needs 2 params: index + value
                op_type_val = OPTYPE_SUBSCRIPT_SET;
            } else {
                name = ast_strdup(parser->X, "[]");
                expected_params = 1;  // [] needs 1 param: index
                op_type_val = OPTYPE_SUBSCRIPT;
            }
            break;

        // Shift operators
        case TK_LSHIFT:
            name = ast_strdup(parser->X, "<<");
            op_type_val = OPTYPE_BAND;  // reuse bitwise type
            break;
        case TK_RSHIFT:
            name = ast_strdup(parser->X, ">>");
            op_type_val = OPTYPE_BAND;  // reuse bitwise type
            break;

        // Compound assignment operators
        case TK_PLUS_ASSIGN:
            name = ast_strdup(parser->X, "+=");
            op_type_val = OPTYPE_ADD;  // reuse add type
            break;
        case TK_MINUS_ASSIGN:
            name = ast_strdup(parser->X, "-=");
            op_type_val = OPTYPE_SUB;  // reuse sub type
            break;
        case TK_MUL_ASSIGN:
            name = ast_strdup(parser->X, "*=");
            op_type_val = OPTYPE_MUL;  // reuse mul type
            break;
        case TK_DIV_ASSIGN:
            name = ast_strdup(parser->X, "/=");
            op_type_val = OPTYPE_DIV;  // reuse div type
            break;
        case TK_MOD_ASSIGN:
            name = ast_strdup(parser->X, "%=");
            op_type_val = OPTYPE_MOD;  // reuse mod type
            break;
        case TK_AND_ASSIGN:
            name = ast_strdup(parser->X, "&=");
            op_type_val = OPTYPE_BAND;  // reuse bitwise and type
            break;
        case TK_OR_ASSIGN:
            name = ast_strdup(parser->X, "|=");
            op_type_val = OPTYPE_BOR;  // reuse bitwise or type
            break;
        case TK_XOR_ASSIGN:
            name = ast_strdup(parser->X, "^=");
            op_type_val = OPTYPE_BXOR;  // reuse bitwise xor type
            break;
        case TK_LSHIFT_ASSIGN:
            name = ast_strdup(parser->X, "<<=");
            op_type_val = OPTYPE_BAND;  // reuse bitwise type
            break;
        case TK_RSHIFT_ASSIGN:
            name = ast_strdup(parser->X, ">>=");
            op_type_val = OPTYPE_BAND;  // reuse bitwise type
            break;

        // Increment/decrement operators
        case TK_INC:
            name = ast_strdup(parser->X, "++");
            op_type_val = OPTYPE_UNARY;  // treat as unary operator
            break;
        case TK_DEC:
            name = ast_strdup(parser->X, "--");
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
    XrTypeRef **param_types = NULL;
    int param_count = 0;

    // Parse parameters: most operators need 1, []= needs 2, unary operators need 0
    if (xr_parser_check(parser, TK_RPAREN)) {
        if (expected_params > 0 && op_token != TK_MINUS && op_token != TK_NOT &&
            op_token != TK_INC && op_token != TK_DEC) {
            xr_parser_error(parser, "operator method requires parameters");
            goto fail;
        }
        // Unary operators (expected_params == 0) can have no parameters
        xr_parser_consume(parser, TK_RPAREN, "expected ')' to end parameter list");
        parameters = NULL;
        param_types = NULL;
        param_count = 0;
        // Go directly to return type parsing, no goto needed
    } else {
        // Allocate parameter arrays in the parse arena
        parameters = (char **) ast_alloc_array(parser->X, sizeof(char *), (size_t) expected_params);
        param_types = (XrTypeRef **) ast_alloc_array(parser->X, sizeof(XrTypeRef *),
                                                     (size_t) expected_params);

        // Parse first parameter
        xr_parser_consume(parser, TK_NAME, "expected parameter name");
        parameters[0] = token_to_string(parser, &parser->previous);

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
                goto fail;
            }

            // Parse second parameter
            xr_parser_consume(parser, TK_NAME, "expected parameter name");
            parameters[1] = token_to_string(parser, &parser->previous);

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
            goto fail;
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

    // Parse return type (optional) — unified arrow `->` (task 082).
    XrTypeRef *return_type = NULL;
    if (xr_parser_match(parser, TK_ARROW)) {
        return_type = xr_parse_type_annotation(parser);
    } else if (xr_parser_check(parser, TK_COLON)) {
        xr_parser_advance(parser);
        xr_parser_error(parser, "use '->' instead of ':' for method return type");
        parser->panic_mode = 0;
        return_type = xr_parse_type_annotation(parser);
    }

    // Parse method body
    xr_parser_consume(parser, TK_LBRACE, "expected '{' to start method body");
    AstNode *body = xr_parse_block(parser);

    // Create method node
    AstNode *method =
        xr_ast_method_decl(parser->X, name, parameters, param_types, param_count, return_type, body,
                           false,  // is_constructor
                           is_static, is_private,
                           false,  // is_getter
                           false,  // is_setter
                           line);

    method->column = name_column;
    if (body && body->end_line > 0) {
        method->end_line = body->end_line;
        method->end_column = body->end_column;
    } else {
        method->end_line = name_line;
        method->end_column = name_column + (int) strlen(name);
    }

    // Set operator flags
    method->as.method_decl.is_operator = true;     // mark as operator method
    method->as.method_decl.op_type = op_type_val;  // set specific operator type

    return method;

fail:
    // Release the operator-method state we still own. XrType entries are
    // shared via the type pool and are NOT freed here.
    if (parameters) {
        for (int i = 0; i < param_count; i++) {
        }
    }
    return NULL;
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
static AstNode *xr_parse_property_accessors(Parser *parser, const char *name, XrTypeRef *field_type,
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
        XrTypeRef **param_types = NULL;
        int param_count = 0;

        if (!xr_parser_check(parser, TK_RPAREN)) {
            // Has parameters = setter
            parameters = (char **) ast_alloc(parser->X, sizeof(char *));
            param_types = (XrTypeRef **) ast_alloc(parser->X, sizeof(XrTypeRef *));

            xr_parser_consume(parser, TK_NAME, "expected parameter name");
            parameters[0] = token_to_string(parser, &parser->previous);

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

        // Parse return type (optional) — unified arrow `->` (task 082).
        XrTypeRef *return_type = NULL;
        if (xr_parser_match(parser, TK_ARROW)) {
            return_type = xr_parse_type_annotation(parser);
        } else if (xr_parser_check(parser, TK_COLON)) {
            xr_parser_advance(parser);
            xr_parser_error(parser, "use '->' instead of ':' for accessor return type");
            parser->panic_mode = 0;
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
        char *method_name = (char *) ast_alloc(parser->X, name_len);
        snprintf(method_name, name_len, "%s:%s", is_getter ? "get" : "set", name);

        // Create method declaration node
        AstNode *method_node = xr_ast_method_decl(parser->X, method_name, parameters, param_types,
                                                  param_count, return_type, body, false, is_static,
                                                  is_private, is_getter, !is_getter, line);

        method_node->column = 1;  // property accessors are synthetic — column
                                  //   mirrors the declaration line (safe 1)
        if (body && body->end_line > 0) {
            method_node->end_line = body->end_line;
            method_node->end_column = body->end_column;
        } else {
            method_node->end_line = line;
            method_node->end_column = 1 + (int) strlen(method_name);
        }

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
    if (getter_node == NULL)
        return setter_node;
    if (setter_node == NULL)
        return getter_node;

    // Both present, link setter to getter's next pointer
    // Note: this needs special handling in class parsing
    getter_node->as.method_decl.base_arg_count = -2;  // special mark: has paired setter
    setter_node->as.method_decl.base_arg_count = -1;  // special mark: this is paired setter

    // Store setter in getter's base_args (temporarily borrowed)
    getter_node->as.method_decl.base_args = (AstNode **) setter_node;

    return getter_node;
}

/* ========== Interface Declaration Parsing ========== */

// Parse interface declaration
// Syntax:
//   interface Iterable<T> [extends Shape, Colorful] {
//       iterator(): Iterator<T>
//       length: int          // property signature
//   }
AstNode *xr_parse_interface_declaration(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_interface_declaration: NULL parser");
    int line = parser->previous.line;

    // 'interface' keyword already consumed

    // Parse interface name
    xr_parser_consume(parser, TK_NAME, "expected interface name");
    char *interface_name = token_to_string(parser, &parser->previous);
    int name_column = parser->previous.column;

    // Parse generic type parameters <T, U: Constraint> — mirrors class parsing
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

    // Register generic type params in type_scope so member type annotations
    // such as `iterator(): Iterator<T>` recognise T as a type parameter.
    XrTypeScope *saved_scope = parser->type_scope;
    if (type_param_count > 0) {
        XrTypeScope *generic_scope = xr_type_scope_new(parser->type_scope);
        for (int i = 0; i < type_param_count; i++) {
            XrTypeRef *tp = xr_tref_type_param(parser->X, type_params[i]->name);
            xr_type_scope_define(generic_scope, type_params[i]->name, tp);
        }
        parser->type_scope = generic_scope;
    }

    // Parse extends clause (optional, interface can extend multiple interfaces).
    // Use full type-reference parser so `extends Pair<K, V>` works.
    XrTypeRef **extends = NULL;
    int extends_count = 0;
    int extends_capacity = 0;

    if (xr_parser_match(parser, TK_EXTENDS)) {
        do {
            XrTypeRef *parent_ref = xr_parse_type_annotation(parser);
            if (!parent_ref)
                break;
            XR_PARSE_PUSH(parser, extends, extends_count, extends_capacity, parent_ref);
        } while (xr_parser_match(parser, TK_COMMA));
    }

    // Parse interface body
    xr_parser_consume(parser, TK_LBRACE, "expected '{' to start interface body");

    // Collect method and property signatures into separate arrays so consumers
    // can iterate them with a static guarantee about node kind (mirrors
    // ClassDeclNode's fields[] / methods[] split).
    AstNode **methods = NULL;
    int method_count = 0;
    int method_capacity = 0;
    AstNode **properties = NULL;
    int property_count = 0;
    int property_capacity = 0;

    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        // Error recovery to avoid infinite loop
        if (parser->panic_mode) {
            xr_parser_synchronize(parser);
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_EOF))
                break;
            continue;
        }

        AstNode *member = xr_parse_interface_member(parser);
        if (!member) {
            // Skip to next member or end of interface
            if (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
                xr_parser_advance(parser);
            }
            continue;
        }

        if (member->type == AST_INTERFACE_PROPERTY) {
            XR_PARSE_PUSH(parser, properties, property_count, property_capacity, member);
        } else {
            XR_PARSE_PUSH(parser, methods, method_count, method_capacity, member);
        }
    }

    xr_parser_consume(parser, TK_RBRACE, "expected '}' to end interface body");
    int if_end_line = parser->previous.line;
    int if_end_column = parser->previous.column + 1;

    // Restore type_scope after parsing interface body
    if (type_param_count > 0) {
        parser->type_scope = saved_scope;
    }

    // Create interface declaration AST node
    AstNode *node = xr_ast_interface_decl(parser->X, interface_name, extends, extends_count,
                                          methods, method_count, properties, property_count,
                                          type_params, type_param_count, line);
    node->column = name_column;
    node->end_line = if_end_line;
    node->end_column = if_end_column;
    return node;
}

// Parse one interface member: either a method signature or a property signature.
// Method:    name(params): retType
// Property:  [const] name: type
//
// The optional `const` prefix marks a read-only property, mirroring the way
// object-type fields tolerate `const` in xparse_type.c.  All forms allow an
// optional trailing semicolon.
AstNode *xr_parse_interface_member(Parser *parser) {
    // Optional `const` modifier — only valid for property signatures.
    bool is_readonly = xr_parser_match(parser, TK_CONST);

    // Parse member name
    xr_parser_consume(parser, TK_NAME, "expected method or property name");
    char *member_name = token_to_string(parser, &parser->previous);
    int member_line = parser->previous.line;

    // Property signature: `name: type`
    if (xr_parser_check(parser, TK_COLON)) {
        xr_parser_advance(parser);  // consume ':'
        XrTypeRef *prop_type = xr_parse_type_annotation(parser);
        xr_parser_match(parser, TK_SEMICOLON);  // optional terminator
        return xr_ast_interface_property(parser->X, member_name, prop_type, is_readonly,
                                         member_line);
    }

    if (is_readonly) {
        xr_parser_error_at_current(parser, "'const' modifier only applies to property signatures");
    }

    // Method signature: `name(params): retType`
    xr_parser_consume(parser, TK_LPAREN, "expected '(' or ':' after interface member name");

    char **parameters = NULL;
    XrTypeRef **param_types = NULL;
    int param_count = 0;
    int param_capacity = 0;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            // Parameter name
            xr_parser_consume(parser, TK_NAME, "expected parameter name");
            char *param_name = token_to_string(parser, &parser->previous);

            // Parameter type (required in interface method signatures)
            xr_parser_consume(parser, TK_COLON, "expected ':'");

            XrTypeRef *param_type = xr_parse_type_annotation(parser);

            // Add to parameters array (arena grow - old buffers released by arena)
            if (param_count >= param_capacity) {
                int old_capacity = param_capacity;
                param_capacity = param_capacity == 0 ? 4 : param_capacity * 2;

                char **_new_parameters =
                    (char **) ast_alloc_array(parser->X, sizeof(char *), (size_t) param_capacity);
                if (old_capacity > 0 && parameters) {
                    memcpy(_new_parameters, parameters, sizeof(char *) * (size_t) old_capacity);
                }
                parameters = _new_parameters;

                XrTypeRef **_new_param_types = (XrTypeRef **) ast_alloc_array(
                    parser->X, sizeof(XrTypeRef *), (size_t) param_capacity);
                if (old_capacity > 0 && param_types) {
                    memcpy(_new_param_types, param_types,
                           sizeof(XrTypeRef *) * (size_t) old_capacity);
                }
                param_types = _new_param_types;
            }
            parameters[param_count] = param_name;
            param_types[param_count] = param_type;
            param_count++;

        } while (xr_parser_match(parser, TK_COMMA));
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')'");

    // Parse return type (optional) — unified arrow `->` (task 082).
    XrTypeRef *return_type = NULL;
    if (xr_parser_match(parser, TK_ARROW)) {
        return_type = xr_parse_type_annotation(parser);
    } else if (xr_parser_check(parser, TK_COLON)) {
        xr_parser_advance(parser);
        xr_parser_error(parser, "use '->' instead of ':' for interface method return type");
        parser->panic_mode = 0;
        return_type = xr_parse_type_annotation(parser);
    }

    // Interface method signature ends with semicolon (optional)
    xr_parser_match(parser, TK_SEMICOLON);

    // Create interface method signature node. xr_ast_interface_method
    // takes ownership of member_name, parameters[] and param_types[].
    return xr_ast_interface_method(parser->X, member_name, parameters, param_types, param_count,
                                   return_type, member_line);

fail:
    // On OOM we still own member_name and any accumulated parameter names.
    // XrType pointers are shared via the type pool and are not freed here.
    if (parameters) {
        for (int i = 0; i < param_count; i++) {
        }
    }
    return NULL;
}

/* ========== Enum Declaration Parsing ========== */

/* Build a type ref from an already-consumed TK_NAME token.
 * Handles generic type args (Name<T, U>) and type scope resolution. */
static XrTypeRef *build_type_from_consumed_name(Parser *parser, Token *name_tok) {
    char temp_name[256];
    int name_len = name_tok->length < 255 ? name_tok->length : 255;
    memcpy(temp_name, name_tok->start, (size_t) name_len);
    temp_name[name_len] = '\0';

    XrTypeRef *result = NULL;
    if (xr_parser_match(parser, TK_LT)) {
        /* Generic: Name<T1, T2, ...> */
        XrTypeRef *type_args[16];
        int type_arg_count = 0;
        do {
            if (type_arg_count < 16)
                type_args[type_arg_count++] = xr_parse_type_annotation(parser);
        } while (xr_parser_match(parser, TK_COMMA));
        xr_parser_consume(parser, TK_GT, "expected '>' in generic type");
        result = xr_tref_generic(parser->X, temp_name, type_args, type_arg_count);
    } else if (parser->type_scope) {
        XrTypeRef *alias = xr_type_scope_resolve(parser->type_scope, temp_name);
        result = alias ? alias : xr_tref_named(parser->X, temp_name);
    } else {
        result = xr_tref_named(parser->X, temp_name);
    }

    /* Handle trailing '?' for optional */
    if (result && xr_parser_match(parser, TK_QUESTION)) {
        result = xr_tref_optional(parser->X, result);
    }
    return result;
}

/* Parse ADT variant payload: '(' FieldList ')'.
 * FieldList ::= Field (',' Field)*
 * Field     ::= (Name ':')? Type
 *
 * Strategy: consume the leading TK_NAME; if ':' follows, it was a field
 * name; otherwise it was the start of a type and we build the type ref
 * from the consumed name. This avoids lexer-position rewind issues. */
static void parse_enum_variant_payload(Parser *parser, char ***out_names, XrTypeRef ***out_types,
                                       int *out_count) {
    XR_DCHECK(parser != NULL, "parse_variant_payload: NULL parser");

    char **names = NULL;
    XrTypeRef **types = NULL;
    int count = 0;
    int name_capacity = 0;

    do {
        if (xr_parser_check(parser, TK_RPAREN))
            break;

        char *field_name = NULL;
        XrTypeRef *field_type = NULL;

        if (xr_parser_check(parser, TK_NAME)) {
            /* Consume the name and decide based on what follows. */
            Token name_tok = parser->current;
            xr_parser_advance(parser);

            if (xr_parser_match(parser, TK_COLON)) {
                /* Named field: 'name: Type' */
                field_name = token_to_string(parser, &name_tok);
                field_type = xr_parse_type_annotation(parser);
            } else {
                /* Positional: the consumed name was the type itself. */
                field_type = build_type_from_consumed_name(parser, &name_tok);
            }
        } else {
            /* Keyword type (int, string, etc.) — parse normally. */
            field_type = xr_parse_type_annotation(parser);
        }

        if (!field_type) {
            xr_parser_error(parser, "expected type in variant payload");
            break;
        }

        /* Grow name and type arrays in lockstep. */
        XR_PARSE_PUSH(parser, names, count, name_capacity, field_name);
        {
            XrTypeRef **new_types = (XrTypeRef **) ast_alloc_array(
                parser->X, sizeof(XrTypeRef *), (size_t) (count > 4 ? count * 2 : 8));
            if (types) {
                for (int i = 0; i < count - 1; i++)
                    new_types[i] = types[i];
            }
            new_types[count - 1] = field_type;
            types = new_types;
        }
    } while (xr_parser_match(parser, TK_COMMA));

    *out_names = names;
    *out_types = types;
    *out_count = count;
}

/* Parse one enum method: 'fn' Name '(' params ')' ReturnType? Block.
 * Enum methods require 'fn' keyword (unlike class methods). */
static AstNode *parse_enum_method(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_enum_method: NULL parser");

    /* 'fn' already consumed by caller */
    xr_parser_consume(parser, TK_NAME, "expected method name after 'fn'");
    char *name = token_to_string(parser, &parser->previous);
    int name_line = parser->previous.line;
    int name_col = parser->previous.column;

    return xr_parse_method_declaration(parser, name, name_line, name_col,
                                       /* is_private */ false,
                                       /* is_static */ false,
                                       /* is_abstract */ false);
}

// Parse enum declaration (simple or ADT)
// Syntax:
//   enum Color { Red, Green, Blue }
//   enum Status : int { Success = 200, Error = 500 }
//   enum Result<T, E> { Ok(T), Err(E)  fn isOk() -> bool { ... } }
//   enum Shape implements Printable { Circle(float), Rect(float, float) }
AstNode *xr_parse_enum_declaration(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_enum_declaration: NULL parser");
    int line = parser->previous.line;

    // 'enum' keyword already consumed

    // Parse enum name
    xr_parser_consume(parser, TK_NAME, "expected enum name");
    char *enum_name = token_to_string(parser, &parser->previous);
    int name_column = parser->previous.column;

    // Parse optional generic type parameters <T, E: Constraint>
    XrGenericParam **type_params = NULL;
    int type_param_count = 0;
    int type_param_capacity = 0;

    if (xr_parser_match(parser, TK_LT)) {
        do {
            xr_parser_consume(parser, TK_NAME, "expected type parameter name");
            char *param_name = token_to_string(parser, &parser->previous);

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

        xr_parser_consume(parser, TK_GT, "expected '>' after type parameters");
    }

    // Register generic type params in type_scope for payload type parsing.
    XrTypeScope *saved_scope = parser->type_scope;
    if (type_param_count > 0) {
        XrTypeScope *generic_scope = xr_type_scope_new(parser->type_scope);
        for (int i = 0; i < type_param_count; i++) {
            XrTypeRef *tp = xr_tref_type_param(parser->X, type_params[i]->name);
            xr_type_scope_define(generic_scope, type_params[i]->name, tp);
        }
        parser->type_scope = generic_scope;
    }

    // Parse type hint (optional, simple enums only): ': int'
    // Mutually exclusive with generic type params.
    char *type_hint = NULL;
    if (xr_parser_match(parser, TK_COLON)) {
        if (type_param_count > 0) {
            xr_parser_error(parser,
                            "ADT enum with type parameters cannot have a backing type hint");
        }
        if (xr_parser_match(parser, TK_INT)) {
            type_hint = ast_strdup(parser->X, TYPE_NAME_INT);
        } else if (xr_parser_match(parser, TK_STRING)) {
            type_hint = ast_strdup(parser->X, TYPE_NAME_STRING);
        } else if (xr_parser_match(parser, TK_FLOAT)) {
            type_hint = ast_strdup(parser->X, TYPE_NAME_FLOAT);
        } else if (xr_parser_match(parser, TK_BOOL)) {
            type_hint = ast_strdup(parser->X, TYPE_NAME_BOOL);
        } else if (xr_parser_match(parser, TK_NAME)) {
            Token t = parser->previous;
            if (t.length == 3 && memcmp(t.start, "int", 3) == 0) {
                type_hint = ast_strdup(parser->X, TYPE_NAME_INT);
            } else if (t.length == 6 && memcmp(t.start, "string", 6) == 0) {
                type_hint = ast_strdup(parser->X, TYPE_NAME_STRING);
            } else if (t.length == 5 && memcmp(t.start, "float", 5) == 0) {
                type_hint = ast_strdup(parser->X, TYPE_NAME_FLOAT);
            } else if (t.length == 4 && memcmp(t.start, "bool", 4) == 0) {
                type_hint = ast_strdup(parser->X, TYPE_NAME_BOOL);
            } else {
                xr_parser_error(parser, "enum type must be int, string, float or bool");
            }
        } else {
            xr_parser_error(parser, "enum type must be int, string, float or bool");
        }
    }

    // Parse optional 'implements' clause
    XrTypeRef **interfaces = NULL;
    int interface_count = 0;
    int interface_capacity = 0;

    if (xr_parser_match(parser, TK_IMPLEMENTS)) {
        do {
            XrTypeRef *iface_ref = xr_parse_type_annotation(parser);
            if (!iface_ref)
                break;
            XR_PARSE_PUSH(parser, interfaces, interface_count, interface_capacity, iface_ref);
        } while (xr_parser_match(parser, TK_COMMA));
    }

    // Parse enum body
    xr_parser_consume(parser, TK_LBRACE, "expected '{' to start enum body");

    // Collect variants and methods
    AstNode **members = NULL;
    int member_count = 0;
    int member_capacity = 0;

    AstNode **methods = NULL;
    int method_count = 0;
    int method_capacity = 0;

    if (xr_parser_check(parser, TK_RBRACE)) {
        xr_parser_error(parser, "enum requires at least one variant");
    }

    /* Variant phase: parse comma-separated variants until we hit 'fn' or '}'. */
    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF) &&
           !xr_parser_check(parser, TK_FN)) {
        if (parser->panic_mode) {
            xr_parser_synchronize(parser);
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_EOF))
                break;
            continue;
        }

        if (!xr_parser_check(parser, TK_NAME)) {
            xr_parser_error_expected_name(parser, "expected enum variant name");
            xr_parser_advance(parser);
            continue;
        }

        xr_parser_consume(parser, TK_NAME, "expected enum variant name");
        char *member_name = token_to_string(parser, &parser->previous);
        int member_line = parser->previous.line;
        int member_col = parser->previous.column;
        int member_name_len = parser->previous.length;

        /* ADT payload: Variant '(' fields ')' */
        char **payload_names = NULL;
        XrTypeRef **payload_types = NULL;
        int payload_count = 0;
        AstNode *member_value = NULL;

        if (xr_parser_match(parser, TK_LPAREN)) {
            /* ADT variant with payload */
            parse_enum_variant_payload(parser, &payload_names, &payload_types, &payload_count);
            xr_parser_consume(parser, TK_RPAREN, "expected ')' after variant payload");
        } else if (xr_parser_match(parser, TK_ASSIGN)) {
            /* Simple enum with backing value: Name = expr */
            member_value = xr_parse_expression(parser);
        }

        AstNode *member = xr_ast_enum_member(parser->X, member_name, member_value, payload_names,
                                             payload_types, payload_count, member_line);
        member->column = member_col;
        if (member_value && member_value->end_line > 0) {
            member->end_line = member_value->end_line;
            member->end_column = member_value->end_column;
        } else {
            member->end_line = member_line;
            member->end_column = member_col + member_name_len;
        }

        XR_PARSE_PUSH(parser, members, member_count, member_capacity, member);

        /* Comma after variant: required between variants, optional before
         * '}' or 'fn'. */
        if (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_FN)) {
            if (!xr_parser_match(parser, TK_COMMA)) {
                xr_parser_error(parser, "expected ',' between enum variants");
                break;
            }
            /* Allow trailing comma before '}' or 'fn' */
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_FN))
                break;
        }
    }

    /* Method phase: parse 'fn' methods until '}'. */
    while (xr_parser_match(parser, TK_FN)) {
        if (parser->panic_mode) {
            xr_parser_synchronize(parser);
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_EOF))
                break;
            continue;
        }

        AstNode *method = parse_enum_method(parser);
        if (method) {
            XR_PARSE_PUSH(parser, methods, method_count, method_capacity, method);
        }
    }

    xr_parser_consume(parser, TK_RBRACE, "expected '}' to end enum body");
    int enum_end_line = parser->previous.line;
    int enum_end_column = parser->previous.column + 1;

    // Restore type_scope after parsing enum body
    if (type_param_count > 0) {
        parser->type_scope = saved_scope;
    }

    AstNode *node = xr_ast_enum_decl(parser->X, enum_name, type_hint, members, member_count,
                                     methods, method_count, type_params, type_param_count,
                                     interfaces, interface_count, line);
    node->column = name_column;
    node->end_line = enum_end_line;
    node->end_column = enum_end_column;
    return node;
}

/* ========== Static Constructor Parsing ========== */

// Parse static constructor
// Syntax:
//   static constructor() {
//       // class-level initialization code
//   }
// @param is_private whether private (usually not allowed for static constructor, but parameter
// kept)
AstNode *xr_parse_static_constructor(Parser *parser, bool is_private) {
    int line = parser->previous.line;
    int name_column = parser->previous.column;  // column of 'constructor' keyword

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
            xr_parser_check(parser, TK_BOOL)) {
            xr_parser_advance(parser);
        }
    }

    // Parse method body
    xr_parser_consume(parser, TK_LBRACE, "expected '{' to start static constructor body");
    AstNode *body = xr_parse_block(parser);

    // Create method declaration node
    AstNode *method_node =
        xr_ast_method_decl(parser->X, "<clinit>", NULL, NULL, 0,  // no parameters
                           NULL,                                  // no return type
                           body,
                           false,  // not a regular constructor
                           true,   // is static
                           is_private, false, false, line);

    method_node->column = name_column;
    if (body && body->end_line > 0) {
        method_node->end_line = body->end_line;
        method_node->end_column = body->end_column;
    } else {
        method_node->end_line = line;
        method_node->end_column = name_column + (int) strlen("<clinit>");
    }

    // Mark as static constructor
    method_node->as.method_decl.is_static_constructor = true;

    return method_node;
}
