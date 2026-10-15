/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static XrJsonValue *type_json_object(TypeWork *w,bool array) {
    if(!type_work(w,64))return NULL;XrJsonValue *value=array?xjson_new_array():xjson_new_object();
    if(!value)type_fail(w,XR_XIR_OUT_OF_MEMORY);return value;
}
static XrJsonValue *type_json_string(TypeWork *w,const char *text,size_t length) {
    if(!type_work(w,length))return NULL;XrJsonValue *value=xjson_new_string_n(text,length);
    if(!value)type_fail(w,XR_XIR_OUT_OF_MEMORY);return value;
}
static XrJsonValue *type_json_literal(TypeWork *w,const char *text) {
    size_t length=0;return type_length(w,text,&length)?type_json_string(w,text,length):NULL;
}
static XrJsonValue *type_json_integer(TypeWork *w,uint32_t integer) {
    if(!type_work(w,16))return NULL;XrJsonValue *value=xjson_new_number(0);
    if(!value){type_fail(w,XR_XIR_OUT_OF_MEMORY);return NULL;}
    value->is_integer=true;value->as.integer=integer;return value;
}
/* Both helpers consume value, including an append failure. */
static bool type_member(TypeWork *w,XrJsonValue *object,const char *key,XrJsonValue *value) {
    if(!value)return false;if(!type_work(w,64)){xjson_free(value);return false;}
    int before=object->as.object.count;xjson_object_set_new(object,key,value);
    if(object->as.object.count==before){xjson_free(value);return type_fail(w,XR_XIR_OUT_OF_MEMORY);}
    if(object->as.object.count!=before+1)return type_fail(w,XR_XIR_BAD_STRUCTURE);
    const XrJsonMember *member=&object->as.object.members[before];
    if(!member->key)return type_fail(w,XR_XIR_OUT_OF_MEMORY);
    size_t length=strlen(key);
    return (member->value==value&&member->key_len==length&&!memcmp(member->key,key,length))||type_fail(w,XR_XIR_BAD_STRUCTURE);
}
static bool type_push(TypeWork *w,XrJsonValue *array,XrJsonValue *value) {
    if(!value)return false;if(!type_work(w,64)||array->as.array.count==INT_MAX){xjson_free(value);return type_fail(w,XR_XIR_BUDGET);}
    int before=array->as.array.count;xjson_array_push(array,value);
    if(array->as.array.count==before){xjson_free(value);return type_fail(w,XR_XIR_OUT_OF_MEMORY);}
    return array->as.array.count==before+1||type_fail(w,XR_XIR_BAD_STRUCTURE);
}
static XrJsonValue *type_json_position(TypeWork *w,XrLspPosition position) {
    XrJsonValue *value=type_json_object(w,false);if(!value)return NULL;
    if(!type_member(w,value,"line",type_json_integer(w,position.line))||
        !type_member(w,value,"character",type_json_integer(w,position.character))){xjson_free(value);return NULL;}
    return value;
}
static XrJsonValue *type_json_range(TypeWork *w,XrLspRange range) {
    XrJsonValue *value=type_json_object(w,false);if(!value)return NULL;
    if(!type_member(w,value,"start",type_json_position(w,range.start))||!type_member(w,value,"end",type_json_position(w,range.end))) {
        xjson_free(value);return NULL;}return value;
}
static bool type_fingerprint(TypeWork *w,TypeSymbol symbol,char *out) {
    static const char hex[]="0123456789abcdef";if(!type_work(w,3*XR_FINGERPRINT_BYTES))return false;
    const XrFingerprint *fp=&type_view(w,symbol.root)->modules[symbol.module].fingerprint;
    for(size_t i=0;i<XR_FINGERPRINT_BYTES;++i){out[i*2]=hex[fp->bytes[i]>>4];out[i*2+1]=hex[fp->bytes[i]&15];}
    out[XR_FINGERPRINT_BYTES*2]=0;return true;
}
static bool type_revision(TypeWork *w,char *out) {
    if(!type_work(w,16))return false;return snprintf(out,17,"%016llx",(unsigned long long)w->request->revision)==16||type_fail(w,XR_XIR_BAD_STRUCTURE);
}
static bool type_version(TypeWork *w,TypeSymbol symbol,char *out) {
    if(!type_work(w,32))return false;int n=snprintf(out,32,"%lld",(long long)w->request->documents[symbol.document].version);
    return (n>0&&n<32)||type_fail(w,XR_XIR_BAD_STRUCTURE);
}
static const char *type_symbol_name(TypeWork *w,TypeSymbol symbol) {
    return type_view(w,symbol.root)->declarations[symbol.declaration-1].name;
}
static uint32_t type_symbol_kind(TypeSymbol symbol) {
    return symbol.kind==XR_XIR_SOURCE_TYPE_CLASS?LSP_SYMBOL_CLASS:symbol.kind==XR_XIR_SOURCE_TYPE_INTERFACE?LSP_SYMBOL_INTERFACE:
        symbol.kind==XR_XIR_SOURCE_TYPE_ENUM?LSP_SYMBOL_ENUM:LSP_SYMBOL_STRUCT;
}
static XrJsonValue *type_json_data(TypeWork *w,TypeSymbol symbol) {
    char fingerprint[XR_FINGERPRINT_BYTES*2+1],revision[17],version[32];
    if(!type_fingerprint(w,symbol,fingerprint)||!type_revision(w,revision)||!type_version(w,symbol,version))return NULL;
    XrJsonValue *data=type_json_object(w,false);if(!data)return NULL;
    if(!type_member(w,data,"schema",type_json_literal(w,"xray-source-type-1"))||
        !type_member(w,data,"identity",type_json_literal(w,type_view(w,symbol.root)->modules[symbol.module].identity))||
        !type_member(w,data,"fingerprint",type_json_literal(w,fingerprint))||!type_member(w,data,"revision",type_json_literal(w,revision))||
        !type_member(w,data,"version",w->request->known_versions[symbol.document]?type_json_literal(w,version):xjson_new_null())){
        xjson_free(data);return NULL;}return data;
}
static XrJsonValue *type_json_item(TypeWork *w,TypeSymbol symbol) {
    const XlspSourceDocument *d=&w->request->documents[symbol.document];XrJsonValue *item=type_json_object(w,false);if(!item)return NULL;
    if(!type_member(w,item,"name",type_json_literal(w,type_symbol_name(w,symbol)))||
        !type_member(w,item,"kind",type_json_integer(w,type_symbol_kind(symbol)))||
        !type_member(w,item,"uri",type_json_string(w,d->uri,d->uri_length))||
        !type_member(w,item,"range",type_json_range(w,symbol.range))||!type_member(w,item,"selectionRange",type_json_range(w,symbol.selection))||
        !type_member(w,item,"data",type_json_data(w,symbol))){xjson_free(item);return NULL;}return item;
}
static XrJsonValue *type_json_result(TypeWork *w) {
    XrJsonValue *result=type_json_object(w,true);if(!result)return NULL;
    if(!w->request->mode){if(!type_push(w,result,type_json_item(w,w->selected))){xjson_free(result);return NULL;}}
    else for(size_t i=0;i<w->count;++i)if(!type_push(w,result,type_json_item(w,w->rows[i]))){xjson_free(result);return NULL;}
    return result;
}
