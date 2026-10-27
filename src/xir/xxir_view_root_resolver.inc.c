/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_root_resolver.inc.c - Owned symbolic root replay for actual accesses
 */
struct XirViewRootResolver { ViewRootCheck check; uint8_t *flags; bool busy; };

/* Slot names do not prove distinct cells. Replay concrete initializers;
 * ordinary admission rejects slot rebinding before this query is used. */
static XrXirStatus view_root_symbolic_slot(ViewRootQuery *q,uint32_t slot,ViewOriginPair context) {
    ViewRootCheck *c=q->check;const XrXirDeclarations *d=c->module->declarations;uint32_t found=0;
    if (!d || slot>=d->slot_count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<c->module->function_count;++f) {
        const XrXirFunction *body=c->module->functions+f;
        for (uint32_t i=0;i<body->instruction_count;++i) {

            const XrXirInstruction *op=body->instructions+i;uint32_t value=UINT32_MAX;
            if (op->op==XR_XIR_SLOT_INIT && (uint32_t)op->immediate==slot)
                value=op->args[0];
            else if (op->op==XR_XIR_SLOT_GROUP_INIT) {
                uint32_t first=(uint32_t)((uint64_t)op->immediate>>32),count=(uint32_t)op->immediate;
                if (slot<first || slot-first>=count) continue;
                uint32_t operand=0;
                for (uint32_t p=first;p<slot;++p) {

                    operand+=d->slots[p].type!=XR_XIR_UNIT;
                }
                if (operand>=op->args[1]) return XR_XIR_BAD_STRUCTURE;
                value=body->operands[op->args[0]+operand];
            }
            if (value==UINT32_MAX) continue;
            XrXirStatus status=view_root_push(q,f,value,context.first,context.second);
            if (status!=XR_XIR_OK) return status;++found;
        }
    }
    return found?XR_XIR_OK:view_root_emit(q,(ViewOriginPair){UINT32_MAX,slot},context,XR_XIR_VIEW_ROOT_MODULE);
}
XR_FUNC void xir_compile_view_root_resolver_free(XirViewRootResolver *resolver) {
    if (!resolver) return;
    XR_CHECK(!resolver->busy,"root replay storage is released outside its borrowed callback");
    ViewRootCheck *c=&resolver->check;view_root_set_drop(c,&c->captured);
    xr_compile_resources_free(c->meta);xr_compile_resources_free(c->edges);
    xr_compile_resources_free(resolver->flags);xr_compile_resources_free(resolver);
}
XR_FUNC XrXirStatus xir_compile_view_root_resolver_new(const XrXirCompileContext *context,
    const XrXirModule *module,XirViewRootResolver **output,XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(context) || !module || !module->functions || !module->function_count ||
        !output || *output) return XR_XIR_BAD_STRUCTURE;
    if (!module->types || !module->types->count) return XR_XIR_BAD_TYPE;
    if (sizeof(XirViewRootResolver)>context->limits.frame_bytes ||
        module->types->count>context->limits.frame_bytes-sizeof(XirViewRootResolver)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    XirViewRootResolver *r=xir_compile_calloc(context,1,sizeof(*r),&status);if (!r) return status;
    r->check=(ViewRootCheck){.context=context,.module=module,.reserved=sizeof(*r),.symbolic_parameters=true,
        .location={XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE}};
    status=view_flags_build(context,module->types,&r->flags);r->check.flags=r->flags;
    if (status==XR_XIR_OK) status=view_root_graph(&r->check);
    if (status==XR_XIR_OK) status=view_root_walk(&r->check,true);
    if (status==XR_XIR_OK) {*output=r;return XR_XIR_OK;}
    if (diagnostic) {*diagnostic=r->check.location;diagnostic->status=status;}
    xir_compile_view_root_resolver_free(r);return status;
}
typedef struct ViewRootForward { XrXirViewRootVisitor visitor; void *user; uint32_t count; } ViewRootForward;
static XrXirStatus view_root_forward(void *user,const XrXirCompileContext *context,const XrXirViewRootEvent *event) {
    ViewRootForward *forward=user;

    if (forward->count==UINT32_MAX) return XR_XIR_BUDGET;++forward->count;
    return forward->visitor?forward->visitor(forward->user,context,event):XR_XIR_OK;
}
static XrXirStatus view_root_request(ViewRootCheck *c,const XirViewRootRequest *request) {
    if (!request || request->function>=c->module->function_count ||
        (request->selection_count && !request->selection)) return XR_XIR_BAD_STRUCTURE;
    if (request->selection_count>c->module->types->count) return XR_XIR_BAD_TYPE;
    const XrXirFunction *body=c->module->functions+request->function;
    if (request->value>=(uint64_t)body->parameter_count+body->instruction_count ||
        (request->instruction!=UINT32_MAX && request->instruction>=body->instruction_count)) return XR_XIR_BAD_STRUCTURE;
    c->location=(XrXirDiagnostic){XR_XIR_OK,request->function,UINT32_MAX,request->instruction,XR_XIR_DIAGNOSTIC_NONE};
    for (uint32_t b=0;b<body->block_count;++b) {

        if (request->instruction>=body->blocks[b].first &&
            request->instruction-body->blocks[b].first<body->blocks[b].count) c->location.block=b;
    }
    XrXirType type=xr_xir_operand_type(body,request->value);
    if (!request->selection_count && xr_xir_place_kind(body,request->value)!=XR_XIR_PLACE_NONE) return XR_XIR_OK;
    for (uint32_t i=0;i<request->selection_count;++i) {

        const XrXirTypeNode *node=xr_xir_type_node(c->module->types,type);uint32_t field=request->selection[i];
        if (!node) return XR_XIR_BAD_TYPE;
        if (field==UINT32_MAX) {
            if (node->kind!=XR_XIR_TYPE_NULLABLE) return XR_XIR_BAD_TYPE;type=node->element;
        } else {
            if (node->kind!=XR_XIR_TYPE_TUPLE || field>=node->parameter_count) return XR_XIR_BAD_TYPE;
            type=node->parameters[field].type;
        }
    }
    return xr_xir_type_is_cell(c->module->types,type) || xr_xir_type_is_class(c->module->types,type) ||
        view_flags_at(c->module->types,c->flags,type)?XR_XIR_OK:XR_XIR_BAD_TYPE;
}
static XrXirStatus view_root_resolve(XirViewRootResolver *r,const XirViewRootRequest *request,
    XrXirViewRootVisitor visitor,void *user) {
    ViewRootCheck *c=&r->check;
    c->location=(XrXirDiagnostic){XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
    XrXirStatus status=view_root_request(c,request);if (status!=XR_XIR_OK) return status;
    ViewRootForward forward={visitor,user,0};c->visitor=view_root_forward;c->user=&forward;
    ViewRootQuery q={.check=c};uint32_t selection=0;
    for (uint32_t i=request->selection_count;i && status==XR_XIR_OK;--i) {
        uint32_t index=0;status=view_root_intern(c,&q.selections,(ViewOriginPair){request->selection[i-1u],selection},&index);
        selection=index+1u;
    }
    if (status==XR_XIR_OK) status=view_root_push(&q,request->function,request->value,0,selection);
    status=view_root_drain(&q,status);c->visitor=NULL;c->user=NULL;
    return status==XR_XIR_OK && !forward.count?XR_XIR_BAD_VALUE:status;
}
XR_FUNC XrXirStatus xir_compile_view_root_resolver_query(XirViewRootResolver *r,
    const XirViewRootRequest *request,XrXirViewRootVisitor visitor,void *user,XrXirDiagnostic *diagnostic) {
    if (!r) return XR_XIR_BAD_STRUCTURE;if (r->busy) return XR_XIR_BAD_STAGE;
    r->busy=true;XrXirStatus status=view_root_resolve(r,request,visitor,user);r->busy=false;
    if (status!=XR_XIR_OK && diagnostic) {*diagnostic=r->check.location;diagnostic->status=status;}
    return status;
}

#include "xxir_view_root_relation.inc.c"
#include "xxir_view_loan_origins.inc.c"
#include "xxir_view_access.inc.c"
