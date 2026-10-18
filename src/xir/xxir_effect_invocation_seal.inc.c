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
    EffectInvocationDeclaredBounds *declared;
    EffectInvocationFinal *final;
} EffectInvocationCertificate;

/* The builder reads its live derivation. A receiving request reads the actual
 * sealed certificate. These storage views carry claims, never admission. */
static bool effect_invocation_roles_shape(const EffectInvocationRead *read) {
    if (!read || !xir_compile_context_valid(read->work) || !read->final ||
        !effect_body_material_shape(read->final->material) ||
        read->final->material->resources!=read->work->resources ||
        !!read->roles.construction==!!read->roles.sealed) return false;
    if (read->roles.construction) {
        const XrXirEffects *construction=read->roles.construction;
        return xir_effects_context_matches(read->work,construction,effect_body_count(read->final->material)) &&
            !effect_final_outcomes(construction) && effect_contracts_read(construction);
    }
    const EffectInvocationCertificate *sealed=read->roles.sealed;
    return sealed->resources==read->work->resources && sealed->final==read->final &&
        sealed->body==read->final->material && sealed->declared==read->declared;
}
static XrXirStatus effect_invocation_role(const EffectInvocationRead *read,
    uint32_t function,uint32_t parameter,uint32_t *output) {
    if (!output || !effect_invocation_roles_shape(read) ||
        function>=effect_body_count(read->final->material) ||
        parameter>=read->final->material->functions[function].parameter_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t kind=UINT32_MAX;
    if (!xir_compile_work(read->work,6+sizeof(*output))) return XR_XIR_BUDGET;
    if (read->roles.construction) {
        const XrXirFunctionEffectContract *contract=&effect_contracts_read(read->roles.construction)[function];
        if (contract->parameter_count!=read->final->material->functions[function].parameter_count ||
            !contract->parameters) return XR_XIR_BAD_STRUCTURE;
        kind=contract->parameters[parameter].kind;
    } else {
        const EffectInvocationCertificate *sealed=read->roles.sealed;
        XrXirStatus status=effect_role_parameter_read(read->work,sealed->body,&read->final->role_proof,
            function,parameter,&kind);
        if (status!=XR_XIR_OK) return status;
    }
    if (kind>XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC) return XR_XIR_BAD_STRUCTURE;
    *output=kind;return XR_XIR_OK;
}

/* The receiving request borrows the actual Final. Its reverse index is fresh
 * verification scratch and never contains a solver queue or copied inventory. */
static XrXirStatus effect_invocation_read_open(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const EffectInvocationFinal *final,EffectInvocationRead *read) {
    if (!xir_compile_context_valid(work) || !certificate || !final || !read ||
        certificate->resources!=work->resources || certificate->final!=final ||
        certificate->body!=final->material || !certificate->declared ||
        !effect_body_material_shape(final->material) || final->material->resources!=work->resources ||
        read->work || read->consumers || read->final || read->discovery ||
        !final->physical || final->physical!=final->material->physical ||
        final->physical->resources!=work->resources ||
        final->physical->count!=effect_body_count(final->material) || !final->physical->functions ||
        !final->nodes || final->count>final->capacity || final->edge_count>final->edge_capacity ||
        (final->edge_count && !final->edges)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(*read))) return XR_XIR_BUDGET;
    *read=(EffectInvocationRead){.work=work,.roles={.sealed=certificate},.declared=certificate->declared,.final=final,
        .body=effect_body_owned_request(work,final->material)};
    read->producer_enabled=final->site_count!=0;
    for (uint32_t f=0;f<effect_body_count(final->material);++f) {
        read->class_enabled|=final->physical->functions[f].classes!=0 ||
            xr_xir_type_is_class(final->material->domain.types,final->material->functions[f].result);
    }
    if (!read->producer_enabled) return XR_XIR_OK;
    uint32_t count=final->count,edges=final->edge_count;
    uint32_t capacity=final->capacity,edge_capacity=final->edge_capacity;
    const EffectInvocationNode *nodes=final->nodes;const EffectInvocationEdge *links=final->edges;
    const EffectBodyMaterial *material=final->material;const EffectPhysicalDomain *physical=final->physical;
    const uint32_t *roots=final->roots,*root_spaces=final->root_spaces;
    if (!xir_compile_work(work,6*sizeof(void *)+4*sizeof(uint32_t))) return XR_XIR_BUDGET;
    if ((uint64_t)count*sizeof(*read->consumers)>SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    read->consumers=xir_compile_alloc(work,(size_t)count*sizeof(*read->consumers),&status);
    if (!read->consumers) return status;
    /* The allocator may change the descriptor. Frozen local extents qualify
     * only this request's allocation, never a callback's later dimensions. */
    if (!xir_compile_work(work,10)) status=XR_XIR_BUDGET;
    else if (final->count!=count || final->edge_count!=edges ||
        final->capacity!=capacity || final->edge_capacity!=edge_capacity ||
        final->nodes!=nodes || final->edges!=links || final->material!=material || final->physical!=physical ||
        final->roots!=roots || final->root_spaces!=root_spaces) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK && !xir_compile_work(work,(uint64_t)count*sizeof(*read->consumers)))
        status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) for (uint32_t n=0;n<count;++n) read->consumers[n]=UINT32_MAX;
    for (uint32_t e=0;e<edges && status==XR_XIR_OK;++e) {
        if (!xir_compile_work(work,5)) { status=XR_XIR_BUDGET;break; }
        const EffectInvocationEdge *edge=&final->edges[e];
        if (edge->caller>=count || edge->target>=count ||
            edge->consumer_next!=read->consumers[edge->target]) { status=XR_XIR_BAD_STRUCTURE;break; }
        read->consumers[edge->target]=e;
    }
    if (status!=XR_XIR_OK) {
        xr_compile_resources_free(read->consumers);read->consumers=NULL;
    }
    return status;
}
static void effect_invocation_read_close(EffectInvocationRead *read) {
    xr_compile_resources_free(read->consumers);read->consumers=NULL;
}


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
        const uint32_t *selected,EffectFinalFunctions *result,void *owner);
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
    const uint32_t *selected,EffectFinalFunctions *result,void *receiver);

