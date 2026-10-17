/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_error_equation_domain.inc.c - One rank domain across Error contexts
 *
 * KEY CONCEPT:
 *   Context records own only semantic positive facts. Every head is read from
 *   current Input; call operands descend in the same global rank domain.
 *   Lexical binding readers must independently establish the invocation domain.
 */
typedef struct EffectErrorEquationContext {
    uint32_t first,count;
} EffectErrorEquationContext;
typedef struct EffectErrorEquationDomain {
    EffectBodyRequest body;
    const XrXirCompileContext *work;
    const EffectErrorAtom *atoms;
    uint32_t atom_count,words,context_count,fact_count;
    const EffectErrorEquationContext *contexts;
    const EffectErrorEquationFact *facts;
    const void *owner;
    XrXirStatus (*function)(const struct EffectErrorEquationDomain *,uint32_t,uint32_t *);
    XrXirStatus (*call)(const EffectErrorEquationRead *,uint32_t,uint32_t,uint64_t *);
} EffectErrorEquationDomain;
typedef struct EffectErrorEquationQuery {
    uint32_t context,block,value,word,rank_limit;
} EffectErrorEquationQuery;

static inline XrXirStatus error_equation_domain_open(const EffectErrorEquationDomain *domain,
    uint32_t context,EffectErrorEquationProof *output) {
    if(context>=domain->context_count) return XR_XIR_BAD_STRUCTURE;
    if(!xir_compile_work(domain->work,sizeof(*output))) return XR_XIR_BUDGET;
    uint32_t function=UINT32_MAX;
    XrXirStatus status=domain->function(domain,context,&function);
    if(status!=XR_XIR_OK) return status;
    if(function>=domain->body.domain.function_count) return XR_XIR_BAD_STRUCTURE;
    EffectErrorEquationRead read={.body=domain->body,.work=domain->work,.atoms=domain->atoms,
        .context=context,.function=function,.atom_count=domain->atom_count,.words=domain->words,
        .rank_limit=UINT32_MAX,.owner=domain,.call=domain->call};
    status=effect_body_request_head(&read.body,function,&read.head);
    if(status!=XR_XIR_OK) return status;
    const EffectErrorEquationContext *record=&domain->contexts[context];
    if(record->first>domain->fact_count || record->count>domain->fact_count-record->first)
        return XR_XIR_BAD_STRUCTURE;
    *output=(EffectErrorEquationProof){.base=read,
        .facts=domain->facts?domain->facts+record->first:NULL,.count=record->count,
        .rank_bound=domain->fact_count};
    return XR_XIR_OK;
}

/* Binding readers use this same rank-limited fact lookup for every real target.
 * A proposed call summary is never an unranked leaf of a positive derivation. */
static inline XrXirStatus error_equation_domain_word(const EffectErrorEquationDomain *domain,
    const EffectErrorEquationQuery *query,uint64_t *output) {
    if(!query || !output || query->word>=domain->words) return XR_XIR_BAD_STRUCTURE;
    EffectErrorEquationProof proof;
    XrXirStatus status=error_equation_domain_open(domain,query->context,&proof);
    if(status!=XR_XIR_OK) return status;
    if(!xir_compile_work(domain->work,sizeof(EffectErrorEquationRead))) return XR_XIR_BUDGET;
    EffectErrorEquationRead read=error_equation_proof_read(&proof,query->rank_limit);
    return error_equation_fact_word(&read,query->block,query->value,query->word,output);
}

