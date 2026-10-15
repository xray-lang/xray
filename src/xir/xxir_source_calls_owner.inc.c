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
