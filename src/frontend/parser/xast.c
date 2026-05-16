/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xast.c - AST node creation and manipulation
 *
 * KEY CONCEPT:
 *   Factory functions for creating all AST node types.
 */

#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "xast.h"
#include "../../base/xmalloc.h"
#include "../../base/xarena.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/value/xtype.h"
#include "xstring_pool.h"

#define INITIAL_CAPACITY 8

// Get arena from Isolate (explicit, no TLS)
static inline XrArena *get_arena(XrayIsolate *X) {
    return xr_isolate_get_current_arena(X);
}

// Arena-mandatory allocation helpers.
// All parser/AST allocations must go through these; a missing arena is a
// programming error and aborts via XR_CHECK.

XR_FUNC void *ast_alloc(XrayIsolate *X, size_t size) {
    XR_DCHECK(X != NULL, "ast_alloc: NULL isolate");
    XrArena *arena = get_arena(X);
    XR_CHECK(arena != NULL, "ast_alloc: parser requires an arena to be set "
                            "on the Isolate (call xr_isolate_set_current_arena "
                            "before parsing)");
    void *p = xr_arena_alloc(arena, size);
    XR_CHECK(p != NULL, "ast_alloc: arena allocation failed (out of memory)");
    return p;
}

XR_FUNC void *ast_alloc_array(XrayIsolate *X, size_t elem_size, size_t count) {
    XR_DCHECK(X != NULL, "ast_alloc_array: NULL isolate");
    if (count == 0)
        return NULL;
    return ast_alloc(X, elem_size * count);
}

XR_FUNC char *ast_strdup(XrayIsolate *X, const char *s) {
    if (!s)
        return NULL;
    /* Deduplicate via compile-time pool when available. */
    XrCompileStringPool *pool = xr_isolate_get_string_pool_compile(X);
    if (pool) {
        return (char *) xr_string_pool_intern(pool, s);
    }
    XrArena *arena = get_arena(X);
    XR_CHECK(arena != NULL, "ast_strdup: parser requires an arena");
    char *dup = xr_arena_strdup(arena, s);
    XR_CHECK(dup != NULL, "ast_strdup: arena allocation failed (out of memory)");
    return dup;
}

// Allocate zero-initialized AST node through the current arena.
static AstNode *alloc_node(XrayIsolate *X, AstNodeType type, int line) {
    XR_DCHECK(X != NULL, "alloc_node: NULL isolate");
    XR_DCHECK(line >= 0, "alloc_node: negative line");
    AstNode *node = (AstNode *) ast_alloc(X, sizeof(AstNode));
    memset(node, 0, sizeof(AstNode));
    node->type = type;
    node->line = line;
    node->node_id = xr_isolate_next_ast_node_id(X);
    return node;
}
AstNode *xr_ast_literal_int(XrayIsolate *X, xr_Integer value, int line) {
    AstNode *node = alloc_node(X, AST_LITERAL_INT, line);
    node->as.literal.kind = LITERAL_KIND_INT;
    node->as.literal.raw_value.int_val = value;  // Store raw value directly
    return node;
}

// Create float literal node
// Store raw value directly, no Runtime encoding dependency
AstNode *xr_ast_literal_float(XrayIsolate *X, xr_Number value, int line) {
    AstNode *node = alloc_node(X, AST_LITERAL_FLOAT, line);
    node->as.literal.kind = LITERAL_KIND_FLOAT;
    node->as.literal.raw_value.float_val = value;  // Store raw value directly
    return node;
}

// Create bigint literal node
// Store text representation (without 'n' suffix)
AstNode *xr_ast_literal_bigint(XrayIsolate *X, const char *value, int line) {
    AstNode *node = alloc_node(X, AST_LITERAL_BIGINT, line);
    node->as.literal.kind = LITERAL_KIND_BIGINT;
    node->as.literal.raw_value.bigint_val = ast_strdup(X, value);
    return node;
}

// Create string literal node
AstNode *xr_ast_literal_string(XrayIsolate *X, const char *value, int line) {
    AstNode *node = alloc_node(X, AST_LITERAL_STRING, line);
    node->as.literal.kind = LITERAL_KIND_STRING;

    node->as.literal.raw_value.string_val = ast_strdup(X, value);

    return node;
}

// Create regex literal node
// Store pattern and flags
AstNode *xr_ast_literal_regex(XrayIsolate *X, const char *pattern, const char *flags, int line) {
    AstNode *node = alloc_node(X, AST_LITERAL_REGEX, line);
    node->as.literal.kind = LITERAL_KIND_REGEX;

    node->as.literal.raw_value.regex.pattern = ast_strdup(X, pattern);
    node->as.literal.raw_value.regex.flags = ast_strdup(X, flags ? flags : "");

    return node;
}

// Create null literal node
// null doesn't need a value, only type marker
AstNode *xr_ast_literal_null(XrayIsolate *X, int line) {
    AstNode *node = alloc_node(X, AST_LITERAL_NULL, line);
    node->as.literal.kind = LITERAL_KIND_NULL;
    // null doesn't need a value
    return node;
}

// Create template string node
// parts: array of string fragments and expressions (alternating)
// part_count: number of parts
AstNode *xr_ast_template_string(XrayIsolate *X, AstNode **parts, int part_count, int line) {
    AstNode *node = alloc_node(X, AST_TEMPLATE_STRING, line);

    // Allocate and copy parts array
    node->as.template_str.parts =
        (AstNode **) ast_alloc_array(X, sizeof(AstNode *), (size_t) part_count);
    for (int i = 0; i < part_count; i++) {
        node->as.template_str.parts[i] = parts[i];
    }
    node->as.template_str.part_count = part_count;

    return node;
}

// Create bool literal node
// value: 0 for false, non-zero for true
AstNode *xr_ast_literal_bool(XrayIsolate *X, int value, int line) {
    AstNode *node = alloc_node(X, value ? AST_LITERAL_TRUE : AST_LITERAL_FALSE, line);
    node->as.literal.kind = LITERAL_KIND_BOOL;
    node->as.literal.raw_value.bool_val = (value != 0);  // Store bool value directly
    return node;
}

/* ========== Operator Node Creation ========== */

// Create binary operator node
// type: operator type (arithmetic, comparison, logical, etc.)
// left: left operand
// right: right operand
AstNode *xr_ast_binary(XrayIsolate *X, AstNodeType type, AstNode *left, AstNode *right, int line) {
    AstNode *node = alloc_node(X, type, line);
    node->as.binary.left = left;
    node->as.binary.right = right;
    return node;
}

// Create unary operator node
// type: operator type (negation, logical not)
// operand: operand
AstNode *xr_ast_unary(XrayIsolate *X, AstNodeType type, AstNode *operand, int line) {
    AstNode *node = alloc_node(X, type, line);
    node->as.unary.operand = operand;
    return node;
}

/* ========== Other Node Creation ========== */

// Create grouping node (parenthesized expression)
AstNode *xr_ast_grouping(XrayIsolate *X, AstNode *expr, int line) {
    AstNode *node = alloc_node(X, AST_GROUPING, line);
    node->as.grouping = expr;
    return node;
}

// Create expression statement node
AstNode *xr_ast_expr_stmt(XrayIsolate *X, AstNode *expr, int line) {
    AstNode *node = alloc_node(X, AST_EXPR_STMT, line);
    node->as.expr_stmt = expr;
    return node;
}

// Create print statement node (supports multiple arguments)
AstNode *xr_ast_print_stmt(XrayIsolate *X, AstNode **exprs, int expr_count, int line) {
    AstNode *node = alloc_node(X, AST_PRINT_STMT, line);

    // Copy expression array
    if (expr_count > 0 && exprs) {
        node->as.print_stmt.exprs =
            (AstNode **) ast_alloc_array(X, sizeof(AstNode *), (size_t) expr_count);
        for (int i = 0; i < expr_count; i++) {
            node->as.print_stmt.exprs[i] = exprs[i];
        }
    } else {
        node->as.print_stmt.exprs = NULL;
    }
    node->as.print_stmt.expr_count = expr_count;
    node->as.print_stmt.skip_null = false;

    return node;
}

/* ========== Program Node Operations ========== */

// Create program node
// Program node contains multiple statements
AstNode *xr_ast_program(XrayIsolate *iso) {
    AstNode *node = alloc_node(iso, AST_PROGRAM, 0);
    node->as.program.statements = NULL;
    node->as.program.count = 0;
    node->as.program.capacity = 0;
    return node;
}

// Add statement to program node
// Uses arena-based dynamic array; old buffer is not freed (arena bulk release).
void xr_ast_program_add(XrayIsolate *X, AstNode *program, AstNode *stmt) {
    // Ensure enough space
    if (program->as.program.count >= program->as.program.capacity) {
        int old_capacity = program->as.program.capacity;
        int new_capacity = old_capacity < INITIAL_CAPACITY ? INITIAL_CAPACITY : old_capacity * 2;

        AstNode **new_stmts =
            (AstNode **) ast_alloc_array(X, sizeof(AstNode *), (size_t) new_capacity);
        if (old_capacity > 0 && program->as.program.statements) {
            memcpy(new_stmts, program->as.program.statements,
                   sizeof(AstNode *) * (size_t) old_capacity);
        }
        program->as.program.statements = new_stmts;
        program->as.program.capacity = new_capacity;
    }

    // Add statement
    program->as.program.statements[program->as.program.count++] = stmt;
}

/* ========== Block Node Operations ========== */

