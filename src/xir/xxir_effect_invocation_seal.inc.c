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
#include "xxir_effect_lowered_snapshot.inc.c"
#include "xxir_effect_invocation_declarations.inc.c"

typedef struct EffectInvocationCertificate {
    XrCompileResources *resources;
    EffectTerms terms;
    XrXirTypes *lowered_types;
    XrXirModule bodies;
    XrXirDeclarations *declarations;
    XrXirGeneric *generics;
    XrXirDefaultTable defaults;
    XrXirOrigin *origins;
    uint32_t *owners;
    uint8_t **parameter_kinds;
    EffectInvocationDeclaredBounds *declared;
    EffectInvocationOwner *equations;
    EffectInvocationSolution *solution;
    XrXirRootEffects *facts;
    XrXirRootEffectWitness *witnesses;
    uint32_t *projection_atoms;
} EffectInvocationCertificate;

_Static_assert(sizeof(EffectInvocationDeclaredBounds)%_Alignof(EffectInvocationFunctionBounds)==0 &&
    sizeof(EffectInvocationFunctionBounds)%_Alignof(EffectInvocationValueBound)==0,
    "Certificate bounds preserve descriptor and value alignment");

static XrXirStatus effect_invocation_certificate_project(const XrXirCompileContext *work,
    EffectInvocationCertificate *certificate);

typedef struct EffectInvocationSealRequest {
    EffectInvocationOwner **equations;
    EffectInvocationSolution **solution;
    const EffectOrdinaryContexts *dense;
} EffectInvocationSealRequest;

static void effect_invocation_certificate_free(EffectInvocationCertificate *certificate) {
    if (!certificate) return;
    effect_invocation_free(certificate->equations);
    effect_invocation_solution_free(certificate->solution);
    xr_compile_resources_free(certificate->declared);
    effect_invocation_declarations_free(certificate->declarations);
    xr_xir_compile_generics_free(certificate->generics,certificate->bodies.function_count);
    effect_terms_free(&certificate->terms);
    xr_xir_compile_types_free(certificate->lowered_types);xr_compile_resources_free(certificate);
}

static void *effect_invocation_copy(EffectTerms *terms,const void *source,
    uint64_t count,size_t size) {
    if (!count) return NULL;
    if (!source) { terms->status=XR_XIR_BAD_STRUCTURE;return NULL; }
    void *copy=effect_terms_alloc(terms,count,size);
    if (!copy) return NULL;
    if (!xir_compile_work(terms->remaining,count*size)) { terms->status=XR_XIR_BUDGET;return NULL; }
    memcpy(copy,source,(size_t)count*size);return copy;
}

/* Full typed children and declaration domains are copied by the existing
 * owned type/metadata implementations. Every referenced numeric type keeps
 * its real ID because this seed preserves the verified descriptor order. */
