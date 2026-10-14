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
    EffectInvocationFinal *final;
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

/* Origins, owner links and parameter-use kinds come from this actual common
 * context owner. There is no reconstructed role table or permission flag. */
static XrXirStatus effect_invocation_copy_origins(EffectInvocationCertificate *certificate,
    const EffectInvocationWorkspace *equations,const EffectOrdinaryContexts *dense) {
    EffectTerms *terms=&certificate->terms;uint32_t count=effect_body_count(equations->final.material);
    if (dense && (dense->count!=count || &dense->uses!=equations->effects ||
        !equations->final.material->ordinary_identities))
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
    if (!xir_compile_context_valid(work) || !certificate || !certificate->final || !target || !count ||
        certificate->resources!=work->resources || !certificate->parameter_kinds ||
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
        (const uint8_t *const *)certificate->parameter_kinds,NULL};
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
    if (!xir_compile_context_valid(work) || !effect_body_material_shape(body) || !output ||
        body->resources!=work->resources || function>=effect_body_count(body)) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *head=&body->functions[function];
    const XrXirDeclarations *decls=body->declarations;
    XrXirRootEffects facts={0};XrXirRootEffectWitness witnesses[2]={{0},{0}};
    if (decls) {
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        if (!decls->modules || !decls->functions || decls->functions[function].module>=decls->module_count)
            return XR_XIR_BAD_STRUCTURE;
        if (decls->modules[decls->functions[function].module].initializer==function) {
            facts.requires_root=true;
            witnesses[0]=(XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INITIALIZER,
                UINT32_MAX,UINT32_MAX,UINT32_MAX,0};
        }
    }
    EffectBodyRead read=effect_body_compile_read(work);
    for (uint32_t i=0;i<head->instruction_count;++i) {
        XrXirInstruction op;XrXirStatus status=effect_body_instruction_get(&read,head,i,&op);
        if (status!=XR_XIR_OK) return status;
        if (op.op==XR_XIR_CALL_INDIRECT || op.op==XR_XIR_INVOKE_INDIRECT) continue;
        if (op.op==XR_XIR_SLOT_LOAD || op.op==XR_XIR_SLOT_PLACE || op.op==XR_XIR_SLOT_STORE) {
            if (!decls || !decls->slots || op.immediate<0 || (uint64_t)op.immediate>=decls->slot_count)
                return XR_XIR_BAD_STRUCTURE;
        }
        const EffectRootSeedRecord request={&body->domain,&op,function,i,XR_XIR_UNIT};
        status=effect_root_seed_output(&request,&facts,&witnesses[0],&witnesses[1],work);
        if (status!=XR_XIR_OK) return status;
        if (op.op==XR_XIR_CALL_REQUIREMENT) { facts.unresolved=false;witnesses[1]=(XrXirRootEffectWitness){0}; }
    }
    if (!xir_compile_work(work,sizeof(*output))) return XR_XIR_BUDGET;
    *output=facts;return XR_XIR_OK;
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
        if ((node->intrinsic_mask&mask)!=mask || (summary->functions[f].formula.constant_mask&mask)!=mask)
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
    if (!effect_body_request_owned_matches(owner->work,&owner->body,owner->final.material) ||
        owner->work->resources!=work->resources || result->resources!=work->resources ||
        result->count!=effect_body_count(owner->final.material) ||
        !effect_invocation_projection_sink_matches(work,sink,effect_body_count(owner->final.material)))
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_body_physical_match(work,owner->final.physical,owner->final.material);
    EffectInvocationLocalInput *local_inputs=NULL;
    if (status==XR_XIR_OK) status=effect_invocation_local_inputs(work,owner->final.material,&local_inputs);
    if (status==XR_XIR_OK && local_inputs) {
        if (!owner->consumers || owner->pending || owner->front || owner->back ||
            owner->producer_enabled || owner->class_enabled || owner->edge_index || owner->edge_index_capacity)
            status=XR_XIR_BAD_STRUCTURE;
        for (uint32_t f=0;status==XR_XIR_OK && f<owner->final.count;++f)
            if (owner->consumers[f]!=UINT32_MAX) status=XR_XIR_BAD_STRUCTURE;
        if (status==XR_XIR_OK) status=effect_invocation_local_equations_match(work,&owner->final,local_inputs);
        if (status==XR_XIR_OK) status=effect_invocation_local_result_match(work,&owner->final,local_inputs,result);
    } else if (status==XR_XIR_OK) {
        status=effect_invocation_producers_verify(owner);
        if (status==XR_XIR_OK) status=effect_invocation_coverage_verify(owner);
        if (status==XR_XIR_OK) status=effect_invocation_class_verify(owner);
        if (status==XR_XIR_OK) status=effect_invocation_closed(owner);
        if (status==XR_XIR_OK) status=effect_invocation_deferred_verify(owner);
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
    if (status==XR_XIR_OK) status=effect_invocation_bind_bounds(certificate,owner->declared,owner->final.physical);
    if (status==XR_XIR_OK) status=effect_invocation_copy_origins(certificate,owner,request->dense);
    if (status==XR_XIR_OK) status=effect_body_owned_types_verify(work,certificate->body);
    if (status==XR_XIR_OK) {
        certificate->final=xir_compile_alloc(work,sizeof(*certificate->final),&status);
        if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(*certificate->final))) status=XR_XIR_BUDGET;
        if (status==XR_XIR_OK) *certificate->final=owner->final;
        if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(certificate->final->summary)+2*sizeof(void *)))
            status=XR_XIR_BUDGET;
        if (status==XR_XIR_OK) {
            certificate->final->summary=*staged;
            result=&certificate->final->summary;*request->result=result;summary_moved=true;
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
            if (certificate->final) *certificate->final=(EffectInvocationFinal){0};
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
        certificate->resources!=work->resources || !certificate->parameter_kinds ||
        certificate->final->material!=certificate->body || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(EffectInvocationTermSource))) return XR_XIR_BUDGET;
    const EffectInvocationTermSource source={work,certificate->final,
        (const uint8_t *const *)certificate->parameter_kinds,NULL};
    return effect_invocation_result_new(&source,output);
}

static XrXirStatus effect_invocation_certificate_result_match(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const EffectFinalFunctions *result) {
    if (!xir_compile_context_valid(work) || !certificate || !certificate->final ||
        certificate->resources!=work->resources || !certificate->parameter_kinds ||
        certificate->final->material!=certificate->body) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(EffectInvocationTermSource))) return XR_XIR_BUDGET;
    const EffectInvocationTermSource source={work,certificate->final,
        (const uint8_t *const *)certificate->parameter_kinds,NULL};
    bool local=false;
    XrXirStatus status=effect_invocation_local_certificate_match(work,certificate,result,&local);
    if (status==XR_XIR_OK && !local) status=effect_invocation_input_seeds_match(work,certificate,result);
    return status==XR_XIR_OK?effect_invocation_result_match_source(&source,result):status;
}