// Create block node
// Block contains multiple statements
AstNode *xr_ast_block(XrayIsolate *X, int line) {
    AstNode *node = alloc_node(X, AST_BLOCK, line);
    node->as.block.statements = NULL;
    node->as.block.count = 0;
    node->as.block.capacity = 0;
    return node;
}

// Add statement to block
// Uses arena-based dynamic array; old buffer is not freed (arena bulk release).
void xr_ast_block_add(XrayIsolate *X, AstNode *block, AstNode *stmt) {
    // Ensure enough space
    if (block->as.block.count >= block->as.block.capacity) {
        int old_capacity = block->as.block.capacity;
        int new_capacity = old_capacity < INITIAL_CAPACITY ? INITIAL_CAPACITY : old_capacity * 2;

        AstNode **new_stmts =
            (AstNode **) ast_alloc_array(X, sizeof(AstNode *), (size_t) new_capacity);
        if (old_capacity > 0 && block->as.block.statements) {
            memcpy(new_stmts, block->as.block.statements,
                   sizeof(AstNode *) * (size_t) old_capacity);
        }
        block->as.block.statements = new_stmts;
        block->as.block.capacity = new_capacity;
    }

    // Add statement
    block->as.block.statements[block->as.block.count++] = stmt;
}

/* ========== Variable Node Creation ========== */

// Create variable declaration node
// name: variable name
// initializer: initialization expression (can be NULL)
// is_const: whether it's a constant
AstNode *xr_ast_var_decl(XrayIsolate *X, const char *name, AstNode *initializer, bool is_const,
                         int line) {
    AstNode *node = alloc_node(X, is_const ? AST_CONST_DECL : AST_VAR_DECL, line);
    node->as.var_decl.name = ast_strdup(X, name);
    node->as.var_decl.initializer = initializer;
    node->as.var_decl.is_const = is_const;
    node->as.var_decl.storage_mode = XR_STORAGE_NORMAL;
    node->as.var_decl.type_annotation = NULL;
    return node;
}

// Create variable declaration node with storage mode
// storage_mode: 0=normal, 1=shared
AstNode *xr_ast_var_decl_with_mode(XrayIsolate *X, const char *name, AstNode *initializer,
                                   bool is_const, uint8_t storage_mode, int line) {
    AstNode *node = alloc_node(X, is_const ? AST_CONST_DECL : AST_VAR_DECL, line);
    node->as.var_decl.name = ast_strdup(X, name);
    node->as.var_decl.initializer = initializer;
    node->as.var_decl.is_const = is_const;
    node->as.var_decl.storage_mode = storage_mode;
    node->as.var_decl.type_annotation = NULL;
    return node;
}

// Create variable reference node
// name: variable name
AstNode *xr_ast_variable(XrayIsolate *X, const char *name, int line) {
    AstNode *node = alloc_node(X, AST_VARIABLE, line);
    node->as.variable.name = ast_strdup(X, name);
    return node;
}

// Create assignment node
// name: variable name
// value: assignment expression
AstNode *xr_ast_assignment(XrayIsolate *X, const char *name, AstNode *value, int line) {
    AstNode *node = alloc_node(X, AST_ASSIGNMENT, line);
    node->as.assignment.name = ast_strdup(X, name);
    node->as.assignment.value = value;
    return node;
}

// Create compound assignment node
// name: variable name
// op: compound assignment operator type
// value: right-hand side expression
AstNode *xr_ast_compound_assignment(XrayIsolate *X, const char *name, XrTokenType op,
                                    AstNode *value, int line) {
    AstNode *node = alloc_node(X, AST_COMPOUND_ASSIGNMENT, line);
    node->as.compound_assignment.name = ast_strdup(X, name);
    node->as.compound_assignment.op = op;
    node->as.compound_assignment.value = value;
    node->as.compound_assignment.object = NULL;  // Regular variable compound assignment, no object
    return node;
}

// Create member compound assignment node
// object: object expression (e.g. this)
// name: member name
// op: compound assignment operator type
// value: right-hand side expression
AstNode *xr_ast_member_compound_assignment(XrayIsolate *X, AstNode *object, const char *name,
                                           XrTokenType op, AstNode *value, int line) {
    AstNode *node = alloc_node(X, AST_COMPOUND_ASSIGNMENT, line);
    node->as.compound_assignment.name = ast_strdup(X, name);
    node->as.compound_assignment.op = op;
    node->as.compound_assignment.value = value;
    node->as.compound_assignment.object = object;  // Member compound assignment, has object
    return node;
}

// Create increment node
// name: variable name
AstNode *xr_ast_inc(XrayIsolate *X, const char *name, int line) {
    AstNode *node = alloc_node(X, AST_INC, line);
    node->as.inc.name = ast_strdup(X, name);
    return node;
}

// Create decrement node
// name: variable name
AstNode *xr_ast_dec(XrayIsolate *X, const char *name, int line) {
    AstNode *node = alloc_node(X, AST_DEC, line);
    node->as.dec.name = ast_strdup(X, name);
    return node;
}

/* ========== Control Flow Node Creation ========== */

// Create if statement node
// condition: condition expression
// then_branch: then branch (must be block)
// else_branch: else branch (optional, can be block or if)
AstNode *xr_ast_if_stmt(XrayIsolate *X, AstNode *condition, AstNode *then_branch,
                        AstNode *else_branch, int line) {
    AstNode *node = alloc_node(X, AST_IF_STMT, line);
    node->as.if_stmt.condition = condition;
    node->as.if_stmt.then_branch = then_branch;
    node->as.if_stmt.else_branch = else_branch;
    return node;
}

// Create while loop node
// condition: loop condition
// body: loop body (must be block)
AstNode *xr_ast_while_stmt(XrayIsolate *X, AstNode *condition, AstNode *body, int line) {
    AstNode *node = alloc_node(X, AST_WHILE_STMT, line);
    node->as.while_stmt.condition = condition;
    node->as.while_stmt.body = body;
    return node;
}

// Create for loop node
// initializer: initialization (optional)
// condition: condition (optional)
// increment: update (optional)
// body: loop body (must be block)
AstNode *xr_ast_for_stmt(XrayIsolate *X, AstNode *initializer, AstNode *condition,
                         AstNode *increment, AstNode *body, int line) {
    AstNode *node = alloc_node(X, AST_FOR_STMT, line);
    node->as.for_stmt.initializer = initializer;
    node->as.for_stmt.condition = condition;
    node->as.for_stmt.increment = increment;
    node->as.for_stmt.body = body;
    return node;
}

// Create for-in loop node
// item_name: loop variable name (e.g. "item")
// item_type: optional type annotation (NULL for type inference)
// collection: collection expression to iterate
// body: loop body (must be block)
AstNode *xr_ast_for_in_stmt(XrayIsolate *X, const char *item_name, XrTypeRef *item_type,
                            AstNode *collection, AstNode *body, int line) {
    AstNode *node = alloc_node(X, AST_FOR_IN_STMT, line);

    node->as.for_in_stmt.item_name = ast_strdup(X, item_name);
    node->as.for_in_stmt.value_name = NULL;  // Single variable mode
    node->as.for_in_stmt.is_keyvalue = false;
    node->as.for_in_stmt.item_type = item_type;
    node->as.for_in_stmt.collection = collection;
    node->as.for_in_stmt.body = body;
    return node;
}

// Create for-in key-value loop node
// for (key, value in map) { body }
AstNode *xr_ast_for_in_keyvalue_stmt(XrayIsolate *X, const char *key_name, const char *value_name,
                                     XrTypeRef *item_type, AstNode *collection, AstNode *body,
                                     int line) {
    AstNode *node = alloc_node(X, AST_FOR_IN_STMT, line);

    node->as.for_in_stmt.item_name = ast_strdup(X, key_name);
    node->as.for_in_stmt.value_name = ast_strdup(X, value_name);
    node->as.for_in_stmt.is_keyvalue = true;  // Key-value pair mode
    node->as.for_in_stmt.item_type = item_type;
    node->as.for_in_stmt.collection = collection;
    node->as.for_in_stmt.body = body;
    return node;
}

// Create break statement node
AstNode *xr_ast_break_stmt(XrayIsolate *X, int line) {
    AstNode *node = alloc_node(X, AST_BREAK_STMT, line);
    node->as.break_stmt.placeholder = 0;
    return node;
}

// Create continue statement node
AstNode *xr_ast_continue_stmt(XrayIsolate *X, int line) {
    AstNode *node = alloc_node(X, AST_CONTINUE_STMT, line);
    node->as.continue_stmt.placeholder = 0;
    return node;
}

/* ========== Function Node Creation ========== */

// Create parameter node
XrParamNode *xr_param_node_new(XrayIsolate *X, const char *name, int line, int column) {
    (void) X;  // May be used for arena allocation in future
    XrParamNode *param = (XrParamNode *) ast_alloc(X, sizeof(XrParamNode));
    param->name = ast_strdup(X, name);
    param->line = line;
    param->column = column;
    param->passing_mode = XR_PARAM_VALUE;
    param->type = NULL;
    param->default_value = NULL;
    param->pattern = NULL;
    param->is_rest = false;
    return param;
}

