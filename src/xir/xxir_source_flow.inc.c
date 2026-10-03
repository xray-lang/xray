/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_flow.inc.c - Flow-sensitive null narrowing of simple local bindings
 *
 * KEY CONCEPT:
 *   A narrowed binding is still its declared Nullable storage; each read of a binding known
 *   to hold a value unwraps it, and any assignment, ref argument or loop write forgets the fact.
 */
static bool source_unwrap_value(SourceContext *ctx, AstNode *node, SourceValue nullable, SourceValue *value);
typedef struct SourceFact { struct SourceFact *next; SourceName *binding; bool non_null; } SourceFact;
typedef struct SourceFactSet { SourceName **bindings; uint32_t count, capacity; } SourceFactSet;

typedef struct SourceWriteScan {
    SourceContext *ctx;
    const char *name;
    uint32_t depth;
    bool closure_only, inside_closure, found;
} SourceWriteScan;
static bool source_write_scan(AstNode *node, void *pointer) {
    SourceWriteScan *scan = pointer;
    if (!node || scan->found) return true;
    if (!source_work(scan->ctx, node)) return false;
    if (scan->depth == 128) return source_fail(scan->ctx, node, XR_XIR_BUDGET, "write scan depth exhausted");
    bool counted = !scan->closure_only || scan->inside_closure;
    const char *written = NULL;
    switch (node->type) {
    case AST_ASSIGNMENT: written = node->as.assignment.name; break;
    case AST_COMPOUND_ASSIGNMENT: if (!node->as.compound_assignment.object) written = node->as.compound_assignment.name; break;
    case AST_INC: written = node->as.inc.name; break;
    case AST_DEC: written = node->as.dec.name; break;
    case AST_CALL_EXPR: {
        CallExprNode *call = &node->as.call_expr;
        for (int a = 0; counted && a < call->arg_count; ++a)
            if (call->arg_accesses && call->arg_accesses[a] == XR_CALL_ARG_REF && call->arguments[a] &&
                call->arguments[a]->type == AST_VARIABLE &&
                source_text_same(scan->ctx, node, call->arguments[a]->as.variable.name, scan->name)) { scan->found = true; return true; }
        break;
    }
    default: break;
    }
    if (counted && written && source_text_same(scan->ctx, node, written, scan->name)) { scan->found = true; return true; }
    bool inside = scan->inside_closure;
    if (node->type == AST_FUNCTION_EXPR || node->type == AST_FUNCTION_DECL || node->type == AST_DEFER_STMT) scan->inside_closure = true;
    ++scan->depth;
    bool ok = xr_ast_for_each_child(node, source_write_scan, scan);
    --scan->depth; scan->inside_closure = inside;
    return ok || source_fail(scan->ctx, node, XR_XIR_BAD_STRUCTURE, "unknown syntax in write scan");
}
static bool source_region_writes(SourceContext *ctx, AstNode *region, const char *name, bool closure_only, bool *writes) {
    SourceWriteScan scan = {ctx, name, 0, closure_only, false, false};
    *writes = false;
    if (!region) return true;
    if (!source_write_scan(region, &scan)) return false;
    *writes = scan.found; return true;
}
/* N-11.4: a binding assigned inside any closure never narrows anywhere in its function. */
static bool source_narrowable(SourceContext *ctx, SourceName *symbol, bool *narrowable) {
    *narrowable = false;
    if (!symbol || symbol->kind != SOURCE_LOCAL || symbol->construction || !xr_xir_type_is_nullable(&ctx->types, symbol->type))
        return true;
    /* The function's own declaration is not a closure of itself, so scan its body block. */
    AstNode *owner = ctx->bodies[ctx->function].node, *body = NULL;
    if (owner && owner->type == AST_FUNCTION_DECL) body = owner->as.function_decl.body;
    else if (owner && owner->type == AST_FUNCTION_EXPR) body = owner->as.function_expr.body;
    else if (owner && owner->type == AST_METHOD_DECL) body = owner->as.method_decl.body;
    bool writes = false;
    if (body && !source_region_writes(ctx, body, symbol->name, true, &writes)) return false;
    *narrowable = !writes; return true;
}
static bool source_fact_known(const SourceContext *ctx, const SourceName *binding) {
    for (const SourceFact *fact = ctx->facts; fact; fact = fact->next)
        if (fact->binding == binding) return fact->non_null;
    return false;
}
static bool source_fact_push(SourceContext *ctx, SourceName *binding, bool non_null) {
    SourceFact *fact = source_alloc(ctx, 1, sizeof(*fact));
    if (!fact) return false;
    *fact = (SourceFact) {ctx->facts, binding, non_null};
    ctx->facts = fact; return true;
}
/* The static type of a binding at this program point: its element while a fact says it holds a value. */
static XrXirType source_symbol_type(const SourceContext *ctx, const SourceName *symbol) {
    if (symbol && symbol->kind == SOURCE_LOCAL && source_fact_known(ctx, symbol) &&
        xr_xir_type_is_nullable(&ctx->types, symbol->type)) return xr_xir_nullable_element(&ctx->types, symbol->type);
    return symbol ? symbol->type : XR_XIR_UNIT;
}
static bool source_fact_collect(SourceContext *ctx, SourceFactSet *set, SourceName *binding) {
    bool narrowable;
    if (!source_narrowable(ctx, binding, &narrowable)) return false;
    if (!narrowable) return true;
    for (uint32_t i = 0; i < set->count; ++i) if (set->bindings[i] == binding) return true;
    if (set->count == set->capacity) {
        uint32_t capacity = set->capacity ? set->capacity * 2 : 4;
        SourceName **bindings = source_alloc(ctx, capacity, sizeof(*bindings));
        if (!bindings || !source_copy_bytes(ctx, NULL, bindings, set->bindings, set->count * sizeof(*bindings))) return false;
        set->bindings = bindings; set->capacity = capacity;
    }
    set->bindings[set->count++] = binding; return true;
}
/* The bindings known to hold a value when `condition` evaluates to `direction` (N-4). */
static bool source_condition_facts(SourceContext *ctx, AstNode *condition, bool direction, SourceFactSet *set, uint32_t depth) {
    while (condition && condition->type == AST_GROUPING) condition = condition->as.grouping;
    if (!condition || !source_work(ctx, condition)) return condition == NULL;
    if (depth >= 128) return source_fail(ctx, condition, XR_XIR_BUDGET, "condition fact depth exhausted");
    switch (condition->type) {
    case AST_UNARY_NOT: return source_condition_facts(ctx, condition->as.unary.operand, !direction, set, depth + 1);
    case AST_BINARY_AND:
        return !direction || (source_condition_facts(ctx, condition->as.binary.left, true, set, depth + 1) &&
            source_condition_facts(ctx, condition->as.binary.right, true, set, depth + 1));
    case AST_BINARY_OR:
        return direction || (source_condition_facts(ctx, condition->as.binary.left, false, set, depth + 1) &&
            source_condition_facts(ctx, condition->as.binary.right, false, set, depth + 1));
    case AST_BINARY_EQ: case AST_BINARY_NE: {
        AstNode *left = condition->as.binary.left, *right = condition->as.binary.right;
        bool null_left = left && left->type == AST_LITERAL_NULL, null_right = right && right->type == AST_LITERAL_NULL;
        if (null_left == null_right) return true;
        AstNode *subject = null_left ? right : left;
        if (subject->type != AST_VARIABLE) return true;
        /* `!= null` holds a value when true, `== null` when false. */
        if (direction != (condition->type == AST_BINARY_NE)) return true;
        return source_fact_collect(ctx, set, visible_name(ctx, subject->as.variable.name));
    }
    default: return true;
    }
}
static bool source_facts_apply(SourceContext *ctx, const SourceFactSet *set) {
    for (uint32_t i = 0; i < set->count; ++i)
        if (!source_fact_push(ctx, set->bindings[i], true)) return false;
    return true;
}
/* The facts of both paths that still hold at a join, evaluated against `first` and `second`. */
static bool source_facts_join(SourceContext *ctx, SourceFact *first, SourceFact *second, SourceFact **joined) {
    SourceFact *saved = ctx->facts, *result = NULL;
    *joined = NULL;
    for (SourceFact *fact = first; fact; fact = fact->next) {
        if (!source_work(ctx, NULL)) return false;
        if (!fact->non_null) continue;
        ctx->facts = first; bool in_first = source_fact_known(ctx, fact->binding);
        ctx->facts = second; bool in_second = source_fact_known(ctx, fact->binding);
        ctx->facts = result; bool present = source_fact_known(ctx, fact->binding);
        if (!in_first || !in_second || present) continue;
        if (!source_fact_push(ctx, fact->binding, true)) { ctx->facts = saved; return false; }
        result = ctx->facts;
    }
    ctx->facts = saved; *joined = result; return true;
}
/* After a construct whose paths are not tracked, forget every fact about a binding it may write. */
static bool source_facts_kill(SourceContext *ctx, AstNode *region) {
    for (SourceFact *fact = ctx->facts; fact; fact = fact->next) {
        if (!source_work(ctx, region)) return false;
        if (!fact->non_null || !source_fact_known(ctx, fact->binding)) continue;
        bool writes;
        if (!source_region_writes(ctx, region, fact->binding->name, false, &writes)) return false;
        if (writes && !source_fact_push(ctx, fact->binding, false)) return false;
    }
    return true;
}
/* Only a conversion that wrapped a present value proves the assigned expression holds one. */
static bool source_value_is_some(const SourceContext *ctx, SourceValue value) {
    const SourceFunction *body = &ctx->bodies[ctx->function];
    uint32_t parameters = ctx->functions[ctx->function].parameter_count;
    if (value.id < parameters || value.id - parameters >= body->count) return false;
    return body->recipes[value.id - parameters].instruction.op == XR_XIR_NULLABLE_SOME;
}
/* An assignment replaces the static type with the assigned expression's. */
static bool source_facts_assign(SourceContext *ctx, SourceName *binding, bool holds) {
    bool narrowable = false;
    if (!source_narrowable(ctx, binding, &narrowable)) return false;
    if (!narrowable) return true;
    return source_fact_push(ctx, binding, holds && !ctx->flow_alternatives);
}
typedef struct SourceBreakScan { SourceContext *ctx; uint32_t depth; bool found; } SourceBreakScan;
static bool source_break_scan(AstNode *node, void *pointer) {
    SourceBreakScan *scan = pointer;
    if (!node || scan->found) return true;
    if (!source_work(scan->ctx, node)) return false;
    if (node->type == AST_BREAK_STMT) { scan->found = true; return true; }
    if (scan->depth == 128) return source_fail(scan->ctx, node, XR_XIR_BUDGET, "break scan depth exhausted");
    ++scan->depth;
    bool ok = xr_ast_for_each_child(node, source_break_scan, scan);
    --scan->depth;
    return ok || source_fail(scan->ctx, node, XR_XIR_BAD_STRUCTURE, "unknown syntax in break scan");
}
/* A loop that can break leaves with the facts of the break point, so its exit condition proves nothing. */
static bool source_has_break(SourceContext *ctx, AstNode *region, bool *found) {
    SourceBreakScan scan = {ctx, 0, false};
    *found = false;
    if (region && !source_break_scan(region, &scan)) return false;
    *found = scan.found; return true;
}
