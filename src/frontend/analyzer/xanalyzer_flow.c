/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_flow.c - Control flow analysis implementation
 */

#include "xanalyzer_flow.h"
#include "../../base/xchecks.h"
#include "../parser/xast.h"
#include "../../runtime/value/xtype_names.h"
#include "../../base/xmalloc.h"
#include <string.h>

// Helper: extract string value from literal node
static const char *get_string_literal(AstNode *node) {
    if (!node || node->type != AST_LITERAL_STRING) return NULL;
    return node->as.literal.raw_value.string_val;
}

// Apply type narrowing based on condition expression
// Analyzes common patterns: x != null, x == null, typeof x == "type", truthiness
static XrType *apply_condition_narrowing(XrAstNode *expr, const char *var_name,
                                          XrType *base_type, bool assume_true) {
    if (!expr || !var_name || !base_type) return base_type;
    
    AstNode *node = (AstNode *)expr;
    AstNodeType type = node->type;
    
    // Pattern: x (truthiness check - variable used directly as condition)
    if (type == AST_VARIABLE) {
        if (node->as.variable.name && strcmp(node->as.variable.name, var_name) == 0) {
            return xa_narrow_by_truthiness(base_type, assume_true);
        }
        return base_type;
    }
    
    // Pattern: !x (negated truthiness)
    if (type == AST_UNARY_NOT) {
        AstNode *operand = node->as.unary.operand;
        if (operand && operand->type == AST_VARIABLE &&
            operand->as.variable.name && strcmp(operand->as.variable.name, var_name) == 0) {
            return xa_narrow_by_truthiness(base_type, !assume_true);
        }
        return base_type;
    }
    
    // Pattern: x == null, x != null, x === null, x !== null
    // Pattern: typeof x == "type", typeof x === "type"
    if (type == AST_BINARY_EQ || type == AST_BINARY_NE ||
        type == AST_BINARY_EQ_STRICT || type == AST_BINARY_NE_STRICT) {
        AstNode *left = node->as.binary.left;
        AstNode *right = node->as.binary.right;
        
        bool is_equal = (type == AST_BINARY_EQ || type == AST_BINARY_EQ_STRICT);
        
        // Check if comparing variable to null
        bool var_on_left = (left && left->type == AST_VARIABLE &&
                           left->as.variable.name && strcmp(left->as.variable.name, var_name) == 0);
        bool var_on_right = (right && right->type == AST_VARIABLE &&
                            right->as.variable.name && strcmp(right->as.variable.name, var_name) == 0);
        bool null_on_left = (left && left->type == AST_LITERAL_NULL);
        bool null_on_right = (right && right->type == AST_LITERAL_NULL);
        
        if ((var_on_left && null_on_right) || (var_on_right && null_on_left)) {
            return xa_narrow_by_null_check(base_type, is_equal, assume_true);
        }
        
        // Check for typeof pattern: typeof(x) == "string"
        // In xray, typeof is a builtin function call: AST_CALL_EXPR
        AstNode *typeof_operand = NULL;
        const char *type_name = NULL;
        
        // Check left side for typeof(x) call
        if (left && left->type == AST_CALL_EXPR &&
            left->as.call_expr.callee && left->as.call_expr.callee->type == AST_VARIABLE &&
            left->as.call_expr.callee->as.variable.name &&
            strcmp(left->as.call_expr.callee->as.variable.name, "typeof") == 0 &&
            left->as.call_expr.arg_count == 1) {
            typeof_operand = left->as.call_expr.arguments[0];
            type_name = get_string_literal(right);
        }
        // Check right side for typeof(x) call
        else if (right && right->type == AST_CALL_EXPR &&
                 right->as.call_expr.callee && right->as.call_expr.callee->type == AST_VARIABLE &&
                 right->as.call_expr.callee->as.variable.name &&
                 strcmp(right->as.call_expr.callee->as.variable.name, "typeof") == 0 &&
                 right->as.call_expr.arg_count == 1) {
            typeof_operand = right->as.call_expr.arguments[0];
            type_name = get_string_literal(left);
        }
        
        if (typeof_operand && type_name &&
            typeof_operand->type == AST_VARIABLE &&
            typeof_operand->as.variable.name &&
            strcmp(typeof_operand->as.variable.name, var_name) == 0) {
            // typeof(x) == "type" with assume_true => narrow to type
            // typeof(x) != "type" with assume_true => exclude type
            bool effective_true = (is_equal == assume_true);
            return xa_narrow_by_typeof(base_type, type_name, effective_true);
        }
    }
    
    // Pattern: a && b (logical AND)
    // true branch: both conditions hold, apply narrowing from both
    // false branch: at least one is false (cannot narrow safely)
    if (type == AST_BINARY_AND) {
        if (assume_true) {
            XrType *narrowed = apply_condition_narrowing(
                (XrAstNode *)node->as.binary.left, var_name, base_type, true);
            return apply_condition_narrowing(
                (XrAstNode *)node->as.binary.right, var_name, narrowed, true);
        }
        return base_type;
    }
    
    // Pattern: a || b (logical OR)
    // false branch: both conditions are false, apply narrowing from both
    // true branch: at least one is true (cannot narrow safely)
    if (type == AST_BINARY_OR) {
        if (!assume_true) {
            XrType *narrowed = apply_condition_narrowing(
                (XrAstNode *)node->as.binary.left, var_name, base_type, false);
            return apply_condition_narrowing(
                (XrAstNode *)node->as.binary.right, var_name, narrowed, false);
        }
        return base_type;
    }
    
    // Pattern: x is ClassName (instanceof check)
    if (type == AST_IS_EXPR) {
        IsExprNode *is_expr = &node->as.is_expr;
        if (is_expr->expr && is_expr->expr->type == AST_VARIABLE &&
            is_expr->expr->as.variable.name &&
            strcmp(is_expr->expr->as.variable.name, var_name) == 0 &&
            is_expr->type) {
            // Extract class name from the type
            const char *class_name = NULL;
            if (XR_TYPE_IS_INSTANCE(is_expr->type) && is_expr->type->instance.class_name) {
                class_name = is_expr->type->instance.class_name;
            } else if (is_expr->type->instance.class_name) {
                class_name = is_expr->type->instance.class_name;
            }
            if (class_name) {
                return xa_narrow_by_instanceof(base_type, class_name, assume_true);
            }
            // For primitive type checks (x is int, x is string, etc.)
            // narrow directly to that type
            if (assume_true) {
                return is_expr->type;
            }
        }
    }
    
    return base_type;
}

