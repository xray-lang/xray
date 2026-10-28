/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_try_optional.inc.c - Lexical business-error to nullable continuation
 */
static bool source_try_optional(SourceContext *ctx,SourceExpressionPlan *plan,SourceValue *value) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    SourceLocalHandler *local=source_alloc(ctx,1,sizeof(*local));
    XrXirType *type=source_alloc(ctx,1,sizeof(*type));
    bool *complete=source_alloc(ctx,1,sizeof(*complete));
    SourceHandlerRecipe *route=source_alloc(ctx,1,sizeof(*route));
    if (!local || !type || !complete || !route) return false;
    *type=XR_XIR_ERROR;*complete=true;
    *local=(SourceLocalHandler){body->local_handler,type,complete,1};
    route->propagation=UINT32_MAX;
    SourceErrorContext handler={.frontier=body->frontier,.route=route};
    SourceErrorContext *outer=body->error_context;
    body->local_handler=local;body->error_context=&handler;
    bool ok=source_facts_kill(ctx,plan->syntax) && source_plan_complete(ctx,plan->left,value);
    body->local_handler=local->previous;body->error_context=outer;
    if (!ok || !source_facts_kill(ctx,plan->syntax)) return false;
    XrXirType result=value->type;
    if (!xr_xir_type_is_nullable(&ctx->types,result)) {
        if (!source_nullable_type(ctx,result,&result) || !source_recipe_record(ctx,
            (XrXirInstruction){XR_XIR_NULLABLE_SOME,result,{value->type==XR_XIR_UNIT?0:value->id},{0},0,{0}},value)) return false;
    }
    if (!handler.count) return true;
    if (!source_root_join_record(ctx,value)) return false;
    uint32_t success=body->current_block.identity,jump=body->count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0,{0}},NULL)) return false;
    uint32_t target=body->block_count;
    if (!begin_block(ctx)) return false;
    SourceValue *inputs=source_alloc(ctx,(size_t)handler.count*2,sizeof(*inputs));
    if (!inputs) return false;
    uint32_t at=handler.count;
    for (SourceErrorEdge *edge=handler.edges;edge;edge=edge->next) {
        if (!source_request_poll(ctx,plan->syntax)) return false;
        body->recipes[edge->jump].instruction.targets[0]=target;
        --at;edge->input=at;
        inputs[at*2]=(SourceValue){edge->block,XR_XIR_UNIT};inputs[at*2+1]=edge->value;
    }
    SourceValue error,none;
    if (!source_recipe_group(ctx,(XrXirInstruction){XR_XIR_PHI,XR_XIR_ERROR,{0},{0},0,{0}},
        inputs,handler.count*2,&error)) return false;
    if (body->handler_count==UINT32_MAX || plan->syntax->node_id==UINT32_MAX)
        return source_fail(ctx,plan->syntax,XR_XIR_BUDGET,"optional handler identity exhausted");
    route->selectors=source_alloc(ctx,1,sizeof(*route->selectors));
    if (!route->selectors) return false;
    route->identity=++body->handler_count;route->error=error.id;
    route->next=body->handler_recipes;body->handler_recipes=route;
    uint32_t accepted=body->count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_NULLABLE_NONE,result,{0},{0},0,{0}},&none)) return false;
    XrXirSourceDebugPoint *point=&body->recipes[accepted].debug;
    point->statement=plan->syntax->node_id+1;point->range=source_query_range(ctx,plan->syntax,NULL);
    point->flags|=XR_XIR_SOURCE_DEBUG_HANDLER_ACCEPT;
    route->selectors[0]=(XrXirSourceDebugSelector){accepted,accepted,UINT32_MAX};route->selector_count=1;
    uint32_t failure=body->current_block.identity,end=body->count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0,{0}},NULL)) return false;
    uint32_t join=body->block_count;
    if (!begin_block(ctx)) return false;
    body->recipes[jump].instruction.targets[0]=join;body->recipes[end].instruction.targets[0]=join;
    SourceValue values[]={{success,XR_XIR_UNIT},*value,{failure,XR_XIR_UNIT},none};
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_PHI,result,{0},{0},0,{0}},values,4,value);
}
