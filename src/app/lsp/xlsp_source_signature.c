/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_signature.c - Signature help from parser and Checked Source facts
 */
#include "xlsp_source_signature.h"
#include "../../xir/xxir_compile_memory.h"
#include <limits.h>
#include <string.h>
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
static XrXirStatus signature_json(const XlspSourceSignature *signature,XrJsonValue **out) {
    XrJsonValue *result=xjson_new_object(),*signatures=xjson_new_array();
    XrJsonValue *info=xjson_new_object(),*parameters=xjson_new_array();XrJsonValue *item=NULL;
    XrXirStatus status=XR_XIR_OUT_OF_MEMORY;
    if(!result||!signatures||!info||!parameters)goto done;
    if(!signature_member(info,"label",xjson_new_string_n(signature->label.text,signature->label.length)))goto done;
    if(signature->parameter_count>INT_MAX){status=XR_XIR_BUDGET;goto done;}
    for(size_t i=0;i<signature->parameter_count;++i) {
        status=XR_XIR_OUT_OF_MEMORY;
        item=xjson_new_object();if(!item)goto done;
        if(!signature_member(item,"label",xjson_new_string_n(signature->parameters[i].text,
            signature->parameters[i].length))||!signature_push(parameters,&item))goto done;
    }
    if(!signature_adopt(info,"parameters",&parameters)||!signature_push(signatures,&info)||
        !signature_adopt(result,"signatures",&signatures)||
        !signature_member(result,"activeSignature",xjson_new_number(0)))goto done;
    size_t active=signature->active_argument;
    if(signature->variadic&&signature->parameter_count&&active>=signature->parameter_count)active=signature->parameter_count-1;
    if(active<signature->parameter_count&&
        !signature_member(result,"activeParameter",xjson_new_number((double)active)))goto done;
    *out=result;result=NULL;status=XR_XIR_OK;
done:
    xjson_free(item);xjson_free(parameters);xjson_free(info);xjson_free(signatures);xjson_free(result);return status;
}
static XrXirStatus signature_equal(const XlspSourceSignature *a,
    const XlspSourceSignature *b) {
    if(a->parameter_count!=b->parameter_count||a->active_argument!=b->active_argument||a->variadic!=b->variadic||
        a->label.length!=b->label.length)return XR_XIR_BAD_STRUCTURE;
    if(memcmp(a->label.text,b->label.text,a->label.length))return XR_XIR_BAD_STRUCTURE;
    if(a->target_native_identity!=b->target_native_identity||
        a->target_identity.length!=b->target_identity.length||a->target_path.length!=b->target_path.length||
        a->target_range.line!=b->target_range.line||a->target_range.column!=b->target_range.column||
        a->target_range.end_line!=b->target_range.end_line||a->target_range.end_column!=b->target_range.end_column)
        return XR_XIR_BAD_STRUCTURE;
    if(memcmp(a->target_identity.text,b->target_identity.text,a->target_identity.length)||
        memcmp(a->target_path.text,b->target_path.text,a->target_path.length)||
        memcmp(&a->target_fingerprint,&b->target_fingerprint,sizeof(a->target_fingerprint)))return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;i<a->parameter_count;++i) {
        if(a->parameters[i].length!=b->parameters[i].length)return XR_XIR_BAD_STRUCTURE;
        if(memcmp(a->parameters[i].text,b->parameters[i].text,a->parameters[i].length))return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus signature_document_equal(const XlspSourceDocument *a,
    const XlspSourceDocument *b) {
    if(a->version!=b->version||a->length!=b->length||a->uri_length!=b->uri_length)return XR_XIR_BAD_STRUCTURE;
    if(memcmp(a->uri,b->uri,a->uri_length))return XR_XIR_BAD_STRUCTURE;
    return memcmp(a->text,b->text,a->length)?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
}
static XrXirStatus signature_target_equal(const XlspSourceLocation *a,
    const XlspSourceLocation *b) {
    if(a->version!=b->version||a->range.start.line!=b->range.start.line||a->range.start.character!=b->range.start.character||
        a->range.end.line!=b->range.end.line||a->range.end.character!=b->range.end.character)return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;;++i) {
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
        status=xir_compile_resource_status(xr_compile_session_poll(request->session));if(status!=XR_XIR_OK)goto done;
        bool contains=false;status=xlsp_source_snapshot_contains(request->snapshots[i],request->uri,request->uri_length,&contains);
        if(status!=XR_XIR_OK)goto done;if(!contains)continue;
        XlspSourceDocument input={0};
        status=xlsp_source_snapshot_document(request->snapshots[i],request->uri,request->uri_length,&input);
        if(status!=XR_XIR_OK)goto done;
        if(!selected) {
            document=input;
            XrParseStatus parsed=xlsp_source_syntax_build(request->session,&document,&syntax);
            if(parsed!=XR_PARSE_OK&&parsed!=XR_PARSE_RECOVERED) {
                switch(parsed) {
                case XR_PARSE_BUDGET: status=XR_XIR_BUDGET;break;
                case XR_PARSE_MEMORY_LIMIT: status=XR_XIR_MEMORY_LIMIT;break;
                case XR_PARSE_OVERFLOW: status=XR_XIR_OVERFLOW;break;
                case XR_PARSE_CANCELLED: status=XR_XIR_CANCELLED;break;
                case XR_PARSE_DEADLINE: status=XR_XIR_DEADLINE;break;
                case XR_PARSE_CLOCK_FAILURE: status=XR_XIR_CLOCK_FAILURE;break;
                case XR_PARSE_OUT_OF_MEMORY: status=XR_XIR_OUT_OF_MEMORY;break;
                default: status=XR_XIR_BAD_STRUCTURE;break;
                }
                goto done;
            }
            status=xlsp_source_syntax_call(syntax,request->position,&selection.call);
            if(status==XR_XIR_UNRESOLVED){*out=xjson_new_null();status=*out?XR_XIR_OK:XR_XIR_OUT_OF_MEMORY;goto done;}
            if(status!=XR_XIR_OK)goto done;selected=true;
        } else {
            status=signature_document_equal(&document,&input);if(status!=XR_XIR_OK)goto done;
        }
        status=xlsp_source_signature(request->snapshots[i],&selection,&current);
        if(status!=XR_XIR_OK)goto done;
        XlspSourceLocation location={0};
        if(!current.target_native_identity) {
            status=xlsp_source_definition(request->snapshots[i],request->uri,request->uri_length,selection.call.callee.start,&location);
            if(status!=XR_XIR_OK)goto done;
        }
        if(accepted.label.text) {
            if(!current.target_native_identity) {
                status=signature_target_equal(&target,&location);if(status!=XR_XIR_OK)goto done;
            }
            status=signature_equal(&accepted,&current);if(status!=XR_XIR_OK)goto done;
            xlsp_source_signature_free(&current);
        } else {accepted=current;current=(XlspSourceSignature){0};target=location;}
    }
    if(!accepted.label.text){status=XR_XIR_UNRESOLVED;goto done;}
    status=xir_compile_resource_status(xr_compile_session_poll(request->session));
    if(status==XR_XIR_OK)status=signature_json(&accepted,out);
done:
    xlsp_source_signature_free(&current);xlsp_source_signature_free(&accepted);xlsp_source_syntax_free(syntax);
    return status;
}
