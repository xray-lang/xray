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
static bool source_null_syntax(SourceContext *ctx,const AstNode *node);
static uint32_t source_fact_depth(SourceContext *ctx,const SourceName *binding);
static bool source_unwrap_value(SourceContext *ctx, AstNode *node, SourceValue nullable, SourceValue *value);
typedef struct SourceFact { struct SourceFact *next; SourceName *binding; uint32_t prefix; uint64_t epoch; } SourceFact;
typedef struct SourceFactEvidence { SourceName *binding; uint32_t prefix; uint64_t epoch; } SourceFactEvidence;
typedef struct SourceFactSet { SourceFactEvidence *entries; uint32_t count, capacity; } SourceFactSet;
static void source_read_forward(SourceExpressionPlan *target,const SourceExpressionPlan *source) {
    target->read_binding=source->read_binding;target->read_declared=source->read_declared;
    target->read_prefix=source->read_prefix;target->read_claims=source->read_claims;
    target->present_prefix=source->present_prefix;target->read_epoch=source->read_epoch;
}

static const SourceExpressionPlan *source_existing_plan(SourceContext *ctx,const AstNode *syntax) {
    const SourceFunction *body=&ctx->bodies[ctx->function];
    for (const SourceExpressionStorage *storage=body->expressions;storage;storage=storage->next)
        for (uint32_t e=storage->count;e;--e) {
            if (!source_work(ctx,(AstNode *)syntax)) return NULL;
            const SourceExpressionPlan *plan=&storage->expressions[e-1];
            if (plan->owner==ctx->function && plan->syntax==syntax) return plan;
        }
    return NULL;
}
static bool source_default_is_dead(SourceContext *ctx,const SourceExpressionPlan *plan) {
    if (!plan || !plan->left) return false;
    const SourceExpressionPlan *left=plan->left;
    bool proven=left->state==SOURCE_TERM_GROUND || !left->read_binding ||
        source_fact_depth(ctx,left->read_binding)>=left->read_prefix;
    return proven && (left->read_claims || (left->type_ready && !xr_xir_type_is_nullable(&ctx->types,left->ground_type)));
}
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
    if (!scan->closure_only && node->type==AST_NULLISH_COALESCE) {
        const SourceExpressionPlan *plan=source_existing_plan(scan->ctx,node);
        if (scan->ctx->diagnostic.status!=XR_XIR_OK) return false;
        if (source_default_is_dead(scan->ctx,plan)) {
            ++scan->depth;bool ok=source_write_scan(node->as.binary.left,scan);--scan->depth;return ok;
        }
    }
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
struct SourceEpoch { SourceEpoch *next;SourceName *binding;uint64_t identity; };
struct SourceConditionFlow { SourceFact *facts[2];SourceEpoch *epochs[2]; };
static uint64_t source_epoch(SourceContext *ctx,const SourceEpoch *epochs,const SourceName *binding) {
    for (const SourceEpoch *epoch=epochs;epoch;epoch=epoch->next) {
        if (!source_work(ctx,NULL)) return 0;
        if (epoch->binding==binding) return epoch->identity;
    }
    return 0;
}
static bool source_epoch_advance(SourceContext *ctx,SourceName *binding) {
    if (ctx->next_epoch==UINT64_MAX) return source_fail(ctx,NULL,XR_XIR_BUDGET,"mutation identity exhausted");
    SourceEpoch *epoch=source_alloc(ctx,1,sizeof(*epoch));if (!epoch) return false;
    *epoch=(SourceEpoch){ctx->epochs,binding,++ctx->next_epoch};ctx->epochs=epoch;return true;
}
static bool source_epochs_join(SourceContext *ctx,SourceEpoch *first,SourceEpoch *second) {
    ctx->epochs=first;
    for (unsigned pass=0;pass<2;++pass)
        for (SourceEpoch *p=pass?second:first;p;p=p->next) {
            if (!source_work(ctx,NULL)) return false;
            uint64_t a=source_epoch(ctx,first,p->binding),b=source_epoch(ctx,second,p->binding);
            uint64_t current=source_epoch(ctx,ctx->epochs,p->binding);
            if (ctx->diagnostic.status!=XR_XIR_OK) return false;
            if (a!=b && (current==a || current==b) && !source_epoch_advance(ctx,p->binding)) return false;
        }
    return true;
}
/* A prefix belongs to this lexical binding, not to a type or an SSA opcode. */
static uint32_t source_fact_depth(SourceContext *ctx,const SourceName *binding) {
    for (const SourceFact *fact=ctx->facts;fact;fact=fact->next) {
        if (!source_work(ctx,NULL)) return 0;
        if (fact->binding==binding) return fact->epoch==source_epoch(ctx,ctx->epochs,binding)?fact->prefix:0;
    }
    return 0;
}
static bool source_fact_push(SourceContext *ctx,SourceName *binding,uint32_t prefix) {
    SourceFact *fact=source_alloc(ctx,1,sizeof(*fact));
    if (!fact) return false;
    if (!prefix && !source_epoch_advance(ctx,binding)) return false;
    uint64_t epoch=source_epoch(ctx,ctx->epochs,binding);
    if (ctx->diagnostic.status!=XR_XIR_OK) return false;
    *fact=(SourceFact){ctx->facts,binding,prefix,epoch};ctx->facts=fact;return true;
}
static XrXirType source_symbol_type(SourceContext *ctx,const SourceName *symbol) {
    XrXirType type=symbol?symbol->type:XR_XIR_UNIT;
    uint32_t prefix=symbol && symbol->kind==SOURCE_LOCAL?source_fact_depth(ctx,symbol):0;
    for (uint32_t layer=0;layer<prefix;++layer) {
        if (!source_work(ctx,NULL)) return XR_XIR_UNIT;
        if (!xr_xir_type_is_nullable(&ctx->types,type)) {
            source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"nullable fact exceeds its declared prefix");return XR_XIR_UNIT;
        }
        type=xr_xir_nullable_element(&ctx->types,type);
    }
    return type;
}
static bool source_fact_collect(SourceContext *ctx,SourceFactSet *set,const SourceExpressionPlan *read) {
    if (!read || !read->read_binding) return true;
    SourceName *binding=read->read_binding;bool narrowable;
    if (!source_narrowable(ctx,binding,&narrowable)) return false;
    if (!narrowable || read->read_epoch!=source_epoch(ctx,ctx->epochs,binding) || read->read_declared!=binding->type || !read->type_ready ||
        !xr_xir_type_is_nullable(&ctx->types,read->ground_type)) return true;
    if (read->read_prefix==UINT32_MAX) return source_fail(ctx,read->syntax,XR_XIR_BUDGET,"nullable prefix exhausted");
    uint32_t prefix=read->read_prefix+1;
    for (uint32_t i=0;i<set->count;++i) {
        if (!source_work(ctx,read->syntax)) return false;
        if (set->entries[i].binding==binding) {
            if (prefix>set->entries[i].prefix) set->entries[i].prefix=prefix;
            return true;
        }
    }
    if (set->count==set->capacity) {
        uint32_t capacity=set->capacity?set->capacity*2:4;
        if (capacity<set->capacity) return source_fail(ctx,read->syntax,XR_XIR_BUDGET,"nullable fact set exhausted");
        SourceFactEvidence *entries=source_alloc(ctx,capacity,sizeof(*entries));
        if (!entries || !source_copy_bytes(ctx,NULL,entries,set->entries,set->count*sizeof(*entries))) return false;
        set->entries=entries;set->capacity=capacity;
    }
    set->entries[set->count++]=(SourceFactEvidence){binding,prefix,read->read_epoch};return true;
}
/* Conditions reuse their collected reads, including the facts used for a
 * short-circuit right operand. No AST re-inference or value evaluation occurs. */
