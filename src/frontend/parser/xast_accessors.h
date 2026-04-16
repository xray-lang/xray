/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xast_accessors.h - Safe AST union accessor macros
 */

#ifndef XAST_ACCESSORS_H
#define XAST_ACCESSORS_H

/* ========================================================================
 * Safe AST union accessors (debug assert, release zero-cost)
 *
 * Usage:  BinaryNode *bin = AST_AS_BINARY(node);
 *         CallExprNode *call = AST_AS_CALL(node);
 *
 * In debug builds, asserts that node->type matches the expected AST type.
 * In release builds, compiles to a plain pointer cast with no overhead.
 * ======================================================================== */

#include <assert.h>

#ifdef NDEBUG
#define XR_AST_ASSERT(node, expected_type) ((void)0)
#define XR_AST_ASSERT_RANGE(node, lo, hi) ((void)0)
#else
#define XR_AST_ASSERT(node, expected_type) \
    assert((node) && (node)->type == (expected_type) && \
           "AST type mismatch: accessed wrong union field")
#define XR_AST_ASSERT_RANGE(node, lo, hi) \
    assert((node) && (node)->type >= (lo) && (node)->type <= (hi) && \
           "AST type mismatch: node type outside expected range")
#endif

#define AST_AS_BINARY(n)       (XR_AST_ASSERT_RANGE(n, AST_BINARY_ADD, AST_BINARY_OR), &(n)->as.binary)
#define AST_AS_UNARY(n)        (XR_AST_ASSERT_RANGE(n, AST_UNARY_NEG, AST_UNARY_BNOT), &(n)->as.unary)
#define AST_AS_LITERAL(n)      (XR_AST_ASSERT_RANGE(n, AST_LITERAL_INT, AST_LITERAL_FALSE), &(n)->as.literal)
#define AST_AS_VARIABLE(n)     (XR_AST_ASSERT(n, AST_VARIABLE), &(n)->as.variable)
#define AST_AS_VAR_DECL(n)     (XR_AST_ASSERT_RANGE(n, AST_VAR_DECL, AST_CONST_DECL), &(n)->as.var_decl)
#define AST_AS_ASSIGNMENT(n)   (XR_AST_ASSERT(n, AST_ASSIGNMENT), &(n)->as.assignment)
#define AST_AS_BLOCK(n)        (XR_AST_ASSERT(n, AST_BLOCK), &(n)->as.block)
#define AST_AS_IF_STMT(n)      (XR_AST_ASSERT(n, AST_IF_STMT), &(n)->as.if_stmt)
#define AST_AS_WHILE_STMT(n)   (XR_AST_ASSERT(n, AST_WHILE_STMT), &(n)->as.while_stmt)
#define AST_AS_FOR_STMT(n)     (XR_AST_ASSERT(n, AST_FOR_STMT), &(n)->as.for_stmt)
#define AST_AS_FOR_IN(n)       (XR_AST_ASSERT(n, AST_FOR_IN_STMT), &(n)->as.for_in_stmt)
#define AST_AS_FUNC_DECL(n)    (XR_AST_ASSERT(n, AST_FUNCTION_DECL), &(n)->as.function_decl)
#define AST_AS_FUNC_EXPR(n)    (XR_AST_ASSERT(n, AST_FUNCTION_EXPR), &(n)->as.function_expr)
#define AST_AS_CALL(n)         (XR_AST_ASSERT(n, AST_CALL_EXPR), &(n)->as.call_expr)
#define AST_AS_RETURN(n)       (XR_AST_ASSERT(n, AST_RETURN_STMT), &(n)->as.return_stmt)
#define AST_AS_MEMBER(n)       (XR_AST_ASSERT(n, AST_MEMBER_ACCESS), &(n)->as.member_access)
#define AST_AS_INDEX_GET(n)    (XR_AST_ASSERT(n, AST_INDEX_GET), &(n)->as.index_get)
#define AST_AS_INDEX_SET(n)    (XR_AST_ASSERT(n, AST_INDEX_SET), &(n)->as.index_set)
#define AST_AS_ARRAY(n)        (XR_AST_ASSERT(n, AST_ARRAY_LITERAL), &(n)->as.array_literal)
#define AST_AS_MAP(n)          (XR_AST_ASSERT(n, AST_MAP_LITERAL), &(n)->as.map_literal)
#define AST_AS_OBJECT(n)       (XR_AST_ASSERT(n, AST_OBJECT_LITERAL), &(n)->as.object_literal)
#define AST_AS_NEW(n)          (XR_AST_ASSERT(n, AST_NEW_EXPR), &(n)->as.new_expr)
#define AST_AS_CLASS(n)        (XR_AST_ASSERT(n, AST_CLASS_DECL), &(n)->as.class_decl)
#define AST_AS_STRUCT(n)       (XR_AST_ASSERT(n, AST_STRUCT_DECL), &(n)->as.struct_decl)
#define AST_AS_ENUM(n)         (XR_AST_ASSERT(n, AST_ENUM_DECL), &(n)->as.enum_decl)
#define AST_AS_MATCH(n)        (XR_AST_ASSERT(n, AST_MATCH_EXPR), &(n)->as.match_expr)
#define AST_AS_TERNARY(n)      (XR_AST_ASSERT(n, AST_TERNARY), &(n)->as.ternary)
#define AST_AS_TRY_CATCH(n)    (XR_AST_ASSERT(n, AST_TRY_CATCH), &(n)->as.try_catch)
#define AST_AS_IMPORT(n)       (XR_AST_ASSERT(n, AST_IMPORT_STMT), &(n)->as.import_stmt)
#define AST_AS_EXPORT(n)       (XR_AST_ASSERT(n, AST_EXPORT_STMT), &(n)->as.export_stmt)
#define AST_AS_IS_EXPR(n)      (XR_AST_ASSERT(n, AST_IS_EXPR), &(n)->as.is_expr)
#define AST_AS_AS_EXPR(n)      (XR_AST_ASSERT(n, AST_AS_EXPR), &(n)->as.as_expr)
#define AST_AS_GO(n)           (XR_AST_ASSERT(n, AST_GO_EXPR), &(n)->as.go_expr)
#define AST_AS_AWAIT(n)        (XR_AST_ASSERT(n, AST_AWAIT_EXPR), &(n)->as.await_expr)
#define AST_AS_PROGRAM(n)      (XR_AST_ASSERT(n, AST_PROGRAM), &(n)->as.program)

// Binary node accessor that accepts any binary AST type
#define AST_AS_BINARY_ANY(n) (&(n)->as.binary)


#endif // XAST_ACCESSORS_H