// Create function declaration node
AstNode *xr_ast_function_decl(XrayIsolate *X, const char *name, XrParamNode **params,
                              int param_count, AstNode *body, int line) {
    AstNode *node = alloc_node(X, AST_FUNCTION_DECL, line);

    // Copy function name
    node->as.function_decl.name = ast_strdup(X, name);

    // Set parameters
    node->as.function_decl.params = params;
    node->as.function_decl.param_count = param_count;
    node->as.function_decl.required_count = param_count;  // Will be adjusted by parser

    node->as.function_decl.return_type = NULL;
    node->as.function_decl.is_generator = false;

    // Initialize attribute list to empty
    node->as.function_decl.attributes = NULL;
    node->as.function_decl.attr_count = 0;

    // Initialize generic type params
    node->as.function_decl.type_params = NULL;
    node->as.function_decl.type_param_count = 0;

    node->as.function_decl.body = body;
    return node;
}

// Create function expression node (arrow function/anonymous function)
AstNode *xr_ast_function_expr(XrayIsolate *X, XrParamNode **params, int param_count, AstNode *body,
                              int line) {
    AstNode *node = alloc_node(X, AST_FUNCTION_EXPR, line);

    // Anonymous function has no name
    node->as.function_expr.name = NULL;

    // Set parameters
    node->as.function_expr.params = params;
    node->as.function_expr.param_count = param_count;
    node->as.function_expr.required_count = param_count;

    node->as.function_expr.return_type = NULL;
    node->as.function_expr.is_generator = false;

    // Initialize attribute list to empty
    node->as.function_expr.attributes = NULL;
    node->as.function_expr.attr_count = 0;

    // Initialize generic type params
    node->as.function_expr.type_params = NULL;
    node->as.function_expr.type_param_count = 0;

    node->as.function_expr.body = body;
    return node;
}

// Create function call node
// callee: expression being called (usually a variable)
// arguments: argument list (expression array)
// arg_count: argument count
AstNode *xr_ast_call_expr(XrayIsolate *X, AstNode *callee, AstNode **arguments, int arg_count,
                          int line) {
    AstNode *node = alloc_node(X, AST_CALL_EXPR, line);
    node->as.call_expr.callee = callee;
    node->as.call_expr.arg_count = arg_count;

    // Copy parameter list
    if (arg_count > 0) {
        node->as.call_expr.arguments =
            (AstNode **) ast_alloc_array(X, sizeof(AstNode *), (size_t) arg_count);
        for (int i = 0; i < arg_count; i++) {
            node->as.call_expr.arguments[i] = arguments[i];
        }
    } else {
        node->as.call_expr.arguments = NULL;
    }

    // No generic type arguments
    node->as.call_expr.type_args = NULL;
    node->as.call_expr.type_arg_count = 0;

    return node;
}

// Create function call node with generic type arguments
// e.g.: foo<int, string>(arg1, arg2)
AstNode *xr_ast_call_expr_generic(XrayIsolate *X, AstNode *callee, AstNode **arguments,
                                  int arg_count, XrTypeRef **type_args, int type_arg_count,
                                  int line) {
    AstNode *node = alloc_node(X, AST_CALL_EXPR, line);
    node->as.call_expr.callee = callee;
    node->as.call_expr.arg_count = arg_count;

    // Copy parameter list
    if (arg_count > 0) {
        node->as.call_expr.arguments =
            (AstNode **) ast_alloc_array(X, sizeof(AstNode *), (size_t) arg_count);
        for (int i = 0; i < arg_count; i++) {
            node->as.call_expr.arguments[i] = arguments[i];
        }
    } else {
        node->as.call_expr.arguments = NULL;
    }

    // Copy generic type arguments
    node->as.call_expr.type_arg_count = type_arg_count;
    if (type_arg_count > 0) {
        node->as.call_expr.type_args =
            (XrTypeRef **) ast_alloc_array(X, sizeof(XrTypeRef *), (size_t) type_arg_count);
        for (int i = 0; i < type_arg_count; i++) {
            node->as.call_expr.type_args[i] = type_args[i];
        }
    } else {
        node->as.call_expr.type_args = NULL;
    }

    return node;
}

// Create return statement node (multi-value return)
// values: return value expression array
// count: return value count (0 means no return value)
AstNode *xr_ast_return_stmt(XrayIsolate *X, AstNode **values, int count, int line) {
    AstNode *node = alloc_node(X, AST_RETURN_STMT, line);
    node->as.return_stmt.values = values;
    node->as.return_stmt.value_count = count;
    return node;
}

// Create is expression node (runtime type check)
AstNode *xr_ast_is_expr(XrayIsolate *X, AstNode *expr, XrTypeRef *type, int line) {
    AstNode *node = alloc_node(X, AST_IS_EXPR, line);
    node->as.is_expr.expr = expr;
    node->as.is_expr.type = type;
    return node;
}

AstNode *xr_ast_as_expr(XrayIsolate *X, AstNode *expr, XrTypeRef *type, bool is_safe, int line) {
    AstNode *node = alloc_node(X, AST_AS_EXPR, line);
    node->as.as_expr.expr = expr;
    node->as.as_expr.type = type;
    node->as.as_expr.is_safe = is_safe;
    return node;
}

/* ========== Array Node Creation ========== */

// Create array literal node
// elements: element expression array
// count: element count
AstNode *xr_ast_array_literal(XrayIsolate *X, AstNode **elements, int count, int line) {
    AstNode *node = alloc_node(X, AST_ARRAY_LITERAL, line);
    node->as.array_literal.count = count;

    // Copy element array
    if (count > 0) {
        node->as.array_literal.elements =
            (AstNode **) ast_alloc_array(X, sizeof(AstNode *), (size_t) count);
        for (int i = 0; i < count; i++) {
            node->as.array_literal.elements[i] = elements[i];
        }
    } else {
        node->as.array_literal.elements = NULL;
    }

    // compile_type set by type inference, not here (avoid analyzer pool dependency)

    return node;
}

// Create tuple literal node: `()`, `(x,)`, or `(a, b, ...)`. Type
// inference fills in compile_type from the inferred element types
// (or the unit singleton for count == 0).
AstNode *xr_ast_tuple_literal(XrayIsolate *X, AstNode **elements, int count, int line) {
    AstNode *node = alloc_node(X, AST_TUPLE_LITERAL, line);
    node->as.tuple_literal.count = count;

    if (count > 0) {
        node->as.tuple_literal.elements =
            (AstNode **) ast_alloc_array(X, sizeof(AstNode *), (size_t) count);
        for (int i = 0; i < count; i++) {
            node->as.tuple_literal.elements[i] = elements[i];
        }
    } else {
        node->as.tuple_literal.elements = NULL;
    }
    return node;
}

// Create object literal node (static structure)
// keys: key expression array (usually string literals)
// values: value expression array
// computed: computed property flag array (can be NULL)
// count: key-value pair count
AstNode *xr_ast_object_literal(XrayIsolate *X, AstNode **keys, AstNode **values, bool *computed,
                               int count, int line) {
    AstNode *node = alloc_node(X, AST_OBJECT_LITERAL, line);
    node->as.object_literal.count = count;

    // Copy key-value pair array
    if (count > 0) {
        node->as.object_literal.keys =
            (AstNode **) ast_alloc_array(X, sizeof(AstNode *), (size_t) count);
        node->as.object_literal.values =
            (AstNode **) ast_alloc_array(X, sizeof(AstNode *), (size_t) count);
        for (int i = 0; i < count; i++) {
            node->as.object_literal.keys[i] = keys[i];
            node->as.object_literal.values[i] = values[i];
        }
        // Copy computed property flags (if any)
        if (computed) {
            node->as.object_literal.computed =
                (bool *) ast_alloc_array(X, sizeof(bool), (size_t) count);
            for (int i = 0; i < count; i++) {
                node->as.object_literal.computed[i] = computed[i];
            }
        } else {
            node->as.object_literal.computed = NULL;
        }
    } else {
        node->as.object_literal.keys = NULL;
        node->as.object_literal.values = NULL;
        node->as.object_literal.computed = NULL;
    }

    return node;
}

// Create Map literal node (dynamic container)
// keys: key expression array
// values: value expression array
// count: key-value pair count
AstNode *xr_ast_map_literal(XrayIsolate *X, AstNode **keys, AstNode **values, int count, int line) {
    AstNode *node = alloc_node(X, AST_MAP_LITERAL, line);
    node->as.map_literal.count = count;

    // Copy key array
    if (count > 0) {
        node->as.map_literal.keys =
            (AstNode **) ast_alloc_array(X, sizeof(AstNode *), (size_t) count);
        node->as.map_literal.values =
            (AstNode **) ast_alloc_array(X, sizeof(AstNode *), (size_t) count);
        for (int i = 0; i < count; i++) {
            node->as.map_literal.keys[i] = keys[i];
            node->as.map_literal.values[i] = values[i];
        }
    } else {
        node->as.map_literal.keys = NULL;
        node->as.map_literal.values = NULL;
    }

    // compile_type set by type inference, not here (avoid analyzer pool dependency)

    return node;
}

// Create Set literal node
// elements: element expression array
// count: element count
AstNode *xr_ast_set_literal(XrayIsolate *X, AstNode **elements, int count, int line) {
    AstNode *node = alloc_node(X, AST_SET_LITERAL, line);
    node->as.set_literal.count = count;

    // Copy element array
    if (count > 0) {
        node->as.set_literal.elements =
            (AstNode **) ast_alloc_array(X, sizeof(AstNode *), (size_t) count);
        for (int i = 0; i < count; i++) {
            node->as.set_literal.elements[i] = elements[i];
        }
    } else {
        node->as.set_literal.elements = NULL;
    }

    return node;
}