static inline XrXirStatus error_equation_domain_span(const EffectErrorEquationDomain *domain,
    const void *memory,uint64_t count,size_t size,size_t alignment) {
    if(!count) return XR_XIR_OK;
    if(!memory || (uintptr_t)memory%alignment || count>SIZE_MAX/size)
        return XR_XIR_BAD_STRUCTURE;
    XrCompileResourceStatus status=xr_compile_resources_extent(domain->work->resources,
        memory,(size_t)count*size);
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:
        status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
}
static inline XrXirStatus error_equation_domain_atoms(const EffectErrorEquationDomain *domain) {
    const XrXirTypes *types=domain->body.domain.types;uint32_t at=0;
    for(uint32_t t=0;types && t<types->count;++t) {
        if(!xir_compile_work(domain->work,1)) return XR_XIR_BUDGET;
        XrXirType type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
        if(!xr_xir_type_is_enum(types,type)) continue;
        uint32_t variants=error_variant_count(types,t);
        for(uint32_t v=0;v<variants;++v) {
            if(!xir_compile_work(domain->work,4)) return XR_XIR_BUDGET;
            if(at>=domain->atom_count || domain->atoms[at].type!=type || domain->atoms[at].variant!=v)
                return XR_XIR_BAD_STRUCTURE;
            ++at;
        }
    }
    uint32_t parameters=0;
    for(uint32_t f=0;domain->body.domain.generics && f<domain->body.domain.function_count;++f) {
        if(!xir_compile_work(domain->work,1)) return XR_XIR_BUDGET;
        uint32_t count=domain->body.domain.generics[f].parameter_count;
        if(count>parameters) parameters=count;
    }
    for(uint32_t p=0;p<parameters;++p) {
        if(!xir_compile_work(domain->work,4)) return XR_XIR_BUDGET;
        if(at>=domain->atom_count || domain->atoms[at].type!=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p) ||
            domain->atoms[at].variant!=XR_XIR_ERROR_SYMBOLIC_VARIANT) return XR_XIR_BAD_STRUCTURE;
        ++at;
    }
    return at==domain->atom_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static inline XrXirStatus error_equation_domain_shape(const EffectErrorEquationDomain *domain) {
    if(!domain || !xir_compile_context_valid(domain->work) || !domain->owner ||
        !domain->function || !domain->call || !domain->context_count ||
        domain->fact_count==UINT32_MAX || domain->words!=((uint64_t)domain->atom_count+65)/64)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=error_equation_domain_span(domain,domain->contexts,domain->context_count,
        sizeof(*domain->contexts),_Alignof(EffectErrorEquationContext));
    if(status==XR_XIR_OK) status=error_equation_domain_span(domain,domain->facts,domain->fact_count,
        sizeof(*domain->facts),_Alignof(EffectErrorEquationFact));
    if(status==XR_XIR_OK) status=error_equation_domain_span(domain,domain->atoms,domain->atom_count,
        sizeof(*domain->atoms),_Alignof(EffectErrorAtom));
    if(status==XR_XIR_OK) status=error_equation_domain_atoms(domain);
    if(status!=XR_XIR_OK) return status;
    uint32_t next=0;
    for(uint32_t c=0;c<domain->context_count;++c) {
        if(!xir_compile_work(domain->work,3)) return XR_XIR_BUDGET;
        const EffectErrorEquationContext *record=&domain->contexts[c];
        if(record->first!=next || record->count>domain->fact_count-next)
            return XR_XIR_BAD_STRUCTURE;
        next+=record->count;
        EffectErrorEquationProof proof;
        status=error_equation_domain_open(domain,c,&proof);
        if(status==XR_XIR_OK) status=error_equation_proof_shape(&proof);
        if(status!=XR_XIR_OK) return status;
    }
    return next==domain->fact_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

static inline XrXirStatus error_equation_domain_closure(const EffectErrorEquationDomain *domain) {
    XrXirStatus status=error_equation_domain_shape(domain);
    if(status!=XR_XIR_OK) return status;
    for(uint32_t c=0;c<domain->context_count;++c) {
        EffectErrorEquationProof proof;
        status=error_equation_domain_open(domain,c,&proof);
        if(status==XR_XIR_OK) status=error_equation_proof_closure(&proof);
        if(status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static inline XrXirStatus error_equation_domain_current(const EffectErrorEquationDomain *domain) {
    XrXirStatus status=error_equation_domain_shape(domain);
    if(status!=XR_XIR_OK) return status;
    for(uint32_t c=0;c<domain->context_count;++c) {
        EffectErrorEquationProof proof;
        status=error_equation_domain_open(domain,c,&proof);
        if(status==XR_XIR_OK) status=error_equation_proof_current(&proof);
        if(status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static inline XrXirStatus error_equation_domain_ground(EffectErrorEquationDomain *domain,
    EffectErrorEquationFact *facts) {
    if(!domain || facts!=domain->facts) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=error_equation_domain_shape(domain);
    if(status!=XR_XIR_OK) return status;
    if(!xir_compile_work(domain->work,(uint64_t)domain->fact_count*sizeof(facts->rank)))
        return XR_XIR_BUDGET;
    for(uint32_t n=0;n<domain->fact_count;++n) facts[n].rank=UINT32_MAX;
    uint32_t derived=0;
    while(derived<domain->fact_count) {
        bool changed=false;
        for(uint32_t direction=0;direction<2;++direction)
            for(uint32_t at=0;at<domain->context_count;++at) {
                uint32_t c=direction?domain->context_count-at-1:at;
                EffectErrorEquationProof proof;
                status=error_equation_domain_open(domain,c,&proof);
                if(status!=XR_XIR_OK) return status;
                const EffectErrorEquationContext *record=&domain->contexts[c];
                for(uint32_t offset=0;offset<record->count;++offset) {
                    if(!xir_compile_work(domain->work,1)) return XR_XIR_BUDGET;
                    uint32_t n=record->first+offset;
                    if(facts[n].rank!=UINT32_MAX) continue;
                    if(!xir_compile_work(domain->work,sizeof(EffectErrorEquationRead))) return XR_XIR_BUDGET;
                    EffectErrorEquationRead read=error_equation_proof_read(&proof,derived+1);
                    uint64_t actual=0;
                    status=error_equation_fact_rhs(&read,facts[n].block,facts[n].value,facts[n].bit/64,&actual);
                    if(status!=XR_XIR_OK) return status;
                    if(actual&(UINT64_C(1)<<(facts[n].bit%64))) {
                        facts[n].rank=++derived;changed=true;
                    }
                }
            }
        if(!changed) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
