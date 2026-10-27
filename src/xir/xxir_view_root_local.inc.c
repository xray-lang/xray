/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_root_local.inc.c - Reaching values at immutable local reads
 */
static const uint8_t view_root_local_edges[XR_XIR_OP_COUNT]={0,
#define XR_XIR_OP(name,stages,result,operands,edges,terminal) edges,
#include "xxir_ops.def"
#undef XR_XIR_OP
};
typedef struct ViewRootLocal {
    ViewRootQuery *query;
    const XrXirFunction *body;
    ViewOriginSet tasks;
    ViewOriginPair context;
    uint32_t function,slot;
} ViewRootLocal;
static XrXirStatus view_root_local_unknown(ViewRootLocal *l) {
    return view_root_emit(l->query,(ViewOriginPair){l->function,l->slot},
        (ViewOriginPair){l->context.first|1u,l->context.second},XR_XIR_VIEW_ROOT_UNRESOLVED);
}
static XrXirStatus view_root_local_scan(ViewRootLocal *l,uint32_t block,uint32_t cutoff,bool prefixes,bool *closed) {
    *closed=false;const XrXirBlock *span=l->body->blocks+block;
    if (cutoff<span->first || cutoff-span->first>span->count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i=cutoff;i>span->first;) {
        --i;
        const XrXirInstruction *op=l->body->instructions+i;uint32_t value=UINT32_MAX;
        bool initialized=true;
        if (l->body->parameter_count+i==l->slot) {
            if (op->op==XR_XIR_LOCAL_NEW || op->op==XR_XIR_OWNED_LOCAL_NEW) value=op->args[0];
            else if (op->op==XR_XIR_LOCAL_UNINIT) initialized=false;
            else return XR_XIR_BAD_STRUCTURE;
        } else if ((op->op==XR_XIR_LOCAL_WRITE || op->op==XR_XIR_OWNED_LOCAL_WRITE || op->op==XR_XIR_PLACE_WRITE) &&
            op->args[0]==l->slot) value=op->args[1];
        else continue;
        XrXirStatus status=initialized?view_root_push(l->query,l->function,value,l->context.first,l->context.second):
            view_root_local_unknown(l);
        if (status!=XR_XIR_OK) return status;
        /* A normal observation stops at its most recent definition. A panic
         * observation also includes every earlier protected prefix. */
        if (!prefixes) {*closed=true;return XR_XIR_OK;}
    }
    return XR_XIR_OK;
}
static XrXirStatus view_root_local_predecessors(ViewRootLocal *l,uint32_t block) {
    ViewRootCheck *c=l->query->check;bool found=false;
    if (!block) return view_root_local_unknown(l);
    for (uint32_t b=0;b<l->body->block_count;++b) {

        const XrXirBlock *source=l->body->blocks+b;
        const XrXirInstruction *end=l->body->instructions+source->first+source->count-1u;
        bool normal=false;
        for (uint32_t e=0;e<view_root_local_edges[end->op];++e) normal|=end->targets[e]==block;
        for (uint32_t prefix=0;prefix<2;++prefix) {
            if (prefix?(!source->panic || source->panic!=block):!normal) continue;
            uint32_t ignored=0;found=true;
            XrXirStatus status=view_root_intern(c,&l->tasks,(ViewOriginPair){b,prefix},&ignored);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return found?XR_XIR_OK:view_root_local_unknown(l);
}
static XrXirStatus view_root_local_at(ViewRootQuery *q,ViewOriginPair site,uint32_t slot,uint32_t instruction,
    ViewOriginPair context) {
    ViewRootCheck *c=q->check;const XrXirFunction *body=c->module->functions+site.first;
    uint64_t fixed=sizeof(ViewRootLocal);
    if (c->reserved>c->context->limits.frame_bytes || fixed>c->context->limits.frame_bytes-c->reserved) return XR_XIR_BUDGET;

    c->reserved+=fixed;ViewRootLocal l={.query=q,.body=body,.context=context,.function=site.first,.slot=slot};
    uint32_t block=UINT32_MAX;
    XrXirStatus status=XR_XIR_OK;bool closed=false;
    for (uint32_t b=0;b<body->block_count;++b) {

        const XrXirBlock *span=body->blocks+b;
        if (instruction>=span->first && instruction-span->first<span->count) block=b;
    }
    if (status==XR_XIR_OK && block==UINT32_MAX) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) status=view_root_local_scan(&l,block,instruction,false,&closed);
    if (status==XR_XIR_OK && !closed) status=view_root_local_predecessors(&l,block);
    for (uint32_t i=0;i<l.tasks.count && status==XR_XIR_OK;++i) {
        /* Parent root interning may grow independently of this copied task. */
        ViewOriginPair task=l.tasks.pairs[i];const XrXirBlock *span=body->blocks+task.first;
        status=view_root_local_scan(&l,task.first,span->first+span->count,task.second!=0,&closed);
        if (status==XR_XIR_OK && !closed) status=view_root_local_predecessors(&l,task.first);
    }
    view_root_set_drop(c,&l.tasks);
    /* Parent tasks and visitor results retain their own charged storage. */
    XR_CHECK(c->reserved>=fixed,"local read scratch retires only its own finite frame record");
    c->reserved-=fixed;return status;
}
static XrXirStatus view_root_local_read(ViewRootQuery *q,ViewOriginPair site,const XrXirInstruction *op,
    ViewOriginPair context) {
    const XrXirFunction *body=q->check->module->functions+site.first;
    return view_root_local_at(q,site,op->args[0],site.second-body->parameter_count,context);
}