// Create index access node
// array: array expression
// index: index expression
AstNode *xr_ast_index_get(XrayIsolate *X, AstNode *array, AstNode *index, int line) {
    AstNode *node = alloc_node(X, AST_INDEX_GET, line);
    node->as.index_get.array = array;
    node->as.index_get.index = index;
    return node;
}

// Create index assignment node
// array: array expression
// index: index expression
// value: assignment expression
AstNode *xr_ast_index_set(XrayIsolate *X, AstNode *array, AstNode *index, AstNode *value,
                          int line) {
    AstNode *node = alloc_node(X, AST_INDEX_SET, line);
    node->as.index_set.array = array;
    node->as.index_set.index = index;
    node->as.index_set.value = value;
    return node;
}

// Create slice expression node
// source: source object expression (Array, String, Bytes)
// start: start index expression (can be NULL)
// end: end index expression (can be NULL)
AstNode *xr_ast_slice_expr(XrayIsolate *X, AstNode *source, AstNode *start, AstNode *end,
                           int line) {
    AstNode *node = alloc_node(X, AST_SLICE_EXPR, line);
    node->as.slice_expr.source = source;
    node->as.slice_expr.start = start;
    node->as.slice_expr.end = end;
    return node;
}

// Create member access node
// object: object expression
// name: member name
AstNode *xr_ast_member_access(XrayIsolate *X, AstNode *object, const char *name, int line) {
    AstNode *node = alloc_node(X, AST_MEMBER_ACCESS, line);
    node->as.member_access.object = object;
    node->as.member_access.name = ast_strdup(X, name);
    return node;
}

// Create ternary expression node
// condition: condition expression
// true_expr: true branch expression
// false_expr: false branch expression
AstNode *xr_ast_ternary(XrayIsolate *X, AstNode *condition, AstNode *true_expr, AstNode *false_expr,
                        int line) {
    AstNode *node = alloc_node(X, AST_TERNARY, line);
    node->as.ternary.condition = condition;
    node->as.ternary.true_expr = true_expr;
    node->as.ternary.false_expr = false_expr;
    return node;
}

// Create optional chain node
// object: object expression
// name: member name (for property access, optional)
// index: index expression (for index access, optional)
// chain_type: 0=property, 1=index, 2=method call
AstNode *xr_ast_optional_chain(XrayIsolate *X, AstNode *object, const char *name, AstNode *index,
                               int chain_type, int line) {
    AstNode *node = alloc_node(X, AST_OPTIONAL_CHAIN, line);
    node->as.optional_chain.object = object;
    if (name) {
        node->as.optional_chain.name = ast_strdup(X, name);
    } else {
        node->as.optional_chain.name = NULL;
    }
    node->as.optional_chain.index = index;
    node->as.optional_chain.chain_type = chain_type;
    return node;
}

// Create range expression node
// start: start value expression
// end: end value expression
AstNode *xr_ast_range(XrayIsolate *X, AstNode *start, AstNode *end, int line) {
    AstNode *node = alloc_node(X, AST_RANGE, line);
    node->as.range.start = start;
    node->as.range.end = end;
    return node;
}

/* ========== AST Free ========== */

/* ========== OOP Node Creation ========== */

// Create class declaration node
AstNode *xr_ast_class_decl(XrayIsolate *X, const char *name, const char *super_name,
                           AstNode **fields, int field_count, AstNode **methods, int method_count,
                           int line) {
    AstNode *node = alloc_node(X, AST_CLASS_DECL, line);
    node->as.class_decl.name = (char *) name;
    node->as.class_decl.super_name = (char *) super_name;
    node->as.class_decl.super_module = NULL;  // No module prefix by default
    node->as.class_decl.interfaces = NULL;
    node->as.class_decl.interface_count = 0;
    node->as.class_decl.fields = fields;
    node->as.class_decl.field_count = field_count;
    node->as.class_decl.methods = methods;
    node->as.class_decl.method_count = method_count;
    node->as.class_decl.is_abstract = false;
    return node;
}

// Create struct declaration node (value type)
AstNode *xr_ast_struct_decl(XrayIsolate *X, const char *name, AstNode **fields, int field_count,
                            AstNode **methods, int method_count, int line) {
    AstNode *node = alloc_node(X, AST_STRUCT_DECL, line);
    node->as.struct_decl.name = (char *) name;
    node->as.struct_decl.super_name = NULL;
    node->as.struct_decl.super_module = NULL;
    node->as.struct_decl.interfaces = NULL;
    node->as.struct_decl.interface_count = 0;
    node->as.struct_decl.fields = fields;
    node->as.struct_decl.field_count = field_count;
    node->as.struct_decl.methods = methods;
    node->as.struct_decl.method_count = method_count;
    node->as.struct_decl.is_abstract = false;
    node->as.struct_decl.is_final = true;  // structs are implicitly final
    node->as.struct_decl.type_params = NULL;
    node->as.struct_decl.type_param_count = 0;
    return node;
}

// Create struct literal node: Point{x: 1.0, y: 2.0}
AstNode *xr_ast_struct_literal(XrayIsolate *X, const char *name, char **field_names,
                               AstNode **field_values, int field_count, int line) {
    AstNode *node = alloc_node(X, AST_STRUCT_LITERAL, line);
    node->as.struct_literal.struct_name = (char *) name;
    node->as.struct_literal.field_names = field_names;
    node->as.struct_literal.field_values = field_values;
    node->as.struct_literal.field_count = field_count;
    return node;
}

// Create interface declaration node
AstNode *xr_ast_interface_decl(XrayIsolate *X, const char *name, XrTypeRef **extends,
                               int extends_count, AstNode **methods, int method_count,
                               AstNode **properties, int property_count,
                               XrGenericParam **type_params, int type_param_count, int line) {
    AstNode *node = alloc_node(X, AST_INTERFACE_DECL, line);
    node->as.interface_decl.name = (char *) name;
    node->as.interface_decl.extends = extends;
    node->as.interface_decl.extends_count = extends_count;
    node->as.interface_decl.methods = methods;
    node->as.interface_decl.method_count = method_count;
    node->as.interface_decl.properties = properties;
    node->as.interface_decl.property_count = property_count;
    node->as.interface_decl.type_params = type_params;
    node->as.interface_decl.type_param_count = type_param_count;
    return node;
}

// Create interface method signature node
AstNode *xr_ast_interface_method(XrayIsolate *X, const char *name, char **parameters,
                                 XrTypeRef **param_types, int param_count, XrTypeRef *return_type,
                                 int line) {
    AstNode *node = alloc_node(X, AST_INTERFACE_METHOD, line);
    node->as.interface_method.name = (char *) name;
    node->as.interface_method.parameters = parameters;
    node->as.interface_method.param_types = param_types;
    node->as.interface_method.param_count = param_count;
    node->as.interface_method.return_type = return_type;
    return node;
}

// Create interface property signature node
AstNode *xr_ast_interface_property(XrayIsolate *X, const char *name, XrTypeRef *prop_type,
                                   bool is_readonly, int line) {
    AstNode *node = alloc_node(X, AST_INTERFACE_PROPERTY, line);
    node->as.interface_property.name = (char *) name;
    node->as.interface_property.prop_type = prop_type;
    node->as.interface_property.is_readonly = is_readonly;
    return node;
}

// Create field declaration node
AstNode *xr_ast_field_decl(XrayIsolate *X, const char *name, XrTypeRef *field_type, bool is_private,
                           bool is_static, AstNode *initializer, int line) {
    AstNode *node = alloc_node(X, AST_FIELD_DECL, line);
    node->as.field_decl.name = (char *) name;
    node->as.field_decl.field_type = field_type;
    node->as.field_decl.is_private = is_private;
    node->as.field_decl.is_static = is_static;
    node->as.field_decl.initializer = initializer;
    return node;
}

// Create method declaration node
AstNode *xr_ast_method_decl(XrayIsolate *X, const char *name, char **parameters,
                            XrTypeRef **param_types, int param_count, XrTypeRef *return_type,
                            AstNode *body, bool is_constructor, bool is_static, bool is_private,
                            bool is_getter, bool is_setter, int line) {
    AstNode *node = alloc_node(X, AST_METHOD_DECL, line);
    node->as.method_decl.name = (char *) name;
    node->as.method_decl.parameters = parameters;
    node->as.method_decl.param_types = param_types;
    node->as.method_decl.param_count = param_count;
    node->as.method_decl.return_type = return_type;
    node->as.method_decl.body = body;
    node->as.method_decl.is_constructor = is_constructor;
    node->as.method_decl.is_static = is_static;
    node->as.method_decl.is_private = is_private;
    node->as.method_decl.is_getter = is_getter;
    node->as.method_decl.is_setter = is_setter;
    node->as.method_decl.is_abstract = false;
    node->as.method_decl.is_static_constructor = false;  // Not a static constructor by default

    // Initialize base() call fields
    node->as.method_decl.base_args = NULL;
    node->as.method_decl.base_arg_count = 0;

    // Initialize default parameter fields
    node->as.method_decl.default_values = NULL;
    node->as.method_decl.param_passing_modes = NULL;

    node->as.method_decl.is_operator = false;  // Ensure is_operator is initialized
    node->as.method_decl.op_type =
        OPTYPE_ADD;  // Default value (doesn't matter since is_operator=false)

    // Initialize generic type parameters
    node->as.method_decl.type_param_names = NULL;
    node->as.method_decl.type_param_count = 0;
    return node;
}

