/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_signature.c - Signature help from parser and Checked Source facts
 */
#include "xlsp_source_signature.h"
#include <limits.h>
#include <string.h>

static XrXirStatus signature_work(XrCompileResources *resources,size_t bytes) {
    XrCompileResourceStatus status=xr_compile_resources_work(resources,bytes);
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?
        XR_XIR_OUT_OF_MEMORY:status==XR_COMPILE_RESOURCE_BAD_ARGUMENT?XR_XIR_BAD_STRUCTURE:XR_XIR_BUDGET;
}
static bool signature_member(XrJsonValue *object,const char *key,XrJsonValue *value) {
    if(!value)return false;
    int before=object->as.object.count;xjson_object_set_new(object,key,value);
    if(object->as.object.count==before){xjson_free(value);return false;}
    if(object->as.object.count!=before+1)return false;
    const XrJsonMember *member=&object->as.object.members[before];
    return member->value==value&&member->key&&member->key_len==strlen(key)&&!memcmp(member->key,key,member->key_len);
}
static bool signature_adopt(XrJsonValue *object,const char *key,XrJsonValue **value) {
    XrJsonValue *child=*value;*value=NULL;return signature_member(object,key,child);
}
static bool signature_push(XrJsonValue *array,XrJsonValue **value) {
    XrJsonValue *child=*value;*value=NULL;int before=array->as.array.count;
    if(before==array->as.array.capacity&&array->as.array.capacity>INT_MAX/2){xjson_free(child);return false;}
    xjson_array_push(array,child);
    if(array->as.array.count==before+1)return true;
    xjson_free(child);return false;
}
static XrXirStatus signature_json(XrCompileResources *resources,const XlspSourceSignature *signature,XrJsonValue **out) {
    XrXirStatus status=signature_work(resources,signature->label.length);
    if(status!=XR_XIR_OK)return status;
    XrJsonValue *result=xjson_new_object(),*signatures=xjson_new_array();
    XrJsonValue *info=xjson_new_object(),*parameters=xjson_new_array();XrJsonValue *item=NULL;
    status=XR_XIR_OUT_OF_MEMORY;
    if(!result||!signatures||!info||!parameters)goto done;
    if(!signature_member(info,"label",xjson_new_string_n(signature->label.text,signature->label.length)))goto done;
    if(signature->parameter_count>INT_MAX){status=XR_XIR_BUDGET;goto done;}
    for(size_t i=0;i<signature->parameter_count;++i) {
        status=signature_work(resources,signature->parameters[i].length+1);
        if(status!=XR_XIR_OK)goto done;status=XR_XIR_OUT_OF_MEMORY;
        item=xjson_new_object();if(!item)goto done;
        if(!signature_member(item,"label",xjson_new_string_n(signature->parameters[i].text,
            signature->parameters[i].length))||!signature_push(parameters,&item))goto done;
    }
    if(!signature_adopt(info,"parameters",&parameters)||!signature_push(signatures,&info)||
        !signature_adopt(result,"signatures",&signatures)||
        !signature_member(result,"activeSignature",xjson_new_number(0)))goto done;
    if(signature->active_argument<signature->parameter_count&&
        !signature_member(result,"activeParameter",xjson_new_number((double)signature->active_argument)))goto done;
    *out=result;result=NULL;status=XR_XIR_OK;
done:
    xjson_free(item);xjson_free(parameters);xjson_free(info);xjson_free(signatures);xjson_free(result);return status;
}
static XrXirStatus signature_equal(XrCompileResources *resources,const XlspSourceSignature *a,
    const XlspSourceSignature *b) {
    if(a->parameter_count!=b->parameter_count||a->active_argument!=b->active_argument||
        a->label.length!=b->label.length)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=signature_work(resources,a->label.length);if(status!=XR_XIR_OK)return status;
    if(memcmp(a->label.text,b->label.text,a->label.length))return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;i<a->parameter_count;++i) {
        if(a->parameters[i].length!=b->parameters[i].length)return XR_XIR_BAD_STRUCTURE;
        status=signature_work(resources,a->parameters[i].length);if(status!=XR_XIR_OK)return status;
        if(memcmp(a->parameters[i].text,b->parameters[i].text,a->parameters[i].length))return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus signature_document_equal(XrCompileResources *resources,const XlspSourceDocument *a,
    const XlspSourceDocument *b) {
    if(a->version!=b->version||a->length!=b->length||a->uri_length!=b->uri_length)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=signature_work(resources,a->uri_length);if(status!=XR_XIR_OK)return status;
    if(memcmp(a->uri,b->uri,a->uri_length))return XR_XIR_BAD_STRUCTURE;
    status=signature_work(resources,a->length);if(status!=XR_XIR_OK)return status;
    return memcmp(a->text,b->text,a->length)?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
}
static XrXirStatus signature_target_equal(XrCompileResources *resources,const XlspSourceLocation *a,
    const XlspSourceLocation *b) {
    if(a->version!=b->version||a->range.start.line!=b->range.start.line||a->range.start.character!=b->range.start.character||
        a->range.end.line!=b->range.end.line||a->range.end.character!=b->range.end.character)return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;;++i) {
        XrXirStatus status=signature_work(resources,2);if(status!=XR_XIR_OK)return status;
        if(a->uri[i]!=b->uri[i])return XR_XIR_BAD_STRUCTURE;
        if(!a->uri[i])return XR_XIR_OK;
    }
}