static XrXirStatus effect_invocation_copy_bodies(EffectInvocationCertificate *certificate,
    const XrXirModule *source) {
    EffectTerms *terms=&certificate->terms;
    const XrXirTypes *types=source->types?source->types:&effect_empty_types;
    XrXirStatus status=XR_XIR_OK;
    if (source->stage==XR_XIR_LOWERED) {
        /* Projected identities and every physical payload child belong to
         * this copy. They cannot enter the semantic declaration seed. */
        if (!xir_compile_work(terms->remaining,sizeof(*types))) return XR_XIR_BUDGET;
        if (types->interfaces || ((types->count!=0)!=(types->nodes!=NULL)) ||
            (types->nominals && (!types->nominals->count || types->nominals->declarations ||
             !types->nominals->identities))) return XR_XIR_BAD_STRUCTURE;
        if (!types->count && !types->nodes && !types->nominals && !types->interfaces) {
            /* An empty owned snapshot contains no producer storage to borrow. */
            terms->types=(XrXirTypes){0};terms->capacity=0;
        } else {
            status=xr_xir_compile_types_clone(terms->remaining,types,&certificate->lowered_types);
            if (status==XR_XIR_OK) {
                terms->types=*certificate->lowered_types;
                terms->capacity=types->count;
            }
        }
    } else {
        status=effect_terms_seed(terms,types);
        if (status==XR_XIR_OK) status=effect_terms_domain_match(terms,types);
    }
    if (status!=XR_XIR_OK) return status;
    uint32_t count=source->function_count;
    certificate->bodies=*source;certificate->bodies.types=&terms->types;
    certificate->bodies.provenance=NULL;certificate->bodies.functions=NULL;
    certificate->bodies.declarations=NULL;certificate->bodies.generics=NULL;certificate->bodies.defaults=NULL;
    XrXirFunction *functions=effect_terms_alloc(terms,count,sizeof(*functions));
    if (count && !functions) return terms->status;
    for (uint32_t f=0;f<count;++f) {
        if (!xir_compile_work(terms->remaining,2*sizeof(*functions))) return XR_XIR_BUDGET;
        XrXirFunction value=source->functions[f];
        value.parameters=effect_invocation_copy(terms,value.parameters,value.parameter_count,sizeof(*value.parameters));
        value.instructions=effect_invocation_copy(terms,value.instructions,value.instruction_count,sizeof(*value.instructions));
        value.blocks=effect_invocation_copy(terms,value.blocks,value.block_count,sizeof(*value.blocks));
        value.operands=effect_invocation_copy(terms,value.operands,value.operand_count,sizeof(*value.operands));
        value.name=effect_invocation_copy(terms,value.name,value.name_length,1);
        if (terms->status!=XR_XIR_OK) return terms->status;
        functions[f]=value;
    }
    certificate->bodies.functions=functions;
    if (source->declarations) status=effect_invocation_declarations_clone(terms->remaining,
        source->declarations,count,&certificate->declarations);
    if (status==XR_XIR_OK && source->generics)
        status=xr_xir_compile_generics_clone(terms->remaining,source,&certificate->generics);
    if (status==XR_XIR_OK && source->defaults) {
        certificate->defaults.records=effect_invocation_copy(terms,source->defaults->records,
            source->defaults->count,sizeof(*source->defaults->records));
        certificate->defaults.count=source->defaults->count;status=terms->status;
        certificate->bodies.defaults=&certificate->defaults;
    }
    certificate->bodies.declarations=certificate->declarations;certificate->bodies.generics=certificate->generics;
    return status;
}

/* Original advertisements remain owned independently of every Source,
 * refinement and temporary dense producer. They cannot be reconstructed from
 * narrowed instruction flags during a later certificate query. */
