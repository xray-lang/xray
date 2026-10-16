/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_parameter_json.c - Consistent parameter hints across Source roots
 */
#include "xlsp_source_inlay.h"
#include "../../shared/xr_param_mode.h"
#include <limits.h>
#include <string.h>
static XrXirStatus parameter_json_work(XrCompileResources *resources,size_t bytes) {
    XrCompileResourceStatus status=xr_compile_resources_work(resources,bytes);
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus parameter_json_text_equal(XrCompileResources *resources,XlspSourceHover a,XlspSourceHover b) {
    if(a.length!=b.length||!!a.text!=!!b.text)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=parameter_json_work(resources,a.length);if(status!=XR_XIR_OK)return status;
    return a.length&&memcmp(a.text,b.text,a.length)?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
}
static bool parameter_json_position_equal(XrLspPosition a,XrLspPosition b) {
    return a.line==b.line&&a.character==b.character;
}
static XrXirStatus parameter_json_equal(XrCompileResources *resources,const XlspSourceParameterHints *a,
    const XlspSourceParameterHints *b) {
    if(a->count!=b->count)return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;i<a->count;++i) {
        XrXirStatus status=parameter_json_work(resources,sizeof(*a->items));if(status!=XR_XIR_OK)return status;
        const XlspSourceParameterHint *left=&a->items[i],*right=&b->items[i];
        if(!parameter_json_position_equal(left->position,right->position)||left->mode!=right->mode||
            left->target_kind!=right->target_kind||left->target_native_identity!=right->target_native_identity||
            left->target_range.line!=right->target_range.line||
            left->target_range.column!=right->target_range.column||left->target_range.end_line!=right->target_range.end_line||
            left->target_range.end_column!=right->target_range.end_column||left->has_target_location!=right->has_target_location||
            memcmp(&left->target_fingerprint,&right->target_fingerprint,sizeof(XrFingerprint)))return XR_XIR_BAD_STRUCTURE;
        status=parameter_json_text_equal(resources,left->label,right->label);if(status!=XR_XIR_OK)return status;
        status=parameter_json_text_equal(resources,left->target_identity,right->target_identity);if(status!=XR_XIR_OK)return status;
        status=parameter_json_text_equal(resources,left->target_path,right->target_path);if(status!=XR_XIR_OK)return status;
        status=parameter_json_text_equal(resources,left->target_uri,right->target_uri);if(status!=XR_XIR_OK)return status;
        if(left->has_target_location&&(left->target_version!=right->target_version||
            !parameter_json_position_equal(left->target_selection.start,right->target_selection.start)||
            !parameter_json_position_equal(left->target_selection.end,right->target_selection.end)))return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static bool parameter_json_member(XrJsonValue *object,const char *key,XrJsonValue *value) {
    if(!value)return false;int before=object->as.object.count;xjson_object_set_new(object,key,value);
    if(object->as.object.count==before){xjson_free(value);return false;}
    if(object->as.object.count!=before+1)return false;
    const XrJsonMember *member=&object->as.object.members[before];
    return member->value==value&&member->key&&member->key_len==strlen(key)&&!memcmp(member->key,key,member->key_len);
}
static bool parameter_json_adopt(XrJsonValue *object,const char *key,XrJsonValue **value) {
    XrJsonValue *child=*value;*value=NULL;return parameter_json_member(object,key,child);
}
static XrXirStatus parameter_json_item(XrCompileResources *resources,const XlspSourceParameterHint *hint,XrJsonValue **out) {
    if(!xr_param_mode_is_valid((XrParamMode)hint->mode))return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=parameter_json_work(resources,hint->label.length);if(status!=XR_XIR_OK)return status;
    status=parameter_json_work(resources,sizeof(XrJsonValue)*7);if(status!=XR_XIR_OK)return status;
    XrJsonValue *item=xjson_new_object(),*position=xjson_new_object();status=XR_XIR_OUT_OF_MEMORY;
    if(!item||!position)goto done;
    if(!parameter_json_member(position,"line",xjson_new_number(hint->position.line))||
        !parameter_json_member(position,"character",xjson_new_number(hint->position.character))||
        !parameter_json_adopt(item,"position",&position)||
        !parameter_json_member(item,"label",xjson_new_string_n(hint->label.text,hint->label.length))||
        !parameter_json_member(item,"kind",xjson_new_number(2))||
        !parameter_json_member(item,"paddingRight",xjson_new_bool(true)))goto done;
    if(hint->mode!=XR_PARAM_READ) {
        const char *tooltip=hint->mode==XR_PARAM_REF?"parameter mode: ref":"parameter mode: move";
        status=parameter_json_work(resources,strlen(tooltip)+sizeof(XrJsonValue));if(status!=XR_XIR_OK)goto done;
        status=XR_XIR_OUT_OF_MEMORY;if(!parameter_json_member(item,"tooltip",xjson_new_string(tooltip)))goto done;
    }
    *out=item;item=NULL;status=XR_XIR_OK;
done:
    xjson_free(position);xjson_free(item);return status;
}
static XrXirStatus parameter_json_render(XrCompileResources *resources,const XlspSourceParameterHints *hints,XrJsonValue **out) {
    if(hints->count>INT_MAX)return XR_XIR_BUDGET;
    XrXirStatus status=parameter_json_work(resources,sizeof(XrJsonValue));if(status!=XR_XIR_OK)return status;
    XrJsonValue *result=xjson_new_array();if(!result)return XR_XIR_OUT_OF_MEMORY;
    for(size_t i=0;i<hints->count;++i) {
        XrJsonValue *item=NULL;status=parameter_json_item(resources,&hints->items[i],&item);if(status!=XR_XIR_OK)goto done;
        if(result->as.array.count==result->as.array.capacity&&result->as.array.capacity>INT_MAX/2) {
            xjson_free(item);status=XR_XIR_BUDGET;goto done;
        }
        int before=result->as.array.count;xjson_array_push(result,item);
        if(result->as.array.count!=before+1){xjson_free(item);status=XR_XIR_OUT_OF_MEMORY;goto done;}
    }
    *out=result;result=NULL;status=XR_XIR_OK;
done:
    xjson_free(result);return status;
}
XR_FUNC XrXirStatus xlsp_source_parameter_hints_many_json(const XlspSourceParameterHintRequest *request,XrJsonValue **out) {
    const XlspSourceCallArgumentsView *input=request?xlsp_source_call_arguments_view(request->arguments):NULL;
    if(!request||!request->resources||!input||input->resources!=request->resources||!request->snapshots||!request->count||
        request->count>SIZE_MAX/sizeof(*request->snapshots)||!out||*out)return XR_XIR_BAD_STRUCTURE;
    XlspSourceParameterHints accepted={0},current={0};bool selected=false;XrXirStatus status=XR_XIR_OK;
    for(size_t i=0;i<request->count;++i) {
        status=parameter_json_work(request->resources,sizeof(*request->snapshots));if(status!=XR_XIR_OK)goto done;
        if(!xlsp_source_snapshot_uses_resources(request->snapshots[i],request->resources)){status=XR_XIR_BAD_STRUCTURE;goto done;}
        bool contains=false;status=xlsp_source_snapshot_contains(request->snapshots[i],input->uri,input->uri_length,&contains);
        if(status!=XR_XIR_OK)goto done;if(!contains)continue;
        status=xlsp_source_parameter_hints(request->snapshots[i],request->arguments,request->range,&current);
        if(status!=XR_XIR_OK)goto done;
        if(selected) {
            status=parameter_json_equal(request->resources,&accepted,&current);if(status!=XR_XIR_OK)goto done;
            xlsp_source_parameter_hints_free(&current);
        } else {accepted=current;current=(XlspSourceParameterHints){0};selected=true;}
    }
    status=selected?parameter_json_render(request->resources,&accepted,out):XR_XIR_UNRESOLVED;
done:
    xlsp_source_parameter_hints_free(&current);xlsp_source_parameter_hints_free(&accepted);return status;
}
