/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_view_normalize.inc.c - Transactional Source lifetime relocation
 */
#include "xxir_view_normalize_internal.h"
#include "xxir_source_view_construction.inc.c"
typedef struct SourceViewRegionMap {
    const XrXirInitializationRegion *source;
    XrXirInitializationRegion *destination;
    const XrXirFunction *old_function;
    XrXirFunction *new_function;
    const uint32_t *values,*boundaries;
    bool ready;
} SourceViewRegionMap;
static void *source_view_copy(SourceContext *ctx,const void *input,size_t count,size_t element) {
    if (!count) return NULL;
    void *output=source_alloc(ctx,count,element);
    if (!output || !source_work_units(ctx,NULL,(uint64_t)count*element)) return NULL;
    memcpy(output,input,count*element);return output;
}
static bool source_view_value(SourceContext *ctx,const XirViewNormalizedFunction *map,
    uint32_t parameters,uint32_t *value) {
    if (*value==UINT32_MAX) return true;
    if (*value>=(uint64_t)parameters+map->old_instructions)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Source lifetime value identity is invalid");
    *value=map->values[*value];return true;
}
static bool source_view_instruction(SourceContext *ctx,const XirViewNormalizedFunction *map,
    uint32_t parameters,uint32_t *instruction) {
    if (*instruction==UINT32_MAX) return true;
    if (*instruction>=map->old_instructions)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Source lifetime instruction identity is invalid");
    uint32_t value=parameters+*instruction;
    if (!source_view_value(ctx,map,parameters,&value)) return false;
    *instruction=value-parameters;return true;
}
static bool source_view_debug_handlers(SourceContext *ctx,const XirViewNormalizedFunction *map,
    uint32_t parameters,SourceFunction *body) {
    XrXirSourceDebugHandler *handlers=source_view_copy(ctx,body->debug_handlers,body->handler_count,sizeof(*handlers));
    XrXirSourceDebugRoute *routes=source_view_copy(ctx,body->debug_routes,body->route_count,sizeof(*routes));
    if ((body->handler_count && !handlers) || (body->route_count && !routes)) return false;
    for (uint32_t h=0;h<body->handler_count;++h) {
        if (!source_work(ctx,NULL) || !source_view_value(ctx,map,parameters,&handlers[h].error) ||
            !source_view_instruction(ctx,map,parameters,&handlers[h].propagation)) return false;
        XrXirSourceDebugSelector *selectors=source_view_copy(ctx,handlers[h].selectors,
            handlers[h].selector_count,sizeof(*selectors));
        if (handlers[h].selector_count && !selectors) return false;
        for (uint32_t s=0;s<handlers[h].selector_count;++s) {
            if (!source_work(ctx,NULL) || !source_view_instruction(ctx,map,parameters,&selectors[s].first) ||
                !source_view_instruction(ctx,map,parameters,&selectors[s].accepted) ||
                !source_view_instruction(ctx,map,parameters,&selectors[s].rejected)) return false;
        }
        handlers[h].selectors=selectors;
    }
    for (uint32_t r=0;r<body->route_count;++r) {
        if (!source_work(ctx,NULL) || !source_view_instruction(ctx,map,parameters,&routes[r].instruction) ||
            !source_view_instruction(ctx,map,parameters,&routes[r].edge) ||
            !source_view_value(ctx,map,parameters,&routes[r].error)) return false;
    }
    body->debug_handlers=handlers;body->debug_routes=routes;return true;
}
static bool source_view_debug(SourceContext *ctx,uint32_t f,const XirViewNormalization *n,SourceFunction *body) {
    const XrXirFunction *old=ctx->functions+f,*function=n->bodies+f;
    const XirViewNormalizedFunction *map=n->functions+f;
    if (body->debug_points) {
        XrXirSourceDebugPoint *points=source_alloc(ctx,function->instruction_count,sizeof(*points));
        if (!points) return false;
        for (uint32_t i=0;i<old->instruction_count;++i) {
            if (!source_work_units(ctx,NULL,sizeof(*points))) return false;
            uint32_t destination=map->values[old->parameter_count+i]-old->parameter_count;
            points[destination]=body->debug_points[i];
        }
        body->debug_points=points;
    }
    XrXirSourceDebugBinding *bindings=source_view_copy(ctx,body->debug_bindings,
        body->debug_binding_count,sizeof(*bindings));
    XrXirSourceDebugThrow *throws=source_view_copy(ctx,body->debug_throws,body->throw_count,sizeof(*throws));
    if ((body->debug_binding_count && !bindings) || (body->throw_count && !throws)) return false;
    for (uint32_t b=0;b<body->debug_binding_count;++b)
        if (!source_work(ctx,NULL) || !source_view_value(ctx,map,old->parameter_count,&bindings[b].value)) return false;
    for (uint32_t t=0;t<body->throw_count;++t) {
        if (!source_work(ctx,NULL) || !source_view_instruction(ctx,map,old->parameter_count,&throws[t].instruction) ||
            !source_view_value(ctx,map,old->parameter_count,&throws[t].operand)) return false;
    }
    body->debug_bindings=bindings;body->debug_throws=throws;
    return source_view_debug_handlers(ctx,map,old->parameter_count,body);
}
static bool source_view_roots(SourceContext *ctx,uint32_t f,const XirViewNormalization *n,SourceFunction *body) {
    const XrXirFunction *old=ctx->functions+f,*function=n->bodies+f;
    const XirViewNormalizedFunction *map=n->functions+f;
    if (body->root_values && body->count!=old->instruction_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Source lifetime root inventory is stale");
    SourceRootValue *roots=source_alloc(ctx,function->instruction_count,sizeof(*roots));
    if (!roots) return false;
    for (uint32_t i=0;i<function->instruction_count;++i) {
        if (!source_work(ctx,NULL)) return false;
        roots[i]=(SourceRootValue){0,XR_XIR_UNIT,UINT32_MAX};
    }
    for (uint32_t i=0;i<old->instruction_count;++i) {
        if (!source_work(ctx,NULL)) return false;
        uint32_t at=map->values[old->parameter_count+i]-old->parameter_count;
        roots[at]=body->root_values?body->root_values[i]:(SourceRootValue){0,old->instructions[i].type,UINT32_MAX};
        if (!source_view_value(ctx,map,old->parameter_count,&roots[at].peer)) return false;
    }
    body->root_values=roots;
    if (body->constructor_places && !body->constructor_captured) {
        uint32_t nominal=ctx->identities[f].nominal_owner;
        if (!nominal || nominal>ctx->nominals.count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Source constructor lifetime owner is invalid");
        uint32_t count=ctx->nominals.declarations[nominal-1u].field_count;
        body->constructor_places=source_view_copy(ctx,body->constructor_places,count,sizeof(*body->constructor_places));
        if (count && !body->constructor_places) return false;
        for (uint32_t i=0;i<count;++i)
            if (!source_view_value(ctx,map,old->parameter_count,body->constructor_places+i)) return false;
    }
    return true;
}
static bool source_view_region_build(SourceContext *ctx,const XrXirModule *module,
    SourceViewRegionMap *row,const SourceViewRegionMap *parent) {
    const XrXirInitializationRegion *old=row->source;const XrXirFunction *source=&old->function;
    const XrXirFunction *p=parent->new_function;uint32_t first=old->first_instruction;
    if (first>source->instruction_count || first>parent->old_function->instruction_count ||
        old->first_block>p->block_count || source->parameter_count!=p->parameter_count ||
        old->checkpoint>parent->old_function->instruction_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Source detached lifetime prefix is invalid");
    uint32_t prefix=parent->boundaries[first];uint64_t total=(uint64_t)prefix+source->instruction_count-first;
    if (total>UINT32_MAX-source->parameter_count) return source_fail(ctx,NULL,XR_XIR_BUDGET,"Source detached lifetime output exhausted");
    uint32_t *values=source_alloc(ctx,(size_t)source->parameter_count+source->instruction_count,sizeof(*values));
    uint32_t *boundaries=source_alloc(ctx,(size_t)source->instruction_count+1u,sizeof(*boundaries));
    XrXirInstruction *instructions=source_alloc(ctx,(uint32_t)total,sizeof(*instructions));
    XrXirBlock *blocks=source_view_copy(ctx,source->blocks,source->block_count,sizeof(*blocks));
    if (!values || !boundaries || !instructions || !blocks) return false;
    for (uint32_t i=0;i<source->parameter_count;++i) values[i]=i;
    for (uint32_t i=0;i<source->instruction_count;++i) {
        if (!source_work(ctx,NULL)) return false;
        boundaries[i]=i<first?parent->boundaries[i]:prefix+i-first;
        values[source->parameter_count+i]=i<first?parent->values[source->parameter_count+i]:source->parameter_count+prefix+i-first;
    }
    boundaries[source->instruction_count]=(uint32_t)total;
    uint64_t operands=0;
    for (uint32_t i=0;i<(uint32_t)total;++i) {
        if (!source_work(ctx,NULL)) return false;
        instructions[i]=i<prefix?p->instructions[i]:source->instructions[first+i-prefix];
        if (i<prefix && instructions[i].op==XR_XIR_VIEW_CLEANUP) instructions[i].args[0]=instructions[i].args[1]=0;
        if (xr_xir_op_uses_operand_table(instructions[i].op)) operands+=instructions[i].args[1];
    }
    if (operands>UINT32_MAX) return source_fail(ctx,NULL,XR_XIR_BUDGET,"Source detached lifetime operands exhausted");
    uint32_t *data=operands?source_alloc(ctx,(size_t)operands,sizeof(*data)):NULL;if (operands && !data) return false;
    uint32_t at=0;
    for (uint32_t i=0;i<(uint32_t)total;++i) {
        if (!source_work(ctx,NULL)) return false;
        XrXirInstruction *op=instructions+i;if (!xr_xir_op_uses_operand_table(op->op)) continue;
        const XrXirFunction *origin=i<prefix?p:source;
        if (!source_work_units(ctx,NULL,op->args[1])) return false;
        if (op->args[1]) memcpy(data+at,origin->operands+op->args[0],(size_t)op->args[1]*sizeof(*data));
        op->args[0]=op->args[1]?at:0;at+=op->args[1];
    }
    for (uint32_t b=0;b<old->first_block;++b) blocks[b]=p->blocks[b];
    XrXirInitializationRegion *result=row->destination;result->function=*source;
    result->function.instructions=instructions;result->function.instruction_count=(uint32_t)total;
    result->function.blocks=blocks;result->function.operands=data;result->function.operand_count=(uint32_t)operands;
    result->first_instruction=prefix;result->checkpoint=parent->boundaries[old->checkpoint];
    XirViewRelocation relocation={module,source,&result->function,values,boundaries,first,old->first_block};
    XrXirStatus status=xir_compile_view_relocate(&ctx->compile,&relocation);
    if (status!=XR_XIR_OK) return source_fail(ctx,NULL,status,"Source detached lifetime relocation failed");
    row->old_function=source;row->new_function=&result->function;row->values=values;row->boundaries=boundaries;row->ready=true;
    return true;
}
static bool source_view_regions(SourceContext *ctx,uint32_t f,const XirViewNormalization *n,SourceFunction *body) {
    uint32_t count=0;const XrXirInitializationRegion *regions=body->initialization_regions;
    for (const XrXirInitializationRegion *r=regions;r;r=r->next) {
        if (!source_work(ctx,NULL)) return false;
        if (count==UINT32_MAX-1u) return source_fail(ctx,NULL,XR_XIR_BUDGET,"Source detached lifetime count exhausted");++count;
    }
    if (!count) return true;
    SourceViewRegionMap *maps=source_alloc(ctx,(size_t)count+1u,sizeof(*maps));
    if (!maps) return false;
    maps[0]=(SourceViewRegionMap){NULL,NULL,ctx->functions+f,n->bodies+f,n->functions[f].values,n->functions[f].boundaries,true};
    uint32_t i=1;
    for (const XrXirInitializationRegion *r=regions;r;r=r->next,++i) {
        maps[i].source=r;maps[i].destination=source_view_copy(ctx,r,1,sizeof(*r));if (!maps[i].destination) return false;
    }
    uint32_t pending=count;
    while (pending) {
        uint32_t before=pending;
        for (uint32_t r=1;r<=count;++r) {
            if (!source_work(ctx,NULL)) return false;if (maps[r].ready) continue;
            for (uint32_t p=0;p<=count;++p) {
                if (!source_work(ctx,NULL)) return false;
                if (!maps[p].ready || maps[p].source!=maps[r].source->parent) continue;
                if (!source_view_region_build(ctx,&n->module,maps+r,maps+p)) return false;
                maps[r].destination->parent=maps[p].destination;--pending;break;
            }
        }
        if (pending==before) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Source detached lifetime parents are cyclic or absent");
    }
    for (uint32_t r=1;r<=count;++r) maps[r].destination->next=r<count?maps[r+1u].destination:NULL;
    body->initialization_regions=maps[1].destination;body->initialization_parent=NULL;return true;
}
static SourceRootQuery *source_view_queries(SourceContext *ctx,const XirViewNormalization *n) {
    SourceRootQuery *head=NULL,**next=&head;
    for (SourceRootQuery *q=ctx->root_queries;q;q=q->next) {
        if (!source_work(ctx,NULL)) return NULL;
        SourceRootQuery *copy=source_view_copy(ctx,q,1,sizeof(*q));if (!copy) return NULL;
        if (q->function>=ctx->function_count) {source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Source lifetime query function is invalid");return NULL;}
        const XirViewNormalizedFunction *map=n->functions+q->function;
        if (map->instructions && !source_view_value(ctx,map,ctx->functions[q->function].parameter_count,&copy->value)) return NULL;
        copy->next=NULL;*next=copy;next=&copy->next;
    }
    return head;
}
static XrXirStatus source_view_access_failure(SourceContext *ctx,const XrXirModule *module,
    const XrXirDiagnostic *diagnostic,XrXirStatus status) {
    AstNode *site=NULL;XrXirOp op=XR_XIR_INVALID;uint32_t block=diagnostic->block;
    if (diagnostic->function<module->function_count) {
        const XrXirFunction *function=module->functions+diagnostic->function;
        site=ctx->bodies[diagnostic->function].node;ctx->module=ctx->bodies[diagnostic->function].module;
        if (diagnostic->instruction<function->instruction_count) op=function->instructions[diagnostic->instruction].op;
        if (block==UINT32_MAX && diagnostic->instruction<function->instruction_count) {
            for (uint32_t p=0;p<function->block_count;++p) {
                if (!source_work(ctx,NULL)) return ctx->diagnostic.status;
                const XrXirBlock *candidate=function->blocks+p;
                if (diagnostic->instruction>=candidate->first && diagnostic->instruction-candidate->first<candidate->count) {block=p;break;}
            }
        }
    }
    char message[192]={0};
    if (status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY && source_format(ctx,message,sizeof(message),
        "constructed XIR failed checking during view lifetime normalization at function %u block %u instruction %u (%s)",
        diagnostic->function,block,diagnostic->instruction,xr_xir_op_name(op))!=XR_DIAG_OK) status=XR_XIR_BUDGET;
    source_fail(ctx,site,status,message);return ctx->diagnostic.status;
}
static XrXirStatus source_view_checked_copy(void *owner,const XrXirModule *module,
    uint32_t function,bool *preserved) {
    SourceContext *ctx=owner;
    if (!ctx || !module || !preserved) return XR_XIR_BAD_STRUCTURE;
    *preserved=false;
    if (module->functions!=ctx->functions || module->function_count!=ctx->function_count ||
        function>=ctx->function_count) return XR_XIR_BAD_STRUCTURE;
    if (!source_work(ctx,NULL)) return ctx->diagnostic.status;
    const SourceFunction *body=ctx->bodies+function;
    if (!body->checked_library) return XR_XIR_OK;
    if (body->module>=(uint32_t)ctx->graph->spec_count) return XR_XIR_BAD_STRUCTURE;
    const XrModuleSpec *spec=ctx->graph->specs+body->module;
    if (spec->representation!=XR_MODULE_CHECKED_LIBRARY) return XR_XIR_OK;
    const XrXirArtifact *artifact=source_owned_artifact(spec);
    if (!artifact) return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *library=xr_xir_compile_artifact_module(artifact);
    const XrXirCompileContext *context=xr_xir_compile_artifact_context(artifact);
    if (!artifact || !library || library->stage!=XR_XIR_CHECKED || library->linkage_kind!=XR_XIR_LIBRARY ||
        !context || context->resources!=ctx->compile.resources) return XR_XIR_BAD_STRUCTURE;
    SourceLibraryUnit *unit=source_library_unit(ctx,library);
    if (!unit) return ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:ctx->diagnostic.status;
    if (unit->artifact!=artifact || unit->construction!=xr_xir_compile_artifact_construction(artifact) ||
        body->node || body->recipes || body->parameters!=module->functions[function].parameters)
        return XR_XIR_BAD_STRUCTURE;
    *preserved=true;return XR_XIR_OK;
}
static XrXirStatus source_view_normalize(SourceContext *ctx,const XrXirModule *module,XrXirDiagnostic *diagnostic) {
    XirViewNormalization *n=NULL;
    XirViewNormalizationSource source={module,ctx,source_view_checked_copy};
    XrXirStatus status=xir_compile_view_normalize_source_verified(&ctx->compile,module,&source,&n,diagnostic);
    if (status!=XR_XIR_OK) return status;
    XrXirConstruction *construction=NULL;
    XrXirFunction *functions=source_alloc(ctx,ctx->function_capacity,sizeof(*functions));
    SourceFunction *bodies=source_alloc(ctx,ctx->function_capacity,sizeof(*bodies));
    uint64_t added=0;
    if (!functions || !bodies) goto source_failure;
    for (uint32_t f=0;f<ctx->function_count;++f) {
        if (!source_work(ctx,NULL)) goto source_failure;
        functions[f]=n->bodies[f];bodies[f]=ctx->bodies[f];
        if (!n->functions[f].instructions) continue;
        added+=functions[f].instruction_count-ctx->functions[f].instruction_count;
        if (added>ctx->remaining_instructions) {source_fail(ctx,NULL,XR_XIR_BUDGET,"Source lifetime instructions exhausted");goto source_failure;}
        functions[f].instructions=source_view_copy(ctx,functions[f].instructions,functions[f].instruction_count,sizeof(*functions[f].instructions));
        functions[f].blocks=source_view_copy(ctx,functions[f].blocks,functions[f].block_count,sizeof(*functions[f].blocks));
        functions[f].operands=source_view_copy(ctx,functions[f].operands,functions[f].operand_count,sizeof(*functions[f].operands));
        if (!functions[f].instructions || !functions[f].blocks || (functions[f].operand_count && !functions[f].operands) ||
            !source_view_roots(ctx,f,n,bodies+f) || !source_view_regions(ctx,f,n,bodies+f) ||
            !source_view_debug(ctx,f,n,bodies+f)) goto source_failure;
        bodies[f].count=functions[f].instruction_count;bodies[f].operands=(uint32_t *)functions[f].operands;
        bodies[f].operand_count=bodies[f].operand_capacity=functions[f].operand_count;
    }
    SourceRootQuery *queries=source_view_queries(ctx,n);if (ctx->diagnostic.status!=XR_XIR_OK) goto source_failure;
    XrXirModule replacement=*module;replacement.functions=functions;
    if (!source_view_construction(ctx,n,&construction)) goto source_failure;
    status=xir_compile_view_prepare_ordinary(&ctx->compile,&replacement,construction,diagnostic);
    for (uint32_t f=0;status==XR_XIR_OK && f<ctx->function_count;++f)
        if (bodies[f].initialization_regions)
            status=xr_xir_compile_initialization_check(&ctx->compile,&replacement,f,bodies[f].initialization_regions,diagnostic);
    if (status==XR_XIR_OK) status=xir_compile_view_return_origins_verified(&ctx->compile,&replacement,diagnostic);
    if (status==XR_XIR_OK) status=xir_compile_view_affine_verified(&ctx->compile,&replacement,diagnostic);
    if (status==XR_XIR_OK) status=xir_compile_view_liveness_verified(&ctx->compile,&replacement,NULL,NULL,diagnostic);
    if (status==XR_XIR_OK) status=xir_compile_view_roots_verified(&ctx->compile,&replacement,NULL,NULL,diagnostic);
    /* Access sees the actual cleanup and relocated SSA before publication. */
    if (status==XR_XIR_OK) {
        status=xir_compile_view_cell_access_verified(&ctx->compile,&replacement,diagnostic);
        if (status!=XR_XIR_OK) status=source_view_access_failure(ctx,&replacement,diagnostic,status);
    }
    if (status==XR_XIR_OK) {
        xr_xir_compile_construction_free(ctx->construction);ctx->construction=construction;construction=NULL;
        ctx->functions=functions;ctx->bodies=bodies;ctx->root_queries=queries;ctx->remaining_instructions-=(uint32_t)added;
    }
    xr_xir_compile_construction_free(construction);xir_compile_view_normalization_free(n);return status;
source_failure:
    xr_xir_compile_construction_free(construction);xir_compile_view_normalization_free(n);return ctx->diagnostic.status;
}
