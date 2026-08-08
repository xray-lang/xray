/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_ast_visitor.c - Generic AST visitor implementation
 */

#include "xanalyzer_ast_visitor.h"
#include "../../base/xchecks.h"
#include <stdlib.h>

// Forward declaration
static void visit_node(AstNode *node, XaAstVisitor *v);

// Helper: call specific callback if set
static void call_callback(XaVisitFn fn, AstNode *node, XaAstVisitor *v) {
    if (fn)
        fn(node, v->user_ctx);
}

// Visit children of a node
static void visit_children(AstNode *node, XaAstVisitor *v) {
    if (!node || v->skip_children) {
        v->skip_children = false;
        return;
    }

    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++) {
                visit_node(node->as.program.statements[i], v);
            }
            break;

        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) {
                visit_node(node->as.block.statements[i], v);
            }
            break;

        case AST_FUNCTION_DECL:
            visit_node(node->as.function_decl.body, v);
            break;

        case AST_FUNCTION_EXPR:
            visit_node(node->as.function_expr.body, v);
            break;

        case AST_COMPTIME_EXPR:
            visit_node(node->as.comptime_expr.expr, v);
            break;

        case AST_VAR_DECL:
        case AST_CONST_DECL:
            visit_node(node->as.var_decl.initializer, v);
            break;

        case AST_DESTRUCTURE_DECL:
            visit_node(node->as.destructure_decl.initializer, v);
            break;

        case AST_CLASS_DECL:
            for (int i = 0; i < node->as.class_decl.field_count; i++) {
                visit_node(node->as.class_decl.fields[i], v);
            }
            for (int i = 0; i < node->as.class_decl.method_count; i++) {
                visit_node(node->as.class_decl.methods[i], v);
            }
            break;

        case AST_STRUCT_DECL:
            for (int i = 0; i < node->as.struct_decl.field_count; i++) {
                visit_node(node->as.struct_decl.fields[i], v);
            }
            for (int i = 0; i < node->as.struct_decl.method_count; i++) {
                visit_node(node->as.struct_decl.methods[i], v);
            }
            break;

        case AST_UNION_DECL:
            for (int i = 0; i < node->as.union_decl.field_count; i++) {
                visit_node(node->as.union_decl.fields[i], v);
            }
            break;

        case AST_METHOD_DECL:
            visit_node(node->as.method_decl.body, v);
            break;

        case AST_FIELD_DECL:
            visit_node(node->as.field_decl.initializer, v);
            break;

        case AST_IF_STMT:
            visit_node(node->as.if_stmt.condition, v);
            visit_node(node->as.if_stmt.then_branch, v);
            visit_node(node->as.if_stmt.else_branch, v);
            break;

        case AST_FOR_STMT:
            visit_node(node->as.for_stmt.initializer, v);
            visit_node(node->as.for_stmt.condition, v);
            visit_node(node->as.for_stmt.increment, v);
            visit_node(node->as.for_stmt.body, v);
            break;

        case AST_FOR_IN_STMT:
            visit_node(node->as.for_in_stmt.collection, v);
            visit_node(node->as.for_in_stmt.body, v);
            break;

        case AST_WHILE_STMT:
            visit_node(node->as.while_stmt.condition, v);
            visit_node(node->as.while_stmt.body, v);
            break;

        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++) {
                visit_node(node->as.return_stmt.values[i], v);
            }
            break;

        case AST_EXPR_STMT:
            visit_node(node->as.expr_stmt, v);
            break;

        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++) {
                visit_node(node->as.print_stmt.exprs[i], v);
            }
            break;

        case AST_ASSIGNMENT:
            visit_node(node->as.assignment.value, v);
            break;

        case AST_COMPOUND_ASSIGNMENT:
            visit_node(node->as.compound_assignment.object, v);
            visit_node(node->as.compound_assignment.value, v);
            break;

        case AST_DESTRUCTURE_ASSIGN:
            visit_node(node->as.destructure_assign.value, v);
            break;

        case AST_CALL_EXPR:
            visit_node(node->as.call_expr.callee, v);
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                visit_node(node->as.call_expr.arguments[i], v);
            }
            break;

        case AST_MEMBER_ACCESS:
            visit_node(node->as.member_access.object, v);
            break;

        case AST_MEMBER_SET:
            visit_node(node->as.member_set.object, v);
            visit_node(node->as.member_set.value, v);
            break;

        case AST_INDEX_GET:
            visit_node(node->as.index_get.array, v);
            visit_node(node->as.index_get.index, v);
            break;

        case AST_INDEX_SET:
            visit_node(node->as.index_set.array, v);
            visit_node(node->as.index_set.index, v);
            visit_node(node->as.index_set.value, v);
            break;

        case AST_ARRAY_LITERAL:
            if (node->as.array_literal.is_repeat) {
                visit_node(node->as.array_literal.repeat_value, v);
                visit_node(node->as.array_literal.repeat_count, v);
            } else {
                for (int i = 0; i < node->as.array_literal.count; i++) {
                    visit_node(node->as.array_literal.elements[i], v);
                }
            }
            break;

        case AST_TUPLE_LITERAL:
            for (int i = 0; i < node->as.tuple_literal.count; i++) {
                visit_node(node->as.tuple_literal.elements[i], v);
            }
            break;

        case AST_SPREAD_EXPR:
            visit_node(node->as.spread_expr.expr, v);
            break;

        case AST_STRUCT_LITERAL:
            for (int i = 0; i < node->as.struct_literal.field_count; i++) {
                visit_node(node->as.struct_literal.field_values[i], v);
            }
            break;

        case AST_OBJECT_LITERAL:
            for (int i = 0; i < node->as.object_literal.count; i++) {
                visit_node(node->as.object_literal.values[i], v);
            }
            break;

        case AST_MAP_LITERAL:
            for (int i = 0; i < node->as.map_literal.count; i++) {
                visit_node(node->as.map_literal.keys[i], v);
                visit_node(node->as.map_literal.values[i], v);
            }
            break;

        case AST_SET_LITERAL:
            for (int i = 0; i < node->as.set_literal.count; i++) {
                visit_node(node->as.set_literal.elements[i], v);
            }
            break;

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
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_NULLISH_COALESCE:
            visit_node(node->as.binary.left, v);
            visit_node(node->as.binary.right, v);
            break;

        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        case AST_FORCE_UNWRAP:
            visit_node(node->as.unary.operand, v);
            break;

        case AST_GROUPING:
            visit_node(node->as.grouping, v);
            break;

        case AST_TERNARY:
            visit_node(node->as.ternary.condition, v);
            visit_node(node->as.ternary.true_expr, v);
            visit_node(node->as.ternary.false_expr, v);
            break;

        case AST_OPTIONAL_CHAIN:
            visit_node(node->as.optional_chain.object, v);
            visit_node(node->as.optional_chain.index, v);
            break;

        case AST_RANGE:
            visit_node(node->as.range.start, v);
            visit_node(node->as.range.end, v);
            break;

        case AST_IS_EXPR:
            visit_node(node->as.is_expr.expr, v);
            break;

        case AST_AS_EXPR:
            visit_node(node->as.as_expr.expr, v);
            break;

        case AST_TRY_CATCH:
            visit_node(node->as.try_catch.try_body, v);
            for (int ci = 0; ci < node->as.try_catch.catch_count; ci++) {
                XrCatchClause *cc = node->as.try_catch.catch_clauses[ci];
                if (cc) {
                    visit_node(cc->pattern, v);
                    visit_node(cc->body, v);
                }
            }
            break;

        case AST_THROW_STMT:
            visit_node(node->as.throw_stmt.expression, v);
            break;

        case AST_GO_EXPR:
            visit_node(node->as.go_expr.expr, v);
            break;

        case AST_CHANNEL_NEW:
            visit_node(node->as.channel_new.buffer_size, v);
            break;

        case AST_UNSAFE_EXPR:
            visit_node(node->as.unsafe_expr.operand, v);
            break;
        case AST_AWAIT_EXPR:
            visit_node(node->as.await_expr.expr, v);
            visit_node(node->as.await_expr.timeout, v);
            visit_node(node->as.await_expr.into, v);
            break;

        case AST_YIELD_STMT:
            visit_node(node->as.yield_stmt.value, v);
            break;

        case AST_DEFER_STMT:
            visit_node(node->as.defer_stmt.expr, v);
            break;

        case AST_SCOPE_BLOCK:
            visit_node(node->as.scope_block.body, v);
            break;

        case AST_MOVE_EXPR:
            visit_node(node->as.move_expr.expr, v);
            break;

        case AST_SLICE_EXPR:
            visit_node(node->as.slice_expr.source, v);
            visit_node(node->as.slice_expr.start, v);
            visit_node(node->as.slice_expr.end, v);
            break;

        case AST_NEW_EXPR:
            for (int i = 0; i < node->as.new_expr.arg_count; i++) {
                visit_node(node->as.new_expr.arguments[i], v);
            }
            break;

        case AST_SUPER_CALL:
            for (int i = 0; i < node->as.super_call.arg_count; i++) {
                visit_node(node->as.super_call.arguments[i], v);
            }
            break;

        case AST_SELECT_STMT:
            for (int i = 0; i < node->as.select_stmt.case_count; i++) {
                visit_node(node->as.select_stmt.cases[i], v);
            }
            break;

        case AST_SELECT_CASE:
            visit_node(node->as.select_case.channel, v);
            visit_node(node->as.select_case.value, v);
            visit_node(node->as.select_case.body, v);
            break;

        case AST_MATCH_EXPR:
            visit_node(node->as.match_expr.expr, v);
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                visit_node(node->as.match_expr.arms[i], v);
            }
            break;

        case AST_MATCH_ARM:
            visit_node(node->as.match_arm.pattern, v);
            visit_node(node->as.match_arm.guard, v);
            visit_node(node->as.match_arm.body, v);
            break;

        case AST_PATTERN_LITERAL:
            visit_node(node->as.pattern_literal.value, v);
            break;

        case AST_PATTERN_RANGE:
            visit_node(node->as.pattern_range.start, v);
            visit_node(node->as.pattern_range.end, v);
            break;

        case AST_PATTERN_MULTI:
            for (int i = 0; i < node->as.pattern_multi.count; i++) {
                visit_node(node->as.pattern_multi.patterns[i], v);
            }
            break;

        case AST_PATTERN_TUPLE:
            for (int i = 0; i < node->as.pattern_tuple.count; i++) {
                visit_node(node->as.pattern_tuple.patterns[i], v);
            }
            break;

        case AST_PATTERN_OBJECT:
            for (int i = 0; i < node->as.pattern_object.count; i++) {
                visit_node(node->as.pattern_object.patterns[i], v);
            }
            break;

        case AST_PATTERN_ARRAY:
            for (int i = 0; i < node->as.pattern_array.count; i++) {
                visit_node(node->as.pattern_array.patterns[i], v);
            }
            break;

        case AST_PATTERN_ADT:
            visit_node(node->as.pattern_adt.variant, v);
            for (int i = 0; i < node->as.pattern_adt.count; i++) {
                visit_node(node->as.pattern_adt.patterns[i], v);
            }
            break;

        case AST_TEMPLATE_STRING:
            for (int i = 0; i < node->as.template_str.part_count; i++) {
                visit_node(node->as.template_str.parts[i], v);
            }
            break;

        case AST_ENUM_INDEX:
            visit_node(node->as.enum_index.collection, v);
            visit_node(node->as.enum_index.index_expr, v);
            break;

        case AST_EXPORT_STMT:
            /* Re-exports have no local AST child. */
            break;
        case AST_GLOBAL_ASM:
            break;

        default:
            break;
    }
}