static XrXirStatus effect_invocation_certificate_project(const XrXirCompileContext *work,
    EffectInvocationCertificate *certificate,EffectFinalFunctions *result,
    const EffectInvocationProjectionSink *sink);

typedef struct EffectInvocationSealRequest {
    EffectInvocationWorkspace **equations;
    EffectFinalFunctions **result;
    const EffectOrdinaryContexts *dense;
} EffectInvocationSealRequest;

static void effect_invocation_certificate_free(EffectInvocationCertificate *certificate) {
    if (!certificate) return;
    effect_invocation_final_free(certificate->final);
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
    const EffectInvocationWorkspace *owner) {
    EffectBodyMaterial *body=owner->final.material;
    if (!effect_body_material_shape(body) || body->resources!=certificate->resources ||
        owner->final.physical!=body->physical || owner->final.cells!=body->cells) return XR_XIR_BAD_STRUCTURE;
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

/* Origins and owner links come from the actual common context owner.
 * Role claims belong only to Final and are independently checked there. */
static XrXirStatus effect_invocation_copy_origins(EffectInvocationCertificate *certificate,
    const EffectInvocationWorkspace *equations,const EffectOrdinaryContexts *dense) {
    EffectTerms *terms=&certificate->terms;uint32_t count=effect_body_count(equations->final.material);
    if (dense && (dense->count!=count || &dense->uses!=equations->effects ||
        !equations->final.material->ordinary_identities))
        return XR_XIR_BAD_STRUCTURE;
    certificate->origins=effect_terms_alloc(terms,count,sizeof(*certificate->origins));
    certificate->owners=effect_terms_alloc(terms,count,sizeof(*certificate->owners));
    if (count && (!certificate->origins || !certificate->owners))
        return terms->status;
    for (uint32_t f=0;f<count;++f) {
        if (!xir_compile_work(terms->remaining,sizeof(XrXirOrigin)+sizeof(uint32_t)))
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
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_certificate_terms(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,
    const EffectInvocationTermTarget *target,uint32_t *count) {
    if (!xir_compile_context_valid(work) || !certificate || !certificate->final || !target || !count ||
        certificate->resources!=work->resources ||
        certificate->final->material!=certificate->body || !certificate->final ||
        certificate->final->summary.resources!=work->resources || !certificate->final->summary.functions ||
        certificate->final->summary.count!=effect_body_count(certificate->body) ||
        certificate->final->summary.capacity!=certificate->final->summary.count || function>=certificate->final->summary.count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectFinalFunction *record=&certificate->final->summary.functions[function];
    if (!certificate->final->roots || !certificate->final->nodes ||
        certificate->final->roots[function]>=certificate->final->count ||
        record->formula.term_count!=record->capacity || !!record->storage!=!!record->capacity ||
        record->formula.terms!=record->storage || target->limit!=record->capacity ||
        record->formula.constant_mask!=certificate->final->nodes[
            certificate->final->roots[function]].intrinsic_mask) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,6+sizeof(EffectInvocationTermSource))) return XR_XIR_BUDGET;
    const EffectInvocationTermSource source={work,certificate->final,
        &certificate->final->role_proof,NULL};
    uint32_t actual=0;
    XrXirStatus status=effect_invocation_result_terms(&source,function,NULL,record->storage,target,&actual);
    if (status==XR_XIR_OK && actual!=record->capacity) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) *count=actual;
    return status;
}

static XrXirStatus effect_invocation_local_result_shape(const XrXirCompileContext *work,
    const EffectInvocationFinal *owner,const EffectFinalFunctions *result) {
    if (!xir_compile_context_valid(work) || !owner || !result ||
        result->resources!=work->resources || !result->functions || !owner->material)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
    uint32_t count=effect_body_count(owner->material);
    return result->count==count && result->capacity==count && !result->term_count?
        XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus effect_invocation_local_result_row(const XrXirCompileContext *work,
    const EffectFinalFunctions *result,uint32_t f,const EffectInvocationLocalInput *input) {
    if (!xir_compile_work(work,8)) return XR_XIR_BUDGET;
    const EffectFinalFunction *actual=&result->functions[f];
    uint32_t mask=(input->facts.requires_root?XR_XIR_CALLABLE_ROOT_REQUIRED:0)|
        (input->facts.unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0);
    return actual->capacity || actual->storage || actual->formula.terms || actual->formula.term_count ||
        actual->formula.constant_mask!=mask?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
}

/* This independently derived relation has no formal or call terms. Actual
 * canonical result fields are still compared, including every empty owner. */
static XrXirStatus effect_invocation_local_result_match(const XrXirCompileContext *work,
    const EffectInvocationFinal *owner,const EffectInvocationLocalInput *inputs,
    const EffectFinalFunctions *result) {
    if (!inputs) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_invocation_local_result_shape(work,owner,result);
    if (status!=XR_XIR_OK) return status;
    for (uint32_t f=0;f<result->count;++f) {
        status=effect_invocation_local_result_row(work,result,f,&inputs[f]);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* A complete zero relation and its actual rows are checked in one uninterrupted
 * Input read. A local-layout mismatch matters only when every Input row proves
 * the relation empty; malformed Input and exhausted work always fail immediately.
 * No callback can invalidate an earlier comparison or a temporary stack value. */
static XrXirStatus effect_invocation_local_certificate_match(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const EffectFinalFunctions *result,bool *local) {
    if (!xir_compile_context_valid(work) || !certificate ||
        !local || !effect_body_material_shape(certificate->body) || certificate->body->resources!=work->resources)
        return XR_XIR_BAD_STRUCTURE;
    *local=false;
    const EffectBodyMaterial *body=certificate->body;
    uint32_t count=effect_body_count(body);
    EffectInvocationLocalRead read={work,body,0};
    XrXirStatus proof=XR_XIR_OK;
    for (uint32_t f=0;f<count;++f) {
        EffectInvocationLocalInput input;bool empty=false;
        XrXirStatus status=effect_invocation_local_head(work,body,f,&empty);
        if (status==XR_XIR_OK && empty)
            status=effect_invocation_local_input_body(&read,f,&input,&empty);
        if (status!=XR_XIR_OK || !empty) return status;
        if (!f) {
            proof=effect_invocation_local_equations_shape(work,certificate->final);
            if (proof==XR_XIR_OK)
                proof=effect_invocation_local_result_shape(work,certificate->final,result);
        }
        if (proof==XR_XIR_OK)
            proof=effect_invocation_local_node_match(work,certificate->final,f,&input);
        if (proof==XR_XIR_OK) proof=effect_invocation_local_result_row(work,result,f,&input);
        if (proof!=XR_XIR_OK && proof!=XR_XIR_BAD_STRUCTURE) return proof;
    }
    if (proof==XR_XIR_OK) *local=true;
    return proof;
}

/* Derive primitive contributions from current Input, not its stored seed or
 * a receiving local row. Dynamic calls remain obligations of the complete
 * invocation relation. No equation generator supplies an expected result. */
static XrXirStatus effect_invocation_input_seed(const XrXirCompileContext *work,
    const EffectBodyMaterial *body,uint32_t function,XrXirRootEffects *output) {
    if(!output) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationPrimitive primitive={0};
    XrXirStatus status=effect_invocation_primitive_read(work,body,function,&primitive);
    if(status!=XR_XIR_OK) return status;
    if(!xir_compile_work(work,sizeof(*output))) return XR_XIR_BUDGET;
    *output=primitive.facts;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_input_seeds_match(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const EffectFinalFunctions *summary) {
    const EffectInvocationFinal *final=certificate->final;
    uint32_t count=effect_body_count(certificate->body);
    if (!final || !summary || summary->count!=count || summary->capacity!=count ||
        !summary->functions || !final->roots || !final->nodes || !final->count || final->count>final->capacity)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<count;++f) {
        if (!xir_compile_work(work,6)) return XR_XIR_BUDGET;
        if (final->roots[f]>=final->count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationNode *node=&final->nodes[final->roots[f]];
        if (node->body!=f) return XR_XIR_BAD_STRUCTURE;
        XrXirRootEffects input={0};XrXirStatus status=effect_invocation_input_seed(work,certificate->body,f,&input);
        if (status!=XR_XIR_OK) return status;
        uint32_t mask=(input.requires_root?XR_XIR_CALLABLE_ROOT_REQUIRED:0)|
            (input.unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0);
        if ((node->intrinsic_mask&mask)!=mask || (node->local_mask&mask)!=mask ||
            (summary->functions[f].formula.constant_mask&mask)!=mask)
            return XR_XIR_BAD_STRUCTURE;
    }
    /* Each genuine environment executes the same body's primitive Input.
     * An intact symbolic root cannot qualify a different erased call key. */
    for (uint32_t n=0;n<final->count;++n) {
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        const EffectInvocationNode *node=&final->nodes[n];
        if (node->body>=count) return XR_XIR_BAD_STRUCTURE;
        if (final->roots[node->body]==n) continue;
        XrXirRootEffects input={0};
        XrXirStatus status=effect_invocation_input_seed(work,certificate->body,node->body,&input);
        if (status!=XR_XIR_OK) return status;
        uint32_t mask=(input.requires_root?XR_XIR_CALLABLE_ROOT_REQUIRED:0)|
            (input.unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0);
        if ((node->intrinsic_mask&mask)!=mask || (node->local_mask&mask)!=mask)
            return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

/* All failure paths keep both input owners and the occupied output intact.
 * Ownership moves only after complete copied correspondence and closure proof;
 * construction-only pointers are then removed from the published certificate. */
static XrXirStatus effect_invocation_seal(const XrXirCompileContext *work,
    const EffectInvocationSealRequest *request,EffectInvocationCertificate **output,
    const EffectInvocationProjectionSink *sink) {
    if (!xir_compile_context_valid(work) || !request || !output || *output || !request->equations ||
        !*request->equations || !request->result || !*request->result) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationWorkspace *owner=*request->equations;EffectFinalFunctions *result=*request->result;
    EffectFinalFunctions *staged=result;bool summary_moved=false;
    if (!effect_body_request_owned_matches(owner->read.work,&owner->read.body,owner->final.material) ||
        owner->read.work->resources!=work->resources || result->resources!=work->resources ||
        result->count!=effect_body_count(owner->final.material) ||
        !effect_invocation_projection_sink_matches(work,sink,effect_body_count(owner->final.material)))
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_body_physical_match(work,owner->final.physical,owner->final.material);
    EffectInvocationLocalInput *local_inputs=NULL;
    if (status==XR_XIR_OK) status=effect_invocation_local_inputs(work,owner->final.material,&local_inputs);
    if (status==XR_XIR_OK && local_inputs) {
        if (!owner->read.consumers || !owner->states || owner->pending || owner->front || owner->back ||
            owner->read.producer_enabled || owner->read.class_enabled || owner->edge_index || owner->edge_index_capacity ||
            owner->node_index || owner->node_index_capacity)
            status=XR_XIR_BAD_STRUCTURE;
        for (uint32_t f=0;status==XR_XIR_OK && f<owner->final.count;++f) {
            if (!xir_compile_work(work,3)) status=XR_XIR_BUDGET;
            else if (owner->read.consumers[f]!=UINT32_MAX || !owner->states[f].expanded || owner->states[f].queued)
                status=XR_XIR_BAD_STRUCTURE;
        }
        if (status==XR_XIR_OK) status=effect_invocation_local_equations_match(work,&owner->final,local_inputs);
        if (status==XR_XIR_OK) status=effect_invocation_local_result_match(work,&owner->final,local_inputs,result);
    } else if (status==XR_XIR_OK) {
        status=effect_invocation_producers_verify(owner);
        if (status==XR_XIR_OK) status=effect_invocation_class_verify(owner);
        if (status==XR_XIR_OK) status=effect_invocation_closed(owner);
        if (status==XR_XIR_OK) status=effect_invocation_deferred_verify(&owner->read);
        if (status==XR_XIR_OK) status=effect_invocation_result_match(owner,result);
        if (status==XR_XIR_OK) status=effect_invocation_causes(owner);
    }
    xr_compile_resources_free(local_inputs);
    EffectInvocationCertificate *certificate=status==XR_XIR_OK ?
        xir_compile_calloc(work,1,sizeof(*certificate),&status):NULL;
    if (status==XR_XIR_OK && !certificate) status=XR_XIR_OUT_OF_MEMORY;
    if (status==XR_XIR_OK) {
        certificate->resources=work->resources;certificate->terms.remaining=work;
        status=effect_invocation_bind_bodies(certificate,owner);
    }
    if (status==XR_XIR_OK) status=effect_invocation_bind_bounds(certificate,owner->read.declared,owner->final.physical);
    if (status==XR_XIR_OK) status=effect_invocation_copy_origins(certificate,owner,request->dense);
    if (status==XR_XIR_OK) status=effect_body_owned_types_verify(work,certificate->body);
    if (status==XR_XIR_OK) {
        /* Budget failure may enter cleanup before copied ownership is installed.
         * Publish only initialized spans while charging their actual clearing. */
        certificate->final=xir_compile_calloc(work,1,sizeof(*certificate->final),&status);
        if (status==XR_XIR_OK) *certificate->final=owner->final;
        if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(certificate->final->summary)+2*sizeof(void *)))
            status=XR_XIR_BUDGET;
        if (status==XR_XIR_OK) {
            certificate->final->summary=*staged;
            result=&certificate->final->summary;*request->result=result;summary_moved=true;
        }
        if (status==XR_XIR_OK) status=certificate->final->role_proof.resources ?
            effect_role_proof_match(work,certificate->body,&certificate->final->role_proof) :
            effect_role_proof_build(work,certificate->body,&certificate->final->role_proof);
        if (status==XR_XIR_OK) status=effect_role_contracts_match(work,certificate->body,
            &certificate->final->role_proof,effect_contracts_read(owner->effects),owner->effects->count);
        if (status==XR_XIR_OK) effect_invocation_discovery_clear(owner);
        if (status==XR_XIR_OK) {
            status=certificate->final->capture.facts ? effect_capture_match(work,certificate->final) :
                effect_capture_build(work,certificate->final,owner);
            effect_invocation_class_discovery_clear(owner);
        }
        if (status==XR_XIR_OK) status=effect_invocation_certificate_project(work,certificate,result,sink);
    }
    if (status==XR_XIR_OK && !xir_compile_work(work,9+sizeof(*request->result)+
        sizeof(owner->edge_index)+sizeof(owner->edge_index_capacity))) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK && (!owner->graph || owner->graph->material!=certificate->body ||
        (!owner->graph->physical_owned && owner->effects->generation!=certificate->body)))
        status=XR_XIR_BAD_STRUCTURE;
    if (status!=XR_XIR_OK) {
        if (summary_moved) { *staged=*result;*request->result=staged; }
        if (certificate) {
            if (certificate->final) {
                /* Copied proof pointers remain borrowed until publication.
                 * Rollback releases only children created by this receiver. */
                if (certificate->final->capture.facts!=owner->final.capture.facts)
                    xr_compile_resources_free(certificate->final->capture.facts);
                EffectRoleProof *proof=&certificate->final->role_proof;
                if (proof->rows==owner->final.role_proof.rows) proof->rows=NULL;
                if (proof->facts==owner->final.role_proof.facts) proof->facts=NULL;
                effect_role_proof_clear(proof);
                *certificate->final=(EffectInvocationFinal){0};
            }
            certificate->body=NULL;
        }
        effect_invocation_certificate_free(certificate);return status;
    }
    owner->graph->physical_owned=false;
    if (owner->effects->generation==certificate->body) owner->effects->generation=NULL;
    certificate->terms.remaining=NULL;certificate->body->terms.remaining=NULL;
    /* Move the exact inventory, then destroy every solver allocation. Readers
     * retain only Final; later proof requests own independent temporary scratch. */
    owner->final=(EffectInvocationFinal){0};
    effect_invocation_free(owner);
    xr_compile_resources_free(staged);
    *request->result=NULL;
    *request->equations=NULL;*output=certificate;return XR_XIR_OK;
}

/* Replaying an immutable certificate uses the same constructor and role
 * inventory. This helper does not publish or retain another result in it. */
static inline XrXirStatus effect_invocation_result_read_new(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,EffectFinalFunctions **output) {
    if (!xir_compile_context_valid(work) || !certificate || !certificate->final ||
        certificate->resources!=work->resources ||
        certificate->final->material!=certificate->body || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(EffectInvocationTermSource))) return XR_XIR_BUDGET;
    const EffectInvocationTermSource source={work,certificate->final,
        &certificate->final->role_proof,NULL};
    return effect_invocation_result_new(&source,output);
}

static XrXirStatus effect_invocation_certificate_result_match(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const EffectFinalFunctions *result) {
    if (!xir_compile_context_valid(work) || !certificate || !certificate->final ||
        certificate->resources!=work->resources ||
        certificate->final->material!=certificate->body) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(EffectInvocationTermSource))) return XR_XIR_BUDGET;
    const EffectInvocationTermSource source={work,certificate->final,
        &certificate->final->role_proof,NULL};
    bool local=false;
    XrXirStatus status=effect_invocation_local_certificate_match(work,certificate,result,&local);
    if (status==XR_XIR_OK && !local) status=effect_invocation_input_seeds_match(work,certificate,result);
    return status==XR_XIR_OK?effect_invocation_result_match_source(&source,result):status;
}
