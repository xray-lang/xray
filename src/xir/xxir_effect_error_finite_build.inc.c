/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_error_finite_build.inc.c - Owned finite Error claims
 */
static void error_finite_release(EffectErrorFinite *proof) {
    if(!proof) return;
    error_bindings_free(proof->bindings);
    error_markers_free(proof->markers);
    effect_terms_free(&proof->terms);
    xr_compile_resources_free(proof->rows);xr_compile_resources_free(proof->facts);
    xr_compile_resources_free(proof);
}
/* Growth is outside every borrowed equation walk. Both allocation and release
 * can call client code, so current ranges are reopened after each boundary. */
static XrXirStatus error_finite_grow(const XrXirCompileContext *work,EffectErrorFinite *proof,
    EffectErrorFiniteView *current) {
    if(!current || current->work!=work || current->proof!=proof || current->material!=proof->material ||
        current->input.work!=work || current->input.material!=proof->material) return XR_XIR_BAD_STRUCTURE;
    if(proof->count!=proof->capacity || proof->capacity>UINT32_MAX/2) return XR_XIR_BUDGET;
    uint32_t count=proof->count,old_capacity=proof->capacity,capacity=old_capacity?old_capacity*2:32;
    if((uint64_t)capacity>SIZE_MAX/sizeof(*proof->facts)) return XR_XIR_BUDGET;
    EffectErrorFiniteFact *old=proof->facts;
    *current=(EffectErrorFiniteView){0};XrXirStatus status=XR_XIR_OK;
    EffectErrorFiniteFact *facts=xir_compile_alloc(work,(size_t)capacity*sizeof(*facts),&status);
    if(!facts) return status;
    status=error_finite_geometry(work,proof);
    if(status==XR_XIR_OK && (proof->facts!=old || proof->count!=count || proof->capacity!=old_capacity))
        status=XR_XIR_BAD_STRUCTURE;
    if(status==XR_XIR_OK) status=effect_role_range(work,facts,capacity,sizeof(*facts));
    if(status==XR_XIR_OK && !xir_compile_work(work,(uint64_t)count*sizeof(*facts))) status=XR_XIR_BUDGET;
    if(status!=XR_XIR_OK) { xr_compile_resources_free(facts);return status; }
    if(count) memcpy(facts,old,(size_t)count*sizeof(*facts));
    if(old) {
        proof->facts=NULL;proof->count=0;proof->capacity=0;
        xr_compile_resources_free(old);
        status=error_finite_geometry(work,proof);
        if(status==XR_XIR_OK && (proof->facts || proof->count || proof->capacity)) status=XR_XIR_BAD_STRUCTURE;
        if(status==XR_XIR_OK) status=effect_role_range(work,facts,capacity,sizeof(*facts));
        if(status!=XR_XIR_OK) { xr_compile_resources_free(facts);return status; }
    }
    proof->facts=facts;proof->count=count;proof->capacity=capacity;
    return XR_XIR_OK;
}
static XrXirStatus error_finite_insert(const XrXirCompileContext *work,EffectErrorFinite *proof,
    uint64_t coordinate,uint32_t rank,bool *changed,bool *paused) {
    EffectErrorFiniteRead read={proof,work,UINT32_MAX,NULL};uint32_t at=0;
    XrXirStatus status=error_finite_lower(&read,coordinate,&at);if(status!=XR_XIR_OK) return status;
    if(at<proof->count && proof->facts[at].coordinate==coordinate) return XR_XIR_OK;
    if(proof->count==proof->capacity) { *paused=true;return XR_XIR_OK; }
    if(!xir_compile_work(work,(uint64_t)(proof->count-at)*sizeof(*proof->facts)+sizeof(*proof->facts)))
        return XR_XIR_BUDGET;
    memmove(proof->facts+at+1,proof->facts+at,(size_t)(proof->count-at)*sizeof(*proof->facts));
    proof->facts[at]=(EffectErrorFiniteFact){coordinate,rank};++proof->count;*changed=true;return XR_XIR_OK;
}
typedef struct ErrorFiniteDerive {
    EffectErrorFinite *proof;
    uint32_t rank;
    bool *changed,full;
} ErrorFiniteDerive;
static XrXirStatus error_finite_derive_coordinate(const EffectErrorFiniteRead *read,
    uint32_t function,uint64_t coordinate,void *owner,bool *paused) {
    ErrorFiniteDerive *derive=owner;uint64_t result=0;uint32_t bits=0;
    XrXirStatus status=error_finite_rhs(read,function,coordinate,&result,&bits);
    if(status!=XR_XIR_OK) return status;
    for(uint32_t bit=0;bit<bits;++bit) if(result&(UINT64_C(1)<<bit)) {
        status=error_finite_insert(read->work,derive->proof,coordinate+bit,derive->rank,derive->changed,paused);
        if(status!=XR_XIR_OK) return status;
        if(*paused) { derive->full=true;return XR_XIR_OK; }
    }
    return XR_XIR_OK;
}
static XrXirStatus error_finite_derive(const XrXirCompileContext *work,EffectErrorFinite *proof) {
    bool changed=true;EffectErrorFiniteView current={0};
    for(uint32_t rank=1;changed;++rank) {
        if(rank==UINT32_MAX) return XR_XIR_BUDGET;
        changed=false;ErrorFiniteDerive derive={proof,rank,&changed,false};
        for(uint32_t f=0;f<proof->contexts;) {
            XrXirStatus status=XR_XIR_OK;
            if(!current.work) status=error_finite_view_open(work,proof,&current);
            if(status!=XR_XIR_OK) return status;
            {
                EffectErrorFiniteRead read={proof,work,rank,&current};bool empty=false;
                status=error_finite_walk(&read,f,error_finite_derive_coordinate,&derive,&empty);
            }
            if(status!=XR_XIR_OK) return status;
            if(derive.full) {
                status=error_finite_grow(work,proof,&current);if(status!=XR_XIR_OK) return status;
                derive.full=false;
            } else ++f;
        }
    }
    return XR_XIR_OK;
}
/* This returns construction storage, never an admitted permission. Geometry
 * frames freshly derived facts before markers read them. Complete closure and
 * support remain mandatory after marker callbacks at final publication. */
