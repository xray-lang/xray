/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static XrJsonValue *call_json_object(CallWork *w,bool array) {
    if(!call_work(w,64))return NULL;XrJsonValue *value=array?xjson_new_array():xjson_new_object();
    if(!value)call_fail(w,XR_XIR_OUT_OF_MEMORY);return value;
}
static XrJsonValue *call_json_string(CallWork *w,const char *text,size_t length) {
    if(!call_work(w,length))return NULL;XrJsonValue *value=xjson_new_string_n(text,length);
    if(!value)call_fail(w,XR_XIR_OUT_OF_MEMORY);return value;
}
static XrJsonValue *call_json_literal(CallWork *w,const char *text) {
    size_t length=0;return call_length(w,text,&length)?call_json_string(w,text,length):NULL;
}
static XrJsonValue *call_json_integer(CallWork *w,uint32_t integer) {
    if(!call_work(w,16))return NULL;XrJsonValue *value=xjson_new_number(0);
    if(!value){call_fail(w,XR_XIR_OUT_OF_MEMORY);return NULL;}
    value->is_integer=true;value->as.integer=integer;return value;
}
/* Both helpers consume value, including an append failure. */
static bool call_member(CallWork *w,XrJsonValue *object,const char *key,XrJsonValue *value) {
    if(!value)return false;if(!call_work(w,64)){xjson_free(value);return false;}
    int before=object->as.object.count;xjson_object_set_new(object,key,value);
    if(object->as.object.count==before){xjson_free(value);return call_fail(w,XR_XIR_OUT_OF_MEMORY);}
    if(object->as.object.count!=before+1)return call_fail(w,XR_XIR_BAD_STRUCTURE);
    const XrJsonMember *member=&object->as.object.members[before];
    if(!member->key)return call_fail(w,XR_XIR_OUT_OF_MEMORY);
    size_t length=strlen(key);
    return (member->value==value&&member->key_len==length&&!memcmp(member->key,key,length))||call_fail(w,XR_XIR_BAD_STRUCTURE);
}
static bool call_push(CallWork *w,XrJsonValue *array,XrJsonValue *value) {
    if(!value)return false;if(!call_work(w,64)||array->as.array.count==INT_MAX){xjson_free(value);return call_fail(w,XR_XIR_BUDGET);}
    int before=array->as.array.count;xjson_array_push(array,value);
    if(array->as.array.count==before){xjson_free(value);return call_fail(w,XR_XIR_OUT_OF_MEMORY);}
    return array->as.array.count==before+1||call_fail(w,XR_XIR_BAD_STRUCTURE);
}
static XrJsonValue *call_json_position(CallWork *w,XrLspPosition position) {
    XrJsonValue *value=call_json_object(w,false);if(!value)return NULL;
    if(!call_member(w,value,"line",call_json_integer(w,position.line))||
        !call_member(w,value,"character",call_json_integer(w,position.character))){xjson_free(value);return NULL;}
    return value;
}
static XrJsonValue *call_json_range(CallWork *w,XrLspRange range) {
    XrJsonValue *value=call_json_object(w,false);if(!value)return NULL;
    if(!call_member(w,value,"start",call_json_position(w,range.start))||!call_member(w,value,"end",call_json_position(w,range.end))) {
        xjson_free(value);return NULL;}return value;
}
static bool call_fingerprint(CallWork *w,CallSymbol symbol,char *out) {
    static const char hex[]="0123456789abcdef";if(!call_work(w,3*XR_FINGERPRINT_BYTES))return false;
    const XrFingerprint *fp=&call_view(w,symbol.root)->modules[symbol.module].fingerprint;
    for(size_t i=0;i<XR_FINGERPRINT_BYTES;++i){out[i*2]=hex[fp->bytes[i]>>4];out[i*2+1]=hex[fp->bytes[i]&15];}
    out[XR_FINGERPRINT_BYTES*2]=0;return true;
}
static bool call_revision(CallWork *w,char *out) {
    if(!call_work(w,16))return false;return snprintf(out,17,"%016llx",(unsigned long long)w->request->revision)==16||call_fail(w,XR_XIR_BAD_STRUCTURE);
}
static bool call_version(CallWork *w,CallSymbol symbol,char *out) {
    if(!call_work(w,32))return false;int n=snprintf(out,32,"%lld",(long long)w->request->documents[symbol.document].version);
    return (n>0&&n<32)||call_fail(w,XR_XIR_BAD_STRUCTURE);
}
static const char *call_symbol_name(CallWork *w,CallSymbol symbol) {
    if(symbol.initializer)return "module initialization";
    if(xlsp_source_bindings_syntax(w->bindings,symbol.root)->declarations[symbol.declaration-1].role==XR_XIR_SOURCE_SYNTAX_CLOSURE)return "anonymous function";
    return call_view(w,symbol.root)->declarations[symbol.declaration-1].name;
}
static int call_symbol_kind(CallWork *w,CallSymbol symbol) {
    return symbol.initializer?LSP_SYMBOL_MODULE:xlsp_source_bindings_syntax(w->bindings,symbol.root)->declarations[symbol.declaration-1].role==XR_XIR_SOURCE_SYNTAX_METHOD?LSP_SYMBOL_METHOD:LSP_SYMBOL_FUNCTION;
}
static XrJsonValue *call_json_data(CallWork *w,CallSymbol symbol) {
    char fingerprint[XR_FINGERPRINT_BYTES*2+1],revision[17],version[32];
    if(!call_fingerprint(w,symbol,fingerprint)||!call_revision(w,revision)||!call_version(w,symbol,version))return NULL;
    XrJsonValue *data=call_json_object(w,false);if(!data)return NULL;
    if(!call_member(w,data,"schema",call_json_literal(w,"xray-source-call-1"))||
        !call_member(w,data,"identity",call_json_literal(w,call_view(w,symbol.root)->modules[symbol.module].identity))||
        !call_member(w,data,"fingerprint",call_json_literal(w,fingerprint))||
        !call_member(w,data,"revision",call_json_literal(w,revision))||
        !call_member(w,data,"version",w->request->known_versions[symbol.document]?call_json_literal(w,version):xjson_new_null())||
        !call_member(w,data,"moduleInitializer",xjson_new_bool(symbol.initializer))) {
        xjson_free(data);return NULL;}return data;
}
static XrJsonValue *call_json_item(CallWork *w,CallSymbol symbol) {
    const XlspSourceDocument *d=&w->request->documents[symbol.document];XrJsonValue *item=call_json_object(w,false);if(!item)return NULL;
    if(!call_member(w,item,"name",call_json_literal(w,call_symbol_name(w,symbol)))||
        !call_member(w,item,"kind",call_json_integer(w,(uint32_t)call_symbol_kind(w,symbol)))||
        !call_member(w,item,"uri",call_json_string(w,d->uri,d->uri_length))||
        !call_member(w,item,"range",call_json_range(w,symbol.range))||
        !call_member(w,item,"selectionRange",call_json_range(w,symbol.selection))||
        !call_member(w,item,"data",call_json_data(w,symbol))) {xjson_free(item);return NULL;}return item;
}
static XrJsonValue *call_json_edge(CallWork *w,const CallEdge *edge) {
    XrJsonValue *value=call_json_object(w,false),*ranges=NULL;if(!value)return NULL;
    if(!call_member(w,value,w->request->mode==1?"from":"to",call_json_item(w,edge->other)))goto failed;
    ranges=call_json_object(w,true);if(!ranges)goto failed;
    for(size_t i=0;i<edge->count;++i)if(!call_push(w,ranges,call_json_range(w,edge->sites[i])))goto failed;
    {XrJsonValue *moved=ranges;ranges=NULL;if(!call_member(w,value,"fromRanges",moved))goto failed;}return value;
failed:
    xjson_free(ranges);xjson_free(value);return NULL;
}
static XrJsonValue *call_json_result(CallWork *w) {
    XrJsonValue *result=call_json_object(w,true);if(!result)return NULL;
    if(!w->request->mode){if(!call_push(w,result,call_json_item(w,w->selected))){xjson_free(result);return NULL;}}
    else for(size_t i=0;i<w->count;++i)if(!call_push(w,result,call_json_edge(w,&w->edges[i]))){xjson_free(result);return NULL;}
    return result;
}
