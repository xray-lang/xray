/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xlsp_format.h"
#include "xlsp_server.h"
#include "xlsp_source_format.h"
#include "xlsp_hierarchy_request.h"
#include <string.h>

XR_FUNC XrJsonValue *xlsp_format_handle(XrLspServer *server,XrJsonValue *params) {
    if(!server)return NULL;server->formatting_failure.stage=1;server->formatting_failure.status=XR_XIR_BAD_STRUCTURE;
    XrJsonValue *document=xjson_get_object(params,"textDocument"),*uri=xjson_get(document,"uri");
    XrJsonValue *options=xjson_get_object(params,"options"),*tabs=xjson_get(options,"tabSize"),*spaces=xjson_get(options,"insertSpaces");
    if(!uri||uri->type!=XR_JSON_STRING||!uri->as.string||memchr(uri->as.string,0,uri->string_len)||
       !tabs||tabs->type!=XR_JSON_NUMBER||!tabs->is_integer||tabs->as.integer<=0||tabs->as.integer>INT32_MAX||
       !spaces||spaces->type!=XR_JSON_BOOL)return NULL;
    XrJsonValue *result=xlsp_source_scope_query(server,params,0,XLSP_SOURCE_FORMATTING);
    server->formatting_failure.status=server->source_failure.status;
    if(result)server->formatting_failure.stage=0;
    return result;
}