// Helper: allocate a flow node
static XaFlowNode *flow_node_alloc(XaFlowBuilder *builder, uint32_t flags) {
    XR_DCHECK(builder != NULL, "flow_node_alloc: NULL builder");
    XaFlowNode *node = xr_calloc(1, sizeof(XaFlowNode));
    if (!node) return NULL;
    
    node->flags = flags;
    node->id = builder->next_id++;
    
    // Track all nodes for cleanup
    if (builder->node_count >= builder->node_capacity) {
        int new_cap = builder->node_capacity == 0 ? 64 : builder->node_capacity * 2;
        builder->all_nodes = xr_realloc(builder->all_nodes, sizeof(XaFlowNode*) * new_cap);
        builder->node_capacity = new_cap;
    }
    builder->all_nodes[builder->node_count++] = node;
    
    return node;
}

// Create flow builder
XaFlowBuilder *xa_flow_builder_new(void) {
    XaFlowBuilder *builder = xr_calloc(1, sizeof(XaFlowBuilder));
    if (!builder) return NULL;
    
    builder->next_id = 1;
    builder->unreachable_flow = flow_node_alloc(builder, XA_FLOW_UNREACHABLE);
    builder->current_flow = builder->unreachable_flow;
    
    return builder;
}

// Free flow builder
void xa_flow_builder_free(XaFlowBuilder *builder) {
    if (!builder) return;
    
    // Free all nodes
    for (int i = 0; i < builder->node_count; i++) {
        XaFlowNode *node = builder->all_nodes[i];
        if (node->antecedents) xr_free(node->antecedents);
        xr_free(node);
    }
    if (builder->all_nodes) xr_free(builder->all_nodes);
    
    xr_free(builder);
}