static bool source_condition_facts(SourceContext *ctx,const SourceExpressionPlan *condition,
    bool direction,SourceFactSet *set,uint32_t depth) {
    if (!condition || !source_work(ctx,condition->syntax)) return condition==NULL;
    if (depth>=128) return source_fail(ctx,condition->syntax,XR_XIR_BUDGET,"condition fact depth exhausted");
    switch(condition->syntax->type) {
    case AST_GROUPING:return source_condition_facts(ctx,condition->left,direction,set,depth+1);
    case AST_UNARY_NOT:return source_condition_facts(ctx,condition->left,!direction,set,depth+1);
    case AST_BINARY_AND:
        return !direction || (source_condition_facts(ctx,condition->left,true,set,depth+1) &&
            source_condition_facts(ctx,condition->right,true,set,depth+1));
    case AST_BINARY_OR:
        return direction || (source_condition_facts(ctx,condition->left,false,set,depth+1) &&
            source_condition_facts(ctx,condition->right,false,set,depth+1));
    case AST_BINARY_EQ:case AST_BINARY_NE: {
        const SourceExpressionPlan *left=condition->left,*right=condition->right;
        bool a=left && source_null_syntax(ctx,left->syntax),b=right && source_null_syntax(ctx,right->syntax);
        if (ctx->diagnostic.status!=XR_XIR_OK) return false;
        if (a==b || direction!=(condition->syntax->type==AST_BINARY_NE)) return true;
        const SourceExpressionPlan *read=a?right:left;
        while (read && read->syntax->type==AST_GROUPING) {
            if (!source_work(ctx,read->syntax)) return false;
            if (++depth>=128) return source_fail(ctx,read->syntax,XR_XIR_BUDGET,"condition grouping depth exhausted");
            read=read->left;
        }
        if (!read || read->syntax->type!=AST_VARIABLE) return true;
        return source_fact_collect(ctx,set,read);
    }
    default:return true;
    }
}
static bool source_facts_apply(SourceContext *ctx,const SourceFactSet *set) {
    for (uint32_t i=0;i<set->count;++i) {
        if (set->entries[i].epoch!=source_epoch(ctx,ctx->epochs,set->entries[i].binding)) continue;
        uint32_t prefix=source_fact_depth(ctx,set->entries[i].binding);
        if (ctx->diagnostic.status!=XR_XIR_OK) return false;
        if (prefix<set->entries[i].prefix && !source_fact_push(ctx,set->entries[i].binding,set->entries[i].prefix)) return false;
    }
    return true;
}
/* A condition's outcome retains the last valid read on that exact path.
 * The ordinary continuation joins those paths separately. */
