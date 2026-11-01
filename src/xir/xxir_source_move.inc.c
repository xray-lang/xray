/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_move.inc.c - Consumption ordinals from actual parameter declarations
 *
 * KEY CONCEPT:
 *   Hidden captures shift physical ordinals but never acquire consumption modes.
 *   The complete common receiver independently verifies every declaration.
 */
static bool source_move_parameters(SourceContext *ctx,uint32_t index,
    XrParamNode *const *parameters,uint32_t count,uint32_t offset) {
    if(index>=ctx->function_count || (count&&!parameters) ||
        offset>ctx->functions[index].parameter_count || count!=ctx->functions[index].parameter_count-offset)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"consuming parameters have no exact declaration span");
    uint32_t consumes=0;
    for(uint32_t p=0;p<count;++p) {
        if(!(p&63u)&&!source_request_poll(ctx,NULL))return false;
        if(!parameters[p] || !xr_param_mode_is_valid(parameters[p]->passing_mode))
            return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"parameter passing mode is invalid");
        consumes+=parameters[p]->passing_mode==XR_PARAM_MOVE;
    }
    uint32_t *ordinals=consumes?source_alloc(ctx,consumes,sizeof(*ordinals)):NULL;
    if(consumes&&!ordinals)return false;
    uint32_t at=0;
    for(uint32_t p=0;p<count;++p) {
        if(!(p&63u)&&!source_request_poll(ctx,NULL))return false;
        if(parameters[p]->passing_mode==XR_PARAM_MOVE)ordinals[at++]=offset+p;
    }
    ctx->functions[index].consume_parameters=ordinals;
    ctx->functions[index].consume_parameter_count=consumes;return true;
}
static bool source_move(SourceContext *ctx,AstNode *node,SourceValue *output) {
    AstNode *position=node->as.move_expr.expr;
    if(!position||position->type!=AST_VARIABLE)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"move requires a whole local identifier");
    SourceName *binding=visible_name(ctx,position->as.variable.name);
    if(!source_local_name(binding)||binding->binding_function!=ctx->function)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"move requires the current function's owned local");
    uint32_t flags=0;XrXirStatus status=xr_xir_compile_view_type_flags(&ctx->compile,&ctx->types,binding->type,&flags);
    if(status!=XR_XIR_OK||flags||xr_xir_type_is_cell(&ctx->types,binding->type))
        return source_fail(ctx,node,status!=XR_XIR_OK?status:XR_XIR_BAD_TYPE,"move cannot consume a borrowed value");
    if(binding->type==XR_XIR_UNIT)
        return source_fail(ctx,node,XR_XIR_UNSUPPORTED,"Unit move has no executable local value yet");
    XrHirBindingAction action;
    if(!source_hir_binding_use(ctx,node,binding,XR_HIR_BINDING_TAKE,(SourceValue){0},&action)||
        !source_query_reference(ctx,position,binding,binding,XR_XIR_SOURCE_READ_WRITE))return false;
    uint32_t before=ctx->bodies[ctx->function].count;
    return source_recipe_record(ctx,(XrXirInstruction){action.operation,action.logical,
        {action.location},{0},0,{0}},output)&&source_hir_binding_emitted(ctx,&action,output,before);
}