// Create new expression node
// Supports two forms:
//   new ClassName()           - module_name = NULL
//   new module.ClassName()    - module_name = "module"
// Also supports generic type arguments: new Box<int>(42)
AstNode *xr_ast_new_expr(XrayIsolate *X, const char *module_name, const char *class_name,
                         AstNode **arguments, int arg_count, XrTypeRef **type_args,
                         int type_arg_count, int line) {
    AstNode *node = alloc_node(X, AST_NEW_EXPR, line);
    node->as.new_expr.module_name = ast_strdup(X, module_name);
    node->as.new_expr.class_name = (char *) class_name;
    node->as.new_expr.arguments = arguments;
    node->as.new_expr.arg_count = arg_count;

    // Copy generic type arguments
    node->as.new_expr.type_arg_count = type_arg_count;
    if (type_arg_count > 0 && type_args) {
        node->as.new_expr.type_args =
            (XrTypeRef **) ast_alloc_array(X, sizeof(XrTypeRef *), (size_t) type_arg_count);
        for (int i = 0; i < type_arg_count; i++) {
            node->as.new_expr.type_args[i] = type_args[i];
        }
    } else {
        node->as.new_expr.type_args = NULL;
    }
    return node;
}

// Create this expression node
AstNode *xr_ast_this_expr(XrayIsolate *X, int line) {
    AstNode *node = alloc_node(X, AST_THIS_EXPR, line);
    node->as.this_expr.placeholder = 0;
    return node;
}

// Create super call node
AstNode *xr_ast_super_call(XrayIsolate *X, const char *method_name, AstNode **arguments,
                           int arg_count, int line) {
    AstNode *node = alloc_node(X, AST_SUPER_CALL, line);
    node->as.super_call.method_name = (char *) method_name;
    node->as.super_call.arguments = arguments;
    node->as.super_call.arg_count = arg_count;
    return node;
}

// Create member assignment node
AstNode *xr_ast_member_set(XrayIsolate *X, AstNode *object, const char *member, AstNode *value,
                           int line) {
    AstNode *node = alloc_node(X, AST_MEMBER_SET, line);
    node->as.member_set.object = object;
    node->as.member_set.member = ast_strdup(X, member);  // Copy string to avoid dangling pointer
    node->as.member_set.value = value;
    return node;
}

/* ========== Enum Node Creation ========== */

// Create enum declaration node
// enum Status : int { Success = 200, Error = 500 }
AstNode *xr_ast_enum_decl(XrayIsolate *X, const char *name, const char *type_hint,
                          AstNode **members, int member_count, int line) {
    AstNode *node = alloc_node(X, AST_ENUM_DECL, line);
    node->as.enum_decl.name = ast_strdup(X, name);
    node->as.enum_decl.type_hint = ast_strdup(X, type_hint);

    // Copy member array
    node->as.enum_decl.members =
        (AstNode **) ast_alloc_array(X, sizeof(AstNode *), (size_t) member_count);
    for (int i = 0; i < member_count; i++) {
        node->as.enum_decl.members[i] = members[i];
    }
    node->as.enum_decl.member_count = member_count;

    return node;
}

// Create enum member node
// Success = 200
AstNode *xr_ast_enum_member(XrayIsolate *X, const char *name, AstNode *value, int line) {
    AstNode *node = alloc_node(X, AST_ENUM_MEMBER, line);
    node->as.enum_member.name = ast_strdup(X, name);
    node->as.enum_member.value = value;  // Can be NULL (auto-increment)
    return node;
}

// Create enum access node
// Status.Success
AstNode *xr_ast_enum_access(XrayIsolate *X, const char *enum_name, const char *member_name,
                            int line) {
    AstNode *node = alloc_node(X, AST_ENUM_ACCESS, line);
    node->as.enum_access.enum_name = ast_strdup(X, enum_name);
    node->as.enum_access.member_name = ast_strdup(X, member_name);
    return node;
}

// Create enum conversion node
// Status(200)
AstNode *xr_ast_enum_convert(XrayIsolate *X, const char *enum_name, AstNode *value_expr, int line) {
    AstNode *node = alloc_node(X, AST_ENUM_CONVERT, line);
    node->as.enum_convert.enum_name = ast_strdup(X, enum_name);
    node->as.enum_convert.value_expr = value_expr;
    return node;
}

// Create enum index node (compiler-generated for for-in desugaring)
AstNode *xr_ast_enum_index(XrayIsolate *X, AstNode *collection, AstNode *index_expr, int line) {
    AstNode *node = alloc_node(X, AST_ENUM_INDEX, line);
    node->as.enum_index.collection = collection;
    node->as.enum_index.index_expr = index_expr;
    return node;
}

/* ========== Match Expression Node Creation ========== */

// Create match expression node
// match x { 1 => "one", _ => "other" }
AstNode *xr_ast_match_expr(XrayIsolate *X, AstNode *expr, AstNode **arms, int arm_count, int line) {
    AstNode *node = alloc_node(X, AST_MATCH_EXPR, line);
    node->as.match_expr.expr = expr;
    node->as.match_expr.arms = arms;
    node->as.match_expr.arm_count = arm_count;
    return node;
}

// Create match arm node
// 1 => "one"
AstNode *xr_ast_match_arm(XrayIsolate *X, AstNode *pattern, AstNode *guard, AstNode *body,
                          int line) {
    AstNode *node = alloc_node(X, AST_MATCH_ARM, line);
    node->as.match_arm.pattern = pattern;
    node->as.match_arm.guard = guard;
    node->as.match_arm.body = body;
    return node;
}

// Create literal pattern node
// 1, "hello", true, HttpStatus.OK
AstNode *xr_ast_pattern_literal(XrayIsolate *X, AstNode *value, int line) {
    AstNode *node = alloc_node(X, AST_PATTERN_LITERAL, line);
    node->as.pattern_literal.value = value;
    return node;
}

// Create range pattern node
// 1..10
AstNode *xr_ast_pattern_range(XrayIsolate *X, AstNode *start, AstNode *end, int line) {
    AstNode *node = alloc_node(X, AST_PATTERN_RANGE, line);
    node->as.pattern_range.start = start;
    node->as.pattern_range.end = end;
    return node;
}

// Create wildcard pattern node
// _
AstNode *xr_ast_pattern_wildcard(XrayIsolate *X, int line) {
    AstNode *node = alloc_node(X, AST_PATTERN_WILDCARD, line);
    return node;
}

// Create multi-value pattern node
// 1, 2, 3
AstNode *xr_ast_pattern_multi(XrayIsolate *X, AstNode **patterns, int count, int line) {
    AstNode *node = alloc_node(X, AST_PATTERN_MULTI, line);
    node->as.pattern_multi.patterns = patterns;
    node->as.pattern_multi.count = count;
    return node;
}

// Destroy a program AST and its owning arena.
// Releases every AST node, array, and string allocated during parsing in O(1).
// For program nodes from xr_parse_recoverable (LSP), the caller owns the
// arena and this call is a no-op.
// The program node itself lives inside the arena, so we capture the arena
// pointer into a local BEFORE xr_arena_destroy frees the segments.
void xr_program_destroy(AstNode *program) {
    if (program == NULL)
        return;
    XR_CHECK(program->type == AST_PROGRAM, "xr_program_destroy: expected AST_PROGRAM node");

    ProgramNode *prog = &program->as.program;
    if (prog->owns_arena && prog->arena) {
        XrArena *arena = prog->arena;
        xr_arena_destroy(arena);
        xr_free(arena);
    }
}

/* ========== Debug and Utility Functions ========== */

