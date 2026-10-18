/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_document_owner.c - Atomic editor transactions and document ownership
 */
#include "xlsp_document_owner.h"
#include "xlsp_source_open.h"
#include "xlsp_document_store.h"
#include "xlsp_cache.h"
#include "xlsp_imports.h"
#include "../../api/xsource_compile.h"
#include "../../base/xmalloc.h"
#include "../../os/os_time.h"
#include <limits.h>
#include <string.h>

static void document_detach_diagnostics(XrLspDocument *doc) {
    XrLspServer *server = doc->server;
    if (server && server->pending_diag) {
        int count = server->pending_diag_count, kept = 0;
        for (int i = 0; i < count; ++i)
            if (server->pending_diag[i] != doc) server->pending_diag[kept++] = server->pending_diag[i];
        for (int i = kept; i < count; ++i) server->pending_diag[i] = NULL;
        server->pending_diag_count = kept;
    }
    doc->diag_pending = false;
    doc->diagnostic_deadline = 0;
}

XR_FUNC void xlsp_document_destroy(XrLspDocument *doc) {
    if (!doc)
        return;
    // Timer entries borrow this document and must disappear before its owners.
    document_detach_diagnostics(doc);
    xlsp_source_syntax_free(doc->syntax_snapshot);
    doc->syntax_snapshot = NULL;
    xlsp_invalidate_import_cache(doc);
    xlsp_free_document_cache(doc);
    xr_arena_destroy(&doc->arena);
    if(doc->source_open)xlsp_source_open_free(doc->source_open);else xr_free(doc->uri);
    if (doc->source_buffer) xlsp_source_buffer_free(doc->source_buffer);
    else { xr_free(doc->content); xr_free(doc->line_offsets); }
    xlsp_source_tokens_free(doc->source_tokens);
    xr_free(doc);
}

/* Collect one exact proposed editor state. Existing input owners remain alive
 * until both the Source transaction and the document-table commit finish. */
