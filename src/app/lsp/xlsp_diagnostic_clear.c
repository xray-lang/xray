/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_diagnostic_clear.c - Publish complete empty diagnostics with one owner
 */
#include "xlsp_diagnostic_internal.h"

XR_FUNC void xlsp_clear_all_diagnostics(XrLspServer *server) {
    if(!server||!server->doc_table||server->exit_received)return;
    XrLspDocTable *table=server->doc_table;
    if(table->bucket_count<=0||!table->buckets){server->exit_received=true;return;}
    /* Closed targets have no document bucket of their own. Their exact clear
     * params are owned by the Source publication which last wrote that URI. */
    for(int i=0;i<table->bucket_count;++i)for(XrLspDocBucket *bucket=table->buckets[i];bucket;bucket=bucket->next) {
        XrLspDocument *doc=bucket->doc;if(!doc){server->exit_received=true;return;}
        const XrJsonValue *clear=xlsp_source_open_published_clear(doc->source_open);
        if(!clear)continue;
        const XrJsonValue *uri=diagnostic_uri(clear);
        if(!uri){server->exit_received=true;return;}
        if(xlsp_document_store_get(table,uri->as.string))continue;
        XrJsonValue *params=xjson_clone((XrJsonValue *)clear);
        if(!params){server->exit_received=true;return;}
        if(xlsp_protocol_send_notification(server,"textDocument/publishDiagnostics",params)!=XR_XIR_OK){server->exit_received=true;return;}
        XrJsonValue *empty=NULL;xlsp_source_open_publication_commit(doc->source_open,&empty);
    }
    for(int i=0;i<table->bucket_count;++i)for(XrLspDocBucket *bucket=table->buckets[i];bucket;bucket=bucket->next) {
        XrLspDocument *doc=bucket->doc;
        if(!doc->uri){server->exit_received=true;return;}
        XrJsonValue uri={.type=XR_JSON_STRING,.string_len=strlen(doc->uri),.as.string=doc->uri};
        XrJsonValue *params=diagnostic_clear_new(&uri);
        if(!params){server->exit_received=true;return;}
        if(xlsp_protocol_send_notification(server,"textDocument/publishDiagnostics",params)!=XR_XIR_OK){server->exit_received=true;return;}
        diagnostic_evict(server,&uri);
    }
}

XR_FUNC XrXirStatus xlsp_diagnostic_document_close(XrLspServer *server,XrLspDocument *doc) {
    if(!server||!doc||doc->server!=server||!doc->uri)return XR_XIR_BAD_STRUCTURE;
    if(server->exit_received||!doc->source_open)return XR_XIR_OK;
    XrLspDocTable *table=server->doc_table;
    XrJsonValue uri={.type=XR_JSON_STRING,.string_len=strlen(doc->uri),.as.string=doc->uri};
    XrXirStatus status=diagnostic_survey(server,doc->source_open,&uri);
    if(status!=XR_XIR_OK)return status;
    XlspSourceOpen *owners[2]={0};XrJsonValue *params[2]={0};unsigned count=0;
    for(int i=0;i<table->bucket_count;++i)for(XrLspDocBucket *bucket=table->buckets[i];bucket;bucket=bucket->next) {
        XlspSourceOpen *open=bucket->doc->source_open;const XrJsonValue *clear=xlsp_source_open_published_clear(open);
        if(!clear||(bucket->doc!=doc&&!diagnostic_equal(diagnostic_uri(clear),&uri)))continue;
        if(count==2){status=XR_XIR_BAD_STRUCTURE;goto done;}
        params[count]=xjson_clone((XrJsonValue *)clear);
        if(!params[count]){status=XR_XIR_OUT_OF_MEMORY;goto done;}owners[count++]=open;
    }
    if(count) {
        status=xlsp_protocol_send_notification_pair(server,"textDocument/publishDiagnostics",params[0],params[1]);
        params[0]=params[1]=NULL;
        if(status==XR_XIR_OK)for(unsigned i=0;i<count;++i){XrJsonValue *empty=NULL;xlsp_source_open_publication_commit(owners[i],&empty);}
    }
done:
    xjson_free(params[0]);xjson_free(params[1]);return status;
}
