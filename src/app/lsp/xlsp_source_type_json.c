/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xlsp_source_inlay.h"
#include "xlsp_source_signature.h"
#include "xlsp_index_json.h"
#include <limits.h>
static XrXirStatus type_json_work(XrCompileResources *resources,size_t work) {
    XrCompileResourceStatus status=xr_compile_resources_work(resources,work);
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus type_json_equal(XrCompileResources *resources,const XlspSourceTypeHints *a,const XlspSourceTypeHints *b) {
    if(a->count!=b->count)return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;i<a->count;++i) {
        const XlspSourceTypeHint *left=&a->items[i],*right=&b->items[i];
        if(left->position.line!=right->position.line||left->position.character!=right->position.character||
            left->label_length!=right->label_length||left->identity.length!=right->identity.length)return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=type_json_work(resources,left->label_length);if(status!=XR_XIR_OK)return status;
        if(memcmp(left->label,right->label,left->label_length))return XR_XIR_BAD_STRUCTURE;
        status=type_json_work(resources,left->identity.length);if(status!=XR_XIR_OK)return status;
        if(!left->identity.length||memcmp(left->identity.text,right->identity.text,left->identity.length))return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus type_json_render(XrCompileResources *resources,const XlspSourceTypeHints *hints,XrJsonValue **out) {
    if(hints->count>INT_MAX)return XR_XIR_BUDGET;
    XrXirStatus status=type_json_work(resources,sizeof(XrJsonValue));if(status!=XR_XIR_OK)return status;
    XrJsonValue *result=xjson_new_array();if(!result)return XR_XIR_OUT_OF_MEMORY;
    for(size_t i=0;i<hints->count;++i) {
        const XlspSourceTypeHint *hint=&hints->items[i];
        status=type_json_work(resources,sizeof(XrJsonValue)*7);if(status!=XR_XIR_OK)goto done;
        status=type_json_work(resources,hint->label_length);if(status!=XR_XIR_OK)goto done;
        XrJsonValue *item=xjson_new_object();status=XR_XIR_OUT_OF_MEMORY;if(!item)goto done;
        if(!index_json_member(item,"position",index_json_position(hint->position))||
            !index_json_member(item,"label",xjson_new_string_n(hint->label,hint->label_length))||
            !index_json_member(item,"kind",xjson_new_number(1))||!index_json_member(item,"paddingLeft",xjson_new_bool(true))) {
            xjson_free(item);goto done;
        }
        if(result->as.array.count==result->as.array.capacity&&result->as.array.capacity>INT_MAX/2) {
            xjson_free(item);status=XR_XIR_BUDGET;goto done;
        }
        int before=result->as.array.count;xjson_array_push(result,item);
        if(result->as.array.count!=before+1){xjson_free(item);goto done;}
    }
    *out=result;result=NULL;status=XR_XIR_OK;
done:
    xjson_free(result);return status;
}
XR_FUNC XrXirStatus xlsp_source_type_hints_many_json(const XlspSourceTypeHintRequest *request,XrJsonValue **out) {
    if(!request||!request->resources||!request->snapshots||!request->count||!request->document.uri||
        !request->document.text||request->document.length==SIZE_MAX||request->document.uri_length==SIZE_MAX||
        request->count>SIZE_MAX/sizeof(*request->snapshots)||!out||*out)return XR_XIR_BAD_STRUCTURE;
    XlspSourceTypeHints accepted={0},current={0};bool selected=false;XrXirStatus status=XR_XIR_OK;
    for(size_t i=0;i<request->count;++i) {
        status=type_json_work(request->resources,sizeof(*request->snapshots));if(status!=XR_XIR_OK)goto done;
        const XlspSourceSnapshot *snapshot=request->snapshots[i];
        if(!xlsp_source_snapshot_uses_resources(snapshot,request->resources)){status=XR_XIR_BAD_STRUCTURE;goto done;}
        bool contains=false;status=xlsp_source_snapshot_contains(snapshot,request->document.uri,request->document.uri_length,&contains);
        if(status!=XR_XIR_OK)goto done;if(!contains)continue;
        XlspSourceDocument actual={0};status=xlsp_source_snapshot_document(snapshot,request->document.uri,request->document.uri_length,&actual);
        if(status!=XR_XIR_OK)goto done;
        if(actual.version!=request->document.version||actual.length!=request->document.length){status=XR_XIR_BAD_STRUCTURE;goto done;}
        status=type_json_work(request->resources,actual.length);if(status!=XR_XIR_OK)goto done;
        if(memcmp(actual.text,request->document.text,actual.length)){status=XR_XIR_BAD_STRUCTURE;goto done;}
        status=xlsp_source_type_hints(snapshot,actual.uri,actual.uri_length,request->range,&current);if(status!=XR_XIR_OK)goto done;
        if(selected){status=type_json_equal(request->resources,&accepted,&current);if(status!=XR_XIR_OK)goto done;xlsp_source_type_hints_free(&current);}
        else {accepted=current;current=(XlspSourceTypeHints){0};selected=true;}
    }
    status=selected?type_json_render(request->resources,&accepted,out):XR_XIR_UNRESOLVED;
done:
    xlsp_source_type_hints_free(&current);xlsp_source_type_hints_free(&accepted);return status;
}