static bool source_condition_capture(SourceContext *ctx,SourceExpressionPlan *plan) {
    SourceConditionFlow *flow=source_alloc(ctx,1,sizeof(*flow));
    if (!flow) return false;
    SourceFact *facts=ctx->facts;SourceEpoch *epochs=ctx->epochs;
    for (unsigned direction=0;direction<2;++direction) {
        SourceFactSet evidence={0};ctx->facts=facts;ctx->epochs=epochs;
        if (!source_condition_facts(ctx,plan,direction!=0,&evidence,0) || !source_facts_apply(ctx,&evidence)) return false;
        flow->facts[direction]=ctx->facts;flow->epochs[direction]=ctx->epochs;
    }
    ctx->facts=facts;ctx->epochs=epochs;plan->condition_flow=flow;return true;
}
static bool source_condition_prepare(SourceContext *ctx,SourceExpressionPlan *plan);
static bool source_condition_enter(SourceContext *ctx,SourceExpressionPlan *plan,bool direction) {
    if (!plan->condition_flow && !source_condition_prepare(ctx,plan)) return false;
    const SourceConditionFlow *flow=plan->condition_flow?plan->condition_flow:plan->planning_flow;
    if (!flow) return source_fail(ctx,plan->syntax,XR_XIR_BAD_STRUCTURE,"condition has no planned flow");
    ctx->facts=flow->facts[direction?1:0];ctx->epochs=flow->epochs[direction?1:0];return true;
}
static bool source_facts_join(SourceContext *ctx,SourceFact *first,SourceEpoch *first_epochs,
    SourceFact *second,SourceEpoch *second_epochs,SourceFact **joined) {
    SourceFact *saved=ctx->facts,*result=NULL;
    if (!source_epochs_join(ctx,first_epochs,second_epochs)) return false;
    SourceEpoch *joined_epochs=ctx->epochs;*joined=NULL;
    for (SourceFact *fact=first;fact;fact=fact->next) {
        if (!source_work(ctx,NULL)) {ctx->facts=saved;ctx->epochs=joined_epochs;return false;}
        if (!fact->prefix) continue;
        ctx->facts=first;ctx->epochs=first_epochs;uint32_t a=source_fact_depth(ctx,fact->binding);
        ctx->facts=second;ctx->epochs=second_epochs;uint32_t b=source_fact_depth(ctx,fact->binding);
        ctx->facts=result;ctx->epochs=joined_epochs;uint32_t present=source_fact_depth(ctx,fact->binding);
        if (ctx->diagnostic.status!=XR_XIR_OK) {ctx->facts=saved;return false;}
        uint32_t prefix=a<b?a:b;
        if (!prefix || present) continue;
        if (!source_fact_push(ctx,fact->binding,prefix)) {ctx->facts=saved;return false;}
        result=ctx->facts;
    }
    ctx->facts=saved;ctx->epochs=joined_epochs;*joined=result;return true;
}
static bool source_facts_kill(SourceContext *ctx,AstNode *region) {
    for (SourceFact *fact=ctx->facts;fact;fact=fact->next) {
        if (!source_work(ctx,region)) return false;
        if (!fact->prefix || !source_fact_depth(ctx,fact->binding)) continue;
        bool writes;
        if (!source_region_writes(ctx,region,fact->binding->name,false,&writes)) return false;
        if (writes && !source_fact_push(ctx,fact->binding,0)) return false;
    }
    return ctx->diagnostic.status==XR_XIR_OK;
}
/* Planning and completion use the same directional join rule. */
static bool source_condition_combine(SourceContext *ctx,AstNode *node,SourceConditionFlow *left,
    SourceConditionFlow *right,SourceConditionFlow **output) {
    if (!left || (node->type!=AST_UNARY_NOT && !right))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"logical operand has no condition flow");
    SourceConditionFlow *flow=source_alloc(ctx,1,sizeof(*flow));if (!flow) return false;
    if (node->type==AST_UNARY_NOT) {
        flow->facts[0]=left->facts[1];flow->epochs[0]=left->epochs[1];
        flow->facts[1]=left->facts[0];flow->epochs[1]=left->epochs[0];
    } else {
        unsigned direct=node->type==AST_BINARY_AND?1u:0u,joined=1u-direct;
        flow->facts[direct]=right->facts[direct];flow->epochs[direct]=right->epochs[direct];
        if (!source_facts_join(ctx,left->facts[joined],left->epochs[joined],
                right->facts[joined],right->epochs[joined],&flow->facts[joined])) return false;
        flow->epochs[joined]=ctx->epochs;
    }
    *output=flow;return true;
}
/* Transfers follow source evaluation order, so a later call cannot revive an
 * earlier assignment. Assignments which execute on every path keep their checked construction
 * proof. A fact about the original binding is not a proof about an unwrapped
 * payload; only an explicit Some conversion supplies a new known layer. */
