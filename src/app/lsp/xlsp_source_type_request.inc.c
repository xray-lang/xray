/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static bool type_check_range(TypeWork *w,const XrJsonValue *object,XrLspRange expected) {
    const XrJsonValue *start=NULL,*end=NULL;XrLspRange actual;
    if(!type_field(w,object,"start",&start)||!type_field(w,object,"end",&end)||
        !type_protocol_position(w,start,&actual.start)||!type_protocol_position(w,end,&actual.end))return false;
    return type_range_same(actual,expected)||type_fail(w,XR_XIR_UNRESOLVED);
}
static bool type_check_text_field(TypeWork *w,const XrJsonValue *object,const char *key,const char *text) {
    const XrJsonValue *value=NULL;size_t length=0;
    return type_field(w,object,key,&value)&&type_length(w,text,&length)&&type_json_text(w,value,text,length);
}
static bool type_check_item(TypeWork *w,const XrJsonValue *item) {
    const XrJsonValue *kind=NULL,*uri=NULL,*range=NULL,*selection=NULL,*data=NULL,*version=NULL;
    TypeSymbol symbol=w->selected;const XlspSourceDocument *d=&w->request->documents[symbol.document];
    char fingerprint[XR_FINGERPRINT_BYTES*2+1],revision[17],version_text[32];
    if(!type_field(w,item,"kind",&kind)||!type_field(w,item,"uri",&uri)||!type_field(w,item,"range",&range)||
        !type_field(w,item,"selectionRange",&selection)||!type_field(w,item,"data",&data)||
        !type_check_text_field(w,item,"name",type_symbol_name(w,symbol)))return false;
    if(kind->type!=XR_JSON_NUMBER||!kind->is_integer||kind->as.integer!=type_symbol_kind(symbol))return type_fail(w,XR_XIR_BAD_STRUCTURE);
    if(!type_json_text(w,uri,d->uri,d->uri_length)||!type_check_range(w,range,symbol.range)||!type_check_range(w,selection,symbol.selection))return false;
    if(data->type!=XR_JSON_OBJECT||data->as.object.count!=5)return type_fail(w,XR_XIR_BAD_STRUCTURE);
    if(!type_fingerprint(w,symbol,fingerprint)||!type_revision(w,revision)||!type_version(w,symbol,version_text)||
        !type_check_text_field(w,data,"schema","xray-source-type-1")||
        !type_check_text_field(w,data,"identity",type_view(w,symbol.root)->modules[symbol.module].identity)||
        !type_check_text_field(w,data,"fingerprint",fingerprint)||!type_check_text_field(w,data,"revision",revision)||
        !type_field(w,data,"version",&version))return false;
    if(w->request->known_versions[symbol.document])return type_json_text(w,version,version_text,strlen(version_text));
    return version->type==XR_JSON_NULL||type_fail(w,XR_XIR_UNRESOLVED);
}
static bool type_request_select(TypeWork *w) {
    const XrJsonValue *object=NULL,*uri=NULL,*position=NULL;XrLspPosition at={0};
    const XlspSourceHierarchyRequest *request=w->request;
    if(!type_field(w,request->params,request->mode?"item":"textDocument",&object)||!type_field(w,object,"uri",&uri))return false;
    const XlspSourceDocument *d=&request->documents[request->entry];if(!type_json_text(w,uri,d->uri,d->uri_length))return false;
    if(!request->mode){if(!type_field(w,request->params,"position",&position)||!type_protocol_position(w,position,&at))return false;}
    else{const XrJsonValue *selection=NULL;if(!type_field(w,object,"selectionRange",&selection)||!type_field(w,selection,"start",&position)||!type_protocol_position(w,position,&at))return false;}
    if(!type_cursor(w,at)||!type_pick(w,at))return false;return !request->mode||type_check_item(w,object);
}
XR_FUNC XrXirStatus xlsp_source_type_hierarchy_json(const XlspSourceHierarchyRequest *request,XrJsonValue **out) {
    if(!request||!request->context||!request->context->resources||!request->documents||!request->known_versions||
        !request->count||request->entry>=request->count||request->mode>2||!request->params||!out||*out)return XR_XIR_BAD_STRUCTURE;
    TypeWork w={0};w.request=request;w.resources=request->context->resources;w.status=XR_XIR_OK;XrJsonValue *result=NULL;
    w.status=xlsp_source_bindings_build(request->context,request->documents,request->count,&w.bindings);if(w.status!=XR_XIR_OK)goto done;
    for(size_t i=0;i<request->count;++i){const XrXirSourceView *view=type_view(&w,i);const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(w.bindings,i);
        const XrXirSourceTypeFacts *facts=xlsp_source_bindings_types(w.bindings,i);
        if(!type_work(&w,sizeof(*view)+sizeof(*syntax)+sizeof(*facts)))goto done;
        if(!view||!view->complete||!syntax||!facts||facts->declaration_count!=view->declaration_count||
            syntax->declaration_count!=view->declaration_count||syntax->reference_count!=view->reference_count){type_fail(&w,XR_XIR_UNRESOLVED);goto done;}}
    if(!type_request_select(&w))goto done;
    if(request->mode&&(!(request->mode==1?type_supertypes(&w):type_subtypes(&w))||!type_sort(&w)))goto done;
    result=type_json_result(&w);if(!result&&w.status==XR_XIR_OK)type_fail(&w,XR_XIR_OUT_OF_MEMORY);
    if(w.status==XR_XIR_OK){*out=result;result=NULL;}
done:
    xjson_free(result);xr_compile_resources_free(w.rows);xlsp_source_bindings_free(w.bindings);return w.status;
}
