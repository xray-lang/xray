/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xast_nodes_stmt.h - Statement / control-flow / coroutine AST nodes
 *
 * KEY CONCEPT (P-03):
 *   Topic header for statement-shaped AST node payloads. Pull in only
 *   via xast_nodes.h.
 */

#ifndef XAST_NODES_STMT_H
#define XAST_NODES_STMT_H

#include "xast_nodes_common.h"

// xtask.h pulls in coroutine-related enums (XR_LINK_*, XR_SCOPE_*) used
// directly by GoExprNode / ScopeBlockNode below.
#include "../../coro/xtask.h"

/* ========== Variable Declarations (statement form) ========== */

// Variable declaration node
//
// Storage mode (storage_mode):
//   0 = normal variable (must use explicit copy/move at coroutine boundaries)
//   1 = shared variable (stored in global heap, passed by reference when const)
//
// shared variable features:
//   - shared const: can be directly read concurrently by coroutine closures
//   - shared let:   can only be accessed serially through Channel
typedef struct VarDeclNode {
    char *name;
    AstNode *initializer;
    bool is_const;
    uint8_t storage_mode;  // 0 = normal, 1 = shared
    XrTypeRef *type_annotation;
    uint32_t symbol_id; /* unique ID from analyzer; 0 = unresolved */
} VarDeclNode;

// Storage mode constants
#define XR_STORAGE_NORMAL 0
#define XR_STORAGE_SHARED 1

// Destructure declaration node
typedef struct DestructureDeclNode {
    XrDestructurePattern *pattern;
    AstNode *initializer;
    bool is_const;
} DestructureDeclNode;

/* ========== Control Flow ========== */

typedef struct IfStmtNode {
    AstNode *condition;
    AstNode *then_branch;
    AstNode *else_branch;
} IfStmtNode;

typedef struct WhileStmtNode {
    char *label;
    AstNode *condition;
    AstNode *body;
} WhileStmtNode;

typedef struct ForStmtNode {
    char *label;
    AstNode *initializer;
    AstNode *condition;
    AstNode *increment;
    AstNode *body;
} ForStmtNode;

typedef struct ForInStmtNode {
    char *label;
    char *item_name;
    char *value_name;
    bool is_keyvalue;
    XrTypeRef *item_type;
    AstNode *collection;
    AstNode *body;
    uint32_t item_symbol_id;  /* symbol ID for iteration key/item variable */
    uint32_t value_symbol_id; /* symbol ID for iteration value variable (key-value loops) */
} ForInStmtNode;

typedef struct ParallelForStmtNode {
    char *label;
    char *item_name;
    char *end_name;
    char *worker_name;
    XrParallelLocalBinding *locals;
    int local_count;
    AstNode *range;
    AstNode *worker_count; /* optional `workers expr`; NULL lets lowering choose */
    AstNode *final_body;
    AstNode *body;
    uint32_t item_symbol_id;
    uint32_t end_symbol_id;
    uint32_t worker_symbol_id;
    bool range_body;
} ParallelForStmtNode;

typedef struct BreakStmtNode {
    char *label;
} BreakStmtNode;
typedef struct ContinueStmtNode {
    char *label;
} ContinueStmtNode;

/* ========== Exception Handling ========== */

// A single catch clause: catch (e), catch (e: NetErr), or catch panic (p)
typedef struct XrCatchClause {
    char *var_name;
    int var_line;     // Line of catch variable (1-indexed)
    int var_column;   // Column of catch variable (1-indexed)
    XrTypeRef *type;  // Type filter annotation (NULL = catch-all)
    AstNode *body;
    uint32_t symbol_id;  // Analyzer-assigned unique ID; 0 = unresolved
    bool is_panic;       // true = catch panic (p) clause for panic channel handling
} XrCatchClause;

typedef struct TryCatchNode {
    AstNode *try_body;
    XrCatchClause **catch_clauses;  // Array of catch clauses (top-down order)
    int catch_count;                // Number of catch clauses (>= 1)
} TryCatchNode;

typedef struct ThrowStmtNode {
    AstNode *expression;
} ThrowStmtNode;

/* ========== Return ========== */

typedef struct ReturnStmtNode {
    AstNode **values;
    int value_count;
} ReturnStmtNode;

/* ========== Match ========== */

typedef struct MatchExprNode {
    AstNode *expr;
    AstNode **arms;
    int arm_count;
} MatchExprNode;

typedef struct MatchArmNode {
    AstNode *pattern;
    AstNode *guard;
    AstNode *body;
} MatchArmNode;

typedef struct PatternLiteralNode {
    AstNode *value;
} PatternLiteralNode;

typedef struct PatternRangeNode {
    AstNode *start;
    AstNode *end;
    bool inclusive_end;
} PatternRangeNode;

typedef struct PatternWildcardNode {
    int placeholder;
} PatternWildcardNode;

typedef struct PatternMultiNode {
    AstNode **patterns;
    int count;
} PatternMultiNode;

