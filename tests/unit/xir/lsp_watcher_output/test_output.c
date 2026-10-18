/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_protocol_output.h"
#include "app/lsp/xlsp_protocol_message.h"
#include "app/lsp/xlsp_watcher_registration.h"
#include "app/lsp/xlsp_framing.h"
#include "app/lsp/xlsp_index_json.h"
#include "base/xmalloc.h"
#include "os/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}} while(0)
static void native_free(void *p){xr_free(p);}
#include "lsp_open_json_owner.h"
#undef xr_free
#define xr_free lsp_json_free
#include "app/lsp/xlsp_protocol_output.c"
static const char *status_name(XrXirStatus s){
    return s==XR_XIR_OK?"OK":s==XR_XIR_OUT_OF_MEMORY?"OOM":s==XR_XIR_BUDGET?"BUDGET":s==XR_XIR_IO?"IO":s==XR_XIR_BAD_STRUCTURE?"BAD_STRUCTURE":"UNEXPECTED";
}
static XrJsonValue *ordinary(void){
    XrJsonValue *object=xjson_new_object();if(!object)return NULL;
    char text[1024];for(size_t i=0;i<sizeof(text);++i)text[i]=(char)(i%5==0?0:i%5==1?'"':i%5==2?'\\':i%5==3?'\t':'x');
    if(!index_json_member(object,"echo",xjson_new_string_n(text,sizeof(text)))){xjson_free(object);return NULL;}return object;
}
static XrJsonValue *oversize(void){
    size_t length=(size_t)XLSP_FRAME_MAX_BODY/6+64;char *input=xr_malloc(length);CHECK(input);memset(input,1,length);
    XrJsonValue *value=xjson_new_string_n(input,length);native_free(input);
    if(!value)return NULL;
    XrJsonValue *object=xjson_new_object();if(!object){xjson_free(value);return NULL;}
    if(!index_json_member(object,"large",value)){xjson_free(object);return NULL;}return object;
}
static void acknowledge(XrLspTransport *t){
    fprintf(stderr,"READY\n");fflush(stderr);
    for(unsigned wait=0;;++wait){size_t n=0;bool blocked=false;char *body=xlsp_transport_try_read(t,&n,&blocked);
        if(body){CHECK(n==2&&!memcmp(body,"{}",2));native_free(body);return;}
        CHECK(t->connected&&blocked&&wait<1000);xr_time_sleep_ms(1);}
}
int main(int argc,char **argv){
    CHECK(argc>=2);unsigned mode=(unsigned)strtoul(argv[1],NULL,10);CHECK(mode<=21);
    XrLspTransport *transport=xlsp_transport_stdio();CHECK(transport);XrLspServer server={0};server.transport=transport;
    if(mode==1||mode==2)server.next_request_id=40;
    if(mode==8)server.next_request_id=INT32_MAX;
    if(mode==9)server.next_request_id=INT32_MAX-1;
    if(mode==17)server.next_request_id=INT32_MAX;
    if(mode==20)server.next_request_id=INT32_MIN;
    if(mode==21)server.next_request_id=-1;
    if(mode==10)transport->connected=false;
    if(mode==11)server.exit_received=true;
    if(mode==12)acknowledge(transport);
    XlspRequestId response_id={0};
    if(mode==3){const char source[]="{\"id\":\"owned\\u0000λ\"}";XrJsonValue *input=xjson_parse(source,sizeof(source)-1);CHECK(input);
        CHECK(xlsp_request_id_parse(input,&response_id)==XLSP_PROTOCOL_OK);xjson_free(input);}
    XrLspServer before=server;lsp_json_calls=0;lsp_json_injected=false;lsp_json_fail=argc>2?(size_t)strtoull(argv[2],NULL,10):SIZE_MAX;
    XrXirStatus status=XR_XIR_OK;XrJsonValue *value=NULL;unsigned sent=0;
    if(mode==1)value=ordinary();else if(mode==6)value=xjson_new_number(1);else if(mode==14)value=oversize();
    else if(mode==15)value=xjson_new_array();else if(mode!=5&&mode!=4&&mode!=19)value=xlsp_watcher_registration_params();
    if(!value&&mode!=5&&mode!=4&&mode!=19)status=XR_XIR_OUT_OF_MEMORY;
    if(status==XR_XIR_OK){
        if(mode==2||mode==17)status=xlsp_protocol_send_notification(&server,"fixture/notify",value);
        else if(mode==3){XrJsonValue *message=NULL;XlspProtocolStatus built=xlsp_protocol_response(&response_id,&value,&message);xjson_free(value);
            status=built==XLSP_PROTOCOL_OK?xlsp_protocol_write_owned(&server,message):built==XLSP_PROTOCOL_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;}
        else if(mode==4){xjson_free(value);XrJsonValue *message=NULL;XlspProtocolStatus built=xlsp_protocol_error(NULL,-32603,"owned λ error",&message);
            status=built==XLSP_PROTOCOL_OK?xlsp_protocol_write_owned(&server,message):built==XLSP_PROTOCOL_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;}
        else if(mode==15||mode==19)status=xlsp_protocol_write_owned(&server,value);
        else status=xlsp_protocol_send_request(mode==16?NULL:&server,mode==18?NULL:mode==7?"":mode==1?"fixture/echo":mode==14?"fixture/oversize":"client/registerCapability",value);
        if(status==XR_XIR_OK&&mode!=2&&mode!=17&&mode!=3&&mode!=4&&mode!=15&&mode!=19)++sent;
    }
    if(mode==9&&status==XR_XIR_OK)CHECK(xlsp_protocol_send_request(&server,"exhausted",NULL)==XR_XIR_BUDGET);
    if(mode==13&&status==XR_XIR_OK){value=xlsp_watcher_registration_params();status=value?xlsp_protocol_send_request(&server,"client/registerCapability",value):XR_XIR_OUT_OF_MEMORY;if(status==XR_XIR_OK)++sent;}
    XrLspServer expected=before;expected.next_request_id=before.next_request_id+(int32_t)sent;
    if(status==XR_XIR_IO)expected.exit_received=true;
    CHECK(!memcmp(&expected,&server,sizeof(server)));
    if(lsp_json_injected)CHECK(status==XR_XIR_OUT_OF_MEMORY);
    xlsp_request_id_free(&response_id);xlsp_transport_free(transport);CHECK(!lsp_json_live&&!lsp_json_bytes);
    fprintf(stderr,"STATS mode=%u status=%s attempts=%zu injected=%u before=%ld after=%ld exit=%u sent=%u live=%zu bytes=%zu\n",mode,status_name(status),lsp_json_calls,lsp_json_injected?1u:0u,(long)before.next_request_id,(long)server.next_request_id,server.exit_received?1u:0u,sent,lsp_json_live,lsp_json_bytes);
    return 0;
}