// Create start node
XaFlowNode *xa_flow_create_start(XaFlowBuilder *builder) {
    XaFlowNode *node = flow_node_alloc(builder, XA_FLOW_START);
    builder->current_flow = node;
    return node;
}

// Create branch label (for merging control flow)
XaFlowNode *xa_flow_create_branch_label(XaFlowBuilder *builder) {
    return flow_node_alloc(builder, XA_FLOW_BRANCH_LABEL);
}

// Create loop label
XaFlowNode *xa_flow_create_loop_label(XaFlowBuilder *builder) {
    return flow_node_alloc(builder, XA_FLOW_LOOP_LABEL);
}

// Create assignment node
XaFlowNode *xa_flow_create_assignment(XaFlowBuilder *builder, XrAstNode *node,
                                       const char *name, XrType *type) {
    if (builder->current_flow->flags & XA_FLOW_UNREACHABLE) {
        return builder->current_flow;
    }
    
    XaFlowNode *flow = flow_node_alloc(builder, XA_FLOW_ASSIGNMENT);
    flow->node = node;
    flow->assigned_name = name;
    flow->assigned_type = type;
    
    // Connect to current flow
    xa_flow_add_antecedent(flow, builder->current_flow);
    builder->current_flow = flow;
    
    return flow;
}

// Create condition node
XaFlowNode *xa_flow_create_condition(XaFlowBuilder *builder, XrAstNode *expr,
                                      bool is_true_branch) {
    if (builder->current_flow->flags & XA_FLOW_UNREACHABLE) {
        return builder->current_flow;
    }
    
    uint32_t flags = is_true_branch ? XA_FLOW_TRUE_CONDITION : XA_FLOW_FALSE_CONDITION;
    XaFlowNode *flow = flow_node_alloc(builder, flags);
    flow->condition_expr = expr;
    
    // Connect to current flow
    xa_flow_add_antecedent(flow, builder->current_flow);
    
    return flow;
}

// Create call node
XaFlowNode *xa_flow_create_call(XaFlowBuilder *builder, XrAstNode *call) {
    if (builder->current_flow->flags & XA_FLOW_UNREACHABLE) {
        return builder->current_flow;
    }
    
    XaFlowNode *flow = flow_node_alloc(builder, XA_FLOW_CALL);
    flow->node = call;
    
    xa_flow_add_antecedent(flow, builder->current_flow);
    builder->current_flow = flow;
    
    return flow;
}

// Add antecedent to a node
void xa_flow_add_antecedent(XaFlowNode *node, XaFlowNode *antecedent) {
    if (!node || !antecedent) return;
    if (antecedent->flags & XA_FLOW_UNREACHABLE) return;
    
    // Check if already present
    for (int i = 0; i < node->antecedent_count; i++) {
        if (node->antecedents[i] == antecedent) return;
    }
    
    // Add to array
    if (node->antecedent_count >= node->antecedent_capacity) {
        int new_cap = node->antecedent_capacity == 0 ? 4 : node->antecedent_capacity * 2;
        node->antecedents = xr_realloc(node->antecedents, sizeof(XaFlowNode*) * new_cap);
        node->antecedent_capacity = new_cap;
    }
    node->antecedents[node->antecedent_count++] = antecedent;
    
    // First reference: mark REFERENCED. Second reference: mark SHARED (enables caching).
    if (antecedent->flags & XA_FLOW_REFERENCED) {
        antecedent->flags |= XA_FLOW_SHARED;
    } else {
        antecedent->flags |= XA_FLOW_REFERENCED;
    }
}

