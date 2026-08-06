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
#include "xanalyzer_ast_visitor.h"
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
    /* Primitive keyword type refs (`x is int`) carry their own kind rather than
     * a name, so they must be matched before the NAMED/GENERIC path. Spec
     * §2.13 N-6 requires both narrowing directions to treat them exactly like
     * named types. */
    switch (tref->kind) {
        case XR_TREF_INT:
        case XR_TREF_INT_WIDTH:
            return type->kind == XR_KIND_INT;
        case XR_TREF_FLOAT:
        case XR_TREF_FLOAT_WIDTH:
            return type->kind == XR_KIND_FLOAT;
        case XR_TREF_STRING:
            return type->kind == XR_KIND_STRING;
        case XR_TREF_BOOL:
            return type->kind == XR_KIND_BOOL;
        case XR_TREF_RUNE:
            return type->kind == XR_KIND_RUNE;
        case XR_TREF_UNIT:
            return type->kind == XR_KIND_UNIT;
        case XR_TREF_NULL:
            return type->kind == XR_KIND_NULL;
        case XR_TREF_TUPLE:
            return type->kind == XR_KIND_TUPLE;
        case XR_TREF_FUNCTION:
            return type->kind == XR_KIND_FUNCTION;
        default:
            break;
    }
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

/* Narrow the non-null part of a type by an `is T` test (spec §2.13 N-6).
 *
 * `null` is carried by a flag rather than a union member, so the caller peels
 * it off first and re-attaches it; this function only sees non-null types.
 * A union keeps the members that match (true direction) or that do not match
 * (false direction). A non-union base narrows to the resolved target in the
 * true direction — that is what makes a downcast `animal is Dog` useful — and
 * only collapses to `never` when the test is statically impossible. */
static XrType *flow_narrow_nonnull_by_tref(XrType *base_type, const XrTypeRef *tref,
                                           bool assume_true) {
    if (!base_type || !tref)
        return base_type;
    if (XR_TYPE_IS_UNION(base_type)) {
        XrType *members[XR_UNION_MAX_MEMBERS];
        int count = 0;
        for (int i = 0; i < xr_type_union_count(base_type) && count < XR_UNION_MAX_MEMBERS; i++) {
            XrType *member = xr_type_union_member(base_type, i);
            if (flow_type_matches_tref(member, tref) == assume_true)
                members[count++] = member;
        }
        if (count == 0)
            return xr_type_new_never(NULL);
        if (count == 1)
            return members[0];
        return xr_type_new_union(NULL, members, count);
    }
    bool matches = flow_type_matches_tref(base_type, tref);
    if (assume_true) {
        if (matches)
            return base_type;
        /* Not a syntactic match: either an impossible test or a downcast to a
         * subtype the name comparison cannot see. Resolving the target keeps
         * `animal is Dog` useful; an unresolvable target leaves the base type
         * untouched rather than inventing an unknown. */
        XrType *resolved = xr_tref_resolve(NULL, tref);
        if (!resolved || XR_TYPE_IS_UNKNOWN(resolved) || XR_TYPE_IS_ERROR(resolved))
            return base_type;
        return resolved;
    }
    /* False direction: only an exact match can be removed. Anything else may
     * still hold at run time (a base class, an interface, `Json`), so the type
     * is left alone. */
    return matches ? xr_type_new_never(NULL) : base_type;
}

static XrType *flow_narrow_by_tref(XrType *base_type, const XrTypeRef *tref, bool assume_true) {
    if (!base_type || !tref)
        return NULL;

    /* `x is null` is the type-test spelling of a null check. */
    if (tref->kind == XR_TREF_NULL)
        return xa_narrow_by_null_check(base_type, true, assume_true);

    bool has_null = XR_TYPE_IS_NULLABLE(base_type);
    XrType *non_null = has_null ? xr_type_non_nullable(NULL, base_type) : base_type;
    if (XR_TYPE_IS_NULL(base_type)) {
        /* The whole type is `null`: a non-null test can only fail. */
        return assume_true ? xr_type_new_never(NULL) : base_type;
    }

    XrType *narrowed = flow_narrow_nonnull_by_tref(non_null, tref, assume_true);
    if (!narrowed)
        return NULL;
    /* `null` never satisfies a non-null type test, so it survives only on the
     * false side. */
    if (has_null && !assume_true) {
        if (XR_TYPE_IS_NEVER(narrowed))
            return xr_type_new_null(NULL);
        return xr_type_make_nullable(NULL, narrowed);
    }
    return narrowed;
}

