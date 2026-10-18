/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static bool call_raw_range_same(XrXirSourceRange a,XrXirSourceRange b) {
    return a.line==b.line&&a.column==b.column&&a.end_line==b.end_line&&a.end_column==b.end_column;
}
static bool call_module_same(CallWork *w,size_t root,uint32_t module,CallSymbol symbol,bool *out) {
    const XrXirSourceView *a=call_view(w,root),*b=call_view(w,symbol.root);*out=false;
    if(module>=a->module_count)return call_fail(w,XR_XIR_BAD_STRUCTURE);
    if(!call_equal(w,a->modules[module].identity,b->modules[symbol.module].identity,false,out)||!*out)return w->status==XR_XIR_OK;
    if(!call_work(w,2*sizeof(XrFingerprint)))return false;
    return !memcmp(&a->modules[module].fingerprint,&b->modules[symbol.module].fingerprint,sizeof(XrFingerprint))||call_fail(w,XR_XIR_UNRESOLVED);
}
static bool call_declaration_same(CallWork *w,size_t a,uint32_t ai,size_t b,uint32_t bi,bool *out) {
    const XrXirSourceView *va=call_view(w,a),*vb=call_view(w,b);*out=false;
    if(!ai||ai>va->declaration_count||!bi||bi>vb->declaration_count)return call_fail(w,XR_XIR_BAD_STRUCTURE);
    if(!call_work(w,2*sizeof(XrXirSourceDeclaration)))return false;
    const XrXirSourceDeclaration *da=&va->declarations[ai-1],*db=&vb->declarations[bi-1];
    const XrXirSourceSyntaxView *sa=xlsp_source_bindings_syntax(w->bindings,a),*sb=xlsp_source_bindings_syntax(w->bindings,b);
    if(da->kind!=db->kind||sa->declarations[ai-1].role!=sb->declarations[bi-1].role||!call_raw_range_same(da->range,db->range))return true;
    CallSymbol key={b,0,bi,db->range.module,false,{0},{0}};
    if(!call_module_same(w,a,da->range.module,key,out)||!*out)return w->status==XR_XIR_OK;
    if(sa->declarations[ai-1].role==XR_XIR_SOURCE_SYNTAX_CLOSURE)return true;
    return call_equal(w,da->name,db->name,false,out);
}
static bool call_resolve_symbol(CallWork *w,size_t root,uint32_t id,CallSymbol *out) {
    const XrXirSourceView *view=call_view(w,root);const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(w->bindings,root);
    if(!id||id>view->declaration_count)return call_fail(w,XR_XIR_BAD_STRUCTURE);
    if(!call_callable(&view->declarations[id-1],&syntax->declarations[id-1]))return call_fail(w,XR_XIR_UNSUPPORTED);
    if(xlsp_source_bindings_calls(w->bindings,root)->functions[id-1].known)return call_symbol(w,root,id,out);
    bool found=false;
    /* Join published identities across roots; no name search or new inference. */
    for(size_t other=0;other<w->request->count;++other){const XrXirSourceView *v=call_view(w,other);
        const XrXirSourceCalls *calls=xlsp_source_bindings_calls(w->bindings,other);
        for(uint32_t i=0;i<v->declaration_count;++i){if(!call_work(w,sizeof(XrXirSourceFunctionRange)))return false;if(!calls->functions[i].known)continue;
            bool same=false;if(!call_declaration_same(w,root,id,other,i+1,&same))return false;if(!same)continue;
            CallSymbol candidate;if(!call_symbol(w,other,i+1,&candidate))return false;
            if(found){if(!call_symbol_same(w,*out,candidate,&same))return false;if(!same)return call_fail(w,XR_XIR_UNSUPPORTED);}
            else {*out=candidate;found=true;}}}
    return found||call_fail(w,XR_XIR_UNSUPPORTED);
}
static bool call_entry_module(CallWork *w,const XrXirSourceView *view,uint32_t module,bool *out) {
    if(module>=view->module_count)return call_fail(w,XR_XIR_BAD_STRUCTURE);
    return call_equal(w,view->modules[module].path,xlsp_source_bindings_path(w->bindings,w->request->entry),true,out);
}
static bool call_pick_declarations(CallWork *w,size_t root,XrLspPosition position) {
    const XrXirSourceView *view=call_view(w,root);const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(w->bindings,root);
    const XrXirSourceCalls *calls=xlsp_source_bindings_calls(w->bindings,root);
    for(uint32_t i=0;i<view->declaration_count;++i){if(!call_work(w,sizeof(XrXirSourceDeclaration)))return false;
        const XrXirSourceDeclaration *d=&view->declarations[i];const XrXirSourceDeclarationSyntax *name=&syntax->declarations[i];
        if(!call_callable(d,name))continue;bool entry=false;if(!call_entry_module(w,view,d->range.module,&entry))return false;if(!entry)continue;
        XrLspRange selection={0};
        if(name->role==XR_XIR_SOURCE_SYNTAX_CLOSURE){if(!calls->functions[i].known)continue;
            if(!call_span(w,w->request->entry,calls->functions[i].range,&selection))return false;selection.end=selection.start;}
        else if(name->role==XR_XIR_SOURCE_SYNTAX_NAME||name->role==XR_XIR_SOURCE_SYNTAX_METHOD){if(!call_span(w,w->request->entry,name->name,&selection))return false;}
        else continue;
        if(!call_contains(selection,position))continue;CallSymbol symbol;
        if(!call_resolve_symbol(w,root,i+1,&symbol)||!call_select(w,symbol))return false;
    }
    return true;
}
static bool call_pick_references(CallWork *w,size_t root,XrLspPosition position) {
    const XrXirSourceView *view=call_view(w,root);const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(w->bindings,root);
    for(uint32_t i=0;i<view->reference_count;++i){if(!call_work(w,sizeof(XrXirSourceReference)))return false;
        XrXirSourceRange raw=syntax->references[i];bool entry=false;if(!call_entry_module(w,view,raw.module,&entry))return false;
        if(!entry||raw.line<=0||raw.column<=0||raw.end_line!=raw.line||raw.end_column<=raw.column)continue;
        XrLspRange selection;if(!call_span(w,w->request->entry,raw,&selection))return false;if(!call_contains(selection,position))continue;
        uint32_t id=view->references[i].target;if(!call_target(w,view,&id))return false;
        if(!call_callable(&view->declarations[id-1],&syntax->declarations[id-1]))continue;
        CallSymbol symbol;if(!call_resolve_symbol(w,root,id,&symbol)||!call_select(w,symbol))return false;
    }
    const XrXirSourceCalls *calls=xlsp_source_bindings_calls(w->bindings,root);
    for(uint32_t i=0;i<calls->count;++i){if(!call_work(w,sizeof(XrXirSourceCall)))return false;const XrXirSourceCall *call=&calls->items[i];
        if(call->reference!=UINT32_MAX||!call->selection_known)continue;bool entry=false;
        if(!call_entry_module(w,view,call->range.module,&entry))return false;if(!entry)continue;
        XrLspRange span;if(!call_span(w,w->request->entry,call->range,&span))return false;if(call_contains(span,position))return call_fail(w,XR_XIR_UNSUPPORTED);}
    return true;
}
static bool call_pick(CallWork *w,XrLspPosition position,bool initializer) {
    for(size_t root=0;root<w->request->count;++root){
        if(!initializer){if(!call_pick_declarations(w,root,position)||!call_pick_references(w,root,position))return false;continue;}
        const XrXirSourceView *view=call_view(w,root);const XrXirSourceCalls *calls=xlsp_source_bindings_calls(w->bindings,root);
        for(uint32_t i=0;i<calls->count;++i){if(!call_work(w,sizeof(XrXirSourceCall)))return false;
            const XrXirSourceCall *call=&calls->items[i];if(!call->module_initializer)continue;bool entry=false;
            if(!call_entry_module(w,view,call->range.module,&entry))return false;if(!entry)continue;
            CallSymbol symbol;if(!call_module_symbol(w,root,call->range.module,&symbol)||!call_select(w,symbol))return false;}}
    return w->selected_ready||call_fail(w,XR_XIR_UNRESOLVED);
}
