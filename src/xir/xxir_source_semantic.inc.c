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


static void source_semantic_free(SourceSemanticEnvironment *environment) {
    if (!environment) return;
    for (uint32_t i=0;i<environment->symbol_count;++i) {
        xr_compile_resources_free((void *)environment->symbols[i].name);
        xr_compile_resources_free((void *)environment->symbols[i].imported);
    }
    xr_compile_module_graph_free(environment->graph);
    xr_compile_resources_free(environment->latest);
    xr_compile_resources_free(environment->symbols);
    xr_compile_resources_free(environment->names);
    xr_compile_resources_free(environment->binding_reads);
    xr_compile_resources_free(environment->history.modules);
    xr_compile_resources_free(environment->history.functions);
    xr_compile_resources_free(environment->history.slots);
    xr_compile_resources_free(environment->history.types);
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
static bool source_semantic_history(SourceContext *ctx,SourceSemanticEnvironment *environment) {
    if (!ctx->history) return true;
    const SourceLibraryMap *map=ctx->history->map;
    if (!map) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"history receiving correspondence is missing");
    SourceHistoryCorrespondence *owned=&environment->history;
    owned->unit=ctx->history->unit;
    owned->module_count=map->module_count;owned->function_count=map->function_count;
    owned->slot_count=map->slot_count;owned->type_count=map->unit ? map->unit->type_count : 0;
    const void *inputs[]={map->modules,map->functions,map->slots,map->unit ? map->unit->types : NULL};
    const uint32_t counts[]={owned->module_count,owned->function_count,owned->slot_count,owned->type_count};
    _Static_assert(sizeof(XrXirType)==sizeof(uint32_t),"history type ordinals use exact wire integers");
    for (unsigned i=0;i<4;++i) {
        if (!source_work(ctx,NULL)) return false;
        if (!counts[i]) continue;
        if (!inputs[i]) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"history receiving array is absent");
        void *copy=source_semantic_alloc(ctx,counts[i],sizeof(uint32_t));
        if (!copy) return false;
        switch (i) {
        case 0:owned->modules=copy;break;
        case 1:owned->functions=copy;break;
        case 2:owned->slots=copy;break;
        default:owned->types=copy;break;
        }
        if (!source_copy_bytes(ctx,NULL,copy,inputs[i],(size_t)counts[i]*sizeof(uint32_t))) return false;
    }
    return true;
}
static SourceName *source_semantic_binding(SourceContext *ctx,const SourceSemanticEnvironment *environment,
    const SourceName *source) {
    uint32_t index=0;
    for (uint32_t m=0;m<environment->module_count;++m) {
        for (const SourceName *symbol=ctx->names[m];symbol;symbol=symbol->next) {
            if (!source_work(ctx,NULL)) return NULL;
            if (symbol==source) return &environment->symbols[index];
            ++index;
        }
    }
    source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Source latest binding has no semantic registration");return NULL;
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
    if (count) {
        environment->latest=source_semantic_alloc(ctx,count,sizeof(*environment->latest));
        if (!environment->latest) goto failed;
    }
    uint32_t entry=(uint32_t)ctx->graph->entry_index;
    for (SourceName *symbol=environment->names[entry];symbol;symbol=symbol->next) {
        if (!source_work(ctx,NULL)) goto failed;
        environment->latest[environment->latest_count++]=symbol;
    }
    for (uint32_t i=0;ctx->history && i<ctx->history->latest_count;++i) {
        const SourceName *symbol=ctx->history->latest[i];
        if (!source_work(ctx,NULL)) goto failed;
        SourceName *shadow=find_name(ctx,ctx->names[entry],symbol->name);
        if (ctx->diagnostic.status!=XR_XIR_OK) goto failed;
        if (shadow) continue;
        SourceName *owned=source_semantic_binding(ctx,environment,symbol);
        if (!owned) goto failed;
        if (environment->latest_count>=count) {
            source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Source latest binding inventory is inconsistent");goto failed;
        }
        environment->latest[environment->latest_count++]=owned;
    }
    if (!source_semantic_history(ctx,environment)) goto failed;
    /* The input owner transfers the complete syntax graph only after this
     * final semantic construction and query publication have succeeded. */
    *output=environment;return true;
failed:
    source_semantic_free(environment);return false;
}
