/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_binding_cell.inc.c - Stable mutable binding storage
 *
 * KEY CONCEPT:
 *   Source names expose logical values while mutable bindings own one Cell.
 */
/* The declaration owner chooses the route and both logical/physical types.
 * This bridge supplies only the actual stored location and operand values. */
static bool source_binding_storage(SourceContext *ctx,const SourceName *symbol,
    const XrHirBindingAction *action,SourceValue *cell) {
    if(symbol->index!=action->location)return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"typed binding storage differs from its declaration instance");
    if(action->route==XR_HIR_BINDING_LOCAL_CELL) {
        *cell=(SourceValue){.id=action->location,.type=action->physical,.place={0}};return true;
    }
    if(action->route!=XR_HIR_BINDING_SLOT_CELL)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"typed binding action has no Cell route");
    if(action->location>=ctx->slot_count||ctx->slots[action->location].type!=action->physical)
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"module Cell element differs from its logical binding");
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SLOT_LOAD,action->physical,
        {0},{0},action->location,{0}},cell);
}
static bool source_binding_cell(SourceContext *ctx,const SourceName *symbol,SourceValue *cell) {
    XrHirBindingAction action;
    if(!source_hir_binding_use(ctx,NULL,symbol,XR_HIR_BINDING_CELL,(SourceValue){0},&action))return false;
    uint32_t before=ctx->bodies[ctx->function].count;
    return source_binding_storage(ctx,symbol,&action,cell)&&source_hir_binding_emitted(ctx,&action,cell,before);
}
static bool source_binding_read(SourceContext *ctx,const SourceName *symbol,SourceValue *value) {
    if(ctx->bodies[ctx->function].infer_initializer&&symbol->kind==SOURCE_SLOT&&symbol->type==XR_XIR_UNIT)
        return source_fail(ctx,symbol->node,XR_XIR_BAD_TYPE,"initializer depends on a binding whose type is not ready");
    XrHirBindingAction action;
    AstNode *syntax=ctx->active_expression?ctx->active_expression->syntax:symbol->node;
    if(!source_hir_binding_use(ctx,syntax,symbol,XR_HIR_BINDING_READ,(SourceValue){0},&action))return false;
    uint32_t before=ctx->bodies[ctx->function].count;bool ok;
    if(action.route==XR_HIR_BINDING_UNIT){*value=(SourceValue){.id=UINT32_MAX,.type=action.logical,.place={0}};ok=true;}
    else if(action.history)
        ok=source_recipe_record(ctx,(XrXirInstruction){action.operation,action.logical,{0},{0},action.accessor,{0}},value);
    else if(action.route==XR_HIR_BINDING_LOCAL_CELL||action.route==XR_HIR_BINDING_SLOT_CELL) {
        SourceValue cell={0};ok=source_binding_storage(ctx,symbol,&action,&cell)&&
            source_recipe_record(ctx,(XrXirInstruction){action.operation,action.logical,{cell.id,0},{0},0,{0}},value);
    } else if(action.route==XR_HIR_BINDING_DIRECT){*value=(SourceValue){.id=action.location,.type=action.logical,.place={0}};ok=true;}
    else ok=source_recipe_record(ctx,(XrXirInstruction){action.operation,action.logical,{0},{0},action.location,{0}},value);
    return ok&&source_hir_binding_emitted(ctx,&action,value,before);
}
static bool source_binding_write_checked(SourceContext *ctx,const SourceName *symbol,
    SourceValue value,const XrHirBindingAction *action) {
    uint32_t before=ctx->bodies[ctx->function].count;bool ok;
    if(action->route==XR_HIR_BINDING_UNIT)ok=true;
    else if(action->history)
        ok=source_recipe_group(ctx,(XrXirInstruction){action->operation,XR_XIR_UNIT,{0},{0},action->accessor,{0}},
            &value,value.type==XR_XIR_UNIT?0u:1u,NULL);
    else {
        SourceValue cell={0};ok=source_binding_storage(ctx,symbol,action,&cell)&&
            source_recipe_record(ctx,(XrXirInstruction){action->operation,XR_XIR_UNIT,
                {cell.id,action->logical==XR_XIR_UNIT?0:value.id},{0},0,{0}},NULL);
    }
    if(!ok||!source_hir_binding_emitted(ctx,action,NULL,before))return false;
    if(action->history)ctx->bodies[ctx->function].typed_projections->recipe=ctx->functions[ctx->function].parameter_count+before;
    return true;
}
static bool source_binding_write(SourceContext *ctx,const SourceName *symbol,SourceValue value) {
    XrHirBindingAction action;
    return source_hir_binding_use(ctx,NULL,symbol,XR_HIR_BINDING_WRITE,value,&action)&&
        source_binding_write_checked(ctx,symbol,value,&action);
}
static bool source_binding_write_reason(SourceContext *ctx,AstNode *node,const SourceName *symbol,SourceValue value,const char *failure) {
    XrHirBodyBinding binding={0};if(!source_hir_binding_receive(ctx,symbol,&binding))return false;
    XrHirBindingUseInput input={.types=&ctx->types,.scope=ctx->bodies[ctx->function].typed_scope,.origin=source_hir_origin(ctx,node,symbol->declaration),
        .kind=XR_HIR_BINDING_WRITE,.value=value.type,.value_id=value.id,.type_failure=failure};
    XrHirBindingAction action;const char *reason=NULL;
    XrXirStatus status=xr_hir_body_binding_use(ctx->typed_body,binding,&input,ctx->request,&action,&reason);
    if(status!=XR_XIR_OK)return source_fail(ctx,node,status,reason?reason:"typed assignment failed");
    return source_binding_write_checked(ctx,symbol,value,&action);
}
static bool source_binding_assign(SourceContext *ctx,AstNode *node,const SourceName *symbol,SourceValue value) {
    return source_binding_write_reason(ctx,node,symbol,value,"assignment type mismatch");
}
static bool source_binding_prepare(SourceContext *ctx,SourceName *symbol,SourceValue *initial) {
    symbol->type=initial->type;XrHirBodyBinding binding={0};if(!source_hir_binding_register(ctx,symbol,&binding))return false;
    XrHirBindingAction action;
    if(!source_hir_binding_use(ctx,symbol->node,symbol,XR_HIR_BINDING_INITIALIZE,*initial,&action))return false;
    uint32_t before=ctx->bodies[ctx->function].count;
    if(action.operation!=XR_XIR_INVALID&&
        !source_recipe_record(ctx,(XrXirInstruction){action.operation,action.physical,
            {action.logical==XR_XIR_UNIT?0:initial->id,0},{0},0,{0}},initial))return false;
    ctx->slots[symbol->index].type=initial->type;
    if(!symbol->hidden && symbol->node && symbol->node->is_exported &&
        !ctx->slot_fact_present[symbol->index]) {
        size_t length=source_text_size(ctx,symbol->name);
        if(ctx->diagnostic.status!=XR_XIR_OK || length>UINT32_MAX ||
            !source_slot_fact(ctx,symbol->index,symbol->name,(uint32_t)length,1))return false;
    }
    if(action.logical==XR_XIR_UNIT)symbol->kind=SOURCE_UNIT_SLOT;
    source_query_binding_type(ctx,symbol);
    return source_hir_binding_emitted(ctx,&action,initial,before);
}
