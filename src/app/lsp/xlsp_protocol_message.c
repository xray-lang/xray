/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_protocol_message.c - Exact owned ids and complete response publication
 */
#include "xlsp_protocol_message.h"
#include "../../base/xmalloc.h"
#include "../../base/xutf8.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
static bool id_empty(const XlspRequestId *id) {
    return id&&id->kind==XLSP_ID_NONE&&!id->string_length&&!id->as.string;
}
XR_FUNC bool xlsp_request_id_valid(const XlspRequestId *id) {
    if(!id)return false;
    if(id->kind==XLSP_ID_NUMBER)return id->string_length==0;
    return id->kind==XLSP_ID_STRING&&id->as.string&&id->string_length!=SIZE_MAX&&
        xr_utf8_validate(id->as.string,id->string_length);
}
static XlspProtocolStatus id_string(const char *text,size_t length,XlspRequestId *out) {
    if(!text||length==SIZE_MAX||!xr_utf8_validate(text,length))return XLSP_PROTOCOL_BAD_ARGUMENT;
    char *copy=xr_malloc(length+1);if(!copy)return XLSP_PROTOCOL_OUT_OF_MEMORY;
    memcpy(copy,text,length);copy[length]=0;
    *out=(XlspRequestId){.kind=XLSP_ID_STRING,.string_length=length,.as.string=copy};return XLSP_PROTOCOL_OK;
}
XR_FUNC XlspProtocolStatus xlsp_request_id_parse(const XrJsonValue *message,XlspRequestId *out) {
    if(!message||message->type!=XR_JSON_OBJECT||!id_empty(out))return XLSP_PROTOCOL_BAD_ARGUMENT;
    const XrJsonValue *value=xjson_get((XrJsonValue *)message,"id");if(!value)return XLSP_PROTOCOL_OK;
    if(value->type==XR_JSON_STRING)return id_string(value->as.string,value->string_len,out);
    if(value->type!=XR_JSON_NUMBER)return XLSP_PROTOCOL_BAD_ARGUMENT;
    int32_t number;
    if(value->is_integer) {
        if(value->as.integer<INT32_MIN||value->as.integer>INT32_MAX)return XLSP_PROTOCOL_BAD_ARGUMENT;
        number=(int32_t)value->as.integer;
    } else {
        double input=value->as.number;
        // This comparison also rejects NaN before the narrowing conversion.
        if(!(input>=INT32_MIN&&input<=INT32_MAX))return XLSP_PROTOCOL_BAD_ARGUMENT;
        number=(int32_t)input;if((double)number!=input)return XLSP_PROTOCOL_BAD_ARGUMENT;
    }
    *out=(XlspRequestId){.kind=XLSP_ID_NUMBER,.as.number=number};return XLSP_PROTOCOL_OK;
}
XR_FUNC XlspProtocolStatus xlsp_request_id_clone(const XlspRequestId *id,XlspRequestId *out) {
    if(!xlsp_request_id_valid(id)||!id_empty(out))return XLSP_PROTOCOL_BAD_ARGUMENT;
    if(id->kind==XLSP_ID_STRING)return id_string(id->as.string,id->string_length,out);
    *out=*id;return XLSP_PROTOCOL_OK;
}
XR_FUNC void xlsp_request_id_free(XlspRequestId *id) {
    if(!id)return;
    if(id->kind==XLSP_ID_STRING)xr_free(id->as.string);
    *id=(XlspRequestId){0};
}
XR_FUNC bool xlsp_request_id_equals(const XlspRequestId *left,const XlspRequestId *right) {
    if(!xlsp_request_id_valid(left)||!xlsp_request_id_valid(right)||left->kind!=right->kind)return false;
    if(left->kind==XLSP_ID_NUMBER)return left->as.number==right->as.number;
    return left->string_length==right->string_length&&!memcmp(left->as.string,right->as.string,left->string_length);
}
XR_FUNC XrJsonValue *xlsp_request_id_to_json(const XlspRequestId *id) {
    if(!id||id_empty(id))return xjson_new_null();
    if(!xlsp_request_id_valid(id))return NULL;
    if(id->kind==XLSP_ID_STRING)return xjson_new_string_n(id->as.string,id->string_length);
    XrJsonValue *value=xjson_new_number(0);if(value){value->is_integer=true;value->as.integer=id->as.number;}
    return value;
}
XR_FUNC const char *xlsp_request_id_debug(const XlspRequestId *id) {
    static _Thread_local char buffer[64];
    if(!id||id_empty(id))return "<notification>";
    if(!xlsp_request_id_valid(id))return "<invalid>";
    if(id->kind==XLSP_ID_NUMBER)snprintf(buffer,sizeof(buffer),"%ld",(long)id->as.number);
    else snprintf(buffer,sizeof(buffer),"string(%zu bytes)",id->string_length);
    return buffer;
}
/* The JSON builder can append the child before key allocation fails. Detach
 * that child so the caller retains it while the partial envelope is freed. */
