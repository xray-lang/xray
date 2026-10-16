/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_protocol_envelope.inc.c - Unambiguous incoming JSON-RPC envelopes
 *
 * KEY CONCEPT:
 *   Validate the complete parsed value before owning an id or dispatching a call.
 */
#include <math.h>
typedef struct EnvelopeFields {const XrJsonValue *values[6];unsigned seen,duplicates;} EnvelopeFields;
static bool envelope_fields(const XrJsonValue *object,const char *const *keys,unsigned count,EnvelopeFields *out) {
    if(!object||object->type!=XR_JSON_OBJECT||object->as.object.count<0||
        object->as.object.capacity<object->as.object.count||(object->as.object.count&&!object->as.object.members))return false;
    for(int i=0;i<object->as.object.count;++i) {
        const XrJsonMember *member=&object->as.object.members[i];
        if(!member->key||!member->value||member->key_len==SIZE_MAX||!xr_utf8_validate(member->key,member->key_len))return false;
        for(unsigned k=0;k<count;++k) {
            size_t length=strlen(keys[k]);if(length!=member->key_len||memcmp(keys[k],member->key,length))continue;
            unsigned bit=1u<<k;if(out->seen&bit)out->duplicates|=bit;else out->values[k]=member->value;
            out->seen|=bit;break;
        }
    }
    return true;
}
static bool envelope_string(const XrJsonValue *value,bool c_name) {
    return value&&value->type==XR_JSON_STRING&&value->as.string&&value->string_len!=SIZE_MAX&&
        xr_utf8_validate(value->as.string,value->string_len)&&(!c_name||!memchr(value->as.string,0,value->string_len));
}
static bool envelope_integer(const XrJsonValue *value) {
    if(!value||value->type!=XR_JSON_NUMBER)return false;
    if(value->is_integer)return value->as.integer>=INT32_MIN&&value->as.integer<=INT32_MAX;
    double input=value->as.number;
    return input>=INT32_MIN&&input<=INT32_MAX&&(double)(int32_t)input==input;
}
static bool envelope_id(const XrJsonValue *value,bool allow_null) {
    return value&&((allow_null&&value->type==XR_JSON_NULL)||envelope_integer(value)||envelope_string(value,false));
}
static bool envelope_version(const XrJsonValue *value) {
    return envelope_string(value,false)&&value->string_len==3&&!memcmp(value->as.string,"2.0",3);
}
typedef struct EnvelopeFrame {const XrJsonValue *value;size_t next;} EnvelopeFrame;
/* Match the JSON parser's node-depth bound, including a scalar leaf. A cycle
 * cannot expand beyond that finite active path. No extra heap or compiler owner
 * is introduced at this transport boundary. */
