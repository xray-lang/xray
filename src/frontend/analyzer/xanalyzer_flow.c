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
#include "xtype_ref_resolve.h"
#include "../../base/xchecks.h"
#include "../parser/xast.h"
#include "../parser/xtype_ref.h"
#include "../../runtime/value/xtype_names.h"
#include "../../base/xmalloc.h"
#include <string.h>

static int type_member_to_tid(AstNode *node) {
    if (!node || node->type != AST_MEMBER_ACCESS)
        return -1;
    MemberAccessNode *ma = &node->as.member_access;
    if (!ma->object || ma->object->type != AST_VARIABLE || !ma->object->as.variable.name ||
        strcmp(ma->object->as.variable.name, "Type") != 0)
        return -1;
    return xr_type_from_name(ma->name);
}

static bool flow_type_matches_tref(const XrType *type, const XrTypeRef *tref) {
    if (!type || !tref)
        return false;
    if (tref->kind == XR_TREF_NAMED || tref->kind == XR_TREF_GENERIC) {
        const char *name = tref->name;
        if (!name)
            return false;
        if (type->kind == XR_KIND_ENUM) {
            if (!type->enum_type.enum_name || strcmp(type->enum_type.enum_name, name) != 0)
                return false;
            if (tref->kind != XR_TREF_GENERIC)
                return type->enum_type.type_arg_count == 0;
            if (type->enum_type.type_arg_count != tref->nchildren)
                return false;
            for (uint8_t i = 0; i < tref->nchildren; i++) {
                if (!flow_type_matches_tref(type->enum_type.type_args[i], tref->children[i]))
                    return false;
            }
            return true;
        }
        if (type->kind == XR_KIND_INSTANCE) {
            if (!type->instance.class_name || strcmp(type->instance.class_name, name) != 0)
                return false;
            if (tref->kind != XR_TREF_GENERIC)
                return type->instance.type_arg_count == 0;
            if (type->instance.type_arg_count != tref->nchildren)
                return false;
            for (uint8_t i = 0; i < tref->nchildren; i++) {
                if (!flow_type_matches_tref(type->instance.type_args[i], tref->children[i]))
                    return false;
            }
            return true;
        }
        if (strcmp(name, "int") == 0)
            return type->kind == XR_KIND_INT;
        if (strcmp(name, "float") == 0)
            return type->kind == XR_KIND_FLOAT;
        if (strcmp(name, "bool") == 0)
            return type->kind == XR_KIND_BOOL;
        if (strcmp(name, "string") == 0)
            return type->kind == XR_KIND_STRING;
    }
    return false;
}

static XrType *flow_narrow_by_tref(XrType *base_type, const XrTypeRef *tref, bool assume_true) {
    if (!base_type || !tref || !XR_TYPE_IS_UNION(base_type))
        return NULL;
    XrType *members[XR_UNION_MAX_MEMBERS];
    int count = 0;
    for (int i = 0; i < xr_type_union_count(base_type) && count < XR_UNION_MAX_MEMBERS; i++) {
        XrType *member = xr_type_union_member(base_type, i);
        bool matches = flow_type_matches_tref(member, tref);
        if (matches == assume_true)
            members[count++] = member;
    }
    if (count == 0)
        return xr_type_new_never(NULL);
    if (count == 1)
        return members[0];
    return xr_type_new_union(NULL, members, count);
}

