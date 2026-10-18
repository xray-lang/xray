/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_diagnostic_publish.c - Publish and clear exact Source diagnostic owners
 */
#include "xlsp_diagnostic_internal.h"
static XrXirStatus diagnostic_clear_previous(XrLspServer *server,XlspSourceOpen *open) {
    const XrJsonValue *old=xlsp_source_open_published_clear(open);
    if(!old)return XR_XIR_UNRESOLVED;
    const XrJsonValue *uri=diagnostic_uri(old);if(!uri)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=diagnostic_survey(server,open,uri);if(status!=XR_XIR_OK)return status;
    XrJsonValue *clear=xjson_clone((XrJsonValue *)old);if(!clear)return XR_XIR_OUT_OF_MEMORY;
    status=xlsp_protocol_send_notification_pair(server,"textDocument/publishDiagnostics",clear,NULL);
    if(status!=XR_XIR_OK)return status;
    XrJsonValue *empty=NULL;xlsp_source_open_publication_commit(open,&empty);
    return XR_XIR_UNRESOLVED;
}
XR_FUNC void xlsp_publish_diagnostics(XrLspServer *server,XrLspDocument *doc) {
    if(!server||!doc||doc->server!=server||!doc->source_open||server->exit_received||!server->config.diagnostics_enabled)return;
    XlspSourceOpen *open=doc->source_open;
    if(xlsp_source_open_diagnostic_revision(open)!=server->source_revision)return;
    XrJsonValue *params=NULL,*clear=NULL,*previous=NULL;
    XrXirStatus status=xlsp_source_open_diagnostics_params_json(open,doc->source_buffer,doc->syntax_snapshot,&params);
    if(status==XR_XIR_UNRESOLVED)status=diagnostic_clear_previous(server,open);
    if(status!=XR_XIR_OK)goto done;
    const XrJsonValue *uri=diagnostic_uri(params);XrJsonValue *version=xjson_get(params,"version");
    if(!uri||!version){status=XR_XIR_BAD_STRUCTURE;goto done;}
    XrLspDocument *target=xlsp_document_get(server,uri->as.string);
    if(version->type==XR_JSON_NULL) {
        if(target)goto done;
    }else if(version->type!=XR_JSON_NUMBER||!target||target->version!=xjson_get_int(params,"version"))goto done;
    status=diagnostic_survey(server,open,uri);if(status!=XR_XIR_OK)goto done;
    clear=diagnostic_clear_new(uri);if(!clear){status=XR_XIR_OUT_OF_MEMORY;goto done;}
    const XrJsonValue *old=xlsp_source_open_published_clear(open);
    if(old&&!diagnostic_equal(diagnostic_uri(old),uri)) {
        previous=xjson_clone((XrJsonValue *)old);if(!previous){status=XR_XIR_OUT_OF_MEMORY;goto done;}
    }
    /* clear retains the target string after the consumed wire params die. */
    status=xlsp_protocol_send_notification_pair(server,"textDocument/publishDiagnostics",
        previous?previous:params,previous?params:NULL);
    params=NULL;previous=NULL;
    if(status==XR_XIR_OK){diagnostic_evict(server,diagnostic_uri(clear));xlsp_source_open_publication_commit(open,&clear);}
done:
    xjson_free(previous);xjson_free(params);xjson_free(clear);
    if(status!=XR_XIR_OK&&status!=XR_XIR_UNRESOLVED){server->exit_received=true;lsp_log("Source diagnostic publication failed: status %u",(unsigned)status);}
}
