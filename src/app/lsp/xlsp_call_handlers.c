/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_call_handlers.c - Capture real workspace input for Source call hierarchy
 */
#include "xlsp_call_hierarchy.h"
#include "xlsp_source_call_hierarchy.h"
#include "xlsp_source_scope.h"
#include <string.h>
static XrXirStatus call_capture_resource(XrCompileResourceStatus s) {
    return s==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:s==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        s==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus call_capture_length(XrCompileResources *r,const char *text,size_t *out) {
    if(!text)return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;;++i){if(xr_compile_resources_work(r,1)!=XR_COMPILE_RESOURCE_OK)return XR_XIR_BUDGET;
        if(!text[i]){*out=i;return XR_XIR_OK;}if(i==SIZE_MAX-1)return XR_XIR_BUDGET;}
}
static XrXirStatus call_capture_documents(XrLspServer *server,XrCompileResources *r,XlspSourceDocument **out,size_t *count) {
    XrLspDocTable *table=server->doc_table;*count=0;
    if(!table)return XR_XIR_OK;
    if(table->doc_count<0||table->bucket_count<=0||!table->buckets)return XR_XIR_BAD_STRUCTURE;
    size_t capacity=(size_t)table->doc_count;if(capacity>SIZE_MAX/sizeof(**out))return XR_XIR_BUDGET;
    XrXirStatus s=capacity?call_capture_resource(xr_compile_resources_calloc(r,capacity,sizeof(**out),(void **)out)):XR_XIR_OK;
    for(int i=0;i<table->bucket_count&&s==XR_XIR_OK;++i){s=call_capture_resource(xr_compile_resources_work(r,1));
        for(XrLspDocBucket *bucket=table->buckets[i];bucket&&s==XR_XIR_OK;bucket=bucket->next){XrLspDocument *doc=bucket->doc;size_t length=0;
            if(*count==capacity||!doc||doc->server!=server||!doc->content||doc->length==SIZE_MAX)return XR_XIR_BAD_STRUCTURE;
            s=call_capture_length(r,doc->uri,&length);if(s!=XR_XIR_OK)break;
            s=call_capture_resource(xr_compile_resources_work(r,sizeof(**out)));if(s!=XR_XIR_OK)break;
            (*out)[(*count)++]=(XlspSourceDocument){doc->uri,doc->content,length,doc->length,doc->version};}}
    return s!=XR_XIR_OK?s:*count==capacity?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static XrJsonValue *call_handle(XrLspServer *server,XrJsonValue *params,unsigned mode) {
    if(!server)return NULL;server->source_failure.stage=24;server->source_failure.status=XR_XIR_BAD_STRUCTURE;
    XrJsonValue *object=xjson_get_object(params,mode?"item":"textDocument"),*uri=xjson_get(object,"uri");
    if(!uri||uri->type!=XR_JSON_STRING||!uri->as.string||memchr(uri->as.string,0,uri->string_len)||server->shutdown_received)return NULL;
    if(server->workspace_folder_count<0||server->workspace_folder_count>MAX_WORKSPACE_FOLDERS)return NULL;
    uint64_t revision=server->source_revision,generation=server->doc_table?server->doc_table->generation:0;
    XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};XrCompileResources *r=NULL;
    XrXirStatus status=call_capture_resource(xr_compile_resources_new(&limits,&r));
    XlspSourceDocument *documents=NULL;XlspSourceScope *scope=NULL;XrJsonValue *result=NULL;size_t count=0,entry=SIZE_MAX;
    if(status!=XR_XIR_OK)goto done;
    status=call_capture_documents(server,r,&documents,&count);if(status!=XR_XIR_OK)goto done;
    const char *roots[MAX_WORKSPACE_FOLDERS];size_t root_count=(size_t)server->workspace_folder_count;
    for(size_t i=0;i<root_count;++i){status=call_capture_resource(xr_compile_resources_work(r,sizeof(*roots)));if(status!=XR_XIR_OK)goto done;
        roots[i]=server->workspace_folders[i].path;if(!roots[i]){status=XR_XIR_BAD_STRUCTURE;goto done;}}
    XrXirCompileContext context={r,xr_xir_compile_default_limits()};
    XlspSourceScopeOptions options={&context,documents,count,roots,root_count,UINT32_C(50)*1024*1024,&server->config};
    status=xlsp_source_scope_capture(&options,&scope);if(status!=XR_XIR_OK)goto done;
    status=xlsp_source_scope_find(scope,uri->as.string,uri->string_len,&entry);if(status!=XR_XIR_OK)goto done;
    XlspSourceCallRequest request={&context,xlsp_source_scope_documents(scope),xlsp_source_scope_known_versions(scope),xlsp_source_scope_count(scope),entry,params,revision,mode};
    status=xlsp_source_call_hierarchy_json(&request,&result);
    if(status==XR_XIR_OK&&(server->shutdown_received||server->source_revision!=revision||
        (server->doc_table?server->doc_table->generation:0)!=generation)){xjson_free(result);result=NULL;status=XR_XIR_UNRESOLVED;}
done:
    xlsp_source_scope_free(scope);xr_compile_resources_free(documents);xr_compile_resources_release(r);
    server->source_failure.status=(unsigned)status;if(status==XR_XIR_OK)server->source_failure.stage=0;return result;
}
XR_FUNC XrJsonValue *xlsp_handle_prepare_call_hierarchy(XrLspServer *server,XrJsonValue *params){return call_handle(server,params,0);}
XR_FUNC XrJsonValue *xlsp_handle_call_hierarchy_incoming(XrLspServer *server,XrJsonValue *params){return call_handle(server,params,1);}
XR_FUNC XrJsonValue *xlsp_handle_call_hierarchy_outgoing(XrLspServer *server,XrJsonValue *params){return call_handle(server,params,2);}