// Get node type name
// Used for debugging and error reporting
const char *xr_ast_typename(AstNodeType type) {
    switch (type) {
        case AST_LITERAL_INT:
            return "LiteralInt";
        case AST_LITERAL_FLOAT:
            return "LiteralFloat";
        case AST_LITERAL_STRING:
            return "LiteralString";
        case AST_LITERAL_REGEX:
            return "LiteralRegex";
        case AST_LITERAL_NULL:
            return "LiteralNull";
        case AST_LITERAL_TRUE:
            return "LiteralTrue";
        case AST_LITERAL_FALSE:
            return "LiteralFalse";
        case AST_BINARY_ADD:
            return "BinaryAdd";
        case AST_BINARY_SUB:
            return "BinarySub";
        case AST_BINARY_MUL:
            return "BinaryMul";
        case AST_BINARY_DIV:
            return "BinaryDiv";
        case AST_BINARY_MOD:
            return "BinaryMod";
        case AST_BINARY_EQ:
            return "BinaryEq";
        case AST_BINARY_NE:
            return "BinaryNe";
        case AST_BINARY_LT:
            return "BinaryLt";
        case AST_BINARY_LE:
            return "BinaryLe";
        case AST_BINARY_GT:
            return "BinaryGt";
        case AST_BINARY_GE:
            return "BinaryGe";
        case AST_BINARY_AND:
            return "BinaryAnd";
        case AST_BINARY_OR:
            return "BinaryOr";
        case AST_UNARY_NEG:
            return "UnaryNeg";
        case AST_UNARY_NOT:
            return "UnaryNot";
        case AST_GROUPING:
            return "Grouping";
        case AST_EXPR_STMT:
            return "ExprStmt";
        case AST_PRINT_STMT:
            return "PrintStmt";
        case AST_BLOCK:
            return "Block";
        case AST_VAR_DECL:
            return "VarDecl";
        case AST_CONST_DECL:
            return "ConstDecl";
        case AST_VARIABLE:
            return "Variable";
        case AST_ASSIGNMENT:
            return "Assignment";
        case AST_COMPOUND_ASSIGNMENT:
            return "CompoundAssignment";
        case AST_INC:
            return "Inc";
        case AST_DEC:
            return "Dec";
        case AST_IF_STMT:
            return "IfStmt";

        case AST_WHILE_STMT:
            return "WhileStmt";
        case AST_FOR_STMT:
            return "ForStmt";
        case AST_BREAK_STMT:
            return "BreakStmt";
        case AST_CONTINUE_STMT:
            return "ContinueStmt";
        case AST_FUNCTION_DECL:
            return "FunctionDecl";
        case AST_FUNCTION_EXPR:
            return "FunctionExpr";
        case AST_CALL_EXPR:
            return "CallExpr";
        case AST_RETURN_STMT:
            return "ReturnStmt";
        case AST_ARRAY_LITERAL:
            return "ArrayLiteral";
        case AST_OBJECT_LITERAL:
            return "ObjectLiteral";
        case AST_MAP_LITERAL:
            return "MapLiteral";
        case AST_SET_LITERAL:
            return "SetLiteral";
        case AST_INDEX_GET:
            return "IndexGet";
        case AST_INDEX_SET:
            return "IndexSet";
        case AST_MEMBER_ACCESS:
            return "MemberAccess";
        case AST_TEMPLATE_STRING:
            return "TemplateString";
        case AST_CLASS_DECL:
            return "ClassDecl";
        case AST_STRUCT_DECL:
            return "StructDecl";
        case AST_STRUCT_LITERAL:
            return "StructLiteral";
        case AST_FIELD_DECL:
            return "FieldDecl";
        case AST_METHOD_DECL:
            return "MethodDecl";
        case AST_NEW_EXPR:
            return "NewExpr";
        case AST_THIS_EXPR:
            return "ThisExpr";
        case AST_SUPER_CALL:
            return "SuperCall";
        case AST_MEMBER_SET:
            return "MemberSet";
        case AST_ENUM_DECL:
            return "EnumDecl";
        case AST_ENUM_MEMBER:
            return "EnumMember";
        case AST_ENUM_ACCESS:
            return "EnumAccess";
        case AST_ENUM_CONVERT:
            return "EnumConvert";
        case AST_ENUM_INDEX:
            return "EnumIndex";
        case AST_TRY_CATCH:
            return "TryCatch";
        case AST_THROW_STMT:
            return "ThrowStmt";
        case AST_IMPORT_STMT:
            return "ImportStmt";
        case AST_EXPORT_STMT:
            return "ExportStmt";
        case AST_DESTRUCTURE_DECL:
            return "DestructureDecl";
        case AST_DESTRUCTURE_ASSIGN:
            return "DestructureAssign";
        case AST_MATCH_EXPR:
            return "MatchExpr";
        case AST_MATCH_ARM:
            return "MatchArm";
        case AST_PATTERN_LITERAL:
            return "PatternLiteral";
        case AST_PATTERN_RANGE:
            return "PatternRange";
        case AST_PATTERN_WILDCARD:
            return "PatternWildcard";
        case AST_PATTERN_MULTI:
            return "PatternMulti";
        case AST_TYPE_ALIAS:
            return "TypeAlias";
        case AST_PROGRAM:
            return "Program";
        default:
            return "Unknown";
    }
}

// Print AST structure (for debugging)
// indent: indentation level
void xr_ast_print(AstNode *node, int indent) {
    if (node == NULL) {
        printf("%*sNULL\n", indent * 2, "");
        return;
    }

    // Print indentation
    printf("%*s", indent * 2, "");

    // Print node type
    printf("%s", xr_ast_typename(node->type));

    // Print node details - print raw values
    switch (node->type) {
        case AST_LITERAL_INT:
            printf("(%lld)", (long long) node->as.literal.raw_value.int_val);  // Print raw value
            break;
        case AST_LITERAL_FLOAT:
            printf("(%g)", node->as.literal.raw_value.float_val);  // Print raw value
            break;
        case AST_LITERAL_STRING:
            printf("(\"%s\")", node->as.literal.raw_value.string_val);  // Print C string
            break;
        case AST_LITERAL_REGEX:
            printf("(/%s/%s)", node->as.literal.raw_value.regex.pattern,
                   node->as.literal.raw_value.regex.flags);
            break;
        case AST_LITERAL_NULL:
            printf("(null)");
            break;
        case AST_LITERAL_TRUE:
            printf("(true)");
            break;
        case AST_LITERAL_FALSE:
            printf("(false)");
            break;
        default:
            break;
    }

    printf("\n");

    // Recursively print child nodes
    switch (node->type) {
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
            xr_ast_print(node->as.binary.left, indent + 1);
            xr_ast_print(node->as.binary.right, indent + 1);
            break;

        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
            xr_ast_print(node->as.unary.operand, indent + 1);
            break;

        case AST_GROUPING:
            xr_ast_print(node->as.grouping, indent + 1);
            break;

        case AST_EXPR_STMT:
            xr_ast_print(node->as.expr_stmt, indent + 1);
            break;

        case AST_PRINT_STMT: {
            PrintNode *print = &node->as.print_stmt;
            for (int i = 0; i < print->expr_count; i++) {
                xr_ast_print(print->exprs[i], indent + 1);
            }
            break;
        }

        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) {
                xr_ast_print(node->as.block.statements[i], indent + 1);
            }
            break;

        case AST_VAR_DECL:
        case AST_CONST_DECL:
            printf("%*s  name: %s\n", indent * 2, "", node->as.var_decl.name);
            if (node->as.var_decl.initializer != NULL) {
                printf("%*s  initializer:\n", indent * 2, "");
                xr_ast_print(node->as.var_decl.initializer, indent + 2);
            }
            break;

        case AST_VARIABLE:
            printf("%*s  name: %s\n", indent * 2, "", node->as.variable.name);
            break;

        case AST_ASSIGNMENT:
            printf("%*s  name: %s\n", indent * 2, "", node->as.assignment.name);
            printf("%*s  value:\n", indent * 2, "");
            xr_ast_print(node->as.assignment.value, indent + 2);
            break;

        case AST_COMPOUND_ASSIGNMENT:
            printf("%*s  name: %s\n", indent * 2, "", node->as.compound_assignment.name);
            printf("%*s  op: %d\n", indent * 2, "", node->as.compound_assignment.op);
            printf("%*s  value:\n", indent * 2, "");
            xr_ast_print(node->as.compound_assignment.value, indent + 2);
            break;

        case AST_INC:
            printf("%*s  name: %s\n", indent * 2, "", node->as.inc.name);
            break;

        case AST_DEC:
            printf("%*s  name: %s\n", indent * 2, "", node->as.dec.name);
            break;

        case AST_IF_STMT:
            printf("%*s  condition:\n", indent * 2, "");
            xr_ast_print(node->as.if_stmt.condition, indent + 2);
            printf("%*s  then:\n", indent * 2, "");
            xr_ast_print(node->as.if_stmt.then_branch, indent + 2);
            if (node->as.if_stmt.else_branch != NULL) {
                printf("%*s  else:\n", indent * 2, "");
                xr_ast_print(node->as.if_stmt.else_branch, indent + 2);
            }
            break;

        case AST_WHILE_STMT:
            printf("%*s  condition:\n", indent * 2, "");
            xr_ast_print(node->as.while_stmt.condition, indent + 2);
            printf("%*s  body:\n", indent * 2, "");
            xr_ast_print(node->as.while_stmt.body, indent + 2);
            break;

        case AST_FOR_STMT:
            if (node->as.for_stmt.initializer != NULL) {
                printf("%*s  initializer:\n", indent * 2, "");
                xr_ast_print(node->as.for_stmt.initializer, indent + 2);
            }
            if (node->as.for_stmt.condition != NULL) {
                printf("%*s  condition:\n", indent * 2, "");
                xr_ast_print(node->as.for_stmt.condition, indent + 2);
            }
            if (node->as.for_stmt.increment != NULL) {
                printf("%*s  increment:\n", indent * 2, "");
                xr_ast_print(node->as.for_stmt.increment, indent + 2);
            }
            printf("%*s  body:\n", indent * 2, "");
            xr_ast_print(node->as.for_stmt.body, indent + 2);
            break;

        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT:
            // No extra info to print
            break;

        // Function related nodes
        case AST_FUNCTION_DECL:
            printf(" (name: %s, params: ", node->as.function_decl.name);
            for (int i = 0; i < node->as.function_decl.param_count; i++) {
                XrParamNode *p = node->as.function_decl.params[i];
                printf("%s", p ? p->name : "?");
                if (i < node->as.function_decl.param_count - 1) {
                    printf(", ");
                }
            }
            printf(")\n");
            if (node->as.function_decl.body != NULL) {
                xr_ast_print(node->as.function_decl.body, indent + 1);
            }
            break;

        case AST_FUNCTION_EXPR:
            printf(" (params: ");
            for (int i = 0; i < node->as.function_expr.param_count; i++) {
                XrParamNode *p = node->as.function_expr.params[i];
                printf("%s", p ? p->name : "?");
                if (i < node->as.function_expr.param_count - 1) {
                    printf(", ");
                }
            }
            printf(")\n");
            if (node->as.function_expr.body != NULL) {
                printf("%*s  body:\n", indent * 2, "");
                xr_ast_print(node->as.function_expr.body, indent + 2);
            }
            break;

        case AST_CALL_EXPR:
            printf("\n");
            // Print callee
            if (node->as.call_expr.callee != NULL) {
                xr_ast_print(node->as.call_expr.callee, indent + 1);
            }
            // Print arguments
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                xr_ast_print(node->as.call_expr.arguments[i], indent + 1);
            }
            break;

        case AST_RETURN_STMT:
            printf(" [%d values]\n", node->as.return_stmt.value_count);
            for (int i = 0; i < node->as.return_stmt.value_count; i++) {
                xr_ast_print(node->as.return_stmt.values[i], indent + 1);
            }
            break;

        // Array related nodes
        case AST_ARRAY_LITERAL:
            printf(" [%d elements]\n", node->as.array_literal.count);
            for (int i = 0; i < node->as.array_literal.count; i++) {
                printf("%*s", (indent + 1) * 2, "");
                printf("Element %d:", i);
                xr_ast_print(node->as.array_literal.elements[i], indent + 2);
            }
            break;

        // Data structure literal nodes
        case AST_OBJECT_LITERAL:
            printf(" {%d pairs} (object)\n", node->as.object_literal.count);
            for (int i = 0; i < node->as.object_literal.count; i++) {
                printf("%*s", (indent + 1) * 2, "");
                printf("Key %d:", i);
                xr_ast_print(node->as.object_literal.keys[i], indent + 2);
                printf("%*s", (indent + 1) * 2, "");
                printf("Value %d:", i);
                xr_ast_print(node->as.object_literal.values[i], indent + 2);
            }
            break;

        case AST_MAP_LITERAL:
            printf(" {%d pairs} (map)\n", node->as.map_literal.count);
            for (int i = 0; i < node->as.map_literal.count; i++) {
                printf("%*s", (indent + 1) * 2, "");
                printf("Key %d:", i);
                xr_ast_print(node->as.map_literal.keys[i], indent + 2);
                printf("%*s", (indent + 1) * 2, "");
                printf("Value %d:", i);
                xr_ast_print(node->as.map_literal.values[i], indent + 2);
            }
            break;

        case AST_SET_LITERAL:
            printf(" #[%d elements] (set)\n", node->as.set_literal.count);
            for (int i = 0; i < node->as.set_literal.count; i++) {
                xr_ast_print(node->as.set_literal.elements[i], indent + 1);
            }
            break;

        case AST_INDEX_GET:
            printf("\n");
            printf("%*s", (indent + 1) * 2, "");
            printf("Array:");
            xr_ast_print(node->as.index_get.array, indent + 2);
            printf("%*s", (indent + 1) * 2, "");
            printf("Index:");
            xr_ast_print(node->as.index_get.index, indent + 2);
            break;

        case AST_INDEX_SET:
            printf("\n");
            printf("%*s", (indent + 1) * 2, "");
            printf("Array:");
            xr_ast_print(node->as.index_set.array, indent + 2);
            printf("%*s", (indent + 1) * 2, "");
            printf("Index:");
            xr_ast_print(node->as.index_set.index, indent + 2);
            printf("%*s", (indent + 1) * 2, "");
            printf("Value:");
            xr_ast_print(node->as.index_set.value, indent + 2);
            break;

        case AST_MEMBER_ACCESS:
            printf(" .%s\n", node->as.member_access.name);
            printf("%*s", (indent + 1) * 2, "");
            printf("Object:");
            xr_ast_print(node->as.member_access.object, indent + 2);
            break;

        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++) {
                xr_ast_print(node->as.program.statements[i], indent + 1);
            }
            break;

        case AST_TRY_CATCH:
            printf("\n");
            printf("%*sTry Block:\n", (indent + 1) * 2, "");
            xr_ast_print(node->as.try_catch.try_body, indent + 2);
            if (node->as.try_catch.catch_body) {
                printf("%*sCatch Block (var: %s):\n", (indent + 1) * 2, "",
                       node->as.try_catch.catch_var ? node->as.try_catch.catch_var : "");
                xr_ast_print(node->as.try_catch.catch_body, indent + 2);
            }
            if (node->as.try_catch.finally_body) {
                printf("%*sFinally Block:\n", (indent + 1) * 2, "");
                xr_ast_print(node->as.try_catch.finally_body, indent + 2);
            }
            break;

        case AST_THROW_STMT:
            printf("\n");
            printf("%*sExpression:\n", (indent + 1) * 2, "");
            xr_ast_print(node->as.throw_stmt.expression, indent + 2);
            break;

        default:
            break;
    }
}

