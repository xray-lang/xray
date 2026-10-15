/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xlsp_links.h"
#include "xlsp_source_links.h"
#include "../../api/xsource_compile.h"
#include <string.h>

static XrXirStatus links_capture_length(XrCompileResources *r,const char *text,size_t *length) {
    if(!text)return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;;++i) {
        if(xr_compile_resources_work(r,1)!=XR_COMPILE_RESOURCE_OK)return XR_XIR_BUDGET;
        if(!text[i]){*length=i;return XR_XIR_OK;}if(i==SIZE_MAX-1)return XR_XIR_BUDGET;
    }
}
XR_FUNC XrJsonValue *xlsp_links_handle(XrLspServer *server,XrJsonValue *params) {
    if(!server)return NULL;server->source_failure.stage=15;server->source_failure.status=XR_XIR_BAD_STRUCTURE;
    XrJsonValue *document=xjson_get_object(params,"textDocument"),*uri=xjson_get(document,"uri");
    if(!uri||uri->type!=XR_JSON_STRING||!uri->as.string||memchr(uri->as.string,0,uri->string_len))return NULL;
    XrLspDocTable *table=server->doc_table;XrLspDocument *selected=xlsp_document_store_get(table,uri->as.string);
    if(!selected||selected->server!=server||!table||table->doc_count<=0||table->bucket_count<=0||!table->buckets)return NULL;
    XrCompileResourceLimits limits=xr_source_compile_default_resource_limits();XrCompileResources *r=NULL;
    XlspSourceDocument *documents=NULL;XlspSourceLinks *links=NULL;XrJsonValue *result=NULL;
    XrCompileResourceStatus created=xr_compile_resources_new(&limits,&r);
    XrXirStatus status=created==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    if(status!=XR_XIR_OK)goto done;
    size_t count=(size_t)table->doc_count,used=0,entry=SIZE_MAX;
    if(count>SIZE_MAX/sizeof(*documents)){status=XR_XIR_BUDGET;goto done;}
    created=xr_compile_resources_calloc(r,count,sizeof(*documents),(void **)&documents);
    if(created!=XR_COMPILE_RESOURCE_OK){status=created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;goto done;}
    for(int i=0;i<table->bucket_count;++i) {
        if(xr_compile_resources_work(r,1)!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
        for(XrLspDocBucket *bucket=table->buckets[i];bucket;bucket=bucket->next) {
            XrLspDocument *doc=bucket->doc;size_t length=0;
            if(used==count||!doc||doc->server!=server||!doc->uri||!doc->content){status=XR_XIR_BAD_STRUCTURE;goto done;}
            status=links_capture_length(r,doc->uri,&length);if(status!=XR_XIR_OK)goto done;
            if(doc==selected)entry=used;
            if(xr_compile_resources_work(r,sizeof(*documents))!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
            documents[used++]=(XlspSourceDocument){doc->uri,doc->content,length,doc->length,doc->version};
        }
    }
    if(used!=count||entry==SIZE_MAX){status=XR_XIR_BAD_STRUCTURE;goto done;}
    XrXirCompileContext context={r,xr_xir_compile_default_limits()};
    status=xlsp_source_links_build(&context,documents,count,entry,&links);
    if(status==XR_XIR_OK)status=xlsp_source_links_json(links,&result);
done:
    xlsp_source_links_free(links);xr_compile_resources_free(documents);xr_compile_resources_release(r);
    server->source_failure.status=(unsigned)status;if(status==XR_XIR_OK)server->source_failure.stage=0;return result;
}
