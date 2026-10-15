/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_semantic.inc.c - Owned declaration bindings from the Source checker
 *
 * KEY CONCEPT:
 *   Binding ordinals come from actual semantic registration. Their AST closure
 *   and module identities survive the temporary Source construction context.
 */
typedef struct SourceSemanticEnvironment {
    XrModuleGraph *graph;
    SourceName **names, *symbols;
    uint32_t module_count, symbol_count;
    uint32_t *binding_reads;
    uint32_t slot_count;
} SourceSemanticEnvironment;

static void source_semantic_free(SourceSemanticEnvironment *environment) {
    if (!environment) return;
    for (uint32_t i=0;i<environment->symbol_count;++i) {
        xr_compile_resources_free((void *)environment->symbols[i].name);
        xr_compile_resources_free((void *)environment->symbols[i].imported);
    }
    xr_compile_module_graph_free(environment->graph);
    xr_compile_resources_free(environment->symbols);
    xr_compile_resources_free(environment->names);
    xr_compile_resources_free(environment->binding_reads);
    xr_compile_resources_free(environment);
}
static void *source_semantic_alloc(SourceContext *ctx,size_t count,size_t size) {
    void *output=NULL;
    XrXirStatus status=xir_compile_resource_status(xr_compile_resources_calloc(ctx->compile.resources,count,size,&output));
    if (status!=XR_XIR_OK) source_fail(ctx,NULL,status,"Source semantic environment allocation failed");
    return output;
}
static const char *source_semantic_text(SourceContext *ctx,const char *text) {
    if (!text) return NULL;
    size_t length;
    if (!source_text_length(ctx,NULL,text,&length) || length==SIZE_MAX) return NULL;
    char *owned=source_semantic_alloc(ctx,length+1,1);
    if (!owned) return NULL;
    if (!source_copy_bytes(ctx,NULL,owned,text,length+1)) {xr_compile_resources_free(owned);return NULL;}
    return owned;
}
static bool source_semantic_capture(SourceContext *ctx,SourceSemanticEnvironment **output) {
    if (!output || *output)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Source semantic output is already occupied");
    if (!ctx->graph || ctx->graph->spec_count<=0 || ctx->module_count<(uint32_t)ctx->graph->spec_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Source semantic closure is incomplete");
    SourceSemanticEnvironment *environment=source_semantic_alloc(ctx,1,sizeof(*environment));
    if (!environment) return false;
    /* Compiler-owned intrinsic modules have no Source name table. Their typed
     * bodies remain in Checked; only actual graph scopes own these bindings. */
    uint32_t modules=(uint32_t)ctx->graph->spec_count,count=0;
    for (uint32_t m=0;m<modules;++m) {
        for (const SourceName *symbol=ctx->names[m];symbol;symbol=symbol->next) {
            if (!source_work(ctx,NULL)) goto failed;
            if (count==UINT32_MAX) {source_fail(ctx,NULL,XR_XIR_BUDGET,"Source semantic binding inventory exhausted");goto failed;}
            ++count;
        }
    }
    environment->module_count=modules;
    if(ctx->binding_reads && ctx->slot_count) {
        environment->slot_count=ctx->slot_count;
        environment->binding_reads=source_semantic_alloc(ctx,ctx->slot_count,sizeof(*environment->binding_reads));
        if(!environment->binding_reads || !source_copy_bytes(ctx,NULL,environment->binding_reads,ctx->binding_reads,
            (size_t)ctx->slot_count*sizeof(*ctx->binding_reads)))goto failed;
    }
    environment->names=source_semantic_alloc(ctx,modules,sizeof(*environment->names));
    environment->symbols=count ? source_semantic_alloc(ctx,count,sizeof(*environment->symbols)) : NULL;
    if (!environment->names || (count && !environment->symbols)) goto failed;
    for (uint32_t m=0;m<modules;++m) {
        SourceName **tail=&environment->names[m];
        for (const SourceName *symbol=ctx->names[m];symbol;symbol=symbol->next) {
            if (!source_work(ctx,NULL)) goto failed;
            SourceName *owned=&environment->symbols[environment->symbol_count++];
            SourceName copy=*symbol;
            copy.next=NULL;copy.name=NULL;copy.imported=NULL;copy.node=NULL;
            if (!source_copy_bytes(ctx,NULL,owned,&copy,sizeof(*owned))) goto failed;
            owned->name=source_semantic_text(ctx,symbol->name);
            if (!owned->name) {
                if (ctx->diagnostic.status==XR_XIR_OK) source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Source semantic name is missing");
                goto failed;
            }
            owned->imported=source_semantic_text(ctx,symbol->imported);
            if (symbol->imported && !owned->imported) goto failed;
            if (symbol->node) {
                bool exact=false;
                XrModuleStatus status=xr_compile_module_graph_owns_top_level_decl(ctx->graph,
                    &ctx->graph->specs[m],symbol->node,&exact);
                if (status!=XR_MODULE_OK) {source_module_fail(ctx,NULL,status,"Source declaration ownership check failed");goto failed;}
                if (exact) owned->node=symbol->node;
            }
            *tail=owned;tail=&owned->next;
        }
    }
    /* The input owner transfers the complete syntax graph only after this
     * final semantic construction and query publication have succeeded. */
    *output=environment;return true;
failed:
    source_semantic_free(environment);return false;
}