/* ========== Exception Handling AST Node Creation ========== */

// Create try-catch-finally statement node
AstNode *xr_ast_try_catch(XrayIsolate *X, AstNode *try_body, const char *catch_var,
                          int catch_var_line, int catch_var_column, AstNode *catch_body,
                          AstNode *finally_body, int line) {
    AstNode *node = alloc_node(X, AST_TRY_CATCH, line);
    node->as.try_catch.try_body = try_body;
    node->as.try_catch.catch_var = ast_strdup(X, catch_var);
    node->as.try_catch.catch_var_line = catch_var_line;
    node->as.try_catch.catch_var_column = catch_var_column;
    node->as.try_catch.catch_body = catch_body;
    node->as.try_catch.finally_body = finally_body;
    return node;
}

// Create throw statement node
AstNode *xr_ast_throw_stmt(XrayIsolate *X, AstNode *expression, int line) {
    AstNode *node = alloc_node(X, AST_THROW_STMT, line);
    node->as.throw_stmt.expression = expression;
    return node;
}

/* ========== Module System AST Node Creation ========== */

// Create import statement node
// import http              - standard library
// import xray/redis        - third-party package
// import "./utils"         - local module
// import xray/redis as r   - rename
AstNode *xr_ast_import_stmt(XrayIsolate *X, const char *module_name, const char *alias,
                            ImportType import_type, int line) {
    AstNode *node = alloc_node(X, AST_IMPORT_STMT, line);
    node->as.import_stmt.module_name = ast_strdup(X, module_name);
    node->as.import_stmt.alias = ast_strdup(X, alias);
    node->as.import_stmt.import_type = import_type;
    node->as.import_stmt.members = NULL;
    node->as.import_stmt.member_count = 0;
    return node;
}

// Create import statement node (extended version, supports selective import)
// import { add, multiply } from "utils"
AstNode *xr_ast_import_stmt_ex(XrayIsolate *X, const char *module_name, const char *alias,
                               ImportType import_type, ImportMember *members, int member_count,
                               int line) {
    AstNode *node = alloc_node(X, AST_IMPORT_STMT, line);
    node->as.import_stmt.module_name = ast_strdup(X, module_name);
    node->as.import_stmt.alias = ast_strdup(X, alias);
    node->as.import_stmt.import_type = import_type;
    node->as.import_stmt.members = members;  // Take ownership, don't copy
    node->as.import_stmt.member_count = member_count;
    return node;
}

// Create export statement node
// export fn add() {}
// export let PI = 3.14
// export class User {}
AstNode *xr_ast_export_stmt(XrayIsolate *X, AstNode *declaration, const char *export_name,
                            int line) {
    AstNode *node = alloc_node(X, AST_EXPORT_STMT, line);
    node->as.export_stmt.declaration = declaration;
    node->as.export_stmt.export_name = export_name ? ast_strdup(X, export_name) : NULL;
    node->as.export_stmt.export_names = NULL;
    node->as.export_stmt.export_count = 0;
    node->as.export_stmt.from_path = NULL;
    node->as.export_stmt.reexport_members = NULL;
    node->as.export_stmt.reexport_count = 0;
    node->as.export_stmt.is_reexport_all = false;
    return node;
}

// Create export list statement node
// export a, b, c
AstNode *xr_ast_export_list(XrayIsolate *X, char **names, int count, int line) {
    AstNode *node = alloc_node(X, AST_EXPORT_STMT, line);
    node->as.export_stmt.declaration = NULL;
    node->as.export_stmt.export_name = NULL;
    node->as.export_stmt.export_names = names;
    node->as.export_stmt.export_count = count;
    node->as.export_stmt.from_path = NULL;
    node->as.export_stmt.reexport_members = NULL;
    node->as.export_stmt.reexport_count = 0;
    node->as.export_stmt.is_reexport_all = false;
    return node;
}

// Create re-export statement node
// export { a, b as c } from "./file"
// export * from "./file"
AstNode *xr_ast_export_reexport(XrayIsolate *X, const char *from_path, ReexportMember *members,
                                int count, bool is_all, int line) {
    AstNode *node = alloc_node(X, AST_EXPORT_STMT, line);
    node->as.export_stmt.declaration = NULL;
    node->as.export_stmt.export_name = NULL;
    node->as.export_stmt.export_names = NULL;
    node->as.export_stmt.export_count = 0;
    node->as.export_stmt.from_path = from_path ? ast_strdup(X, from_path) : NULL;
    node->as.export_stmt.reexport_members = members;
    node->as.export_stmt.reexport_count = count;
    node->as.export_stmt.is_reexport_all = is_all;
    return node;
}

/* ========== Destructuring Implementation ========== */

// Create array destructuring pattern
// let [a, b, c] = arr
XrDestructurePattern *xr_pattern_array(XrayIsolate *X, XrDestructurePattern **elements, int count) {
    (void) X;
    XrDestructurePattern *pattern =
        (XrDestructurePattern *) ast_alloc(X, sizeof(XrDestructurePattern));
    pattern->type = PATTERN_ARRAY;
    pattern->as.array.elements = elements;
    pattern->as.array.element_count = count;
    return pattern;
}