static XrXirStatus document_prepare(XrLspServer *server,const XlspSourceDocument *input,
    XrLspDocument *replaced,XrCompileResources *resources,XlspSourceOpen **out) {
    if(!server||!server->doc_table||!input||!input->uri||!input->text||!out||*out)return XR_XIR_BAD_STRUCTURE;
    XrLspDocTable *table=server->doc_table;
    if(table->doc_count<0||(!replaced&&table->doc_count==INT_MAX))return XR_XIR_BAD_STRUCTURE;
    size_t count=(size_t)table->doc_count+(replaced?0u:1u),used=0,entry=SIZE_MAX;
    if(!count||count>SIZE_MAX/sizeof(XlspSourceDocument))return XR_XIR_BUDGET;
    XlspSourceDocument *documents=NULL;
    XrCompileResourceStatus allocated=xr_compile_resources_calloc(resources,count,sizeof(*documents),(void **)&documents);
    if(allocated!=XR_COMPILE_RESOURCE_OK)return allocated==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrXirStatus status=XR_XIR_OK;
    if(table->bucket_count<=0||!table->buckets){status=XR_XIR_BAD_STRUCTURE;goto done;}
    if(xr_compile_resources_work(resources,(uint64_t)table->bucket_count)!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
    for(int i=0;i<table->bucket_count;++i)for(XrLspDocBucket *bucket=table->buckets[i];bucket;bucket=bucket->next) {
        XrLspDocument *doc=bucket->doc;
        if(used==count||!doc||!doc->content||!doc->uri){status=XR_XIR_BAD_STRUCTURE;goto done;}
        size_t length=0;
        do {
            if(xr_compile_resources_work(resources,1)!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
            if(!doc->uri[length])break;
            if(length==SIZE_MAX-1){status=XR_XIR_BUDGET;goto done;}++length;
        } while(true);
        if(xr_compile_resources_work(resources,sizeof(*documents))!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
        if(doc==replaced){entry=used;documents[used++]=*input;}
        else documents[used++]=(XlspSourceDocument){doc->uri,doc->content,length,doc->length,doc->version};
    }
    if(!replaced){entry=used;documents[used++]=*input;}
    if(used!=count||entry==SIZE_MAX){status=XR_XIR_BAD_STRUCTURE;goto done;}
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    status=xlsp_source_open_prepare(&context,documents,count,entry,out);
done:
    xr_compile_resources_free(documents);return status;
}
static void document_commit_owner(XrLspDocument *doc,XlspSourceOpen *fresh) {
    ++doc->server->source_revision;
    XlspSourceOpen *old_open=doc->source_open;XlspSourceBuffer *old_buffer=doc->source_buffer;
    XlspSyntaxSnapshot *old_syntax=doc->syntax_snapshot;char *old_uri=doc->uri,*old_text=doc->content;
    uint32_t *old_lines=doc->line_offsets;
    doc->source_open=fresh;doc->uri=(char *)xlsp_source_open_uri(fresh);
    doc->source_buffer=xlsp_source_open_take_buffer(fresh);doc->syntax_snapshot=xlsp_source_open_take_syntax(fresh);
    doc->ast=(AstNode *)xlsp_source_syntax_ast(doc->syntax_snapshot);
    doc->content=(char *)xlsp_source_buffer_text(doc->source_buffer);doc->length=xlsp_source_buffer_length(doc->source_buffer);
    doc->version=xlsp_source_buffer_version(doc->source_buffer);
    doc->line_offsets=(uint32_t *)xlsp_source_buffer_lines(doc->source_buffer,&doc->line_count);
    doc->parse_error=xlsp_source_syntax_status(doc->syntax_snapshot)==XR_PARSE_RECOVERED||xlsp_source_open_query_status(fresh)!=XR_XIR_OK;
    doc->dirty=true;doc->content_hash=XLSP_CONTENT_HASH_UNINITIALIZED;
    XlspSourceWorkspace *old_workspace=doc->server->source_workspace;
    doc->server->source_workspace=xlsp_source_open_take_workspace(fresh);
    xlsp_invalidate_import_cache(doc);xlsp_free_document_cache(doc);
    xlsp_source_syntax_free(old_syntax);
    if(old_buffer)xlsp_source_buffer_free(old_buffer);else {xr_free(old_text);xr_free(old_lines);}
    if(old_open)xlsp_source_open_free(old_open);else xr_free(old_uri);
    xlsp_source_workspace_free(old_workspace);
}
XR_FUNC XrLspDocument *xlsp_document_open(XrLspServer *server,const XlspSourceDocument *input) {
    if(!server||!server->doc_table||!input||!input->uri||!input->text||
        input->uri_length==SIZE_MAX||server->source_revision==UINT64_MAX)return NULL;
    XrCompileResourceLimits limits=xr_source_compile_default_resource_limits();
    XrCompileResources *resources=NULL;XlspSourceOpen *fresh=NULL;XrLspDocument *doc=NULL;
    XlspDocumentInsertion insertion={0};
    XrCompileResourceStatus created=xr_compile_resources_new(&limits,&resources);
    if(created!=XR_COMPILE_RESOURCE_OK)return NULL;
    XrXirStatus status=document_prepare(server,input,NULL,resources,&fresh);
    if(status!=XR_XIR_OK)goto done;
    doc=xr_calloc(1,sizeof(*doc));if(!doc)goto done;
    doc->server=server;
    if(!xlsp_document_store_prepare(server->doc_table,xlsp_source_open_uri(fresh),doc,&insertion))goto done;
    /* The event loop is synchronous: after prepare there is no reentrant
     * operation, allocation or fallible computation before this commit. */
    if(!xlsp_document_store_commit(&insertion))goto done;
    document_commit_owner(doc,fresh);fresh=NULL;
    xr_compile_resources_release(resources);return doc;
done:
    xlsp_document_store_abort(&insertion);xlsp_source_open_free(fresh);xr_free(doc);
    xr_compile_resources_release(resources);return NULL;
}
XR_FUNC XrXirStatus xlsp_document_apply_changes(XrLspDocument *doc,const XrJsonValue *changes,int version) {
    if(!doc||!doc->server||!doc->content||!doc->uri)return XR_XIR_BAD_STRUCTURE;
    if(doc->server->source_revision==UINT64_MAX)return XR_XIR_BUDGET;
    XrCompileResourceLimits limits=xr_source_compile_default_resource_limits();
    XrCompileResources *resources=NULL;XlspSourceBuffer *buffer=NULL;XlspSourceOpen *fresh=NULL;
    XrCompileResourceStatus created=xr_compile_resources_new(&limits,&resources);
    if(created!=XR_COMPILE_RESOURCE_OK)return created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrXirStatus status=xlsp_source_buffer_edit_json(resources,doc->content,doc->length,doc->version,changes,version,&buffer);
    if(status!=XR_XIR_OK)goto done;
    size_t length=0;
    do {if(xr_compile_resources_work(resources,1)!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
        if(!doc->uri[length])break;if(length==SIZE_MAX-1){status=XR_XIR_BUDGET;goto done;}++length;} while(true);
    XlspSourceDocument input={doc->uri,xlsp_source_buffer_text(buffer),length,xlsp_source_buffer_length(buffer),version};
    status=document_prepare(doc->server,&input,doc,resources,&fresh);
    if(status==XR_XIR_OK){document_commit_owner(doc,fresh);fresh=NULL;}
done:
    xlsp_source_open_free(fresh);xlsp_source_buffer_free(buffer);xr_compile_resources_release(resources);return status;
}

XR_FUNC void xlsp_document_close(XrLspServer *server, const char *uri) {
    if(!server||!uri||!server->doc_table||server->source_revision==UINT64_MAX||
       !xlsp_document_store_get(server->doc_table,uri))return;
    ++server->source_revision;
    lsp_log("Closed document: %s", uri);
    xlsp_document_store_remove(server->doc_table, uri);
    xlsp_source_workspace_free(server->source_workspace);server->source_workspace=NULL;
}

XR_FUNC XrLspDocument *xlsp_document_get(XrLspServer *server, const char *uri) {
    return server?xlsp_document_store_get(server->doc_table, uri):NULL;
}

XR_FUNC void xlsp_schedule_diagnostics(XrLspServer *server, XrLspDocument *doc) {
    if (!server->config.diagnostics_enabled)
        return;

    uint64_t now = xr_time_monotonic_ms();
    doc->last_change_time = now;
    int debounce_ms = server->config.diagnostic_debounce_ms > 0
                          ? server->config.diagnostic_debounce_ms
                          : 300;
    doc->diagnostic_deadline = now + (uint64_t) debounce_ms;

    // O(1) dedup via per-document flag
    if (doc->diag_pending)
        return;

    // Grow queue if needed
    if (server->pending_diag_count >= server->pending_diag_capacity) {
        int new_cap = server->pending_diag_capacity ? server->pending_diag_capacity * 2 : 16;
        XrLspDocument **tmp =
            xr_realloc(server->pending_diag, (size_t) new_cap * sizeof(XrLspDocument *));
        if (!tmp)
            return;
        server->pending_diag = tmp;
        server->pending_diag_capacity = new_cap;
    }
    server->pending_diag[server->pending_diag_count++] = doc;
    doc->diag_pending = true;
}
