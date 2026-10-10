/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_publish.inc.c - Atomic common ROOT publication
 *
 * KEY CONCEPT:
 *   The single invocation queue completes and seals every equation before
 *   canonical formulas, facts and cause copies enter the receiving owner.
 */
typedef struct EffectInvocationPublication {
    XrCompileResources *resources;
    uint32_t count;
    XrXirRootFormula *formulas;
    XrXirRootEffectWitness *witnesses;
} EffectInvocationPublication;

static void effect_invocation_publication_free(EffectInvocationPublication *publication) {
    if (!publication) return;
    for (uint32_t f=0;f<publication->count;++f)
        xr_compile_resources_free((void *)publication->formulas[f].terms);
    xr_compile_resources_free(publication->witnesses);
    xr_compile_resources_free(publication->formulas);xr_compile_resources_free(publication);
}

/* The legacy receiving contracts free each term vector independently. Copy
 * their exact canonical records; never lend pointers into the packed solution. */
static XrXirStatus effect_invocation_publication_stage(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,EffectInvocationPublication **output) {
    if (!certificate || certificate->resources!=work->resources || !certificate->solution ||
        !output || *output || !certificate->facts || !certificate->witnesses ||
        certificate->solution->count!=certificate->bodies.function_count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;uint32_t count=certificate->solution->count;
    EffectInvocationPublication *publication=xir_compile_calloc(work,1,sizeof(*publication),&status);
    if (!publication) return status;
    publication->resources=work->resources;
    publication->formulas=xir_compile_calloc(work,count,sizeof(*publication->formulas),&status);
    if (!publication->formulas) { effect_invocation_publication_free(publication);return status; }
    publication->count=count;
    uint64_t witness_count=(uint64_t)count*4;
    if (witness_count>SIZE_MAX/sizeof(*publication->witnesses)) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) publication->witnesses=xir_compile_calloc(work,(size_t)witness_count,
        sizeof(*publication->witnesses),&status);
    if (!publication->witnesses) { effect_invocation_publication_free(publication);return status; }
    for (uint32_t f=0;f<count && status==XR_XIR_OK;++f) {
        XrXirRootFormula value=certificate->solution->formulas[f];
        uint64_t bytes=(uint64_t)value.term_count*sizeof(*value.terms);
        if (bytes>SIZE_MAX || !!value.terms!=!!value.term_count) { status=XR_XIR_BAD_STRUCTURE;break; }
        XrXirRootTerm *terms=bytes?xir_compile_alloc(work,(size_t)bytes,&status):NULL;
        if (bytes && !terms) break;
        if (!xir_compile_work(work,bytes+sizeof(value))) {
            xr_compile_resources_free(terms);status=XR_XIR_BUDGET;break;
        }
        if (bytes) memcpy(terms,value.terms,(size_t)bytes);
        value.terms=terms;publication->formulas[f]=value;
        for (uint32_t domain=0;domain<4 && status==XR_XIR_OK;++domain) {
            bool symbolic=false;
            status=effect_invocation_public_selected(work,certificate,f,domain,
                &publication->witnesses[(size_t)domain*count+f],&symbolic);
        }
    }
    if (status!=XR_XIR_OK) { effect_invocation_publication_free(publication);return status; }
    *output=publication;return XR_XIR_OK;
}

/* A source refinement owns its pre-bottom records in the verified descriptor
 * prefix. INSTANCE originals instead come from the full owned source proof. */
static XrXirStatus effect_invocation_publication_bounds(const XrXirCompileContext *work,
    const XrXirModule *module,const XrXirEffects *effects,EffectInvocationDeclaredBounds **output) {
    if (module->provenance && module->provenance->kind==XR_XIR_EVIDENCE_INSTANCE)
        return effect_invocation_instance_bounds(work,module,output);
    EffectRefinementBounds *refinement=effects->refinement;
    XrXirStatus status=refinement?effect_refinement_match(work,module,refinement):XR_XIR_OK;
    EffectInvocationDeclaredBounds *bounds=NULL;
    if (status==XR_XIR_OK) status=effect_invocation_bounds_new(work,&bounds);
    for (uint32_t f=0;f<module->function_count && status==XR_XIR_OK;++f) {
        XrXirFunction original=module->functions[f];const XrXirType *instructions=NULL;
        XrXirType *source_types=NULL;
        if (refinement) {
            original.parameters=refinement->functions[f].parameters;
            instructions=refinement->functions[f].instructions;
        }
        status=effect_invocation_source_declarations(work,module,f,instructions,&source_types);
        if (source_types) instructions=source_types;
        if (status==XR_XIR_OK)
            status=effect_invocation_bounds_capture(work,bounds,module->types,&original,f,instructions);
        xr_compile_resources_free(source_types);
    }
    if (status!=XR_XIR_OK) { effect_invocation_bounds_free(bounds);return status; }
    *output=bounds;return XR_XIR_OK;
}

/* Precharge the complete commit before changing any receiving field. All
 * allocations, typed proof and cause construction have already succeeded. */
