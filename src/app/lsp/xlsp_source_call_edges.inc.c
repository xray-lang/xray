/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static bool call_site(CallWork *w,CallEdge *edge,XrLspRange site) {
    for(size_t i=0;i<edge->count;++i){if(!call_work(w,sizeof(site)))return false;if(call_range_same(edge->sites[i],site))return true;}
    if(edge->count==edge->capacity){size_t next=edge->capacity?edge->capacity*2:4;
        if(next<edge->capacity||next>INT_MAX||next>SIZE_MAX/sizeof(site))return call_fail(w,XR_XIR_BUDGET);
        if(!call_resource(w,xr_compile_resources_resize(w->resources,(void **)&edge->sites,next*sizeof(site))))return false;edge->capacity=next;}
    size_t at=edge->count;
    while(at){if(!call_work(w,sizeof(site)))return false;if(call_position_compare(edge->sites[at-1].start,site.start)<=0)break;edge->sites[at]=edge->sites[at-1];--at;}
    edge->sites[at]=site;++edge->count;return true;
}
static bool call_edge(CallWork *w,CallSymbol other,XrLspRange site) {
    for(size_t i=0;i<w->count;++i){bool same=false;if(!call_symbol_same(w,w->edges[i].other,other,&same))return false;
        if(same)return call_site(w,&w->edges[i],site);}
    if(w->count==w->capacity){size_t next=w->capacity?w->capacity*2:8;
        if(next<w->capacity||next>INT_MAX||next>SIZE_MAX/sizeof(CallEdge))return call_fail(w,XR_XIR_BUDGET);
        if(!call_resource(w,xr_compile_resources_resize(w->resources,(void **)&w->edges,next*sizeof(CallEdge))))return false;w->capacity=next;}
    if(!call_work(w,sizeof(CallEdge)))return false;
    size_t index=w->count++;w->edges[index]=(CallEdge){other,NULL,0,0};return call_site(w,&w->edges[index],site);
}
static bool call_incoming(CallWork *w,size_t root,const XrXirSourceCall *call) {
    const XrXirSourceView *view=call_view(w,root);if(w->selected.initializer)return true;
    if(call->reference==UINT32_MAX)return call_fail(w,XR_XIR_UNSUPPORTED);
    if(call->reference>=view->reference_count)return call_fail(w,XR_XIR_BAD_STRUCTURE);
    uint32_t id=view->references[call->reference].target;if(!call_target(w,view,&id))return false;
    bool same=false;if(!call_declaration_same(w,root,id,w->selected.root,w->selected.declaration,&same))return false;if(!same)return true;
    if(!call->selection_known||(!call->caller&&!call->module_initializer))return call_fail(w,XR_XIR_UNSUPPORTED);
    CallSymbol caller;if(!(call->module_initializer?call_module_symbol(w,root,call->range.module,&caller):call_resolve_symbol(w,root,call->caller,&caller)))return false;
    XrLspRange site;return call_span(w,caller.document,call->range,&site)&&call_edge(w,caller,site);
}
static bool call_outgoing(CallWork *w,size_t root,const XrXirSourceCall *call) {
    bool same=false;
    if(w->selected.initializer){if(!call->module_initializer)return true;if(!call_module_same(w,root,call->range.module,w->selected,&same))return false;}
    else {if(!call->caller)return true;if(!call_declaration_same(w,root,call->caller,w->selected.root,w->selected.declaration,&same))return false;}
    if(!same)return true;if(call->reference==UINT32_MAX||!call->selection_known)return call_fail(w,XR_XIR_UNSUPPORTED);
    const XrXirSourceView *view=call_view(w,root);if(call->reference>=view->reference_count)return call_fail(w,XR_XIR_BAD_STRUCTURE);
    uint32_t id=view->references[call->reference].target;if(!call_target(w,view,&id))return false;
    CallSymbol target;if(!call_resolve_symbol(w,root,id,&target))return false;
    XrLspRange site;return call_span(w,w->selected.document,call->range,&site)&&call_edge(w,target,site);
}
static bool call_edges(CallWork *w) {
    for(size_t root=0;root<w->request->count;++root){const XrXirSourceCalls *calls=xlsp_source_bindings_calls(w->bindings,root);
        for(uint32_t i=0;i<calls->count;++i){if(!call_work(w,sizeof(XrXirSourceCall)))return false;
            if(!(w->request->mode==1?call_incoming(w,root,&calls->items[i]):call_outgoing(w,root,&calls->items[i])))return false;}}
    return true;
}
static bool call_edge_order(CallWork *w,const CallEdge *a,const CallEdge *b,int *out) {
    const XlspSourceDocument *da=&w->request->documents[a->other.document],*db=&w->request->documents[b->other.document];
    size_t count=da->uri_length<db->uri_length?da->uri_length:db->uri_length;
    for(size_t i=0;i<count;++i){if(!call_work(w,2))return false;
        unsigned char x=(unsigned char)da->uri[i],y=(unsigned char)db->uri[i];if(x!=y){*out=x<y?-1:1;return true;}}
    if(da->uri_length!=db->uri_length){*out=da->uri_length<db->uri_length?-1:1;return true;}
    *out=call_position_compare(a->other.selection.start,b->other.selection.start);return true;
}
static bool call_sort(CallWork *w) {
    for(size_t i=1;i<w->count;++i){CallEdge current=w->edges[i];size_t at=i;
        while(at){int order=0;if(!call_edge_order(w,&w->edges[at-1],&current,&order)){w->edges[at]=current;return false;}if(order<=0)break;
            if(!call_work(w,sizeof(current))){w->edges[at]=current;return false;}w->edges[at]=w->edges[at-1];--at;}
        w->edges[at]=current;}
    return true;
}