static bool envelope_tree(const XrJsonValue *root) {
    EnvelopeFrame stack[XJSON_MAX_DEPTH];size_t count=1;stack[0]=(EnvelopeFrame){root,0};
    while(count) {
        EnvelopeFrame *frame=&stack[count-1];const XrJsonValue *value=frame->value;
        if(!value||value->type<XR_JSON_NULL||value->type>XR_JSON_OBJECT)return false;
        if(value->type==XR_JSON_STRING) {if(!envelope_string(value,false))return false;--count;continue;}
        if(value->type==XR_JSON_NUMBER) {if(!value->is_integer&&!isfinite(value->as.number))return false;--count;continue;}
        if(value->type!=XR_JSON_ARRAY&&value->type!=XR_JSON_OBJECT){--count;continue;}
        int size=value->type==XR_JSON_ARRAY?value->as.array.count:value->as.object.count;
        int capacity=value->type==XR_JSON_ARRAY?value->as.array.capacity:value->as.object.capacity;
        if(size<0||capacity<size)return false;
        if(value->type==XR_JSON_ARRAY&&size&&!value->as.array.items)return false;
        if(value->type==XR_JSON_OBJECT&&size&&!value->as.object.members)return false;
        if(frame->next==(size_t)size){--count;continue;}
        const XrJsonValue *child=NULL;
        if(value->type==XR_JSON_ARRAY)child=value->as.array.items[frame->next++];
        else {
            const XrJsonMember *member=&value->as.object.members[frame->next++];
            if(!member->key||member->key_len==SIZE_MAX||!xr_utf8_validate(member->key,member->key_len))return false;
            child=member->value;
        }
        if(count==XJSON_MAX_DEPTH)return false;stack[count++]=(EnvelopeFrame){child,0};
    }
    return true;
}
static XlspProtocolStatus envelope_failure(const EnvelopeFields *fields) {
    if(!(fields->seen&2u)&&(fields->seen&48u))return XLSP_PROTOCOL_INVALID_RESPONSE;
    if(!(fields->seen&4u)&&!(fields->duplicates&3u)&&envelope_version(fields->values[0])&&
        envelope_string(fields->values[1],true))return XLSP_PROTOCOL_INVALID_NOTIFICATION;
    return XLSP_PROTOCOL_BAD_ARGUMENT;
}
static bool envelope_empty(const XlspProtocolEnvelope *out) {
    return out&&out->kind==XLSP_ENVELOPE_NONE&&id_empty(&out->id)&&!out->method&&!out->method_length&&
        !out->params&&!out->response_id&&!out->result&&!out->error;
}
XR_FUNC void xlsp_protocol_envelope_clear(XlspProtocolEnvelope *envelope) {
    if(!envelope)return;xlsp_request_id_free(&envelope->id);*envelope=(XlspProtocolEnvelope){0};
}
XR_FUNC XlspProtocolStatus xlsp_protocol_envelope_decode(XrJsonValue *input,XlspProtocolEnvelope *out) {
    if(!envelope_empty(out))return XLSP_PROTOCOL_BAD_ARGUMENT;
    static const char *const keys[]={"jsonrpc","method","id","params","result","error"};EnvelopeFields fields={0};
    if(!envelope_fields(input,keys,6,&fields))return XLSP_PROTOCOL_BAD_ARGUMENT;
    XlspProtocolStatus failure=envelope_failure(&fields);
    if(fields.duplicates||!envelope_tree(input)||!envelope_version(fields.values[0]))return failure;
    XlspProtocolEnvelope result={0};
    if(fields.seen&2u) {
        if(!envelope_string(fields.values[1],true)||(fields.seen&48u)||
            ((fields.seen&8u)&&fields.values[3]->type!=XR_JSON_OBJECT&&fields.values[3]->type!=XR_JSON_ARRAY)||
            ((fields.seen&4u)&&!envelope_id(fields.values[2],false)))return failure;
        XlspProtocolStatus status=xlsp_request_id_parse(input,&result.id);if(status!=XLSP_PROTOCOL_OK)return status;
        result.kind=fields.seen&4u?XLSP_ENVELOPE_REQUEST:XLSP_ENVELOPE_NOTIFICATION;
        result.method=fields.values[1]->as.string;result.method_length=fields.values[1]->string_len;
        result.params=(XrJsonValue *)fields.values[3];
    } else {
        if(!(fields.seen&4u)||(fields.seen&8u)||!envelope_id(fields.values[2],true)||
            ((fields.seen&48u)!=16u&&(fields.seen&48u)!=32u))return failure;
        if(fields.seen&32u) {
            static const char *const error_keys[]={"code","message","data"};EnvelopeFields error={0};
            if(!envelope_fields(fields.values[5],error_keys,3,&error)||error.duplicates||
                !envelope_integer(error.values[0])||!envelope_string(error.values[1],false))return failure;
        }
        result.kind=XLSP_ENVELOPE_RESPONSE;result.response_id=fields.values[2];result.result=fields.values[4];result.error=fields.values[5];
    }
    *out=result;return XLSP_PROTOCOL_OK;
}