// Apply type narrowing based on condition expression
// Analyzes common patterns: x != null, x == null, typeOf(x) == Type.xxx, truthiness
static XrType *apply_condition_narrowing(XrAstNode *expr, const char *var_name, XrType *base_type,
                                         bool assume_true) {
    if (!expr || !var_name || !base_type)
        return base_type;

    AstNode *node = (AstNode *) expr;
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
        if (operand && operand->type == AST_VARIABLE && operand->as.variable.name &&
            strcmp(operand->as.variable.name, var_name) == 0) {
            return xa_narrow_by_truthiness(base_type, !assume_true);
        }
        return base_type;
    }

    // Pattern: x == null, x != null
    // Pattern: typeOf(x) == Type.xxx
    if (type == AST_BINARY_EQ || type == AST_BINARY_NE) {
        AstNode *left = node->as.binary.left;
        AstNode *right = node->as.binary.right;

        bool is_equal = (type == AST_BINARY_EQ);

        // Check if comparing variable to null
        bool var_on_left = (left && left->type == AST_VARIABLE && left->as.variable.name &&
                            strcmp(left->as.variable.name, var_name) == 0);
        bool var_on_right = (right && right->type == AST_VARIABLE && right->as.variable.name &&
                             strcmp(right->as.variable.name, var_name) == 0);
        bool null_on_left = (left && left->type == AST_LITERAL_NULL);
        bool null_on_right = (right && right->type == AST_LITERAL_NULL);

        if ((var_on_left && null_on_right) || (var_on_right && null_on_left)) {
            return xa_narrow_by_null_check(base_type, is_equal, assume_true);
        }

        // Check for typeOf pattern: typeOf(x) == Type.xxx
        // In Xray, typeOf is a builtin function call: AST_CALL_EXPR
        AstNode *typeof_operand = NULL;
        int type_id = -1;

        // Check left side for typeOf(x) call
        if (left && left->type == AST_CALL_EXPR && left->as.call_expr.callee &&
            left->as.call_expr.callee->type == AST_VARIABLE &&
            left->as.call_expr.callee->as.variable.name &&
            strcmp(left->as.call_expr.callee->as.variable.name, "typeOf") == 0 &&
            left->as.call_expr.arg_count == 1) {
            typeof_operand = left->as.call_expr.arguments[0];
            type_id = type_member_to_tid(right);
        }
        // Check right side for typeOf(x) call
        else if (right && right->type == AST_CALL_EXPR && right->as.call_expr.callee &&
                 right->as.call_expr.callee->type == AST_VARIABLE &&
                 right->as.call_expr.callee->as.variable.name &&
                 strcmp(right->as.call_expr.callee->as.variable.name, "typeOf") == 0 &&
                 right->as.call_expr.arg_count == 1) {
            typeof_operand = right->as.call_expr.arguments[0];
            type_id = type_member_to_tid(left);
        }

        if (typeof_operand && type_id >= 0 && typeof_operand->type == AST_VARIABLE &&
            typeof_operand->as.variable.name &&
            strcmp(typeof_operand->as.variable.name, var_name) == 0) {
            // typeOf(x) == Type.xxx with assume_true => narrow to type
            // typeOf(x) != Type.xxx with assume_true => exclude type
            bool effective_true = (is_equal == assume_true);
            return xa_narrow_by_typeid(base_type, (XrTypeId) type_id, effective_true);
        }
    }

    // Pattern: a && b (logical AND)
    // true branch: both conditions hold, apply narrowing from both
    // false branch: at least one is false (cannot narrow safely)
    if (type == AST_BINARY_AND) {
        if (assume_true) {
            XrType *narrowed = apply_condition_narrowing((XrAstNode *) node->as.binary.left,
                                                         var_name, base_type, true);
            return apply_condition_narrowing((XrAstNode *) node->as.binary.right, var_name,
                                             narrowed, true);
        }
        return base_type;
    }

    // Pattern: a || b (logical OR)
    // false branch: both conditions are false, apply narrowing from both
    // true branch: at least one is true (cannot narrow safely)
    if (type == AST_BINARY_OR) {
        if (!assume_true) {
            XrType *narrowed = apply_condition_narrowing((XrAstNode *) node->as.binary.left,
                                                         var_name, base_type, false);
            return apply_condition_narrowing((XrAstNode *) node->as.binary.right, var_name,
                                             narrowed, false);
        }
        return base_type;
    }

    // Pattern: x is ClassName (instanceof check)
    if (type == AST_IS_EXPR) {
        IsExprNode *is_expr = &node->as.is_expr;
        if (is_expr->expr && is_expr->expr->type == AST_VARIABLE &&
            is_expr->expr->as.variable.name &&
            strcmp(is_expr->expr->as.variable.name, var_name) == 0 && is_expr->type) {
            XrTypeRef *tref = is_expr->type;
            // Extract class name from NAMED / GENERIC type refs
            if ((tref->kind == XR_TREF_NAMED || tref->kind == XR_TREF_GENERIC) && tref->name) {
                XrType *precise = flow_narrow_by_tref(base_type, tref, assume_true);
                if (precise)
                    return precise;
                return xa_narrow_by_instanceof(base_type, tref->name, assume_true);
            }
            // For primitive type checks (x is int, x is string, etc.)
            // resolve the type ref and narrow directly
            if (assume_true) {
                return xr_tref_resolve(NULL, tref);
            }
        }
    }

    return base_type;
}