static bool message_take(XrJsonValue *object,const char *key,XrJsonValue **child) {
    if(!*child)return false;
    int before=object->as.object.count;xjson_object_set_new(object,key,*child);
    if(object->as.object.count==before)return false;
    XrJsonMember *member=&object->as.object.members[before];
    if(!member->key){member->value=NULL;return false;}
    *child=NULL;return true;
}
static bool message_put(XrJsonValue *object,const char *key,XrJsonValue *child) {
    bool attached=message_take(object,key,&child);xjson_free(child);return attached;
}
static XrJsonValue *message_header(const XlspRequestId *id) {
    XrJsonValue *object=xjson_new_object();if(!object)return NULL;
    if(!message_put(object,"jsonrpc",xjson_new_string("2.0"))||
        !message_put(object,"id",xlsp_request_id_to_json(id))){xjson_free(object);return NULL;}
    return object;
}
XR_FUNC XlspProtocolStatus xlsp_protocol_response(const XlspRequestId *id,XrJsonValue **result,XrJsonValue **out) {
    if(!xlsp_request_id_valid(id)||!result||!out||*out||result==out)return XLSP_PROTOCOL_BAD_ARGUMENT;
    XrJsonValue *object=message_header(id);if(!object)return XLSP_PROTOCOL_OUT_OF_MEMORY;
    XrJsonValue *created=*result?NULL:xjson_new_null(),*child=*result?*result:created;
    if(!message_take(object,"result",&child)) {
        xjson_free(created);xjson_free(object);return XLSP_PROTOCOL_OUT_OF_MEMORY;
    }
    *result=NULL;*out=object;return XLSP_PROTOCOL_OK;
}
XR_FUNC XlspProtocolStatus xlsp_protocol_error(const XlspRequestId *id,int32_t code,const char *message,XrJsonValue **out) {
    if(!message||!out||*out||(id&&!id_empty(id)&&!xlsp_request_id_valid(id)))return XLSP_PROTOCOL_BAD_ARGUMENT;
    XrJsonValue *object=message_header(id);if(!object)return XLSP_PROTOCOL_OUT_OF_MEMORY;
    XrJsonValue *error=xjson_new_object();if(!error){xjson_free(object);return XLSP_PROTOCOL_OUT_OF_MEMORY;}
    if(!message_put(error,"code",xjson_new_number(code))||!message_put(error,"message",xjson_new_string(message))) {
        xjson_free(error);xjson_free(object);return XLSP_PROTOCOL_OUT_OF_MEMORY;
    }
    if(!message_put(object,"error",error)){xjson_free(object);return XLSP_PROTOCOL_OUT_OF_MEMORY;}
    *out=object;return XLSP_PROTOCOL_OK;
}
XR_FUNC XlspProtocolStatus xlsp_protocol_call(const XlspRequestId *id,const char *method,XrJsonValue **params,XrJsonValue **out) {
    if(!method||!*method||!xr_utf8_validate(method,strlen(method))||!params||!out||*out||params==out||
       (id&&!id_empty(id)&&!xlsp_request_id_valid(id))||
       (*params&&(*params)->type!=XR_JSON_OBJECT&&(*params)->type!=XR_JSON_ARRAY))return XLSP_PROTOCOL_BAD_ARGUMENT;
    XrJsonValue *object=xjson_new_object();if(!object)return XLSP_PROTOCOL_OUT_OF_MEMORY;
    if(!message_put(object,"jsonrpc",xjson_new_string("2.0"))||
       ((id&&!id_empty(id))&&!message_put(object,"id",xlsp_request_id_to_json(id)))||
       !message_put(object,"method",xjson_new_string(method))||
       (*params&&!message_take(object,"params",params))) {
        xjson_free(object);return XLSP_PROTOCOL_OUT_OF_MEMORY;
    }
    *out=object;return XLSP_PROTOCOL_OK;
}