XR_FUNC XrXirStatus xlsp_source_signature_many_json(const XlspSourceSignatureRequest *request,XrJsonValue **out) {
    if(!request||!request->resources||!request->session||!request->snapshots||!request->count||!request->uri||
        request->uri_length==SIZE_MAX||!out||*out||
        xr_compile_state_resources(xr_compile_session_compile_state(request->session))!=request->resources)
        return XR_XIR_BAD_STRUCTURE;
    XlspSyntaxSnapshot *syntax=NULL;XlspSourceDocument document={0};XlspSourceLocation target={0};
    XlspSourceSignature accepted={0},current={0};XlspSourceSignatureSelection selection={0};
    selection.resources=request->resources;selection.uri=request->uri;selection.uri_length=request->uri_length;
    XrXirStatus status=XR_XIR_OK;bool selected=false;
    for(size_t i=0;i<request->count;++i) {
        bool contains=false;status=xlsp_source_snapshot_contains(request->snapshots[i],request->uri,request->uri_length,&contains);
        if(status!=XR_XIR_OK)goto done;if(!contains)continue;
        XlspSourceDocument input={0};
        status=xlsp_source_snapshot_document(request->snapshots[i],request->uri,request->uri_length,&input);
        if(status!=XR_XIR_OK)goto done;
        if(!selected) {
            document=input;
            XrParseStatus parsed=xlsp_source_syntax_build(request->session,&document,&syntax);
            if(parsed!=XR_PARSE_OK&&parsed!=XR_PARSE_RECOVERED) {
                status=parsed==XR_PARSE_BUDGET?XR_XIR_BUDGET:parsed==XR_PARSE_OUT_OF_MEMORY?
                    XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;goto done;
            }
            status=xlsp_source_syntax_call(syntax,request->position,&selection.call);
            if(status==XR_XIR_UNRESOLVED){*out=xjson_new_null();status=*out?XR_XIR_OK:XR_XIR_OUT_OF_MEMORY;goto done;}
            if(status!=XR_XIR_OK)goto done;selected=true;
        } else {
            status=signature_document_equal(request->resources,&document,&input);if(status!=XR_XIR_OK)goto done;
        }
        status=xlsp_source_signature(request->snapshots[i],&selection,&current);
        if(status!=XR_XIR_OK)goto done;
        XlspSourceLocation location={0};
        status=xlsp_source_definition(request->snapshots[i],request->uri,request->uri_length,selection.call.callee.start,&location);
        if(status!=XR_XIR_OK)goto done;
        if(accepted.label.text) {
            status=signature_target_equal(request->resources,&target,&location);if(status!=XR_XIR_OK)goto done;
            status=signature_equal(request->resources,&accepted,&current);if(status!=XR_XIR_OK)goto done;
            xlsp_source_signature_free(&current);
        } else {accepted=current;current=(XlspSourceSignature){0};target=location;}
    }
    if(!accepted.label.text){status=XR_XIR_UNRESOLVED;goto done;}
    status=signature_json(request->resources,&accepted,out);
done:
    xlsp_source_signature_free(&current);xlsp_source_signature_free(&accepted);xlsp_source_syntax_free(syntax);
    return status;
}
