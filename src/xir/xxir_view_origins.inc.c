/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_origins.inc.c - Returned borrowed leaves in the actual SSA graph
 *
 * KEY CONCEPT:
 *   Declared input selection precedes body traversal; body facts never choose it.
 */
typedef struct ViewOriginPair {uint32_t first,second;} ViewOriginPair;
typedef struct ViewOriginSet {
    ViewOriginPair *pairs;
    uint32_t *slots,count,capacity,slot_count;
} ViewOriginSet;
typedef struct ViewOriginQuery {
    const XrXirCompileContext *context;
    const XrXirModule *module;
    const uint8_t *flags;
    const uint32_t *candidates;
    const XrXirFunction *function;
    uint32_t candidate;
    ViewOriginSet tasks,paths;
} ViewOriginQuery;

static uint32_t view_origin_hash(uint32_t first,uint32_t second) {
    uint64_t key=((uint64_t)first<<32)|second;
    key^=key>>33;key*=UINT64_C(0xff51afd7ed558ccd);key^=key>>33;
    return (uint32_t)key;
}
static void view_origin_set_free(ViewOriginSet *set) {
    xr_compile_resources_free(set->pairs);xr_compile_resources_free(set->slots);
    *set=(ViewOriginSet){0};
}
static XrXirStatus view_origin_set_grow(const XrXirCompileContext *context,ViewOriginSet *set) {
    uint32_t capacity=set->capacity ? set->capacity*2u : 8u;
    if (set->capacity>UINT32_MAX/4u || (uint64_t)capacity*2u*sizeof(uint32_t)+
        (uint64_t)capacity*sizeof(ViewOriginPair)>context->limits.frame_bytes) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    uint32_t *slots=xir_compile_calloc(context,(size_t)capacity*2u,sizeof(*slots),&status);
    if (!slots) return status;
    for (uint32_t i=0;i<set->count;++i) {
        if (!xir_compile_work(context,4)) {status=XR_XIR_BUDGET;break;}
        uint32_t at=view_origin_hash(set->pairs[i].first,set->pairs[i].second)&(capacity*2u-1u);
        while (slots[at]) {
            if (!xir_compile_work(context,1)) {status=XR_XIR_BUDGET;break;}
            at=(at+1u)&(capacity*2u-1u);
        }
        if (status!=XR_XIR_OK) break;
        slots[at]=i+1u;
    }
    void *pairs=set->pairs;
    if (status==XR_XIR_OK) status=xir_compile_resource_status(xr_compile_resources_resize(
        context->resources,&pairs,(size_t)capacity*sizeof(ViewOriginPair)));
    if (status!=XR_XIR_OK) {xr_compile_resources_free(slots);return status;}
    xr_compile_resources_free(set->slots);set->slots=slots;set->pairs=pairs;
    set->capacity=capacity;set->slot_count=capacity*2u;return XR_XIR_OK;
}
static XrXirStatus view_origin_intern(const XrXirCompileContext *context,
    ViewOriginSet *set,ViewOriginPair pair,uint32_t *output) {
    if (!xir_compile_work(context,3)) return XR_XIR_BUDGET;
    if (set->count==set->capacity) {
        XrXirStatus status=view_origin_set_grow(context,set);if (status!=XR_XIR_OK) return status;
    }
    uint32_t at=view_origin_hash(pair.first,pair.second)&(set->slot_count-1u);
    while (set->slots[at]) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        uint32_t index=set->slots[at]-1u;
        if (set->pairs[index].first==pair.first && set->pairs[index].second==pair.second) {
            *output=index;return XR_XIR_OK;
        }
        at=(at+1u)&(set->slot_count-1u);
    }
    if (!xir_compile_work(context,sizeof(pair)+sizeof(*set->slots))) return XR_XIR_BUDGET;
    *output=set->count;set->pairs[set->count++]=pair;set->slots[at]=set->count;return XR_XIR_OK;
}
static XrXirStatus view_origin_push(ViewOriginQuery *q,uint32_t value,uint32_t path) {
    if (value>=(uint64_t)q->function->parameter_count+q->function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t ignored;
    return view_origin_intern(q->context,&q->tasks,(ViewOriginPair){value,path},&ignored);
}
static XrXirStatus view_origin_select(ViewOriginQuery *q,uint32_t value,uint32_t selector,uint32_t tail) {
    uint32_t index;
    XrXirStatus status=view_origin_intern(q->context,&q->paths,(ViewOriginPair){selector,tail},&index);
    return status==XR_XIR_OK ? view_origin_push(q,value,index+1u) : status;
}

/* Captures are the actual bound physical prefix, not a return-body guess.
 * Every exposed callable signature must name the same physical candidate. */
static XrXirStatus view_origin_candidates(const XrXirCompileContext *context,
    const XrXirModule *module,const uint8_t *flags,uint32_t *captures,uint32_t *candidates) {
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        for (uint32_t i=0;i<body->instruction_count;++i) {
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            const XrXirInstruction *op=&body->instructions[i];
            if (op->op!=XR_XIR_FUNCTION_REF) continue;
            if (op->immediate<0 || (uint64_t)op->immediate>=module->function_count) return XR_XIR_BAD_STRUCTURE;
            uint32_t target=(uint32_t)op->immediate;
            if (op->args[1]>module->functions[target].parameter_count) return XR_XIR_BAD_STRUCTURE;
            if (op->args[1]>captures[target]) captures[target]=op->args[1];
        }
    }
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];candidates[f]=UINT32_MAX;
        uint32_t result=view_flags_at(module->types,flags,body->result),count=0,permission=0;
        if (!result) continue;
        for (uint32_t p=captures[f];p<body->parameter_count;++p) {
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            bool reference=xr_xir_type_is_cell(module->types,body->parameters[p]);
            uint32_t leaves=reference ? 1u : view_leaves_at(module->types,flags,body->parameters[p]);
            if (!leaves) continue;
            count=count>1 || leaves>1 ? 2u : count+leaves;candidates[f]=p;
            permission=reference ? XR_XIR_VIEW_MUTABLE : view_flags_at(module->types,flags,body->parameters[p]);
        }
        if (count!=1 || ((result&XR_XIR_VIEW_MUTABLE) && !(permission&XR_XIR_VIEW_MUTABLE)))
            return XR_XIR_BAD_TYPE;
    }
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        for (uint32_t i=0;i<body->instruction_count;++i) {
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            const XrXirInstruction *op=&body->instructions[i];
            if (op->op!=XR_XIR_FUNCTION_REF) continue;
            uint32_t target=(uint32_t)op->immediate;
            if (candidates[target]==UINT32_MAX) continue;
            const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,op->type);
            if (!signature || signature->parameter_count!=module->functions[target].parameter_count-op->args[1])
                return XR_XIR_BAD_TYPE;
            uint32_t candidate;
            XrXirStatus status=view_return_candidate(context,module->types,flags,signature,&candidate);
            if (status!=XR_XIR_OK) return status;
            if (candidate==UINT32_MAX || candidate+op->args[1]!=candidates[target]) return XR_XIR_BAD_TYPE;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus view_origin_tuple(ViewOriginQuery *q,const XrXirInstruction *op,uint32_t path) {
    const XrXirTypeNode *tuple=xr_xir_tuple_signature(q->module->types,op->type);
    if (!tuple || (path && path>q->paths.count)) return XR_XIR_BAD_STRUCTURE;
    ViewOriginPair selection=path ? q->paths.pairs[path-1u] : (ViewOriginPair){UINT32_MAX,0};
    if (path && selection.first>=tuple->parameter_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t operand=0;
    for (uint32_t p=0;p<tuple->parameter_count;++p) {
        if (!xir_compile_work(q->context,1)) return XR_XIR_BUDGET;
        XrXirType field=tuple->parameters[p].type;
        if (field==XR_XIR_UNIT) continue;
        if (operand>=op->args[1] || op->args[0]>q->function->operand_count ||
            operand>=q->function->operand_count-op->args[0]) return XR_XIR_BAD_STRUCTURE;
        uint32_t value=q->function->operands[op->args[0]+operand++];
        if ((path && selection.first!=p) || !view_flags_at(q->module->types,q->flags,field)) continue;
        XrXirStatus status=view_origin_push(q,value,path ? selection.second : 0);
        if (status!=XR_XIR_OK) return status;
    }
    return operand==op->args[1] ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus view_origin_call(ViewOriginQuery *q,const XrXirInstruction *op) {
    uint32_t candidate=UINT32_MAX;
    if (op->op==XR_XIR_CALL) {
        if (op->immediate<0 || (uint64_t)op->immediate>=q->module->function_count) return XR_XIR_BAD_STRUCTURE;
        candidate=q->candidates[op->immediate];
    } else {
        if (op->immediate<0 || (uint64_t)op->immediate>=
            (uint64_t)q->function->parameter_count+q->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
        const XrXirTypeNode *signature=xr_xir_callable_signature(q->module->types,
            xr_xir_operand_type(q->function,(uint32_t)op->immediate));
        if (!signature) return XR_XIR_BAD_TYPE;
        XrXirStatus status=view_return_candidate(q->context,q->module->types,q->flags,signature,&candidate);
        if (status!=XR_XIR_OK) return status;
    }
    if (candidate==UINT32_MAX || candidate>=op->args[1] || op->args[0]>q->function->operand_count ||
        candidate>=q->function->operand_count-op->args[0]) return XR_XIR_BAD_STRUCTURE;
    return view_origin_push(q,q->function->operands[op->args[0]+candidate],0);
}
static XrXirStatus view_origin_step(ViewOriginQuery *q,ViewOriginPair task) {
    if (!xir_compile_work(q->context,2)) return XR_XIR_BUDGET;
    if (task.first<q->function->parameter_count)
        return task.first==q->candidate ? XR_XIR_OK : XR_XIR_BAD_VALUE;
    const XrXirInstruction *op=&q->function->instructions[task.first-q->function->parameter_count];
    switch (op->op) {
    case XR_XIR_VIEW_NEW:case XR_XIR_VIEW_REBORROW:
        if (task.second || op->args[1]!=3 || op->args[0]>q->function->operand_count ||
            3u>q->function->operand_count-op->args[0]) return XR_XIR_BAD_STRUCTURE;
        return view_origin_push(q,q->function->operands[op->args[0]],0);
    case XR_XIR_COPY:case XR_XIR_OWNED_RETAIN:case XR_XIR_CELL_PROJECT:
    case XR_XIR_CELL_PLACE:case XR_XIR_CELL_READ:case XR_XIR_FIELD_PLACE:
    case XR_XIR_INDEX_PLACE:case XR_XIR_OBJECT_PLACE:
        return view_origin_push(q,op->args[0],task.second);
    case XR_XIR_NULLABLE_NONE:return XR_XIR_OK;
    case XR_XIR_NULLABLE_SOME:
        if (!task.second) return view_origin_push(q,op->args[0],0);
        if (task.second>q->paths.count || q->paths.pairs[task.second-1u].first!=UINT32_MAX)
            return XR_XIR_BAD_STRUCTURE;
        return view_origin_push(q,op->args[0],q->paths.pairs[task.second-1u].second);
    case XR_XIR_NULLABLE_UNWRAP:return view_origin_select(q,op->args[0],UINT32_MAX,task.second);
    case XR_XIR_TUPLE_FIELD:
        if (op->immediate<0 || (uint64_t)op->immediate>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
        return view_origin_select(q,op->args[0],(uint32_t)op->immediate,task.second);
    case XR_XIR_TUPLE_NEW:return view_origin_tuple(q,op,task.second);
    case XR_XIR_CALL:case XR_XIR_CALL_INDIRECT:return view_origin_call(q,op);
    case XR_XIR_PHI:
        if (op->args[0]>q->function->operand_count || op->args[1]>q->function->operand_count-op->args[0] ||
            !op->args[1] || op->args[1]%2u) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t p=1;p<op->args[1];p+=2) {
            XrXirStatus status=view_origin_push(q,q->function->operands[op->args[0]+p],task.second);
            if (status!=XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    default:return XR_XIR_BAD_VALUE;
    }
}
static XrXirStatus view_origin_query(ViewOriginQuery *q,uint32_t value) {
    XrXirStatus status=view_origin_push(q,value,0);
    for (uint32_t at=0;at<q->tasks.count && status==XR_XIR_OK;++at)
        status=view_origin_step(q,q->tasks.pairs[at]);
    view_origin_set_free(&q->tasks);view_origin_set_free(&q->paths);return status;
}
static XrXirStatus view_origins_verify(const XrXirCompileContext *context,
    const XrXirModule *module,const uint8_t *flags,XrXirDiagnostic *diagnostic) {
    if (!module->function_count || !module->functions) return XR_XIR_BAD_STRUCTURE;
    bool needed=false;
    for (uint32_t f=0;f<module->function_count;++f) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        needed|=view_flags_at(module->types,flags,module->functions[f].result)!=0;
    }
    if (!needed) return XR_XIR_OK;
    XrXirStatus status=XR_XIR_OK;
    uint32_t *storage=xir_compile_calloc(context,module->function_count,2*sizeof(uint32_t),&status);
    if (!storage) return status;
    uint32_t *captures=storage,*candidates=storage+module->function_count;
    status=view_origin_candidates(context,module,flags,captures,candidates);
    for (uint32_t f=0;f<module->function_count && status==XR_XIR_OK;++f) {
        if (candidates[f]==UINT32_MAX) continue;
        const XrXirFunction *body=&module->functions[f];
        ViewOriginQuery query={context,module,flags,candidates,body,candidates[f],{0},{0}};
        for (uint32_t b=0;b<body->block_count && status==XR_XIR_OK;++b) {
            if (!xir_compile_work(context,1)) {status=XR_XIR_BUDGET;break;}
            uint32_t last=body->blocks[b].first+body->blocks[b].count-1u;
            const XrXirInstruction *op=&body->instructions[last];
            if (op->op!=XR_XIR_RETURN) continue;
            status=view_origin_query(&query,op->args[0]);
            if (status!=XR_XIR_OK && diagnostic) *diagnostic=(XrXirDiagnostic){status,f,b,last,XR_XIR_DIAGNOSTIC_NONE};
        }
    }
    xr_compile_resources_free(storage);return status;
}
XR_FUNC XrXirStatus xir_compile_view_return_origins_verified(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(context) || !module) return XR_XIR_BAD_STRUCTURE;
    uint8_t *flags=NULL;
    XrXirStatus status=view_flags_build(context,module->types,&flags);
    if (status==XR_XIR_OK && flags) status=view_origins_verify(context,module,flags,diagnostic);
    xr_compile_resources_free(flags);return status;
}
