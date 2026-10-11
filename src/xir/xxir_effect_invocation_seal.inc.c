/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_seal.inc.c - Independent owned invocation and typed origin certificates
 *
 * KEY CONCEPT:
 *   The certificate owns its private body correspondence and complete inputs.
 *   This copy is evidence metadata, never an independently admitted executable.
 */

typedef struct EffectInvocationCertificate {
    XrCompileResources *resources;
    EffectTerms terms;
    EffectBodyMaterial *body;
    XrXirDeclarations *declarations;
    XrXirGeneric *generics;
    XrXirDefaultTable defaults;
    XrXirOrigin *origins;
    uint32_t *owners;
    uint8_t **parameter_kinds;
    EffectInvocationDeclaredBounds *declared;
    EffectInvocationOwner *equations;
    EffectInvocationResult *result;
    uint32_t *projection_atoms;
} EffectInvocationCertificate;

_Static_assert(sizeof(EffectInvocationDeclaredBounds)%_Alignof(EffectInvocationFunctionBounds)==0 &&
    sizeof(EffectInvocationFunctionBounds)%_Alignof(EffectInvocationValueBound)==0,
    "Certificate bounds preserve descriptor and value alignment");

/* Both outputs consume this call's fresh selected values. The receiver is
 * private parent scratch, never a saved authorization or a public output. */
typedef struct EffectInvocationProjectedCause {
    uint32_t atom;
    const XrXirRootEffectWitness *witness;
} EffectInvocationProjectedCause;

typedef struct EffectInvocationProjectionSink {
    XrCompileResources *resources;
    uint32_t count;
    void *owner;
    XrXirStatus (*write)(const XrXirCompileContext *work,
        const EffectInvocationCertificate *certificate,uint32_t function,
        const uint32_t *selected,EffectInvocationResult *result,void *owner);
} EffectInvocationProjectionSink;

static bool effect_invocation_projection_sink_matches(const XrXirCompileContext *work,
    const EffectInvocationProjectionSink *sink,uint32_t count) {
    return !sink || (sink->resources==work->resources && sink->count==count && sink->owner && sink->write);
}

static XrXirStatus effect_invocation_public_transform(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,
    const EffectInvocationProjectedCause *selection,XrXirRootEffectWitness *output,bool *symbolic);

static XrXirStatus effect_invocation_publication_write(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,
    const uint32_t *selected,EffectInvocationResult *result,void *receiver);

static XrXirStatus effect_invocation_certificate_project(const XrXirCompileContext *work,
    EffectInvocationCertificate *certificate,EffectInvocationResult *result,
    const EffectInvocationProjectionSink *sink);

typedef struct EffectInvocationSealRequest {
    EffectInvocationOwner **equations;
    EffectInvocationResult **result;
    const EffectOrdinaryContexts *dense;
} EffectInvocationSealRequest;

static void effect_invocation_certificate_free(EffectInvocationCertificate *certificate) {
    if (!certificate) return;
    effect_invocation_result_free(certificate->result);
    effect_invocation_free(certificate->equations);
    /* Original bounds are a child of the sole transferred material. */
    effect_body_material_free(certificate->body);
    effect_terms_free(&certificate->terms);
    xr_compile_resources_free(certificate);
}

static void *effect_invocation_copy(EffectTerms *terms,const void *source,
    uint64_t count,size_t size) {
    if (!count || terms->status!=XR_XIR_OK) return NULL;
    if (!source) { terms->status=XR_XIR_BAD_STRUCTURE;return NULL; }
    void *copy=effect_terms_reserve(terms,count,size);
    if (!copy) return NULL;
    if (!xir_compile_work(terms->remaining,count*size)) { terms->status=XR_XIR_BUDGET;return NULL; }
    memcpy(copy,source,(size_t)count*size);return copy;
}

/* This generation already owns every body field. Seal borrows it while
 * all independent proofs run and transfers that same owner only at commit. */
