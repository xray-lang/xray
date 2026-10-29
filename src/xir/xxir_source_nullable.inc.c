/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_nullable.inc.c - Presence tests, force unwrap and null coalescing
 *
 * KEY CONCEPT:
 *   Each form inspects only the outermost Nullable layer; none requires the element to be
 *   comparable, and only the unwrapping forms can fail, with the null-unwrap panic.
 */
static bool source_nullable_prepare(SourceContext *ctx,AstNode *site,XrHirNullableInput input,
    XrHirNullablePlan *output) {
    XrXirSourceRange range=source_query_range(ctx,site,NULL);
    input.origin=(XrHirNullableOrigin){site?site->node_id:0,range.module,range.line,range.column,range.end_line,range.end_column};
    XrHirNullableContext context={&ctx->types,ctx->request,ctx,source_conversion_intern};
    const char *reason=NULL;XrXirStatus status=xr_hir_nullable_prepare(&context,&input,output,&reason);
    return status==XR_XIR_OK || source_fail(ctx,site,status,reason);
}
static bool source_nullable_check(SourceContext *ctx,AstNode *site,const XrHirNullablePlan *plan,
    XrXirType actual_default,XrHirNullable *output) {
    const char *reason=NULL;XrXirStatus status=xr_hir_nullable_check(plan,actual_default,ctx->request,output,&reason);
    return status==XR_XIR_OK || source_fail(ctx,site,status,reason);
}
static bool source_nullable_value(SourceContext *ctx,AstNode *site,XrHirNullableInput input,
    uint32_t operand,SourceValue *value) {
    XrHirNullablePlan plan;XrHirNullable typed;
    return source_nullable_prepare(ctx,site,input,&plan) && source_nullable_check(ctx,site,&plan,XR_XIR_UNIT,&typed) &&
        source_recipe_record(ctx,xr_hir_nullable_instruction(&typed,operand),value);
}
static bool source_null_syntax(SourceContext *ctx,const AstNode *node) {
    uint32_t depth=0;
    while (node && node->type==AST_GROUPING) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (++depth>=128) return source_fail(ctx,(AstNode *)node,XR_XIR_BUDGET,"null grouping depth exhausted");
        node=node->as.grouping;
    }
    return node && node->type==AST_LITERAL_NULL;
}
/* Grouping preserves the null literal's exact nullable context. */
static bool source_null_literal(SourceContext *ctx,const SourceExpressionPlan *plan) {
    return plan && source_null_syntax(ctx,plan->syntax);
}
static bool source_null_discard(SourceContext *ctx,SourceExpressionPlan *plan) {
    uint32_t depth=0;
    while (plan) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        plan->state=SOURCE_TERM_DISCARDED;
        if (plan->syntax->type!=AST_GROUPING) return plan->syntax->type==AST_LITERAL_NULL;
        if (++depth>=128) return source_fail(ctx,plan->syntax,XR_XIR_BUDGET,"null grouping depth exhausted");
        plan=plan->left;
    }
    return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"null grouping has no collected child");
}
/* Presence does not require an equality contract for the element. */
static bool source_null_comparison(SourceContext *ctx,const SourceExpressionPlan *plan) {
    return plan && plan->left && plan->right &&
        (source_null_literal(ctx,plan->left) != source_null_literal(ctx,plan->right));
}
static bool source_null_test(SourceContext *ctx, AstNode *node, SourceValue *value) {
    SourceExpressionPlan *plan = ctx->active_expression;
    bool null_on_left = source_null_literal(ctx,plan->left);
    SourceExpressionPlan *subject = null_on_left ? plan->right : plan->left;
    if (!source_null_discard(ctx,null_on_left?plan->left:plan->right)) return false;
    SourceValue operand;
    if (!source_plan_complete(ctx, subject, &operand)) return false;
    return source_nullable_value(ctx,node,(XrHirNullableInput){.role=node->type==AST_BINARY_EQ?
        XR_HIR_NULLABLE_EQUAL_NULL:XR_HIR_NULLABLE_UNEQUAL_NULL,.source=operand.type},operand.id,value);
}
static bool source_unwrap_value(SourceContext *ctx, AstNode *node, SourceValue nullable, SourceValue *value) {
    return source_nullable_value(ctx,node,(XrHirNullableInput){.role=XR_HIR_NULLABLE_UNWRAP,
        .source=nullable.type},nullable.id,value);
}
/* Explicit consumption first claims the matching layer of this read. */
static bool source_force_unwrap(SourceContext *ctx,AstNode *node,SourceValue *value) {
    SourceExpressionPlan *plan=ctx->active_expression;SourceValue operand;
    if (!plan->left || !source_plan_complete(ctx,plan->left,&operand)) return false;
    source_read_forward(plan,plan->left);
    if (plan->read_claims) {--plan->read_claims;*value=operand;return true;}
    XrHirNullablePlan types;XrHirNullable typed;
    if(!source_nullable_prepare(ctx,node,(XrHirNullableInput){.role=XR_HIR_NULLABLE_FORCE,.source=operand.type},&types) ||
        !source_nullable_check(ctx,node,&types,XR_XIR_UNIT,&typed))return false;
    if (typed.kind==XR_HIR_NULLABLE_IDENTITY) {*value=operand;return true;}
    if (plan->present_prefix) --plan->present_prefix;
    return source_recipe_record(ctx,xr_hir_nullable_instruction(&typed,operand.id),value);
}
/* The unreachable alternative is still a real member of the same Built /
 * Checked program: ordinary planning checks its contracts and effects once.
 * Its result never participates in the already narrowed read's SSA value. */
