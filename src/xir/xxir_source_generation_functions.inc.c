/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_generation_functions.inc.c - Actual specialized entry receiving
 */
typedef struct GenerationOrdinaryFunctions {
    const XrXirCompileContext *context;
    const XrXirArtifact *source;
    uint32_t *functions;
    uint32_t count;
    XrXirArtifact *closed;
} GenerationOrdinaryFunctions;
static XrXirStatus generation_ordinary_receive(void *opaque,const XrXirSpecializationMapView *view) {
    GenerationOrdinaryFunctions *receive=opaque;
    const XrXirModule *source=xr_xir_compile_artifact_module(receive->source);
    const XrXirModule *closed=xr_xir_compile_artifact_module(view->closed);
    if (view->source!=receive->source || !source || !closed || receive->functions ||
        view->function_count!=source->function_count || (!view->identity && !view->functions))
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=generation_allocate(receive->context,view->function_count,
        sizeof(*receive->functions),(void **)&receive->functions);
    if (status!=XR_XIR_OK) return status;
    receive->count=view->function_count;
    for (uint32_t f=0;f<view->function_count;++f) {
        if (!xir_compile_work(receive->context,1)) return XR_XIR_BUDGET;
        uint32_t target=view->identity ? f : view->functions[f];
        if (target!=UINT32_MAX && target>=closed->function_count) return XR_XIR_BAD_STRUCTURE;
        receive->functions[f]=target;
    }
    return XR_XIR_OK;
}
static XrXirStatus generation_actual_program(GenerationOrdinaryFunctions *receive,
    const XrXirProgram *program) {
    XrXirArtifact *closed=NULL,*lowered=NULL;
    const XrXirSpecializationReceiver receiver={receive,generation_ordinary_receive};
    XrXirStatus status=xr_xir_compile_specialize_receive(receive->source,&receiver,&closed,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(closed,&program->target,&lowered,NULL);
    if (status==XR_XIR_OK && !xir_compile_work(receive->context,sizeof(program->checked_identity)))
        status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) {
        XrXirProgramProof proof=xr_xir_compile_program_proof(lowered);
        if (!proof.identity || memcmp(proof.identity,program->checked_identity,sizeof(program->checked_identity)))
            status=XR_XIR_BAD_STRUCTURE;
    }
    if (status==XR_XIR_OK) {receive->closed=closed;closed=NULL;}
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);
    return status;
}
static XrXirStatus generation_scalar_origin(const XrXirCompileContext *context,
    const XrXirOrigin *from,const XrXirOrigin *to,bool *equal) {
    *equal=false;
    if (from->argument_count!=to->argument_count || from->effect_argument_count!=to->effect_argument_count)
        return XR_XIR_OK;
    for (uint32_t a=0;a<from->argument_count;++a) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        XrXirType left=from->arguments[a],right=to->arguments[a];
        if (left!=right || left==XR_XIR_ERROR || (uint32_t)left>=XR_XIR_CONSTRUCTED_TYPE_BASE)
            return XR_XIR_OK;
    }
    for (uint32_t a=0;a<from->effect_argument_count;++a) {
        if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
        const XrXirEffectArgument *left=&from->effect_arguments[a],*right=&to->effect_arguments[a];
        if (left->parameter!=right->parameter || left->type!=right->type || left->type==XR_XIR_ERROR ||
            (uint32_t)left->type>=XR_XIR_CONSTRUCTED_TYPE_BASE) return XR_XIR_OK;
    }
    *equal=true;return XR_XIR_OK;
}
typedef struct GenerationFunctionReceiving {
    const XrXirCompileContext *context;
    const SourceGenerationRecord *record;
    const XrXirProgram *program;
    const GenerationOrdinaryFunctions *before,*after;
    uint32_t *functions;
} GenerationFunctionReceiving;
/* Template ordinals come from the actual Source receiving table. Complete
 * origins come from both authenticated specialization operations. Constructed
 * arguments require an additional receiving proof and cannot match by shape. */