static XrXirStatus effect_invocation_bind_bodies(EffectInvocationCertificate *certificate,
    const EffectInvocationOwner *owner) {
    EffectBodyMaterial *body=owner->material;
    if (!effect_body_material_shape(body) || body->resources!=certificate->resources ||
        owner->physical!=body->physical || owner->cells!=body->cells) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(certificate->terms.remaining,5)) return XR_XIR_BUDGET;
    certificate->body=body;certificate->terms.types=body->terms.types;
    certificate->terms.capacity=body->terms.capacity;
    certificate->declarations=body->declarations;certificate->generics=body->generics;
    certificate->defaults=body->defaults;return XR_XIR_OK;
}

/* Bounds were copied or written before the first equation consumes them.
 * Their sorted complete physical membership remains independently checked
 * at seal; this boundary does not allocate or copy another bound owner. */
static XrXirStatus effect_invocation_bind_bounds(EffectInvocationCertificate *certificate,
    const EffectInvocationDeclaredBounds *source,const EffectPhysicalDomain *domain) {
    const XrXirCompileContext *work=certificate->terms.remaining;
    if (!effect_body_material_shape(certificate->body) || certificate->body->declared!=source ||
        !source || source->resources!=work->resources || source->count!=effect_body_count(certificate->body) ||
        source->count>source->capacity || (source->count && !source->functions) || !domain ||
        domain->resources!=work->resources || domain->count!=source->count ||
        (domain->count && !domain->functions)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<source->count;++f) {
        if (!xir_compile_work(work,10)) return XR_XIR_BUDGET;
        const EffectBodyFunction *function=&certificate->body->functions[f];
        const EffectInvocationFunctionBounds *body=&source->functions[f];
        const EffectPhysicalFunction *physical=&domain->functions[f];
        uint64_t values=(uint64_t)function->parameter_count+function->instruction_count;
        if (physical->parameters!=function->parameter_count || physical->instructions!=function->instruction_count ||
            physical->values!=values || body->values!=values || physical->rows!=body->count ||
            body->count>values || !!body->bounds!=!!body->count || physical->count>physical->capacity ||
            physical->capacity>values || !!physical->records!=!!physical->capacity) return XR_XIR_BAD_STRUCTURE;
        uint32_t at=0,previous=0;
        for (uint32_t r=0;r<physical->count;++r) {
            if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
            EffectPhysicalRecord record=physical->records[r];
            if (!record.mask || record.value>=values || (r && record.value<=previous)) return XR_XIR_BAD_STRUCTURE;
            previous=record.value;
            if (!(record.mask&EFFECT_PHYSICAL_CALLABLE)) continue;
            if (at>=body->count) return XR_XIR_BAD_STRUCTURE;
            const EffectInvocationValueBound *bound=&body->bounds[at++];
            if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
            if (bound->value!=record.value || !bound->callable || bound->mask&~
                (XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED)) return XR_XIR_BAD_STRUCTURE;
        }
        if (at!=body->count) return XR_XIR_BAD_STRUCTURE;
    }
    if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
    certificate->declared=certificate->body->declared;return XR_XIR_OK;
}

/* Origins, owner links and parameter-use kinds come from this actual common
 * context owner. There is no reconstructed role table or permission flag. */
