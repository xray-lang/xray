/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_binding_readers.inc.c - Ordinary checked root binding inspection
 */
static bool source_binding_reader_body(SourceContext *ctx,const SourceName *symbol) {
    SourceValue value={UINT32_MAX,XR_XIR_UNIT};
    if(symbol->kind!=SOURCE_UNIT_SLOT || symbol->mutable) {
        XrXirType physical=ctx->slots[symbol->index].type;
        if(!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SLOT_LOAD,physical,{0},{0},symbol->index,{0}},&value))return false;
        if(symbol->mutable && !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,symbol->type,
            {value.id,0},{0},0,{0}},&value))return false;
    }
    if(!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,
        {value.type==XR_XIR_UNIT ? 0 : value.id,0},{0},0,{0}},NULL))return false;
    ctx->returned=true;return finish_body(ctx);
}
static bool source_binding_accessor(SourceContext *ctx,const SourceName *symbol,SourceBindingAccessor mode,uint32_t *output) {
    bool write=mode==SOURCE_ACCESS_WRITE,push=mode==SOURCE_ACCESS_PUSH;
    if(push && (!symbol->mutable || !xr_xir_type_is_array(&ctx->types,symbol->type)))
        return source_fail(ctx,symbol->node,XR_XIR_BAD_STRUCTURE,"push accessor requires its actual mutable Array binding");
    if(ctx->function_count>=ctx->function_capacity || ctx->function_count>=ctx->compile.limits.functions)
        return source_fail(ctx,symbol->node,XR_XIR_BUDGET,"binding accessor function budget exhausted");
    char *name=source_alloc(ctx,32,1);if(!name)return false;
    if(source_format(ctx,name,32,write ? "$write%u" : push ? "$push%u" : "$binding%u",symbol->index)!=XR_DIAG_OK)
        return source_fail(ctx,symbol->node,XR_XIR_BUDGET,"binding accessor name exhausted");
    uint32_t function=ctx->function_count++,root=(uint32_t)ctx->graph->entry_index;
    XrXirType type=write || push ? XR_XIR_UNIT : symbol->type;
    XrXirType input=push ? xr_xir_array_element(&ctx->types,symbol->type) : symbol->type;
    XrXirType *parameter=NULL;uint32_t parameters=(write || push) && input!=XR_XIR_UNIT ? 1u : 0u;
    if(parameters){parameter=source_alloc(ctx,1,sizeof(*parameter));if(!parameter)return false;*parameter=input;}
    ctx->function=function;ctx->module=root;ctx->locals=ctx->scope=NULL;
    ctx->loop=NULL;ctx->facts=NULL;ctx->epochs=NULL;ctx->flow_alternatives=0;ctx->returned=false;
    ctx->functions[function]=(XrXirFunction){name,(uint32_t)source_text_size(ctx,name),parameter,parameters,type,NULL,0,NULL,0,NULL,0, XR_XIR_NEVER};
    ctx->bodies[function].module=root;ctx->bodies[function].infer_result=true;
    ctx->identities[function]=(XrXirFunctionIdentity){.module=root,.exported=1,.promises=XR_XIR_FUNCTION_NO_SUSPEND};
    if(push) {
        SourceValue cell={0},place={0};XrXirType physical=ctx->slots[symbol->index].type;
        if(!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SLOT_LOAD,physical,{0},{0},symbol->index,{0}},&cell) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_PLACE,symbol->type,{cell.id},{0},0,{0}},&place) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_PUSH,XR_XIR_UNIT,{place.id,0},{0},0,{0}},NULL) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}},NULL))return false;
        ctx->returned=true;if(!finish_body(ctx))return false;
    } else if(write) {
        SourceValue cell={0};XrXirType physical=ctx->slots[symbol->index].type;
        if(!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SLOT_LOAD,physical,{0},{0},symbol->index,{0}},&cell) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_WRITE,XR_XIR_UNIT,{cell.id,0},{0},0,{0}},NULL) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}},NULL))return false;
        ctx->returned=true;if(!finish_body(ctx))return false;
    } else if(!source_binding_reader_body(ctx,symbol))return false;
    *output=function+1;return true;
}
static bool source_binding_readers(SourceContext *ctx) {
    if(!ctx->binding_readers)return true;
    if(ctx->linkage_kind!=XR_XIR_PROGRAM || ctx->graph->entry_index<0)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"binding inspection requires an actual Program root");
    if(ctx->slot_count) {
        ctx->binding_reads=source_alloc(ctx,ctx->slot_count,sizeof(*ctx->binding_reads));
        ctx->binding_writes=source_alloc(ctx,ctx->slot_count,sizeof(*ctx->binding_writes));
        ctx->binding_pushes=source_alloc(ctx,ctx->slot_count,sizeof(*ctx->binding_pushes));
        if(!ctx->binding_reads || !ctx->binding_writes || !ctx->binding_pushes)return false;
    }
    if(!source_history_binding_accessors(ctx))return false;
    uint32_t root=(uint32_t)ctx->graph->entry_index;
    for(SourceName *symbol=ctx->names[root];symbol;symbol=symbol->next) {
        if(!((ctx)->diagnostic.status == XR_XIR_OK))return false;
        if(symbol->kind!=SOURCE_SLOT && symbol->kind!=SOURCE_UNIT_SLOT)continue;
        if(symbol->index>=ctx->slot_count || ctx->slots[symbol->index].module!=root || ctx->binding_reads[symbol->index])
            return source_fail(ctx,symbol->node,XR_XIR_BAD_STRUCTURE,"binding reader has no unique actual root slot");
        if(!source_binding_accessor(ctx,symbol,SOURCE_ACCESS_READ,&ctx->binding_reads[symbol->index]))return false;
        if(symbol->mutable && !source_binding_accessor(ctx,symbol,SOURCE_ACCESS_WRITE,&ctx->binding_writes[symbol->index]))return false;
        if(symbol->mutable && xr_xir_type_is_array(&ctx->types,symbol->type) && !source_binding_accessor(ctx,symbol,SOURCE_ACCESS_PUSH,&ctx->binding_pushes[symbol->index]))return false;
    }
    for(SourceFunctionValue *pair=ctx->function_values;pair;pair=pair->next) {
        if(!((ctx)->diagnostic.status == XR_XIR_OK))return false;
        if(pair->value->index>=ctx->slot_count || !ctx->binding_reads[pair->value->index])
            return source_fail(ctx,pair->function->node,XR_XIR_BAD_STRUCTURE,"function value has no actual slot reader");
        pair->function->reader=ctx->binding_reads[pair->value->index];
    }
    return true;
}
