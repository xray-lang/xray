/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_root_graph.inc.c - Actual parameter bindings and bounded root sets
 */
#include "xxir_view_roots_internal.h"
#include "xxir_operand_roles.h"
#include "../base/xchecks.h"
typedef struct ViewRootEdge {uint32_t function,value,next;} ViewRootEdge;
typedef struct ViewRootCheck {
    const XrXirCompileContext *context;
    const XrXirModule *module;
    const uint8_t *flags;
    XrXirViewRootVisitor visitor;
    void *user;
    uint32_t *meta,*offsets,*captures,*candidates,*heads;
    ViewRootEdge *edges;
    uint32_t edge_count;
    uint64_t reserved;
    ViewOriginSet captured;
    XrXirDiagnostic location;
    bool symbolic_parameters;
} ViewRootCheck;
typedef struct ViewRootQuery {
    ViewRootCheck *check;
    ViewOriginSet sites,tasks,contexts,paths,selections;
    bool capture;
} ViewRootQuery;
static uint64_t view_root_set_bytes(const ViewOriginSet *set) {
    return (uint64_t)set->capacity*(sizeof(ViewOriginPair)+2u*sizeof(uint32_t));
}
static XrXirStatus view_root_intern(ViewRootCheck *c,ViewOriginSet *set,
    ViewOriginPair pair,uint32_t *output) {
    uint64_t before=view_root_set_bytes(set);
    if (set->count==set->capacity) {
        /* Resize temporarily holds both old arrays and their replacements. */
        uint64_t growth=before ? before*2u : 8u*(sizeof(ViewOriginPair)+2u*sizeof(uint32_t));
        if (c->reserved>c->context->limits.frame_bytes || growth>c->context->limits.frame_bytes-c->reserved)
            return XR_XIR_BUDGET;
    }
    XrXirStatus status=view_origin_intern(c->context,set,pair,output);
    /* A completed resize remains owned even if later work charging fails. */
    c->reserved+=view_root_set_bytes(set)-before;
    return status;
}
static void view_root_set_drop(ViewRootCheck *c,ViewOriginSet *set) {
    uint64_t bytes=view_root_set_bytes(set);
    XR_CHECK(c->reserved>=bytes,"root scratch belongs to the same finite metadata owner");
    c->reserved-=bytes;view_origin_set_free(set);
}
static XrXirStatus view_root_find(ViewRootCheck *c,ViewOriginPair key,bool *found) {
    *found=false;if (!c->captured.slot_count) return XR_XIR_OK;
    if (!xir_compile_work(c->context,3)) return XR_XIR_BUDGET;
    uint32_t at=view_origin_hash(key.first,key.second)&(c->captured.slot_count-1u);
    while (c->captured.slots[at]) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
        ViewOriginPair p=c->captured.pairs[c->captured.slots[at]-1u];
        if (p.first==key.first && p.second==key.second) {*found=true;break;}
        at=(at+1u)&(c->captured.slot_count-1u);
    }
    return XR_XIR_OK;
}
static XrXirStatus view_root_target(ViewRootCheck *c,uint32_t f,const XrXirInstruction *op,
    uint32_t *target,uint32_t *prefix) {
    *target=UINT32_MAX;*prefix=0;
    if (op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE || op->op==XR_XIR_FUNCTION_REF || op->op==XR_XIR_CLEANUP_REGISTER) {
        *target=(uint32_t)op->immediate;return XR_XIR_OK;
    }
    if (op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) return XR_XIR_OK;
    const XrXirFunction *body=&c->module->functions[f];uint32_t value=(uint32_t)op->immediate;
    for (uint32_t depth=0;value>=body->parameter_count && depth<body->instruction_count;++depth) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
        const XrXirInstruction *producer=&body->instructions[value-body->parameter_count];
        if (producer->op==XR_XIR_FUNCTION_REF) {*target=(uint32_t)producer->immediate;*prefix=producer->args[1];break;}
        if (producer->op!=XR_XIR_COPY && producer->op!=XR_XIR_OWNED_RETAIN && producer->op!=XR_XIR_FUNCTION_WEAKEN) break;
        value=producer->args[0];
    }
    return XR_XIR_OK;
}
static bool view_root_tracked(const ViewRootCheck *c,XrXirType type) {
    return xr_xir_type_is_cell(c->module->types,type) || xr_xir_type_is_class(c->module->types,type) ||
        view_flags_at(c->module->types,c->flags,type)!=0;
}
static XrXirStatus view_root_edges(ViewRootCheck *c,bool fill) {
    uint32_t count=0;
    for (uint32_t f=0;f<c->module->function_count;++f) {
        const XrXirFunction *body=&c->module->functions[f];
        for (uint32_t i=0;i<body->instruction_count;++i) {
            if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
            const XrXirInstruction *op=&body->instructions[i];uint32_t target,prefix;
            XrXirStatus status=view_root_target(c,f,op,&target,&prefix);if (status!=XR_XIR_OK) return status;
            if (target==UINT32_MAX) continue;
            if (target>=c->module->function_count || prefix>c->module->functions[target].parameter_count ||
                op->args[1]>c->module->functions[target].parameter_count-prefix) return XR_XIR_BAD_STRUCTURE;
            for (uint32_t p=0;p<op->args[1];++p) {
                if (!xir_compile_work(c->context,3)) return XR_XIR_BUDGET;
                if (!view_root_tracked(c,c->module->functions[target].parameters[prefix+p])) continue;
                if (count==UINT32_MAX) return XR_XIR_BUDGET;
                if (fill) {
                    uint32_t slot=c->offsets[target]+prefix+p;
                    c->edges[count]=(ViewRootEdge){f,body->operands[op->args[0]+p],c->heads[slot]};c->heads[slot]=count;
                }
                ++count;
            }
        }
    }
    if (fill) return count==c->edge_count ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
    c->edge_count=count;return XR_XIR_OK;
}
static XrXirStatus view_root_graph(ViewRootCheck *c) {
    uint64_t parameters=0,functions=c->module->function_count;
    for (uint32_t f=0;f<functions;++f) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
        parameters+=c->module->functions[f].parameter_count;
    }
    if (parameters>UINT32_MAX || functions*3u+1u+parameters>SIZE_MAX/sizeof(uint32_t)) return XR_XIR_BUDGET;
    uint64_t bytes=(functions*3u+1u+parameters)*sizeof(uint32_t)+c->module->types->count;
    if (c->reserved>c->context->limits.frame_bytes || bytes>c->context->limits.frame_bytes-c->reserved)
        return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    c->meta=xir_compile_calloc(c->context,(size_t)(functions*3u+1u+parameters),sizeof(uint32_t),&status);
    if (!c->meta) return status;
    c->reserved+=bytes;c->offsets=c->meta;c->captures=c->offsets+functions+1u;
    c->candidates=c->captures+functions;c->heads=c->candidates+functions;
    uint32_t offset=0;
    for (uint32_t f=0;f<functions;++f) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
        c->offsets[f]=offset;offset+=c->module->functions[f].parameter_count;
    }
    c->offsets[functions]=offset;
    for (uint32_t p=0;p<offset;++p) {if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;c->heads[p]=UINT32_MAX;}
    status=view_origin_candidates(c->context,c->module,c->flags,c->captures,c->candidates);
    if (status==XR_XIR_OK) status=view_root_edges(c,false);
    uint64_t edge_bytes=(uint64_t)c->edge_count*sizeof(*c->edges);
    if (status!=XR_XIR_OK) return status;
    if (edge_bytes>SIZE_MAX || edge_bytes>c->context->limits.frame_bytes-c->reserved) return XR_XIR_BUDGET;
    if (edge_bytes) {
        c->edges=xir_compile_alloc(c->context,(size_t)edge_bytes,&status);if (!c->edges) return status;
        c->reserved+=edge_bytes;
    }
    return view_root_edges(c,true);
}
static XrXirStatus view_root_push(ViewRootQuery *q,uint32_t f,uint32_t value,uint32_t path,uint32_t selection) {
    ViewRootCheck *c=q->check;const XrXirFunction *body=&c->module->functions[f];uint32_t site=0,context=0,ignored=0;
    if (value>=(uint64_t)body->parameter_count+body->instruction_count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=view_root_intern(c,&q->sites,(ViewOriginPair){f,value},&site);
    if (status==XR_XIR_OK) status=view_root_intern(c,&q->contexts,(ViewOriginPair){path,selection},&context);
    return status==XR_XIR_OK ? view_root_intern(c,&q->tasks,(ViewOriginPair){site,context},&ignored) : status;
}
static XrXirStatus view_root_select(ViewRootQuery *q,ViewOriginPair site,uint32_t selector,
    ViewOriginPair context) {
    uint32_t selection;XrXirStatus status=view_root_intern(q->check,&q->selections,
        (ViewOriginPair){selector,context.second},&selection);
    return status==XR_XIR_OK ? view_root_push(q,site.first,site.second,context.first,selection+1u) : status;
}
