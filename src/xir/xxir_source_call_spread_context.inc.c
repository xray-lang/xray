/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_call_spread_context.inc.c - Contextual plans for original Tuple fields
 *
 * KEY CONCEPT: Header inference observes owned field plans without evaluating them.
 */
static bool source_call_spread_field_plans(SourceContext *ctx,AstNode *node,
    SourceExpressionPlan **plans,uint32_t limit,uint32_t *at,uint32_t depth) {
    if (!at || *at>limit || (limit && !plans))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call field plan vector is inconsistent");
    if (!node || !((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    if (depth>=128) return source_fail(ctx,node,XR_XIR_BUDGET,"call field planning depth exhausted");
    if (node->type==AST_GROUPING)
        return source_call_spread_field_plans(ctx,node->as.grouping,plans,limit,at,depth+1);
    if (node->type!=AST_TUPLE_LITERAL) {
        uint32_t width=0;
        if (!source_call_spread_shape(ctx,node,depth+1,&width)) return false;
        if (width>limit-*at)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call field plans exceed their static Tuple");
        *at+=width;
        return source_facts_kill(ctx,node);
    }
    const TupleLiteralNode *tuple=&node->as.tuple_literal;
    for (int i=0;i<tuple->count;++i) {
        AstNode *child=tuple->elements[i];
        if (!child || !((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (child->type==AST_SPREAD_EXPR) {
            if (!source_call_spread_field_plans(ctx,child->as.spread_expr.expr,plans,limit,at,depth+1)) return false;
        } else {
            if (*at>=limit)
                return source_fail(ctx,child,XR_XIR_BAD_STRUCTURE,"call field plan exceeds its static Tuple");
            SourceExpressionPlan *plan=source_plan_collect(ctx,child,(SourceExpectedType){0});
            if (!plan || !source_planned_effects(ctx,plan,0)) return false;
            plans[(*at)++]=plan;
        }
    }
    return true;
}
static SourceExpressionPlan *source_call_argument_value_plan(SourceExpressionPlan *argument) {
    if (argument->syntax->type==AST_SPREAD_EXPR && argument->call_projection) {
        SourceCallProjection *projection=argument->call_projection;
        SourceCallSpread *spread=projection->spread;
        if (spread->field_plans && spread->field_plans[projection->field])
            return spread->field_plans[projection->field];
    }
    return argument;
}
static bool source_call_spread_context(SourceContext *ctx,SourceCallSpread *spread,
    SourceExpressionPlan *literal,const SourceCallPlan *formal) {
    SourceExpectedType *fields=source_recipe_storage(ctx,spread->count,sizeof(*fields));
    SourceTuplePrefix *prefix=source_recipe_storage(ctx,1,sizeof(*prefix));
    if (!fields || !prefix) return false;
    for (uint32_t f=0;f<spread->count;++f) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        fields[f]=formal->argument_contexts[spread->first+f+formal->parameter_offset];
    }
    if(!source_tuple_prefix(ctx,fields,spread->count,spread->field_plans,prefix))return false;
    literal->tuple_prefix=prefix;return true;
}