static XrXirStatus effect_invocation_publication_commit(const XrXirCompileContext *work,
    XrXirEffects *effects,EffectInvocationCertificate **certificate,
    EffectInvocationPublication *publication) {
    EffectInvocationCertificate *fresh=*certificate;uint32_t count=effects->count;
    if (!publication || publication->resources!=work->resources || publication->count!=count ||
        !fresh || fresh->resources!=work->resources || fresh->solution->count!=count ||
        !effects->contracts || !effects->root || !effects->root_witnesses ||
        !effects->unresolved_witnesses || !effects->formula_terminals || !effects->constant_witnesses)
        return XR_XIR_BAD_STRUCTURE;
    uint64_t bytes=(uint64_t)count*(sizeof(XrXirRootFormula)+sizeof(XrXirRootEffects)+
        8*sizeof(XrXirRootEffectWitness));
    if (!xir_compile_work(work,bytes)) return XR_XIR_BUDGET;
    for (uint32_t f=0;f<count;++f) {
        xr_compile_resources_free((void *)effects->contracts[f].formula.terms);
        effects->contracts[f].formula=publication->formulas[f];
        publication->formulas[f].terms=NULL;
    }
    memcpy(effects->root,fresh->facts,(size_t)count*sizeof(*effects->root));
    memcpy(effects->root_witnesses,publication->witnesses,(size_t)count*sizeof(*effects->root_witnesses));
    memcpy(effects->unresolved_witnesses,publication->witnesses+count,
        (size_t)count*sizeof(*effects->unresolved_witnesses));
    memcpy(effects->formula_terminals,publication->witnesses,(size_t)count*4*sizeof(*effects->formula_terminals));
    memcpy(effects->constant_witnesses,publication->witnesses+(size_t)count*2,
        (size_t)count*2*sizeof(*effects->constant_witnesses));
    EffectInvocationCertificate *old=effects->invocations;effects->invocations=fresh;*certificate=NULL;
    effect_invocation_certificate_free(old);return XR_XIR_OK;
}

/* This is the sole production ROOT derivation. It neither runs the retired
 * formula solver nor grants Program permission from a temporary empty mask. */
static XrXirStatus effect_invocation_publish(const XrXirCompileContext *work,
    const XrXirModule *module,XrXirEffects *effects,EffectGraph *graph,
    const EffectInvocationDeclaredBounds *declared,const EffectOrdinaryContexts *dense) {
    if (!module || !graph || !xir_effects_context_matches(work,effects,module->function_count) ||
        !effects->contracts || (declared && declared->resources!=work->resources)) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationDeclaredBounds *temporary=NULL;EffectInvocationCertificate *certificate=NULL;
    EffectInvocationPublication *publication=NULL;XrXirStatus status=XR_XIR_OK;
    if (!declared) status=effect_invocation_publication_bounds(work,module,effects,&temporary);
    EffectInvocationDriverInput input={work,module,effects,graph,declared?declared:temporary,dense};
    if (status==XR_XIR_OK) status=effect_invocation_derive(&input,&certificate);
    if (status==XR_XIR_OK) status=effect_invocation_publication_stage(work,certificate,&publication);
    if (status==XR_XIR_OK) status=effect_invocation_publication_commit(work,effects,&certificate,publication);
    effect_invocation_publication_free(publication);effect_invocation_certificate_free(certificate);
    effect_invocation_bounds_free(temporary);return status;
}

static XrXirStatus effect_invocation_effect_edge_mask(const XrXirCompileContext *work,
    const XrXirEffects *effects,uint32_t caller,uint32_t instruction,uint32_t target,uint32_t *output) {
    if (!effects || effects->resources!=work->resources) return XR_XIR_BAD_STRUCTURE;
    return effect_invocation_certificate_edge_mask(work,effects->invocations,caller,instruction,target,output);
}

#include "xxir_effect_invocation_forest.inc.c"

/* The last expansion still visits every actual body and edge. A stable
 * round may retain only this operation's already sealed summary, after exact
 * correspondence with its independently owned input copy has been checked. */
