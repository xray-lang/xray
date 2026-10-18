/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static const XrXirSourceTypeFact *type_fact(TypeWork *w,size_t root,uint32_t id) {
    const XrXirSourceTypeFacts *facts=xlsp_source_bindings_types(w->bindings,root);
    return facts&&id&&id<=facts->declaration_count?&facts->declarations[id-1]:NULL;
}
static bool type_target(TypeWork *w,const XrXirSourceView *view,uint32_t *id) {
    for(uint32_t i=0;i<view->declaration_count;++i){if(!type_work(w,sizeof(XrXirSourceDeclaration)))return false;
        if(!*id||*id>view->declaration_count)return type_fail(w,XR_XIR_UNRESOLVED);
        uint32_t next=view->declarations[*id-1].target;if(!next||next==*id)return true;*id=next;}
    return type_fail(w,XR_XIR_BAD_STRUCTURE);
}
static bool type_symbol(TypeWork *w,size_t root,uint32_t id,TypeSymbol *out) {
    const XrXirSourceView *view=type_view(w,root);const XrXirSourceTypeFact *fact=type_fact(w,root,id);
    if(!fact||id>view->declaration_count)return type_fail(w,XR_XIR_BAD_STRUCTURE);
    if(fact->kind==XR_XIR_SOURCE_TYPE_NONE||!fact->range_known)return type_fail(w,XR_XIR_UNSUPPORTED);
    const XrXirSourceDeclaration *d=&view->declarations[id-1];const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(w->bindings,root);
    if(d->kind!=XR_XIR_SOURCE_TYPE||syntax->declarations[id-1].role!=XR_XIR_SOURCE_SYNTAX_NAME)return type_fail(w,XR_XIR_BAD_STRUCTURE);
    TypeSymbol symbol={root,0,id,d->range.module,fact->kind,fact->table_index,{0},{0}};
    if(!type_document(w,view,symbol.module,&symbol.document)||!type_span(w,symbol.document,fact->range,&symbol.range)||
        !type_span(w,symbol.document,syntax->declarations[id-1].name,&symbol.selection))return false;
    *out=symbol;return true;
}
static bool type_symbol_same(TypeWork *w,TypeSymbol a,TypeSymbol b,bool *out) {
    *out=false;if(a.kind!=b.kind)return true;const XrXirSourceView *va=type_view(w,a.root),*vb=type_view(w,b.root);bool same=false;
    if(!type_equal(w,va->modules[a.module].identity,vb->modules[b.module].identity,false,&same)||!same)return w->status==XR_XIR_OK;
    if(!type_equal(w,va->modules[a.module].path,vb->modules[b.module].path,true,&same)||!same)return w->status==XR_XIR_OK;
    if(!type_work(w,2*sizeof(XrFingerprint)))return false;
    if(memcmp(&va->modules[a.module].fingerprint,&vb->modules[b.module].fingerprint,sizeof(XrFingerprint)))return type_fail(w,XR_XIR_UNRESOLVED);
    if(!type_range_same(a.range,b.range)||!type_range_same(a.selection,b.selection))return true;
    return type_equal(w,va->declarations[a.declaration-1].name,vb->declarations[b.declaration-1].name,false,out);
}
static bool type_select(TypeWork *w,TypeSymbol symbol) {
    if(w->selected_ready){bool same=false;if(!type_symbol_same(w,w->selected,symbol,&same))return false;if(!same)return type_fail(w,XR_XIR_UNRESOLVED);}
    else{w->selected=symbol;w->selected_ready=true;}return true;
}
static bool type_entry_module(TypeWork *w,const XrXirSourceView *view,uint32_t module,bool *out) {
    if(module>=view->module_count)return type_fail(w,XR_XIR_BAD_STRUCTURE);
    if(!view->modules[module].path){*out=false;return true;}
    return type_equal(w,view->modules[module].path,xlsp_source_bindings_path(w->bindings,w->request->entry),true,out);
}
static bool type_pick(TypeWork *w,XrLspPosition at) {
    for(size_t root=0;root<w->request->count;++root){const XrXirSourceView *view=type_view(w,root);
        const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(w->bindings,root);
        for(uint32_t i=0;i<view->declaration_count;++i){if(!type_work(w,sizeof(XrXirSourceDeclaration)))return false;
            if(view->declarations[i].kind!=XR_XIR_SOURCE_TYPE||syntax->declarations[i].role!=XR_XIR_SOURCE_SYNTAX_NAME)continue;
            bool entry=false;if(!type_entry_module(w,view,view->declarations[i].range.module,&entry))return false;if(!entry)continue;
            XrLspRange span;if(!type_span(w,w->request->entry,syntax->declarations[i].name,&span))return false;if(!type_contains(span,at))continue;
            TypeSymbol symbol;if(!type_symbol(w,root,i+1,&symbol)||!type_select(w,symbol))return false;}
        for(uint32_t i=0;i<view->reference_count;++i){if(!type_work(w,sizeof(XrXirSourceReference)))return false;
            if(view->references[i].access!=XR_XIR_SOURCE_TYPE_USE)continue;XrXirSourceRange raw=syntax->references[i];bool entry=false;
            if(!type_entry_module(w,view,raw.module,&entry))return false;if(!entry||raw.line<=0||raw.column<=0||raw.end_line!=raw.line||raw.end_column<=raw.column)continue;
            XrLspRange span;if(!type_span(w,w->request->entry,raw,&span))return false;if(!type_contains(span,at))continue;
            uint32_t id=view->references[i].target;if(!type_target(w,view,&id))return false;
            if(view->declarations[id-1].kind!=XR_XIR_SOURCE_TYPE)continue;TypeSymbol symbol;
            if(!type_symbol(w,root,id,&symbol)||!type_select(w,symbol))return false;}}
    return w->selected_ready||type_fail(w,XR_XIR_UNRESOLVED);
}
