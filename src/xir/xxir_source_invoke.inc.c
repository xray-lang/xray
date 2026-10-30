/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_invoke.inc.c - Function-local explicit error continuations
 */
static bool source_handler_site(SourceContext *ctx,uint32_t instruction,uint32_t error) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    if (!body->error_context || body->error_context->asserted) return true;
    if (!body->error_context->route || instruction>=body->count || body->route_count==UINT32_MAX)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source handler site has no route");
    SourceRouteRecipe *site=source_alloc(ctx,1,sizeof(*site));if (!site) return false;
    *site=(SourceRouteRecipe){body->route_recipes,body->error_context->route,instruction,body->count-1,error,body->error_context->edges};
    body->route_recipes=site;++body->route_count;return true;
}
static bool source_error_edge(SourceContext *ctx, SourceValue value) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!body->block_count && !begin_block(ctx)) return false;
    SourceErrorContext *handler = body->error_context;
    if (handler && handler->asserted)
        return source_recipe_append(ctx,(XrXirInstruction){XR_XIR_PANIC_ERROR,XR_XIR_UNIT,{value.id},{0},0,{0}},NULL);
    SourceErrorEdge *edge = source_alloc(ctx,1,sizeof(*edge));
    if (!edge || !handler || handler->count >= UINT32_MAX/2)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"error continuation budget exhausted");
    *edge = (SourceErrorEdge){handler->edges,body->current_block.identity,body->count,value,UINT32_MAX};
    handler->edges = edge; ++handler->count;
    bool leaving = body->frontier != handler->frontier;
    return source_recipe_append(ctx,(XrXirInstruction){leaving ? XR_XIR_CLEANUP_ERROR : XR_XIR_JUMP,
        XR_XIR_UNIT,{leaving ? value.id : 0},{0},leaving ? handler->frontier : 0,{0}},NULL);
}
static bool source_recipe_record(SourceContext *ctx, XrXirInstruction op, SourceValue *result) {
    if (!source_local_call(ctx,&op) || !source_call_borrows(ctx,&op)) return false;
    SourceFunction *body = &ctx->bodies[ctx->function];
    if(op.op==XR_XIR_RETURN) {
        XrHirBodyOrigin origin=source_hir_origin(ctx,NULL,0);
        if(ctx->debug_function==ctx->function) {
            XrXirSourceRange range=ctx->debug_point.range;origin.node=ctx->debug_point.statement?ctx->debug_point.statement-1:0;
            origin.line=range.line;origin.column=range.column;origin.end_line=range.end_line;origin.end_column=range.end_column;
        }
        XrHirBodyUse use={0};XrHirBodyAction action;
        XrXirStatus status=xr_hir_body_return(ctx->typed_body,origin,op.args[0],ctx->request,&use,&action);
        if(status!=XR_XIR_OK)return source_fail(ctx,NULL,status,"return emission has no actual declaration owner");
        op.op=action.operation;op.type=action.target;if(action.source==XR_XIR_UNIT)op.args[0]=0;
        uint32_t before=body->count;
        return source_recipe_append(ctx,op,result)&&
            source_hir_binding_emitted(ctx,&(XrHirBindingAction){.use=use},NULL,before);
    }
    if (!body->error_context || (op.op != XR_XIR_CALL && op.op != XR_XIR_CALL_INDIRECT &&
        op.op != XR_XIR_CALL_DEFAULT))
        return source_recipe_append(ctx,op,result);
    if (!body->block_count && !begin_block(ctx)) return false;
    uint32_t origin = body->count, error = body->block_count;
    if (error == UINT32_MAX) return source_fail(ctx,NULL,XR_XIR_BUDGET,"invoke block identity exhausted");
    if (op.op == XR_XIR_CALL_DEFAULT) {
        op.args[0] = op.targets[0]; op.args[1] = op.targets[1];
        op.op = XR_XIR_INVOKE_DEFAULT;
    } else op.op = op.op == XR_XIR_CALL ? XR_XIR_INVOKE : XR_XIR_INVOKE_INDIRECT;
    op.targets[0] = error+1; op.targets[1] = error;
    if (!source_recipe_append(ctx,op,NULL) || !begin_block(ctx)) return false;
    SourceValue caught;
    if (!source_recipe_append(ctx,(XrXirInstruction){XR_XIR_INVOKE_ERROR,XR_XIR_ERROR,{0},{0},origin,{0}},&caught) ||
        !source_error_edge(ctx,caught) || !source_handler_site(ctx,origin,caught.id) || !begin_block(ctx)) return false;
    if (op.type == XR_XIR_UNIT) {
        if (result) *result = (SourceValue){.id=0,.type=XR_XIR_UNIT,.place={0}};
        return true;
    }
    return source_recipe_append(ctx,(XrXirInstruction){XR_XIR_INVOKE_RESULT,op.type,{0},{0},origin,{0}},result);
}
