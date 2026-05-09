/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xast_nodes_expr.h - Expression AST nodes
 *
 * KEY CONCEPT (P-03):
 *   Topic header for expression-shaped AST node payloads. Pull in only
 *   via xast_nodes.h.
 */

#ifndef XAST_NODES_EXPR_H
#define XAST_NODES_EXPR_H

#include "xast_nodes_common.h"

/* ========== Variable / Assignment ========== */

// Variable reference node
typedef struct VariableNode {
    char *name;
    uint32_t symbol_id; /* unique ID from analyzer scope resolution; 0 = unresolved */
} VariableNode;

// Assignment node
typedef struct AssignmentNode {
    char *name;
    AstNode *value;
    uint32_t symbol_id; /* resolved target variable ID; 0 = unresolved */
} AssignmentNode;

// Compound assignment node
typedef struct CompoundAssignmentNode {
    char *name;
    XrTokenType op;
    AstNode *value;
    AstNode *object;
    uint32_t symbol_id; /* resolved target variable ID; 0 = unresolved */
} CompoundAssignmentNode;

// Increment/decrement node
typedef struct IncDecNode {
    char *name;
    uint32_t symbol_id; /* resolved target variable ID; 0 = unresolved */
} IncDecNode;

// Destructure assignment node
typedef struct DestructureAssignNode {
    XrDestructurePattern *pattern;
    AstNode *value;
} DestructureAssignNode;

// Multi-value declaration node
typedef struct MultiVarDeclNode {
    char **names;
    int name_count;
    AstNode **values;
    int value_count;
    bool is_const;
} MultiVarDeclNode;

// Multi-value assignment node
typedef struct MultiAssignNode {
    AstNode **targets;
    int target_count;
    AstNode **values;
    int value_count;
} MultiAssignNode;

/* ========== Calls & Type-Discriminating Exprs ========== */

// Function call node — supports generic call syntax: foo<int, string>(arg1, arg2)
typedef struct CallExprNode {
    AstNode *callee;
    AstNode **arguments;
    int arg_count;
    XrTypeRef **type_args;
    int type_arg_count;
} CallExprNode;

// is expression node (runtime type check)
typedef struct IsExprNode {
    AstNode *expr;
    XrTypeRef *type;
} IsExprNode;

typedef struct AsExprNode {
    AstNode *expr;
    XrTypeRef *type;
    bool is_safe;  // true = safe cast (returns null on failure)
} AsExprNode;

/* ========== Aggregate / Indexed Exprs ========== */

// Array literal node
typedef struct ArrayLiteralNode {
    AstNode **elements;
    int count;
} ArrayLiteralNode;

// Index access / set / slice
typedef struct IndexGetNode {
    AstNode *array;
    AstNode *index;
} IndexGetNode;

typedef struct IndexSetNode {
    AstNode *array;
    AstNode *index;
    AstNode *value;
} IndexSetNode;

typedef struct SliceExprNode {
    AstNode *source;
    AstNode *start;
    AstNode *end;
} SliceExprNode;

// Member access
typedef struct MemberAccessNode {
    AstNode *object;
    char *name;
} MemberAccessNode;

typedef struct MemberSetNode {
    AstNode *object;
    char *member;
    AstNode *value;
} MemberSetNode;

// Template string node
typedef struct TemplateStringNode {
    AstNode **parts;
    int part_count;
} TemplateStringNode;

// Object / Map / Set literals
typedef struct ObjectLiteralNode {
    AstNode **keys;
    AstNode **values;
    bool *computed;
    int count;
} ObjectLiteralNode;

typedef struct MapLiteralNode {
    AstNode **keys;
    AstNode **values;
    int count;
} MapLiteralNode;

typedef struct SetLiteralNode {
    AstNode **elements;
    int count;
} SetLiteralNode;

// Struct literal node: Point{x: 1.0, y: 2.0} or Pair<int, string>{first: 1, second: "a"}
typedef struct StructLiteralNode {
    char *struct_name;
    char **field_names;
    AstNode **field_values;
    int field_count;
    XrTypeRef **type_args;  // Generic type arguments (for monomorphization)
    int type_arg_count;
} StructLiteralNode;

/* ========== Other Operators ========== */

// Ternary expression node
typedef struct TernaryNode {
    AstNode *condition;
    AstNode *true_expr;
    AstNode *false_expr;
} TernaryNode;

// Optional chain node
typedef struct OptionalChainNode {
    AstNode *object;
    char *name;
    AstNode *index;
    int chain_type;
} OptionalChainNode;

// Range expression node
typedef struct RangeNode {
    AstNode *start;
    AstNode *end;
} RangeNode;

#endif  // XAST_NODES_EXPR_H
