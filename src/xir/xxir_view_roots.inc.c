/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_roots.inc.c - Concrete Cell paths and explicit unresolved roots
 */
#include "xxir_view_root_graph.inc.c"
static XrXirStatus view_root_emit(ViewRootQuery *q,ViewOriginPair site,ViewOriginPair context,
    XrXirViewRootKind kind) {
    ViewRootCheck *c=q->check;uint32_t ignored=0;
    if (q->capture) {
        if (context.first || kind==XR_XIR_VIEW_ROOT_OBJECT || kind==XR_XIR_VIEW_ROOT_ABSENT ||
            kind==XR_XIR_VIEW_ROOT_UNRESOLVED) return XR_XIR_BAD_VALUE;
        return view_root_intern(c,&c->captured,site,&ignored);
    }
    bool captured=false;XrXirStatus status=view_root_find(c,site,&captured);
    if (status!=XR_XIR_OK || captured) return captured ? XR_XIR_BAD_VALUE : status;
    if (!c->visitor) return XR_XIR_OK;
    uint32_t paths=0,selections=0;
    for (uint32_t p=context.first>>1;p;p=q->paths.pairs[p-1u].second) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;++paths;
    }
    for (uint32_t p=context.second;p;p=q->selections.pairs[p-1u].second) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;++selections;
    }
    uint64_t bytes=(uint64_t)paths*sizeof(XrXirViewRootStep)+(uint64_t)selections*sizeof(uint32_t);
    if (bytes>SIZE_MAX || bytes>c->context->limits.frame_bytes-c->reserved) return XR_XIR_BUDGET;
    unsigned char *storage=bytes ? xir_compile_alloc(c->context,(size_t)bytes,&status) : NULL;
    if (bytes && !storage) return status;
    c->reserved+=bytes;
    XrXirViewRootStep *steps=paths ? (XrXirViewRootStep *)storage : NULL;
    uint32_t *selected=selections ? (uint32_t *)(storage+(size_t)paths*sizeof(*steps)) : NULL;
    uint32_t count=0;
    for (uint32_t p=context.first>>1;p;p=q->paths.pairs[p-1u].second) {
        if (!xir_compile_work(c->context,sizeof(*steps))) {status=XR_XIR_BUDGET;break;}
        ViewOriginPair location=q->sites.pairs[q->paths.pairs[p-1u].first];
        const XrXirFunction *body=&c->module->functions[location.first];
        const XrXirInstruction *op=&body->instructions[location.second-body->parameter_count];
        steps[count++]=(XrXirViewRootStep){location.first,location.second,op->op,
            xr_xir_operand_type(body,op->args[0]),op->op==XR_XIR_FIELD_PLACE ? (uint32_t)op->immediate : op->args[1]};
    }
    count=0;
    for (uint32_t p=context.second;status==XR_XIR_OK && p;p=q->selections.pairs[p-1u].second) {
        if (!xir_compile_work(c->context,1)) {status=XR_XIR_BUDGET;break;}
        selected[count++]=q->selections.pairs[p-1u].first;
    }
    if (status==XR_XIR_OK) {
        XrXirViewRootEvent event={c->location.function,c->location.instruction,site.first,site.second,
            kind,steps,paths,selected,selections,(context.first&1u)!=0,kind==XR_XIR_VIEW_ROOT_OBJECT};
        if (!xir_compile_work(c->context,sizeof(event))) status=XR_XIR_BUDGET;
        else status=c->visitor(c->user,c->context,&event);
    }
    XR_CHECK(c->reserved>=bytes,"borrowed root callback scratch belongs to its finite owner");
    c->reserved-=bytes;xr_compile_resources_free(storage);return status;
}
static XrXirStatus view_root_parameter(ViewRootQuery *q,ViewOriginPair site,ViewOriginPair context) {
    ViewRootCheck *c=q->check;const XrXirFunction *body=&c->module->functions[site.first];
    XrXirType type=body->parameters[site.second],selected_type=type;
    for (uint32_t p=context.second;p;p=q->selections.pairs[p-1u].second) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
        uint32_t field=q->selections.pairs[p-1u].first;
        const XrXirTypeNode *n=xr_xir_type_node(c->module->types,selected_type);
        if (!n) return XR_XIR_BAD_TYPE;
        if (field==UINT32_MAX) {
            if (n->kind!=XR_XIR_TYPE_NULLABLE) return XR_XIR_BAD_TYPE;selected_type=n->element;
        } else {
            if (n->kind!=XR_XIR_TYPE_TUPLE || field>=n->parameter_count) return XR_XIR_BAD_TYPE;
            selected_type=n->parameters[field].type;
        }
    }
    bool cell=xr_xir_type_is_cell(c->module->types,type),object=xr_xir_type_is_class(c->module->types,type);
    if (!cell && !object && !view_flags_at(c->module->types,c->flags,selected_type)) return XR_XIR_BAD_TYPE;
    XrXirStatus status=view_root_emit(q,site,context,cell ? XR_XIR_VIEW_ROOT_PARAMETER :
        object ? XR_XIR_VIEW_ROOT_OBJECT : XR_XIR_VIEW_ROOT_BORROWED_PARAMETER);
    if (c->symbolic_parameters && !q->capture) return status;
    for (uint32_t e=c->heads[c->offsets[site.first]+site.second];e!=UINT32_MAX && status==XR_XIR_OK;e=c->edges[e].next) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
        ViewRootEdge edge=c->edges[e];status=view_root_push(q,edge.function,edge.value,context.first,context.second);
    }
    return status;
}
static XrXirStatus view_root_tuple(ViewRootQuery *q,ViewOriginPair site,const XrXirInstruction *op,
    ViewOriginPair context) {
    ViewRootCheck *c=q->check;const XrXirFunction *body=&c->module->functions[site.first];
    const XrXirTypeNode *tuple=xr_xir_tuple_signature(c->module->types,op->type);if (!tuple) return XR_XIR_BAD_TYPE;
    ViewOriginPair selection=context.second ? q->selections.pairs[context.second-1u] : (ViewOriginPair){UINT32_MAX,0};
    if (context.second && selection.first>=tuple->parameter_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t operand=0;
    for (uint32_t p=0;p<tuple->parameter_count;++p) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
        XrXirType type=tuple->parameters[p].type;if (type==XR_XIR_UNIT) continue;
        uint32_t value=body->operands[op->args[0]+operand++];
        if (!view_flags_at(c->module->types,c->flags,type) || (context.second && selection.first!=p)) continue;
        XrXirStatus status=view_root_push(q,site.first,value,context.first,context.second ? selection.second : 0);
        if (status!=XR_XIR_OK) return status;
    }
    return operand==op->args[1] ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus view_root_call(ViewRootQuery *q,ViewOriginPair site,const XrXirInstruction *op,
    ViewOriginPair context) {
    ViewRootCheck *c=q->check;const XrXirFunction *body=&c->module->functions[site.first];uint32_t candidate=UINT32_MAX;
    if (op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE) candidate=c->candidates[op->immediate];
    else if (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT) {
        const XrXirTypeNode *signature=xr_xir_callable_signature(c->module->types,xr_xir_operand_type(body,(uint32_t)op->immediate));
        if (!signature) return XR_XIR_BAD_TYPE;
        XrXirStatus status=view_return_candidate(c->context,c->module->types,c->flags,signature,&candidate);
        if (status!=XR_XIR_OK) return status;
    } else return view_root_emit(q,site,(ViewOriginPair){context.first|1u,context.second},XR_XIR_VIEW_ROOT_UNRESOLVED);
    if (candidate==UINT32_MAX || candidate>=op->args[1]) return XR_XIR_BAD_TYPE;
    uint32_t value=body->operands[op->args[0]+candidate];
    if (xr_xir_type_is_cell(c->module->types,xr_xir_operand_type(body,value))) context.first|=1u;
    return view_root_push(q,site.first,value,context.first,0);
}
#include "xxir_view_root_local.inc.c"
static XrXirStatus view_root_local(ViewRootQuery *q,ViewOriginPair site,const XrXirInstruction *op,
    ViewOriginPair context) {
    ViewRootCheck *c=q->check;const XrXirFunction *body=&c->module->functions[site.first];
    if (c->symbolic_parameters && !q->capture && site.first==c->location.function)
        return view_root_local_at(q,site,site.second,c->location.instruction,context);
    XrXirStatus status=view_root_push(q,site.first,op->args[0],context.first,context.second);
    for (uint32_t i=0;i<body->instruction_count && status==XR_XIR_OK;++i) {
        if (!xir_compile_work(q->check->context,1)) return XR_XIR_BUDGET;
        const XrXirInstruction *write=&body->instructions[i];
        if ((write->op==XR_XIR_LOCAL_WRITE || write->op==XR_XIR_OWNED_LOCAL_WRITE ||
            write->op==XR_XIR_PLACE_WRITE) && write->args[0]==site.second)
            status=view_root_push(q,site.first,write->args[1],context.first,context.second);
    }
    return status;
}
static XrXirStatus view_root_symbolic_slot(ViewRootQuery *q,uint32_t slot,ViewOriginPair context);
static XrXirStatus view_root_step(ViewRootQuery *q,ViewOriginPair site,ViewOriginPair context) {
    ViewRootCheck *c=q->check;const XrXirFunction *body=&c->module->functions[site.first];
    if (!xir_compile_work(c->context,2)) return XR_XIR_BUDGET;
    if (site.second<body->parameter_count) return view_root_parameter(q,site,context);
    const XrXirInstruction *op=&body->instructions[site.second-body->parameter_count];
    switch (op->op) {
    case XR_XIR_CELL_NEW:return context.second ? XR_XIR_BAD_TYPE : view_root_emit(q,site,context,XR_XIR_VIEW_ROOT_CELL);
    case XR_XIR_SLOT_LOAD:
        if (xr_xir_type_is_cell(c->module->types,op->type)) {
            if (c->symbolic_parameters) return view_root_symbolic_slot(q,(uint32_t)op->immediate,context);
            return view_root_emit(q,(ViewOriginPair){UINT32_MAX,(uint32_t)op->immediate},context,XR_XIR_VIEW_ROOT_MODULE);
        }
        break;
    case XR_XIR_FIELD_PLACE:case XR_XIR_INDEX_PLACE: {
        uint32_t location=0,path=0;XrXirStatus status=view_root_intern(c,&q->sites,site,&location);
        if (status==XR_XIR_OK) status=view_root_intern(c,&q->paths,(ViewOriginPair){location,context.first>>1},&path);
        if (status!=XR_XIR_OK) return status;
        if (path>=UINT32_MAX/2u) return XR_XIR_BUDGET;
        return view_root_push(q,site.first,op->args[0],((path+1u)<<1)|(context.first&1u),context.second);
    }
    case XR_XIR_COPY:case XR_XIR_OWNED_RETAIN:case XR_XIR_CELL_PROJECT:case XR_XIR_CELL_PLACE:
    case XR_XIR_OBJECT_PLACE:
        return view_root_push(q,site.first,op->args[0],context.first,context.second);
    case XR_XIR_LOCAL_READ:case XR_XIR_OWNED_LOCAL_READ:return view_root_local_read(q,site,op,context);
    case XR_XIR_PLACE_READ:
        if (xr_xir_place_kind(body,op->args[0])==XR_XIR_PLACE_LOCAL) return view_root_local_read(q,site,op,context);
        break;
    case XR_XIR_LOCAL_NEW:case XR_XIR_OWNED_LOCAL_NEW:return view_root_local(q,site,op,context);
    case XR_XIR_VIEW_NEW:case XR_XIR_VIEW_REBORROW:
        if (context.second || op->args[1]!=3) return XR_XIR_BAD_STRUCTURE;
        return view_root_push(q,site.first,body->operands[op->args[0]],context.first,0);
    case XR_XIR_PHI:
        for (uint32_t p=1;p<op->args[1];p+=2) {
            XrXirStatus status=view_root_push(q,site.first,body->operands[op->args[0]+p],context.first,context.second);
            if (status!=XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    case XR_XIR_NULLABLE_NONE:return view_root_emit(q,site,context,XR_XIR_VIEW_ROOT_ABSENT);
    case XR_XIR_NULLABLE_SOME:
        if (!context.second) return view_root_push(q,site.first,op->args[0],context.first,0);
        if (q->selections.pairs[context.second-1u].first!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
        return view_root_push(q,site.first,op->args[0],context.first,q->selections.pairs[context.second-1u].second);
    case XR_XIR_NULLABLE_UNWRAP:return view_root_select(q,(ViewOriginPair){site.first,op->args[0]},UINT32_MAX,context);
    case XR_XIR_TUPLE_FIELD:return view_root_select(q,(ViewOriginPair){site.first,op->args[0]},(uint32_t)op->immediate,context);
    case XR_XIR_TUPLE_NEW:return view_root_tuple(q,site,op,context);
    case XR_XIR_CALL:case XR_XIR_CALL_INDIRECT:case XR_XIR_CALL_DEFAULT:case XR_XIR_CALL_REQUIREMENT:
        return view_root_call(q,site,op,context);
    case XR_XIR_INVOKE_RESULT:
        return view_root_call(q,site,&body->instructions[op->immediate],context);
    default:break;
    }
    if (xr_xir_type_is_class(c->module->types,xr_xir_operand_type(body,site.second)))
        return view_root_emit(q,site,context,XR_XIR_VIEW_ROOT_OBJECT);
    return XR_XIR_BAD_VALUE;
}
static XrXirStatus view_root_drain(ViewRootQuery *q,XrXirStatus status) {
    for (uint32_t i=0;i<q->tasks.count && status==XR_XIR_OK;++i) {
        ViewOriginPair task=q->tasks.pairs[i],site=q->sites.pairs[task.first],context=q->contexts.pairs[task.second];
        status=view_root_step(q,site,context);
    }
    ViewRootCheck *c=q->check;
    view_root_set_drop(c,&q->sites);view_root_set_drop(c,&q->tasks);view_root_set_drop(c,&q->contexts);
    view_root_set_drop(c,&q->paths);view_root_set_drop(c,&q->selections);return status;
}
static XrXirStatus view_root_query(ViewRootCheck *c,uint32_t f,uint32_t value,bool capture) {
    ViewRootQuery q={.check=c,.capture=capture};
    return view_root_drain(&q,view_root_push(&q,f,value,0,0));
}
static XrXirStatus view_root_walk(ViewRootCheck *c,bool captures) {
    for (uint32_t f=0;f<c->module->function_count;++f) {
        const XrXirFunction *body=&c->module->functions[f];
        for (uint32_t b=0;b<body->block_count;++b) for (uint32_t i=body->blocks[b].first;i<body->blocks[b].first+body->blocks[b].count;++i) {
            if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
            const XrXirInstruction *op=&body->instructions[i];c->location=(XrXirDiagnostic){XR_XIR_OK,f,b,i,XR_XIR_DIAGNOSTIC_NONE};
            if (captures) {
                if (op->op!=XR_XIR_FUNCTION_REF) continue;
                for (uint32_t p=0;p<op->args[1];++p) {
                    if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
                    uint32_t value=body->operands[op->args[0]+p];
                    if (!xr_xir_type_is_cell(c->module->types,xr_xir_operand_type(body,value))) continue;
                    uint32_t ignored;XrXirStatus status=view_root_intern(c,&c->captured,(ViewOriginPair){(uint32_t)op->immediate,p},&ignored);
                    if (status==XR_XIR_OK) status=view_root_query(c,f,value,true);
                    if (status!=XR_XIR_OK) return status;
                }
            } else if (op->op==XR_XIR_VIEW_NEW || op->op==XR_XIR_VIEW_REBORROW) {
                XrXirStatus status=view_root_query(c,f,body->parameter_count+i,false);if (status!=XR_XIR_OK) return status;
            }
        }
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_compile_view_roots_verified(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirViewRootVisitor visitor,void *user,XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(context) || !module || !module->function_count || !module->functions) return XR_XIR_BAD_STRUCTURE;
    bool needed=false;
    for (uint32_t f=0;f<module->function_count;++f) for (uint32_t i=0;i<module->functions[f].instruction_count;++i) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        XrXirOp op=module->functions[f].instructions[i].op;needed|=op==XR_XIR_VIEW_NEW || op==XR_XIR_VIEW_REBORROW;
    }
    if (!needed) return XR_XIR_OK;
    uint8_t *flags=NULL;XrXirStatus status=view_flags_build(context,module->types,&flags);
    if (status!=XR_XIR_OK || !flags) return status==XR_XIR_OK ? XR_XIR_BAD_TYPE : status;
    ViewRootCheck c={.context=context,.module=module,.flags=flags,.visitor=visitor,.user=user,
        .location={XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE}};
    status=view_root_graph(&c);
    if (status==XR_XIR_OK) status=view_root_walk(&c,true);
    if (status==XR_XIR_OK) status=view_root_walk(&c,false);
    if (diagnostic && status!=XR_XIR_OK) {*diagnostic=c.location;diagnostic->status=status;}
    view_root_set_drop(&c,&c.captured);xr_compile_resources_free(c.meta);xr_compile_resources_free(c.edges);
    xr_compile_resources_free(flags);return status;
}

#include "xxir_view_root_resolver.inc.c"