// Finish a label node
XaFlowNode *xa_flow_finish_label(XaFlowBuilder *builder, XaFlowNode *label) {
    if (!label) return builder->unreachable_flow;
    
    // If no antecedents, unreachable
    if (label->antecedent_count == 0) {
        return builder->unreachable_flow;
    }
    
    // If single antecedent, can skip the label
    if (label->antecedent_count == 1) {
        return label->antecedents[0];
    }
    
    return label;
}

// Set current flow
void xa_flow_set_current(XaFlowBuilder *builder, XaFlowNode *flow) {
    if (builder) builder->current_flow = flow ? flow : builder->unreachable_flow;
}

// Get current flow
XaFlowNode *xa_flow_get_current(XaFlowBuilder *builder) {
    return builder ? builder->current_flow : NULL;
}

// Check if current flow is unreachable
bool xa_flow_is_unreachable(XaFlowBuilder *builder) {
    return builder && (builder->current_flow->flags & XA_FLOW_UNREACHABLE);
}

// Create flow cache (open-addressing hash, power-of-2 capacity)
XaFlowCache *xa_flow_cache_new(void) {
    XaFlowCache *cache = xr_calloc(1, sizeof(XaFlowCache));
    if (!cache) return NULL;
    cache->capacity = 64;
    cache->ids = xr_calloc(cache->capacity, sizeof(uint32_t));
    cache->types = xr_calloc(cache->capacity, sizeof(XrType*));
    return cache;
}

// Free flow cache
void xa_flow_cache_free(XaFlowCache *cache) {
    if (!cache) return;
    if (cache->ids) xr_free(cache->ids);
    if (cache->types) xr_free(cache->types);
    xr_free(cache);
}

// Clear flow cache
void xa_flow_cache_clear(XaFlowCache *cache) {
    if (!cache) return;
    memset(cache->ids, 0, sizeof(uint32_t) * cache->capacity);
    cache->count = 0;
}

// Get from cache (open-addressing probe with node->id as key)
XrType *xa_flow_cache_get(XaFlowCache *cache, XaFlowNode *node) {
    if (!cache || !node || node->id == 0) return NULL;
    uint32_t mask = (uint32_t)(cache->capacity - 1);
    uint32_t idx = node->id & mask;
    for (int probe = 0; probe < cache->capacity; probe++) {
        uint32_t slot = (idx + probe) & mask;
        if (cache->ids[slot] == 0) return NULL;
        if (cache->ids[slot] == node->id) return cache->types[slot];
    }
    return NULL;
}

// Rehash when load factor > 0.7
static void flow_cache_rehash(XaFlowCache *cache) {
    int old_cap = cache->capacity;
    uint32_t *old_ids = cache->ids;
    XrType **old_types = cache->types;
    
    cache->capacity = old_cap * 2;
    cache->ids = xr_calloc(cache->capacity, sizeof(uint32_t));
    cache->types = xr_calloc(cache->capacity, sizeof(XrType*));
    cache->count = 0;
    
    uint32_t mask = (uint32_t)(cache->capacity - 1);
    for (int i = 0; i < old_cap; i++) {
        if (old_ids[i] == 0) continue;
        uint32_t idx = old_ids[i] & mask;
        while (cache->ids[idx] != 0) idx = (idx + 1) & mask;
        cache->ids[idx] = old_ids[i];
        cache->types[idx] = old_types[i];
        cache->count++;
    }
    
    xr_free(old_ids);
    xr_free(old_types);
}

// Set in cache
void xa_flow_cache_set(XaFlowCache *cache, XaFlowNode *node, XrType *type) {
    if (!cache || !node || node->id == 0) return;
    
    // Rehash at 70% load
    if (cache->count * 10 >= cache->capacity * 7) {
        flow_cache_rehash(cache);
    }
    
    uint32_t mask = (uint32_t)(cache->capacity - 1);
    uint32_t idx = node->id & mask;
    for (;;) {
        if (cache->ids[idx] == 0) {
            cache->ids[idx] = node->id;
            cache->types[idx] = type;
            cache->count++;
            return;
        }
        if (cache->ids[idx] == node->id) {
            cache->types[idx] = type;
            return;
        }
        idx = (idx + 1) & mask;
    }
}

