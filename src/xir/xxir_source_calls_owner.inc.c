/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_calls_owner.inc.c - Own canonical call observations after producer death
 */
static bool query_call_callable(const XrXirSourceSnapshot *snapshot,uint32_t declaration) {
    if(!declaration||declaration>snapshot->view.declaration_count)return false;
    const XrXirSourceDeclaration *decl=&snapshot->view.declarations[declaration-1];
    return decl->kind==XR_XIR_SOURCE_FUNCTION||(decl->kind==XR_XIR_SOURCE_MEMBER&&
        snapshot->syntax.declarations[declaration-1].role==XR_XIR_SOURCE_SYNTAX_METHOD);
}
static bool query_call_contains(XrXirSourceRange outer,XrXirSourceRange inner) {
    return outer.module==inner.module&&
        (inner.line>outer.line||(inner.line==outer.line&&inner.column>=outer.column))&&
        (inner.end_line<outer.end_line||(inner.end_line==outer.end_line&&inner.end_column<=outer.end_column));
}
static XrXirStatus query_call_functions(SourceQueryCopy *copy,const XrXirSourceCalls *calls) {
    const XrXirSourceView *view=&copy->snapshot->view;
    for(uint32_t i=0;i<calls->declaration_count;++i) {
        if(!query_work(copy,sizeof(calls->functions[i])))return copy->status;
        const XrXirSourceFunctionRange *fact=&calls->functions[i];
        XrXirSourceRange range=fact->range;
        if(!fact->known) {
            if(range.module||range.line||range.column||range.end_line||range.end_column)return XR_XIR_BAD_STRUCTURE;
            continue;
        }
        if(!query_call_callable(copy->snapshot,i+1)||range.module>=view->module_count||
            range.module!=view->declarations[i].range.module||range.line<=0||range.column<=0||
            range.end_line<range.line||range.end_column<=0||
            (range.end_line==range.line&&range.end_column<=range.column))return XR_XIR_BAD_STRUCTURE;
        const XrXirSourceDeclarationSyntax *syntax=&copy->snapshot->syntax.declarations[i];
        if((syntax->role==XR_XIR_SOURCE_SYNTAX_NAME||syntax->role==XR_XIR_SOURCE_SYNTAX_METHOD)&&
            !query_call_contains(range,syntax->name))return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus query_call_signature_type(SourceQueryCopy *copy,XrXirSourceType type) {
    const XrXirSourceView *view=&copy->snapshot->view;
    if(!query_work(copy,sizeof(type)))return copy->status;
    if(!type.known||type.generic_owner>view->declaration_count)return XR_XIR_BAD_TYPE;
    uint32_t parameters=0;
    if(type.generic_owner) {
        const XrXirSourceDeclaration *owner=&view->declarations[type.generic_owner-1];
        if(owner->kind!=XR_XIR_SOURCE_TYPE&&!query_call_callable(copy->snapshot,type.generic_owner))return XR_XIR_BAD_TYPE;
        parameters=owner->generic_parameter_count;
        if(!parameters)return XR_XIR_BAD_TYPE;
    }
    XrXirTypes empty={0};
    return xr_xir_compile_type_expression_shape(&copy->snapshot->context,view->types?view->types:&empty,type.type,parameters);
}
static XrXirStatus query_call_signature(SourceQueryCopy *copy,const XrXirSourceCall *call) {
    const XrXirSourceCallSignature *signature=&call->signature;
    if(!signature->known) {
        return signature->parameters||signature->parameter_count||signature->receiver_count||signature->result.type||
            signature->result.generic_owner||signature->result.known||signature->type_parameters||
            signature->type_parameter_count||signature->type_parameter_owner?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
    }
    const XrXirSourceView *view=&copy->snapshot->view;
    if((!!signature->type_parameters!=!!signature->type_parameter_count)||
        (!!signature->type_parameter_owner!=!!signature->type_parameter_count)||
        signature->type_parameter_owner>view->declaration_count||signature->type_parameter_count>65536)
        return XR_XIR_BAD_STRUCTURE;
    if(signature->type_parameter_owner&&view->declarations[signature->type_parameter_owner-1].generic_parameter_count!=
        signature->type_parameter_count)return XR_XIR_BAD_STRUCTURE;
    if(signature->type_parameter_owner) {
        uint32_t owner=call->caller,remaining=view->declaration_count;
        while(owner&&owner<=view->declaration_count&&owner!=signature->type_parameter_owner&&remaining--) {
            if(!query_work(copy,sizeof(uint32_t)))return copy->status;
            owner=view->declarations[owner-1].generic_parent;
        }
        if(owner!=signature->type_parameter_owner)return XR_XIR_BAD_STRUCTURE;
    }
    for(uint32_t p=0;p<signature->type_parameter_count;++p) {
        if(query_length(copy,signature->type_parameters[p])<2)
            return copy->status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:copy->status;
    }
    if(call->reference>=view->reference_count||signature->parameter_count>65536||
        (!!signature->parameters!=!!signature->parameter_count)||signature->receiver_count>1||
        signature->receiver_count>signature->parameter_count||
        !query_call_callable(copy->snapshot,view->references[call->reference].target))return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceDeclaration *callee=&view->declarations[view->references[call->reference].target-1];
    if(signature->parameter_count!=callee->parameter_count)return XR_XIR_BAD_STRUCTURE;
    uint32_t receivers=0;
    for(uint32_t i=0;i<view->declaration_count;++i) {
        if(!query_work(copy,sizeof(view->declarations[i])))return copy->status;
        if(view->declarations[i].parent==callee->id&&
            copy->snapshot->syntax.declarations[i].role==XR_XIR_SOURCE_SYNTAX_RECEIVER)++receivers;
    }
    if(receivers!=signature->receiver_count)return XR_XIR_BAD_STRUCTURE;
    if(signature->result.generic_owner&&signature->result.generic_owner!=signature->type_parameter_owner)return XR_XIR_BAD_TYPE;
    XrXirStatus status=query_call_signature_type(copy,signature->result);
    for(uint32_t p=0;p<signature->parameter_count&&status==XR_XIR_OK;++p) {
        if(signature->parameters[p].generic_owner&&signature->parameters[p].generic_owner!=signature->type_parameter_owner)
            return XR_XIR_BAD_TYPE;
        status=query_call_signature_type(copy,signature->parameters[p]);
    }
    return status;
}
static XrXirStatus query_call_items(SourceQueryCopy *copy,const XrXirSourceCalls *calls) {
    const XrXirSourceSnapshot *snapshot=copy->snapshot;
    const XrXirSourceView *view=&snapshot->view;
    uint32_t next=0;
    for(uint32_t i=0;i<calls->count;++i) {
        if(!query_work(copy,sizeof(calls->items[i])))return copy->status;
        const XrXirSourceCall *call=&calls->items[i];
        if(!query_syntax_range(view,call->range,!call->selection_known)||(call->caller&&
            (!query_call_callable(snapshot,call->caller)||call->module_initializer||
                view->declarations[call->caller-1].range.module!=call->range.module)))return XR_XIR_BAD_STRUCTURE;
        if(call->caller&&call->selection_known&&calls->functions[call->caller-1].known&&
            !query_call_contains(calls->functions[call->caller-1].range,call->range))return XR_XIR_BAD_STRUCTURE;
        XrXirStatus signature=query_call_signature(copy,call);if(signature!=XR_XIR_OK)return signature;
        if(call->reference==UINT32_MAX)continue;
        while(next<view->reference_count&&view->references[next].access!=XR_XIR_SOURCE_CALL) {
            if(!query_work(copy,sizeof(view->references[next])))return copy->status;
            ++next;
        }
        if(call->reference!=next||next>=view->reference_count)return XR_XIR_BAD_STRUCTURE;
        XrXirSourceRange selection=snapshot->syntax.references[next];
        if(call->range.module!=selection.module||call->selection_known!=query_syntax_range(view,selection,false)||
            (call->selection_known&&!query_syntax_same_range(call->range,selection)))return XR_XIR_BAD_STRUCTURE;
        ++next;
    }
    while(next<view->reference_count) {
        if(!query_work(copy,sizeof(view->references[next])))return copy->status;
        if(view->references[next++].access==XR_XIR_SOURCE_CALL)return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_source_snapshot_calls_copy(XrXirSourceSnapshot *snapshot,const XrXirSourceCalls *calls) {
    if(!snapshot||!calls||snapshot->calls_ready||!snapshot->syntax_ready||
        calls->declaration_count!=snapshot->view.declaration_count||
        (!!calls->functions!=!!calls->declaration_count)|| (!!calls->items!=!!calls->count))return XR_XIR_BAD_STRUCTURE;
    SourceQueryCopy copy={snapshot,XR_XIR_OK};
    XrXirStatus status=query_call_functions(&copy,calls);
    if(status==XR_XIR_OK)status=query_call_items(&copy,calls);
    if(status!=XR_XIR_OK)return status;
    SourceQueryMemory *previous=snapshot->memory;
    XrXirSourceCalls owned=*calls;
    owned.items=query_copy(&copy,calls->items,calls->count,sizeof(*calls->items));
    if(copy.status==XR_XIR_OK)for(uint32_t i=0;i<calls->count&&copy.status==XR_XIR_OK;++i) {
        const XrXirSourceCallSignature *signature=&calls->items[i].signature;
        if(!query_work(&copy,sizeof(*signature)))break;
        XrXirSourceCallSignature value=*signature;
        value.parameters=query_copy(&copy,signature->parameters,signature->parameter_count,sizeof(*signature->parameters));
        const char **names=query_allocate(&copy,signature->type_parameter_count,sizeof(*names));
        for(uint32_t p=0;names&&p<signature->type_parameter_count&&copy.status==XR_XIR_OK;++p)
            names[p]=query_string(&copy,signature->type_parameters[p]);
        value.type_parameters=names;
        if(copy.status==XR_XIR_OK)((XrXirSourceCall *)owned.items)[i].signature=value;
    }
    owned.functions=query_copy(&copy,calls->functions,calls->declaration_count,sizeof(*calls->functions));
    (void)query_work(&copy,sizeof(owned)+sizeof(snapshot->calls_ready));
    if(copy.status!=XR_XIR_OK) {
        while(snapshot->memory!=previous) {SourceQueryMemory *next=snapshot->memory->next;xr_compile_resources_free(snapshot->memory);snapshot->memory=next;}
        return copy.status;
    }
    snapshot->calls=owned;snapshot->calls_ready=true;return XR_XIR_OK;
}
XR_FUNC const XrXirSourceCalls *xr_xir_compile_source_snapshot_calls(const XrXirSourceSnapshot *snapshot) {
    return snapshot&&snapshot->calls_ready?&snapshot->calls:NULL;
}
