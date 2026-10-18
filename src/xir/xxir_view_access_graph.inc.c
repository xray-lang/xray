/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_access_graph.inc.c - Owned scope observations and repeatable blocks
 */
#include "xxir_view_access_internal.h"
#include "xxir_view_may_owned_internal.h"
#include "xxir_view_owner_path_internal.h"
typedef struct ViewAccessRule { uint8_t operands,edges; } ViewAccessRule;
static const ViewAccessRule view_access_rules[XR_XIR_OP_COUNT]={
    {0,0},
#define XR_XIR_OP(name,stages,result,operands,edges,terminal) {operands,edges},
#include "xxir_ops.def"
#undef XR_XIR_OP
};
typedef struct ViewAccessOrigin {
    struct ViewAccessOrigin *next;
    XrXirViewLoanOrigin event;
    uint64_t bytes;
    uint32_t queried_value;
} ViewAccessOrigin;
typedef struct ViewAccessCache { ViewAccessOrigin *origins;bool ready; } ViewAccessCache;
typedef struct ViewAccess {
    XirViewRootResolver *resolver;
    const XirViewAccessCells *cells;
    const XrXirFunction *body;
    uint32_t function,instruction;
    ViewAccessCache *cache;
    uint32_t *queue;
    uint8_t *seen,*repeated;
    uint64_t function_bytes,initial_reservation;
} ViewAccess;
static void *view_access_allocate(ViewAccess *a,uint64_t bytes,XrXirStatus *status) {
    ViewRootCheck *c=&a->resolver->check;
    if (bytes>SIZE_MAX || c->reserved>c->context->limits.frame_bytes ||
        bytes>c->context->limits.frame_bytes-c->reserved) {*status=XR_XIR_BUDGET;return NULL;}
    if (!xir_compile_work(c->context,bytes)) {*status=XR_XIR_BUDGET;return NULL;}
    void *p=xir_compile_calloc(c->context,1,(size_t)bytes,status);if (p) c->reserved+=bytes;return p;
}
static void view_access_release(ViewAccess *a,void *p,uint64_t bytes) {
    if (!p) return;ViewRootCheck *c=&a->resolver->check;
    XR_CHECK(c->reserved>=bytes,"access observations remain owned by their exact compile ledger");
    c->reserved-=bytes;xr_compile_resources_free(p);
}
static void view_access_function_free(ViewAccess *a) {
    if (a->cache) for (uint32_t v=0;v<a->body->parameter_count+a->body->instruction_count;++v) {
        ViewAccessOrigin *p=a->cache[v].origins;
        while (p) {ViewAccessOrigin *next=p->next;view_access_release(a,p,p->bytes);p=next;}
    }
    view_access_release(a,a->cache,a->function_bytes);a->cache=NULL;a->function_bytes=0;
}
static XrXirStatus view_access_function_new(ViewAccess *a,uint32_t function) {
    ViewRootCheck *c=&a->resolver->check;a->function=function;a->body=c->module->functions+function;
    uint64_t values=(uint64_t)a->body->parameter_count+a->body->instruction_count,blocks=a->body->block_count;
    if (values>UINT32_MAX) return XR_XIR_BUDGET;
    uint64_t bytes=values*sizeof(ViewAccessCache)+blocks*(sizeof(uint32_t)+2u);
    XrXirStatus status=XR_XIR_OK;a->cache=view_access_allocate(a,bytes,&status);if (!a->cache) return status;
    a->function_bytes=bytes;a->queue=(uint32_t *)(a->cache+values);a->seen=(uint8_t *)(a->queue+blocks);a->repeated=a->seen+blocks;
    for (uint32_t start=0;start<blocks;++start) {
        if (!xir_compile_work(c->context,blocks)) return XR_XIR_BUDGET;
        memset(a->seen,0,(size_t)blocks);uint32_t head=0,tail=1;a->queue[0]=start;a->seen[start]=1;
        while (head<tail && !a->repeated[start]) {
            uint32_t b=a->queue[head++];const XrXirBlock *block=a->body->blocks+b;
            const XrXirInstruction *term=a->body->instructions+block->first+block->count-1u;
            for (uint32_t e=0;e<3;++e) {
                if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
                if (e<2?e>=view_access_rules[term->op].edges:!block->panic) continue;
                uint32_t target=e<2?term->targets[e]:block->panic;
                if (target==start) {a->repeated[start]=1;break;}
                if (!a->seen[target]) {
                    XR_CHECK(tail<blocks,"each reachable block enters the finite queue once");
                    a->seen[target]=1;a->queue[tail++]=target;
                }
            }
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus view_access_stable(ViewAccess *a,uint32_t value,bool *stable) {
    *stable=value<a->body->parameter_count;if (*stable) return XR_XIR_OK;
    uint32_t instruction=value-a->body->parameter_count;
    for (uint32_t b=0;b<a->body->block_count;++b) {
        if (!xir_compile_work(a->resolver->check.context,1)) return XR_XIR_BUDGET;
        const XrXirBlock *block=a->body->blocks+b;
        if (instruction>=block->first && instruction-block->first<block->count) {*stable=!a->repeated[b];return XR_XIR_OK;}
    }
    return XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus view_access_prefix_scope(ViewAccess *a,uint32_t value,bool *current) {
    *current=false;
    if (value<a->body->parameter_count) return XR_XIR_OK;
    uint32_t producer=value-a->body->parameter_count;
    for (uint32_t b=0;b<a->body->block_count;++b) {
        if (!xir_compile_work(a->resolver->check.context,1)) return XR_XIR_BUDGET;
        const XrXirBlock *block=a->body->blocks+b;
        if (a->instruction<block->first || a->instruction-block->first>=block->count) continue;
        /* No CFG edge can replace a direct producer between these two points.
         * A wrapper that merely names this producer can retain an older epoch. */
        *current=producer>=block->first && producer<a->instruction;return XR_XIR_OK;
    }
    return XR_XIR_BAD_STRUCTURE;
}
typedef struct ViewAccessReceiving { ViewAccess *access;ViewAccessCache *cache;uint32_t value; } ViewAccessReceiving;
static XrXirStatus view_access_save(void *user,const XrXirCompileContext *context,const XrXirViewLoanOrigin *event) {
    ViewAccessReceiving *r=user;uint64_t choices=(uint64_t)event->choice_count*sizeof(*event->choices);
    uint64_t selections=(uint64_t)event->selection_count*sizeof(uint32_t),bytes=sizeof(ViewAccessOrigin)+choices+selections;
    (void)context;XrXirStatus status=XR_XIR_OK;ViewAccessOrigin *p=view_access_allocate(r->access,bytes,&status);if (!p) return status;
    p->bytes=bytes;p->event=*event;p->queried_value=r->value;p->next=r->cache->origins;r->cache->origins=p;
    XrXirViewLoanChoice *branch=(XrXirViewLoanChoice *)(p+1);uint32_t *selected=(uint32_t *)((unsigned char *)branch+choices);
    if (choices) memcpy(branch,event->choices,(size_t)choices);if (selections) memcpy(selected,event->selection,(size_t)selections);
    p->event.choices=event->choice_count?branch:NULL;p->event.selection=event->selection_count?selected:NULL;return XR_XIR_OK;
}
static XrXirStatus view_access_origins(ViewAccess *a,uint32_t value,ViewAccessOrigin **output) {
    if (value>=(uint64_t)a->body->parameter_count+a->body->instruction_count) return XR_XIR_BAD_STRUCTURE;
    ViewAccessCache *cache=a->cache+value;
    if (!cache->ready) {
        ViewAccessReceiving receiving={a,cache,value};XirViewRootRequest request={a->function,value,a->instruction,NULL,0};
        XrXirStatus status=xir_compile_view_loan_origins(a->resolver,&request,view_access_save,&receiving,NULL);
        if (status!=XR_XIR_OK) return status;cache->ready=true;
    }
    *output=cache->origins;return XR_XIR_OK;
}
static bool view_access_leaf(const ViewAccess *a,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(a->resolver->check.module->types,type);
    return node && (node->kind==XR_XIR_TYPE_SLICE || node->kind==XR_XIR_TYPE_MUT_SLICE);
}
static bool view_access_mutable(const ViewAccess *a,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(a->resolver->check.module->types,type);
    return node && node->kind==XR_XIR_TYPE_MUT_SLICE;
}
static bool view_access_shared(const ViewAccess *a,XrXirType type) {
    const ViewRootCheck *c=&a->resolver->check;uint32_t flags=view_flags_at(c->module->types,c->flags,type);
    return flags && !(flags&XR_XIR_VIEW_MUTABLE);
}