// Get type at a flow node (core narrowing algorithm)
static XrType *get_type_at_flow_node(XaFlowBuilder *builder,
                                      const char *name,
                                      XrType *declared_type,
                                      XaFlowNode *flow,
                                      XaFlowCache *cache,
                                      int depth) {
    // Depth limit to prevent stack overflow
    if (depth > 100) {
        return declared_type;
    }
    
    if (!flow || (flow->flags & XA_FLOW_UNREACHABLE)) {
        return xr_type_new_never();
    }
    
    // Check cache for shared nodes
    if (flow->flags & XA_FLOW_SHARED) {
        XrType *cached = xa_flow_cache_get(cache, flow);
        if (cached) return cached;
    }
    
    XrType *result = NULL;
    
    // Process based on node type
    if (flow->flags & XA_FLOW_ASSIGNMENT) {
        // Assignment: check if this assignment affects our variable
        if (flow->assigned_name && strcmp(flow->assigned_name, name) == 0) {
            result = flow->assigned_type ? flow->assigned_type : declared_type;
        } else {
            // Continue to antecedent
            if (flow->antecedent_count > 0) {
                result = get_type_at_flow_node(builder, name, declared_type,
                                               flow->antecedents[0], cache, depth + 1);
            } else {
                result = declared_type;
            }
        }
    }
    else if (flow->flags & XA_FLOW_TRUE_CONDITION) {
        // Narrow type based on condition being true
        if (flow->antecedent_count > 0) {
            XrType *base = get_type_at_flow_node(builder, name, declared_type,
                                                  flow->antecedents[0], cache, depth + 1);
            // Apply narrowing based on condition expression
            result = apply_condition_narrowing(flow->condition_expr, name, base, true);
        } else {
            result = declared_type;
        }
    }
    else if (flow->flags & XA_FLOW_FALSE_CONDITION) {
        // Narrow type based on condition being false
        if (flow->antecedent_count > 0) {
            XrType *base = get_type_at_flow_node(builder, name, declared_type,
                                                  flow->antecedents[0], cache, depth + 1);
            // Apply narrowing based on condition expression
            result = apply_condition_narrowing(flow->condition_expr, name, base, false);
        } else {
            result = declared_type;
        }
    }
    else if (flow->flags & XA_FLOW_BRANCH_LABEL) {
        // Branch merge: union of all antecedent types
        if (flow->antecedent_count == 0) {
            result = xr_type_new_never();
        } else if (flow->antecedent_count == 1) {
            result = get_type_at_flow_node(builder, name, declared_type,
                                           flow->antecedents[0], cache, depth + 1);
        } else {
            // Compute union of all paths
            XrType *union_type = NULL;
            for (int i = 0; i < flow->antecedent_count; i++) {
                XrType *path_type = get_type_at_flow_node(builder, name, declared_type,
                                                          flow->antecedents[i], cache, depth + 1);
                union_type = xr_type_union(union_type, path_type);
            }
            result = union_type;
        }
    }
    else if (flow->flags & XA_FLOW_LOOP_LABEL) {
        // Loop: union of all antecedents (entry path + back-edges).
        // Same logic as BRANCH_LABEL — loop body may assign different types.
        if (flow->antecedent_count == 0) {
            result = declared_type;
        } else if (flow->antecedent_count == 1) {
            result = get_type_at_flow_node(builder, name, declared_type,
                                           flow->antecedents[0], cache, depth + 1);
        } else {
            XrType *union_type = NULL;
            for (int i = 0; i < flow->antecedent_count; i++) {
                XrType *path_type = get_type_at_flow_node(builder, name, declared_type,
                                                          flow->antecedents[i], cache, depth + 1);
                union_type = xr_type_union(union_type, path_type);
            }
            result = union_type;
        }
    }
    else if (flow->flags & XA_FLOW_START) {
        // Function start: use declared type
        result = declared_type;
    }
    else {
        // Default: follow antecedent
        if (flow->antecedent_count > 0) {
            result = get_type_at_flow_node(builder, name, declared_type,
                                           flow->antecedents[0], cache, depth + 1);
        } else {
            result = declared_type;
        }
    }
    
    // Cache shared nodes
    if (flow->flags & XA_FLOW_SHARED) {
        xa_flow_cache_set(cache, flow, result);
    }
    
    return result;
}

