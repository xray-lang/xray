/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static bool call_check_range(CallWork *w,const XrJsonValue *object,XrLspRange expected) {
    const XrJsonValue *start=NULL,*end=NULL;XrLspRange actual;
    if(!call_field(w,object,"start",&start)||!call_field(w,object,"end",&end)||
        !call_protocol_position(w,start,&actual.start)||!call_protocol_position(w,end,&actual.end))return false;
    return call_range_same(actual,expected)||call_fail(w,XR_XIR_UNRESOLVED);
}
static bool call_check_text_field(CallWork *w,const XrJsonValue *object,const char *key,const char *text) {
    const XrJsonValue *value=NULL;size_t length=0;
    return call_field(w,object,key,&value)&&call_length(w,text,&length)&&call_json_text(w,value,text,length);
}
static bool call_check_item(CallWork *w,const XrJsonValue *item) {
    const XrJsonValue *kind=NULL,*uri=NULL,*range=NULL,*selection=NULL,*data=NULL,*version=NULL,*initializer=NULL;
    CallSymbol symbol=w->selected;const XlspSourceDocument *d=&w->request->documents[symbol.document];
    char fingerprint[XR_FINGERPRINT_BYTES*2+1],revision[17],version_text[32];
    if(!call_field(w,item,"kind",&kind)||!call_field(w,item,"uri",&uri)||!call_field(w,item,"range",&range)||
        !call_field(w,item,"selectionRange",&selection)||!call_field(w,item,"data",&data)||
        !call_check_text_field(w,item,"name",call_symbol_name(w,symbol)))return false;
    if(kind->type!=XR_JSON_NUMBER||!kind->is_integer||kind->as.integer!=call_symbol_kind(w,symbol))return call_fail(w,XR_XIR_BAD_STRUCTURE);
    if(!call_json_text(w,uri,d->uri,d->uri_length)||!call_check_range(w,range,symbol.range)||!call_check_range(w,selection,symbol.selection))return false;
    if(data->type!=XR_JSON_OBJECT||data->as.object.count!=6)return call_fail(w,XR_XIR_BAD_STRUCTURE);
    if(!call_fingerprint(w,symbol,fingerprint)||!call_revision(w,revision)||!call_version(w,symbol,version_text)||
        !call_check_text_field(w,data,"schema","xray-source-call-1")||
        !call_check_text_field(w,data,"identity",call_view(w,symbol.root)->modules[symbol.module].identity)||
        !call_check_text_field(w,data,"fingerprint",fingerprint)||!call_check_text_field(w,data,"revision",revision)||
        !call_field(w,data,"version",&version)||!call_field(w,data,"moduleInitializer",&initializer))return false;
    if(initializer->type!=XR_JSON_BOOL||initializer->as.boolean!=symbol.initializer)return call_fail(w,XR_XIR_BAD_STRUCTURE);
    if(w->request->known_versions[symbol.document])return call_json_text(w,version,version_text,strlen(version_text));
    return version->type==XR_JSON_NULL||call_fail(w,XR_XIR_UNRESOLVED);
}
static bool call_request_select(CallWork *w) {
    const XrJsonValue *object=NULL,*uri=NULL,*position=NULL,*data=NULL,*initializer=NULL;XrLspPosition at={0};bool module=false;
    const XlspSourceHierarchyRequest *request=w->request;
    if(!call_field(w,request->params,request->mode?"item":"textDocument",&object)||!call_field(w,object,"uri",&uri))return false;
    const XlspSourceDocument *d=&request->documents[request->entry];if(!call_json_text(w,uri,d->uri,d->uri_length))return false;
    if(!request->mode){if(!call_field(w,request->params,"position",&position)||!call_protocol_position(w,position,&at))return false;}
    else {const XrJsonValue *selection=NULL;if(!call_field(w,object,"selectionRange",&selection)||!call_field(w,selection,"start",&position)||
        !call_protocol_position(w,position,&at)||!call_field(w,object,"data",&data)||!call_field(w,data,"moduleInitializer",&initializer))return false;
        if(initializer->type!=XR_JSON_BOOL)return call_fail(w,XR_XIR_BAD_STRUCTURE);module=initializer->as.boolean;}
    if(!call_cursor(w,at)||!call_pick(w,at,module))return false;return !request->mode||call_check_item(w,object);
}
XR_FUNC XrXirStatus xlsp_source_call_hierarchy_json(const XlspSourceHierarchyRequest *request,XrJsonValue **out) {
    if(!request||!request->context||!request->context->resources||!request->documents||!request->known_versions||
        !request->count||request->entry>=request->count||request->mode>2||!request->params||!out||*out)return XR_XIR_BAD_STRUCTURE;
    CallWork w={0};w.request=request;w.resources=request->context->resources;w.status=XR_XIR_OK;XrJsonValue *result=NULL;
    w.status=xlsp_source_bindings_build(request->context,request->documents,request->count,&w.bindings);
    if(w.status!=XR_XIR_OK)goto done;
    for(size_t i=0;i<request->count;++i){const XrXirSourceView *view=call_view(&w,i);const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(w.bindings,i);
        const XrXirSourceCalls *calls=xlsp_source_bindings_calls(w.bindings,i);
        if(!call_work(&w,sizeof(*view)+sizeof(*syntax)+sizeof(*calls)))goto done;
        if(!view||!view->complete||!syntax||!calls||calls->declaration_count!=view->declaration_count||
            syntax->declaration_count!=view->declaration_count||syntax->reference_count!=view->reference_count){call_fail(&w,XR_XIR_UNRESOLVED);goto done;}}
    if(!call_request_select(&w))goto done;
    if(request->mode&&(!call_edges(&w)||!call_sort(&w)))goto done;
    result=call_json_result(&w);
    if(!result&&w.status==XR_XIR_OK)call_fail(&w,XR_XIR_OUT_OF_MEMORY);
    if(w.status==XR_XIR_OK){*out=result;result=NULL;}
done:
    xjson_free(result);for(size_t i=0;i<w.count;++i)xr_compile_resources_free(w.edges[i].sites);
    xr_compile_resources_free(w.edges);xlsp_source_bindings_free(w.bindings);return w.status;
}