// Create object destructuring pattern (curly braces)
// let {name, age} = person or let {name: userName} = person
XrDestructurePattern *xr_pattern_object(XrayIsolate *X, char **fields,
                                        XrDestructurePattern **patterns, int count,
                                        bool use_shorthand) {
    (void) X;
    XrDestructurePattern *pattern =
        (XrDestructurePattern *) ast_alloc(X, sizeof(XrDestructurePattern));
    pattern->type = PATTERN_OBJECT;
    pattern->as.object.field_names = fields;
    pattern->as.object.patterns = patterns;
    pattern->as.object.field_count = count;
    pattern->as.object.use_shorthand = use_shorthand;
    return pattern;
}

// Create identifier destructuring pattern
// a or a: int
XrDestructurePattern *xr_pattern_identifier(XrayIsolate *X, const char *name, XrTypeRef *type) {
    (void) X;
    XrDestructurePattern *pattern =
        (XrDestructurePattern *) ast_alloc(X, sizeof(XrDestructurePattern));
    pattern->type = PATTERN_IDENTIFIER;
    pattern->as.identifier.name = ast_strdup(X, name);
    pattern->as.identifier.type = type;
    return pattern;
}

// Create skip element pattern
// _
XrDestructurePattern *xr_pattern_skip(XrayIsolate *X) {
    XrDestructurePattern *pattern =
        (XrDestructurePattern *) ast_alloc(X, sizeof(XrDestructurePattern));
    pattern->type = PATTERN_SKIP;
    return pattern;
}

// Create destructuring declaration node
// let [a, b] = arr or const {x, y} = obj
AstNode *xr_ast_destructure_decl(XrayIsolate *X, XrDestructurePattern *pattern,
                                 AstNode *initializer, bool is_const, int line) {
    AstNode *node = alloc_node(X, AST_DESTRUCTURE_DECL, line);
    node->as.destructure_decl.pattern = pattern;
    node->as.destructure_decl.initializer = initializer;
    node->as.destructure_decl.is_const = is_const;
    return node;
}

// Create destructuring assignment node
// [a, b] = arr or {x, y} = obj
AstNode *xr_ast_destructure_assign(XrayIsolate *X, XrDestructurePattern *pattern, AstNode *value,
                                   int line) {
    AstNode *node = alloc_node(X, AST_DESTRUCTURE_ASSIGN, line);
    node->as.destructure_assign.pattern = pattern;
    node->as.destructure_assign.value = value;
    return node;
}

// Create multi-value declaration node
// let a, b = 1, 2  or  let a, b = foo()  or  let a, b, c
AstNode *xr_ast_multi_var_decl(XrayIsolate *X, char **names, int name_count, AstNode **values,
                               int value_count, bool is_const, int line) {
    AstNode *node = alloc_node(X, AST_MULTI_VAR_DECL, line);
    node->as.multi_var_decl.names = names;
    node->as.multi_var_decl.name_count = name_count;
    node->as.multi_var_decl.values = values;
    node->as.multi_var_decl.value_count = value_count;
    node->as.multi_var_decl.is_const = is_const;
    return node;
}

// Create multi-value assignment node
// a, b = b, a
AstNode *xr_ast_multi_assign(XrayIsolate *X, AstNode **targets, int target_count, AstNode **values,
                             int value_count, int line) {
    AstNode *node = alloc_node(X, AST_MULTI_ASSIGN, line);
    node->as.multi_assign.targets = targets;
    node->as.multi_assign.target_count = target_count;
    node->as.multi_assign.values = values;
    node->as.multi_assign.value_count = value_count;
    return node;
}

/* ========== Match Expression AST Node Creation ========== */

/* ========== Type Alias Node ========== */

// Create type alias node
// type User = { name: string, age: int, email?: string }
AstNode *xr_ast_type_alias(XrayIsolate *X, const char *name, char **field_names,
                           XrTypeRef **field_types, bool *field_optional, int field_count,
                           int line) {
    AstNode *node = alloc_node(X, AST_TYPE_ALIAS, line);

    // Copy type name
    node->as.type_alias.name = ast_strdup(X, name);
    node->as.type_alias.field_count = field_count;

    if (field_count > 0) {
        // Copy field names array
        node->as.type_alias.field_names =
            (char **) ast_alloc_array(X, sizeof(char *), (size_t) field_count);
        for (int i = 0; i < field_count; i++) {
            node->as.type_alias.field_names[i] = ast_strdup(X, field_names[i]);
        }

        // Copy field types array (shallow copy, types managed by type pool)
        node->as.type_alias.field_types =
            (XrTypeRef **) ast_alloc_array(X, sizeof(XrTypeRef *), (size_t) field_count);
        memcpy(node->as.type_alias.field_types, field_types, sizeof(XrTypeRef *) * field_count);

        // Copy optional flags array
        node->as.type_alias.field_optional =
            (bool *) ast_alloc_array(X, sizeof(bool), (size_t) field_count);
        memcpy(node->as.type_alias.field_optional, field_optional, sizeof(bool) * field_count);
    } else {
        node->as.type_alias.field_names = NULL;
        node->as.type_alias.field_types = NULL;
        node->as.type_alias.field_optional = NULL;
    }

    return node;
}

/* ========== Coroutine AST Node Creation ========== */

// Create go expression node
// go fn() or go { block } or go(name: "xxx") fn() or go(priority: Coro.HIGH) fn()
// linked go fn() or monitored go fn()
AstNode *xr_ast_go_expr(XrayIsolate *X, AstNode *expr, const char *name, AstNode *priority,
                        uint8_t link_mode, int line) {
    AstNode *node = alloc_node(X, AST_GO_EXPR, line);
    node->as.go_expr.expr = expr;
    node->as.go_expr.name = name;
    node->as.go_expr.priority = priority;
    node->as.go_expr.link_mode = link_mode;
    return node;
}

// Create await expression node
// await task, await(timeout: N) task, await all/await any/await anySuccess [tasks]
AstNode *xr_ast_await_expr(XrayIsolate *X, AstNode *expr, AstNode *timeout, bool is_any,
                           bool is_all, bool is_any_success, int line) {
    AstNode *node = alloc_node(X, AST_AWAIT_EXPR, line);
    node->as.await_expr.expr = expr;
    node->as.await_expr.timeout = timeout;
    node->as.await_expr.is_any = is_any;
    node->as.await_expr.is_all = is_all;
    node->as.await_expr.is_any_success = is_any_success;
    return node;
}

// Create Channel creation node
// Channel() or Channel(10)
AstNode *xr_ast_channel_new(XrayIsolate *X, AstNode *buffer_size, int line) {
    AstNode *node = alloc_node(X, AST_CHANNEL_NEW, line);
    node->as.channel_new.buffer_size = buffer_size;
    return node;
}

// Create select case node
// msg from ch => expr or ch.send(val) => expr
AstNode *xr_ast_select_case(XrayIsolate *X, const char *var_name, AstNode *channel, AstNode *value,
                            AstNode *body, bool is_send, bool is_default, bool is_timeout,
                            int line) {
    AstNode *node = alloc_node(X, AST_SELECT_CASE, line);
    node->as.select_case.var_name = var_name ? ast_strdup(X, var_name) : NULL;
    node->as.select_case.channel = channel;
    node->as.select_case.value = value;
    node->as.select_case.body = body;
    node->as.select_case.is_send = is_send;
    node->as.select_case.is_default = is_default;
    node->as.select_case.is_timeout = is_timeout;
    return node;
}

// Create select statement node
// select { msg from ch => ..., _ => ... }
AstNode *xr_ast_select_stmt(XrayIsolate *X, AstNode **cases, int case_count, int line) {
    AstNode *node = alloc_node(X, AST_SELECT_STMT, line);

    if (case_count > 0 && cases != NULL) {
        node->as.select_stmt.cases =
            (AstNode **) ast_alloc_array(X, sizeof(AstNode *), (size_t) case_count);
        memcpy(node->as.select_stmt.cases, cases, sizeof(AstNode *) * case_count);
    } else {
        node->as.select_stmt.cases = NULL;
    }
    node->as.select_stmt.case_count = case_count;

    return node;
}

// Create defer statement node
// defer fn() or defer { block }
AstNode *xr_ast_defer_stmt(XrayIsolate *X, AstNode *expr, int line) {
    AstNode *node = alloc_node(X, AST_DEFER_STMT, line);
    node->as.defer_stmt.expr = expr;
    return node;
}

// Create scope block node
// scope { ... } or linked scope { ... } or supervisor scope { ... }
AstNode *xr_ast_scope_block(XrayIsolate *X, AstNode *body, uint8_t scope_mode, int line) {
    AstNode *node = alloc_node(X, AST_SCOPE_BLOCK, line);
    node->as.scope_block.body = body;
    node->as.scope_block.scope_mode = scope_mode;
    return node;
}

// Create yield statement node
// yield - yields execution
AstNode *xr_ast_yield_stmt(XrayIsolate *X, int line) {
    AstNode *node = alloc_node(X, AST_YIELD_STMT, line);
    return node;
}

// Create cancelled() expression node
AstNode *xr_ast_cancelled_expr(XrayIsolate *X, int line) {
    AstNode *node = alloc_node(X, AST_CANCELLED_EXPR, line);
    node->as.cancelled_expr.placeholder = 0;
    return node;
}

// Create move expression node
AstNode *xr_ast_move_expr(XrayIsolate *X, AstNode *expr, int line, int column) {
    AstNode *node = alloc_node(X, AST_MOVE_EXPR, line);
    node->column = column;
    node->as.move_expr.expr = expr;
    return node;
}
