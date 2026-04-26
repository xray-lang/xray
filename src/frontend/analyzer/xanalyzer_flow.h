/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_flow.h - Control flow analysis for type narrowing
 *
 * KEY CONCEPT:
 *   FlowNodes form a graph representing control flow. Each statement
 *   has a FlowNode that tracks how types may be narrowed at that point.
 *
 * WHY THIS DESIGN:
 *   - Enables precise type narrowing (if x != null, then x is non-null)
 *   - Tracks assignments to update variable types
 *   - Supports union type discrimination
 */

#ifndef XANALYZER_FLOW_H
#define XANALYZER_FLOW_H

#include "../../runtime/value/xtype.h"
#include <stdint.h>
#include <stdbool.h>
#include "../../base/xdefs.h"

// Flow node flags
typedef enum XrFlowFlags {
    XA_FLOW_UNREACHABLE = 1 << 0,      // Dead code
    XA_FLOW_START = 1 << 1,            // Function entry
    XA_FLOW_BRANCH_LABEL = 1 << 2,     // Join point (multiple paths merge)
    XA_FLOW_LOOP_LABEL = 1 << 3,       // Loop header
    XA_FLOW_ASSIGNMENT = 1 << 4,       // Variable assignment
    XA_FLOW_TRUE_CONDITION = 1 << 5,   // True branch of condition
    XA_FLOW_FALSE_CONDITION = 1 << 6,  // False branch of condition
    XA_FLOW_SWITCH_CLAUSE = 1 << 7,    // Switch case clause
    XA_FLOW_CALL = 1 << 8,             // Function call (may affect types)
    XA_FLOW_RETURN = 1 << 9,           // Return statement
    XA_FLOW_THROW = 1 << 10,           // Throw statement
    XA_FLOW_MOVE = 1 << 11,            // Variable moved (shared let send)

    // Internal flags
    XA_FLOW_REFERENCED = 1 << 16,  // Has been referenced
    XA_FLOW_SHARED = 1 << 17,      // Multiple references (cache results)
} XrFlowFlags;

// Forward declaration
typedef struct XaFlowNode XaFlowNode;
typedef struct AstNode XrAstNode;

// Flow node structure
struct XaFlowNode {
    uint32_t flags;
    uint32_t id;  // Unique ID for caching

    XrAstNode *node;  // Associated AST node

    // Antecedents (predecessor nodes in flow graph)
    XaFlowNode **antecedents;
    int antecedent_count;
    int antecedent_capacity;

    // For condition nodes: the expression being tested
    XrAstNode *condition_expr;

    // For assignment nodes: the variable being assigned
    const char *assigned_name;
    XrType *assigned_type;

    // For switch clauses
    struct {
        XrAstNode *switch_stmt;
        int clause_start;
        int clause_end;
    } switch_data;
};

// Flow label (for branch/loop merging)
typedef struct XrFlowLabel {
    XaFlowNode base;
    // Branch labels collect multiple antecedents
} XrFlowLabel;

// Flow builder context
typedef struct XaFlowBuilder {
    XaFlowNode *current_flow;      // Current flow node
    XaFlowNode *unreachable_flow;  // Singleton for unreachable

    // For loops
    XrFlowLabel *current_break_target;
    XrFlowLabel *current_continue_target;

    // For functions
    XrFlowLabel *current_return_target;

    // For try-catch
    XrFlowLabel *current_exception_target;

    // ID counter
    uint32_t next_id;

    // Allocator for flow nodes
    XaFlowNode **all_nodes;
    int node_count;
    int node_capacity;
} XaFlowBuilder;

// API: Flow builder lifecycle
XR_FUNC XaFlowBuilder *xa_flow_builder_new(void);
XR_FUNC void xa_flow_builder_free(XaFlowBuilder *builder);

// API: Create flow nodes
XR_FUNC XaFlowNode *xa_flow_create_start(XaFlowBuilder *builder);
XR_FUNC XaFlowNode *xa_flow_create_branch_label(XaFlowBuilder *builder);
XR_FUNC XaFlowNode *xa_flow_create_loop_label(XaFlowBuilder *builder);
XR_FUNC XaFlowNode *xa_flow_create_assignment(XaFlowBuilder *builder, XrAstNode *node,
                                              const char *name, XrType *type);
XR_FUNC XaFlowNode *xa_flow_create_condition(XaFlowBuilder *builder, XrAstNode *expr,
                                             bool is_true_branch);
XR_FUNC XaFlowNode *xa_flow_create_call(XaFlowBuilder *builder, XrAstNode *call);

// API: Connect flow nodes
XR_FUNC void xa_flow_add_antecedent(XaFlowNode *label, XaFlowNode *antecedent);
XR_FUNC XaFlowNode *xa_flow_finish_label(XaFlowBuilder *builder, XaFlowNode *label);

// API: Flow builder operations
XR_FUNC void xa_flow_set_current(XaFlowBuilder *builder, XaFlowNode *flow);
XR_FUNC XaFlowNode *xa_flow_get_current(XaFlowBuilder *builder);
XR_FUNC bool xa_flow_is_unreachable(XaFlowBuilder *builder);

// Type narrowing cache (open-addressing hash table, keyed by node->id)
typedef struct XaFlowCache {
    uint32_t *ids;   // node IDs (0 = empty slot)
    XrType **types;  // corresponding types
    int capacity;    // always power of 2
    int count;
} XaFlowCache;

XR_FUNC XaFlowCache *xa_flow_cache_new(void);
XR_FUNC void xa_flow_cache_free(XaFlowCache *cache);
XR_FUNC void xa_flow_cache_clear(XaFlowCache *cache);
XR_FUNC XrType *xa_flow_cache_get(XaFlowCache *cache, XaFlowNode *node);
XR_FUNC void xa_flow_cache_set(XaFlowCache *cache, XaFlowNode *node, XrType *type);

// API: Type narrowing
XR_FUNC XrType *xa_flow_get_type_of_reference(XaFlowBuilder *builder, const char *name,
                                              XrType *declared_type, XaFlowNode *flow_node,
                                              XaFlowCache *cache);

// API: Move tracking for shared let variables (use-after-move detection)
XR_FUNC XaFlowNode *xa_flow_create_move(XaFlowBuilder *builder, const char *name);
XR_FUNC bool xa_flow_is_moved(XaFlowBuilder *builder, const char *name, XaFlowNode *at_node);

// Narrowing helpers
XR_FUNC XrType *xa_narrow_by_typeof(XrType *type, const char *type_name, bool assume_true);
XR_FUNC XrType *xa_narrow_by_null_check(XrType *type, bool is_equal_null, bool assume_true);
XR_FUNC XrType *xa_narrow_by_truthiness(XrType *type, bool assume_true);
XR_FUNC XrType *xa_narrow_by_instanceof(XrType *type, const char *class_name, bool assume_true);

#endif  // XANALYZER_FLOW_H
