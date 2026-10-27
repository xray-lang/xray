/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_may_owned.inc.c - Forward possible-owner union and panic prefixes
 *
 * KEY CONCEPT:
 *   Reuse actual affine transfers without changing the strict checker.
 */
#include "xxir_view_may_owned_internal.h"

typedef struct ViewMayOwner {
    AffineCheck affine;
    XrXirViewOwnerVisitor visitor;
    void *user;
    uint64_t *extra,*before,*panic;
    uint32_t *values;
    uint64_t frame_bytes;
} ViewMayOwner;

static XrXirStatus view_may_allocate(ViewMayOwner *m) {
    AffineCheck *a=&m->affine;const XrXirFunction *f=a->function;
    uint64_t values=(uint64_t)f->parameter_count+f->instruction_count,count=0;
    if (values>UINT32_MAX) return XR_XIR_BUDGET;
    for (uint32_t v=0;v<(uint32_t)values;++v) {

        count+=affine_owner(a,v);
    }
    if (!count) return XR_XIR_OK;
    uint64_t words=(count+63u)/64u,extra=words*2u*sizeof(uint64_t)+count*sizeof(uint32_t);
    uint64_t bytes=sizeof(*m)+a->module->types->count+values*sizeof(uint32_t)+
        (uint64_t)f->block_count*5u+((uint64_t)f->block_count+4u)*words*sizeof(uint64_t)+extra;
    if (extra>SIZE_MAX || bytes>a->context->limits.frame_bytes) return XR_XIR_BUDGET;
    m->frame_bytes=bytes;
    XrXirStatus status=affine_allocate(a);if (status!=XR_XIR_OK) return status;
    XR_CHECK(a->count==count && a->words==words,"possible owners share the actual affine inventory");
    m->extra=xir_compile_calloc(a->context,1,(size_t)extra,&status);if (!m->extra) return status;
    m->before=m->extra;m->panic=m->before+a->words;m->values=(uint32_t *)(m->panic+a->words);
    for (uint32_t v=0;v<(uint32_t)values;++v) {

        if (a->indices[v]!=UINT32_MAX) m->values[a->indices[v]]=v;
    }
    /* The strict checker starts at top for intersection. This analysis starts
     * at bottom for union and must not retain that strict initialization. */
    for (uint32_t b=0;b<f->block_count;++b) for (uint32_t w=0;w<a->words;++w) {

        a->inputs[(size_t)b*a->words+w]=b?0:a->initial[w];
    }
    return affine_reachable(a);
}
static XrXirStatus view_may_emit(ViewMayOwner *m,XrXirViewOwnerEvent *event) {
    AffineCheck *a=&m->affine;

    event->function=a->location.function;event->block=a->location.block;
    event->before.values=event->after.values=m->values;
    event->before.count=event->after.count=a->count;event->before.words=event->after.words=a->words;
    return m->visitor(m->user,a->context,event);
}
static XrXirStatus view_may_transfer(ViewMayOwner *m,uint32_t b,bool emit) {
    AffineCheck *a=&m->affine;a->location.block=b;a->location.instruction=UINT32_MAX;
    XrXirStatus status=affine_copy(a,a->current,a->inputs+(size_t)b*a->words);
    if (status==XR_XIR_OK) status=affine_copy(a,m->panic,a->current);
    if (status!=XR_XIR_OK) return status;
    if (emit) {
        XrXirViewOwnerEvent event={.kind=XR_XIR_VIEW_OWNER_ENTRY,.instruction=UINT32_MAX,.target=UINT32_MAX,
            .before={.bits=a->current},.after={.bits=a->current}};
        status=view_may_emit(m,&event);if (status!=XR_XIR_OK) return status;
    }
    const XrXirBlock *block=a->function->blocks+b;
    for (uint32_t i=block->first;i<block->first+block->count;++i) {

        if (emit) {status=affine_copy(a,m->before,a->current);if (status!=XR_XIR_OK) return status;}
        status=affine_instruction(a,i,false);if (status!=XR_XIR_OK) return status;
        for (uint32_t w=0;w<a->words;++w) {

            m->panic[w]|=a->current[w];
        }
        if (emit) {
            XrXirViewOwnerEvent event={.kind=XR_XIR_VIEW_OWNER_INSTRUCTION,.instruction=i,.target=UINT32_MAX,
                .before={.bits=m->before},.after={.bits=a->current}};
            status=view_may_emit(m,&event);if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus view_may_edges(ViewMayOwner *m,uint32_t b,bool emit,bool *changed) {
    AffineCheck *a=&m->affine;const XrXirBlock *block=a->function->blocks+b;
    uint32_t last=block->first+block->count-1u;const XrXirInstruction *term=a->function->instructions+last;
    for (uint32_t e=0;e<3;++e) {

        if (e<2?e>=affine_rules[term->op].edges:!block->panic) continue;
        uint32_t target=e<2?term->targets[e]:block->panic;
        XrXirStatus status=affine_copy(a,a->edge,e<2?a->current:m->panic);
        if (status==XR_XIR_OK && e<2) status=affine_phi_edge(a,b,target,false);
        if (status!=XR_XIR_OK) return status;
        if (emit) {
            a->location.instruction=last;
            XrXirViewOwnerEvent event={.kind=XR_XIR_VIEW_OWNER_EDGE,.instruction=last,.target=target,.panic=e==2,
                .before={.bits=a->current},.after={.bits=a->edge}};
            status=view_may_emit(m,&event);if (status!=XR_XIR_OK) return status;
        } else for (uint32_t w=0;w<a->words;++w) {

            uint64_t *input=a->inputs+(size_t)target*a->words+w,meet=*input|a->edge[w];
            if (*input!=meet) {*input=meet;*changed=true;}
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus view_may_function(ViewMayOwner *m) {
    AffineCheck *a=&m->affine;XrXirStatus status=view_may_allocate(m);
    if (status!=XR_XIR_OK || !a->count) return status;
    bool changed=true;
    while (changed) {
        changed=false;
        for (uint32_t b=0;b<a->function->block_count;++b) {

            if (!a->reachable[b]) continue;
            status=view_may_transfer(m,b,false);
            if (status==XR_XIR_OK) status=view_may_edges(m,b,false,&changed);
            if (status!=XR_XIR_OK) return status;
        }
    }
    if (!m->visitor) return XR_XIR_OK;
    for (uint32_t b=0;b<a->function->block_count;++b) {

        if (!a->reachable[b]) continue;
        status=view_may_transfer(m,b,true);
        if (status==XR_XIR_OK) status=view_may_edges(m,b,true,NULL);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_compile_view_may_owned_verified(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirViewOwnerVisitor visitor,void *user,XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(context) || !module || !module->functions || !module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    if (module->types && (sizeof(ViewMayOwner)>context->limits.frame_bytes ||
        module->types->count>context->limits.frame_bytes-sizeof(ViewMayOwner))) return XR_XIR_BUDGET;
    uint8_t *flags=NULL;XrXirStatus status=xir_compile_view_flags_new(context,module->types,&flags);
    if (status!=XR_XIR_OK || !flags) return status;
    for (uint32_t f=0;f<module->function_count;++f) {

        ViewMayOwner m={.affine={.context=context,.module=module,.function=module->functions+f,.types=flags,
            .location={XR_XIR_OK,f,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE}},.visitor=visitor,.user=user};
        status=view_may_function(&m);
        if (status!=XR_XIR_OK && diagnostic) {*diagnostic=m.affine.location;diagnostic->status=status;}
        xr_compile_resources_free(m.extra);affine_free(&m.affine);if (status!=XR_XIR_OK) break;
    }
    xr_compile_resources_free(flags);return status;
}

typedef struct ViewMayScoped {
    ViewMayOwner *owner;
    XrXirViewOwnerVisitor visitor;
    void *user;
} ViewMayScoped;
static XrXirStatus view_may_scoped_visit(void *user,const XrXirCompileContext *context,
    const XrXirViewOwnerEvent *event) {
    ViewMayScoped *scope=user;
    XR_CHECK(scope->owner->frame_bytes<=context->limits.frame_bytes,"nested analysis has prepaid its resident frame");
    XrXirCompileContext remaining=*context;remaining.limits.frame_bytes-=scope->owner->frame_bytes;
    return scope->visitor(scope->user,&remaining,event);
}
XR_FUNC XrXirStatus xir_compile_view_may_owned_function_scoped(const XrXirCompileContext *context,
    const XrXirModule *module,uint32_t function,XrXirViewOwnerVisitor visitor,void *user,XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(context) || !module || !module->functions || function>=module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    uint64_t fixed=sizeof(ViewMayScoped)+sizeof(XrXirCompileContext);
    if (fixed>context->limits.frame_bytes) return XR_XIR_BUDGET;
    XrXirCompileContext limited=*context;limited.limits.frame_bytes-=fixed;
    if (module->types && (sizeof(ViewMayOwner)>limited.limits.frame_bytes ||
        module->types->count>limited.limits.frame_bytes-sizeof(ViewMayOwner))) return XR_XIR_BUDGET;
    uint8_t *flags=NULL;XrXirStatus status=xir_compile_view_flags_new(&limited,module->types,&flags);
    if (status!=XR_XIR_OK || !flags) return status;
    ViewMayOwner m={.affine={.context=&limited,.module=module,.function=module->functions+function,.types=flags,
        .location={XR_XIR_OK,function,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE}}};
    ViewMayScoped scope={&m,visitor,user};m.visitor=visitor?view_may_scoped_visit:NULL;m.user=&scope;
    if (module->declarations && module->declarations->functions[function].native_callable)
        status=xir_native_callable_verify(module,function);
    else status=view_may_function(&m);
    if (status!=XR_XIR_OK && diagnostic) {*diagnostic=m.affine.location;diagnostic->status=status;}
    xr_compile_resources_free(m.extra);affine_free(&m.affine);xr_compile_resources_free(flags);return status;
}

#include "xxir_view_owner_path.inc.c"
