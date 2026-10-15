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
static bool source_binding_readers(SourceContext *ctx) {
    if(!ctx->binding_readers)return true;
    if(ctx->linkage_kind!=XR_XIR_PROGRAM || ctx->graph->entry_index<0)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"binding inspection requires an actual Program root");
    if(ctx->slot_count) {
        ctx->binding_reads=source_alloc(ctx,ctx->slot_count,sizeof(*ctx->binding_reads));
        if(!ctx->binding_reads)return false;
    }
    uint32_t root=(uint32_t)ctx->graph->entry_index;
    for(const SourceName *symbol=ctx->names[root];symbol;symbol=symbol->next) {
        if(!source_work(ctx,symbol->node))return false;
        if(symbol->kind!=SOURCE_SLOT && symbol->kind!=SOURCE_UNIT_SLOT)continue;
        if(symbol->index>=ctx->slot_count || ctx->slots[symbol->index].module!=root || ctx->binding_reads[symbol->index])
            return source_fail(ctx,symbol->node,XR_XIR_BAD_STRUCTURE,"binding reader has no unique actual root slot");
        if(ctx->function_count>=ctx->function_capacity || ctx->function_count>=ctx->compile.limits.functions)
            return source_fail(ctx,symbol->node,XR_XIR_BUDGET,"binding reader function budget exhausted");
        char *name=source_alloc(ctx,32,1);if(!name)return false;
        if(source_format(ctx,name,32,"$binding%u",symbol->index)!=XR_DIAG_OK)
            return source_fail(ctx,symbol->node,XR_XIR_BUDGET,"binding reader name exhausted");
        uint32_t function=ctx->function_count++;
        ctx->function=function;ctx->module=root;ctx->locals=ctx->scope=NULL;
        ctx->loop=NULL;ctx->facts=NULL;ctx->epochs=NULL;ctx->flow_alternatives=0;ctx->returned=false;
        ctx->functions[function]=(XrXirFunction){name,(uint32_t)source_text_size(ctx,name),NULL,0,symbol->type,NULL,0,NULL,0,NULL,0};
        ctx->bodies[function].module=root;
        ctx->bodies[function].infer_result=true;
        ctx->identities[function]=(XrXirFunctionIdentity){.module=root,.exported=1,.promises=XR_XIR_FUNCTION_NO_SUSPEND};
        if(!source_binding_reader_body(ctx,symbol))return false;
        ctx->binding_reads[symbol->index]=function+1;
    }
    return true;
}
