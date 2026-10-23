/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_facts.inc.c - Owned final Checked Template inputs and facts
 *
 * KEY CONCEPT:
 *   Retention preserves immutable storage, never admission. Every consumer
 *   matches its current complete input and replays the independent terminal.
 */
struct XirSourceFacts {
    XrXirCompileContext context;
    atomic_uint_fast64_t references;
    XrXirEffects *effects;
    XrXirProvenance *input;
    XrXirConstruction *construction;
};

XR_FUNC void xir_source_facts_release(XirSourceFacts *facts) {
    if (!facts) return;
    uint_fast64_t previous=atomic_fetch_sub_explicit(&facts->references,1,memory_order_acq_rel);
    XR_CHECK(previous,"A live Source facts reference must be released exactly once");
    if (previous!=1) return;
    xr_xir_compile_effects_free(facts->effects);
    xr_xir_compile_provenance_free(facts->input);
    xr_xir_compile_construction_free(facts->construction);
    xr_compile_resources_free(facts);
}

static XrXirStatus source_facts_retain(const XrXirCompileContext *context,
    XirSourceFacts *facts,XirSourceFacts **output) {
    if (!facts || !output || *output || facts->context.resources!=context->resources)
        return XR_XIR_BAD_STRUCTURE;
    uint_fast64_t count=atomic_load_explicit(&facts->references,memory_order_relaxed);
    for (;;) {
        if (!count || count==UINT_FAST64_MAX) return XR_XIR_BUDGET;
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (atomic_compare_exchange_strong_explicit(&facts->references,&count,count+1,
            memory_order_relaxed,memory_order_relaxed)) break;
    }
    if (!xir_compile_work(context,sizeof(*output))) { xir_source_facts_release(facts);return XR_XIR_BUDGET; }
    *output=facts;return XR_XIR_OK;
}

