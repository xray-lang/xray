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

static void attach_leading_trivia_to_member(AstNode *member, XrTrivia *leading_trivia) {
    if (!leading_trivia)
        return;
    if (member && !member->leading_comments) {
        member->leading_comments = leading_trivia;
    } else {
        (void) (leading_trivia);
    }
}

static void attach_trailing_trivia_to_member(Parser *parser, AstNode *member) {
    if (!xr_parser_healthy(parser)) return;
    if (!member || !parser->previous.trailing_trivia)
        return;
    if (!member->trailing_comments) {
        member->trailing_comments = parser->previous.trailing_trivia;
        parser->previous.trailing_trivia = NULL;
    }
}

static uint32_t xr_parse_struct_align_clause(Parser *parser) {
    if (!xr_parser_healthy(parser)) return 0;
    if (!xr_parser_match_name(parser, "align"))
        return 0;
    do {
        xr_parser_consume(parser, TK_LPAREN, "expected '(' after align");
        if (!xr_parser_healthy(parser)) return 0;
    } while (0);
    uint32_t result = 0;
    if (xr_parser_check(parser, TK_LITERAL_INT)) {
        Token n = parser->current;
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return 0;
        } while (0);
        ParsedIntLiteral value = xr_parser_integer_literal(parser, n.start, n.length);
        if (!xr_parser_healthy(parser)) return 0;
        uint64_t parsed = value.bits;
        if (value.overflows_u64 || parsed > UINT32_MAX) {
            do {
                xr_parser_error(parser, "align value is too large");
                if (!xr_parser_healthy(parser)) return 0;
            } while (0);
        } else {
            result = (uint32_t) parsed;
        }
    } else {
        do {
            xr_parser_error(parser, "align requires an integer, e.g. align(64)");
            if (!xr_parser_healthy(parser)) return 0;
        } while (0);
    }
    do {
        xr_parser_consume(parser, TK_RPAREN, "expected ')' to close align clause");
        if (!xr_parser_healthy(parser)) return 0;
    } while (0);
    return result;
}

static bool current_is_removed_public_modifier(Parser *parser) {
    if (!xr_parser_healthy(parser)) return false;
    if (!xr_parser_check_name(parser, "public"))
        return false;

    Scanner saved;
    if (xr_compile_state_copy(parser->state, &saved, &parser->scanner, sizeof(saved)) != XR_COMPILE_RESOURCE_OK) return false;
    Token peek = xr_scanner_scan(&saved);
    if (!xr_parser_healthy(parser)) return false;
    return peek.type == TK_NAME || peek.type == TK_STATIC || peek.type == TK_PRIVATE ||
           peek.type == TK_PROTECTED || peek.type == TK_CONST || peek.type == TK_CONSTRUCTOR ||
           peek.type == TK_OPERATOR || peek.type == TK_FINAL;
}

static bool current_is_removed_oop_modifier(Parser *parser, const char **out_name) {
    if (!xr_parser_healthy(parser)) return false;
    const char *name = NULL;
    if (xr_parser_check_name(parser, "open")) {
        name = "open";
    } else if (xr_parser_check_name(parser, "virtual")) {
        name = "virtual";
    } else {
        return false;
    }

    Scanner saved;
    if (xr_compile_state_copy(parser->state, &saved, &parser->scanner, sizeof(saved)) != XR_COMPILE_RESOURCE_OK) return false;
    Token peek = xr_scanner_scan(&saved);
    if (!xr_parser_healthy(parser)) return false;
    if (peek.type != TK_NAME && peek.type != TK_STATIC && peek.type != TK_PRIVATE &&
        peek.type != TK_PROTECTED && peek.type != TK_CONSTRUCTOR && peek.type != TK_OPERATOR)
        return false;
    if (out_name)
        *out_name = name;
    return true;
}

static bool current_can_start_class_member_after_recovery(Parser *parser) {
    if (!xr_parser_healthy(parser)) return false;
    return xr_parser_check(parser, TK_NAME) || xr_parser_check(parser, TK_PRIVATE) ||
           xr_parser_check(parser, TK_PROTECTED) || xr_parser_check(parser, TK_CONST) ||
           xr_parser_check(parser, TK_STATIC) || xr_parser_check(parser, TK_CONSTRUCTOR) ||
           xr_parser_check(parser, TK_FINAL) || xr_parser_check(parser, TK_OPERATOR);
}

