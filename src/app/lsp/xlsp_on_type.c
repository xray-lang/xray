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
#include "xlsp_hierarchy_request.h"
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
    XrJsonValue *result=xlsp_source_scope_query(server,params,0,XLSP_SOURCE_ON_TYPE);
    if(!result)server->source_failure.stage=16;
    return result;
}