// Public API: Get narrowed type
XrType *xa_flow_get_type_of_reference(XaFlowBuilder *builder,
                                       const char *name,
                                       XrType *declared_type,
                                       XaFlowNode *flow_node,
                                       XaFlowCache *cache) {
    if (!builder || !name || !declared_type) {
        return declared_type;
    }
    if (!flow_node) {
        return declared_type;
    }
    
    // Clear cache: entries are per-variable, not reusable across queries
    if (cache) {
        xa_flow_cache_clear(cache);
    }
    
    return get_type_at_flow_node(builder, name, declared_type, flow_node, cache, 0);
}

// Narrow by typeof
XrType *xa_narrow_by_typeof(XrType *type, const char *type_name, bool assume_true) {
    if (!type || !type_name) return type;
    
    // Use -1 as sentinel since XR_KIND_INT == 0
    int target_kind = -1;
    if (strcmp(type_name, TYPE_NAME_INT64) == 0 || strcmp(type_name, TYPE_NAME_INT) == 0) {
        target_kind = XR_KIND_INT;
    } else if (strcmp(type_name, TYPE_NAME_FLOAT64) == 0 || strcmp(type_name, TYPE_NAME_FLOAT) == 0) {
        target_kind = XR_KIND_FLOAT;
    } else if (strcmp(type_name, TYPE_NAME_STRING) == 0) {
        target_kind = XR_KIND_STRING;
    } else if (strcmp(type_name, TYPE_NAME_BOOL) == 0) {
        target_kind = XR_KIND_BOOL;
    } else if (strcmp(type_name, TYPE_NAME_FUNCTION) == 0) {
        target_kind = XR_KIND_FUNCTION;
    } else if (strcmp(type_name, TYPE_NAME_ARRAY) == 0) {
        target_kind = XR_KIND_ARRAY;
    } else if (strcmp(type_name, TYPE_NAME_MAP) == 0) {
        target_kind = XR_KIND_MAP;
    } else if (strcmp(type_name, TYPE_NAME_NULL) == 0) {
        target_kind = XR_KIND_NULL;
    }
    
    if (target_kind < 0) return type;
    
    if (assume_true) {
        return xr_type_filter(type, (XrTypeKind)target_kind);
    } else {
        return xr_type_exclude(type, (XrTypeKind)target_kind);
    }
}

// Narrow by null check
XrType *xa_narrow_by_null_check(XrType *type, bool is_equal_null, bool assume_true) {
    if (!type) return type;
    
    // x == null && assumeTrue  => x is null
    // x == null && !assumeTrue => x is non-null
    // x != null && assumeTrue  => x is non-null
    // x != null && !assumeTrue => x is null
    
    bool is_null = (is_equal_null == assume_true);
    
    if (is_null) {
        return xr_type_filter(type, XR_KIND_NULL);
    } else {
        return xr_type_non_nullable(type);
    }
}

// Narrow by truthiness
XrType *xa_narrow_by_truthiness(XrType *type, bool assume_true) {
    if (!type) return type;
    
    if (assume_true) {
        // Truthy: exclude null, undefined
        return xr_type_non_nullable(type);
    } else {
        // Falsy: could be null, 0, "", false
        // Can't narrow much here without more context
        return type;
    }
}

