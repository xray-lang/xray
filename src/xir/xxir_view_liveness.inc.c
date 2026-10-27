/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_liveness.inc.c - Finite backward borrowed demand and PHI edges
 *
 * KEY CONCEPT:
 *   Normal edge operands and protected entry demand are distinct CFG facts.
 */
#include "xxir_view_liveness_internal.h"
#include "../base/xchecks.h"

typedef struct ViewLive {
    const XrXirCompileContext *context;
    const XrXirModule *module;
    const XrXirFunction *function;
    const uint8_t *types;
    XrXirViewLiveVisitor visitor;
    void *user;
    XrXirDiagnostic location;
    uint32_t *indices,*values,*graph,*queue,*heads,*predecessors,*next;
    uint8_t *reachable,*queued;
    uint64_t *storage,*inputs,*outputs,*current,*edge,*after;
    uint32_t count,words,total,head,tail,pending,edges;
} ViewLive;
static bool view_live_type(const ViewLive *a,XrXirType type) {
    return xr_xir_type_node(a->module->types,type) &&
        (a->types[(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE]&3u)!=0;
}
static bool view_live_owner(const ViewLive *a,uint32_t value) {
    if (value>=a->function->parameter_count) {
        XrXirOp op=a->function->instructions[value-a->function->parameter_count].op;
        if (op==XR_XIR_INVOKE || op==XR_XIR_INVOKE_INDIRECT || op==XR_XIR_INVOKE_DEFAULT) return false;
    }
    return view_live_type(a,xr_xir_operand_type(a->function,value));
}
static void view_live_free(ViewLive *a) {
    xr_compile_resources_free(a->indices);xr_compile_resources_free(a->values);
    xr_compile_resources_free(a->graph);xr_compile_resources_free(a->reachable);
    xr_compile_resources_free(a->queued);xr_compile_resources_free(a->storage);
}
static XrXirStatus view_live_allocate(ViewLive *a) {
    const XrXirFunction *f=a->function;uint64_t total=(uint64_t)f->parameter_count+f->instruction_count;
    if (!f->block_count || !f->blocks || !f->instructions || total>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    a->total=(uint32_t)total;
    for (uint32_t v=0;v<a->total;++v) {

        a->count+=view_live_owner(a,v);
    }
    if (!a->count) return XR_XIR_OK;
    a->words=a->count/64u+(a->count%64u!=0);
    uint64_t cells=((uint64_t)f->block_count*2u+3u)*a->words;
    uint64_t bytes=(total+a->count)*sizeof(uint32_t)+(uint64_t)f->block_count*(8u*sizeof(uint32_t)+2u);
    if (bytes>SIZE_MAX || bytes>a->context->limits.frame_bytes ||
        cells>(a->context->limits.frame_bytes-bytes)/sizeof(uint64_t) || cells>SIZE_MAX/sizeof(uint64_t))
        return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    a->indices=xir_compile_alloc(a->context,(size_t)total*sizeof(*a->indices),&status);
    a->values=xir_compile_alloc(a->context,(size_t)a->count*sizeof(*a->values),&status);
    a->graph=xir_compile_calloc(a->context,f->block_count,8u*sizeof(*a->graph),&status);
    a->reachable=xir_compile_calloc(a->context,f->block_count,1,&status);
    a->queued=xir_compile_calloc(a->context,f->block_count,1,&status);
    a->storage=xir_compile_calloc(a->context,(size_t)cells,sizeof(*a->storage),&status);
    if (status!=XR_XIR_OK) return status;
    uint32_t count=0;
    for (uint32_t v=0;v<a->total;++v) {

        a->indices[v]=UINT32_MAX;
        if (view_live_owner(a,v)) {a->indices[v]=count;a->values[count++]=v;}
    }
    XR_CHECK(count==a->count,"borrowed demand inventory is immutable during analysis");
    a->queue=a->graph;a->heads=a->queue+f->block_count;
    a->predecessors=a->heads+f->block_count;a->next=a->predecessors+(size_t)f->block_count*3u;
    a->inputs=a->storage;a->outputs=a->inputs+(size_t)f->block_count*a->words;
    a->current=a->outputs+(size_t)f->block_count*a->words;a->edge=a->current+a->words;a->after=a->edge+a->words;
    return XR_XIR_OK;
}
static XrXirStatus view_live_row(ViewLive *a,uint64_t *to,const uint64_t *from,bool merge) {
    for (uint32_t w=0;w<a->words;++w) {

        to[w]=merge ? to[w]|from[w] : from[w];
    }
    return XR_XIR_OK;
}
static XrXirStatus view_live_value(ViewLive *a,uint64_t *bits,uint32_t value,bool use) {

    if (value>=a->total) return XR_XIR_BAD_STRUCTURE;
    uint32_t id=a->indices[value];if (id==UINT32_MAX) return XR_XIR_OK;
    uint64_t mask=UINT64_C(1)<<(id%64u);
    if (use) bits[id/64u]|=mask;else bits[id/64u]&=~mask;
    return XR_XIR_OK;
}
static void view_live_enqueue(ViewLive *a,uint32_t block) {
    if (a->queued[block]) return;
    XR_CHECK(a->pending<a->function->block_count,"each liveness block has at most one pending queue entry");
    a->queue[a->tail++]=block;if (a->tail==a->function->block_count) a->tail=0;
    a->queued[block]=1;++a->pending;
}
static XrXirStatus view_live_graph(ViewLive *a) {
    const XrXirFunction *f=a->function;
    for (uint32_t b=0;b<f->block_count;++b) {

        a->heads[b]=UINT32_MAX;
    }
    a->reachable[0]=1;view_live_enqueue(a,0);
    while (a->pending) {
        uint32_t b=a->queue[a->head++];if (a->head==f->block_count) a->head=0;
        --a->pending;a->queued[b]=0;const XrXirBlock *block=&f->blocks[b];
        const XrXirInstruction *term=&f->instructions[block->first+block->count-1u];
        for (uint32_t e=0;e<3;++e) {

            if (e<2 ? e>=affine_rules[term->op].edges : !block->panic) continue;
            uint32_t target=e<2 ? term->targets[e] : block->panic;
            if (target>=f->block_count) return XR_XIR_BAD_STRUCTURE;
            uint32_t edge=a->edges++;a->predecessors[edge]=b;a->next[edge]=a->heads[target];a->heads[target]=edge;
            if (!a->reachable[target]) {a->reachable[target]=1;view_live_enqueue(a,target);}
        }
    }
    for (uint32_t b=f->block_count;b>0;--b) {

        if (!a->reachable[b-1u]) {a->location.block=b-1u;return XR_XIR_BAD_STRUCTURE;}
        view_live_enqueue(a,b-1u);
    }
    return XR_XIR_OK;
}
static XrXirStatus view_live_edge(ViewLive *a,uint32_t from,uint32_t target,bool panic) {
    XrXirStatus status=view_live_row(a,a->edge,a->inputs+(size_t)target*a->words,false);
    if (status!=XR_XIR_OK || panic) return status;
    const XrXirFunction *f=a->function;const XrXirBlock *block=&f->blocks[target];
    for (uint32_t i=block->first;i<block->first+block->count;++i) {

        const XrXirInstruction *op=&f->instructions[i];if (op->op!=XR_XIR_PHI) break;
        for (uint32_t p=0;p<op->args[1];p+=2) {

            if (f->operands[op->args[0]+p]!=from) continue;
            status=view_live_value(a,a->edge,f->operands[op->args[0]+p+1u],true);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
static uint32_t view_live_operands(const ViewLive *a,const XrXirInstruction *op) {
    if (op->op==XR_XIR_RETURN) return a->function->result!=XR_XIR_UNIT;
    if (op->op==XR_XIR_SLOT_INIT || op->op==XR_XIR_SLOT_STORE) {
        const XrXirDeclarations *d=a->module->declarations;
        return d ? xr_xir_slot_payload_operands(d->slots,d->slot_count,op) : 1u;
    }
    if (op->op==XR_XIR_CELL_NEW || op->op==XR_XIR_CELL_READ || op->op==XR_XIR_CELL_WRITE || op->op==XR_XIR_CELL_LOCAL_WRITE) {
        XrXirType type=op->op==XR_XIR_CELL_NEW ? op->type : xr_xir_operand_type(a->function,op->args[0]);
        return xr_xir_cell_payload_operands(a->module->types,type,op->op);
    }
    return xr_xir_op_uses_operand_table(op->op) ? op->args[1] : affine_rules[op->op].operands;
}
static XrXirStatus view_live_instruction(ViewLive *a,uint32_t i) {
    const XrXirInstruction *op=&a->function->instructions[i];a->location.instruction=i;
    XrXirStatus status=view_live_value(a,a->current,a->function->parameter_count+i,false);
    if (status!=XR_XIR_OK || op->op==XR_XIR_PHI) return status;
    if (op->op==XR_XIR_VIEW_CLEANUP) {
        for (uint32_t p=0;p<op->args[1] && status==XR_XIR_OK;++p)
            status=view_live_value(a,a->current,a->function->operands[op->args[0]+p],false);
        return status;
    }
    for (uint32_t p=0;p<view_live_operands(a,op);++p) {
        uint32_t value=xr_xir_op_uses_operand_table(op->op) ? a->function->operands[op->args[0]+p] : op->args[p];
        status=view_live_value(a,a->current,value,true);if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus view_live_output(ViewLive *a,uint32_t b) {
    const XrXirBlock *block=&a->function->blocks[b];
    const XrXirInstruction *term=&a->function->instructions[block->first+block->count-1u];

    memset(a->current,0,(size_t)a->words*sizeof(*a->current));
    for (uint32_t e=0;e<3;++e) {

        if (e<2 ? e>=affine_rules[term->op].edges : !block->panic) continue;
        uint32_t target=e<2 ? term->targets[e] : block->panic;
        XrXirStatus status=view_live_edge(a,b,target,e==2);
        if (status==XR_XIR_OK) status=view_live_row(a,a->current,a->edge,true);
        if (status!=XR_XIR_OK) return status;
    }
    return view_live_row(a,a->outputs+(size_t)b*a->words,a->current,false);
}
static XrXirStatus view_live_solve(ViewLive *a) {
    const XrXirFunction *f=a->function;
    while (a->pending) {
        uint32_t b=a->queue[a->head++];if (a->head==f->block_count) a->head=0;
        --a->pending;a->queued[b]=0;a->location.block=b;
        XrXirStatus status=view_live_output(a,b);if (status!=XR_XIR_OK) return status;
        const XrXirBlock *block=&f->blocks[b];
        for (uint32_t i=block->first+block->count;i>block->first;--i) {
            status=view_live_instruction(a,i-1u);if (status!=XR_XIR_OK) return status;
        }
        bool changed=false;
        for (uint32_t w=0;w<a->words;++w) {

            uint64_t *input=&a->inputs[(size_t)b*a->words+w];changed|=*input!=a->current[w];*input=a->current[w];
        }
        if (!changed) continue;
        for (uint32_t edge=a->heads[b];edge!=UINT32_MAX;edge=a->next[edge]) {

            view_live_enqueue(a,a->predecessors[edge]);
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus view_live_emit(ViewLive *a,XrXirViewLiveEvent *event) {

    event->function=a->location.function;event->block=a->location.block;
    event->before.values=event->after.values=a->values;event->before.count=event->after.count=a->count;
    event->before.words=event->after.words=a->words;
    return a->visitor(a->user,a->context,event);
}
static XrXirStatus view_live_visit(ViewLive *a) {
    if (!a->visitor) return XR_XIR_OK;
    const XrXirFunction *f=a->function;
    for (uint32_t b=0;b<f->block_count;++b) {

        if (!a->reachable[b]) continue;
        a->location.block=b;a->location.instruction=UINT32_MAX;
        XrXirViewLiveEvent event={.kind=XR_XIR_VIEW_LIVE_ENTRY,.instruction=UINT32_MAX,.target=UINT32_MAX,
            .before={.bits=a->inputs+(size_t)b*a->words},.after={.bits=a->inputs+(size_t)b*a->words}};
        XrXirStatus status=view_live_emit(a,&event);if (status!=XR_XIR_OK) return status;
        status=view_live_row(a,a->current,a->outputs+(size_t)b*a->words,false);if (status!=XR_XIR_OK) return status;
        const XrXirBlock *block=&f->blocks[b];
        for (uint32_t i=block->first+block->count;i>block->first;--i) {
            status=view_live_row(a,a->after,a->current,false);
            if (status==XR_XIR_OK) status=view_live_instruction(a,i-1u);
            if (status!=XR_XIR_OK) return status;
            event=(XrXirViewLiveEvent){.kind=XR_XIR_VIEW_LIVE_INSTRUCTION,.instruction=i-1u,.target=UINT32_MAX,
                .before={.bits=a->current},.after={.bits=a->after}};
            status=view_live_emit(a,&event);if (status!=XR_XIR_OK) return status;
        }
        const XrXirInstruction *term=&f->instructions[block->first+block->count-1u];
        for (uint32_t e=0;e<3;++e) {

            if (e<2 ? e>=affine_rules[term->op].edges : !block->panic) continue;
            uint32_t target=e<2 ? term->targets[e] : block->panic;
            status=view_live_edge(a,b,target,e==2);if (status!=XR_XIR_OK) return status;
            a->location.instruction=block->first+block->count-1u;
            event=(XrXirViewLiveEvent){.kind=XR_XIR_VIEW_LIVE_EDGE,.instruction=a->location.instruction,.target=target,.panic=e==2,
                .before={.bits=a->outputs+(size_t)b*a->words},.after={.bits=a->edge}};
            status=view_live_emit(a,&event);if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_compile_view_liveness_verified(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirViewLiveVisitor visitor,void *user,XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(context) || !module || !module->function_count || !module->functions)
        return XR_XIR_BAD_STRUCTURE;
    uint8_t *flags=NULL;XrXirStatus status=xir_compile_view_flags_new(context,module->types,&flags);
    if (status!=XR_XIR_OK || !flags) return status;
    for (uint32_t f=0;f<module->function_count && status==XR_XIR_OK;++f) {

        ViewLive a={.context=context,.module=module,.function=&module->functions[f],.types=flags,.visitor=visitor,.user=user,
            .location={XR_XIR_OK,f,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE}};
        status=view_live_allocate(&a);
        if (status==XR_XIR_OK && a.count) status=view_live_graph(&a);
        if (status==XR_XIR_OK && a.count) status=view_live_solve(&a);
        if (status==XR_XIR_OK && a.count) status=view_live_visit(&a);
        if (diagnostic && status!=XR_XIR_OK) {*diagnostic=a.location;diagnostic->status=status;}
        view_live_free(&a);
    }
    xr_compile_resources_free(flags);return status;
}
