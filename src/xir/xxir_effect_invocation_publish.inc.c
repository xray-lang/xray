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
/* Only the current private request supplies selected values. A failure leaves
 * partial parent scratch for unconditional free, with no public field changed. */
static XrXirStatus effect_invocation_publication_write(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,
    const uint32_t *selected,EffectInvocationResult *result,void *receiver) {
    EffectInvocationResult **expected=receiver;
    if (!xir_compile_context_valid(work) || !certificate || !selected || !expected || *expected!=result ||
        !effect_body_material_shape(certificate->body) || !result || certificate->result!=result || !result->functions ||
        certificate->body->resources!=work->resources || certificate->resources!=work->resources ||
        result->resources!=work->resources || result->count!=effect_body_count(certificate->body) ||
        result->capacity!=result->count || function>=result->count || !result->facts || !result->witnesses)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t domain=0;domain<4;++domain) {
        XrXirRootEffectWitness witness;
        XrXirStatus status=effect_invocation_selected_witness(work,certificate,function,selected[domain],&witness);
        if (status!=XR_XIR_OK) return status;
        bool symbolic=false;
        if (!xir_compile_work(work,sizeof(EffectInvocationProjectedCause))) return XR_XIR_BUDGET;
        const EffectInvocationProjectedCause projected={selected[domain],&witness};
        status=effect_invocation_public_transform(work,certificate,function,&projected,
            &result->witnesses[(size_t)domain*result->count+function],&symbolic);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* A source refinement owns its pre-bottom records in the verified descriptor
 * prefix. INSTANCE originals instead come from the full owned source proof. */
static inline XrXirStatus effect_invocation_publication_bounds(const XrXirCompileContext *work,
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

/* The receiving packet has no permission mirror. Compare every final field
 * to an independent current-material replay after all projection callbacks,
 * before any public contract or witness changes. Atom indices only locate. */
static XrXirStatus effect_invocation_publication_match(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const EffectInvocationResult *result) {
    if (!xir_compile_context_valid(work) || !certificate || !result ||
        !certificate->projection_atoms || !result->facts || !result->witnesses)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_invocation_certificate_result_match(work,certificate,result);
    for (uint32_t f=0;f<result->count && status==XR_XIR_OK;++f) {
        uint32_t selected[4];
        status=effect_invocation_projection_domains(work,certificate,f,0,4,selected);
        if (status!=XR_XIR_OK) break;
        if (!xir_compile_work(work,4)) { status=XR_XIR_BUDGET;break; }
        if (result->facts[f].requires_root!=(selected[0]!=UINT32_MAX) ||
            result->facts[f].unresolved!=(selected[1]!=UINT32_MAX)) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        for (uint32_t domain=0;domain<4 && status==XR_XIR_OK;++domain) {
            size_t at=(size_t)domain*result->count+f;
            if (!xir_compile_work(work,2)) { status=XR_XIR_BUDGET;break; }
            if (certificate->projection_atoms[at]!=selected[domain]) { status=XR_XIR_BAD_STRUCTURE;break; }
            XrXirRootEffectWitness witness,expected;bool symbolic=false;
            status=effect_invocation_selected_witness(work,certificate,f,selected[domain],&witness);
            if (status!=XR_XIR_OK) break;
            if (!xir_compile_work(work,sizeof(EffectInvocationProjectedCause))) { status=XR_XIR_BUDGET;break; }
            const EffectInvocationProjectedCause cause={selected[domain],&witness};
            status=effect_invocation_public_transform(work,certificate,f,&cause,&expected,&symbolic);
            if (status!=XR_XIR_OK) break;
            if (!xir_compile_work(work,5)) { status=XR_XIR_BUDGET;break; }
            if (!effect_invocation_witness_equal(&expected,&result->witnesses[at])) status=XR_XIR_BAD_STRUCTURE;
        }
    }
    return status;
}

/* Precharge every final assignment before changing a receiving field.
 * Canonical vectors and the immutable four-domain witness inventory belong
 * solely to the new certificate. Mutable context projections stay separate. */
static XrXirStatus effect_invocation_publication_commit(const XrXirCompileContext *work,
    XrXirEffects *effects,EffectInvocationCertificate **certificate) {
    if (!xir_compile_context_valid(work) || !effects || effects->resources!=work->resources ||
        !certificate) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationCertificate *fresh=*certificate;uint32_t count=effects->count;
    EffectInvocationResult *result=fresh?fresh->result:NULL;
    if (!result || result->resources!=work->resources || result->count!=count ||
        fresh->resources!=work->resources || result->capacity!=count ||
        !result->functions || !result->facts || !result->witnesses || !effects->contracts ||
        !effects->root || !effects->root_witnesses || !effects->unresolved_witnesses)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_invocation_publication_match(work,fresh,result);
    if (status!=XR_XIR_OK) return status;
    uint64_t bytes=(uint64_t)count*(sizeof(XrXirRootFormula)+sizeof(XrXirRootEffects)+
        2*sizeof(XrXirRootEffectWitness))+3*sizeof(void *);
    if (!xir_compile_work(work,bytes)) return XR_XIR_BUDGET;
    EffectInvocationCertificate *old=effects->invocations;
    const EffectInvocationResult *old_result=effects->result_borrow;
    for (uint32_t f=0;f<count;++f) {
        if (!old_result) xr_compile_resources_free((void *)effects->contracts[f].formula.terms);
        effects->contracts[f].formula=result->functions[f].formula;
    }
    memcpy(effects->root,result->facts,(size_t)count*sizeof(*effects->root));
    memcpy(effects->root_witnesses,result->witnesses,(size_t)count*sizeof(*effects->root_witnesses));
    memcpy(effects->unresolved_witnesses,result->witnesses+count,
        (size_t)count*sizeof(*effects->unresolved_witnesses));
    if (!old_result) {
        xr_compile_resources_free(effects->formula_terminals);
        xr_compile_resources_free(effects->constant_witnesses);
    }
    effects->formula_terminals=result->witnesses;
    effects->constant_witnesses=result->witnesses+(size_t)count*2;
    effects->result_borrow=result;
    effects->invocations=fresh;effects->cells=fresh->body->cells;*certificate=NULL;
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
    EffectInvocationResult *publication=NULL;XrXirStatus status=XR_XIR_OK;
    if (!graph->material || (!graph->physical_owned && graph->material!=effects->generation))
        status=effect_graph_physical_refresh(work,module,graph,effects->refinement);
    if (status==XR_XIR_OK && (!effect_body_material_shape(graph->material) ||
        graph->material->resources!=work->resources || !graph->material->declared)) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK && declared) {
        status=effect_invocation_bounds_copy_physical(work,declared,graph->physical,&temporary);
        if (status==XR_XIR_OK && !xir_compile_work(work,2)) status=XR_XIR_BUDGET;
        if (status==XR_XIR_OK) {
            effect_invocation_bounds_free(graph->material->declared);
            graph->material->declared=temporary;temporary=NULL;
        }
    }
    if (status==XR_XIR_OK && dense && (dense->count!=module->function_count ||
        dense->functions!=module->functions || &dense->terms.types!=module->types ||
        &dense->uses!=effects)) status=XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest body={0};
    if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(body))) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) status=effect_invocation_collect(work,module,effects,graph,
        graph->material->declared,&body);
    EffectInvocationDriverInput input={work,&body,effects,graph,
        status==XR_XIR_OK?graph->material->declared:NULL,dense};
    EffectInvocationProjectionSink sink;
    if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(sink))) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) {
        sink=(EffectInvocationProjectionSink){work->resources,module->function_count,
            &publication,effect_invocation_publication_write};
        status=effect_invocation_derive(&input,&certificate,&sink,&publication);
    }
    /* The raw boundary remains inside this whole operation, after the
     * last projection callback and before any public result changes. */
    if (status==XR_XIR_OK) status=effect_invocation_current_input_match(work,module,certificate->body);
    if (status==XR_XIR_OK && !xir_compile_work(work,2)) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) status=effect_invocation_publication_commit(work,effects,&certificate);
    if (status==XR_XIR_OK) {
        /* Construction ownership has moved through seal to this certificate.
         * The graph borrows it only while the same generation remains live. */
        graph->material=effects->invocations->body;
        graph->physical=graph->material->physical;graph->physical_owned=false;
    }
    if (certificate && certificate->body) {
        if (effects->cells==certificate->body->cells) effects->cells=NULL;
        if (graph->material==certificate->body) {
            graph->material=NULL;graph->physical=NULL;graph->physical_owned=false;
        }
    }
    effect_invocation_result_free(publication);effect_invocation_certificate_free(certificate);
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
    const EffectContextOwner *owner,uint32_t index,const EffectInvocationCertificate *certificate) {
    const EffectOrdinaryContexts *dense=owner->dense;
    const XrXirFunction *actual=&dense->functions[index];
    const EffectBodyFunction *sealed=&certificate->body->functions[index];
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
    if (status==XR_XIR_OK) {
        EffectBodyRead read=effect_body_compile_read(context);
        status=effect_body_records_match(&read,sealed,actual);
    }
    if (status==XR_XIR_OK) status=effect_context_dense_bytes(context,actual->operands,sealed->operands,
        actual->operand_count,sizeof(*actual->operands));
    if (status==XR_XIR_OK) status=effect_context_dense_bytes(context,actual->blocks,sealed->blocks,
        actual->block_count,sizeof(*actual->blocks));
    if (status==XR_XIR_OK) status=effect_context_dense_bytes(context,node->arguments,origin->arguments,
        node->argument_count,sizeof(*node->arguments));
    const EffectInvocationFunctionBounds *bounds=&owner->declared->functions[index];
    const EffectInvocationFunctionBounds *copied=&certificate->declared->functions[index];
    uint64_t values=(uint64_t)actual->parameter_count+actual->instruction_count;
    if (status==XR_XIR_OK && (values!=bounds->values || values!=copied->values ||
        bounds->count!=copied->count || bounds->count>values ||
        (!!bounds->bounds!=!!bounds->count) || (!!copied->bounds!=!!copied->count))) status=XR_XIR_BAD_STRUCTURE;
    for (uint32_t v=0;v<bounds->count && status==XR_XIR_OK;++v) {
        if (!xir_compile_work(context,4)) { status=XR_XIR_BUDGET;break; }
        if (bounds->bounds[v].value!=copied->bounds[v].value ||
            bounds->bounds[v].mask!=copied->bounds[v].mask ||
            bounds->bounds[v].callable!=copied->bounds[v].callable) status=XR_XIR_BAD_STRUCTURE;
    }
    return status;
}

