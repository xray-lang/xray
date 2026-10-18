/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef LSP_SOURCE_ASYNC_CASES_H
#define LSP_SOURCE_ASYNC_CASES_H
#include "app/lsp/xlsp_async.h"
typedef struct SourceAsyncCase {
    XrLspTask *task;
    XlspSourceRequest *request;
    XlspSourceRequestResult result;
    _Atomic bool started,release;
    bool blocker,cancelled;
    unsigned executions,completions;
} SourceAsyncCase;
static void source_async_execute(void *data) {
    SourceAsyncCase *owner=data;++owner->executions;
    CHECK(owner->task->data==owner&&atomic_load(&owner->task->state)==XLSP_TASK_RUNNING);
    atomic_store(&owner->started,true);
    if(owner->blocker)while(!atomic_load(&owner->release)&&!xlsp_task_is_cancelled(owner->task))xr_thread_yield();
    if(owner->request)CHECK(xlsp_source_request_run(owner->request,&owner->task->cancel_requested,&owner->result)==XR_XIR_OK);
    owner->task->result=&owner->result;
}
static void source_async_complete(void *data,void *result) {
    SourceAsyncCase *owner=data;CHECK(!owner->completions);++owner->completions;
    CHECK(owner->cancelled?result==NULL:result==&owner->result);
    if(!owner->cancelled)CHECK(owner->result.status==XR_XIR_OK&&!owner->result.stage&&!owner->result.cancelled);
    xlsp_source_request_free(owner->request);owner->request=NULL;
}
static void source_async_submit(XrLspAsync *async,SourceAsyncCase *owner) {
    atomic_init(&owner->started,false);atomic_init(&owner->release,false);
    owner->task=xlsp_task_new(source_async_execute,source_async_complete,owner,XLSP_TASK_NORMAL);CHECK(owner->task);
    CHECK(xlsp_async_submit(async,owner->task));
}
static void source_async_wait(XrLspAsync *async,SourceAsyncCase *owner,bool started) {
    for(unsigned i=0;i<2000;++i) {
        if(started?atomic_load(&owner->started):owner->completions!=0)return;
        if(!started)xlsp_async_poll(async);xr_thread_sleep_ms(1);
    }
    CHECK(false);
}
static XlspSourceRequest *source_async_capture(const char *root_uri,const char *lib_uri,unsigned mode) {
    XrCompileResourceLimits limits=xr_source_compile_default_resource_limits();XrCompileResources *resources=sdk_ledger(&limits);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    char root[sizeof(open_root)],lib[sizeof(open_library)];memcpy(root,open_root,sizeof(root));memcpy(lib,open_library,sizeof(lib));
    char *first=xr_strdup(root_uri),*second=xr_strdup(lib_uri);CHECK(first&&second);
    XlspSourceDocument documents[2]={{first,root,strlen(first),sizeof(root)-1,1},{second,lib,strlen(second),sizeof(lib)-1,7}};
    XlspSourceRequestInput input={&context,documents,2,0,{1,43},mode,false};XlspSourceRequest *request=NULL;
    CHECK(xlsp_source_request_new(&input,&request)==XR_XIR_OK);
    memset(root,'?',sizeof(root));memset(lib,'?',sizeof(lib));memset(first,'?',strlen(first));memset(second,'?',strlen(second));
    xr_free(first);xr_free(second);memset(documents,0,sizeof(documents));memset(&input,0,sizeof(input));memset(&context,0,sizeof(context));
    xr_compile_resources_release(resources);return request;
}
static void source_async_cases(const char *root_uri,const char *lib_uri) {
    for(unsigned mode=0;mode<4;++mode) {
        XrLspAsync *async=xlsp_async_new(NULL,NULL);CHECK(async);
        SourceAsyncCase owner={0};owner.request=source_async_capture(root_uri,lib_uri,mode);
        source_async_submit(async,&owner);source_async_wait(async,&owner,false);
        CHECK(owner.executions==1&&owner.completions==1&&!owner.request);
        xlsp_async_free(async);CHECK(!runtime_live&&!runtime_bytes);
        request_facts(owner.result.json,root_uri,lib_uri,mode);xjson_free(owner.result.json);CHECK(!lsp_json_live&&!lsp_json_bytes);
    }
    for(unsigned shutdown=0;shutdown<2;++shutdown) {
        XrLspAsync *async=xlsp_async_new(NULL,NULL);CHECK(async);
        SourceAsyncCase blocker={0},queued={0};blocker.blocker=blocker.cancelled=true;queued.cancelled=true;
        source_async_submit(async,&blocker);source_async_wait(async,&blocker,true);
        queued.request=source_async_capture(root_uri,lib_uri,0);source_async_submit(async,&queued);
        if(!shutdown) {
            CHECK(xlsp_async_cancel_task(async,queued.task->task_id));
            CHECK(xlsp_async_cancel_task(async,blocker.task->task_id));
            source_async_wait(async,&queued,false);CHECK(blocker.completions==1);
        }
        xlsp_async_free(async);
        CHECK(blocker.executions==1&&queued.executions==0&&blocker.completions==1&&queued.completions==1&&!queued.request);
        CHECK(!queued.result.json&&!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);
    }
    puts("LSP real worker + owned Source: four queries, queued cancel, shutdown, caller/owner death, physical=0/0");
}
#endif // LSP_SOURCE_ASYNC_CASES_H