static XrXirStatus error_finite_construct(const XrXirCompileContext *work,const EffectInvocationCertificate *certificate,
    const EffectFinalOutcomes *outcomes,EffectTerms *types,EffectErrorFinite **output) {
    if(!output || *output || !outcomes || !xir_compile_context_valid(work)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_role_range(work,certificate,1,sizeof(*certificate));
    if(status!=XR_XIR_OK) return status;
    if(certificate->resources!=work->resources) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyMaterial *material=certificate->body;
    status=effect_role_material_frame(work,material);if(status!=XR_XIR_OK) return status;
    uint32_t functions=effect_body_count(material);
    if(outcomes->error_rows<functions || outcomes->atom_count>outcomes->atom_capacity ||
        outcomes->words!=((uint64_t)outcomes->atom_count+65)/64) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,material->functions,functions,sizeof(*material->functions));if(status!=XR_XIR_OK) return status;
    EffectErrorFinite *proof=xir_compile_calloc(work,1,sizeof(*proof),&status);if(!proof) return status;
    proof->resources=work->resources;proof->material=material;proof->atoms=outcomes->atoms;
    proof->atom_count=outcomes->atom_count;proof->words=outcomes->words;proof->functions=functions;
    proof->invocations=certificate->final;
    if(types) {
        if(!xir_compile_work(work,2*sizeof(*types))) { error_finite_release(proof);return XR_XIR_BUDGET; }
        proof->terms=*types;*types=(EffectTerms){0};
    } else {
        const XrXirTypes *domain=material->domain.types;
        if(outcomes->error_proof) {
            status=effect_role_range(work,outcomes->error_proof,1,sizeof(*outcomes->error_proof));
            if(status!=XR_XIR_OK) { error_finite_release(proof);return status; }
            domain=error_finite_types(outcomes->error_proof);
        }
        proof->terms.types=*domain;
    }
    status=error_finite_context_count(work,proof,&proof->contexts,&proof->contextual);
    if(status==XR_XIR_OK && proof->contexts!=outcomes->error_rows) status=XR_XIR_BAD_STRUCTURE;
    if(status!=XR_XIR_OK) { error_finite_release(proof);return status; }
    proof->rows=xir_compile_calloc(work,proof->contexts,sizeof(*proof->rows),&status);
    if(!proof->rows) { error_finite_release(proof);return status; }
    uint64_t at=0,summary=(uint64_t)proof->words*64;
    for(uint32_t f=0;f<proof->contexts && status==XR_XIR_OK;++f) {
        uint32_t node=UINT32_MAX;status=error_finite_context_node(work,proof,f,&node);
        if(status!=XR_XIR_OK) break;
        if(proof->invocations->nodes[node].body>=functions) { status=XR_XIR_BAD_STRUCTURE;break; }
        const EffectBodyFunction *head=&material->functions[proof->invocations->nodes[node].body];uint64_t values=(uint64_t)head->parameter_count+head->instruction_count;
        uint64_t rows=(uint64_t)head->block_count*values;
        if(values>UINT32_MAX || at>UINT64_MAX-summary || head->block_count>UINT64_MAX-at-summary ||
            rows>(UINT64_MAX-at-summary-head->block_count)/summary) { status=XR_XIR_BUDGET;break; }
        uint64_t size=head->block_count+rows*summary+summary;
        if(!xir_compile_work(work,sizeof(*proof->rows))) { status=XR_XIR_BUDGET;break; }
        proof->rows[f]=(EffectErrorFiniteRow){at,at+size,head->block_count,(uint32_t)values,node};at+=size;
    }
    if(status==XR_XIR_OK) status=error_finite_types_frame(work,proof);
    if(status==XR_XIR_OK) status=error_bindings_build(work,proof);
    if(status==XR_XIR_OK) status=effect_terms_retire(work,&proof->terms,true);
    if(status==XR_XIR_OK) status=error_finite_derive(work,proof);
    if(status==XR_XIR_OK) status=error_finite_geometry(work,proof);
    if(status==XR_XIR_OK) status=error_marker_build(work,proof,&proof->markers);
    if(status!=XR_XIR_OK) { error_finite_release(proof);return status; }
    *output=proof;return XR_XIR_OK;
}
/* Only unpublished constructor storage accepts these framed proposals. They
 * grant no permission: complete closure, support and output correspondence
 * remain mandatory after the final callback and before publication. */