static bool source_planned_effects(SourceContext *ctx,const SourceExpressionPlan *plan,uint32_t depth) {
    if (!plan || !source_work(ctx,plan->syntax)) return plan==NULL;
    if (depth>=128) return source_fail(ctx,plan->syntax,XR_XIR_BUDGET,"assignment planning depth exhausted");
    switch (plan->syntax->type) {
    case AST_ASSIGNMENT:
        if (!source_planned_effects(ctx,plan->left,depth+1)) return false;
        if (plan->binding) {
            bool narrowable=false;
            if (!source_narrowable(ctx,plan->binding,&narrowable)) return false;
            if (narrowable) {
                uint32_t prefix=ctx->flow_alternatives?0:plan->present_prefix;
                if (prefix && !source_epoch_advance(ctx,plan->binding)) return false;
                if (!source_fact_push(ctx,plan->binding,prefix)) return false;
            }
        }
        return true;
    case AST_GROUPING:case AST_FORCE_UNWRAP:
        return source_planned_effects(ctx,plan->left,depth+1);
    case AST_NULLISH_COALESCE:
        if (!source_planned_effects(ctx,plan->left,depth+1)) return false;
        return source_default_is_dead(ctx,plan) || source_facts_kill(ctx,plan->syntax->as.binary.right);
    case AST_BINARY_ADD:case AST_BINARY_SUB:case AST_BINARY_MUL:case AST_BINARY_DIV:case AST_BINARY_MOD:
    case AST_BINARY_EQ:case AST_BINARY_NE:case AST_BINARY_LT:case AST_BINARY_LE:case AST_BINARY_GT:case AST_BINARY_GE:
        return source_planned_effects(ctx,plan->left,depth+1) && source_planned_effects(ctx,plan->right,depth+1);
    default:return source_facts_kill(ctx,plan->syntax);
    }
}
static bool source_condition_prepare(SourceContext *ctx,SourceExpressionPlan *plan) {
    if (plan->planning_flow || plan->condition_flow) return true;
    SourceFact *facts=ctx->facts;SourceEpoch *epochs=ctx->epochs;bool ok=true;
    AstNode *node=plan->syntax;
    if (node->type==AST_GROUPING) {
        ok=plan->left && source_condition_prepare(ctx,plan->left);
        if (ok) plan->planning_flow=plan->left->planning_flow;
    } else if (node->type==AST_UNARY_NOT || node->type==AST_BINARY_AND || node->type==AST_BINARY_OR) {
        ok=plan->left && source_condition_prepare(ctx,plan->left);
        if (ok && node->type!=AST_UNARY_NOT) ok=plan->right && source_condition_prepare(ctx,plan->right);
        if (ok) ok=source_condition_combine(ctx,node,plan->left->planning_flow,
            node->type==AST_UNARY_NOT?NULL:plan->right->planning_flow,&plan->planning_flow);
    } else {
        ok=source_planned_effects(ctx,plan,0) && source_condition_capture(ctx,plan);
        if (ok) {plan->planning_flow=plan->condition_flow;plan->condition_flow=NULL;}
    }
    ctx->facts=facts;ctx->epochs=epochs;return ok;
}
static bool source_facts_assign(SourceContext *ctx,SourceName *binding,uint32_t prefix) {
    bool narrowable=false;
    if (!source_narrowable(ctx,binding,&narrowable)) return false;
    if (!narrowable) return true;
    if (prefix && !source_epoch_advance(ctx,binding)) return false;
    return source_fact_push(ctx,binding,ctx->flow_alternatives?0:prefix);
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
