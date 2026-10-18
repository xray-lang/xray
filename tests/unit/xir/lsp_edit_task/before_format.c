/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xlsp_format.h"
#include "xlsp_server.h"
#include "xlsp_source_format.h"
#include "../../api/xsource_compile.h"
#include <string.h>

XR_FUNC XrJsonValue *xlsp_format_handle(XrLspServer *server,XrJsonValue *params) {
    if(!server)return NULL;server->formatting_failure.stage=1;server->formatting_failure.status=XR_XIR_BAD_STRUCTURE;
    XrJsonValue *document=xjson_get_object(params,"textDocument"),*uri=xjson_get(document,"uri");
    XrJsonValue *options=xjson_get_object(params,"options"),*tabs=xjson_get(options,"tabSize"),*spaces=xjson_get(options,"insertSpaces");
    if(!uri||uri->type!=XR_JSON_STRING||!uri->as.string||memchr(uri->as.string,0,uri->string_len)||
       !tabs||tabs->type!=XR_JSON_NUMBER||!tabs->is_integer||tabs->as.integer<=0||tabs->as.integer>INT32_MAX||
       !spaces||spaces->type!=XR_JSON_BOOL)return NULL;
    XrLspDocument *doc=xlsp_document_get(server,uri->as.string);if(!doc||doc->server!=server||!doc->content)return NULL;
    XrFmtConfig config=xfmt_default_config;XlspConfig *settings=&server->config;
    config.indent_size=(int)tabs->as.integer;config.use_tabs=spaces->as.boolean?0:1;
    config.max_line_length=settings->format_max_line_length;
    config.align_branch_arrows=settings->format_align_branch_arrows?1:0;config.align_enum_values=settings->format_align_enum_values?1:0;
    config.align_struct_fields=settings->format_align_struct_fields?1:0;config.align_trailing_comments=settings->format_align_trailing_comments?1:0;
    config.wrap_long_lines=settings->format_wrap_long_lines?1:0;config.multiline_trailing_comma=settings->format_multiline_trailing_comma?1:0;
    XrCompileResourceLimits limits=xr_source_compile_default_resource_limits();XrCompileResources *resources=NULL;XrJsonValue *result=NULL;
    XrCompileResourceStatus created=xr_compile_resources_new(&limits,&resources);
    XrXirStatus status=created==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XlspSourceDocument input={uri->as.string,doc->content,uri->string_len,doc->length,doc->version};
    if(status==XR_XIR_OK)status=xlsp_source_format_json(resources,&input,&config,&result);
    xr_compile_resources_release(resources);server->formatting_failure.status=(unsigned)status;
    if(status==XR_XIR_OK)server->formatting_failure.stage=0;return result;
}
