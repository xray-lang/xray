/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef LSP_NAVIGATION_ADMISSION_CASES_H
#define LSP_NAVIGATION_ADMISSION_CASES_H
#include "app/lsp/xlsp_source_request.h"
typedef struct NavigationAdmission {
    XrLspServer server,foreign;
    XrLspAsync *async;
    XrLspTransport *transport;
    XlspRequestId id;
    Gate gate;
    XrLspDocument *root,*library;
} NavigationAdmission;
static void admission_fixture(NavigationAdmission *f,const char *uri,const char *library) {
    memset(f,0,sizeof(*f));f->server.initialized=true;f->server.source_revision=41;f->server.next_request_id=71;
    f->server.transport=f->transport=xlsp_transport_stdio();CHECK(f->transport);
    f->server.async=f->async=xlsp_async_new(NULL,NULL);CHECK(f->async);
    XrLspTask *task=xlsp_task_new(block,block_complete,&f->gate,XLSP_TASK_NORMAL);CHECK(task);
    CHECK(xlsp_async_submit(f->async,task));uint64_t deadline=xr_time_monotonic_ms()+5000;
    while(!atomic_load(&f->gate.entered)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
    f->server.doc_table=xlsp_document_store_new(destroy_document);CHECK(f->server.doc_table);
    add_document(&f->server,uri,root_text,1);add_document(&f->server,library,lib_text,7);
    f->root=xlsp_document_store_get(f->server.doc_table,uri);f->library=xlsp_document_store_get(f->server.doc_table,library);
    CHECK(f->root&&f->library);f->id=identity();XlspRequestId owned={0};
    CHECK(xlsp_request_id_clone(&f->id,&owned)==XLSP_PROTOCOL_OK);
    CHECK(xlsp_pending_request_add(&f->server.pending_requests,owned,"navigation/admission",99));
}
static XrJsonValue *admission_params(NavigationAdmission *f,unsigned scenario) {
    f->root->server=f->library->server=&f->server;
    XrJsonValue *p=parameters(f->root->uri),*doc=xjson_get_object(p,"textDocument"),*pos=xjson_get_object(p,"position");
    if(scenario==0)CHECK(index_json_member(p,"textDocument",xjson_clone(doc)));
    if(scenario==1)CHECK(index_json_member(p,"position",xjson_clone(pos)));
    if(scenario==2)CHECK(index_json_member(doc,"uri",xjson_new_string(f->root->uri)));
    if(scenario==3)CHECK(index_json_member(pos,"line",xjson_new_number(1)));
    if(scenario==4)CHECK(index_json_member(pos,"character",xjson_new_number(43)));
    if(scenario==5)CHECK(index_json_member(p,"context",xjson_clone(xjson_get_object(p,"context"))));
    if(scenario==6)CHECK(index_json_member(xjson_get_object(p,"context"),"includeDeclaration",xjson_new_bool(false)));
    if(scenario==7)f->root->server=&f->foreign;
    if(scenario==8)f->library->server=&f->foreign;
    if(scenario==9){XrJsonValue *line=xjson_get(pos,"line");line->is_integer=false;line->as.number=1.25;}
    if(scenario==10)xjson_get(pos,"character")->as.integer=-1;
    if(scenario==11)xjson_get(pos,"line")->as.integer=(int64_t)INT32_MAX+1;
    if(scenario==12){XrJsonValue *uri=xjson_get(doc,"uri");CHECK(uri&&uri->string_len);uri->as.string[uri->string_len-1]=0;}
    if(scenario==13){XrJsonValue *include=xjson_get(xjson_get_object(p,"context"),"includeDeclaration");include->type=XR_JSON_NUMBER;include->is_integer=true;include->as.integer=0;}
    if(scenario==14){
        CHECK(index_json_member(p,"ignored",xjson_new_number(1))&&index_json_member(p,"ignored",xjson_new_number(2)));
        CHECK(index_json_member(p,"positionX",xjson_new_number(3)));
        XrJsonMember *last=&p->as.object.members[p->as.object.count-1];CHECK(last->key_len==9);last->key[8]=0;
    }
    return p;
}
static void admission_join(NavigationAdmission *f,unsigned tasks) {
    atomic_store(&f->gate.release,true);uint64_t deadline=xr_time_monotonic_ms()+10000;unsigned processed=0;
    while(processed<tasks){processed+=(unsigned)xlsp_async_poll(f->async);CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
    xlsp_async_free(f->async);f->async=NULL;f->server.async=NULL;CHECK(f->gate.completed==1);
}
static void admission_close(NavigationAdmission *f) {
    xlsp_source_workspace_free(f->server.source_workspace);f->server.source_workspace=NULL;
    if(xlsp_pending_request_find(&f->server.pending_requests,&f->id))CHECK(xlsp_pending_request_remove(&f->server.pending_requests,&f->id));
    CHECK(!f->server.pending_requests.count&&f->server.next_request_id==71);
    xlsp_request_id_free(&f->id);xlsp_document_store_free(f->server.doc_table);xlsp_transport_free(f->transport);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
}
static unsigned admission_rejections(NavigationAdmission *f,unsigned method) {
    XrJsonValue *valid=admission_params(f,15),*value=xlsp_navigation_handle(&f->server,valid,method);CHECK(value);
    xjson_free(value);xjson_free(valid);XlspSourceWorkspace *cache=f->server.source_workspace;CHECK(cache);
    unsigned checked=0;
    for(unsigned scenario=0;scenario<14;++scenario){
        if(method!=1&&(scenario==5||scenario==6||scenario==13))continue;
        XrJsonValue *params=admission_params(f,scenario);
        size_t blocks=runtime_live,bytes=runtime_bytes,json=lsp_json_live,json_bytes=lsp_json_bytes;
        size_t app=task_live,app_bytes=task_bytes;XlspPendingRequests pending=f->server.pending_requests;
        XrXirStatus status=xlsp_source_task_submit(&f->server,&f->id,params,method);
        if(status!=XR_XIR_BAD_STRUCTURE)fprintf(stderr,"Rejected navigation mode=%u case=%u status=%u\n",method,scenario,(unsigned)status);
        CHECK(status==XR_XIR_BAD_STRUCTURE&&!memcmp(&pending,&f->server.pending_requests,sizeof(pending)));
        CHECK(!xlsp_navigation_handle(&f->server,params,method)&&f->server.source_failure.status==XR_XIR_BAD_STRUCTURE);
        CHECK(f->server.source_workspace==cache&&runtime_live==blocks&&runtime_bytes==bytes);
        CHECK(lsp_json_live==json&&lsp_json_bytes==json_bytes&&task_live==app&&task_bytes==app_bytes);
        xjson_free(params);++checked;
    }
    valid=admission_params(f,14);XlspSourceRequest *request=NULL;
    CHECK(xlsp_navigation_capture(&f->server,valid,method,&request)==XR_XIR_OK&&request);
    xlsp_source_request_free(request);xjson_free(valid);xlsp_source_workspace_free(cache);f->server.source_workspace=NULL;
    CHECK(!runtime_live&&!runtime_bytes);return checked;
}
static size_t admission_resources(NavigationAdmission *f,unsigned method,bool full) {
    XrJsonValue *params=admission_params(f,15);XlspSourceRequest *request=NULL;size_t begin=runtime_attempts;
    CHECK(xlsp_navigation_capture(&f->server,params,method,&request)==XR_XIR_OK);xlsp_source_request_free(request);request=NULL;
    size_t sites=runtime_attempts-begin;XrCompileResourceStats stats=task_last_stats;CHECK(sites&&stats.allocated_bytes&&stats.peak_bytes&&stats.work);
    if(full){
        for(size_t i=0;i<sites;++i){runtime_fail_at=runtime_attempts+i;
            CHECK(xlsp_navigation_capture(&f->server,params,method,&request)==XR_XIR_OUT_OF_MEMORY&&!request);
            CHECK(runtime_attempts==runtime_fail_at+1&&!runtime_live&&!runtime_bytes);runtime_fail_at=SIZE_MAX;}
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){
            XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};
            uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;
            CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);task_axis_limits=&exact;
            CHECK(xlsp_navigation_capture(&f->server,params,method,&request)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
            CHECK((request!=NULL)==(delta>=0));xlsp_source_request_free(request);request=NULL;task_axis_limits=NULL;
            CHECK(!runtime_live&&!runtime_bytes);}
    }
    fprintf(stderr,"NAV_CAPTURE mode=%u source=%zu allocated=%llu peak=%llu work=%llu full=%u axes=%u physical0/0\n",method,sites,
        (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work,full?1u:0u,full?9u:0u);
    xjson_free(params);return sites;
}
static int navigation_admission_main(int argc,char **argv) {
    CHECK(argc==6||argc==7);unsigned method=(unsigned)strtoul(argv[4],NULL,10);CHECK(method<4);
    bool before=!strcmp(argv[5],"--before"),full=!strcmp(argv[5],"--full");CHECK(before||full||!strcmp(argv[5],"--normal"));
    NavigationAdmission f;admission_fixture(&f,argv[1],argv[3]);unsigned checked=0,scenario=15;
    if(before){CHECK(argc==7);scenario=(unsigned)strtoul(argv[6],NULL,10);CHECK(scenario<=8&&((scenario!=5&&scenario!=6)||method==1));}
    else{CHECK(argc==6);checked=admission_rejections(&f,method);(void)admission_resources(&f,method,full);}
    XrJsonValue *params=admission_params(&f,scenario);XrXirStatus status=xlsp_source_task_submit(&f.server,&f.id,params,method);
    CHECK(status==XR_XIR_OK);xjson_free(params);
    XlspPendingRequest *pending=xlsp_pending_request_find(&f.server.pending_requests,&f.id);CHECK(pending&&pending->source_task_id);
    if(before)CHECK(xlsp_request_cancel(&f.server,&f.id));
    admission_join(&f,2);admission_close(&f);
    fprintf(stderr,"NAV_ADMISSION mode=%u checks=%u case=%u accepted=%u physical0/0 JSON0/0 app0/0\n",method,checked,scenario,before?1u:0u);
    return before?1:0;
}
#endif // LSP_NAVIGATION_ADMISSION_CASES_H
