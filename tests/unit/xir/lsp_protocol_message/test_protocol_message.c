/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_protocol_message.h"
#include "base/xmalloc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "../lsp_open_json_owner.h"
static size_t id_calls,id_live,id_bytes,id_fail=SIZE_MAX;
static struct {void *pointer;size_t bytes;} id_blocks[8];
static void *id_malloc(size_t bytes) {
    if(id_calls++==id_fail)return NULL;
    void *out=xr_malloc(bytes);
    if(out){size_t i=0;while(i<8&&id_blocks[i].pointer)++i;CHECK(i<8);id_blocks[i].pointer=out;id_blocks[i].bytes=bytes;++id_live;id_bytes+=bytes;}
    return out;
}
static void id_free(void *pointer) {
    if(pointer){size_t i=0;while(i<8&&id_blocks[i].pointer!=pointer)++i;CHECK(i<8&&id_live&&id_bytes>=id_blocks[i].bytes);
        --id_live;id_bytes-=id_blocks[i].bytes;id_blocks[i].pointer=NULL;id_blocks[i].bytes=0;xr_free(pointer);}
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(n) id_malloc(n)
#define xr_free(p) id_free(p)
#include "app/lsp/xlsp_protocol_message.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
static const unsigned char identity_bytes[]={'a',0,'b',0xf0,0x9f,0x98,0x80};
static XrJsonValue *parsed(const char *text) {
    XrJsonValue *out=xjson_parse(text,strlen(text));CHECK(out);return out;
}
static void identity_facts(const XlspRequestId *id) {
    CHECK(id->kind==XLSP_ID_STRING&&id->string_length==sizeof(identity_bytes));
    CHECK(!memcmp(id->as.string,identity_bytes,sizeof(identity_bytes))&&id->as.string[sizeof(identity_bytes)]==0);
}
static XlspRequestId identity(void) {
    XrJsonValue *message=parsed("{\"id\":\"a\\u0000b\\ud83d\\ude00\"}");XlspRequestId out={0};
    CHECK(xlsp_request_id_parse(message,&out)==XLSP_PROTOCOL_OK);
    XrJsonValue *value=xjson_get(message,"id");memset(value->as.string,'?',value->string_len);xjson_free(message);
    identity_facts(&out);return out;
}
static void identity_cases(void) {
    const char *numbers[]={"-2147483648","2147483647","-1","0","1","1.0","1e0"};
    const int32_t expected[]={INT32_MIN,INT32_MAX,-1,0,1,1,1};
    for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) {
        char text[80];snprintf(text,sizeof(text),"{\"id\":%s}",numbers[i]);XrJsonValue *message=parsed(text);XlspRequestId id={0};
        CHECK(xlsp_request_id_parse(message,&id)==XLSP_PROTOCOL_OK&&id.kind==XLSP_ID_NUMBER&&id.as.number==expected[i]);
        xjson_free(message);XlspRequestId copy={0};CHECK(xlsp_request_id_clone(&id,&copy)==XLSP_PROTOCOL_OK);
        CHECK(xlsp_request_id_equals(&id,&copy));XrJsonValue *json=xlsp_request_id_to_json(&copy);
        CHECK(json&&json->is_integer&&json->as.integer==expected[i]);xjson_free(json);xlsp_request_id_free(&id);xlsp_request_id_free(&copy);
    }
    const char *invalid[]={"null","true","false","[]","{}","1.5","2147483648","-2147483649","9223372036854775807","1e400"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        char text[80];snprintf(text,sizeof(text),"{\"id\":%s}",invalid[i]);XrJsonValue *message=parsed(text);XlspRequestId id={0};size_t before=id_calls;
        CHECK(xlsp_request_id_parse(message,&id)==XLSP_PROTOCOL_BAD_ARGUMENT&&!id.kind&&!id.as.string&&id_calls==before);xjson_free(message);
    }
    XrJsonValue *message=parsed("{}"),*null_message=xjson_new_null();XlspRequestId empty={0};
    CHECK(xlsp_request_id_parse(message,&empty)==XLSP_PROTOCOL_OK&&!xlsp_request_id_valid(&empty));
    CHECK(xlsp_request_id_parse(null_message,&empty)==XLSP_PROTOCOL_BAD_ARGUMENT&&!xlsp_request_id_equals(&empty,&empty));
    xjson_free(message);xjson_free(null_message);
    message=parsed("{\"id\":\"\"}");CHECK(xlsp_request_id_parse(message,&empty)==XLSP_PROTOCOL_OK&&empty.kind==XLSP_ID_STRING&&!empty.string_length);
    xjson_free(message);xlsp_request_id_free(&empty);
    message=xjson_new_object();XrJsonValue *bad=xjson_new_string_n("\xff",1);CHECK(message&&bad);xjson_object_set_new(message,"id",bad);
    CHECK(xlsp_request_id_parse(message,&empty)==XLSP_PROTOCOL_BAD_ARGUMENT&&!empty.kind);xjson_free(message);
    message=parsed("{\"id\":\"a\\u0000b\\ud83d\\ude00\"}");id_calls=0;id_fail=0;
    CHECK(xlsp_request_id_parse(message,&empty)==XLSP_PROTOCOL_OUT_OF_MEMORY&&!empty.kind&&!id_live&&id_calls==1);
    id_fail=SIZE_MAX;xjson_free(message);
    XlspRequestId id=identity();identity_facts(&id);size_t before=id_calls;XlspRequestId occupied={.kind=XLSP_ID_NUMBER,.as.number=73};
    CHECK(xlsp_request_id_clone(&id,&occupied)==XLSP_PROTOCOL_BAD_ARGUMENT&&occupied.as.number==73&&id_calls==before);
    id_calls=0;id_fail=0;CHECK(xlsp_request_id_clone(&id,&empty)==XLSP_PROTOCOL_OUT_OF_MEMORY&&!empty.kind&&id_calls==1);id_fail=SIZE_MAX;
    CHECK(!strcmp(xlsp_request_id_debug(&id),"string(7 bytes)"));
    XlspRequestId prefix={.kind=XLSP_ID_STRING,.string_length=1,.as.string="a"};CHECK(!xlsp_request_id_equals(&id,&prefix));
    CHECK(xlsp_request_id_clone(&id,&empty)==XLSP_PROTOCOL_OK&&empty.as.string!=id.as.string&&xlsp_request_id_equals(&id,&empty));
    memset(id.as.string,'?',id.string_length);xlsp_request_id_free(&id);identity_facts(&empty);xlsp_request_id_free(&empty);
    CHECK(!id_live&&!id_bytes&&!lsp_json_live&&!lsp_json_bytes);
}
static void response_facts(XrJsonValue *response,bool error) {
    CHECK(response&&response->type==XR_JSON_OBJECT&&response->as.object.count==3);
    CHECK(!strcmp(xjson_get_string(response,"jsonrpc"),"2.0"));XrJsonValue *id=xjson_get(response,"id");
    CHECK(id&&id->type==XR_JSON_STRING&&id->string_len==sizeof(identity_bytes)&&!memcmp(id->as.string,identity_bytes,sizeof(identity_bytes)));
    if(error){CHECK(!xjson_get(response,"result"));XrJsonValue *value=xjson_get_object(response,"error");CHECK(value&&xjson_get_int(value,"code")==-32800&&!strcmp(xjson_get_string(value,"message"),"Request cancelled"));}
    else {CHECK(!xjson_get(response,"error"));CHECK(!strcmp(xjson_get_string(xjson_get(response,"result"),"value"),"retained result"));}
}
static XrJsonValue *response_result(void) {
    XrJsonValue *result=xjson_new_object();CHECK(result);xjson_object_set_new(result,"value",xjson_new_string("retained result"));
    CHECK(result->as.object.count==1);return result;
}
static void response_cases(bool error) {
    XlspRequestId id=identity();size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        XrJsonValue *input=error?NULL:response_result(),*saved=input,*out=NULL;size_t live=lsp_json_live,bytes=lsp_json_bytes;
        lsp_json_calls=0;lsp_json_fail=probe?probe-1:SIZE_MAX;lsp_json_injected=false;
        XlspProtocolStatus status=error?xlsp_protocol_error(&id,-32800,"Request cancelled",&out):xlsp_protocol_response(&id,&input,&out);
        if(!probe){CHECK(status==XLSP_PROTOCOL_OK&&out&&!input);sites=lsp_json_calls;CHECK(sites);response_facts(out,error);}
        else {
            CHECK(status==XLSP_PROTOCOL_OUT_OF_MEMORY&&lsp_json_injected&&!out&&input==saved&&live==lsp_json_live&&bytes==lsp_json_bytes);
            if(input)CHECK(!strcmp(xjson_get_string(input,"value"),"retained result"));
        }
        lsp_json_fail=SIZE_MAX;lsp_json_injected=false;xjson_free(out);xjson_free(input);CHECK(!lsp_json_live&&!lsp_json_bytes);
    }
    XrJsonValue *input=error?NULL:response_result(),*out=xjson_new_number(73),*saved=out;size_t before=lsp_json_calls;
    CHECK((error?xlsp_protocol_error(&id,-32800,"Request cancelled",&out):xlsp_protocol_response(&id,&input,&out))==XLSP_PROTOCOL_BAD_ARGUMENT&&out==saved&&before==lsp_json_calls);
    xjson_free(out);out=NULL;
    CHECK((error?xlsp_protocol_error(&id,-32800,"Request cancelled",&out):xlsp_protocol_response(&id,&input,&out))==XLSP_PROTOCOL_OK);
    memset(id.as.string,'?',id.string_length);xlsp_request_id_free(&id);CHECK(!id_live&&!id_bytes);response_facts(out,error);
    size_t length=0;char *encoded=xjson_stringify(out,&length);CHECK(encoded&&length);
    CHECK(strstr(encoded,"a\\u0000b"));lsp_json_free(encoded);xjson_free(out);CHECK(!lsp_json_live&&!lsp_json_bytes);
    printf("LSP exact id/error=%u, atomic real JSON FI=%zu, detached final-key failure, producer death, physical=0/0\n",(unsigned)error,sites);
}
int main(void) {
    identity_cases();response_cases(false);response_cases(true);
    XlspRequestId number={.kind=XLSP_ID_NUMBER,.as.number=0};XrJsonValue *result=NULL,*out=NULL;
    CHECK(xlsp_protocol_response(&number,&result,&out)==XLSP_PROTOCOL_OK&&xjson_get(out,"result")->type==XR_JSON_NULL);xjson_free(out);out=NULL;
    CHECK(xlsp_protocol_error(NULL,-32600,"Invalid Request",&out)==XLSP_PROTOCOL_OK&&xjson_get(out,"id")->type==XR_JSON_NULL);xjson_free(out);
    CHECK(!id_live&&!id_bytes&&!lsp_json_live&&!lsp_json_bytes);puts("LSP owned integer/string IDs: strict bounds, NUL/emoji, exact keys, parse/clone OOM PASS");return 0;
}