// Helper: allocate a flow node
static XaFlowNode *flow_node_alloc(XaFlowBuilder *builder, uint32_t flags) {
    XR_DCHECK(builder != NULL, "flow_node_alloc: NULL builder");
    XaFlowNode *node = xr_calloc(1, sizeof(XaFlowNode));
    if (!node)
        return NULL;

    node->flags = flags;
    node->id = builder->next_id++;

    // Track all nodes for cleanup
    if (builder->node_count >= builder->node_capacity) {
        int new_cap = builder->node_capacity == 0 ? 64 : builder->node_capacity * 2;
        XR_REALLOC_OR_ABORT(builder->all_nodes, sizeof(XaFlowNode *) * (size_t) new_cap,
                            "flow all_nodes grow");
        builder->node_capacity = new_cap;
    }
    builder->all_nodes[builder->node_count++] = node;

    return node;
}

// Create flow builder
XaFlowBuilder *xa_flow_builder_new(void) {
    XaFlowBuilder *builder = xr_calloc(1, sizeof(XaFlowBuilder));
    if (!builder)
        return NULL;

    builder->next_id = 1;
    builder->unreachable_flow = flow_node_alloc(builder, XA_FLOW_UNREACHABLE);
    builder->current_flow = builder->unreachable_flow;

    return builder;
}