static XrXirStatus effect_context_dense_stable_read(const XrXirCompileContext *context,
    const EffectContextOwner *owner,const XrXirModule *source,
    const EffectInvocationCertificate *certificate,EffectTerms *ordinary_terms,EffectTerms *dense_terms) {
    if (!xir_compile_context_valid(context) || !owner || !source || !owner->dense || !owner->ordinary)
        return XR_XIR_BAD_STRUCTURE;
    const EffectOrdinaryContexts *dense=owner->dense,*ordinary=owner->ordinary;
    if (!xir_compile_context_valid(context) || !dense->dense ||
        !ordinary || ordinary->dense || ordinary->base_count!=source->function_count ||
        dense->base_count!=source->function_count || ordinary->uses.resources!=context->resources ||
        dense->uses.resources!=context->resources || dense->uses.count!=dense->count || !dense->uses.root ||
        !certificate || !effect_body_material_shape(certificate->body) ||
        certificate->body->resources!=certificate->resources || certificate->resources!=context->resources || !certificate->equations ||
        !certificate->result || certificate->result->count!=dense->count ||
        dense->uses.result_borrow!=certificate->result ||
        certificate->equations->work || certificate->equations->graph ||
        certificate->equations->effects || certificate->equations->declared ||
        effect_body_count(certificate->body)!=dense->count || !certificate->body->functions ||
        certificate->body->stage!=source->stage || certificate->body->linkage_kind!=source->linkage_kind ||
        certificate->body->domain.types!=&certificate->body->terms.types || !certificate->origins || !certificate->owners ||
        !source->declarations || !certificate->declarations ||
        certificate->declarations->root_module!=source->declarations->root_module ||
        certificate->declarations->entry_function!=source->declarations->entry_function ||
        certificate->declarations->module_count!=source->declarations->module_count ||
        certificate->declarations->slot_count!=source->declarations->slot_count ||
        certificate->declarations->literal_count!=source->declarations->literal_count ||
        !owner->declared || owner->declared->resources!=context->resources || owner->declared->count!=dense->count ||
        !certificate->declared || certificate->declared->resources!=context->resources ||
        certificate->declared->count!=dense->count) return XR_XIR_BAD_STRUCTURE;
    if (!ordinary_terms || !dense_terms || ordinary_terms->remaining!=context ||
        dense_terms->remaining!=context) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=owner->refinement?effect_refinement_match(context,source,owner->refinement):XR_XIR_OK;
    if (status==XR_XIR_OK)
        status=effect_terms_snapshot_match(ordinary_terms,source->types,ordinary->source_type_count);
    if (status==XR_XIR_OK) status=effect_ordinary_bodies_match(ordinary,source,ordinary_terms);
    if (status==XR_XIR_OK)
        status=effect_terms_snapshot_match(dense_terms,&certificate->terms.types,dense->terms.types.count);
    for (uint32_t f=0;f<dense->count && status==XR_XIR_OK;++f)
        status=effect_context_dense_body_match(context,owner,f,certificate);
    return status;
}

/* Mutable construction retains its own scratch capacity. Published queries
 * supply separate request-owned Terms to the exact same correspondence core. */
static XrXirStatus effect_context_dense_stable(const XrXirCompileContext *context,
    EffectContextOwner *owner,const XrXirModule *source) {
    if (!owner || !owner->dense || !owner->ordinary || owner->dense->terms.remaining!=context)
        return XR_XIR_BAD_STRUCTURE;
    owner->ordinary->terms.remaining=context;
    XrXirStatus status=effect_context_dense_stable_read(context,owner,source,
        owner->dense->uses.invocations,&owner->ordinary->terms,&owner->dense->terms);
    owner->ordinary->terms.remaining=NULL;return status;
}