static XrXirStatus effect_invocation_copy_origins(EffectInvocationCertificate *certificate,
    const EffectInvocationOwner *equations,const EffectOrdinaryContexts *dense) {
    EffectTerms *terms=&certificate->terms;uint32_t count=equations->module->function_count;
    if (dense && (dense->count!=count || dense->functions!=equations->module->functions ||
        &dense->terms.types!=equations->module->types || &dense->uses!=equations->effects))
        return XR_XIR_BAD_STRUCTURE;
    certificate->origins=effect_terms_alloc(terms,count,sizeof(*certificate->origins));
    certificate->owners=effect_terms_alloc(terms,count,sizeof(*certificate->owners));
    certificate->parameter_kinds=effect_terms_alloc(terms,count,sizeof(*certificate->parameter_kinds));
    if (count && (!certificate->origins || !certificate->owners || !certificate->parameter_kinds))
        return terms->status;
    for (uint32_t f=0;f<count;++f) {
        if (!xir_compile_work(terms->remaining,sizeof(XrXirOrigin)+sizeof(uint32_t)+sizeof(void *)))
            return XR_XIR_BUDGET;
        XrXirOrigin origin={.function=f};uint32_t parent=UINT32_MAX;
        if (dense) {
            EffectOrdinaryNode node=dense->nodes[f];origin.function=node.declaration;
            origin.argument_count=node.argument_count;
            origin.arguments=effect_invocation_copy(terms,node.arguments,node.argument_count,sizeof(*node.arguments));
            parent=node.owner;
        } else if (certificate->declarations && certificate->declarations->functions[f].cleanup_owner)
            parent=certificate->declarations->functions[f].cleanup_owner-1;
        if (terms->status!=XR_XIR_OK || (parent!=UINT32_MAX && parent>=count))
            return terms->status!=XR_XIR_OK?terms->status:XR_XIR_BAD_STRUCTURE;
        certificate->origins[f]=origin;certificate->owners[f]=parent;
        const XrXirFunctionEffectContract *contract=&equations->effects->contracts[f];
        uint32_t parameters=certificate->body->functions[f].parameter_count;
        if (contract->parameter_count!=parameters || (parameters && !contract->parameters)) return XR_XIR_BAD_STRUCTURE;
        uint8_t *kinds=effect_terms_alloc(terms,parameters,sizeof(*kinds));
        if (parameters && !kinds) return terms->status;
        certificate->parameter_kinds[f]=kinds;
        for (uint32_t p=0;p<parameters;++p) {
            if (!xir_compile_work(terms->remaining,2)) return XR_XIR_BUDGET;
            if (contract->parameters[p].kind>XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC) return XR_XIR_BAD_STRUCTURE;
            kinds[p]=(uint8_t)contract->parameters[p].kind;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_certificate_terms(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,
    const EffectInvocationTermTarget *target,uint32_t *count) {
    if (!xir_compile_context_valid(work) || !certificate || !certificate->equations || !target || !count ||
        certificate->resources!=work->resources || !certificate->parameter_kinds ||
        certificate->equations->material!=certificate->body || !certificate->result ||
        certificate->result->resources!=work->resources || !certificate->result->functions ||
        certificate->result->count!=effect_body_count(certificate->body) ||
        certificate->result->capacity!=certificate->result->count || function>=certificate->result->count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationResultFunction *record=&certificate->result->functions[function];
    if (!certificate->equations->roots || !certificate->equations->nodes ||
        certificate->equations->roots[function]>=certificate->equations->count ||
        record->formula.term_count!=record->capacity || !!record->storage!=!!record->capacity ||
        record->formula.terms!=record->storage || target->limit!=record->capacity ||
        record->formula.constant_mask!=certificate->equations->nodes[
            certificate->equations->roots[function]].intrinsic_mask) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,6+sizeof(EffectInvocationTermSource))) return XR_XIR_BUDGET;
    const EffectInvocationTermSource source={work,certificate->equations,
        (const uint8_t *const *)certificate->parameter_kinds};
    uint32_t actual=0;
    XrXirStatus status=effect_invocation_result_terms(&source,function,NULL,record->storage,target,&actual);
    if (status==XR_XIR_OK && actual!=record->capacity) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) *count=actual;
    return status;
}

/* All failure paths keep both input owners and the occupied output intact.
 * Ownership moves only after complete copied correspondence and closure proof;
 * construction-only pointers are then removed from the published certificate. */
static XrXirStatus effect_invocation_seal(const XrXirCompileContext *work,
    const EffectInvocationSealRequest *request,EffectInvocationCertificate **output,
    const EffectInvocationProjectionSink *sink) {
    if (!xir_compile_context_valid(work) || !request || !output || *output || !request->equations ||
        !*request->equations || !request->result || !*request->result) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationOwner *owner=*request->equations;EffectInvocationResult *result=*request->result;
    if (!xir_compile_context_valid(owner->work) || !owner->module || owner->work->resources!=work->resources || result->resources!=work->resources ||
        result->count!=owner->module->function_count ||
        !effect_invocation_projection_sink_matches(work,sink,owner->module->function_count))
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_physical_match(work,owner->physical,owner->module);
    if (status==XR_XIR_OK) status=effect_invocation_producers_verify(owner);
    if (status==XR_XIR_OK) status=effect_invocation_coverage_verify(owner);
    if (status==XR_XIR_OK) status=effect_invocation_class_verify(owner);
    if (status==XR_XIR_OK) status=effect_invocation_closed(owner);
    if (status==XR_XIR_OK) status=effect_invocation_deferred_verify(owner);
    if (status==XR_XIR_OK) status=effect_invocation_result_match(owner,result);
    if (status==XR_XIR_OK) status=effect_invocation_causes(owner);
    EffectInvocationCertificate *certificate=status==XR_XIR_OK ?
        xir_compile_calloc(work,1,sizeof(*certificate),&status):NULL;
    if (status==XR_XIR_OK && !certificate) status=XR_XIR_OUT_OF_MEMORY;
    if (status==XR_XIR_OK) {
        certificate->resources=work->resources;certificate->terms.remaining=work;
        status=effect_invocation_bind_bodies(certificate,owner);
    }
    if (status==XR_XIR_OK) status=effect_invocation_bind_bounds(certificate,owner->declared,owner->physical);
    if (status==XR_XIR_OK) status=effect_invocation_copy_origins(certificate,owner,request->dense);
    if (status==XR_XIR_OK) status=effect_body_type_domain_match(work,certificate->body,owner->module);
    if (status==XR_XIR_OK) {
        certificate->equations=owner;
        if (!xir_compile_work(work,sizeof(certificate->result))) status=XR_XIR_BUDGET;
        if (status==XR_XIR_OK) certificate->result=result;
        if (status==XR_XIR_OK) status=effect_invocation_certificate_project(work,certificate,result,sink);
    }
    if (status==XR_XIR_OK && !xir_compile_work(work,9+sizeof(*request->result)+
        sizeof(owner->edge_index)+sizeof(owner->edge_index_capacity))) status=XR_XIR_BUDGET;
    if (status!=XR_XIR_OK) {
        if (certificate) { certificate->equations=NULL;certificate->body=NULL;certificate->result=NULL; }
        effect_invocation_certificate_free(certificate);return status;
    }
    if (!owner->graph || owner->graph->material!=certificate->body ||
        (!owner->graph->physical_owned && owner->effects->generation!=certificate->body)) {
        certificate->equations=NULL;certificate->body=NULL;certificate->result=NULL;
        effect_invocation_certificate_free(certificate);return XR_XIR_BAD_STRUCTURE;
    }
    owner->graph->physical_owned=false;
    if (owner->effects->generation==certificate->body) owner->effects->generation=NULL;
    certificate->terms.remaining=NULL;certificate->body->terms.remaining=NULL;
    xr_compile_resources_free(owner->edge_index);
    owner->edge_index=NULL;owner->edge_index_capacity=0;
    owner->body.read=(EffectBodyRead){0};
    owner->work=NULL;owner->module=NULL;owner->effects=NULL;owner->graph=NULL;owner->declared=NULL;
    *request->result=NULL;
    *request->equations=NULL;*output=certificate;return XR_XIR_OK;
}

/* Replaying an immutable certificate uses the same constructor and role
 * inventory. This helper does not publish or retain another result in it. */
static inline XrXirStatus effect_invocation_result_read_new(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,EffectInvocationResult **output) {
    if (!xir_compile_context_valid(work) || !certificate || !certificate->equations ||
        certificate->resources!=work->resources || !certificate->parameter_kinds ||
        certificate->equations->material!=certificate->body || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(EffectInvocationTermSource))) return XR_XIR_BUDGET;
    const EffectInvocationTermSource source={work,certificate->equations,
        (const uint8_t *const *)certificate->parameter_kinds};
    return effect_invocation_result_new(&source,output);
}

static XrXirStatus effect_invocation_certificate_result_match(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const EffectInvocationResult *result) {
    if (!xir_compile_context_valid(work) || !certificate || !certificate->equations ||
        certificate->resources!=work->resources || !certificate->parameter_kinds ||
        certificate->equations->material!=certificate->body) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(EffectInvocationTermSource))) return XR_XIR_BUDGET;
    const EffectInvocationTermSource source={work,certificate->equations,
        (const uint8_t *const *)certificate->parameter_kinds};
    return effect_invocation_result_match_source(&source,result);
}
