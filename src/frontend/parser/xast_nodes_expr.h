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
#include "../../shared/xr_param_mode.h"

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

/* ========== Calls & Type-Discriminating Exprs ========== */

// Function call node — supports generic call syntax: foo<int, string>(arg1, arg2)
typedef struct CallExprNode {
    AstNode *callee;
    AstNode **arguments;
    XrCallArgAccess *arg_accesses;
    int arg_count;
    int supplied_arg_count;
    int default_arg_count;
    int default_arg_param_count;
    int required_arg_count;
    XrTypeRef **type_args;
    int type_arg_count;
    uint32_t semantic_type_id;
    XrTypeRef **semantic_type_args;
    int semantic_type_arg_count;
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

typedef struct ComptimeExprNode {
    AstNode *expr;
} ComptimeExprNode;

/* ========== Aggregate / Indexed Exprs ========== */

// Array literal node
typedef struct ArrayLiteralNode {
    AstNode **elements;
    int count;
    AstNode *repeat_value;
    AstNode *repeat_count;
    bool is_repeat;
} ArrayLiteralNode;

// Tuple literal node — `()`, `(x,)`, `(a, b, ...)`.
// Distinct from ArrayLiteral: tuples are heterogeneous, fixed-arity,
// and indexed via the dedicated `.N` field access (not `[i]`).
//
// Elements may include AST_SPREAD_EXPR nodes — `(...t, x)` splices the
// tuple `t` into the literal at compile time. The arity of every spread
// source must be statically known; the analyzer expands the per-element
// types and the lowerer emits one TUPLE_GET per spliced slot.
typedef struct TupleLiteralNode {
    AstNode **elements;
    int count;
} TupleLiteralNode;

// Spread element: `...expr`. Only valid as a child of a tuple literal
// or as an argument inside a call expression. The wrapped expression
// must evaluate to a tuple of known arity; that arity is spliced into
// the surrounding literal / argument list at static-analysis time.
typedef struct SpreadExprNode {
    AstNode *expr;
} SpreadExprNode;

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
    XrLiteralEscapeMode escape_mode;
    XrLiteralSourceForm source_form;
} TemplateStringNode;

// Object / Map / Set literals
typedef struct ObjectLiteralNode {
    AstNode **keys;
    AstNode **values;
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
    /* True for a link the programmer wrote with a plain `.` / `[` / `(` that
     * continues an optional chain (`a?.b.c` — spec §3.6). Such a link only
     * short-circuits on a null prefix: its own member must be non-null, which
     * the analyzer enforces. */
    bool implicit_link;
    /* Analysis result: this link's own member/element type is nullable, i.e.
     * the link can produce null for a reason other than the chain's
     * short-circuit. The next implicit link uses it to demand a `?.`. */
    bool value_nullable;
} OptionalChainNode;

// Range expression node
typedef struct RangeNode {
    AstNode *start;
    AstNode *end;
    bool inclusive_end;
} RangeNode;

#endif  // XAST_NODES_EXPR_H
