/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static bool type_append(TypeWork *w,TypeSymbol symbol) {
    for(size_t i=0;i<w->count;++i){bool same=false;if(!type_symbol_same(w,w->rows[i],symbol,&same))return false;if(same)return true;}
    if(w->count==INT_MAX)return type_fail(w,XR_XIR_BUDGET);
    if(w->count==w->capacity){size_t next=w->capacity?w->capacity*2:16;
        if(next<w->capacity||next>SIZE_MAX/sizeof(*w->rows)||next>INT_MAX)return type_fail(w,XR_XIR_BUDGET);
        if(!type_resource(w,xr_compile_resources_resize(w->resources,(void **)&w->rows,next*sizeof(*w->rows))))return false;w->capacity=next;}
    if(!type_work(w,sizeof(symbol)))return false;w->rows[w->count++]=symbol;return true;
}
static bool type_table_symbol(TypeWork *w,size_t root,bool interface,uint32_t index,TypeSymbol *out) {
    const XrXirSourceTypeFacts *facts=xlsp_source_bindings_types(w->bindings,root);bool found=false;
    for(uint32_t i=0;i<facts->declaration_count;++i){if(!type_work(w,sizeof(facts->declarations[i])))return false;
        const XrXirSourceTypeFact *fact=&facts->declarations[i];if(fact->kind==XR_XIR_SOURCE_TYPE_NONE||
            (fact->kind==XR_XIR_SOURCE_TYPE_INTERFACE)!=interface||fact->table_index!=index)continue;
        if(found)return type_fail(w,XR_XIR_BAD_STRUCTURE);if(!type_symbol(w,root,i+1,out))return false;found=true;}
    return found||type_fail(w,XR_XIR_UNSUPPORTED);
}
static bool type_literal_same(TypeWork *w,XrXirLiteral literal,const char *text,bool *out) {
    size_t length=0;if(!type_length(w,text,&length))return false;
    if(!literal.bytes)return type_fail(w,XR_XIR_BAD_STRUCTURE);*out=false;if(literal.length!=length)return true;
    if(!type_work(w,length))return false;*out=!memcmp(literal.bytes,text,length);return true;
}
/* Only checked table identities decide whether this root can contain edges.
 * A matching identity without source evidence is unavailable, never absent. */
static bool type_local_selected(TypeWork *w,size_t root,TypeSymbol *out,bool *found) {
    *found=false;const XrXirSourceView *view=type_view(w,root),*selected=type_view(w,w->selected.root);
    const char *module=selected->modules[w->selected.module].identity,*name=selected->declarations[w->selected.declaration-1].name;
    bool interface=w->selected.kind==XR_XIR_SOURCE_TYPE_INTERFACE;
    uint32_t count=interface?(view->types&&view->types->interfaces?view->types->interfaces->count:0):
        (view->types&&view->types->nominals?view->types->nominals->count:0);
    for(uint32_t i=0;i<count;++i){if(!type_work(w,1))return false;XrXirLiteral local_module,local_name;
        if(interface){local_module=view->types->interfaces->declarations[i].module;local_name=view->types->interfaces->declarations[i].name;}
        else{local_module=view->types->nominals->declarations[i].module;local_name=view->types->nominals->declarations[i].name;}
        bool same=false;if(!type_literal_same(w,local_module,module,&same))return false;if(!same)continue;
        if(!type_literal_same(w,local_name,name,&same))return false;if(!same)continue;
        TypeSymbol local;if(!type_table_symbol(w,root,interface,i,&local)||!type_symbol_same(w,local,w->selected,&same))return false;
        if(!same)continue;if(*found)return type_fail(w,XR_XIR_BAD_STRUCTURE);*found=true;*out=local;}
    return true;
}
static bool type_supertypes(TypeWork *w) {
    TypeSymbol selected=w->selected;const XrXirSourceView *view=type_view(w,selected.root);
    if(selected.kind==XR_XIR_SOURCE_TYPE_INTERFACE){const XrXirInterfaceDeclaration *row=&view->types->interfaces->declarations[selected.table];
        for(uint32_t i=0;i<row->parent_count;++i){if(!type_work(w,sizeof(row->parents[i])))return false;TypeSymbol parent;
            if(!type_table_symbol(w,selected.root,true,row->parents[i].declaration,&parent)||!type_append(w,parent))return false;}}
    else if(view->implementations)for(uint32_t i=0;i<view->implementations->count;++i){if(!type_work(w,sizeof(XrXirImplementation)))return false;
        const XrXirImplementation *row=&view->implementations->records[i];if(row->nominal_declaration!=selected.table)continue;TypeSymbol parent;
        if(!type_table_symbol(w,selected.root,true,row->interface.declaration,&parent)||!type_append(w,parent))return false;}
    return true;
}
static bool type_subtypes(TypeWork *w) {
    /* Verified nominal classes are roots in this table. Source rejects class
     * inheritance before publication; no AST parent inference is available. */
    if(w->selected.kind!=XR_XIR_SOURCE_TYPE_INTERFACE)return true;
    for(size_t root=0;root<w->request->count;++root){TypeSymbol selected;bool found=false;
        if(!type_local_selected(w,root,&selected,&found))return false;if(!found)continue;
        const XrXirSourceView *view=type_view(w,root);const XrXirInterfaceTable *table=view->types->interfaces;
        for(uint32_t i=0;i<table->count;++i)for(uint32_t p=0;p<table->declarations[i].parent_count;++p){
            if(!type_work(w,sizeof(XrXirInterfaceApplication)))return false;if(table->declarations[i].parents[p].declaration!=selected.table)continue;
            TypeSymbol child;if(!type_table_symbol(w,root,true,i,&child)||!type_append(w,child))return false;}
        if(view->implementations)for(uint32_t i=0;i<view->implementations->count;++i){if(!type_work(w,sizeof(XrXirImplementation)))return false;
            const XrXirImplementation *row=&view->implementations->records[i];if(row->interface.declaration!=selected.table)continue;
            TypeSymbol child;if(!type_table_symbol(w,root,false,row->nominal_declaration,&child)||!type_append(w,child))return false;}}
    return true;
}
static bool type_before(TypeWork *w,TypeSymbol a,TypeSymbol b,bool *out) {
    const char *x=w->request->documents[a.document].uri,*y=w->request->documents[b.document].uri;
    for(;;){if(!type_work(w,2))return false;unsigned char p=(unsigned char)*x++,q=(unsigned char)*y++;
        if(p!=q){*out=p<q;return true;}if(!p)break;}
    int order=type_position_compare(a.selection.start,b.selection.start);if(!order)order=type_position_compare(a.selection.end,b.selection.end);
    *out=order<0;return true;
}
static bool type_sort(TypeWork *w) {
    for(size_t i=1;i<w->count;++i){TypeSymbol current=w->rows[i];size_t at=i;
        while(at){bool before=false;if(!type_before(w,current,w->rows[at-1],&before)){w->rows[at]=current;return false;}if(!before)break;
            w->rows[at]=w->rows[at-1];--at;}w->rows[at]=current;}return true;
}