// Check if a type is an instance of a specific class (by name)
static bool type_is_class_instance(XrType *t, const char *class_name) {
    return XR_TYPE_IS_INSTANCE(t) && t->instance.class_name &&
           strcmp(t->instance.class_name, class_name) == 0;
}

// Narrow by instanceof
XrType *xa_narrow_by_instanceof(XrType *type, const char *class_name, bool assume_true) {
    if (!type || !class_name) return type;
    
    if (assume_true) {
        // x instanceof ClassName is true => x is ClassName instance
        XrType *instance_type = xr_type_new_instance(NULL);
        if (instance_type) {
            instance_type->instance.class_name = class_name;
        }
        return instance_type;
    } else {
        // x instanceof ClassName is false => exclude ClassName from the type
        
        // Union type: rebuild without the excluded class
        if (XR_TYPE_IS_UNION(type)) {
            int total = xr_type_union_count(type);
            XrType *remaining[XR_UNION_MAX_MEMBERS];
            int count = 0;
            for (int i = 0; i < total && count < XR_UNION_MAX_MEMBERS; i++) {
                XrType *m = xr_type_union_member(type, i);
                if (!type_is_class_instance(m, class_name)) {
                    remaining[count++] = m;
                }
            }
            if (count == 0) return xr_type_new_never();
            if (count == 1) return remaining[0];
            XrType *result = xr_type_new_union(remaining, count);
            return result ? result : type;
        }
        
        // Non-union: exact match => never
        if (type_is_class_instance(type, class_name)) {
            return xr_type_new_never();
        }
        return type;
    }
}

// ============================================================================
// Move tracking for shared let variables
// ============================================================================

XaFlowNode *xa_flow_create_move(XaFlowBuilder *builder, const char *name) {
    if (!builder || !name) return NULL;
    
    XaFlowNode *node = flow_node_alloc(builder, XA_FLOW_MOVE);
    if (!node) return NULL;
    
    node->assigned_name = name;
    
    // Link to current flow
    xa_flow_add_antecedent(node, builder->current_flow);
    builder->current_flow = node;
    
    return node;
}

// Recursive helper: check if variable is moved at a given flow node
// Returns: 1 = definitely moved, 0 = not moved, -1 = maybe moved (some paths moved)
static int flow_is_moved_at(const char *name, XaFlowNode *node, int depth) {
    if (!node || depth > 64) return 0;
    
    // Moved: variable was moved on this path
    if ((node->flags & XA_FLOW_MOVE) && node->assigned_name &&
        strcmp(node->assigned_name, name) == 0) {
        return 1;
    }
    
    // Re-assigned: ownership restored, no longer moved
    if ((node->flags & XA_FLOW_ASSIGNMENT) && node->assigned_name &&
        strcmp(node->assigned_name, name) == 0) {
        return 0;
    }
    
    // Start node: variable was never moved
    if (node->flags & XA_FLOW_START) return 0;
    
    if (node->antecedent_count == 0) return 0;
    
    if (node->antecedent_count == 1) {
        return flow_is_moved_at(name, node->antecedents[0], depth + 1);
    }
    
    // Branch merge: check all paths
    int all_moved = 1;
    int any_moved = 0;
    for (int i = 0; i < node->antecedent_count; i++) {
        int r = flow_is_moved_at(name, node->antecedents[i], depth + 1);
        if (r == 0) all_moved = 0;
        if (r != 0) any_moved = 1;
    }
    if (all_moved) return 1;
    if (any_moved) return -1;  // maybe moved
    return 0;
}

// Check if variable is moved at a given flow node (walks back through flow graph)
// Returns true if the variable is DEFINITELY moved on all reaching paths.
bool xa_flow_is_moved(XaFlowBuilder *builder, const char *name, XaFlowNode *at_node) {
    if (!builder || !name || !at_node) return false;
    return flow_is_moved_at(name, at_node, 0) == 1;
}