static XrXirStatus effect_context_dense_bytes(const XrXirCompileContext *context,
    const void *left,const void *right,uint64_t count,size_t size) {
    if (!size || count>SIZE_MAX/size || count*size==UINT64_MAX || (count && (!left || !right)))
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,count*size+1)) return XR_XIR_BUDGET;
    return !count || !memcmp(left,right,(size_t)count*size)?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus effect_context_dense_body_match(const XrXirCompileContext *context,
    const EffectContextOwner *owner,uint32_t index) {
    const EffectOrdinaryContexts *dense=owner->dense;
    const EffectInvocationCertificate *certificate=dense->uses.invocations;
    const XrXirFunction *actual=&dense->functions[index],*sealed=&certificate->bodies.functions[index];
    const EffectOrdinaryNode *node=&dense->nodes[index];const XrXirOrigin *origin=&certificate->origins[index];
    if (!xir_compile_work(context,10)) return XR_XIR_BUDGET;
    if (actual->name_length!=sealed->name_length || actual->parameter_count!=sealed->parameter_count ||
        actual->result!=sealed->result || actual->instruction_count!=sealed->instruction_count ||
        actual->operand_count!=sealed->operand_count || actual->block_count!=sealed->block_count ||
        node->declaration!=origin->function || node->argument_count!=origin->argument_count ||
        node->owner!=certificate->owners[index]) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_context_dense_bytes(context,actual->name,sealed->name,actual->name_length,1);
    if (status==XR_XIR_OK) status=effect_context_dense_bytes(context,actual->parameters,sealed->parameters,
        actual->parameter_count,sizeof(*actual->parameters));
    if (status==XR_XIR_OK) status=effect_context_dense_bytes(context,node->physical_types,sealed->parameters,
        actual->parameter_count,sizeof(*actual->parameters));
    if (status==XR_XIR_OK) status=effect_context_dense_bytes(context,actual->instructions,sealed->instructions,
        actual->instruction_count,sizeof(*actual->instructions));
    if (status==XR_XIR_OK) status=effect_context_dense_bytes(context,actual->operands,sealed->operands,
        actual->operand_count,sizeof(*actual->operands));
    if (status==XR_XIR_OK) status=effect_context_dense_bytes(context,actual->blocks,sealed->blocks,
        actual->block_count,sizeof(*actual->blocks));
    if (status==XR_XIR_OK) status=effect_context_dense_bytes(context,node->arguments,origin->arguments,
        node->argument_count,sizeof(*node->arguments));
    const EffectInvocationFunctionBounds *bounds=&owner->declared->functions[index];
    const EffectInvocationFunctionBounds *copied=&certificate->declared->functions[index];
    uint64_t values=(uint64_t)actual->parameter_count+actual->instruction_count;
    if (status==XR_XIR_OK && (bounds->values!=values || bounds->values!=copied->values ||
        (values && (!bounds->bounds || !copied->bounds)))) status=XR_XIR_BAD_STRUCTURE;
    for (uint32_t v=0;v<bounds->values && status==XR_XIR_OK;++v) {
        if (!xir_compile_work(context,4)) { status=XR_XIR_BUDGET;break; }
        if (bounds->bounds[v].mask!=copied->bounds[v].mask ||
            bounds->bounds[v].callable!=copied->bounds[v].callable) status=XR_XIR_BAD_STRUCTURE;
    }
    return status;
}

static XrXirStatus effect_context_dense_stable(const XrXirCompileContext *context,
    EffectContextOwner *owner,const XrXirModule *source) {
    if (!xir_compile_context_valid(context) || !owner || !source || !owner->dense || !owner->ordinary)
        return XR_XIR_BAD_STRUCTURE;
    EffectOrdinaryContexts *dense=owner->dense,*ordinary=owner->ordinary;
    const EffectInvocationCertificate *certificate=dense->uses.invocations;
    if (!xir_compile_context_valid(context) || dense->terms.remaining!=context || !dense->dense ||
        !ordinary || ordinary->dense || ordinary->base_count!=source->function_count ||
        dense->base_count!=source->function_count || ordinary->uses.resources!=context->resources ||
        dense->uses.resources!=context->resources || dense->uses.count!=dense->count || !dense->uses.root ||
        !certificate || certificate->resources!=context->resources || !certificate->equations ||
        !certificate->solution || certificate->solution->count!=dense->count ||
        certificate->equations->work || certificate->equations->module || certificate->equations->graph ||
        certificate->equations->effects || certificate->equations->declared ||
        certificate->bodies.function_count!=dense->count || !certificate->bodies.functions ||
        certificate->bodies.stage!=source->stage || certificate->bodies.linkage_kind!=source->linkage_kind ||
        certificate->bodies.types!=&certificate->terms.types || !certificate->origins || !certificate->owners ||
        !source->declarations || !certificate->declarations ||
        certificate->declarations->root_module!=source->declarations->root_module ||
        certificate->declarations->entry_function!=source->declarations->entry_function ||
        certificate->declarations->module_count!=source->declarations->module_count ||
        certificate->declarations->slot_count!=source->declarations->slot_count ||
        certificate->declarations->literal_count!=source->declarations->literal_count ||
        !owner->declared || owner->declared->resources!=context->resources || owner->declared->count!=dense->count ||
        !certificate->declared || certificate->declared->resources!=context->resources ||
        certificate->declared->count!=dense->count) return XR_XIR_BAD_STRUCTURE;
    ordinary->terms.remaining=context;
    XrXirStatus status=owner->refinement?effect_refinement_match(context,source,owner->refinement):XR_XIR_OK;
    if (status==XR_XIR_OK)
        status=effect_terms_snapshot_match(&ordinary->terms,source->types,ordinary->source_type_count);
    if (status==XR_XIR_OK) status=effect_ordinary_bodies_match(ordinary,source);
    ordinary->terms.remaining=NULL;
    if (status==XR_XIR_OK)
        status=effect_terms_snapshot_match(&dense->terms,&certificate->terms.types,dense->terms.types.count);
    for (uint32_t f=0;f<dense->count && status==XR_XIR_OK;++f)
        status=effect_context_dense_body_match(context,owner,f);
    return status;
}
