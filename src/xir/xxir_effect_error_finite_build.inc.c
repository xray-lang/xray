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
    error_markers_free(proof->markers);
    xr_compile_resources_free(proof->rows);xr_compile_resources_free(proof->facts);
    xr_compile_resources_free(proof);
}
static XrXirStatus error_finite_insert(const XrXirCompileContext *work,EffectErrorFinite *proof,
    uint64_t coordinate,uint32_t rank,bool *changed) {
    EffectErrorFiniteRead read={proof,work,UINT32_MAX,NULL};uint32_t at=0;
    XrXirStatus status=error_finite_lower(&read,coordinate,&at);if(status!=XR_XIR_OK) return status;
    if(at<proof->count && proof->facts[at].coordinate==coordinate) return XR_XIR_OK;
    if(proof->count==proof->capacity) {
        if(proof->capacity>UINT32_MAX/2) return XR_XIR_BUDGET;
        uint32_t capacity=proof->capacity?proof->capacity*2:32;
        if((uint64_t)capacity>SIZE_MAX/sizeof(*proof->facts)) return XR_XIR_BUDGET;
        EffectErrorFiniteFact *facts=xir_compile_alloc(work,(size_t)capacity*sizeof(*facts),&status);
        if(!facts) return status;
        if(!xir_compile_work(work,(uint64_t)proof->count*sizeof(*facts))) {
            xr_compile_resources_free(facts);return XR_XIR_BUDGET;
        }
        if(proof->count) memcpy(facts,proof->facts,(size_t)proof->count*sizeof(*facts));
        xr_compile_resources_free(proof->facts);proof->facts=facts;proof->capacity=capacity;
    }
    if(!xir_compile_work(work,(uint64_t)(proof->count-at)*sizeof(*proof->facts)+sizeof(*proof->facts)))
        return XR_XIR_BUDGET;
    memmove(proof->facts+at+1,proof->facts+at,(size_t)(proof->count-at)*sizeof(*proof->facts));
    proof->facts[at]=(EffectErrorFiniteFact){coordinate,rank};++proof->count;*changed=true;return XR_XIR_OK;
}
typedef struct ErrorFiniteDerive { EffectErrorFinite *proof;uint32_t rank;bool *changed; } ErrorFiniteDerive;
static XrXirStatus error_finite_derive_coordinate(const EffectErrorFiniteRead *read,
    uint32_t function,uint64_t coordinate,void *owner) {
    ErrorFiniteDerive *derive=owner;uint64_t result=0;uint32_t bits=0;
    XrXirStatus status=error_finite_rhs(read,function,coordinate,&result,&bits);
    if(status!=XR_XIR_OK) return status;
    for(uint32_t bit=0;bit<bits;++bit) if(result&(UINT64_C(1)<<bit)) {
        status=error_finite_insert(read->work,derive->proof,coordinate+bit,derive->rank,derive->changed);
        if(status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus error_finite_derive(const XrXirCompileContext *work,EffectErrorFinite *proof) {
    bool changed=true;
    for(uint32_t rank=1;changed;++rank) {
        if(rank==UINT32_MAX) return XR_XIR_BUDGET;
        changed=false;EffectErrorFiniteRead read={proof,work,rank,NULL};
        ErrorFiniteDerive derive={proof,rank,&changed};
        for(uint32_t f=0;f<proof->contexts;++f) {
            bool empty=false;XrXirStatus status=error_finite_walk(&read,f,error_finite_derive_coordinate,&derive,&empty);
            if(status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
/* This returns construction storage, never an admitted permission. Geometry
 * frames freshly derived facts before markers read them. Complete closure and
 * support remain mandatory after marker callbacks at final publication. */
static XrXirStatus error_finite_construct(const XrXirCompileContext *work,const EffectInvocationCertificate *certificate,
    const EffectFinalOutcomes *outcomes,EffectErrorFinite **output) {
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
    if(status==XR_XIR_OK) status=error_finite_geometry(work,proof);
    if(status==XR_XIR_OK) status=error_finite_derive(work,proof);
    if(status==XR_XIR_OK) status=error_finite_geometry(work,proof);
    if(status==XR_XIR_OK) status=error_marker_build(work,proof,&proof->markers);
    if(status!=XR_XIR_OK) { error_finite_release(proof);return status; }
    *output=proof;return XR_XIR_OK;
}
static XrXirStatus error_finite_outcomes_match(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectFinalOutcomes *outcomes) {
    if(!outcomes) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_role_range(work,outcomes,1,sizeof(*outcomes));if(status!=XR_XIR_OK) return status;
    status=error_finite_check(work,outcomes->error_proof);if(status!=XR_XIR_OK) return status;
    const EffectErrorFinite *proof=outcomes->error_proof;
    if(proof->material!=material || proof->atoms!=outcomes->atoms || proof->atom_count!=outcomes->atom_count ||
        proof->words!=outcomes->words || proof->contexts!=outcomes->error_rows) return XR_XIR_BAD_STRUCTURE;
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