// Free flow builder
void xa_flow_builder_free(XaFlowBuilder *builder) {
    if (!builder)
        return;

    // Free all nodes
    for (int i = 0; i < builder->node_count; i++) {
        XaFlowNode *node = builder->all_nodes[i];
        if (node->antecedents)
            xr_free(node->antecedents);
        xr_free(node);
    }
    if (builder->all_nodes)
        xr_free(builder->all_nodes);

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
XaFlowNode *xa_flow_create_assignment(XaFlowBuilder *builder, XrAstNode *node, const char *name,
                                      XrType *type) {
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
XaFlowNode *xa_flow_create_condition(XaFlowBuilder *builder, XrAstNode *expr, bool is_true_branch) {
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
    if (!node || !antecedent)
        return;
    if (antecedent->flags & XA_FLOW_UNREACHABLE)
        return;

    // Check if already present
    for (int i = 0; i < node->antecedent_count; i++) {
        if (node->antecedents[i] == antecedent)
            return;
    }

    // Add to array
    if (node->antecedent_count >= node->antecedent_capacity) {
        int new_cap = node->antecedent_capacity == 0 ? 4 : node->antecedent_capacity * 2;
        XR_REALLOC_OR_ABORT(node->antecedents, sizeof(XaFlowNode *) * (size_t) new_cap,
                            "flow antecedents grow");
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
    if (!label)
        return builder->unreachable_flow;

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
    if (builder)
        builder->current_flow = flow ? flow : builder->unreachable_flow;
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
    if (!cache)
        return NULL;
    cache->capacity = 64;
    cache->ids = xr_calloc(cache->capacity, sizeof(uint32_t));
    cache->types = xr_calloc(cache->capacity, sizeof(XrType *));
    if (!cache->ids || !cache->types) {
        xa_flow_cache_free(cache);
        return NULL;
    }
    return cache;
}

// Free flow cache
void xa_flow_cache_free(XaFlowCache *cache) {
    if (!cache)
        return;
    if (cache->ids)
        xr_free(cache->ids);
    if (cache->types)
        xr_free(cache->types);
    xr_free(cache);
}

// Clear flow cache
void xa_flow_cache_clear(XaFlowCache *cache) {
    if (!cache)
        return;
    memset(cache->ids, 0, sizeof(uint32_t) * cache->capacity);
    cache->count = 0;
}

// Get from cache (open-addressing probe with node->id as key)
XrType *xa_flow_cache_get(XaFlowCache *cache, XaFlowNode *node) {
    if (!cache || !node || node->id == 0)
        return NULL;
    uint32_t mask = (uint32_t) (cache->capacity - 1);
    uint32_t idx = node->id & mask;
    for (int probe = 0; probe < cache->capacity; probe++) {
        uint32_t slot = (idx + probe) & mask;
        if (cache->ids[slot] == 0)
            return NULL;
        if (cache->ids[slot] == node->id)
            return cache->types[slot];
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
    cache->types = xr_calloc(cache->capacity, sizeof(XrType *));
    if (!cache->ids || !cache->types) {
        cache->ids = old_ids;
        cache->types = old_types;
        cache->capacity = old_cap;
        return;
    }
    cache->count = 0;

    uint32_t mask = (uint32_t) (cache->capacity - 1);
    for (int i = 0; i < old_cap; i++) {
        if (old_ids[i] == 0)
            continue;
        uint32_t idx = old_ids[i] & mask;
        while (cache->ids[idx] != 0)
            idx = (idx + 1) & mask;
        cache->ids[idx] = old_ids[i];
        cache->types[idx] = old_types[i];
        cache->count++;
    }

    xr_free(old_ids);
    xr_free(old_types);
}

// Set in cache
void xa_flow_cache_set(XaFlowCache *cache, XaFlowNode *node, XrType *type) {
    if (!cache || !node || node->id == 0)
        return;

    // Rehash at 70% load
    if (cache->count * 10 >= cache->capacity * 7) {
        flow_cache_rehash(cache);
    }

    uint32_t mask = (uint32_t) (cache->capacity - 1);
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
static XrType *get_type_at_flow_node(XaFlowBuilder *builder, const char *name,
                                     XrType *declared_type, XaFlowNode *flow, XaFlowCache *cache,
                                     int depth) {
    // Depth limit to prevent stack overflow
    if (depth > 100) {
        return declared_type;
    }

    if (!flow || (flow->flags & XA_FLOW_UNREACHABLE)) {
        return xr_type_new_never(NULL);
    }

    // Check cache for shared nodes
    if (flow->flags & XA_FLOW_SHARED) {
        XrType *cached = xa_flow_cache_get(cache, flow);
        if (cached)
            return cached;
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
                result = get_type_at_flow_node(builder, name, declared_type, flow->antecedents[0],
                                               cache, depth + 1);
            } else {
                result = declared_type;
            }
        }
    } else if (flow->flags & XA_FLOW_TRUE_CONDITION) {
        // Narrow type based on condition being true
        if (flow->antecedent_count > 0) {
            XrType *base = get_type_at_flow_node(builder, name, declared_type, flow->antecedents[0],
                                                 cache, depth + 1);
            // Apply narrowing based on condition expression
            result = apply_condition_narrowing(flow->condition_expr, name, base, true);
        } else {
            result = declared_type;
        }
    } else if (flow->flags & XA_FLOW_FALSE_CONDITION) {
        // Narrow type based on condition being false
        if (flow->antecedent_count > 0) {
            XrType *base = get_type_at_flow_node(builder, name, declared_type, flow->antecedents[0],
                                                 cache, depth + 1);
            // Apply narrowing based on condition expression
            result = apply_condition_narrowing(flow->condition_expr, name, base, false);
        } else {
            result = declared_type;
        }
    } else if (flow->flags & XA_FLOW_BRANCH_LABEL) {
        // Branch merge: union of all antecedent types
        if (flow->antecedent_count == 0) {
            result = xr_type_new_never(NULL);
        } else if (flow->antecedent_count == 1) {
            result = get_type_at_flow_node(builder, name, declared_type, flow->antecedents[0],
                                           cache, depth + 1);
        } else {
            // Compute union of all paths
            XrType *union_type = NULL;
            for (int i = 0; i < flow->antecedent_count; i++) {
                XrType *path_type = get_type_at_flow_node(builder, name, declared_type,
                                                          flow->antecedents[i], cache, depth + 1);
                union_type = xr_type_union(NULL, union_type, path_type);
            }
            result = union_type;
        }
    } else if (flow->flags & XA_FLOW_LOOP_LABEL) {
        // Loop: union of all antecedents (entry path + back-edges).
        // Same logic as BRANCH_LABEL — loop body may assign different types.
        if (flow->antecedent_count == 0) {
            result = declared_type;
        } else if (flow->antecedent_count == 1) {
            result = get_type_at_flow_node(builder, name, declared_type, flow->antecedents[0],
                                           cache, depth + 1);
        } else {
            XrType *union_type = NULL;
            for (int i = 0; i < flow->antecedent_count; i++) {
                XrType *path_type = get_type_at_flow_node(builder, name, declared_type,
                                                          flow->antecedents[i], cache, depth + 1);
                union_type = xr_type_union(NULL, union_type, path_type);
            }
            result = union_type;
        }
    } else if (flow->flags & XA_FLOW_START) {
        // Function start: use declared type
        result = declared_type;
    } else {
        // Default: follow antecedent
        if (flow->antecedent_count > 0) {
            result = get_type_at_flow_node(builder, name, declared_type, flow->antecedents[0],
                                           cache, depth + 1);
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
XrType *xa_flow_get_type_of_reference(XaFlowBuilder *builder, const char *name,
                                      XrType *declared_type, XaFlowNode *flow_node,
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

// Narrow by typeof TypeId
XrType *xa_narrow_by_typeid(XrType *type, XrTypeId type_id, bool assume_true) {
    if (!type)
        return type;

    int target_kind = -1;
    switch (type_id) {
        case XR_TID_I8:
        case XR_TID_U8:
        case XR_TID_I16:
        case XR_TID_U16:
        case XR_TID_I32:
        case XR_TID_U32:
        case XR_TID_INT:
        case XR_TID_U64:
            target_kind = XR_KIND_INT;
            break;
        case XR_TID_F32:
        case XR_TID_FLOAT:
            target_kind = XR_KIND_FLOAT;
            break;
        case XR_TID_STRING:
            target_kind = XR_KIND_STRING;
            break;
        case XR_TID_BOOL:
            target_kind = XR_KIND_BOOL;
            break;
        case XR_TID_RUNE:
            target_kind = XR_KIND_RUNE;
            break;
        case XR_TID_FUNCTION:
            target_kind = XR_KIND_FUNCTION;
            break;
        case XR_TID_ARRAY:
            target_kind = XR_KIND_ARRAY;
            break;
        case XR_TID_MAP:
            target_kind = XR_KIND_MAP;
            break;
        case XR_TID_SET:
            target_kind = XR_KIND_SET;
            break;
        case XR_TID_NULL:
            target_kind = XR_KIND_NULL;
            break;
        default:
            break;
    }

    if (target_kind < 0)
        return type;

    if (assume_true) {
        return xr_type_filter(NULL, type, (XrTypeKind) target_kind);
    } else {
        return xr_type_exclude(NULL, type, (XrTypeKind) target_kind);
    }
}

XrType *xa_narrow_by_typeof(XrType *type, const char *type_name, bool assume_true) {
    return xa_narrow_by_typeid(type, (XrTypeId) xr_type_from_name(type_name), assume_true);
}

// Narrow by null check
XrType *xa_narrow_by_null_check(XrType *type, bool is_equal_null, bool assume_true) {
    if (!type)
        return type;

    // x == null && assumeTrue  => x is null
    // x == null && !assumeTrue => x is non-null
    // x != null && assumeTrue  => x is non-null
    // x != null && !assumeTrue => x is null

    bool is_null = (is_equal_null == assume_true);

    if (is_null) {
        return xr_type_filter(NULL, type, XR_KIND_NULL);
    } else {
        return xr_type_non_nullable(NULL, type);
    }
}

// Narrow by truthiness
XrType *xa_narrow_by_truthiness(XrType *type, bool assume_true) {
    if (!type)
        return type;

    if (assume_true) {
        // Truthy: exclude null, undefined
        return xr_type_non_nullable(NULL, type);
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
    if (!type || !class_name)
        return type;

    if (assume_true) {
        // x instanceof ClassName is true => x is ClassName instance
        XrType *instance_type = xr_type_new_instance(NULL, NULL);
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
            if (count == 0)
                return xr_type_new_never(NULL);
            if (count == 1)
                return remaining[0];
            XrType *result = xr_type_new_union(NULL, remaining, count);
            return result ? result : type;
        }

        // Non-union: exact match => never
        if (type_is_class_instance(type, class_name)) {
            return xr_type_new_never(NULL);
        }
        return type;
    }
}

// ============================================================================
// Move tracking for explicit ownership transfer of local variables
// ============================================================================

XaFlowNode *xa_flow_create_move(XaFlowBuilder *builder, const char *name) {
    if (!builder || !name)
        return NULL;

    XaFlowNode *node = flow_node_alloc(builder, XA_FLOW_MOVE);
    if (!node)
        return NULL;

    node->assigned_name = name;

    // Link to current flow
    xa_flow_add_antecedent(node, builder->current_flow);
    builder->current_flow = node;

    return node;
}

XaBindingUseState xa_flow_binding_use_state(XaFlowBuilder *builder, const char *name,
                                            XaFlowNode *at_node) {
    if (!builder || !name || !at_node)
        return XA_BINDING_LIVE;

    /* Walk the complete backward slice instead of imposing an arbitrary
     * recursion depth. Long straight-line functions are ordinary programs,
     * not analysis failures. Marking nodes when enqueued also terminates CFG
     * cycles; the loop preheader/backedge terminals still contribute their
     * respective lattice states. Allocation or malformed graph metadata is
     * UNKNOWN so ownership-sensitive consumers fail closed. */
    size_t id_count = builder->next_id ? (size_t) builder->next_id : 1;
    size_t stack_capacity = builder->node_count ? (size_t) builder->node_count : 1;
    bool *seen = (bool *) xr_calloc(id_count, sizeof(bool));
    XaFlowNode **stack = (XaFlowNode **) xr_malloc(stack_capacity * sizeof(*stack));
    if (!seen || !stack) {
        xr_free(seen);
        xr_free(stack);
        return XA_BINDING_UNKNOWN;
    }

    bool any_live = false;
    bool any_moved = false;
    bool malformed = at_node->id >= id_count;
    size_t count = 0;
    if (!malformed) {
        stack[count++] = at_node;
        seen[at_node->id] = true;
    }
    while (count > 0 && !malformed) {
        XaFlowNode *node = stack[--count];
        if ((node->flags & XA_FLOW_MOVE) && node->assigned_name &&
            strcmp(node->assigned_name, name) == 0) {
            any_moved = true;
            continue;
        }
        if (((node->flags & XA_FLOW_ASSIGNMENT) && node->assigned_name &&
             strcmp(node->assigned_name, name) == 0) ||
            (node->flags & XA_FLOW_START) || node->antecedent_count == 0) {
            any_live = true;
            continue;
        }
        for (int i = 0; i < node->antecedent_count; i++) {
            XaFlowNode *antecedent = node->antecedents ? node->antecedents[i] : NULL;
            if (!antecedent || antecedent->id >= id_count) {
                malformed = true;
                break;
            }
            if (seen[antecedent->id])
                continue;
            if (count >= stack_capacity) {
                malformed = true;
                break;
            }
            seen[antecedent->id] = true;
            stack[count++] = antecedent;
        }
    }

    xr_free(seen);
    xr_free(stack);
    if (malformed || (!any_live && !any_moved))
        return XA_BINDING_UNKNOWN;
    if (any_live && any_moved)
        return XA_BINDING_MAYBE_MOVED;
    return any_moved ? XA_BINDING_MOVED : XA_BINDING_LIVE;
}
