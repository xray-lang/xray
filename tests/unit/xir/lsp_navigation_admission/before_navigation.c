/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xlsp_navigation.h"
#include "xlsp_server.h"
#include "xlsp_source_workspace.h"
#include "xlsp_source_request.h"
#include "xlsp_hierarchy_request.h"
#include <string.h>

static const XrCompileResourceLimits navigation_limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};

static XrXirStatus navigation_length(XrCompileResources *r,const char *text,size_t *out) {
    if(!text)return XR_XIR_BAD_STRUCTURE;
    size_t n=0;
    for(;;) {
        if(xr_compile_resources_work(r,1)!=XR_COMPILE_RESOURCE_OK)return XR_XIR_BUDGET;
        if(!text[n]){*out=n;return XR_XIR_OK;}
        if(n==SIZE_MAX-1)return XR_XIR_BUDGET;
        ++n;
    }
}
XR_FUNC XrXirStatus xlsp_navigation_capture(XrLspServer *server,XrJsonValue *params,unsigned mode,XlspSourceRequest **out) {
    if(!server||!out||*out||mode>3)return XR_XIR_BAD_STRUCTURE;
    server->source_failure.stage=8;server->source_failure.status=XR_XIR_BAD_STRUCTURE;
    XrJsonValue *document=xjson_get_object(params,"textDocument"),*position=xjson_get_object(params,"position");
    XrJsonValue *uri=xjson_get(document,"uri"),*line=xjson_get(position,"line"),*column=xjson_get(position,"character");
    if(!uri||uri->type!=XR_JSON_STRING||!uri->as.string||memchr(uri->as.string,0,uri->string_len)||
        !line||line->type!=XR_JSON_NUMBER||!line->is_integer||line->as.integer<0||line->as.integer>INT32_MAX||
        !column||column->type!=XR_JSON_NUMBER||!column->is_integer||column->as.integer<0||column->as.integer>INT32_MAX)return XR_XIR_BAD_STRUCTURE;
    bool include=false;
    if(mode==1){XrJsonValue *c=xjson_get_object(params,"context"),*v=xjson_get(c,"includeDeclaration");
        if(!v||v->type!=XR_JSON_BOOL)return XR_XIR_BAD_STRUCTURE;include=v->as.boolean;}
    XrLspDocument *selected=xlsp_document_store_get(server->doc_table,uri->as.string);XrLspDocTable *table=server->doc_table;
    if(!selected||!table||table->doc_count<=0||table->bucket_count<=0||!table->buckets)return XR_XIR_BAD_STRUCTURE;
    XrCompileResources *resources=NULL;
    XlspSourceDocument *documents=NULL;size_t count=(size_t)table->doc_count,used=0,entry=SIZE_MAX;
    XrCompileResourceStatus created=xr_compile_resources_new(&navigation_limits,&resources);
    XrXirStatus status=created==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    if(status!=XR_XIR_OK)goto done;
    if(count>SIZE_MAX/sizeof(*documents)){status=XR_XIR_BUDGET;goto done;}
    created=xr_compile_resources_calloc(resources,count,sizeof(*documents),(void **)&documents);
    if(created!=XR_COMPILE_RESOURCE_OK){status=created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;goto done;}
    for(int i=0;i<table->bucket_count;++i) {
        if(xr_compile_resources_work(resources,1)!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
        for(XrLspDocBucket *bucket=table->buckets[i];bucket;bucket=bucket->next) {
            XrLspDocument *doc=bucket->doc;size_t length=0;
            if(used==count||!doc||!doc->uri||!doc->content){status=XR_XIR_BAD_STRUCTURE;goto done;}
            status=navigation_length(resources,doc->uri,&length);if(status!=XR_XIR_OK)goto done;
            if(doc==selected)entry=used;
            documents[used++]=(XlspSourceDocument){doc->uri,doc->content,length,doc->length,doc->version};
        }
    }
    if(used!=count||entry==SIZE_MAX){status=XR_XIR_BAD_STRUCTURE;goto done;}
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XlspSourceRequestInput input={&context,documents,count,entry,{(uint32_t)line->as.integer,(uint32_t)column->as.integer},mode,include};
    status=xlsp_source_request_new(&input,out);
done:
    xr_compile_resources_free(documents);xr_compile_resources_release(resources);
    server->source_failure.status=(unsigned)status;if(status==XR_XIR_OK)server->source_failure.stage=0;
    return status;
}
/* A request is synchronous with document notifications on the dispatch thread.
 * Always construct the current inputs; the previous committed owner is never
 * a fallback cache. Its lifetime ends only after complete new JSON publication. */
static XrJsonValue *navigation_request(XrLspServer *server,XrLspDocument *doc,
    XrLspPosition position,unsigned mode,bool include_declaration) {
    if(!server)return NULL;
    server->source_failure.stage=1;server->source_failure.status=XR_XIR_BAD_STRUCTURE;
    if(!doc||doc->server!=server||!server->doc_table||server->doc_table->doc_count<=0)return NULL;
    XrCompileResources *resources=NULL;XlspSourceDocument *documents=NULL;
    XlspSourceWorkspace *fresh=NULL;XrJsonValue *result=NULL;
    XrCompileResourceStatus created=xr_compile_resources_new(&navigation_limits,&resources);
    XrXirStatus status=created==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:
        created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    if(status!=XR_XIR_OK)goto done;
    size_t count=(size_t)server->doc_table->doc_count,used=0,selected_length=0;bool selected=false;
    if(count>SIZE_MAX/sizeof(*documents)){status=XR_XIR_BUDGET;goto done;}
    created=xr_compile_resources_calloc(resources,count,sizeof(*documents),(void **)&documents);
    if(created!=XR_COMPILE_RESOURCE_OK){status=created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;goto done;}
    for(int i=0;i<server->doc_table->bucket_count;++i) {
        if(xr_compile_resources_work(resources,1)!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
        for(XrLspDocBucket *bucket=server->doc_table->buckets[i];bucket;bucket=bucket->next) {
            XrLspDocument *item=bucket->doc;
            if(!item||!item->content||used==count){status=XR_XIR_BAD_STRUCTURE;goto done;}
            size_t length=0;status=navigation_length(resources,item->uri,&length);
            if(status!=XR_XIR_OK)goto done;
            if(xr_compile_resources_work(resources,sizeof(*documents))!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
            documents[used++]=(XlspSourceDocument){item->uri,item->content,length,item->length,item->version};
            if(item==doc){selected=true;selected_length=length;}
        }
    }
    if(!selected||used!=count){status=XR_XIR_BAD_STRUCTURE;goto done;}
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    const XlspSourceDocument *inputs=documents;size_t input_count=count;
    status=xlsp_source_workspace_build(&context,inputs,input_count,&fresh,&server->source_failure.stage,NULL);
    if(status!=XR_XIR_OK)goto done;
    server->source_failure.stage=7;
    status=xlsp_source_workspace_navigation(fresh,doc->uri,selected_length,position,mode,include_declaration,&result);
    if(status==XR_XIR_OK) {
        XlspSourceWorkspace *old=server->source_workspace;server->source_workspace=fresh;fresh=NULL;
        xlsp_source_workspace_free(old);server->source_failure.stage=0;
    }
done:
    server->source_failure.status=(unsigned)status;
    xlsp_source_workspace_free(fresh);xr_compile_resources_free(documents);xr_compile_resources_release(resources);
    return result;
}
XrJsonValue *xlsp_analyze_definition(XrLspServer *server,XrLspDocument *doc,XrLspPosition pos) {
    return navigation_request(server,doc,pos,0,false);
}
XrJsonValue *xlsp_analyze_references(XrLspServer *server,XrLspDocument *doc,XrLspPosition pos,bool include_declaration) {
    return navigation_request(server,doc,pos,1,include_declaration);
}
XrJsonValue *xlsp_analyze_document_highlight(XrLspServer *server,XrLspDocument *doc,XrLspPosition pos) {
    return navigation_request(server,doc,pos,2,true);
}
XrJsonValue *xlsp_code_lens_handle(XrLspServer *server,XrJsonValue *params) {
    return xlsp_source_scope_query(server,params,0,XLSP_SOURCE_BINDING_LENSES);
}
/* Strict protocol UInteger/URI admission is shared by navigation and typed hover handlers. */
XrJsonValue *xlsp_navigation_handle(XrLspServer *server,XrJsonValue *params,unsigned mode) {
    if(!server)return NULL;
    server->source_failure.stage=8;server->source_failure.status=XR_XIR_BAD_STRUCTURE;
    XrJsonValue *document=xjson_get_object(params,"textDocument"),*position=xjson_get_object(params,"position");
    XrJsonValue *uri=xjson_get(document,"uri"),*line=xjson_get(position,"line"),*character=xjson_get(position,"character");
    if(!uri||uri->type!=XR_JSON_STRING||!uri->as.string||memchr(uri->as.string,0,uri->string_len)||
        !line||line->type!=XR_JSON_NUMBER||!line->is_integer||line->as.integer<0||line->as.integer>INT32_MAX||
        !character||character->type!=XR_JSON_NUMBER||!character->is_integer||character->as.integer<0||character->as.integer>INT32_MAX)return NULL;
    bool include_declaration=false;
    if(mode==1) {
        XrJsonValue *context=xjson_get_object(params,"context"),*include=xjson_get(context,"includeDeclaration");
        if(!include||include->type!=XR_JSON_BOOL)return NULL;
        include_declaration=include->as.boolean;
    }
    XrLspDocument *doc=xlsp_document_store_get(server->doc_table,uri->as.string);
    return navigation_request(server,doc,(XrLspPosition){(uint32_t)line->as.integer,(uint32_t)character->as.integer},mode,include_declaration);
}

XrJsonValue *xlsp_semantic_handle(XrLspServer *server,XrJsonValue *params,unsigned mode) {
    if(!server)return NULL;
    server->source_failure.stage=11;server->source_failure.status=XR_XIR_BAD_STRUCTURE;
    if(mode>2)return NULL;
    return xlsp_source_scope_query(server,params,0,(XlspSourceScopeQueryKind)(XLSP_SOURCE_SEMANTIC_FULL+mode));
}