/* Is this expression the narrowable subject `var_name` (spec §2.13 N-1)?
 * Only a bare identifier qualifies; parentheses around it are transparent.
 * Fields, indices, and call results are deliberately excluded (N-2). */
static bool flow_expr_is_subject(const AstNode *node, const char *var_name) {
    while (node && node->type == AST_GROUPING)
        node = node->as.grouping;
    return node && node->type == AST_VARIABLE && node->as.variable.name && var_name &&
           strcmp(node->as.variable.name, var_name) == 0;
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
        if (flow_expr_is_subject(node, var_name)) {
            return xa_narrow_by_truthiness(base_type, assume_true);
        }
        return base_type;
    }

    // Pattern: (e) — grouping is transparent to narrowing (spec §2.13 N-4).
    if (type == AST_GROUPING) {
        return apply_condition_narrowing((XrAstNode *) node->as.grouping, var_name, base_type,
                                         assume_true);
    }

    // Pattern: !e — the operand's facts swap direction (spec §2.13 N-4).
    if (type == AST_UNARY_NOT) {
        return apply_condition_narrowing((XrAstNode *) node->as.unary.operand, var_name, base_type,
                                         !assume_true);
    }

    // Pattern: x == null, x != null
    // Pattern: typeOf(x) == Type.xxx
    if (type == AST_BINARY_EQ || type == AST_BINARY_NE) {
        AstNode *left = node->as.binary.left;
        AstNode *right = node->as.binary.right;

        bool is_equal = (type == AST_BINARY_EQ);

        // Check if comparing variable to null
        bool var_on_left = flow_expr_is_subject(left, var_name);
        bool var_on_right = flow_expr_is_subject(right, var_name);
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

        if (typeof_operand && type_id >= 0 && flow_expr_is_subject(typeof_operand, var_name)) {
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

    // Pattern: x is T (spec §2.13 N-6). Both directions narrow, and every type
    // ref kind — primitive keyword, named class, generic instance — goes
    // through the same path.
    if (type == AST_IS_EXPR) {
        IsExprNode *is_expr = &node->as.is_expr;
        if (is_expr->type && flow_expr_is_subject(is_expr->expr, var_name)) {
            XrType *precise = flow_narrow_by_tref(base_type, is_expr->type, assume_true);
            if (precise)
                return precise;
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
    if (builder->closure_written_names)
        xr_free(builder->closure_written_names);

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
        // Branch merge (spec §2.13 N-9): union of all antecedent types;
        // unreachable predecessors contribute `never` and are absorbed.
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
        // Loop header: union of the entry edge and every back edge (spec
        // §2.13 N-10). A back edge that reaches this header again without
        // passing an assignment carries the header's own type — assignments
        // terminate the walk — so the cycle contributes nothing and the union
        // is decided by the entry edge and by the assignments in the body.
        if (flow->flags & XA_FLOW_IN_PROGRESS) {
            return xr_type_new_never(NULL);
        }
        flow->flags |= XA_FLOW_IN_PROGRESS;
        if (flow->antecedent_count == 0) {
            result = declared_type;
        } else {
            XrType *union_type = NULL;
            for (int i = 0; i < flow->antecedent_count; i++) {
                XrType *path_type = get_type_at_flow_node(builder, name, declared_type,
                                                          flow->antecedents[i], cache, depth + 1);
                union_type = xr_type_union(NULL, union_type, path_type);
            }
            result = union_type;
        }
        flow->flags &= ~(uint32_t) XA_FLOW_IN_PROGRESS;
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

    if (type_id == XR_TID_OBJECT) {
        if (XR_TYPE_IS_UNION(type)) {
            XrType *members[XR_UNION_MAX_MEMBERS];
            int count = 0;
            for (int i = 0; i < xr_type_union_count(type) && count < XR_UNION_MAX_MEMBERS; i++) {
                XrType *member = xr_type_union_member(type, i);
                bool is_object = member && (member->kind == XR_KIND_JSON ||
                                            member->kind == XR_KIND_STRUCT_OBJECT);
                if (is_object == assume_true)
                    members[count++] = member;
            }
            return count == 0 ? xr_type_new_never(NULL)
                              : (count == 1 ? members[0] : xr_type_new_union(NULL, members, count));
        }
        bool is_object = type->kind == XR_KIND_JSON || type->kind == XR_KIND_STRUCT_OBJECT;
        return is_object == assume_true ? type : xr_type_new_never(NULL);
    }

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

// ============================================================================
// Closure-written bindings (spec §2.13 N-11.4)
//
// A binding assigned from inside a nested closure can change whenever that
// closure runs, which the flow graph does not model, so it never narrows. The
// set is collected before a function body is traversed, so a closure written
// *after* the narrowing site suppresses it just the same. Name-keyed, like
// narrowing itself (§2.13 N-1 limits subjects to plain identifiers).
// ============================================================================

typedef struct XaClosureWriteScan {
    XaFlowBuilder *builder;
    int closure_depth;
} XaClosureWriteScan;

static void flow_closure_write_add(XaFlowBuilder *builder, const char *name) {
    if (!builder || !name)
        return;
    for (int i = 0; i < builder->closure_written_count; i++) {
        if (strcmp(builder->closure_written_names[i], name) == 0)
            return;
    }
    if (builder->closure_written_count >= builder->closure_written_capacity) {
        int new_cap = builder->closure_written_capacity ? builder->closure_written_capacity * 2 : 8;
        XR_REALLOC_OR_ABORT(builder->closure_written_names, sizeof(const char *) * (size_t) new_cap,
                            "closure-written names grow");
        builder->closure_written_capacity = new_cap;
    }
    builder->closure_written_names[builder->closure_written_count++] = name;
}

static bool flow_node_opens_closure(const AstNode *node) {
    return node->type == AST_FUNCTION_EXPR || node->type == AST_FUNCTION_DECL ||
           node->type == AST_METHOD_DECL;
}

static void flow_closure_write_pre(AstNode *node, void *vscan) {
    XaClosureWriteScan *scan = (XaClosureWriteScan *) vscan;
    if (flow_node_opens_closure(node)) {
        scan->closure_depth++;
        return;
    }
    if (scan->closure_depth == 0)
        return;
    switch (node->type) {
        case AST_ASSIGNMENT:
            flow_closure_write_add(scan->builder, node->as.assignment.name);
            break;
        case AST_COMPOUND_ASSIGNMENT:
            flow_closure_write_add(scan->builder, node->as.compound_assignment.name);
            break;
        case AST_INC:
            flow_closure_write_add(scan->builder, node->as.inc.name);
            break;
        case AST_DEC:
            flow_closure_write_add(scan->builder, node->as.dec.name);
            break;
        default:
            break;
    }
}

static void flow_closure_write_post(AstNode *node, void *vscan) {
    XaClosureWriteScan *scan = (XaClosureWriteScan *) vscan;
    if (flow_node_opens_closure(node))
        scan->closure_depth--;
}

int xa_flow_closure_writes_collect(XaFlowBuilder *builder, XrAstNode *body) {
    if (!builder)
        return 0;
    int saved = builder->closure_written_count;
    if (body) {
        XaClosureWriteScan scan = {.builder = builder, .closure_depth = 0};
        xa_ast_walk((AstNode *) body, flow_closure_write_pre, flow_closure_write_post, &scan);
    }
    return saved;
}

void xa_flow_closure_writes_restore(XaFlowBuilder *builder, int saved_count) {
    if (builder)
        builder->closure_written_count = saved_count;
}

bool xa_flow_narrowing_suppressed(const XaFlowBuilder *builder, const char *name) {
    if (!builder || !name)
        return false;
    for (int i = 0; i < builder->closure_written_count; i++) {
        if (strcmp(builder->closure_written_names[i], name) == 0)
            return true;
    }
    return false;
}

/* `assert(cond)` / `assert_true(cond)` / `assert_false(cond)` inject the
 * condition's fact into the following code (spec §2.13 N-7). The assert
 * builtins always evaluate and always throw on failure — they are never
 * stripped — so code after them may rely on the condition holding. */
void xa_flow_apply_assert_narrowing(XaFlowBuilder *builder, XrAstNode *expr) {
    if (!builder || !expr)
        return;
    AstNode *node = (AstNode *) expr;
    if (node->type != AST_CALL_EXPR)
        return;
    CallExprNode *call = &node->as.call_expr;
    if (!call->callee || call->callee->type != AST_VARIABLE || call->arg_count < 1)
        return;
    const char *name = call->callee->as.variable.name;
    if (!name)
        return;
    bool assume_true;
    if (strcmp(name, "assert") == 0 || strcmp(name, "assert_true") == 0)
        assume_true = true;
    else if (strcmp(name, "assert_false") == 0)
        assume_true = false;
    else
        return;
    AstNode *condition = call->arguments ? call->arguments[0] : NULL;
    if (!condition)
        return;
    builder->current_flow = xa_flow_create_condition(builder, (XrAstNode *) condition, assume_true);
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
