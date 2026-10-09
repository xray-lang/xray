/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xlsp_navigation.h"
#include "xlsp_server.h"
#include "xlsp_source_workspace.h"
#include "../cli/xcli_canonical_source.h"
#include <string.h>
#include <stdio.h>

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
/* A request is synchronous with document notifications on the dispatch thread.
 * Always construct the current inputs; the previous committed owner is never
 * a fallback cache. Its lifetime ends only after complete new JSON publication. */
static XrJsonValue *navigation_request(XrLspServer *server,XrLspDocument *doc,
    XrLspPosition position,unsigned mode,bool include_declaration,const XrJsonValue *previous_id,const XrLspRange *range) {
    if(!server)return NULL;
    server->source_failure.stage=1;server->source_failure.status=XR_XIR_BAD_STRUCTURE;
    if(!doc||doc->server!=server||!server->doc_table||server->doc_table->doc_count<=0)return NULL;
    XrCompileResourceLimits limits=xr_cli_compile_default_resource_limits();
    XrCompileResources *resources=NULL;XlspSourceDocument *documents=NULL;
    XlspSourceWorkspace *fresh=NULL;XrJsonValue *result=NULL;XlspSourceTokens *tokens=NULL;
    XrCompileResourceStatus created=xr_compile_resources_new(&limits,&resources);
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
    status=xlsp_source_workspace_build(&context,documents,count,&fresh,&server->source_failure.stage,NULL);
    if(status!=XR_XIR_OK)goto done;
    server->source_failure.stage=7;
    if(mode>=4) {
        status=xlsp_source_workspace_tokens(fresh,doc->uri,selected_length,range,&tokens);
        if(status!=XR_XIR_OK)goto done;
        if(!range&&doc->sem_token_result_id==UINT32_MAX){status=XR_XIR_BUDGET;goto done;}
        bool matches=false;
        if(mode==5&&previous_id&&doc->source_tokens) {
            char id[16];int n=snprintf(id,sizeof(id),"%u",doc->sem_token_result_id);
            if(n<=0||(size_t)n>=sizeof(id)){status=XR_XIR_BAD_STRUCTURE;goto done;}
            if(xr_compile_resources_work(resources,(size_t)n)!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
            matches=previous_id->string_len==(size_t)n&&!memcmp(previous_id->as.string,id,(size_t)n);
        }
        status=xlsp_source_tokens_json(resources,tokens,matches?doc->source_tokens:NULL,
            range?0:doc->sem_token_result_id+1,mode==5,range,&result);
        if(status==XR_XIR_OK&&!range) {
            XlspSourceTokens *old=doc->source_tokens;doc->source_tokens=tokens;tokens=NULL;
            ++doc->sem_token_result_id;xlsp_source_tokens_free(old);
        }
    } else status=xlsp_source_workspace_navigation(fresh,doc->uri,selected_length,position,mode,include_declaration,&result);
    if(status==XR_XIR_OK) {
        XlspSourceWorkspace *old=server->source_workspace;server->source_workspace=fresh;fresh=NULL;
        xlsp_source_workspace_free(old);server->source_failure.stage=0;
    }
done:
    server->source_failure.status=(unsigned)status;
    xlsp_source_tokens_free(tokens);xlsp_source_workspace_free(fresh);xr_compile_resources_free(documents);xr_compile_resources_release(resources);
    return result;
}
XrJsonValue *xlsp_analyze_definition(XrLspServer *server,XrLspDocument *doc,XrLspPosition pos) {
    return navigation_request(server,doc,pos,0,false,NULL,NULL);
}
XrJsonValue *xlsp_analyze_references(XrLspServer *server,XrLspDocument *doc,XrLspPosition pos,bool include_declaration) {
    return navigation_request(server,doc,pos,1,include_declaration,NULL,NULL);
}
XrJsonValue *xlsp_analyze_document_highlight(XrLspServer *server,XrLspDocument *doc,XrLspPosition pos) {
    return navigation_request(server,doc,pos,2,true,NULL,NULL);
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
    XrLspDocument *doc=xlsp_document_get(server,uri->as.string);
    return navigation_request(server,doc,(XrLspPosition){(uint32_t)line->as.integer,(uint32_t)character->as.integer},mode,include_declaration,NULL,NULL);
}

static bool semantic_protocol_position(XrJsonValue *object,XrLspPosition *out) {
    XrJsonValue *line=xjson_get(object,"line"),*column=xjson_get(object,"character");
    if(!line||line->type!=XR_JSON_NUMBER||!line->is_integer||line->as.integer<0||line->as.integer>INT32_MAX||
        !column||column->type!=XR_JSON_NUMBER||!column->is_integer||column->as.integer<0||column->as.integer>INT32_MAX)return false;
    *out=(XrLspPosition){(uint32_t)line->as.integer,(uint32_t)column->as.integer};return true;
}
XrJsonValue *xlsp_semantic_handle(XrLspServer *server,XrJsonValue *params,unsigned mode) {
    if(!server)return NULL;server->source_failure.stage=11;server->source_failure.status=XR_XIR_BAD_STRUCTURE;
    if(mode>2)return NULL;
    XrJsonValue *document=xjson_get_object(params,"textDocument"),*uri=xjson_get(document,"uri"),*previous=NULL;
    if(!uri||uri->type!=XR_JSON_STRING||!uri->as.string||memchr(uri->as.string,0,uri->string_len))return NULL;
    XrLspRange range={0};
    if(mode==1) {
        previous=xjson_get(params,"previousResultId");
        if(!previous||previous->type!=XR_JSON_STRING||!previous->as.string||memchr(previous->as.string,0,previous->string_len))return NULL;
    } else if(mode==2) {
        XrJsonValue *r=xjson_get_object(params,"range");
        if(!r||!semantic_protocol_position(xjson_get_object(r,"start"),&range.start)||!semantic_protocol_position(xjson_get_object(r,"end"),&range.end))return NULL;
    }
    XrLspDocument *doc=xlsp_document_get(server,uri->as.string);
    return navigation_request(server,doc,(XrLspPosition){0},mode+4,false,previous,mode==2?&range:NULL);
}