static XrXirStatus source_facts_contract_match(const XrXirCompileContext *context,
    const XrXirFunctionEffectContract *stored,const XrXirFunctionEffectContract *actual) {
    if (!xir_compile_work(context,5)) return XR_XIR_BUDGET;
    if (stored->parameter_count!=actual->parameter_count ||
        stored->formula.constant_mask!=actual->formula.constant_mask ||
        stored->formula.term_count!=actual->formula.term_count ||
        stored->value_count!=actual->value_count || stored->binding_count!=actual->binding_count)
        return XR_XIR_BAD_STRUCTURE;
    if (!!stored->parameters!=!!stored->parameter_count || !!actual->parameters!=!!actual->parameter_count ||
        !!stored->formula.terms!=!!stored->formula.term_count || !!actual->formula.terms!=!!actual->formula.term_count ||
        !!stored->values!=!!stored->value_count || !!actual->values!=!!actual->value_count ||
        !!stored->bindings!=!!stored->binding_count || !!actual->bindings!=!!actual->binding_count)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=0;p<stored->parameter_count;++p) {
        if (!xir_compile_work(context,4)) return XR_XIR_BUDGET;
        if (stored->parameters[p].kind!=actual->parameters[p].kind ||
            stored->parameters[p].uses!=actual->parameters[p].uses) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t t=0;t<stored->formula.term_count;++t) {
        if (!xir_compile_work(context,4)) return XR_XIR_BUDGET;
        if (stored->formula.terms[t].kind!=actual->formula.terms[t].kind ||
            stored->formula.terms[t].index!=actual->formula.terms[t].index) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t v=0;v<stored->value_count;++v) {
        if (!xir_compile_work(context,6)) return XR_XIR_BUDGET;
        if (stored->values[v].instruction!=actual->values[v].instruction ||
            stored->values[v].mode!=actual->values[v].mode ||
            stored->values[v].declared_type!=actual->values[v].declared_type) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t b=0;b<stored->binding_count;++b) {
        if (!xir_compile_work(context,8)) return XR_XIR_BUDGET;
        if (stored->bindings[b].family!=actual->bindings[b].family ||
            stored->bindings[b].instruction!=actual->bindings[b].instruction ||
            stored->bindings[b].parameter!=actual->bindings[b].parameter ||
            stored->bindings[b].value!=actual->bindings[b].value) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus source_facts_construction_match(const XrXirCompileContext *context,
    const XrXirModule *source,const XrXirConstruction *stored,const XrXirConstruction *actual) {
    XrXirStatus status=xir_construction_shape(context,source->types,actual);
    if (status==XR_XIR_OK) status=xir_construction_shape(context,source->types,stored);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    if (stored->count!=actual->count) return XR_XIR_BAD_STRUCTURE;
    if (stored->slots.count!=actual->slots.count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t s=0;s<stored->slots.count;++s) {
        const XrXirSlotFact *a=&stored->slots.rows[s],*b=&actual->slots.rows[s];
        if (!xir_compile_work(context,sizeof(*a)+a->name_length)) return XR_XIR_BUDGET;
        if (a->slot!=b->slot || a->name_length!=b->name_length || a->exported!=b->exported ||
            memcmp(a->name,b->name,a->name_length)) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t n=0;n<stored->count;++n) {
        const XrXirConstructionRow *a=&stored->rows[n],*b=&actual->rows[n];
        if (!xir_compile_work(context,6)) return XR_XIR_BUDGET;
        if (stored->kinds[n]!=actual->kinds[n] || a->default_initializer!=b->default_initializer ||
            a->field_count!=b->field_count) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t f=0;f<a->field_count;++f) {
            if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
            if (a->field_initializers[f]!=b->field_initializers[f]) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus source_facts_input_match(const XrXirCompileContext *context,
    const XrXirArtifact *artifact,const XirSourceFacts *facts) {
    if (!xir_compile_context_valid(context) || !artifact || !facts ||
        artifact->module.stage!=XR_XIR_CHECKED || facts->context.resources!=context->resources ||
        !facts->effects || !facts->input || !facts->construction ||
        !artifact->module.provenance || artifact->module.provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirProvenance *actual=artifact->module.provenance,*stored=facts->input;
    if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
    if (stored->kind!=actual->kind || stored->contract_count!=actual->contract_count ||
        stored->contract_count!=artifact->module.function_count || !stored->contract_count ||
        !stored->contracts || !actual->contracts || stored->source || actual->source ||
        stored->origins || actual->origins || stored->count || actual->count ||
        stored->bindings || actual->bindings || stored->binding_count || actual->binding_count)
        return XR_XIR_BAD_STRUCTURE;
    /* This request reads the matched inventory. Copy-only prospective
     * allocation admission belongs to the actual clone, never to this read. */
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t f=0;f<stored->contract_count && status==XR_XIR_OK;++f)
        status=source_facts_contract_match(context,&stored->contracts[f],&actual->contracts[f]);
    if (status==XR_XIR_OK) status=source_facts_construction_match(context,&artifact->module,
        facts->construction,artifact->construction);
    return status;
}

/* The caller owns a live Artifact reference. The allocation containing this
 * owner itself retains the real heap ledger, so it cannot be destroyed and
 * reused while immutable facts remain live. A pointer comparison is only a
 * rejection boundary; input fields and the complete proof still decide use. */
static XrXirStatus source_facts_match(const XrXirCompileContext *context,
    const XrXirArtifact *artifact,XrXirDiagnostic *diagnostic) {
    if (!artifact) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=source_facts_input_match(context,artifact,artifact->source_facts);
    if (status==XR_XIR_OK) status=xr_xir_compile_verify_source_effects_v2(context,
        &artifact->module,artifact->construction,artifact->source_facts->effects,diagnostic);
    if (status==XR_XIR_OK) status=xir_effects_source_terminal(context,
        &artifact->module,artifact->source_facts->effects);
    if (status==XR_XIR_OK) status=xr_xir_compile_layout_match_verified(artifact);
    return status;
}

/* This entry is called only while the receiving Artifact is unpublished.
 * The fresh collector sees the actual final Checked body, not Built facts. */
static XrXirStatus source_facts_build(XrXirArtifact *artifact,XrXirDiagnostic *diagnostic) {
    if (!artifact || artifact->source_facts || artifact->module.stage!=XR_XIR_CHECKED ||
        !artifact->module.provenance || artifact->module.provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirCompileContext *context=&artifact->context;
    XrXirEffects *effects=NULL;
    XrXirStatus status=xr_xir_compile_verify_owned_effects_v2(context,&artifact->module,
        artifact->construction,&effects,diagnostic);
    XirSourceFacts *facts=status==XR_XIR_OK?xir_compile_calloc(context,1,sizeof(*facts),&status):NULL;
    if (status==XR_XIR_OK && !facts) status=XR_XIR_OUT_OF_MEMORY;
    if (facts && !xir_compile_work(context,
        sizeof(facts->context)+sizeof(facts->references)+sizeof(facts->effects))) {
        xr_compile_resources_free(facts);xr_xir_compile_effects_free(effects);
        return XR_XIR_BUDGET;
    }
    if (facts) {
        atomic_init(&facts->references,1);
        facts->context=*context;facts->effects=effects;effects=NULL;
        if (status==XR_XIR_OK) facts->input=provenance_clone(context,artifact->module.provenance,&status);
        if (status==XR_XIR_OK) status=xir_construction_clone(context,artifact->module.types,
            artifact->construction,&facts->construction);
        if (status==XR_XIR_OK) status=source_facts_input_match(context,artifact,facts);
        if (status==XR_XIR_OK) status=xir_effects_source_terminal(context,&artifact->module,facts->effects);
    }
    xr_xir_compile_effects_free(effects);
    if (status!=XR_XIR_OK) { xir_source_facts_release(facts);return status; }
    if (!xir_compile_work(context,sizeof(artifact->source_facts))) {
        xir_source_facts_release(facts);return XR_XIR_BUDGET;
    }
    artifact->source_facts=facts;return XR_XIR_OK;
}

/* Complete EOF and structure admission precede decoded facts. A partial
 * packet never publishes either its outer Artifact or its nested Source. */
static XrXirStatus source_facts_prepare(XrXirArtifact *artifact,XrXirDiagnostic *diagnostic) {
    if (!artifact || !xir_compile_context_valid(&artifact->context)) return XR_XIR_BAD_STRUCTURE;
    const XrXirProvenance *provenance=artifact->module.provenance;
    if (!provenance) return artifact->source_facts?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
    if (provenance->kind==XR_XIR_EVIDENCE_TEMPLATE) return source_facts_build(artifact,diagnostic);
    if (!xir_effect_kind_is_closed(provenance->kind) || !provenance->source || artifact->source_facts)
        return XR_XIR_BAD_STRUCTURE;
    XrXirArtifact *source=provenance->source;
    if (!source->module.provenance) return source->source_facts?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
    if (source->module.provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE) return XR_XIR_BAD_STRUCTURE;
    return source->source_facts?source_facts_match(&artifact->context,source,diagnostic):
        source_facts_build(source,diagnostic);
}

XR_FUNC XrXirStatus xir_source_facts_prepare_decoded(XrXirArtifact *artifact,
    XrXirDiagnostic *diagnostic) {
    return source_facts_prepare(artifact,diagnostic);
}

XR_FUNC XrXirStatus xir_source_facts_acquire(const XrXirArtifact *artifact,
    XirSourceFacts **output,XrXirDiagnostic *diagnostic) {
    if (!artifact || !output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=source_facts_match(&artifact->context,artifact,diagnostic);
    if (status==XR_XIR_OK) status=source_facts_retain(&artifact->context,artifact->source_facts,output);
    return status;
}

XR_FUNC const XrXirEffects *xir_source_facts_effects(const XirSourceFacts *facts) {
    return facts?facts->effects:NULL;
}

static XrXirArtifact *source_artifact_clone(const XrXirCompileContext *context,
    const XrXirArtifact *source,XrXirStatus *status) {
    if (!source || source->module.stage!=XR_XIR_CHECKED) { *status=XR_XIR_BAD_STAGE;return NULL; }
    XrXirArtifact *copy=clone_module(context,&source->module,source->construction,status);
    if (!copy) return NULL;
    if (source->module.provenance && source->module.provenance->kind==XR_XIR_EVIDENCE_TEMPLATE) {
        if (!source->source_facts) { *status=XR_XIR_BAD_STRUCTURE;goto failed; }
        if (source->context.resources==context->resources) {
            *status=source_facts_match(context,source,NULL);
            if (*status==XR_XIR_OK) *status=source_facts_input_match(context,copy,source->source_facts);
            if (*status==XR_XIR_OK) *status=source_facts_retain(context,source->source_facts,&copy->source_facts);
            if (*status==XR_XIR_OK) *status=source_facts_match(context,copy,NULL);
        } else {
            *status=source_facts_build(copy,NULL);
        }
    } else if (source->source_facts) *status=XR_XIR_BAD_STRUCTURE;
failed:
    if (*status!=XR_XIR_OK) { xr_xir_compile_artifact_free(copy);return NULL; }
    return copy;
}