static AstNode *reject_removed_member_modifier(Parser *parser, bool *is_method_out,
                                               const char *message) {
    if (!xr_parser_healthy(parser)) return NULL;
    int modifier_line = parser->current.line;
    do {
        xr_parser_error_at_current(parser, message);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    do {
        xr_parser_skip_invalid_construct(parser, modifier_line,
                                     current_can_start_class_member_after_recovery, true);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    if (is_method_out)
        *is_method_out = false;
    return NULL;
}

/* ========== Class Declaration Parsing ========== */

// Parse class declaration
// Syntax: class Dog extends Animal { field_declarations... method_declarations... }
AstNode *xr_parse_class_declaration(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_class_declaration: NULL parser");
    int line = parser->previous.line;

    // 'class' keyword already consumed

    // Parse class name
    do {
        xr_parser_consume(parser, TK_NAME, "expected class name");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    char *class_name = xr_parser_token_string(parser, &parser->previous);
    if (!xr_parser_healthy(parser)) return NULL;
    int name_column = parser->previous.column;

    /* Parse generic type parameters <T, U: Constraint, V = int>. The helper
     * installs a scope holding them so they resolve in fields and methods. */
    XrTypeScope *saved_scope = parser->type_scope;
    int type_param_count = 0;
    XrGenericParam **type_params = xr_parse_generic_params(parser, &type_param_count);
    if (!xr_parser_healthy(parser)) return NULL;
    XrTypeScope *generic_scope = type_param_count > 0 ? parser->type_scope : NULL;

    // Parse extends clause (optional)
    // Supports: extends Class or extends module.Class
    char *super_name = NULL;
    char *super_module = NULL;
    if (xr_parser_match(parser, TK_EXTENDS)) {
        do {
            xr_parser_consume(parser, TK_NAME, "expected superclass name");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        char *first_name = xr_parser_token_string(parser, &parser->previous);
        if (!xr_parser_healthy(parser)) return NULL;

        // Check for module.Class form
        if (xr_parser_match(parser, TK_DOT)) {
            do {
                xr_parser_consume(parser, TK_NAME, "expected class name after '.'");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            super_module = first_name;
            do {
                super_name = xr_parser_token_string(parser, &parser->previous);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
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
            if (!xr_parser_healthy(parser)) return NULL;
            if (!iface_ref)
                break;
            do {
                XR_PARSE_PUSH(parser, interfaces, interface_count, interface_capacity, iface_ref);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA));
    }

    // Detect colon-style inheritance: class Dog : Animal
    if (xr_parser_check(parser, TK_COLON)) {
        do {
            xr_parser_error_at_current(
            parser,
            "use 'extends' instead of ':' for class inheritance, e.g. class Dog extends Animal");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        // Restore type_scope so the rest of the file continues to parse in the outer scope.
        // Local parser allocations are arena-owned and released at parse end.
        if (type_param_count > 0) {
            parser->type_scope = saved_scope;
            do {
                xr_parser_type_scope_free(parser, generic_scope);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
        // interfaces[] entries are arena-owned XrTypeRefs; nothing to free.
        (void) interfaces;
        (void) interface_count;
        return NULL;
    }

    /* `class Box<T> implements Show where T: Comparable { ... }` */
    do {
        xr_parse_where_clause(parser, type_params, type_param_count);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    // Parse class body
    do {
        xr_parser_consume(parser, TK_LBRACE, "expected '{' to start class body");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    parser->scope_depth++;

    // Collect field and method declarations
    AstNode **fields = NULL;
    int field_count = 0;
    int field_capacity = 0;

    AstNode **methods = NULL;
    int method_count = 0;
    int method_capacity = 0;

    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        // Error recovery: skip to next valid token
        if (parser->panic_mode) {
            do {
                xr_parser_synchronize(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_EOF))
                break;
            continue;
        }

        if (xr_parser_check(parser, TK_VAR)) {
            do {
                xr_parser_error_at_current(parser,
                                       "'var' is not used for field declarations in class body; "
                                       "write the field name directly, e.g.: name: string");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            continue;
        }

        if (xr_parser_check(parser, TK_FN)) {
            do {
                xr_parser_error_at_current(parser,
                                       "'fn' keyword not needed for method definitions in class "
                                       "body, write method name directly, e.g.: greet() { ... }");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            continue;
        }

        // Skip optional semicolons (Xray supports optional semicolons)
        if (xr_parser_check(parser, TK_SEMICOLON)) {
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            continue;
        }

        // Skip unknown tokens to avoid infinite loop
        if (!xr_parser_check(parser, TK_NAME) && !xr_parser_check(parser, TK_PRIVATE) &&
            !xr_parser_check(parser, TK_PROTECTED) && !xr_parser_check(parser, TK_CONST) &&
            !xr_parser_check(parser, TK_STATIC) && !xr_parser_check(parser, TK_CONSTRUCTOR) &&
            !xr_parser_check(parser, TK_FINAL) && !xr_parser_check(parser, TK_OPERATOR) &&
            !xr_parser_check(parser, TK_AT)) {
            do {
                xr_parser_error_expected_name(parser, "expected field or method name");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            continue;
        }

        // Determine if this is a method or field
        bool is_method = false;
        XrTrivia *leading_trivia = parser->current.leading_trivia;
        parser->current.leading_trivia = NULL;
        AstNode *member = xr_parse_field_declaration(parser, &is_method);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!member) {
            attach_leading_trivia_to_member(NULL, leading_trivia);
            continue;
        }
        attach_leading_trivia_to_member(member, leading_trivia);
        attach_trailing_trivia_to_member(parser, member);

        if (is_method) {
            do {
                XR_PARSE_PUSH(parser, methods, method_count, method_capacity, member);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);

            // Check for paired setter (property accessor block case)
            if (member->type == AST_METHOD_DECL && member->as.method_decl.base_arg_count == -2) {
                // Has paired setter, extract from temporary storage and add
                AstNode *setter = (AstNode *) member->as.method_decl.base_args;
                member->as.method_decl.base_arg_count = 0;  // restore normal value
                member->as.method_decl.base_args = NULL;
                setter->as.method_decl.base_arg_count = 0;  // restore normal value

                do {
                    XR_PARSE_PUSH(parser, methods, method_count, method_capacity, setter);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
        } else {
            do {
                XR_PARSE_PUSH(parser, fields, field_count, field_capacity, member);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
    }

    parser->scope_depth--;

    do {
        xr_parser_consume(parser, TK_RBRACE, "expected '}' to end class body");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    int end_line = parser->previous.line;
    int end_column = parser->previous.column + 1;  // exclusive, past '}'

    // Restore type_scope after parsing class body
    if (type_param_count > 0) {
        parser->type_scope = saved_scope;
        do {
            xr_parser_type_scope_free(parser, generic_scope);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    // Create class declaration AST node
    AstNode *class_node = xr_ast_class_decl(parser->compiler_session, class_name, super_name,
                                            fields, field_count, methods, method_count, line);
    if (!xr_parser_healthy(parser)) return NULL;
    class_node->column = name_column;
    class_node->end_line = end_line;
    class_node->end_column = end_column;

    // Set superclass module (supports extends module.Class syntax)
    class_node->as.class_decl.super_module = super_module;
    class_node->as.class_decl.interfaces = interfaces;
    class_node->as.class_decl.interface_count = interface_count;

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
 *   - Explicit constructors and field literals share the declaration syntax
 *   - Fields must have type annotations
 */
AstNode *xr_parse_struct_declaration(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_struct_declaration: NULL parser");
    int line = parser->previous.line;  // 'struct' already consumed

    // Parse struct name
    do {
        xr_parser_consume(parser, TK_NAME, "expected struct name");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    char *struct_name = xr_parser_token_string(parser, &parser->previous);
    if (!xr_parser_healthy(parser)) return NULL;
    int name_column = parser->previous.column;

    /* Parse generic type parameters <T, U: Constraint, V = int>. The helper
     * installs a scope holding them so they resolve in field types. */
    XrTypeScope *saved_scope = parser->type_scope;
    int type_param_count = 0;
    XrGenericParam **type_params = xr_parse_generic_params(parser, &type_param_count);
    if (!xr_parser_healthy(parser)) return NULL;
    XrTypeScope *generic_scope = type_param_count > 0 ? parser->type_scope : NULL;

    uint32_t explicit_align = xr_parse_struct_align_clause(parser);
    if (!xr_parser_healthy(parser)) return NULL;

    // Structs do not support extends (no inheritance)
    if (xr_parser_check(parser, TK_EXTENDS)) {
        do {
            xr_parser_error_at_current(parser, "structs cannot inherit from other types");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    // Parse implements clause (structs can implement interfaces).
    // Use full type-reference parser so `implements Container<int>` works.
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

    // Parse struct body
    /* `struct Pair<K, V> where K: Hashable { ... }` */
    do {
        xr_parse_where_clause(parser, type_params, type_param_count);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    do {
        xr_parser_consume(parser, TK_LBRACE, "expected '{' to start struct body");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    AstNode **fields = NULL;
    int field_count = 0;
    int field_capacity = 0;

    AstNode **methods = NULL;
    int method_count = 0;
    int method_capacity = 0;

    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        if (parser->panic_mode) {
            do {
                xr_parser_synchronize(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_EOF))
                break;
            continue;
        }

        // Skip optional semicolons
        if (xr_parser_check(parser, TK_SEMICOLON)) {
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            continue;
        }

        // Reject invalid modifiers for structs
        if (xr_parser_check_name(parser, "abstract")) {
            bool ignored = false;
            reject_removed_member_modifier(parser, &ignored,
                                           "'abstract' is not allowed in struct declarations");
            continue;
        }
        if (xr_parser_check_name(parser, "override")) {
            bool ignored = false;
            reject_removed_member_modifier(parser, &ignored,
                                           "'override' is not allowed in struct declarations");
            continue;
        }
        if (xr_parser_check(parser, TK_FINAL)) {
            bool ignored = false;
            reject_removed_member_modifier(parser, &ignored,
                                           "'final' is not allowed in struct declarations");
            continue;
        }

        if (xr_parser_check(parser, TK_VAR)) {
            do {
                xr_parser_error_at_current(parser,
                                       "'var' is not used for field declarations in struct body; "
                                       "write the field name directly, e.g.: name: i64");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            continue;
        }

        // Reject method keyword hints.
        if (xr_parser_check(parser, TK_FN)) {
            do {
                xr_parser_error_at_current(
                parser, "'fn' keyword not needed for method definitions in struct body");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            continue;
        }

        // Skip unknown tokens
        if (!xr_parser_check(parser, TK_NAME) && !xr_parser_check(parser, TK_PRIVATE) &&
            !xr_parser_check(parser, TK_PROTECTED) && !xr_parser_check(parser, TK_CONST) &&
            !xr_parser_check(parser, TK_STATIC) && !xr_parser_check(parser, TK_OPERATOR) &&
            !xr_parser_check(parser, TK_CONSTRUCTOR) && !xr_parser_check(parser, TK_AT)) {
            do {
                xr_parser_error_expected_name(parser, "expected field or method name in struct");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            continue;
        }

        bool is_method = false;
        AstNode *member = xr_parse_field_declaration(parser, &is_method);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!member)
            continue;

        if (is_method) {
            do {
                XR_PARSE_PUSH(parser, methods, method_count, method_capacity, member);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);

            // Check for paired setter (property accessor block case)
            if (member->type == AST_METHOD_DECL && member->as.method_decl.base_arg_count == -2) {
                AstNode *setter = (AstNode *) member->as.method_decl.base_args;
                member->as.method_decl.base_arg_count = 0;
                member->as.method_decl.base_args = NULL;
                setter->as.method_decl.base_arg_count = 0;

                do {
                    XR_PARSE_PUSH(parser, methods, method_count, method_capacity, setter);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
        } else {
            do {
                XR_PARSE_PUSH(parser, fields, field_count, field_capacity, member);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
    }

    do {
        xr_parser_consume(parser, TK_RBRACE, "expected '}' to end struct body");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    int struct_end_line = parser->previous.line;
    int struct_end_column = parser->previous.column + 1;

    // Restore type_scope after parsing struct body
    if (type_param_count > 0) {
        parser->type_scope = saved_scope;
        do {
            xr_parser_type_scope_free(parser, generic_scope);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    // Create struct declaration AST node
    AstNode *struct_node = xr_ast_struct_decl(parser->compiler_session, struct_name, fields,
                                              field_count, methods, method_count, line);
    if (!xr_parser_healthy(parser)) return NULL;
    struct_node->column = name_column;
    struct_node->end_line = struct_end_line;
    struct_node->end_column = struct_end_column;
    struct_node->as.struct_decl.interfaces = interfaces;
    struct_node->as.struct_decl.interface_count = interface_count;
    struct_node->as.struct_decl.type_params = type_params;
    struct_node->as.struct_decl.type_param_count = type_param_count;
    struct_node->as.struct_decl.explicit_align = explicit_align;

    return struct_node;
}

/* ========== Union Declaration Parsing ========== */

AstNode *xr_parse_union_declaration(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_union_declaration: NULL parser");
    int line = parser->previous.line;  // 'union' already consumed

    do {
        xr_parser_consume(parser, TK_NAME, "expected union name");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    char *union_name = xr_parser_token_string(parser, &parser->previous);
    if (!xr_parser_healthy(parser)) return NULL;
    int name_column = parser->previous.column;

    if (xr_parser_check(parser, TK_LT)) {
        do {
            xr_parser_error_at_current(parser, "union declarations cannot be generic");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        int depth = 0;
        do {
            if (xr_parser_match(parser, TK_LT)) {
                depth++;
            } else if (xr_parser_match(parser, TK_GT)) {
                depth--;
            } else {
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
        } while (xr_parser_healthy(parser) && depth > 0 && !xr_parser_check(parser, TK_EOF));
        parser->panic_mode = 0;
    }

    uint32_t explicit_align = xr_parse_struct_align_clause(parser);
    if (!xr_parser_healthy(parser)) return NULL;

    if (xr_parser_match(parser, TK_IMPLEMENTS)) {
        do {
            xr_parser_error_at_previous(parser, "union declarations cannot implement interfaces");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            (void) xr_parse_type_annotation(parser);
        } while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA));
        parser->panic_mode = 0;
    }

    do {
        xr_parser_consume(parser, TK_LBRACE, "expected '{' to start union body");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    AstNode **fields = NULL;
    int field_count = 0;
    int field_capacity = 0;

    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        if (parser->panic_mode) {
            do {
                xr_parser_synchronize(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_EOF))
                break;
            parser->panic_mode = 0;
        }

        if (xr_parser_match(parser, TK_SEMICOLON))
            continue;

        if (xr_parser_check(parser, TK_STATIC) || xr_parser_check(parser, TK_PRIVATE) ||
            xr_parser_check(parser, TK_PROTECTED) || xr_parser_check(parser, TK_CONST) ||
            xr_parser_check(parser, TK_OPERATOR) || xr_parser_check(parser, TK_FN) ||
            xr_parser_check(parser, TK_CONSTRUCTOR)) {
            do {
                xr_parser_error_at_current(parser,
                                       "union declarations may contain only plain typed fields");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            continue;
        }

        do {
            xr_parser_consume(parser, TK_NAME, "expected union field name");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        Token name_tok = parser->previous;
        char *field_name = xr_parser_token_string(parser, &name_tok);
        if (!xr_parser_healthy(parser)) return NULL;

        if (xr_parser_check(parser, TK_LPAREN) || xr_parser_check(parser, TK_LT)) {
            do {
                xr_parser_error_at_current(parser, "union declarations cannot declare methods");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_SEMICOLON) &&
                   !xr_parser_check(parser, TK_EOF)) {
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
            do {
                xr_parser_match(parser, TK_SEMICOLON);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            parser->panic_mode = 0;
            continue;
        }

        do {
            xr_parser_consume(parser, TK_COLON, "expected ':' after union field name");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        bool is_flexible = false;
        if (xr_parser_check_name(parser, "flex")) {
            is_flexible = true;
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
        XrTypeRef *field_type = xr_parse_type_annotation(parser);
        if (!xr_parser_healthy(parser)) return NULL;

        AstNode *initializer = NULL;
        if (xr_parser_match(parser, TK_ASSIGN)) {
            do {
                xr_parser_error_at_previous(parser, "union fields cannot have default initializers");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                initializer = xr_parse_expression(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            parser->panic_mode = 0;
        }

        AstNode *field = xr_ast_field_decl(parser->compiler_session, field_name, field_type, false,
                                           false, initializer, name_tok.line);
        if (!xr_parser_healthy(parser)) return NULL;
        if (field) {
            field->as.field_decl.is_flexible = is_flexible;
            field->column = name_tok.column;
            field->end_line = name_tok.line;
            field->end_column = name_tok.column + name_tok.length;
            do {
                XR_PARSE_PUSH(parser, fields, field_count, field_capacity, field);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
    }

    do {
        xr_parser_consume(parser, TK_RBRACE, "expected '}' to end union body");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    int union_end_line = parser->previous.line;
    int union_end_column = parser->previous.column + 1;

    AstNode *union_node =
        xr_ast_union_decl(parser->compiler_session, union_name, fields, field_count, line);
    if (!xr_parser_healthy(parser)) return NULL;
    union_node->column = name_column;
    union_node->end_line = union_end_line;
    union_node->end_column = union_end_column;
    union_node->as.union_decl.explicit_align = explicit_align;
    return union_node;
}

/* ========== Field Declaration Parsing ========== */

// Parse field or method declaration
// Syntax:
//   Field: name: string or private age: int = 0
//   Method: greet() { ... } or constructor(name) { ... }
// @param is_method_out output parameter, true for method, false for field
AstNode *xr_parse_field_declaration(Parser *parser, bool *is_method_out) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_field_declaration: NULL parser");
    int line = parser->current.line;

    XrAttribute **attributes = NULL;
    int attr_count = 0;
    int attr_capacity = 0;
    while (xr_parser_healthy(parser) && xr_parser_check(parser, TK_AT)) {
        XrAttribute *attribute = xr_parse_single_attribute(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!attribute)
            return NULL;
        if (!xr_parser_validate_member_attr(parser, attribute))
            return NULL;
        do {
            XR_PARSE_PUSH(parser, attributes, attr_count, attr_capacity, attribute);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }
    if (!xr_parser_reject_duplicate_assertion_attrs(parser, attributes, attr_count))
        return NULL;

    // Parse access modifiers (optional)
    bool is_private = false;
    bool is_protected = false;
    bool is_static = false;
    bool is_getter = false;
    (void) is_getter;
    bool is_setter = false;
    (void) is_setter;
    bool is_const = false;
    bool is_flexible = false;
    bool is_weak = false;
    bool is_override = false;
    XrParamMode receiver_mode = XR_PARAM_READ;

    if (current_is_removed_public_modifier(parser)) {
        return reject_removed_member_modifier(
            parser, is_method_out,
            "'public' modifier was removed; members are public by default, delete it");
    }

    for (; xr_parser_step(parser) && (true);) {
        const char *removed_name = NULL;
        if (xr_parser_check_name(parser, "abstract")) {
            return reject_removed_member_modifier(
                parser, is_method_out, "'abstract' was removed; use an interface for contracts");
        }
        if (xr_parser_check(parser, TK_FINAL)) {
            return reject_removed_member_modifier(parser, is_method_out,
                                                  "'final' applies only to class declarations");
        }
        if (current_is_removed_oop_modifier(parser, &removed_name)) {
            char msg[128];
            xr_parser_format(parser, msg, sizeof(msg), "'%s' was removed; class dispatch strategy is inferred",
                     removed_name);
            return reject_removed_member_modifier(parser, is_method_out, msg);
        }
        break;
    }

    if (xr_parser_match(parser, TK_PRIVATE)) {
        is_private = true;
    } else if (xr_parser_match(parser, TK_PROTECTED)) {
        is_protected = true;
    }

    if (xr_parser_match_name(parser, "override"))
        is_override = true;

    if (xr_parser_match(parser, TK_REF) || xr_parser_match_name(parser, "ref")) {
        receiver_mode = XR_PARAM_REF;
    } else if (xr_parser_match(parser, TK_MOVE) || xr_parser_match_name(parser, "move")) {
        receiver_mode = XR_PARAM_MOVE;
    }

    if (xr_parser_match(parser, TK_STATIC)) {
        if (is_override)
            do {
                xr_parser_error_at_previous(parser, "override applies only to instance methods");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        if (receiver_mode != XR_PARAM_READ)
            do {
                xr_parser_error_at_previous(parser, "static methods cannot declare a receiver mode");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        is_static = true;

        // Check for static constructor: static constructor()
        if (xr_parser_check(parser, TK_CONSTRUCTOR)) {
            if (is_private || is_protected) {
                do {
                    xr_parser_error_at_current(parser,
                                           "static constructors cannot have visibility modifiers");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
            *is_method_out = true;
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);  // consume 'constructor'
            AstNode *method = xr_parse_static_constructor(parser, is_private);
            if (!xr_parser_healthy(parser)) return NULL;
            if (attr_count > 0)
                do {
                    xr_parser_error(parser, "method attributes cannot annotate a static constructor");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            return method;
        }
    }

    if (xr_parser_check(parser, TK_FINAL)) {
        return reject_removed_member_modifier(parser, is_method_out,
                                              "'final' applies only to class declarations");
    }

    // `const` marks an immutable field (assignable only in the constructor).
    if (xr_parser_match(parser, TK_CONST)) {
        is_const = true;
    }

    /* `weak parent: Node?` — contextual like `flex`, so it does not reserve
     * the identifier globally and existing code using `weak` as a name keeps
     * working. Only a field can carry it (W3: no [weak self], no contagion). */
    if (xr_parser_check_name(parser, "weak")) {
        is_weak = true;
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    if (xr_parser_match(parser, TK_OPERATOR)) {
        *is_method_out = true;
        AstNode *method = xr_parse_operator_method(parser, is_private, is_static);
        if (!xr_parser_healthy(parser)) return NULL;
        if (method) {
            method->as.method_decl.receiver_mode = receiver_mode;
            method->as.method_decl.is_override = is_override;
            method->as.method_decl.is_protected = is_protected;
            method->as.method_decl.attributes = attributes;
            method->as.method_decl.attr_count = attr_count;
        }
        return method;
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
        if (is_override)
            do {
                xr_parser_error_at_previous(parser, "constructors cannot declare override");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        if (is_protected) {
            do {
                xr_parser_error_at_previous(
                parser,
                "protected constructors are not supported; use private constructor or a public "
                "factory");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
        do {
            name = (char *) ast_alloc(parser->compiler_session, sizeof(XR_KEYWORD_CONSTRUCTOR));
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            xr_parser_copy_string(parser, name, XR_KEYWORD_CONSTRUCTOR);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        name_line = parser->previous.line;
        name_column = parser->previous.column;
        do {
            name_length = (int) (sizeof(XR_KEYWORD_CONSTRUCTOR) - 1);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    } else {
        // Normal name
        do {
            xr_parser_consume(parser, TK_NAME, "expected field or method name");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            name = xr_parser_token_string(parser, &parser->previous);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        name_line = parser->previous.line;
        name_column = parser->previous.column;
        name_length = parser->previous.length;
    }

    // Distinguish field and method: methods have a parameter list, generic params, or constructor.
    if (xr_parser_check(parser, TK_LPAREN) || xr_parser_check(parser, TK_LT) || is_constructor) {
        // Method: has parameter list or generic type params.
        *is_method_out = true;
        if (is_const) {
            do {
                xr_parser_error_at_current(parser, "'const' applies to fields, not methods; remove it");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
        AstNode *method = xr_parse_method_declaration(parser, name, name_line, name_column,
                                                      is_private, is_static);
        if (!xr_parser_healthy(parser)) return NULL;
        if (method) {
            if (method->as.method_decl.is_constructor && receiver_mode != XR_PARAM_READ)
                do {
                    xr_parser_error(parser, "constructors cannot declare a receiver mode");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            method->as.method_decl.receiver_mode = receiver_mode;
            method->as.method_decl.is_override = is_override;
            method->as.method_decl.is_protected = is_protected;
            if (method->as.method_decl.is_constructor && attr_count > 0) {
                do {
                    xr_parser_error(parser, "method attributes cannot annotate a constructor");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
            method->as.method_decl.attributes = attributes;
            method->as.method_decl.attr_count = attr_count;
        }
        return method;
    } else {
        // Field: has type annotation or initializer
        *is_method_out = false;
        if (is_override) {
            do {
                xr_parser_error(parser, "override applies only to instance methods");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }

        if (receiver_mode != XR_PARAM_READ) {
            do {
                xr_parser_error(parser, "receiver mode applies only to instance methods");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }

        // Parse type annotation (optional)
        XrTypeRef *field_type = NULL;
        if (xr_parser_match(parser, TK_COLON)) {
            if (xr_parser_check_name(parser, "flex")) {
                is_flexible = true;
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
            // Use type annotation parser, supports all types and generic syntax
            do {
                field_type = xr_parse_type_annotation(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }

        // Check for property block { fn() {} fn(v) {} }
        if (xr_parser_check(parser, TK_LBRACE)) {
            // This is a property definition with getter/setter
            *is_method_out = true;
            if (attr_count > 0) {
                do {
                    xr_parser_error(parser,
                                "method attributes cannot annotate a property accessor block");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
            return xr_parse_property_accessors(parser, name, field_type, is_private, is_static,
                                               line);
        }

        // Parse initializer expression (optional)
        AstNode *initializer = NULL;
        if (xr_parser_match(parser, TK_ASSIGN)) {
            do {
                initializer = xr_parse_expression(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }

        // Field declaration doesn't need semicolon (Xray doesn't use semicolons)

        AstNode *field = xr_ast_field_decl(parser->compiler_session, name, field_type, is_private,
                                           is_static, initializer, name_line);
        if (!xr_parser_healthy(parser)) return NULL;
        if (attr_count > 0) {
            do {
                xr_parser_error(parser,
                            "attributes can only annotate a function or method, not a field");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        if (field) {
            /* `flex` is contextual after a field colon, so it does not reserve
             * the identifier globally.  Preserve it on the field rather than
             * disguising an unsized tail as a zero-length fixed array. */
            field->as.field_decl.is_flexible = is_flexible;
            field->as.field_decl.is_final = false;
            field->as.field_decl.is_protected = is_protected;
            field->as.field_decl.is_const = is_const;
            field->as.field_decl.is_weak = is_weak;
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
                                     int name_column, bool is_private, bool is_static) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_method_declaration: NULL parser");
    int line = name_line;

    // Check if this is a constructor
    bool is_constructor = (xr_parser_compare_string(parser, name, XR_KEYWORD_CONSTRUCTOR) == 0);
    if (!xr_parser_healthy(parser)) return NULL;

    /* Parse optional generic type parameters: methodName<T, U: Hashable>(...).
     * The shared helper installs a scope holding them so the signature and body
     * resolve T; the caller restores its own scope below. */
    XrTypeScope *saved_scope = parser->type_scope;
    int type_param_count = 0;
    XrGenericParam **type_params = xr_parse_generic_params(parser, &type_param_count);
    if (!xr_parser_healthy(parser)) return NULL;
    XrTypeScope *generic_scope = type_param_count > 0 ? parser->type_scope : NULL;

    // Parse parameter list
    do {
        xr_parser_consume(parser, TK_LPAREN, "expected '(' to start parameter list");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    XrParamNode **params = NULL;
    int param_count = 0;
    int param_capacity = 0;
    int required_count = 0;
    bool seen_default = false;
    bool is_variadic = false;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            // Extend arrays (arena grow - old buffers are bulk-released)
            if (param_count >= param_capacity) {
                int old_capacity = param_capacity;
                do {
                    param_capacity = xr_parser_grow_capacity(parser, param_capacity);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);

                XrParamNode **_new_params = (XrParamNode **) ast_alloc_array(
                    parser->compiler_session, sizeof(XrParamNode *), (size_t) param_capacity);
                if (!xr_parser_healthy(parser)) return NULL;
                if (old_capacity > 0 && params) {
                    do { if (!ast_copy(parser->compiler_session, _new_params, params, sizeof(XrParamNode *) * (size_t) old_capacity)) return NULL; } while (0);
                }
                params = _new_params;
            }

            if (xr_parser_check(parser, TK_DOT_DOT_DOT)) {
                XrParamNode *param = xr_parse_parameter(parser, XR_PARSE_PARAMETER_ALLOW_REST);
                if (!xr_parser_healthy(parser)) return NULL;
                params[param_count] = param;
                is_variadic = true;
                param_count++;
                if (xr_parser_check(parser, TK_COMMA)) {
                    do {
                        xr_parser_error(parser, "rest parameter must be last");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    break;
                }
                break;
            }

            // Parse parameter name
            XrParamNode *param = xr_parse_parameter(parser, XR_PARSE_PARAMETER_ALLOW_MODE);
            if (!xr_parser_healthy(parser)) return NULL;
            params[param_count] = param;

            if (xr_parser_match(parser, TK_ASSIGN)) {
                do {
                    xr_parse_reject_ref_out_default_param(parser, param);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                do {
                    param->default_value = xr_parse_expression(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                if (param->type == NULL && param->default_value != NULL) {
                    AstNode *dv = param->default_value;
                    switch (dv->type) {
                        case AST_LITERAL_INT:
                            do {
                                param->type = xr_tref_i64(parser->compiler_session);
                                if (!xr_parser_healthy(parser)) return NULL;
                            } while (0);
                            break;
                        case AST_LITERAL_FLOAT:
                            do {
                                param->type = xr_tref_f64(parser->compiler_session);
                                if (!xr_parser_healthy(parser)) return NULL;
                            } while (0);
                            break;
                        case AST_LITERAL_STRING:
                        case AST_TEMPLATE_STRING:
                            do {
                                param->type = xr_tref_string(parser->compiler_session);
                                if (!xr_parser_healthy(parser)) return NULL;
                            } while (0);
                            break;
                        case AST_LITERAL_TRUE:
                        case AST_LITERAL_FALSE:
                            do {
                                param->type = xr_tref_bool(parser->compiler_session);
                                if (!xr_parser_healthy(parser)) return NULL;
                            } while (0);
                            break;
                        case AST_LITERAL_NULL:
                            do {
                                param->type = xr_tref_null(parser->compiler_session);
                                if (!xr_parser_healthy(parser)) return NULL;
                            } while (0);
                            break;
                        default:
                            break;
                    }
                }
            }

            if (param->default_value) {
                seen_default = true;
            } else if (seen_default) {
                do {
                    xr_parser_error(parser, "required parameter cannot follow optional parameter");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            } else {
                required_count++;
            }

            param_count++;
        } while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RPAREN));
    }

    do {
        xr_parser_consume(parser, TK_RPAREN, "expected ')' to end parameter list");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    // Parse return type (optional) — unified arrow `->`.
    XrTypeRef *return_type = NULL;
    XrBorrowOriginSyntaxState borrow_origin_syntax = XR_BORROW_ORIGIN_OMITTED;
    AstBorrowOriginRef *borrow_origins = NULL;
    int borrow_origin_count = 0;
    if (xr_parser_match(parser, TK_ARROW)) {
        do {
            return_type = xr_parse_type_annotation(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            xr_parse_borrow_origin_set(parser, &borrow_origin_syntax, &borrow_origins,
                                   &borrow_origin_count);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    } else if (xr_parser_check(parser, TK_COLON)) {
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            xr_parser_error(parser, "use '->' instead of ':' for method return type");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        parser->panic_mode = 0;
        do {
            return_type = xr_parse_type_annotation(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    int condition_count = 0;
    XrGenericParam **conditions = xr_parse_method_conditions(parser, &condition_count);
    if (!xr_parser_healthy(parser)) return NULL;
    do {
        xr_parser_consume(parser, TK_LBRACE, "expected '{' to start method body");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    AstNode *body = xr_parse_block(parser);
    if (!xr_parser_healthy(parser)) return NULL;

    // Create method declaration node
    AstNode *method_node = xr_ast_method_decl(
        parser->compiler_session, name, params, param_count, return_type, body, is_constructor,
        is_static, is_private, false, false, line);
    if (!xr_parser_healthy(parser)) return NULL;  // is_getter, is_setter

    method_node->column = name_column;
    if (body && body->end_line > 0) {
        // Normal method: end is body's closing brace.
        method_node->end_line = body->end_line;
        method_node->end_column = body->end_column;
    }

    method_node->as.method_decl.is_variadic = is_variadic;
    method_node->as.method_decl.required_count = required_count;
    method_node->as.method_decl.borrow_origin_syntax = borrow_origin_syntax;
    method_node->as.method_decl.borrow_origins = borrow_origins;
    method_node->as.method_decl.borrow_origin_count = borrow_origin_count;

    // Set generic type parameters
    method_node->as.method_decl.type_params = type_params;
    method_node->as.method_decl.type_param_count = type_param_count;
    method_node->as.method_decl.conditions = conditions;
    method_node->as.method_decl.condition_count = condition_count;

    parser->type_scope = saved_scope;
    do {
        xr_parser_type_scope_free(parser, generic_scope);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    return method_node;
}

/* ========== New Expression Parsing ========== */

// Parse new expression
// Syntax: new Dog("Rex", "Labrador")
// Also supports: new Box<int>(42)
AstNode *xr_parse_new_expression(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_new_expression: NULL parser");
    int line = parser->previous.line;

    // The `new` keyword was removed. Construction is now `T(args)` directly.
    // Emit a clear migration error, then keep parsing the construction so
    // analysis can recover and report any further issues in the same pass.
    do {
        xr_parser_error(
        parser, "the 'new' keyword was removed; construct with 'T(...)' directly (delete 'new')");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    // 'new' keyword already consumed

    // Parse class name (can be class name or type keyword like Map, Array)
    // Supports two forms:
    //   new ClassName()
    //   new module.ClassName()
    char *module_name = NULL;
    char *class_name = NULL;

    if (xr_parser_match(parser, TK_NAME)) {
        char *first_name = xr_parser_token_string(parser, &parser->previous);
        if (!xr_parser_healthy(parser)) return NULL;

        // Check if it's module.Class form
        if (xr_parser_match(parser, TK_DOT)) {
            // Is module.Class form
            module_name = first_name;

            if (xr_parser_match(parser, TK_NAME)) {
                do {
                    class_name = xr_parser_token_string(parser, &parser->previous);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            } else {
                do {
                    xr_parser_error_expected_name(parser, "expected class name");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                do {
                    class_name = ast_strdup(parser->compiler_session, TYPE_NAME_NULL);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
        } else {
            // Just a normal class name
            class_name = first_name;
        }
    } else {
        // Support other built-in type names as new target
        do {
            xr_parser_error_expected_name(parser, "expected class name or type name");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            class_name = ast_strdup(parser->compiler_session, TYPE_NAME_NULL);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    // Parse optional generic type parameters: new Box<int>(...)
    XrTypeRef *type_args[16];  // Max 16 type args
    int type_arg_count = 0;

    if (xr_parser_match(parser, TK_LT)) {
        do {
            if (type_arg_count >= 16)
                break;

            XrTypeRef *type = xr_parse_type_annotation(parser);
            if (!xr_parser_healthy(parser)) return NULL;
            if (!type) {
                do {
                    xr_parser_error(parser, "expected type in generic type arguments");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                break;
            }
            type_args[type_arg_count++] = type;
        } while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_GT));

        // Consume closing '>' (handle >> as two > for nested generics like Box<Array<int>>)
        if (xr_parser_check(parser, TK_GT)) {
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
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
            do {
                xr_parser_error(parser, "expected '>' after generic type arguments");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
    }

    // Parse constructor arguments
    do {
        xr_parser_consume(parser, TK_LPAREN, "expected '(' to start argument list");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    AstNode **arguments = NULL;
    XrCallArgAccess *arg_accesses = NULL;
    int arg_count = 0;
    int arg_capacity = 0;
    int access_count = 0;
    int access_capacity = 0;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            XrCallArgAccess access = XR_CALL_ARG_PLAIN;
            AstNode *arg = xr_parse_call_argument_with_access(parser, &access);
            if (!xr_parser_healthy(parser)) return NULL;
            do {
                XR_PARSE_PUSH(parser, arguments, arg_count, arg_capacity, arg);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                XR_PARSE_PUSH(parser, arg_accesses, access_count, access_capacity, access);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RPAREN));
    }

    do {
        xr_parser_consume(parser, TK_RPAREN, "expected ')' to end argument list");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    // Create new expression node. xr_ast_new_expr takes ownership of
    // class_name / arguments / type_args, but deep-copies module_name,
    // so release our copy of module_name here.
    AstNode *node = xr_ast_new_expr(parser->compiler_session, module_name, class_name, arguments,
                                    arg_accesses, arg_count, type_args, type_arg_count, line);
    if (!xr_parser_healthy(parser)) return NULL;
    return node;
}

/* ========== This Expression Parsing ========== */

// Parse this expression
// Syntax: this
AstNode *xr_parse_this_expression(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    int line = parser->previous.line;

    // 'this' keyword already consumed

    // Create this expression node
    return xr_ast_this_expr(parser->compiler_session, line);
}

/* ========== Super Expression Parsing ========== */

// Parse super call
// Syntax:
//   super.greet(args)  - call superclass method
//   super(args)        - call superclass constructor
AstNode *xr_parse_super_expression(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    int line = parser->previous.line;

    // 'super' keyword already consumed

    char *method_name = NULL;

    // Check if super() or super.method()
    if (xr_parser_match(parser, TK_DOT)) {
        // super.method()
        do {
            xr_parser_consume(parser, TK_NAME, "expected method name");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            method_name = xr_parser_token_string(parser, &parser->previous);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);

        // Parse parameter list
        do {
            xr_parser_consume(parser, TK_LPAREN, "expected '(' to start argument list");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    } else if (xr_parser_match(parser, TK_LPAREN)) {
        // super(args) - call superclass constructor
        method_name = NULL;  // NULL means constructor
    } else {
        do {
            xr_parser_error(parser, "expected '.' or '(' after super");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    // Parse arguments
    AstNode **arguments = NULL;
    XrCallArgAccess *arg_accesses = NULL;
    int arg_count = 0;
    int arg_capacity = 0;
    int access_count = 0;
    int access_capacity = 0;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            XrCallArgAccess access = XR_CALL_ARG_PLAIN;
            AstNode *arg = xr_parse_call_argument_with_access(parser, &access);
            if (!xr_parser_healthy(parser)) return NULL;
            do {
                XR_PARSE_PUSH(parser, arguments, arg_count, arg_capacity, arg);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                XR_PARSE_PUSH(parser, arg_accesses, access_count, access_capacity, access);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RPAREN));
    }

    do {
        xr_parser_consume(parser, TK_RPAREN, "expected ')' to end argument list");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    // Create super call node
    return xr_ast_super_call(parser->compiler_session, method_name, arguments, arg_accesses,
                             arg_count, line);
}

/* ========== Operator Method Parsing ========== */

// Parse operator method declaration
// Syntax: operator +(other: Type): Type { ... }
// @param is_private whether private
// @param is_static whether static
AstNode *xr_parse_operator_method(Parser *parser, bool is_private, bool is_static) {
    if (!xr_parser_healthy(parser)) return NULL;
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
        case TK_NAME:
            if (parser->current.length != 3 || xr_parser_compare_bytes(parser, parser->current.start, "len", 3) != 0) {
                do {
                    xr_parser_error(parser, "unsupported named operator; expected 'len'");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
            /* Store named operators outside the ordinary member namespace.
             * The spelling is retained in op_type for formatter/tooling use. */
            do {
                name = ast_strdup(parser->compiler_session, "__operator_len");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_LEN;
            expected_params = 0;
            break;
        // Arithmetic operators
        case TK_PLUS:
            do {
                name = ast_strdup(parser->compiler_session, "+");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_ADD;
            break;
        case TK_MINUS:
            do {
                name = ast_strdup(parser->compiler_session, "-");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_SUB;  // default to binary, adjusted later based on param count
            break;
        case TK_STAR:
            do {
                name = ast_strdup(parser->compiler_session, "*");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_MUL;
            break;
        case TK_SLASH:
            do {
                name = ast_strdup(parser->compiler_session, "/");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_DIV;
            break;
        case TK_PERCENT:
            do {
                name = ast_strdup(parser->compiler_session, "%");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_MOD;
            break;
        // Bitwise operators
        case TK_AMP:
            do {
                name = ast_strdup(parser->compiler_session, "&");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_BAND;
            break;
        case TK_PIPE:
            do {
                name = ast_strdup(parser->compiler_session, "|");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_BOR;
            break;
        case TK_CARET:
            do {
                name = ast_strdup(parser->compiler_session, "^");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_BXOR;
            break;
        case TK_TILDE:
            do {
                name = ast_strdup(parser->compiler_session, "~");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_UNARY;  // unary operator
            expected_params = 0;         // unary operator needs no extra params
            break;
        case TK_NOT:
            do {
                name = ast_strdup(parser->compiler_session, "!");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_UNARY;  // unary operator
            expected_params = 0;         // unary operator needs no extra params
            break;
        // Comparison operators
        case TK_EQ:
            do {
                name = ast_strdup(parser->compiler_session, "==");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_EQ;
            break;
        case TK_NE:
            do {
                name = ast_strdup(parser->compiler_session, "!=");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_NE;
            break;
        case TK_LT:
            do {
                name = ast_strdup(parser->compiler_session, "<");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_LT;
            break;
        case TK_LE:
            do {
                name = ast_strdup(parser->compiler_session, "<=");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_LE;
            break;
        case TK_GT:
            do {
                name = ast_strdup(parser->compiler_session, ">");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_GT;
            break;
        case TK_GE:
            do {
                name = ast_strdup(parser->compiler_session, ">=");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_GE;
            break;

        // Subscript operator
        case TK_LBRACKET:
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            if (!xr_parser_match(parser, TK_RBRACKET)) {
                do {
                    xr_parser_error(parser, "expected ']' in operator []");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
            // Check for = sign (operator []=)
            if (xr_parser_match(parser, TK_ASSIGN)) {
                do {
                    name = ast_strdup(parser->compiler_session, "[]=");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                expected_params = 2;  // []= needs 2 params: index + value
                op_type_val = OPTYPE_SUBSCRIPT_SET;
            } else {
                do {
                    name = ast_strdup(parser->compiler_session, "[]");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                expected_params = 1;  // [] needs 1 param: index
                op_type_val = OPTYPE_SUBSCRIPT;
            }
            break;

        // Shift operators
        case TK_LSHIFT:
            do {
                name = ast_strdup(parser->compiler_session, "<<");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_BAND;  // reuse bitwise type
            break;
        case TK_RSHIFT:
            do {
                name = ast_strdup(parser->compiler_session, ">>");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_BAND;  // reuse bitwise type
            break;

        // Compound assignment operators
        case TK_PLUS_ASSIGN:
            do {
                name = ast_strdup(parser->compiler_session, "+=");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_ADD;  // reuse add type
            break;
        case TK_MINUS_ASSIGN:
            do {
                name = ast_strdup(parser->compiler_session, "-=");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_SUB;  // reuse sub type
            break;
        case TK_MUL_ASSIGN:
            do {
                name = ast_strdup(parser->compiler_session, "*=");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_MUL;  // reuse mul type
            break;
        case TK_DIV_ASSIGN:
            do {
                name = ast_strdup(parser->compiler_session, "/=");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_DIV;  // reuse div type
            break;
        case TK_MOD_ASSIGN:
            do {
                name = ast_strdup(parser->compiler_session, "%=");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_MOD;  // reuse mod type
            break;
        case TK_AND_ASSIGN:
            do {
                name = ast_strdup(parser->compiler_session, "&=");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_BAND;  // reuse bitwise and type
            break;
        case TK_OR_ASSIGN:
            do {
                name = ast_strdup(parser->compiler_session, "|=");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_BOR;  // reuse bitwise or type
            break;
        case TK_XOR_ASSIGN:
            do {
                name = ast_strdup(parser->compiler_session, "^=");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_BXOR;  // reuse bitwise xor type
            break;
        case TK_LSHIFT_ASSIGN:
            do {
                name = ast_strdup(parser->compiler_session, "<<=");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_BAND;  // reuse bitwise type
            break;
        case TK_RSHIFT_ASSIGN:
            do {
                name = ast_strdup(parser->compiler_session, ">>=");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_BAND;  // reuse bitwise type
            break;

        // Increment/decrement operators
        case TK_INC:
            do {
                name = ast_strdup(parser->compiler_session, "++");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_UNARY;  // treat as unary operator
            break;
        case TK_DEC:
            do {
                name = ast_strdup(parser->compiler_session, "--");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            op_type_val = OPTYPE_UNARY;  // treat as unary operator
            break;

        default:
            do {
                xr_parser_error(parser, "unsupported operator type");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
    }

    // Consume operator token (except [] already consumed)
    if (op_token != TK_LBRACKET) {
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    // Parse parameter list
    do {
        xr_parser_consume(parser, TK_LPAREN, "expected '(' to start parameter list");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    XrParamNode **params = NULL;
    int param_count = 0;

    // Parse parameters: most operators need 1, []= needs 2, unary operators need 0
    if (xr_parser_check(parser, TK_RPAREN)) {
        if (expected_params > 0 && op_token != TK_MINUS && op_token != TK_NOT &&
            op_token != TK_INC && op_token != TK_DEC) {
            do {
                xr_parser_error(parser, "operator method requires parameters");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            goto fail;
        }
        // Unary operators (expected_params == 0) can have no parameters
        do {
            xr_parser_consume(parser, TK_RPAREN, "expected ')' to end parameter list");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        params = NULL;
        param_count = 0;
        // Go directly to return type parsing, no goto needed
    } else {
        // Allocate parameter arrays in the parse arena
        do {
            params = (XrParamNode **) ast_alloc_array(parser->compiler_session, sizeof(XrParamNode *),
                                                  (size_t) expected_params);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);

        // Parse first parameter
        XrParamNode *first_param = xr_parse_parameter(parser, XR_PARSE_PARAMETER_ALLOW_MODE);
        if (!xr_parser_healthy(parser)) return NULL;
        params[0] = first_param;

        param_count = 1;

        // operator []= needs second parameter (value)
        if (expected_params == 2) {
            if (!xr_parser_match(parser, TK_COMMA)) {
                do {
                    xr_parser_error(parser, "operator []= requires 2 parameters");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                goto fail;
            }

            XrParamNode *second_param = xr_parse_parameter(parser, XR_PARSE_PARAMETER_ALLOW_MODE);
            if (!xr_parser_healthy(parser)) return NULL;
            params[1] = second_param;

            param_count = 2;
        }

        // Should not have more parameters
        if (xr_parser_match(parser, TK_COMMA)) {
            do {
                xr_parser_error(parser, "too many parameters for operator method");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            goto fail;
        }

        do {
            xr_parser_consume(parser, TK_RPAREN, "expected ')' to end parameter list");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    // Adjust operator type based on parameter count (handle unary/binary ambiguity)
    if (op_token == TK_MINUS && param_count == 0) {
        op_type_val = OPTYPE_UNARY;  // no params - is unary negation
    }
    if (op_token == TK_NOT && param_count == 0) {
        op_type_val = OPTYPE_UNARY;  // no params ! is unary logical not
    }

    // Parse return type (optional) — unified arrow `->`.
    XrTypeRef *return_type = NULL;
    XrBorrowOriginSyntaxState borrow_origin_syntax = XR_BORROW_ORIGIN_OMITTED;
    AstBorrowOriginRef *borrow_origins = NULL;
    int borrow_origin_count = 0;
    if (xr_parser_match(parser, TK_ARROW)) {
        do {
            return_type = xr_parse_type_annotation(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            xr_parse_borrow_origin_set(parser, &borrow_origin_syntax, &borrow_origins,
                                   &borrow_origin_count);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    } else if (xr_parser_check(parser, TK_COLON)) {
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            xr_parser_error(parser, "use '->' instead of ':' for method return type");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        parser->panic_mode = 0;
        do {
            return_type = xr_parse_type_annotation(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    // Parse method body
    do {
        xr_parser_consume(parser, TK_LBRACE, "expected '{' to start method body");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    AstNode *body = xr_parse_block(parser);
    if (!xr_parser_healthy(parser)) return NULL;

    // Create method node
    AstNode *method =
        xr_ast_method_decl(parser->compiler_session, name, params, param_count, return_type, body,
                           false,  // is_constructor
                           is_static, is_private,
                           false,  // is_getter
                           false,  // is_setter
                           line);
    if (!xr_parser_healthy(parser)) return NULL;

    method->column = name_column;
    if (body && body->end_line > 0) {
        method->end_line = body->end_line;
        method->end_column = body->end_column;
    } else {
        method->end_line = name_line;
        do {
            method->end_column = name_column + (int) xr_parser_string_length(parser, name);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    // Set operator flags
    method->as.method_decl.is_operator = true;     // mark as operator method
    method->as.method_decl.op_type = op_type_val;  // set specific operator type
    method->as.method_decl.borrow_origin_syntax = borrow_origin_syntax;
    method->as.method_decl.borrow_origins = borrow_origins;
    method->as.method_decl.borrow_origin_count = borrow_origin_count;
    return method;

fail:
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
    if (!xr_parser_healthy(parser)) return NULL;
    do {
        xr_parser_consume(parser, TK_LBRACE, "expected '{' to start property accessor block");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    AstNode *getter_node = NULL;
    AstNode *setter_node = NULL;

    // Parse fn definitions in property block
    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        if (!xr_parser_match(parser, TK_FN)) {
            do {
                xr_parser_error(parser, "property accessor block can only contain fn() definitions");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            break;
        }

        // Parse parameter list
        do {
            xr_parser_consume(parser, TK_LPAREN, "expected '(' to start parameter list");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);

        XrParamNode **params = NULL;
        int param_count = 0;

        if (!xr_parser_check(parser, TK_RPAREN)) {
            // Has parameters = setter
            do {
                params = (XrParamNode **) ast_alloc(parser->compiler_session, sizeof(XrParamNode *));
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);

            XrParamNode *param = xr_parse_parameter(parser, XR_PARSE_PARAMETER_ALLOW_MODE);
            if (!xr_parser_healthy(parser)) return NULL;
            if (!param->type)
                param->type = field_type;
            params[0] = param;

            param_count = 1;

            if (xr_parser_match(parser, TK_COMMA)) {
                do {
                    xr_parser_error(parser, "setter can only have one parameter");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
        }

        do {
            xr_parser_consume(parser, TK_RPAREN, "expected ')' to end parameter list");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);

        // Parse return type (optional) — unified arrow `->`.
        XrTypeRef *return_type = NULL;
        if (xr_parser_match(parser, TK_ARROW)) {
            do {
                return_type = xr_parse_type_annotation(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } else if (xr_parser_check(parser, TK_COLON)) {
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                xr_parser_error(parser, "use '->' instead of ':' for accessor return type");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            parser->panic_mode = 0;
            do {
                return_type = xr_parse_type_annotation(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } else if (param_count == 0) {
            // getter defaults to property type
            return_type = field_type;
        }

        // Parse method body
        do {
            xr_parser_consume(parser, TK_LBRACE, "expected '{' to start method body");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        AstNode *body = xr_parse_block(parser);
        if (!xr_parser_healthy(parser)) return NULL;

        // Construct method name: get:xxx or set:xxx
        bool is_getter = (param_count == 0);
        size_t name_len = xr_parser_string_length(parser, name) + 5;
        if (!xr_parser_healthy(parser)) return NULL;
        char *method_name = (char *) ast_alloc(parser->compiler_session, name_len);
        if (!xr_parser_healthy(parser)) return NULL;
        xr_parser_format(parser, method_name, name_len, "%s:%s", is_getter ? "get" : "set", name);

        // Create method declaration node
        AstNode *method_node = xr_ast_method_decl(parser->compiler_session, method_name, params,
                                                  param_count, return_type, body, false, is_static,
                                                  is_private, is_getter, !is_getter, line);
        if (!xr_parser_healthy(parser)) return NULL;
        method_node->as.method_decl.receiver_mode =
            is_getter || is_static ? XR_PARAM_READ : XR_PARAM_REF;

        method_node->column = 1;  // property accessors are synthetic — column
                                  //   mirrors the declaration line (safe 1)
        if (body && body->end_line > 0) {
            method_node->end_line = body->end_line;
            method_node->end_column = body->end_column;
        } else {
            method_node->end_line = line;
            do {
                method_node->end_column = 1 + (int) xr_parser_string_length(parser, method_name);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }

        if (is_getter) {
            if (getter_node != NULL) {
                do {
                    xr_parser_error(parser, "property can only have one getter (fn with no params)");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
            getter_node = method_node;
        } else {
            if (setter_node != NULL) {
                do {
                    xr_parser_error(parser, "property can only have one setter (fn with params)");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
            setter_node = method_node;
        }
    }

    do {
        xr_parser_consume(parser, TK_RBRACE, "expected '}' to end property accessor block");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    if (getter_node == NULL && setter_node == NULL) {
        do {
            xr_parser_error(parser, "property accessor block cannot be empty");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
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
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_interface_declaration: NULL parser");
    int line = parser->previous.line;

    // 'interface' keyword already consumed

    // Parse interface name
    do {
        xr_parser_consume(parser, TK_NAME, "expected interface name");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    char *interface_name = xr_parser_token_string(parser, &parser->previous);
    if (!xr_parser_healthy(parser)) return NULL;
    int name_column = parser->previous.column;

    /* Parse generic type parameters <T, U: Constraint, V = int>. The helper
     * installs a scope holding them so member type annotations such as
     * `iterator(): Iterator<T>` recognise T as a type parameter. */
    XrTypeScope *saved_scope = parser->type_scope;
    int type_param_count = 0;
    XrGenericParam **type_params = xr_parse_generic_params(parser, &type_param_count);
    if (!xr_parser_healthy(parser)) return NULL;
    XrTypeScope *generic_scope = type_param_count > 0 ? parser->type_scope : NULL;

    // Parse extends clause (optional, interface can extend multiple interfaces).
    // Use full type-reference parser so `extends Pair<K, V>` works.
    XrTypeRef **extends = NULL;
    int extends_count = 0;
    int extends_capacity = 0;

    if (xr_parser_match(parser, TK_EXTENDS)) {
        do {
            XrTypeRef *parent_ref = xr_parse_type_annotation(parser);
            if (!xr_parser_healthy(parser)) return NULL;
            if (!parent_ref)
                break;
            do {
                XR_PARSE_PUSH(parser, extends, extends_count, extends_capacity, parent_ref);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA));
    }

    // Parse interface body
    /* `interface Seq<T> where T: Comparable { ... }` */
    do {
        xr_parse_where_clause(parser, type_params, type_param_count);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    do {
        xr_parser_consume(parser, TK_LBRACE, "expected '{' to start interface body");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    // Collect method and property signatures into separate arrays so consumers
    // can iterate them with a static guarantee about node kind (mirrors
    // ClassDeclNode's fields[] / methods[] split).
    AstNode **methods = NULL;
    int method_count = 0;
    int method_capacity = 0;
    AstNode **properties = NULL;
    int property_count = 0;
    int property_capacity = 0;

    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        // Error recovery to avoid infinite loop
        if (parser->panic_mode) {
            Token before_sync = parser->current;
            do {
                xr_parser_synchronize(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_EOF))
                break;
            /* Top-level recovery tokens such as `class` may be left untouched
             * by synchronize().  Inside an interface that means the closing
             * brace was already skipped or is missing.  Stop the nested loop
             * and let the outer declaration parser consume the token instead
             * of repeatedly diagnosing the same byte offset forever. */
            if (parser->current.start == before_sync.start &&
                parser->current.type == before_sync.type)
                break;
            continue;
        }

        XrTrivia *leading_trivia = parser->current.leading_trivia;
        parser->current.leading_trivia = NULL;
        AstNode *member = xr_parse_interface_member(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!member) {
            attach_leading_trivia_to_member(NULL, leading_trivia);
            // Skip to next member or end of interface
            if (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
            continue;
        }
        attach_leading_trivia_to_member(member, leading_trivia);
        attach_trailing_trivia_to_member(parser, member);

        if (member->type == AST_INTERFACE_PROPERTY) {
            do {
                XR_PARSE_PUSH(parser, properties, property_count, property_capacity, member);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } else {
            do {
                XR_PARSE_PUSH(parser, methods, method_count, method_capacity, member);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
    }

    do {
        xr_parser_consume(parser, TK_RBRACE, "expected '}' to end interface body");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    int if_end_line = parser->previous.line;
    int if_end_column = parser->previous.column + 1;

    // Restore type_scope after parsing interface body
    if (type_param_count > 0) {
        parser->type_scope = saved_scope;
        do {
            xr_parser_type_scope_free(parser, generic_scope);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    // Create interface declaration AST node
    AstNode *node = xr_ast_interface_decl(parser->compiler_session, interface_name, extends,
                                          extends_count, methods, method_count, properties,
                                          property_count, type_params, type_param_count, line);
    if (!xr_parser_healthy(parser)) return NULL;
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
    if (!xr_parser_healthy(parser)) return NULL;
    XrAttribute **attributes = NULL;
    int attr_count = 0;
    int attr_capacity = 0;
    while (xr_parser_healthy(parser) && xr_parser_check(parser, TK_AT)) {
        XrAttribute *attribute = xr_parse_single_attribute(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!attribute)
            return NULL;
        if (!xr_parser_validate_member_attr(parser, attribute)) {
            return NULL;
        }
        do {
            XR_PARSE_PUSH(parser, attributes, attr_count, attr_capacity, attribute);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }
    if (!xr_parser_reject_duplicate_assertion_attrs(parser, attributes, attr_count))
        return NULL;

    XrParamMode receiver_mode = XR_PARAM_READ;
    if (xr_parser_match(parser, TK_REF) || xr_parser_match_name(parser, "ref")) {
        receiver_mode = XR_PARAM_REF;
    } else if (xr_parser_match(parser, TK_MOVE) || xr_parser_match_name(parser, "move")) {
        receiver_mode = XR_PARAM_MOVE;
    }

    if (xr_parser_match(parser, TK_OPERATOR)) {
        int member_line = parser->previous.line;
        do {
            xr_parser_consume(parser, TK_NAME, "expected named operator after 'operator'");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        if (parser->previous.length != 3 || xr_parser_compare_bytes(parser, parser->previous.start, "len", 3) != 0) {
            do {
                xr_parser_error(parser, "unsupported named operator; expected 'len'");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        do {
            xr_parser_consume(parser, TK_LPAREN, "expected '(' after 'operator len'");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            xr_parser_consume(parser, TK_RPAREN, "operator len does not accept parameters");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            xr_parser_consume(parser, TK_ARROW, "expected '->' after 'operator len()'");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        XrTypeRef *return_type = xr_parse_type_annotation(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        XrBorrowOriginSyntaxState borrow_origin_syntax = XR_BORROW_ORIGIN_OMITTED;
        AstBorrowOriginRef *borrow_origins = NULL;
        int borrow_origin_count = 0;
        do {
            xr_parse_borrow_origin_set(parser, &borrow_origin_syntax, &borrow_origins,
                                   &borrow_origin_count);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            xr_parser_match(parser, TK_SEMICOLON);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        AstNode *method = xr_ast_interface_method(
            parser->compiler_session, ast_strdup(parser->compiler_session, "__operator_len"), NULL,
            0, return_type, member_line);
        if (!xr_parser_healthy(parser)) return NULL;
        method->as.interface_method.attributes = attributes;
        method->as.interface_method.attr_count = attr_count;
        method->as.interface_method.receiver_mode = receiver_mode;
        method->as.interface_method.borrow_origin_syntax = borrow_origin_syntax;
        method->as.interface_method.borrow_origins = borrow_origins;
        method->as.interface_method.borrow_origin_count = borrow_origin_count;
        return method;
    }

    // Optional `const` modifier — only valid for property signatures.
    bool is_readonly = xr_parser_match(parser, TK_CONST);
    if (!xr_parser_healthy(parser)) return NULL;

    // Parse member name
    do {
        xr_parser_consume(parser, TK_NAME, "expected method or property name");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    char *member_name = xr_parser_token_string(parser, &parser->previous);
    if (!xr_parser_healthy(parser)) return NULL;
    int member_line = parser->previous.line;

    // Property signature: `name: type`
    if (xr_parser_check(parser, TK_COLON)) {
        if (receiver_mode != XR_PARAM_READ) {
            do {
                xr_parser_error(parser, "receiver mode applies only to interface methods");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        if (attr_count > 0) {
            do {
                xr_parser_error(parser, "assertion attributes cannot annotate an interface property");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);  // consume ':'
        XrTypeRef *prop_type = xr_parse_type_annotation(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        do {
            xr_parser_match(parser, TK_SEMICOLON);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);  // optional terminator
        return xr_ast_interface_property(parser->compiler_session, member_name, prop_type,
                                         is_readonly, member_line);
    }

    if (is_readonly) {
        do {
            xr_parser_error_at_current(parser, "'const' modifier only applies to property signatures");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    /* Optional generic type parameters: `wrap<T: Hashable>(x: T) -> int`.
     * Same shared helper as every other declaration form; it installs the scope
     * the signature resolves against and the caller restores its own below. */
    XrTypeScope *saved_scope = parser->type_scope;
    int type_param_count = 0;
    XrGenericParam **type_params = xr_parse_generic_params(parser, &type_param_count);
    if (!xr_parser_healthy(parser)) return NULL;
    XrTypeScope *generic_scope = type_param_count > 0 ? parser->type_scope : NULL;

    // Method signature: `name(params) -> retType`
    do {
        xr_parser_consume(parser, TK_LPAREN, "expected '(', '<' or ':' after interface member name");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    XrParamNode **params = NULL;
    int param_count = 0;
    int param_capacity = 0;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            XrParamNode *param = xr_parse_parameter(parser, XR_PARSE_PARAMETER_ALLOW_MODE |
                                                                XR_PARSE_PARAMETER_REQUIRE_TYPE);
            if (!xr_parser_healthy(parser)) return NULL;

            // Add to parameters array (arena grow - old buffers released by arena)
            if (param_count >= param_capacity) {
                int old_capacity = param_capacity;
                do {
                    param_capacity = xr_parser_grow_capacity(parser, param_capacity);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);

                XrParamNode **_new_params = (XrParamNode **) ast_alloc_array(
                    parser->compiler_session, sizeof(XrParamNode *), (size_t) param_capacity);
                if (!xr_parser_healthy(parser)) return NULL;
                if (old_capacity > 0 && params) {
                    do { if (!ast_copy(parser->compiler_session, _new_params, params, sizeof(XrParamNode *) * (size_t) old_capacity)) return NULL; } while (0);
                }
                params = _new_params;
            }
            params[param_count] = param;
            param_count++;

        } while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RPAREN));
    }

    do {
        xr_parser_consume(parser, TK_RPAREN, "expected ')'");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    // Parse return type (optional) — unified arrow `->`.
    XrTypeRef *return_type = NULL;
    XrBorrowOriginSyntaxState borrow_origin_syntax = XR_BORROW_ORIGIN_OMITTED;
    AstBorrowOriginRef *borrow_origins = NULL;
    int borrow_origin_count = 0;
    if (xr_parser_match(parser, TK_ARROW)) {
        do {
            return_type = xr_parse_type_annotation(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            xr_parse_borrow_origin_set(parser, &borrow_origin_syntax, &borrow_origins,
                                   &borrow_origin_count);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    } else if (xr_parser_check(parser, TK_COLON)) {
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            xr_parser_error(parser, "use '->' instead of ':' for interface method return type");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        parser->panic_mode = 0;
        do {
            return_type = xr_parse_type_annotation(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    /* Only method-local parameters may be subjects; bound types retain the full lexical scope. */
    do {
        xr_parse_where_clause(parser, type_params, type_param_count);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    // Interface method signature ends with semicolon (optional)
    do {
        xr_parser_match(parser, TK_SEMICOLON);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    AstNode *method = xr_ast_interface_method(parser->compiler_session, member_name, params,
                                              param_count, return_type, member_line);
    if (!xr_parser_healthy(parser)) return NULL;
    method->as.interface_method.attributes = attributes;
    method->as.interface_method.attr_count = attr_count;
    method->as.interface_method.type_params = type_params;
    method->as.interface_method.type_param_count = type_param_count;
    method->as.interface_method.receiver_mode = receiver_mode;
    method->as.interface_method.borrow_origin_syntax = borrow_origin_syntax;
    method->as.interface_method.borrow_origins = borrow_origins;
    method->as.interface_method.borrow_origin_count = borrow_origin_count;
    parser->type_scope = saved_scope;
    do {
        xr_parser_type_scope_free(parser, generic_scope);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    return method;
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
    if (!xr_parser_healthy(parser)) return NULL;
    int line = parser->previous.line;
    int name_column = parser->previous.column;  // column of 'constructor' keyword

    // Static constructor cannot have parameters
    do {
        xr_parser_consume(parser, TK_LPAREN, "expected '('");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            xr_parser_error(parser, "static constructor cannot have parameters");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        // Skip all parameters until ')'
        while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RPAREN) && !xr_parser_check(parser, TK_EOF)) {
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
    }

    do {
        xr_parser_consume(parser, TK_RPAREN, "expected ')'");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    // Static constructor cannot have return type
    if (xr_parser_check(parser, TK_COLON)) {
        do {
            xr_parser_error(parser, "static constructor cannot have return type");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);  // skip :
        // Skip type annotation
        if (xr_parser_check(parser, TK_NAME) || xr_parser_check(parser, TK_STRING) ||
            xr_parser_check(parser, TK_BOOL) ||
            (parser->current.type >= TK_I8 && parser->current.type <= TK_USIZE)) {
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
    }

    // Parse method body
    do {
        xr_parser_consume(parser, TK_LBRACE, "expected '{' to start static constructor body");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    AstNode *body = xr_parse_block(parser);
    if (!xr_parser_healthy(parser)) return NULL;

    // Create method declaration node
    AstNode *method_node =
        xr_ast_method_decl(parser->compiler_session, "<clinit>", NULL, 0,  // no parameters
                           NULL,                                           // no return type
                           body,
                           false,  // not a regular constructor
                           true,   // is static
                           is_private, false, false, line);
    if (!xr_parser_healthy(parser)) return NULL;

    method_node->column = name_column;
    if (body && body->end_line > 0) {
        method_node->end_line = body->end_line;
        method_node->end_column = body->end_column;
    } else {
        method_node->end_line = line;
        do {
            method_node->end_column = name_column + (int) xr_parser_string_length(parser, "<clinit>");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    // Mark as static constructor
    method_node->as.method_decl.is_static_constructor = true;

    return method_node;
}