static XrXirStatus effect_invocation_copy_bounds(EffectInvocationCertificate *certificate,
    const EffectInvocationDeclaredBounds *source) {
    const XrXirCompileContext *work=certificate->terms.remaining;
    if (!source || source->resources!=work->resources || source->count!=certificate->bodies.function_count ||
        (source->count && !source->functions)) return XR_XIR_BAD_STRUCTURE;
    if (source->count>work->limits.functions) return XR_XIR_BUDGET;
    uint64_t header_bytes=sizeof(EffectInvocationDeclaredBounds)+
        (uint64_t)source->count*sizeof(EffectInvocationFunctionBounds);
    if (header_bytes>SIZE_MAX) return XR_XIR_BUDGET;
    size_t bytes=(size_t)header_bytes;
    for (uint32_t f=0;f<source->count;++f) {
        if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        const XrXirFunction *function=&certificate->bodies.functions[f];
        uint64_t values=(uint64_t)function->parameter_count+function->instruction_count;
        if (values!=source->functions[f].values || (values && !source->functions[f].bounds) ||
            values>SIZE_MAX/sizeof(EffectInvocationValueBound)) return XR_XIR_BAD_STRUCTURE;
        size_t row_bytes=(size_t)values*sizeof(EffectInvocationValueBound);
        if (row_bytes>SIZE_MAX-bytes) return XR_XIR_BUDGET;
        bytes+=row_bytes;
    }
    XrXirStatus status=XR_XIR_OK;
    EffectInvocationDeclaredBounds *copy=xir_compile_alloc(work,bytes,&status);
    if (!copy) return status;
    if (!xir_compile_work(work,sizeof(*copy))) {
        xr_compile_resources_free(copy);return XR_XIR_BUDGET;
    }
    EffectInvocationFunctionBounds *functions=source->count?
        (EffectInvocationFunctionBounds *)(copy+1):NULL;
    EffectInvocationValueBound *records=(EffectInvocationValueBound *)((uint8_t *)copy+(size_t)header_bytes);
    *copy=(EffectInvocationDeclaredBounds){.resources=work->resources,
        .capacity=source->count,.functions=functions};
    /* This receiving copy is one certificate allocation. Mutable original
     * bounds keep their independent allocation and destruction contracts. */
    certificate->declared=copy;
    for (uint32_t f=0;f<source->count && status==XR_XIR_OK;++f) {
        const XrXirFunction *function=&certificate->bodies.functions[f];
        uint32_t values=source->functions[f].values;
        for (uint32_t v=0;v<values;++v) {
            if (!xir_compile_work(work,3+sizeof(*records))) { status=XR_XIR_BUDGET;break; }
            EffectInvocationValueBound record=source->functions[f].bounds[v];
            if (record.mask&~(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED) ||
                record.callable!=(xr_xir_callable_signature(&certificate->terms.types,
                    xr_xir_operand_type(function,v))!=NULL) || (!record.callable && record.mask)) {
                status=XR_XIR_BAD_STRUCTURE;break;
            }
            records[v]=record;
        }
        if (status!=XR_XIR_OK) break;
        if (!xir_compile_work(work,sizeof(*functions)+1)) { status=XR_XIR_BUDGET;break; }
        functions[f]=(EffectInvocationFunctionBounds){values,values?records:NULL};
        ++copy->count;records+=values;
    }
    return status;
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
        uint32_t parameters=certificate->bodies.functions[f].parameter_count;
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

/* All failure paths keep both input owners and the occupied output intact.
 * Ownership moves only after complete copied correspondence and closure proof;
 * construction-only pointers are then removed from the published certificate. */
static XrXirStatus effect_invocation_seal(const XrXirCompileContext *work,
    const EffectInvocationSealRequest *request,EffectInvocationCertificate **output) {
    if (!xir_compile_context_valid(work) || !request || !output || *output || !request->equations ||
        !*request->equations || !request->solution || !*request->solution) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationOwner *owner=*request->equations;EffectInvocationSolution *solution=*request->solution;
    if (owner->work->resources!=work->resources || solution->resources!=work->resources ||
        solution->count!=owner->module->function_count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_invocation_producers_verify(owner);
    if (status==XR_XIR_OK) status=effect_invocation_coverage_verify(owner);
    if (status==XR_XIR_OK) status=effect_invocation_closed(owner);
    if (status==XR_XIR_OK) status=effect_invocation_deferred_verify(owner);
    if (status==XR_XIR_OK) status=effect_invocation_solution_match(owner,solution);
    if (status==XR_XIR_OK) status=effect_invocation_causes(owner);
    EffectInvocationCertificate *certificate=status==XR_XIR_OK ?
        xir_compile_calloc(work,1,sizeof(*certificate),&status):NULL;
    if (status==XR_XIR_OK && !certificate) status=XR_XIR_OUT_OF_MEMORY;
    if (status==XR_XIR_OK) {
        certificate->resources=work->resources;certificate->terms.remaining=work;
        status=effect_invocation_copy_bodies(certificate,owner->module);
    }
    if (status==XR_XIR_OK) status=effect_invocation_copy_bounds(certificate,owner->declared);
    if (status==XR_XIR_OK) status=effect_invocation_copy_origins(certificate,owner,request->dense);
    if (status==XR_XIR_OK) {
        const XrXirTypes *source_types=owner->module->types?owner->module->types:&effect_empty_types;
        status=owner->module->stage==XR_XIR_LOWERED ?
            effect_lowered_snapshot_match(work,&certificate->terms.types,source_types) :
            effect_terms_snapshot_match(&certificate->terms,source_types,source_types->count);
    }
    if (status==XR_XIR_OK) {
        certificate->equations=owner;certificate->solution=solution;
        status=effect_invocation_certificate_project(work,certificate);
    }
    if (status==XR_XIR_OK && !xir_compile_work(work,9)) status=XR_XIR_BUDGET;
    if (status!=XR_XIR_OK) {
        if (certificate) { certificate->equations=NULL;certificate->solution=NULL; }
        effect_invocation_certificate_free(certificate);return status;
    }
    certificate->terms.remaining=NULL;
    owner->work=NULL;owner->module=NULL;owner->effects=NULL;owner->graph=NULL;owner->declared=NULL;
    *request->equations=NULL;*request->solution=NULL;*output=certificate;return XR_XIR_OK;
}