static XrXirStatus generation_scalar_instances(const GenerationFunctionReceiving *receive) {
    const SourceHistoryCorrespondence *map=&receive->record->capture.semantic->history;
    const XrXirProgram *prior=map->unit->program;
    const XrXirModule *before=xr_xir_compile_artifact_module(receive->before->closed);
    const XrXirModule *after=xr_xir_compile_artifact_module(receive->after->closed);
    if (!before || !after || before->function_count!=prior->entry_count ||
        after->function_count!=receive->program->entry_count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<prior->entry_count;++f) {
        if (!xir_compile_work(receive->context,1)) return XR_XIR_BUDGET;
        if (receive->functions[f]!=UINT32_MAX || prior->entries[f].cleanup_owner) continue;
        const XrXirProvenance *a=before->provenance,*b=after->provenance;
        if (!a || !b || a->kind!=XR_XIR_EVIDENCE_INSTANCE || b->kind!=XR_XIR_EVIDENCE_INSTANCE ||
            a->count!=before->function_count || b->count!=after->function_count || !a->origins || !b->origins)
            return XR_XIR_BAD_STRUCTURE;
        const XrXirOrigin *origin=&a->origins[f];
        if (origin->function>=map->function_count) return XR_XIR_BAD_STRUCTURE;
        uint32_t declaration=map->functions[origin->function],found=UINT32_MAX;
        for (uint32_t g=0;g<after->function_count;++g) {
            if (!xir_compile_work(receive->context,1)) return XR_XIR_BUDGET;
            if (receive->program->entries[g].cleanup_owner || b->origins[g].function!=declaration) continue;
            bool equal=false;
            XrXirStatus status=generation_scalar_origin(receive->context,origin,&b->origins[g],&equal);
            if (status!=XR_XIR_OK) return status;
            if (!equal) continue;
            if (found!=UINT32_MAX || prior->entries[f].parameter_count!=receive->program->entries[g].parameter_count)
                return XR_XIR_BAD_STRUCTURE;
            found=g;
        }
        if (found==UINT32_MAX) continue;
        for (uint32_t other=0;other<prior->entry_count;++other) {
            if (!xir_compile_work(receive->context,1)) return XR_XIR_BUDGET;
            if (receive->functions[other]==found) return XR_XIR_BAD_STRUCTURE;
        }
        receive->functions[f]=found;
    }
    return XR_XIR_OK;
}
/* Two same-operation receipts and the real Checked receiving table establish
 * this composition. Physical ordinals, names and matching signatures alone
 * never authorize a route to code in another Program. */
static XrXirStatus generation_function_correspondence(const XrXirCompileContext *context,
    SourceGenerationRecord *record,const XrXirProgram *program) {
    const SourceHistoryCorrespondence *map=&record->capture.semantic->history;
    GenerationOrdinaryFunctions after={.context=context,.source=record->result.checked},before={0};
    XrXirStatus status=generation_actual_program(&after,program);
    if (status!=XR_XIR_OK || !map->unit) goto done;
    if (!map->unit->published || map->unit->owner!=record->unit.owner || !map->unit->checked ||
        !map->unit->program || map->unit->serial>=record->unit.serial) {status=XR_XIR_BAD_STAGE;goto done;}
    before=(GenerationOrdinaryFunctions){.context=context,.source=map->unit->checked};
    status=generation_actual_program(&before,map->unit->program);
    if (status!=XR_XIR_OK) goto done;
    if (before.count!=map->function_count || !map->functions) {status=XR_XIR_BAD_STRUCTURE;goto done;}
    uint32_t count=map->unit->program->entry_count;
    uint32_t *functions=NULL;
    status=generation_allocate(context,count,sizeof(*functions),(void **)&functions);
    if (status!=XR_XIR_OK) goto done;
    if (!xir_compile_work(context,(uint64_t)count*sizeof(*functions))) {
        xr_compile_resources_free(functions);status=XR_XIR_BUDGET;goto done;
    }
    for (uint32_t f=0;f<count;++f) functions[f]=UINT32_MAX;
    for (uint32_t f=0;f<before.count;++f) {
        if (!xir_compile_work(context,1)) {status=XR_XIR_BUDGET;break;}
        uint32_t source=before.functions[f],received=map->functions[f];
        if (received>=after.count) {status=XR_XIR_BAD_STRUCTURE;break;}
        uint32_t target=after.functions[received];
        if (source==UINT32_MAX) continue;
        if (source<count && map->unit->program->entries[source].cleanup_owner) continue;
        if (source>=count || target>=program->entry_count || functions[source]!=UINT32_MAX ||
            map->unit->program->entries[source].parameter_count!=program->entries[target].parameter_count ||
            map->unit->program->entries[source].cleanup_owner || program->entries[target].cleanup_owner) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        functions[source]=target;
    }
    if (status==XR_XIR_OK) {
        const GenerationFunctionReceiving receive={context,record,program,&before,&after,functions};
        status=generation_scalar_instances(&receive);
    }
    if (status==XR_XIR_OK) {
        xr_compile_resources_free(record->received_functions);
        record->received_functions=functions;record->received_function_count=count;
    } else xr_compile_resources_free(functions);
done:
    xr_xir_compile_artifact_free(before.closed);xr_xir_compile_artifact_free(after.closed);
    xr_compile_resources_free(before.functions);xr_compile_resources_free(after.functions);
    return status;
}