/* Positional tuple pattern: `(p0, p1, ...)`.
 * Each sub-pattern is itself a regular AST_PATTERN_* node, so tuple
 * patterns nest naturally and may contain wildcards, bindings (a bare
 * AST_VARIABLE wrapped in AST_PATTERN_LITERAL) or further tuples. The
 * arity is fixed at parse time and validated against the scrutinee's
 * static tuple type by the analyzer. */
typedef struct PatternTupleNode {
    AstNode **patterns;
    int count;
} PatternTupleNode;

/* ADT variant destructure: `Shape.Circle(r)` / `Result.Ok(v)`.
 * variant is the AST_MEMBER_ACCESS / AST_ENUM_ACCESS node for the
 * variant name; sub-patterns are AST_PATTERN_* nodes for each payload
 * slot (bindings, wildcards, or literals). */
// Object/record match pattern: `{ x, y }` or `{ x: sub }`. field_names[i] is the
// source field; patterns[i] is the sub-pattern bound to that field value (a
// bare-name binding for shorthand `{ x }`, or a nested/renamed sub-pattern for
// `{ x: sub }`). Matches any object/Json carrying those fields.
typedef struct PatternObjectNode {
    char **field_names;
    AstNode **patterns;
    int count;
} PatternObjectNode;

// Array match pattern: `[a, b, ..rest]`. patterns[i] are positional element
// sub-patterns. has_rest enables a trailing rest binding capturing the tail as a
// new array; rest_name is the binding (NULL for a bare `..` that drops the tail).
typedef struct PatternArrayNode {
    AstNode **patterns;
    int count;
    bool has_rest;
    char *rest_name;
    uint32_t rest_symbol_id;  // analyzer-assigned id for the rest binding (0 if none)
} PatternArrayNode;

typedef struct PatternAdtNode {
    AstNode *variant;    // e.g. AST_MEMBER_ACCESS(Shape, Circle)
    AstNode **patterns;  // payload sub-patterns
    int count;           // number of payload slots
} PatternAdtNode;

/* Type pattern: `is T` or `is T name`.
 * type is the static type to test against; binding_name is the optional
 * narrowed binding (NULL when absent). symbol_id is assigned by the
 * analyzer when binding_name is non-NULL. */
typedef struct PatternTypeNode {
    XrTypeRef *type;
    const char *binding_name;
    uint32_t symbol_id;
} PatternTypeNode;

/* ========== Coroutine / Concurrency ==========
 *
 * Supports:
 *   go fn()                       - start coroutine
 *   go(name: "xxx") fn()          - coroutine with name
 *   linked go fn()                - bidirectional error propagation
 *   monitored go fn()             - one-way completion notification
 */
typedef enum SpawnExprKind {
    XR_SPAWN_COROUTINE = 0,
    XR_SPAWN_THREAD = 1,
} SpawnExprKind;

typedef struct GoExprNode {
    AstNode *expr;       // Expression to execute (function call or closure)
    const char *name;    // Coroutine name (optional, for debugging)
    uint8_t link_mode;   // XR_LINK_NONE / XR_LINK_LINKED / XR_LINK_MONITORED
    uint8_t spawn_kind;  // SpawnExprKind; sys.Thread.spawn reuses go capture rules
} GoExprNode;

typedef struct AwaitExprNode {
    AstNode *expr;
    AstNode *timeout;
    AstNode *into;
    bool is_any;
    bool is_all;
    bool is_any_success;
} AwaitExprNode;

typedef struct ChannelNewNode {
    AstNode *buffer_size;
} ChannelNewNode;

typedef struct SelectCaseNode {
    char *var_name;
    AstNode *channel;
    AstNode *value;
    AstNode *body;
    bool is_send;
    bool is_default;
    bool is_timeout;
    uint32_t var_symbol_id; /* symbol ID for recv variable; 0 = unresolved */
} SelectCaseNode;

typedef struct SelectStmtNode {
    AstNode **cases;
    int case_count;
} SelectStmtNode;

// yield statement:
//   `yield expr`     — generator value production (the enclosing function is a
//                      generator returning Iterator<T>).
//   value == NULL is invalid at parse time (bare `yield` is rejected; use
//   `Coro.yield()` for cooperative scheduling).
typedef struct YieldStmtNode {
    AstNode *value;
} YieldStmtNode;

typedef struct DeferStmtNode {
    AstNode *expr;
} DeferStmtNode;

typedef struct ScopeBlockNode {
    AstNode *body;
    uint8_t scope_mode;  // XR_SCOPE_WAIT / XR_SCOPE_LINKED / XR_SCOPE_SUPERVISOR
} ScopeBlockNode;

// move expression: move var (explicit ownership transfer)
typedef struct MoveExprNode {
    AstNode *expr;  // must be a variable reference
} MoveExprNode;

// unsafe expression: unsafe { expr }
// Semantically transparent (value/type = operand); the wrapper exists so
// the analyzer can permit otherwise-restricted operations (extern calls,
// raw-pointer dereference) only inside its dynamic extent.
typedef struct UnsafeExprNode {
    AstNode *operand;
} UnsafeExprNode;

// cancelled() expression
typedef struct CancelledExprNode {
    int placeholder;
} CancelledExprNode;

#endif  // XAST_NODES_STMT_H
