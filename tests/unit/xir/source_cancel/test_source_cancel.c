/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "app/lsp/xlsp_source_request.h"
#include "toolchain/xcompiler_session.h"
#include "base/xwindows_utf8.h"
#include "os/os_thread.h"
#include "os/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"
/* Observe actual parser-session safe points, including the production poll. */
#define xr_compile_session_poll cancel_session_poll_impl
#include "toolchain/xcompiler_session.c"
#undef xr_compile_session_poll
static _Atomic bool token;
static bool active,barrier,cut_observed;
static size_t poll_calls,trigger;
static XrCompileResourceStatus cut_status;
static _Atomic bool entered,released;
XR_FUNC XrCompileResourceStatus xr_compile_session_poll(XrCompilerSession *session) {
    bool selected=active&&xr_compile_session_current_arena(session)!=NULL;
    bool cutting=selected&&++poll_calls==trigger;
    if(cutting){
        CHECK(barrier);atomic_store_explicit(&entered,true,memory_order_release);
        uint64_t deadline=xr_time_monotonic_ms()+5000;
        while(!atomic_load_explicit(&released,memory_order_acquire)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
    }
    XrCompileResourceStatus status=cancel_session_poll_impl(session);
    if(cutting){cut_status=status;cut_observed=true;}
    return status;
}
static const char root_text[]="import { value } from \"./lib\"\nexport fn result()->i64 { /* \xf0\x9f\x98\x80 */ return value() }\n";
static const char lib_text[]="/* \xf0\x9f\x98\x80 */ export fn value()->i64 { return 41 }\n";
typedef struct Observation {size_t polls;bool cancelled;XrXirStatus status;XrCompileResourceStatus poll_status;} Observation;
typedef struct Execution {XlspSourceRequest *request;const _Atomic bool *cancel;XlspSourceRequestResult result;XrXirStatus status;} Execution;
static void *execute(void *raw){Execution *e=raw;e->status=xlsp_source_request_run(e->request,e->cancel,&e->result);return NULL;}
static Observation run(char **argv,unsigned mode,size_t at,bool pre,bool watch) {
    const XrCompileResourceLimits bounds={XR_COMPILE_MEMORY_FINITE,UINT64_C(8)*1024*1024,true};
    XrCompileResources *r=NULL;CHECK(xr_compile_resources_new(&bounds,&r)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline=sdk_stats(r);
    XrXirCompileContext context={r,xr_xir_compile_default_limits()};char root[sizeof(root_text)],lib[sizeof(lib_text)];
    memcpy(root,root_text,sizeof(root));memcpy(lib,lib_text,sizeof(lib));
    XlspSourceDocument documents[]={{argv[1],root,strlen(argv[1]),sizeof(root)-1,1},{argv[2],lib,strlen(argv[2]),sizeof(lib)-1,7}};
    XlspSourceRequestInput input={&context,documents,2,0,{1,43},mode,false};XlspSourceRequest *request=NULL;
    CHECK(xlsp_source_request_new(&input,&request)==XR_XIR_OK);memset(root,'?',sizeof(root));memset(lib,'?',sizeof(lib));memset(documents,0,sizeof(documents));
    atomic_init(&token,pre);atomic_init(&entered,false);atomic_init(&released,false);
    poll_calls=0;trigger=at;barrier=at!=SIZE_MAX;cut_observed=false;cut_status=XR_COMPILE_RESOURCE_OK;active=true;
    Execution execution={.request=request,.cancel=watch?&token:NULL};xr_thread_t thread;
    CHECK(xr_thread_create(&thread,execute,&execution));
    if(barrier){uint64_t deadline=xr_time_monotonic_ms()+5000;
        while(!atomic_load_explicit(&entered,memory_order_acquire)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
        atomic_store_explicit(&token,true,memory_order_release);atomic_store_explicit(&released,true,memory_order_release);}
    CHECK(xr_thread_join(thread,NULL)==0&&execution.status==XR_XIR_OK);active=false;XlspSourceRequestResult result=execution.result;
    Observation observed={poll_calls,result.cancelled,result.status,cut_status};
    if(pre||barrier){CHECK(watch&&result.cancelled&&!result.json);if(barrier)CHECK(cut_observed);}
    else{if(result.status!=XR_XIR_OK)fprintf(stderr,"Source cancel baseline mode%u stage%u status%u\n",mode,result.stage,(unsigned)result.status);
        CHECK(result.status==XR_XIR_OK&&!result.stage&&!result.cancelled&&result.json);}
    XlspSourceRequestResult again={0};CHECK(xlsp_source_request_run(request,NULL,&again)==XR_XIR_BAD_STRUCTURE&&!again.json);
    xlsp_source_request_free(request);CHECK(sdk_stats(r).held_bytes==baseline.held_bytes);
    xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);
    if(result.json){if(mode==0)CHECK(!strcmp(xjson_get_string(result.json,"uri"),argv[2]));
        else if(mode==1||mode==2)CHECK(result.json->type==XR_JSON_ARRAY&&result.json->as.array.count==1);
        else CHECK(strstr(xjson_get_string(xjson_get_object(result.json,"contents"),"value"),"value"));}
    xjson_free(result.json);CHECK(!lsp_json_live&&!lsp_json_bytes);return observed;
}
static int command(int argc,char **argv) {
    CHECK(argc==3);(void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_ledger;(void)sdk_unlimited;
    bool interrupted=true;
    for(unsigned mode=0;mode<4;++mode){Observation baseline=run(argv,mode,SIZE_MAX,false,true);CHECK(baseline.polls>=3);
        Observation nil=run(argv,mode,SIZE_MAX,false,false);CHECK(nil.polls>=3&&!nil.cancelled);
        Observation before=run(argv,mode,SIZE_MAX,true,true);CHECK(!before.polls&&before.cancelled&&before.status==XR_XIR_OK);
        size_t points[]={1,baseline.polls/3};
        for(unsigned i=0;i<2;++i){Observation cut=run(argv,mode,points[i],false,true);
            bool stopped=cut.status==XR_XIR_CANCELLED&&cut.poll_status==XR_COMPILE_RESOURCE_CANCELLED;
            if(!stopped)fprintf(stderr,"Cancellation did not interrupt inside Source mode%u trigger%zu polls%zu baseline%zu status%u poll_status%u\n",mode,points[i],cut.polls,baseline.polls,(unsigned)cut.status,(unsigned)cut.poll_status);
            interrupted=interrupted&&stopped;
            printf("Source cancel mode%u trigger%zu actual_polls%zu/%zu status%u poll_status%u cancelled=%u held-restored physical0/0 JSON0/0\n",mode,points[i],cut.polls,baseline.polls,(unsigned)cut.status,(unsigned)cut.poll_status,cut.cancelled?1u:0u);
        }
    }
    return interrupted?0:1;
}
int wmain(int argc,wchar_t **wide){XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);int result=command(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;}