// Main visit function for a single node
static void visit_node(AstNode *node, XaAstVisitor *v) {
    if (!node)
        return;

    // Pre-visit callback
    call_callback(v->visit_pre, node, v);

    // Type-specific callbacks
    switch (node->type) {
        case AST_FUNCTION_DECL:
            call_callback(v->visit_function_decl, node, v);
            break;
        case AST_VAR_DECL:
            call_callback(v->visit_var_decl, node, v);
            break;
        case AST_CONST_DECL:
            call_callback(v->visit_const_decl, node, v);
            break;
        case AST_CLASS_DECL:
            call_callback(v->visit_class_decl, node, v);
            break;
        case AST_STRUCT_DECL:
            call_callback(v->visit_class_decl, node, v);  // reuse class visitor
            break;
        case AST_UNION_DECL:
            call_callback(v->visit_class_decl, node, v);  // reuse class visitor
            break;
        case AST_METHOD_DECL:
            call_callback(v->visit_method_decl, node, v);
            break;
        case AST_FIELD_DECL:
            call_callback(v->visit_field_decl, node, v);
            break;
        case AST_INTERFACE_DECL:
            call_callback(v->visit_interface_decl, node, v);
            break;
        case AST_VARIABLE:
            call_callback(v->visit_variable, node, v);
            break;
        case AST_ASSIGNMENT:
            call_callback(v->visit_assignment, node, v);
            break;
        case AST_CALL_EXPR:
            call_callback(v->visit_call_expr, node, v);
            break;
        case AST_MEMBER_ACCESS:
            call_callback(v->visit_member_access, node, v);
            break;
        case AST_INDEX_GET:
            call_callback(v->visit_index_get, node, v);
            break;
        case AST_FUNCTION_EXPR:
            call_callback(v->visit_function_expr, node, v);
            break;
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_STRING:
        case AST_FIXED_BYTES_LITERAL:
        case AST_LITERAL_RUNE:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_LITERAL_NULL:
            call_callback(v->visit_literal, node, v);
            break;
        case AST_BLOCK:
            call_callback(v->visit_block, node, v);
            break;
        case AST_IF_STMT:
            call_callback(v->visit_if_stmt, node, v);
            break;
        case AST_FOR_STMT:
            call_callback(v->visit_for_stmt, node, v);
            break;
        case AST_FOR_IN_STMT:
            call_callback(v->visit_for_in_stmt, node, v);
            break;
        case AST_WHILE_STMT:
            call_callback(v->visit_while_stmt, node, v);
            break;
        case AST_RETURN_STMT:
            call_callback(v->visit_return_stmt, node, v);
            break;
        case AST_IMPORT_STMT:
            call_callback(v->visit_import_stmt, node, v);
            break;
        default:
            break;
    }

    // Visit children
    visit_children(node, v);

    // Post-visit callback
    call_callback(v->visit_post, node, v);
}

