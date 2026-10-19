/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_runtime_data.inc.c - Current semantic coordinates in one sealed owner
 */
typedef struct EffectRuntimeKeyRead {
    const EffectInvocationCertificate *certificate;
    const XirEffectInvocationRequest *request;
    const EffectInvocationNode *from;
    EffectInvocationBasis basis;
    uint32_t columns;
} EffectRuntimeKeyRead;

static XrXirStatus effect_runtime_key_layout(EffectRuntimeKeyRead *read,uint32_t parent) {
    const EffectInvocationFinal *owner=read->certificate->final;
    if(owner->material!=read->certificate->body || parent>=owner->count) return XR_XIR_BAD_STRUCTURE;
    read->from=&owner->nodes[parent];
    if(read->from->producer_count>read->from->producer_capacity ||
        (!!read->from->producers!=!!read->from->producer_capacity)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_runtime_basis(read->certificate,read->from->root,&read->basis,&read->request->read);
    if(status!=XR_XIR_OK) return status;
    uint64_t cell=(uint64_t)read->basis.parameters+3,origin=(uint64_t)owner->site_count+cell,stride=origin+cell;
    if(stride>=UINT32_MAX || owner->binding_count>(UINT32_MAX-stride-1)/stride) return XR_XIR_BAD_STRUCTURE;
    status=effect_runtime_work(&read->request->read,7+sizeof(read->columns));
    if(status==XR_XIR_OK) read->columns=(uint32_t)(stride+(uint64_t)owner->binding_count*stride+1);
    return status;
}

/* Data atoms have no causes. Identity and capture atoms were independently
 * proved before this immutable Final entered the Program. Their numeric
 * coordinates project the same complete row, with no reconstructed graph. */
static XrXirStatus effect_runtime_key_position(const EffectRuntimeKeyRead *read,
    const EffectInvocationProducer *fact,EffectInvocationColumn *output,bool *fixed) {
    const EffectInvocationFinal *owner=read->certificate->final;
    const EffectInvocationBasis *basis=&read->basis;
    uint64_t cell=(uint64_t)basis->parameters+3,origin=(uint64_t)owner->site_count+cell,column=0;
    if(fact->site==UINT32_MAX) {
        if(fact->atom>=read->columns || fact->distance || fact->next_node!=UINT32_MAX ||
            fact->next_value!=UINT32_MAX || fact->edge!=UINT32_MAX || fact->instruction!=UINT32_MAX)
            return XR_XIR_BAD_STRUCTURE;
        if(fact->atom==read->columns-1) {
            XrXirStatus status=effect_runtime_work(&read->request->read,sizeof(*fixed));
            if(status==XR_XIR_OK) *fixed=true;return status;
        }
        column=fact->atom;
    } else {
        if(fact->site>=owner->site_count) return XR_XIR_BAD_STRUCTURE;
        if(!fact->atom) column=fact->site;
        else {
            uint64_t cells=(uint64_t)owner->binding_count*cell,code=(uint64_t)fact->atom-1;
            bool capture_origin=code>=cells;if(capture_origin) code-=cells;
            uint64_t width=capture_origin?origin:cell,binding=code/width,bit=code%width;
            if(binding>=owner->binding_count) return XR_XIR_BAD_STRUCTURE;
            column=origin+binding*(origin+cell)+(capture_origin?0:origin)+bit;
        }
    }
    if(column>=read->columns-1) return XR_XIR_BAD_STRUCTURE;
    uint64_t index=column,word=0;
    if(index<origin) word=index/64;
    else {
        index-=origin;uint64_t captures=(uint64_t)owner->binding_count*(origin+cell);
        if(index<captures) {
            uint64_t binding=index/(origin+cell);index%=origin+cell;
            word=basis->origin_words+binding*(basis->origin_words+basis->cell_words);
            if(index>=origin) {index-=origin;word+=basis->origin_words;}
            word+=index/64;
        } else {index-=captures;word=basis->fn_words+index/64;}
    }
    if(word>=basis->row_words) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_runtime_work(&read->request->read,24+sizeof(*output)+sizeof(*fixed));
    if(status==XR_XIR_OK) {*output=(EffectInvocationColumn){(uint32_t)word,(uint32_t)(index%64)};*fixed=false;}
    return status;
}

static XrXirStatus effect_runtime_key_has(const EffectRuntimeKeyRead *read,uint32_t value,
    uint32_t word,uint32_t bit,bool *output) {
    uint32_t hits=0;
    for(uint32_t p=0;p<read->from->producer_count;++p) {
        XrXirStatus status=effect_runtime_work(&read->request->read,2);
        if(status!=XR_XIR_OK) return status;
        const EffectInvocationProducer *fact=&read->from->producers[p];
        if(fact->value!=value) continue;
        EffectInvocationColumn position={0};bool fixed=false;
        status=effect_runtime_key_position(read,fact,&position,&fixed);
        if(status!=XR_XIR_OK) return status;
        if(!fixed && position.word==word && position.bit==bit && ++hits>1) return XR_XIR_BAD_STRUCTURE;
    }
    XrXirStatus status=effect_runtime_work(&read->request->read,sizeof(*output));
    if(status==XR_XIR_OK) *output=hits!=0;return status;
}
static XrXirStatus effect_runtime_key_fixed(const EffectRuntimeKeyRead *read,uint32_t value,bool *output) {
    uint32_t hits=0;
    for(uint32_t p=0;p<read->from->producer_count;++p) {
        XrXirStatus status=effect_runtime_work(&read->request->read,2);
        if(status!=XR_XIR_OK) return status;
        const EffectInvocationProducer *fact=&read->from->producers[p];
        if(fact->value!=value || fact->site!=UINT32_MAX || fact->atom!=read->columns-1) continue;
        EffectInvocationColumn position={0};bool fixed=false;
        status=effect_runtime_key_position(read,fact,&position,&fixed);
        if(status!=XR_XIR_OK) return status;
        if(!fixed || ++hits>1) return XR_XIR_BAD_STRUCTURE;
    }
    XrXirStatus status=effect_runtime_work(&read->request->read,sizeof(*output));
    if(status==XR_XIR_OK) *output=hits!=0;return status;
}
