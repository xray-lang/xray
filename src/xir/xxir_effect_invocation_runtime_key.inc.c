/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_runtime_key.inc.c - Exact current inputs before physical execution matching
 */
#include "xxir_effect_invocation_runtime_data.inc.c"
typedef struct EffectRuntimeKeySpan {uint32_t source,destination,count;} EffectRuntimeKeySpan;
typedef struct EffectRuntimeKeyProjection {
    EffectRuntimeKeySpan spans[2];
    uint32_t value,count,opaque_mask,class_mask;
} EffectRuntimeKeyProjection;

static XrXirStatus effect_runtime_key_bound_mask(const EffectRuntimeKeyRead *read,
    uint32_t function,uint32_t value,uint32_t *output) {
    const EffectInvocationDeclaredBounds *bounds=read->certificate->declared;
    if(!bounds || bounds->resources!=read->certificate->resources ||
        bounds->count!=effect_body_count(read->certificate->body) || !bounds->functions || function>=bounds->count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationFunctionBounds *body=&bounds->functions[function];
    if(value>=body->values || body->count>body->values || (!!body->bounds!=!!body->count)) return XR_XIR_BAD_STRUCTURE;
    uint32_t comparisons=0;const EffectInvocationValueBound *bound=effect_invocation_bound_find(body,value,&comparisons);
    XrXirStatus status=effect_runtime_work(&read->request->read,8+(uint64_t)comparisons*3+sizeof(*output));
    if(status!=XR_XIR_OK) return status;
    if(!bound || !bound->callable || (bound->mask&~(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED)))
        return XR_XIR_BAD_STRUCTURE;
    *output=bound->mask;return XR_XIR_OK;
}
static XrXirStatus effect_runtime_key_bound(const EffectRuntimeKeyRead *read,uint32_t p,
    bool captured,EffectRuntimeKeyProjection *projection) {
    const EffectInvocationCertificate *certificate=read->certificate;
    const XrXirTypeNode *signature=xr_xir_callable_signature(&certificate->terms.types,
        certificate->body->functions[read->request->target].parameters[p]);
    if(!signature) return XR_XIR_OK;
    if(signature->parameter_count && !signature->parameters) return XR_XIR_BAD_STRUCTURE;
    bool ref=false,fixed=false;
    for(uint32_t a=0;a<signature->parameter_count;++a) {
        XrXirStatus status=effect_runtime_work(&read->request->read,1);
        if(status!=XR_XIR_OK) return status;
        if(!xr_xir_callable_parameter_storage_valid(&certificate->terms.types,&signature->parameters[a]))
            return XR_XIR_BAD_STRUCTURE;
        ref|=signature->parameters[a].mode==XR_PARAM_REF;
    }
    XrXirStatus status=XR_XIR_OK;
    if(!captured) status=effect_runtime_key_fixed(read,projection->value,&fixed);
    if(status!=XR_XIR_OK) return status;
    uint32_t kind=UINT32_MAX;EffectBodyRead body=effect_body_runtime_read(&read->request->read);
    status=effect_role_parameter_value(&body,certificate->body,&certificate->final->role_proof,
        read->request->target,p,&kind);
    if(status!=XR_XIR_OK) return status;
    if(kind>XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC) return XR_XIR_BAD_STRUCTURE;
    if(!fixed && kind!=XR_XIR_EFFECT_PARAMETER_FIXED) return XR_XIR_OK;
    uint32_t mask=0;
    status=effect_runtime_key_bound_mask(read,fixed?read->request->caller:read->request->target,
        fixed?projection->value:p,&mask);
    if(status!=XR_XIR_OK) return status;
    if(!fixed && ref) projection->opaque_mask=(mask&XR_XIR_CALLABLE_ROOT_REQUIRED)?3:0;
    else projection->opaque_mask=1|(mask<<1);
    return XR_XIR_OK;
}
static XrXirStatus effect_runtime_key_class(const EffectRuntimeKeyRead *read,uint32_t value,uint32_t *output) {
    return effect_runtime_class_mask(read->certificate,&read->request->read,read->node,value,output);
}
static XrXirStatus effect_runtime_key_leaf(const EffectRuntimeKeyRead *read,uint32_t value,
    uint32_t first,bool *output) {
    const EffectInvocationFinal *owner=read->certificate->final;bool concrete=false,leaf=true;
    uint32_t first_fact=0,last_fact=0;
    XrXirStatus span_status=effect_runtime_key_span(read,value,&first_fact,&last_fact);
    if(span_status!=XR_XIR_OK) return span_status;
    for(uint32_t p=first_fact;p<last_fact;++p) {
        XrXirStatus status=effect_runtime_work(&read->request->read,6);
        if(status!=XR_XIR_OK) return status;
        EffectCaptureFact fact=owner->capture.facts[p];
        if(fact.node!=read->node || fact.value!=value || fact.word==EFFECT_CAPTURE_FLAGS) continue;
        if(fact.word>=read->basis.row_words || fact.bit>=64 || fact.rank>=owner->capture.count)
            return XR_XIR_BAD_STRUCTURE;
        if(fact.word<first || fact.word-first>=read->basis.origin_words) continue;
        uint64_t site=(uint64_t)(fact.word-first)*64+fact.bit;
        if(site>=owner->site_count || owner->sites[site].captures) leaf=false;
        concrete=true;
    }
    XrXirStatus status=effect_runtime_work(&read->request->read,sizeof(*output));
    if(status==XR_XIR_OK) *output=concrete && leaf;return status;
}

/* Spans contain only numeric locations in this request. They neither retain
 * decoded rows nor qualify an edge. Both directions below compare every real
 * bit, including unused columns and zero padding in the complete input. */
static XrXirStatus effect_runtime_key_project(const EffectRuntimeKeyRead *read,
    const XrXirInstruction *op,uint32_t p,EffectRuntimeKeyProjection *output) {
    const EffectInvocationCertificate *certificate=read->certificate;
    const EffectInvocationFinal *owner=certificate->final;
    const EffectBodyFunction *caller=&certificate->body->functions[read->request->caller];
    const EffectBodyFunction *target=&certificate->body->functions[read->request->target];
    if(p>=target->parameter_count || !target->parameters) return XR_XIR_BAD_STRUCTURE;
    bool captured=p<read->request->captures;XrXirType type=target->parameters[p];
    bool origin=effect_physical_origin(&certificate->terms.types,type);
    bool cell=xr_xir_type_is_cell(&certificate->terms.types,type),is_class=xr_xir_type_is_class(&certificate->terms.types,type);
    XrXirStatus status=effect_runtime_work(&read->request->read,sizeof(*output)+8);
    if(status!=XR_XIR_OK) return status;
    *output=(EffectRuntimeKeyProjection){0};uint32_t first=0;
    if(captured) {
        if(read->request->producer>=owner->site_count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationSite *site=&owner->sites[read->request->producer];
        if(site->binding>owner->binding_count || site->captures>owner->binding_count-site->binding || p>=site->captures)
            return XR_XIR_BAD_STRUCTURE;
        status=effect_runtime_closure_value(op,&output->value);if(status!=XR_XIR_OK) return status;
        uint64_t position=(uint64_t)read->basis.origin_words+
            ((uint64_t)site->binding+p)*(read->basis.origin_words+read->basis.cell_words);
        if(position+read->basis.origin_words+read->basis.cell_words>read->basis.fn_words)
            return XR_XIR_BAD_STRUCTURE;
        first=(uint32_t)position;
    } else {
        uint32_t suffix=p-read->request->captures;
        if(op->args[0]>caller->operand_count || suffix>=op->args[1] ||
            suffix>=caller->operand_count-op->args[0] || !caller->operands) return XR_XIR_BAD_STRUCTURE;
        output->value=caller->operands[op->args[0]+suffix];
    }
    if(output->value>=(uint64_t)caller->parameter_count+caller->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if(origin) {
        output->spans[0]=(EffectRuntimeKeySpan){captured?first:0,0,captured?read->basis.origin_words:read->basis.fn_words};
        output->count=1;
        if(captured) {
            bool leaf=false;status=effect_runtime_key_leaf(read,output->value,first,&leaf);
            if(status!=XR_XIR_OK) return status;
            if(!leaf) {output->spans[1]=(EffectRuntimeKeySpan){read->basis.origin_words,read->basis.origin_words,
                read->basis.fn_words-read->basis.origin_words};output->count=2;}
        }
        return effect_runtime_key_bound(read,p,captured,output);
    }
    if(cell || (is_class && captured)) {
        output->spans[0]=(EffectRuntimeKeySpan){captured?first+read->basis.origin_words:read->basis.fn_words,
            read->basis.fn_words,read->basis.cell_words};output->count=1;
    } else if(is_class) return effect_runtime_key_class(read,output->value,&output->class_mask);
    return XR_XIR_OK;
}

static XrXirStatus effect_runtime_key_literal(const EffectRuntimeKeyRead *read,
    const EffectRuntimeKeyProjection *projection,uint32_t word,uint64_t *output) {
    uint64_t bits=0;uint32_t opaque=read->certificate->final->site_count+read->basis.parameters;
    for(uint32_t bit=0;bit<3;++bit) {
        uint32_t origin=opaque+bit,cell=read->basis.parameters+bit;
        uint32_t flag=bit==0?EFFECT_CLASS_ROOT:bit==1?EFFECT_CLASS_UNKNOWN:EFFECT_CLASS_KNOWN;
        if((projection->opaque_mask&(1u<<bit)) && word==origin/64) bits|=UINT64_C(1)<<(origin%64);
        if((projection->class_mask&flag) && word==read->basis.fn_words+cell/64) bits|=UINT64_C(1)<<(cell%64);
    }
    XrXirStatus status=effect_runtime_work(&read->request->read,6+sizeof(*output));
    if(status==XR_XIR_OK) *output=bits;return status;
}
static XrXirStatus effect_runtime_key_row(const EffectRuntimeKeyRead *read,
    const EffectRuntimeKeyProjection *projection,const uint64_t *row,bool *matched) {
    bool same=true;
    for(uint32_t w=0;w<read->basis.row_words;++w) {
        uint64_t required=0;XrXirStatus status=effect_runtime_key_literal(read,projection,w,&required);
        if(status!=XR_XIR_OK) return status;
        if((row[w]&required)!=required) same=false;
        uint64_t bits=row[w]&~required;
        while(bits) {
            status=effect_runtime_work(&read->request->read,4);if(status!=XR_XIR_OK) return status;
            uint32_t bit=(uint32_t)__builtin_ctzll(bits);bits&=bits-1;bool present=false;
            for(uint32_t s=0;s<projection->count;++s) {
                const EffectRuntimeKeySpan *span=&projection->spans[s];
                if(w<span->destination || w-span->destination>=span->count) continue;
                status=effect_runtime_key_has(read,projection->value,span->source+w-span->destination,bit,&present);
                if(status!=XR_XIR_OK) return status;
            }
            if(!present) same=false;
        }
    }
    const EffectInvocationFinal *owner=read->certificate->final;
    uint32_t first_fact=0,last_fact=0;
    XrXirStatus span_status=projection->count?
        effect_runtime_key_span(read,projection->value,&first_fact,&last_fact):XR_XIR_OK;
    if(span_status!=XR_XIR_OK) return span_status;
    for(uint32_t p=first_fact;p<last_fact;++p) {
        XrXirStatus status=effect_runtime_work(&read->request->read,6);if(status!=XR_XIR_OK) return status;
        EffectCaptureFact fact=owner->capture.facts[p];
        if(fact.node!=read->node || fact.value!=projection->value || fact.word==EFFECT_CAPTURE_FLAGS) continue;
        if(fact.word>=read->basis.row_words || fact.bit>=64 || fact.rank>=owner->capture.count)
            return XR_XIR_BAD_STRUCTURE;
        EffectInvocationColumn at={fact.word,fact.bit};
        for(uint32_t s=0;s<projection->count;++s) {
            const EffectRuntimeKeySpan *span=&projection->spans[s];
            if(at.word<span->source || at.word-span->source>=span->count) continue;
            uint32_t word=span->destination+at.word-span->source;
            if(word>=read->basis.row_words) return XR_XIR_BAD_STRUCTURE;
            if(!(row[word]&(UINT64_C(1)<<at.bit))) same=false;
        }
    }
    XrXirStatus status=effect_runtime_work(&read->request->read,sizeof(*matched));
    if(status==XR_XIR_OK) *matched=same;return status;
}
static XrXirStatus effect_runtime_current_key(const EffectInvocationCertificate *certificate,
    const XirEffectInvocationRequest *request,const XrXirInstruction *op,uint32_t parent,
    const EffectInvocationNode *node,bool *matched) {
    EffectRuntimeKeyRead read={certificate,request,NULL,{0},0};
    XrXirStatus status=effect_runtime_work(&request->read,sizeof(read));
    if(status!=XR_XIR_OK) return status;
    status=effect_runtime_key_layout(&read,parent);if(status!=XR_XIR_OK) return status;
    const EffectBodyFunction *target=&certificate->body->functions[request->target];
    if(node->root!=read.from->root || node->parameter_count!=target->parameter_count ||
        request->count!=target->parameter_count || (node->parameter_count && (!target->parameters || !node->input)))
        return XR_XIR_BAD_STRUCTURE;
    if(request->producer!=UINT32_MAX) {
        uint32_t value=UINT32_MAX;status=effect_runtime_closure_value(op,&value);
        if(status!=XR_XIR_OK) return status;
        bool present=false;status=effect_runtime_key_has(&read,value,
            request->producer/64,request->producer%64,&present);
        if(status!=XR_XIR_OK) return status;
        if(!present) {*matched=false;return XR_XIR_OK;}
    }
    bool same=true;
    for(uint32_t p=0;p<node->parameter_count;++p) {
        EffectRuntimeKeyProjection projection;status=effect_runtime_key_project(&read,op,p,&projection);
        if(status!=XR_XIR_OK) return status;
        bool row=false;status=effect_runtime_key_row(&read,&projection,node->input+(size_t)p*read.basis.row_words,&row);
        if(status!=XR_XIR_OK) return status;
        if(!row) same=false;
    }
    status=effect_runtime_work(&request->read,sizeof(*matched));if(status==XR_XIR_OK)*matched=same;return status;
}