static bool source_nullable_dead_default(SourceContext *ctx,SourceExpressionPlan *plan,
    SourceExpectedType expected,XrXirType *actual_default) {
    SourceValue always,ignored;SourceFunction *body=&ctx->bodies[ctx->function];
    SourceFact *facts=ctx->facts;SourceEpoch *epochs=ctx->epochs;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_BOOL,XR_XIR_BOOL,{0},{0},1,{0}},&always)) return false;
    uint32_t branch=body->count,yes=body->block_count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_BRANCH,XR_XIR_UNIT,{always.id},{0},0,{0}},NULL) || !begin_block(ctx)) return false;
    uint32_t yes_jump=body->count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0,{0}},NULL)) return false;
    uint32_t no=body->block_count;
    if (!begin_block(ctx)) return false;
    plan->right=source_plan_collect(ctx,plan->syntax->as.binary.right,expected);
    if (!plan->right || !source_plan_complete(ctx,plan->right,&ignored)) return false;
    ctx->facts=facts;ctx->epochs=epochs;
    uint32_t no_jump=body->count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0,{0}},NULL)) return false;
    uint32_t join=body->block_count;
    if (!begin_block(ctx)) return false;
    body->recipes[branch].instruction.targets[0]=yes;body->recipes[branch].instruction.targets[1]=no;
    body->recipes[yes_jump].instruction.targets[0]=join;body->recipes[no_jump].instruction.targets[0]=join;
    *actual_default=ignored.type;
    return true;
}
static bool source_coalesce(SourceContext *ctx,AstNode *node,SourceExpectedType expected,SourceValue *value) {
    SourceExpressionPlan *plan=ctx->active_expression;SourceValue subject,some;
    if (!plan->left || !source_plan_complete(ctx,plan->left,&subject)) return false;
    XrHirNullablePlan types;XrHirNullable typed;
    bool claimed=plan->left->read_claims!=0;
    if (claimed) {
        source_read_forward(plan,plan->left);
        XrXirType element=subject.type;
        if (plan->read_claims) {
            element=plan->read_declared;
            uint32_t consumed=plan->read_prefix-plan->read_claims;
            for (uint32_t layer=0;layer<=consumed;++layer) {
                if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
                if (!xr_xir_type_is_nullable(&ctx->types,element))
                    return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"nullable claim has no declared layer");
                element=xr_xir_nullable_element(&ctx->types,element);
            }
            --plan->read_claims;
        }
        if(!source_nullable_prepare(ctx,node,(XrHirNullableInput){.role=XR_HIR_NULLABLE_DEAD_DEFAULT,
            .source=subject.type,.default_base=element,.null_default=source_null_syntax(ctx,node->as.binary.right)},&types))return false;
    } else if(!source_nullable_prepare(ctx,node,(XrHirNullableInput){.role=XR_HIR_NULLABLE_COALESCE,
        .source=subject.type,.has_expected=expected.present,.expected=expected.type,
        .null_default=source_null_syntax(ctx,node->as.binary.right)},&types))return false;
    if (types.kind==XR_HIR_NULLABLE_IDENTITY) {
        if(!claimed)source_read_forward(plan,plan->left);
        XrXirType actual_default;
        if (!source_nullable_dead_default(ctx,plan,(SourceExpectedType){true,types.default_type,false, false},&actual_default) ||
            !source_nullable_check(ctx,node,&types,actual_default,&typed)) return false;
        *value=subject;return true;
    }
    XrXirType result=types.result;
    if (!source_nullable_value(ctx,node,(XrHirNullableInput){.role=XR_HIR_NULLABLE_UNEQUAL_NULL,
        .source=subject.type},subject.id,&some)) return false;
    SourceFunction *body=&ctx->bodies[ctx->function];
    if (!body->block_count && !begin_block(ctx)) return false;
    uint32_t branch=body->count,yes_block=body->block_count;
    SourceValue yes=subject,no;SourceFact *entry_facts=ctx->facts;SourceEpoch *entry_epochs=ctx->epochs;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_BRANCH,XR_XIR_UNIT,{some.id},{0},0,{0}},NULL) || !begin_block(ctx)) return false;
    if (types.unwrap_present && !source_unwrap_value(ctx,node,subject,&yes)) return false;
    uint32_t yes_end=body->current_block.identity,yes_jump=body->count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0,{0}},NULL)) return false;
    uint32_t no_block=body->block_count;
    if (!begin_block(ctx)) return false;
    plan->right=source_plan_collect(ctx,node->as.binary.right,(SourceExpectedType){true,result,false, false});
    if (!plan->right || !source_plan_complete(ctx,plan->right,&no)) return false;
    SourceFact *no_facts=ctx->facts,*joined=NULL;
    if (!source_facts_join(ctx,entry_facts,entry_epochs,no_facts,ctx->epochs,&joined)) return false;
    ctx->facts=joined;
    if (!source_nullable_check(ctx,node,&types,no.type,&typed)) return false;
    result=typed.result;
    uint32_t no_end=body->current_block.identity,no_jump=body->count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0,{0}},NULL)) return false;
    uint32_t join=body->block_count;
    if (!begin_block(ctx)) return false;
    body->recipes[branch].instruction.targets[0]=yes_block;body->recipes[branch].instruction.targets[1]=no_block;
    body->recipes[yes_jump].instruction.targets[0]=join;body->recipes[no_jump].instruction.targets[0]=join;
    if (result==XR_XIR_UNIT) { *value=(SourceValue){UINT32_MAX,XR_XIR_UNIT};return true; }
    SourceValue inputs[]={{yes_end,XR_XIR_UNIT},yes,{no_end,XR_XIR_UNIT},no};
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_PHI,result,{0},{0},0,{0}},inputs,4,value);
}
