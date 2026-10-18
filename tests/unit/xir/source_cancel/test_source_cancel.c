/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "app/lsp/xlsp_source_request.h"
#include "base/xwindows_utf8.h"
#include "os/os_thread.h"
#include "os/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#define xr_compile_resources_work cancel_work_impl
#include "xir_sdk_resource_test.h"
#undef xr_compile_resources_work
static _Atomic bool token;
static bool active;
static size_t work_calls,trigger;
static bool barrier;
static _Atomic bool entered,released;
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *r,uint64_t units) {
    if(active&&++work_calls==trigger){
        CHECK(barrier);atomic_store_explicit(&entered,true,memory_order_release);
        uint64_t deadline=xr_time_monotonic_ms()+5000;
        while(!atomic_load_explicit(&released,memory_order_acquire)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
    }
    return cancel_work_impl(r,units);
}
#include "lsp_open_json_owner.h"
static const char root_text[]="import { value } from \"./lib\"\nexport fn result()->i64 { /* \xf0\x9f\x98\x80 */ return value() }\n";
static const char lib_text[]="/* \xf0\x9f\x98\x80 */ export fn value()->i64 { return 41 }\n";
typedef struct Observation {size_t calls;uint64_t work;bool cancelled;} Observation;
static bool no_cancel(const void *unused){(void)unused;return false;}
typedef struct Execution {XlspSourceRequest *request;const _Atomic bool *cancel;XlspSourceRequestResult result;XrXirStatus status;} Execution;
static void *execute(void *raw){Execution *e=raw;e->status=xlsp_source_request_run(e->request,e->cancel,&e->result);return NULL;}
static Observation run(char **argv,unsigned mode,size_t at,bool pre,bool watch) {
    XrCompileResourceLimits bounds={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrCompileResources *r=NULL;CHECK(xr_compile_resources_new(&bounds,&r)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={r,xr_xir_compile_default_limits()};char root[sizeof(root_text)],lib[sizeof(lib_text)];
    memcpy(root,root_text,sizeof(root));memcpy(lib,lib_text,sizeof(lib));
    XlspSourceDocument documents[]={{argv[1],root,strlen(argv[1]),sizeof(root)-1,1},{argv[2],lib,strlen(argv[2]),sizeof(lib)-1,7}};
    XlspSourceRequestInput input={&context,documents,2,0,{1,43},mode,false};XlspSourceRequest *request=NULL;
    CHECK(xlsp_source_request_new(&input,&request)==XR_XIR_OK);memset(root,'?',sizeof(root));memset(lib,'?',sizeof(lib));memset(documents,0,sizeof(documents));
    atomic_init(&token,pre);atomic_init(&entered,false);atomic_init(&released,false);work_calls=0;trigger=at;barrier=at!=SIZE_MAX;active=true;
    Execution execution={.request=request,.cancel=watch?&token:NULL};xr_thread_t thread;
    CHECK(xr_thread_create(&thread,execute,&execution));
    if(barrier){uint64_t deadline=xr_time_monotonic_ms()+5000;
        while(!atomic_load_explicit(&entered,memory_order_acquire)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
        atomic_store_explicit(&token,true,memory_order_release);atomic_store_explicit(&released,true,memory_order_release);}
    CHECK(xr_thread_join(thread,NULL)==0&&execution.status==XR_XIR_OK);active=false;XlspSourceRequestResult result=execution.result;
    Observation observed={work_calls,sdk_stats(r).work,result.cancelled};
    if(pre||at!=SIZE_MAX){CHECK(watch&&result.cancelled&&!result.json);}
    else{if(result.status!=XR_XIR_OK)fprintf(stderr,"Source cancel baseline mode%u stage%u status%u\n",mode,result.stage,(unsigned)result.status);
        CHECK(result.status==XR_XIR_OK&&!result.stage&&!result.cancelled&&result.json);}
    CHECK(xr_compile_resources_begin_cancel(r,no_cancel,NULL)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_end_cancel(r,no_cancel,NULL)==XR_COMPILE_RESOURCE_OK);
    XlspSourceRequestResult again={0};CHECK(xlsp_source_request_run(request,NULL,&again)==XR_XIR_BAD_STRUCTURE&&!again.json);
    xr_compile_resources_release(r);xlsp_source_request_free(request);CHECK(!runtime_live&&!runtime_bytes);
    if(result.json){if(mode==0)CHECK(!strcmp(xjson_get_string(result.json,"uri"),argv[2]));
        else if(mode==1||mode==2)CHECK(result.json->type==XR_JSON_ARRAY&&result.json->as.array.count==1);
        else CHECK(strstr(xjson_get_string(xjson_get_object(result.json,"contents"),"value"),"value"));}
    xjson_free(result.json);CHECK(!lsp_json_live&&!lsp_json_bytes);return observed;
}
static int command(int argc,char **argv) {
    CHECK(argc==3);(void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_ledger;(void)sdk_unlimited;
    bool reduced=true;
    for(unsigned mode=0;mode<4;++mode){Observation baseline=run(argv,mode,SIZE_MAX,false,true);CHECK(baseline.calls>256);
        Observation nil=run(argv,mode,SIZE_MAX,false,false);CHECK(nil.calls==baseline.calls&&nil.work==baseline.work);
        Observation before=run(argv,mode,SIZE_MAX,true,true);CHECK(!before.calls&&before.cancelled);
        size_t points[]={1,baseline.calls/3};
        for(unsigned i=0;i<2;++i){Observation cut=run(argv,mode,points[i],false,true);
            bool short_run=cut.calls<=points[i]+64&&cut.work<baseline.work;
            if(!short_run)fprintf(stderr,"Cancellation did not interrupt inside Source mode%u trigger%zu calls%zu baseline%zu work%llu baseline_work%llu\n",mode,points[i],cut.calls,baseline.calls,(unsigned long long)cut.work,(unsigned long long)baseline.work);
            reduced=reduced&&short_run;
            printf("Source cancel mode%u trigger%zu actual_calls%zu/%zu work%llu/%llu cancelled=%u physical0/0 JSON0/0\n",mode,points[i],cut.calls,baseline.calls,(unsigned long long)cut.work,(unsigned long long)baseline.work,cut.cancelled?1u:0u);
        }
    }
    return reduced?0:1;
}
int wmain(int argc,wchar_t **wide){XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);int result=command(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;}
