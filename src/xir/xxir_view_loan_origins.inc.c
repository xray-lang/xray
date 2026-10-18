/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_loan_origins.inc.c - Borrowed scope producers without root collapse
 */
#include "xxir_view_loan_origins_internal.h"
typedef struct ViewLoanQuery {
    ViewRootQuery root;
    ViewOriginSet choices;
    XrXirViewLoanOriginVisitor visitor;
    void *user;
    uint32_t count;
} ViewLoanQuery;

static XrXirStatus view_loan_selected(ViewLoanQuery *q,ViewOriginPair site,uint32_t selection,XrXirType *type) {
    ViewRootCheck *c=q->root.check;*type=xr_xir_operand_type(c->module->functions+site.first,site.second);
    for (uint32_t p=selection;p;p=q->root.selections.pairs[p-1u].second) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
        const XrXirTypeNode *node=xr_xir_type_node(c->module->types,*type);
        uint32_t field=q->root.selections.pairs[p-1u].first;if (!node) return XR_XIR_BAD_TYPE;
        if (field==UINT32_MAX) {
            if (node->kind!=XR_XIR_TYPE_NULLABLE) return XR_XIR_BAD_TYPE;*type=node->element;
        } else {
            if (node->kind!=XR_XIR_TYPE_TUPLE || field>=node->parameter_count) return XR_XIR_BAD_TYPE;
            *type=node->parameters[field].type;
        }
    }
    return view_flags_at(c->module->types,c->flags,*type)?XR_XIR_OK:XR_XIR_BAD_TYPE;
}
static XrXirStatus view_loan_emit(ViewLoanQuery *q,ViewOriginPair site,ViewOriginPair context,
    XrXirViewLoanOriginKind kind,uint32_t source,bool repeated) {
    ViewRootCheck *c=q->root.check;XrXirType type=XR_XIR_UNIT;
    XrXirStatus status=view_loan_selected(q,site,context.second,&type);if (status!=XR_XIR_OK) return status;
    uint32_t choices=0,selections=0;
    for (uint32_t p=context.first;p;p=q->root.paths.pairs[p-1u].second) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;++choices;
    }
    for (uint32_t p=context.second;p;p=q->root.selections.pairs[p-1u].second) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;++selections;
    }
    uint64_t bytes=(uint64_t)choices*sizeof(XrXirViewLoanChoice)+(uint64_t)selections*sizeof(uint32_t);
    if (bytes>SIZE_MAX || c->reserved>c->context->limits.frame_bytes ||
        bytes>c->context->limits.frame_bytes-c->reserved) return XR_XIR_BUDGET;
    unsigned char *storage=bytes?xir_compile_alloc(c->context,(size_t)bytes,&status):NULL;
    if (bytes && !storage) return status;c->reserved+=bytes;
    XrXirViewLoanChoice *branch=choices?(XrXirViewLoanChoice *)storage:NULL;
    uint32_t *selected=selections?(uint32_t *)(storage+(size_t)choices*sizeof(*branch)):NULL,index=0;
    for (uint32_t p=context.first;p;p=q->root.paths.pairs[p-1u].second) {
        if (!xir_compile_work(c->context,sizeof(*branch))) {status=XR_XIR_BUDGET;break;}
        ViewOriginPair choice=q->choices.pairs[q->root.paths.pairs[p-1u].first];
        branch[index++]=(XrXirViewLoanChoice){choice.first,choice.second};
    }
    index=0;
    for (uint32_t p=context.second;status==XR_XIR_OK && p;p=q->root.selections.pairs[p-1u].second) {
        if (!xir_compile_work(c->context,1)) {status=XR_XIR_BUDGET;break;}
        selected[index++]=q->root.selections.pairs[p-1u].first;
    }
    if (status==XR_XIR_OK) {
        XrXirViewLoanOrigin event={site.first,c->location.instruction,site.second,source,kind,type,
            selected,selections,branch,choices,repeated};
        if (q->count==UINT32_MAX || !xir_compile_work(c->context,sizeof(event))) status=XR_XIR_BUDGET;
        else {++q->count;if (q->visitor) status=q->visitor(q->user,c->context,&event);}
    }
    XR_CHECK(c->reserved>=bytes,"loan observations borrow storage from their exact replay owner");
    c->reserved-=bytes;xr_compile_resources_free(storage);return status;
}
static XrXirStatus view_loan_phi(ViewLoanQuery *q,ViewOriginPair site,ViewOriginPair context) {
    ViewRootCheck *c=q->root.check;const XrXirFunction *body=c->module->functions+site.first;
    uint32_t instruction=site.second-body->parameter_count,block=UINT32_MAX;
    for (uint32_t b=0;b<body->block_count;++b) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
        if (instruction>=body->blocks[b].first && instruction-body->blocks[b].first<body->blocks[b].count) block=b;
    }
    if (block==UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=context.first;p;p=q->root.paths.pairs[p-1u].second) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
        if (q->choices.pairs[q->root.paths.pairs[p-1u].first].first==block)
            return view_loan_emit(q,site,context,XR_XIR_VIEW_LOAN_UNRESOLVED,UINT32_MAX,true);
    }
    const XrXirInstruction *op=body->instructions+instruction;
    XR_CHECK(op->op==XR_XIR_PHI && !(op->args[1]&1u),"loan choices use admitted normal PHI pairs");
    for (uint32_t p=0;p<op->args[1];p+=2) {
        uint32_t choice=0,path=0;ViewOriginPair key={block,body->operands[op->args[0]+p]};
        XrXirStatus status=view_root_intern(c,&q->choices,key,&choice);
        if (status==XR_XIR_OK) status=view_root_intern(c,&q->root.paths,(ViewOriginPair){choice,context.first},&path);
        if (status==XR_XIR_OK) status=view_root_push(&q->root,site.first,body->operands[op->args[0]+p+1u],path+1u,context.second);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus view_loan_call(ViewLoanQuery *q,ViewOriginPair site,ViewOriginPair context,
    const XrXirInstruction *op) {
    ViewRootCheck *c=q->root.check;const XrXirFunction *body=c->module->functions+site.first;uint32_t candidate=UINT32_MAX;
    if (op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE) candidate=c->candidates[op->immediate];
    else if (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT) {
        const XrXirTypeNode *signature=xr_xir_callable_signature(c->module->types,
            xr_xir_operand_type(body,(uint32_t)op->immediate));
        if (!signature) return XR_XIR_BAD_TYPE;
        XrXirStatus status=view_return_candidate(c->context,c->module->types,c->flags,signature,&candidate);
        if (status!=XR_XIR_OK) return status;
    }
    if (candidate==UINT32_MAX) return view_loan_emit(q,site,context,XR_XIR_VIEW_LOAN_UNRESOLVED,UINT32_MAX,false);
    if (candidate>=op->args[1]) return XR_XIR_BAD_TYPE;
    return view_loan_emit(q,site,context,XR_XIR_VIEW_LOAN_CALL,body->operands[op->args[0]+candidate],false);
}
static XrXirStatus view_loan_step(ViewLoanQuery *q,ViewOriginPair site,ViewOriginPair context) {
    ViewRootCheck *c=q->root.check;const XrXirFunction *body=c->module->functions+site.first;
    if (!xir_compile_work(c->context,2)) return XR_XIR_BUDGET;
    if (site.second<body->parameter_count) return view_loan_emit(q,site,context,XR_XIR_VIEW_LOAN_INPUT,UINT32_MAX,false);
    const XrXirInstruction *op=body->instructions+site.second-body->parameter_count;
    switch (op->op) {
    case XR_XIR_VIEW_NEW:case XR_XIR_VIEW_REBORROW:
        if (context.second || op->args[1]!=3) return XR_XIR_BAD_STRUCTURE;
        return view_loan_emit(q,site,context,op->op==XR_XIR_VIEW_NEW?XR_XIR_VIEW_LOAN_NEW:XR_XIR_VIEW_LOAN_REBORROW,
            body->operands[op->args[0]],false);
    case XR_XIR_COPY:case XR_XIR_OWNED_RETAIN:
        return view_root_push(&q->root,site.first,op->args[0],context.first,context.second);
    case XR_XIR_PHI:return view_loan_phi(q,site,context);
    case XR_XIR_NULLABLE_NONE:return view_loan_emit(q,site,context,XR_XIR_VIEW_LOAN_ABSENT,UINT32_MAX,false);
    case XR_XIR_NULLABLE_SOME:
        if (!context.second) return view_root_push(&q->root,site.first,op->args[0],context.first,0);
        if (q->root.selections.pairs[context.second-1u].first!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
        return view_root_push(&q->root,site.first,op->args[0],context.first,q->root.selections.pairs[context.second-1u].second);
    case XR_XIR_NULLABLE_UNWRAP:return view_root_select(&q->root,(ViewOriginPair){site.first,op->args[0]},UINT32_MAX,context);
    case XR_XIR_TUPLE_FIELD:return view_root_select(&q->root,(ViewOriginPair){site.first,op->args[0]},(uint32_t)op->immediate,context);
    case XR_XIR_TUPLE_NEW:return view_root_tuple(&q->root,site,op,context);
    case XR_XIR_CALL:case XR_XIR_CALL_INDIRECT:case XR_XIR_CALL_DEFAULT:case XR_XIR_CALL_REQUIREMENT:
        return view_loan_call(q,site,context,op);
    case XR_XIR_INVOKE_RESULT:return view_loan_call(q,site,context,body->instructions+op->immediate);
    default:return view_loan_emit(q,site,context,XR_XIR_VIEW_LOAN_UNRESOLVED,UINT32_MAX,false);
    }
}
static XrXirStatus view_loan_resolve(XirViewRootResolver *resolver,const XirViewRootRequest *request,
    XrXirViewLoanOriginVisitor visitor,void *user) {
    ViewRootCheck *c=&resolver->check;c->location=(XrXirDiagnostic){XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
    XrXirStatus status=view_root_request(c,request);if (status!=XR_XIR_OK) return status;
    if (c->reserved>c->context->limits.frame_bytes || sizeof(ViewLoanQuery)>c->context->limits.frame_bytes-c->reserved)
        return XR_XIR_BUDGET;
    c->reserved+=sizeof(ViewLoanQuery);
    ViewLoanQuery q={.root={.check=c},.visitor=visitor,.user=user};uint32_t selection=0;
    status=view_loan_selected(&q,(ViewOriginPair){request->function,request->value},0,&(XrXirType){0});
    for (uint32_t i=request->selection_count;i && status==XR_XIR_OK;--i) {
        uint32_t index=0;status=view_root_intern(c,&q.root.selections,(ViewOriginPair){request->selection[i-1u],selection},&index);
        selection=index+1u;
    }
    if (status==XR_XIR_OK) status=view_root_push(&q.root,request->function,request->value,0,selection);
    for (uint32_t i=0;i<q.root.tasks.count && status==XR_XIR_OK;++i) {
        ViewOriginPair task=q.root.tasks.pairs[i],site=q.root.sites.pairs[task.first],context=q.root.contexts.pairs[task.second];
        status=view_loan_step(&q,site,context);
    }
    view_root_set_drop(c,&q.root.sites);view_root_set_drop(c,&q.root.tasks);view_root_set_drop(c,&q.root.contexts);
    view_root_set_drop(c,&q.root.paths);view_root_set_drop(c,&q.root.selections);view_root_set_drop(c,&q.choices);
    XR_CHECK(c->reserved>=sizeof(ViewLoanQuery),"loan query scratch remains owned until every borrowed array expires");
    c->reserved-=sizeof(ViewLoanQuery);
    if (status==XR_XIR_OK && !q.count) status=XR_XIR_BAD_VALUE;
    return status;
}
XR_FUNC XrXirStatus xir_compile_view_loan_origins(XirViewRootResolver *resolver,
    const XirViewRootRequest *request,XrXirViewLoanOriginVisitor visitor,void *user,XrXirDiagnostic *diagnostic) {
    if (!resolver) return XR_XIR_BAD_STRUCTURE;if (resolver->busy) return XR_XIR_BAD_STAGE;
    resolver->busy=true;XrXirStatus status=view_loan_resolve(resolver,request,visitor,user);resolver->busy=false;
    ViewRootCheck *c=&resolver->check;
    if (status!=XR_XIR_OK && diagnostic) {*diagnostic=c->location;diagnostic->status=status;}
    return status;
}