// Public API
void xa_ast_visit(AstNode *root, XaAstVisitor *visitor) {
    if (!root || !visitor)
        return;
    visit_node(root, visitor);
}

void xa_ast_walk(AstNode *root, XaVisitFn pre, XaVisitFn post, void *ctx) {
    XaAstVisitor v = {.visit_pre = pre, .visit_post = post, .user_ctx = ctx};
    xa_ast_visit(root, &v);
}

// Convenience: visit only declarations
static void decl_visitor(AstNode *node, void *ctx) {
    XaVisitFn fn = ((void **) ctx)[0];
    void *user_ctx = ((void **) ctx)[1];

    switch (node->type) {
        case AST_FUNCTION_DECL:
        case AST_VAR_DECL:
        case AST_CONST_DECL:
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
        case AST_UNION_DECL:
        case AST_METHOD_DECL:
        case AST_FIELD_DECL:
        case AST_INTERFACE_DECL:
            fn(node, user_ctx);
            break;
        default:
            break;
    }
}

void xa_ast_visit_decls(AstNode *root, XaVisitFn fn, void *ctx) {
    XR_DCHECK(fn != NULL, "xa_ast_visit_decls: NULL fn");
    void *wrapper_ctx[2] = {fn, ctx};
    xa_ast_walk(root, decl_visitor, NULL, wrapper_ctx);
}

// Convenience: visit only variable references
static void ref_visitor(AstNode *node, void *ctx) {
    XaVisitFn fn = ((void **) ctx)[0];
    void *user_ctx = ((void **) ctx)[1];

    if (node->type == AST_VARIABLE) {
        fn(node, user_ctx);
    }
}

void xa_ast_visit_refs(AstNode *root, XaVisitFn fn, void *ctx) {
    XR_DCHECK(fn != NULL, "xa_ast_visit_refs: NULL fn");
    void *wrapper_ctx[2] = {fn, ctx};
    xa_ast_walk(root, ref_visitor, NULL, wrapper_ctx);
}

// Convenience: visit only function calls
static void call_visitor(AstNode *node, void *ctx) {
    XaVisitFn fn = ((void **) ctx)[0];
    void *user_ctx = ((void **) ctx)[1];

    if (node->type == AST_CALL_EXPR) {
        fn(node, user_ctx);
    }
}

void xa_ast_visit_calls(AstNode *root, XaVisitFn fn, void *ctx) {
    XR_DCHECK(fn != NULL, "xa_ast_visit_calls: NULL fn");
    void *wrapper_ctx[2] = {fn, ctx};
    xa_ast_walk(root, call_visitor, NULL, wrapper_ctx);
}
