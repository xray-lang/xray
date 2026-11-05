/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_runtime_iterator.inc.c - Complete physical origin capture traversal
 *
 * KEY CONCEPT:
 *   Current static rows remain complete. Physical objects supply members and
 *   original captures; only an already active object closes its own back edge.
 */
typedef struct EffectRuntimeOriginRead {
    const EffectInvocationCertificate *certificate;
    const XirEffectInvocationRequest *request;
    EffectInvocationBasis basis;
    const uint64_t *row;
} EffectRuntimeOriginRead;
typedef struct EffectRuntimeOriginFrame {XirEffectRuntimeValue value;uint32_t next;} EffectRuntimeOriginFrame;
typedef struct EffectRuntimeOriginStack {
    EffectRuntimeOriginFrame *frames,first;size_t depth,capacity,bytes;
} EffectRuntimeOriginStack;
static XrXirStatus effect_runtime_origin_shape(const EffectRuntimeOriginRead *read,
    const XirEffectRuntimeValue *value,XrXirType expected,uint32_t column,uint32_t *captures) {
    const EffectInvocationCertificate *certificate=read->certificate;
    const EffectInvocationFinal *owner=certificate->final;
    bool iterator=effect_physical_iterator(&certificate->terms.types,expected);
    bool terminal=iterator && value->publication==0;
    if(value->reserved || !value->payload || value->producer>=owner->site_count || (!terminal && value->publication!=UINT32_MAX) ||
        !effect_physical_origin(&certificate->terms.types,(XrXirType)value->type) ||
        iterator!=effect_physical_iterator(&certificate->terms.types,(XrXirType)value->type)) return XR_XIR_BAD_TYPE;
    const EffectInvocationSite *site=&owner->sites[value->producer];
    if(site->target>=effect_body_count(certificate->body) || site->function>=effect_body_count(certificate->body) ||
        site->binding>owner->binding_count || site->captures>owner->binding_count-site->binding ||
        site->captures>certificate->body->functions[site->target].parameter_count ||
        (site->captures && !certificate->body->functions[site->target].parameters) ||
        column>read->basis.fn_words || read->basis.origin_words>read->basis.fn_words-column)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_runtime_work(&read->request->read,13+sizeof(*captures));
    if(status!=XR_XIR_OK) return status;
    if(!(read->row[column+value->producer/64]&(UINT64_C(1)<<(value->producer%64)))) return XR_XIR_BAD_TYPE;
    EffectBodyRead body=(EffectBodyRead){.execution=&read->request->read};XrXirInstruction reference;
    status=effect_body_instruction_get(&body,&certificate->body->functions[site->function],site->instruction,&reference);
    if(status!=XR_XIR_OK) return status;
    if(reference.op!=(iterator?XR_XIR_GENERATOR_NEW:XR_XIR_FUNCTION_REF) ||
        reference.immediate!=(int64_t)site->target || reference.args[1]!=site->captures ||
        reference.type!=(XrXirType)value->type) return XR_XIR_BAD_TYPE;
    /* A retired producer has no Frame captures. Its complete static key and
     * closed node remain unchanged; this physical fact grants no body execution. */
    *captures=terminal?0:site->captures;return XR_XIR_OK;
}
static XrXirStatus effect_runtime_origin_storage(const EffectRuntimeOriginRead *read,
    const XirEffectRuntimeValue *value,XrXirType type,uint32_t column) {
    const EffectInvocationBasis *basis=&read->basis;
    bool cell=xr_xir_type_is_cell(&read->certificate->terms.types,type);
    bool is_class=xr_xir_type_is_class(&read->certificate->terms.types,type);
    if(value->reserved || value->producer!=UINT32_MAX || (!cell && !is_class?value->publication!=UINT32_MAX:value->publication>1))
        return XR_XIR_BAD_TYPE;
    XrXirStatus checked=effect_runtime_work(&read->request->read,1);
    if(checked!=XR_XIR_OK || (!cell && !is_class)) return checked;
    uint64_t first=(uint64_t)column+basis->origin_words;
    if(first>basis->fn_words || basis->cell_words>basis->fn_words-first) return XR_XIR_BAD_STRUCTURE;
    for(uint32_t w=0;w<basis->cell_words;++w) {
        XrXirStatus status=effect_runtime_work(&read->request->read,5);
        if(status!=XR_XIR_OK) return status;
        uint64_t bits=read->row[first+w];
        uint64_t known=w==(basis->parameters+2)/64?UINT64_C(1)<<((basis->parameters+2)%64):0;
        uint64_t root=w==basis->parameters/64?UINT64_C(1)<<(basis->parameters%64):0;
        if(cell) {if(bits!=(known|(value->publication?root:0))) return XR_XIR_BAD_TYPE;}
        else if((known && !(bits&known)) || (value->publication && root && !(bits&root)))
            return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
/* The fixed first frame is automatic. Deeper physical paths use only the
 * live runtime owner's finite allocation and scratch budget, never a C call. */
static XrXirStatus effect_runtime_origin_push(EffectRuntimeOriginStack *stack,
    const XirEffectRuntimeValue *value,const XirEffectRuntimeRead *read) {
    XrXirStatus status=effect_runtime_work(read,sizeof(EffectRuntimeOriginFrame)+3);
    if(status!=XR_XIR_OK) return status;
    if(stack->depth==stack->capacity) {
        if(!read->allocate || !read->deallocate) return XR_XIR_BAD_TYPE;
        if(stack->capacity>SIZE_MAX/2/sizeof(*stack->frames)) return XR_XIR_BUDGET;
        size_t capacity=stack->capacity*2,bytes=capacity*sizeof(*stack->frames);
        status=effect_runtime_work(read,(uint64_t)stack->depth*sizeof(*stack->frames));
        if(status!=XR_XIR_OK) return status;
        EffectRuntimeOriginFrame *memory=read->allocate(read->owner,bytes,&status);
        if(!memory) return status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:status;
        if(status!=XR_XIR_OK) {read->deallocate(read->owner,memory,bytes);return status;}
        memcpy(memory,stack->frames,stack->depth*sizeof(*memory));
        if(stack->frames!=&stack->first) read->deallocate(read->owner,stack->frames,stack->bytes);
        stack->frames=memory;stack->capacity=capacity;stack->bytes=bytes;
    }
    stack->frames[stack->depth++]=(EffectRuntimeOriginFrame){*value,0};return XR_XIR_OK;
}
static XrXirStatus effect_runtime_origin_walk(const EffectRuntimeOriginRead *read,
    uint32_t parameter,const XirEffectRuntimeValue *root,uint32_t captures) {
    if(!captures) return XR_XIR_OK;
    const XirEffectRuntimeRead *actual=&read->request->read;const EffectInvocationFinal *owner=read->certificate->final;
    EffectRuntimeOriginStack stack={0};stack.frames=&stack.first;stack.capacity=1;
    XrXirStatus status=effect_runtime_origin_push(&stack,root,actual);
    while(stack.depth && status==XR_XIR_OK) {
        EffectRuntimeOriginFrame frame=stack.frames[stack.depth-1];
        const EffectInvocationSite *site=&owner->sites[frame.value.producer];
        if(frame.next==site->captures) {--stack.depth;continue;}
        if(frame.next>site->captures) {status=XR_XIR_BAD_STRUCTURE;break;}
        uint32_t c=stack.frames[stack.depth-1].next++;
        status=effect_runtime_work(actual,7+sizeof(XirEffectRuntimeValue));if(status!=XR_XIR_OK) break;
        uint64_t width=(uint64_t)read->basis.origin_words+read->basis.cell_words;
        uint64_t offset=((uint64_t)site->binding+c)*width;
        uint64_t available=(uint64_t)read->basis.fn_words-read->basis.origin_words;
        if(offset>available || width>available-offset) {status=XR_XIR_BAD_STRUCTURE;break;}
        uint32_t position=read->basis.origin_words+(uint32_t)offset;
        XrXirType expected=read->certificate->body->functions[site->target].parameters[c];
        XirEffectRuntimeValue child={0};status=actual->value(actual->owner,parameter,&frame.value,c,&child);
        if(status!=XR_XIR_OK) break;
        if(child.type!=(uint32_t)expected) {status=XR_XIR_BAD_TYPE;break;}
        if(!effect_physical_origin(&read->certificate->terms.types,expected)) {
            status=effect_runtime_origin_storage(read,&child,expected,(uint32_t)position);continue;
        }
        uint32_t count=0;status=effect_runtime_origin_shape(read,&child,expected,(uint32_t)position,&count);
        if(status!=XR_XIR_OK || !count) continue;
        bool ancestor=false;
        for(size_t n=0;n<stack.depth;++n) {
            status=effect_runtime_work(actual,2);if(status!=XR_XIR_OK) break;
            if(stack.frames[n].value.payload==child.payload) {
                status=effect_runtime_work(actual,4);if(status!=XR_XIR_OK) break;
                const XirEffectRuntimeValue *prior=&stack.frames[n].value;
                /* One physical object keeps its complete immutable origin. */
                if(prior->type!=child.type || prior->reserved!=child.reserved ||
                    prior->producer!=child.producer || prior->publication!=child.publication)
                    status=XR_XIR_BAD_TYPE;
                else ancestor=true;
                break;
            }
        }
        if(status==XR_XIR_OK && !ancestor) status=effect_runtime_origin_push(&stack,&child,actual);
    }
    if(stack.frames!=&stack.first) actual->deallocate(actual->owner,stack.frames,stack.bytes);
    return status;
}
static XrXirStatus effect_runtime_origin_member(const EffectInvocationCertificate *certificate,
    const XirEffectInvocationRequest *request,const EffectInvocationNode *node,uint32_t parameter,bool *output) {
    if(!request->read.value || !output || parameter>=node->parameter_count) return XR_XIR_BAD_TYPE;
    EffectRuntimeOriginRead read={certificate,request,{0},NULL};
    XrXirStatus status=effect_runtime_basis(certificate,node->root,&read.basis,&request->read);
    if(status!=XR_XIR_OK) return status;
    read.row=node->input+(size_t)parameter*read.basis.row_words;
    XirEffectRuntimeValue value={0};status=request->read.value(request->read.owner,parameter,NULL,UINT32_MAX,&value);
    uint32_t captures=0;
    if(status==XR_XIR_OK) status=effect_runtime_origin_shape(&read,&value,
        certificate->body->functions[node->body].parameters[parameter],0,&captures);
    if(status==XR_XIR_OK) status=effect_runtime_origin_walk(&read,parameter,&value,captures);
    if(status==XR_XIR_OK) *output=true;return status;
}