static XrXirStatus error_finite_outcomes_project(const XrXirCompileContext *work,XrXirEffects *effects) {
    if(!effects || !xir_effects_context_matches(work,effects,effects->count) || !effects->outcomes_owner ||
        effects->root_input || !effects->invocations || !effects->invocations->final ||
        effects->invocations->final->outcomes) return XR_XIR_BAD_STRUCTURE;
    EffectFinalOutcomes *outcomes=&effects->outcomes_work;
    if(outcomes->resources || outcomes->count) return XR_XIR_BAD_STRUCTURE;
    const EffectErrorFinite *proof=outcomes->error_proof;
    XrXirStatus status=error_finite_geometry(work,proof);if(status!=XR_XIR_OK) return status;
    if(proof->atoms!=outcomes->atoms || proof->atom_count!=outcomes->atom_count ||
        proof->words!=outcomes->words || proof->contexts!=outcomes->error_rows ||
        outcomes->source_type_count!=proof->material->domain.types->count) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,outcomes->errors,(uint64_t)proof->contexts*proof->words,sizeof(*outcomes->errors));
    if(status==XR_XIR_OK) status=effect_role_range(work,outcomes->functions,proof->functions,sizeof(*outcomes->functions));
    if(status==XR_XIR_OK) status=effect_role_range(work,outcomes->task_errors,proof->functions,sizeof(*outcomes->task_errors));
    if(status!=XR_XIR_OK) return status;
    status=error_marker_permissions_fold(work,proof,NULL,outcomes->task_errors);if(status!=XR_XIR_OK) return status;
    EffectErrorFiniteRead read={proof,work,UINT32_MAX,NULL};
    for(uint32_t f=0;f<proof->contexts;++f) {
        XrXirEffect throws=XR_XIR_EFFECT_NONE;
        for(uint32_t word=0;word<proof->words;++word) {
            uint64_t row=0;status=error_finite_word(&read,proof->rows[f].end-(uint64_t)proof->words*64+
                (uint64_t)word*64,64,&row);if(status!=XR_XIR_OK) return status;
            if(!xir_compile_work(work,sizeof(row))) return XR_XIR_BUDGET;
            outcomes->errors[(size_t)f*proof->words+word]=row;
            if(!word) {
                if(row&1) throws=XR_XIR_EFFECT_UNKNOWN;
                if(row&~UINT64_C(1)) throws=XR_XIR_EFFECT_MAY;
            } else if(row) throws=XR_XIR_EFFECT_MAY;
        }
        if(f<proof->functions) {
            if(!xir_compile_work(work,sizeof(throws))) return XR_XIR_BUDGET;
            outcomes->functions[f].throws=throws;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus error_finite_outcomes_match(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectFinalOutcomes *outcomes) {
    if(!outcomes) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_role_range(work,outcomes,1,sizeof(*outcomes));if(status!=XR_XIR_OK) return status;
    status=error_finite_check(work,outcomes->error_proof);if(status!=XR_XIR_OK) return status;
    const EffectErrorFinite *proof=outcomes->error_proof;
    if(proof->material!=material || proof->atoms!=outcomes->atoms || proof->atom_count!=outcomes->atom_count ||
        proof->words!=outcomes->words || proof->contexts!=outcomes->error_rows ||
        outcomes->source_type_count!=material->domain.types->count) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,outcomes->errors,(uint64_t)proof->contexts*proof->words,sizeof(*outcomes->errors));
    if(status==XR_XIR_OK) status=effect_role_range(work,outcomes->functions,proof->functions,sizeof(*outcomes->functions));
    if(status==XR_XIR_OK) status=effect_role_range(work,outcomes->task_errors,proof->functions,sizeof(*outcomes->task_errors));
    if(status!=XR_XIR_OK) return status;
    EffectErrorFiniteRead read={proof,work,UINT32_MAX,NULL};
    for(uint32_t f=0;f<proof->contexts;++f) {
        XrXirEffect throws=XR_XIR_EFFECT_NONE;
        for(uint32_t word=0;word<proof->words;++word) {
            uint64_t row=0;status=error_finite_word(&read,proof->rows[f].end-(uint64_t)proof->words*64+
                (uint64_t)word*64,64,&row);if(status!=XR_XIR_OK) return status;
            if(!xir_compile_work(work,6)) return XR_XIR_BUDGET;
            if(row!=outcomes->errors[(size_t)f*proof->words+word]) return XR_XIR_BAD_STRUCTURE;
            if(!word) {
                if(row&1) throws=XR_XIR_EFFECT_UNKNOWN;
                if(row&~UINT64_C(1)) throws=XR_XIR_EFFECT_MAY;
            } else if(row) throws=XR_XIR_EFFECT_MAY;
        }
        if(f<proof->functions && outcomes->functions[f].throws!=throws) return XR_XIR_BAD_STRUCTURE;
    }
    return error_marker_permissions_match(work,proof,outcomes->task_errors);
}
