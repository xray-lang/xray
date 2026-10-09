/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xlsp_folding.h"
#include "xlsp_source_syntax.h"
#include "../cli/xcli_canonical_source.h"
#include <string.h>
XrJsonValue *xlsp_handle_folding_range(XrLspServer *server,XrJsonValue *params) {
    if(!server)return NULL;
    server->source_failure.stage=9;server->source_failure.status=XR_XIR_BAD_STRUCTURE;
    XrJsonValue *document=xjson_get_object(params,"textDocument"),*uri=xjson_get(document,"uri");
    if(!uri||uri->type!=XR_JSON_STRING||!uri->as.string||memchr(uri->as.string,0,uri->string_len))return NULL;
    XrLspDocument *doc=xlsp_document_get(server,uri->as.string);
    if(!doc||doc->server!=server||!doc->content)return NULL;
    XrCompileResourceLimits limits=xr_cli_compile_default_resource_limits();
    XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;XlspSyntaxSnapshot *fresh=NULL;
    XrJsonValue *result=NULL;XrXirStatus status=XR_XIR_OK;
    XrCompileResourceStatus created=xr_compile_resources_new(&limits,&resources);
    if(created!=XR_COMPILE_RESOURCE_OK) {status=created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;goto done;}
    XrCompilerSessionStatus opened=xr_compile_session_new(resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK) {status=opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;goto done;}
    XlspSourceDocument input={uri->as.string,doc->content,uri->string_len,doc->length,doc->version};
    XrParseStatus parsed=xlsp_source_syntax_build(session,&input,&fresh);
    if(parsed!=XR_PARSE_OK&&parsed!=XR_PARSE_RECOVERED) {
        status=parsed==XR_PARSE_BUDGET?XR_XIR_BUDGET:parsed==XR_PARSE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;goto done;
    }
    status=xlsp_source_syntax_folding_json(fresh,&result);
    if(status==XR_XIR_OK) {
        XlspSyntaxSnapshot *old=doc->syntax_snapshot;doc->syntax_snapshot=fresh;fresh=NULL;
        xlsp_source_syntax_free(old);server->source_failure.stage=0;
    }
done:
    server->source_failure.status=(unsigned)status;
    xlsp_source_syntax_free(fresh);xr_compile_session_free(session);xr_compile_resources_release(resources);
    return result;
}
