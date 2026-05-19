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
//   0 = normal variable (deep copy across coroutines)
//   1 = shared variable (stored in global heap, passed by reference)
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
    AstNode *condition;
    AstNode *body;
} WhileStmtNode;

typedef struct ForStmtNode {
    AstNode *initializer;
    AstNode *condition;
    AstNode *increment;
    AstNode *body;
} ForStmtNode;

typedef struct ForInStmtNode {
    char *item_name;
    char *value_name;
    bool is_keyvalue;
    XrTypeRef *item_type;
    AstNode *collection;
    AstNode *body;
    uint32_t item_symbol_id;  /* symbol ID for iteration key/item variable */
    uint32_t value_symbol_id; /* symbol ID for iteration value variable (key-value loops) */
} ForInStmtNode;

typedef struct BreakStmtNode {
    int placeholder;
} BreakStmtNode;
typedef struct ContinueStmtNode {
    int placeholder;
} ContinueStmtNode;

/* ========== Exception Handling ========== */

typedef struct TryCatchNode {
    AstNode *try_body;
    char *catch_var;
    int catch_var_line;    // Line of catch variable (1-indexed)
    int catch_var_column;  // Column of catch variable (1-indexed)
    AstNode *catch_body;
    AstNode *finally_body;
    uint32_t catch_symbol_id; /* symbol ID for catch variable; 0 = unresolved */
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
typedef struct PatternAdtNode {
    AstNode *variant;    // e.g. AST_MEMBER_ACCESS(Shape, Circle)
    AstNode **patterns;  // payload sub-patterns
    int count;           // number of payload slots
} PatternAdtNode;

/* ========== Coroutine / Concurrency ==========
 *
 * Supports:
 *   go fn()                       - start coroutine
 *   go(name: "xxx") fn()          - coroutine with name
 *   go(priority: Coro.HIGH) fn()  - coroutine with priority
 *   linked go fn()                - bidirectional error propagation
 *   monitored go fn()             - one-way completion notification
 */
typedef struct GoExprNode {
    AstNode *expr;      // Expression to execute (function call or closure)
    const char *name;   // Coroutine name (optional, for debugging)
    AstNode *priority;  // Priority expression (optional)
    uint8_t link_mode;  // XR_LINK_NONE / XR_LINK_LINKED / XR_LINK_MONITORED
} GoExprNode;

typedef struct AwaitExprNode {
    AstNode *expr;
    AstNode *timeout;
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

// cancelled() expression
typedef struct CancelledExprNode {
    int placeholder;
} CancelledExprNode;

// catch! { body } expression — evaluates body, returns Result.Ok(value)
// on success and Result.Err(exception) on throw.
typedef struct CatchExprNode {
    AstNode *body;  // block to execute
} CatchExprNode;

#endif  // XAST_NODES_STMT_H
