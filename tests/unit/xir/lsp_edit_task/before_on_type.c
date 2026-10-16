/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_on_type.c - Strict on-type formatting request adapter
 *
 * KEY CONCEPT:
 *   Reject malformed editor positions and publish only complete owned edits.
 */
#include "xlsp_on_type.h"
#include "xlsp_source_on_type.h"
#include "xlsp_server.h"
#include "xlsp_document_store.h"
#include "../../api/xsource_compile.h"
#include <string.h>
XR_FUNC XrJsonValue *xlsp_on_type_handle(XrLspServer *server,XrJsonValue *params) {
    if(!server)return NULL;server->source_failure.stage=16;server->source_failure.status=XR_XIR_BAD_STRUCTURE;
    XrJsonValue *document=xjson_get_object(params,"textDocument"),*uri=xjson_get(document,"uri");
    XrJsonValue *position=xjson_get_object(params,"position"),*line=xjson_get(position,"line"),*column=xjson_get(position,"character");
    XrJsonValue *options=xjson_get_object(params,"options"),*tabs=xjson_get(options,"tabSize"),*spaces=xjson_get(options,"insertSpaces"),*trigger=xjson_get(params,"ch");
    if(!uri||uri->type!=XR_JSON_STRING||!uri->as.string||memchr(uri->as.string,0,uri->string_len)||
       !line||line->type!=XR_JSON_NUMBER||!line->is_integer||line->as.integer<0||line->as.integer>INT32_MAX||
       !column||column->type!=XR_JSON_NUMBER||!column->is_integer||column->as.integer<0||column->as.integer>INT32_MAX||
       !tabs||tabs->type!=XR_JSON_NUMBER||!tabs->is_integer||tabs->as.integer<=0||tabs->as.integer>INT32_MAX||
       !spaces||spaces->type!=XR_JSON_BOOL||!trigger||trigger->type!=XR_JSON_STRING||!trigger->as.string||trigger->string_len!=1||
       (trigger->as.string[0]!='}'&&trigger->as.string[0]!='\n'&&trigger->as.string[0]!=';'))return NULL;
    XrLspDocument *doc=xlsp_document_store_get(server->doc_table,uri->as.string);if(!doc||doc->server!=server||!doc->content)return NULL;
    XlspSourceDocument input={uri->as.string,doc->content,uri->string_len,doc->length,doc->version};
    XlspSourceOnTypeInput settings={{(uint32_t)line->as.integer,(uint32_t)column->as.integer},(uint32_t)tabs->as.integer,spaces->as.boolean,trigger->as.string[0]};
    XrCompileResourceLimits limits=xr_source_compile_default_resource_limits();XrCompileResources *resources=NULL;XrJsonValue *result=NULL;
    XrCompileResourceStatus made=xr_compile_resources_new(&limits,&resources);
    XrXirStatus status=made==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    if(status==XR_XIR_OK)status=xlsp_source_on_type_json(resources,&input,&settings,&result);
    xr_compile_resources_release(resources);server->source_failure.status=(unsigned)status;if(status==XR_XIR_OK)server->source_failure.stage=0;return result;
}
