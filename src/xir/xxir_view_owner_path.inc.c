/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_owner_path.inc.c - Conditional owner replay without path expansion
 *
 * KEY CONCEPT:
 *   Acyclic waypoints compose linear owner transfers between real CFG edges.
 */
#include "xxir_view_owner_path_graph.inc.c"

static XrXirStatus owner_path_edges(ViewOwnerPath *p,uint32_t b,bool *changed) {
    AffineCheck *a=&p->may.affine;
    for (uint32_t e=0;e<3;++e) {
        uint32_t target=0;bool present=false;XrXirStatus status=owner_path_edge(p,b,e,true,&target,&present);
        if (status!=XR_XIR_OK) return status;if (!present) continue;
        XR_CHECK(a->reachable[target],"conditional propagation stays inside its computed reachable segment");
        status=affine_copy(a,a->edge,e<2?a->current:p->may.panic);
        if (status==XR_XIR_OK && e<2) status=affine_phi_edge(a,b,target,false);
        if (status!=XR_XIR_OK) return status;
        for (uint32_t w=0;w<a->words;++w) {
            if (!xir_compile_work(a->context,1)) return XR_XIR_BUDGET;
            uint64_t *input=a->inputs+(size_t)target*a->words+w,meet=*input|a->edge[w];
            if (*input!=meet) {*input=meet;*changed=true;}
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus owner_path_phase(ViewOwnerPath *p,uint32_t start,uint32_t stop,bool *reached) {
    AffineCheck *a=&p->may.affine;bool repeated=false;*reached=false;
    XrXirStatus status=owner_path_walk(p,start,stop,true,&repeated);if (status!=XR_XIR_OK) return status;
    if (stop!=UINT32_MAX && !a->reachable[stop]) return XR_XIR_OK;
    uint64_t cells=(uint64_t)a->function->block_count*a->words;
    if (!xir_compile_work(a->context,cells)) return XR_XIR_BUDGET;
    memset(a->inputs,0,(size_t)cells*sizeof(*a->inputs));
    status=affine_copy(a,a->inputs+(size_t)start*a->words,a->initial);if (status!=XR_XIR_OK) return status;
    bool changed=true;
    while (changed) {
        changed=false;
        for (uint32_t b=0;b<a->function->block_count;++b) {
            if (!xir_compile_work(a->context,1)) return XR_XIR_BUDGET;
            if (!a->reachable[b] || b==stop) continue;a->location.block=b;
            status=view_may_transfer(&p->may,b,false);
            if (status==XR_XIR_OK) status=owner_path_edges(p,b,&changed);if (status!=XR_XIR_OK) return status;
        }
    }
    if (stop!=UINT32_MAX) status=affine_copy(a,a->initial,a->inputs+(size_t)stop*a->words);
    *reached=status==XR_XIR_OK;return status;
}
static XrXirStatus owner_path_result(ViewOwnerPath *p,bool *possible) {
    AffineCheck *a=&p->may.affine;uint32_t start=0;bool reached=false;*possible=false;
    XrXirStatus status=owner_path_choices(p);if (status==XR_XIR_OK) status=owner_path_order(p);
    if (status!=XR_XIR_OK || p->conflicting) return status;
    for (uint32_t i=0;i<p->count;++i) {
        status=owner_path_phase(p,start,p->points[i].block,&reached);
        if (status!=XR_XIR_OK || !reached) return status;start=p->points[i].block;
    }
    status=owner_path_phase(p,start,UINT32_MAX,&reached);if (status!=XR_XIR_OK) return status;
    uint32_t block=UINT32_MAX;
    for (uint32_t b=0;b<a->function->block_count;++b) {
        if (!xir_compile_work(a->context,1)) return XR_XIR_BUDGET;
        const XrXirBlock *body=a->function->blocks+b;
        if (p->request->instruction>=body->first && p->request->instruction-body->first<body->count) block=b;
    }
    XR_CHECK(block!=UINT32_MAX,"ordinary admission assigns the queried instruction to one block");
    if (!a->reachable[block]) return XR_XIR_OK;
    status=affine_copy(a,a->current,a->inputs+(size_t)block*a->words);
    for (uint32_t i=a->function->blocks[block].first;i<p->request->instruction && status==XR_XIR_OK;++i) {
        if (!xir_compile_work(a->context,1)) return XR_XIR_BUDGET;status=affine_instruction(a,i,false);
    }
    if (status==XR_XIR_OK) *possible=affine_has(a->current,affine_index(a,p->request->owner));return status;
}
XR_FUNC XrXirStatus xir_compile_view_owner_path_possible(const XrXirCompileContext *context,
    const XrXirModule *module,const XirViewOwnerPathRequest *request,bool *possible,XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(context) || !module || !module->functions || !request || !possible ||
        request->function>=module->function_count || (request->left_count && !request->left) ||
        (request->right_count && !request->right)) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=module->functions+request->function;
    if (request->instruction>=function->instruction_count || request->left_count>function->block_count ||
        request->right_count>function->block_count || request->owner>=(uint64_t)function->parameter_count+function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    if (!module->types || !module->types->count) return XR_XIR_BAD_TYPE;
    uint64_t points=((uint64_t)request->left_count+request->right_count)*sizeof(ViewOwnerWaypoint);
    uint64_t fixed=sizeof(ViewOwnerPath)-sizeof(ViewMayOwner)+sizeof(XrXirCompileContext);
    if (points>SIZE_MAX || fixed>context->limits.frame_bytes || points>context->limits.frame_bytes-fixed)
        return XR_XIR_BUDGET;
    XrXirCompileContext limited=*context;limited.limits.frame_bytes-=fixed+points;
    if (sizeof(ViewMayOwner)>limited.limits.frame_bytes || module->types->count>limited.limits.frame_bytes-sizeof(ViewMayOwner))
        return XR_XIR_BUDGET;
    ViewOwnerPath p={.may={.affine={.context=&limited,.module=module,.function=function,
        .location={XR_XIR_OK,request->function,UINT32_MAX,request->instruction,XR_XIR_DIAGNOSTIC_NONE}}},.request=request};
    XrXirStatus status=XR_XIR_OK;uint8_t *flags=NULL;bool result=false;
    if (points) p.points=xir_compile_calloc(context,1,(size_t)points,&status);
    if (status==XR_XIR_OK) status=xir_compile_view_flags_new(&limited,module->types,&flags);p.may.affine.types=flags;
    if (status==XR_XIR_OK && (!flags || !affine_owner(&p.may.affine,request->owner))) status=XR_XIR_BAD_TYPE;
    if (status==XR_XIR_OK) status=view_may_allocate(&p.may);
    if (status==XR_XIR_OK) status=owner_path_result(&p,&result);
    if (status!=XR_XIR_OK && diagnostic) {*diagnostic=p.may.affine.location;diagnostic->status=status;}
    xr_compile_resources_free(p.may.extra);affine_free(&p.may.affine);
    xr_compile_resources_free(flags);xr_compile_resources_free(p.points);if (status==XR_XIR_OK) *possible=result;return status;
}
