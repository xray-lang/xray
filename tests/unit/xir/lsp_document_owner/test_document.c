/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_document_owner.h"
#include "app/lsp/xlsp_document_store.h"
#include "app/lsp/xlsp_source_open.h"
#include "app/lsp/xlsp_cache.h"
#include "app/lsp/xlsp_imports.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d:%s\n",__LINE__,#c); exit(2); } } while (0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"
#pragma push_macro("xr_free")
#undef xr_free
#define xr_free(p) lsp_json_free(p)
#include "app/lsp/xlsp_protocol_output.c"
#pragma pop_macro("xr_free")
typedef struct Block { void *pointer; size_t bytes; } Block;
static Block blocks[128];
static size_t app_attempts, app_fail=SIZE_MAX, app_live, app_bytes, rollbacks;
XR_FUNC void baseline_destroy(XrLspDocument *);
XR_FUNC void baseline_clear_diagnostics(XrLspServer *);
XR_FUNC void *document_test_malloc(size_t bytes) {
    if(app_attempts++==app_fail)return NULL;
    void *p=sdk_fixture_malloc(bytes);if(!p)return NULL;
    for(size_t i=0;i<128;++i)if(!blocks[i].pointer){blocks[i]=(Block){p,bytes};++app_live;app_bytes+=bytes;return p;}
    CHECK(false);return NULL;
}
XR_FUNC void *document_test_calloc(size_t count,size_t bytes) {
    CHECK(!count||bytes<=SIZE_MAX/count);void *p=document_test_malloc(count*bytes);
    if(p)memset(p,0,count*bytes);return p;
}
XR_FUNC void document_test_free(void *p) {
    if(!p)return;
    for(size_t i=0;i<128;++i)if(blocks[i].pointer==p){--app_live;app_bytes-=blocks[i].bytes;blocks[i]=(Block){0};sdk_fixture_free(p);return;}
    CHECK(false);
}
XR_FUNC void *document_test_realloc(void *p,size_t bytes) {
    if(!p)return document_test_malloc(bytes);
    if(app_attempts++==app_fail)return NULL;
    for(size_t i=0;i<128;++i)if(blocks[i].pointer==p){
        void *next=xr_realloc(p,bytes);if(!next)return NULL;
        app_bytes-=blocks[i].bytes;app_bytes+=bytes;blocks[i]=(Block){next,bytes};return next;
    }
    CHECK(false);return NULL;
}
XR_FUNC void lsp_log(const char *format,...) { (void)format; }
static const char root_text[]="import { value } from \"./lib\"\r\nexport fn main()->i64 { /* 😀 */ return value() }\r\n";
static const char lib_text[]="export fn value()->i64 { return 1 }\n";
static const char extra_text[]="export fn extra()->i64 { return 3 }\n";
static XrLspDocument *open_owned(XrLspServer *server,const char *uri,const char *text,int version) {
    size_t n=strlen(uri),m=strlen(text);char *u=document_test_malloc(n+1),*t=document_test_malloc(m+1);CHECK(u&&t);
    memcpy(u,uri,n+1);memcpy(t,text,m+1);XlspSourceDocument input={u,t,n,m,version};
    XrLspDocument *doc=xlsp_document_open(server,&input);CHECK(doc);
    memset(u,'?',n);memset(t,'?',m);document_test_free(u);document_test_free(t);
    CHECK(!strcmp(doc->uri,uri)&&!strcmp(doc->content,text)&&doc->version==version&&doc->source_open&&doc->source_buffer&&doc->syntax_snapshot);
    return doc;
}
static XrJsonValue *json(const char *text){XrJsonValue *p=xjson_parse(text,strlen(text));CHECK(p);return p;}
static void caches(XrLspDocument *doc) {
    CHECK(!doc->cached_imports&&!doc->cached_diagnostics);
    XlspImportInfo *item=document_test_calloc(1,sizeof(*item));CHECK(item);
    item->module_name=document_test_malloc(2);item->import_path=document_test_malloc(2);item->resolved_path=document_test_malloc(2);
    CHECK(item->module_name&&item->import_path&&item->resolved_path);
    memcpy(item->module_name,"a",2);memcpy(item->import_path,"b",2);memcpy(item->resolved_path,"c",2);
    doc->cached_imports=item;doc->cached_diagnostics=xjson_new_object();CHECK(doc->cached_diagnostics);
    doc->imports_content_hash=37;
}
static void rejected_edit(XrLspDocument *doc,XrJsonValue *changes,int version) {
    XrLspDocument before=*doc;XrLspServer server=*doc->server;
    size_t resource=runtime_live,bytes=runtime_bytes,app=app_live,app_size=app_bytes,j=lsp_json_live,jb=lsp_json_bytes;
    CHECK(xlsp_document_apply_changes(doc,changes,version)==XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&before,doc,sizeof(before))&&!memcmp(&server,doc->server,sizeof(server)));
    CHECK(runtime_live==resource&&runtime_bytes==bytes&&app_live==app&&app_bytes==app_size&&lsp_json_live==j&&lsp_json_bytes==jb);++rollbacks;
}
static void edits(XrLspServer *server,XrLspDocument *doc) {
    caches(doc);
    XrJsonValue *bad=json("[{\"text\":\"export fn temporary()->i64 { return 9 }\\n\"},{\"range\":{\"start\":{\"line\":99,\"character\":0},\"end\":{\"line\":99,\"character\":0}},\"text\":\"x\"}]");
    rejected_edit(doc,bad,2);xjson_free(bad);
    XrJsonValue *half=json("[{\"range\":{\"start\":{\"line\":1,\"character\":28},\"end\":{\"line\":1,\"character\":29}},\"text\":\"x\"}]");
    rejected_edit(doc,half,2);xjson_free(half);
    XrJsonValue *good=json("[{\"range\":{\"start\":{\"line\":1,\"character\":40},\"end\":{\"line\":1,\"character\":45}},\"text\":\"value\"},{\"range\":{\"start\":{\"line\":1,\"character\":10},\"end\":{\"line\":1,\"character\":14}},\"text\":\"next\"}]");
    uint64_t revision=server->source_revision;CHECK(xlsp_document_apply_changes(doc,good,2)==XR_XIR_OK);
    CHECK(server->source_revision==revision+1&&doc->version==2&&strstr(doc->content,"fn next()")&&!doc->cached_imports&&!doc->cached_diagnostics);
    CHECK(doc->imports_content_hash==XLSP_CONTENT_HASH_UNINITIALIZED&&doc->source_buffer&&doc->syntax_snapshot);
    rejected_edit(doc,good,2);xjson_free(good);
    XrJsonValue *broken=json("[{\"text\":\"export fn broken( {\\n\"}]");
    CHECK(xlsp_document_apply_changes(doc,broken,3)==XR_XIR_OK);xjson_free(broken);
    CHECK(doc->version==3&&doc->parse_error&&xlsp_source_open_query_status(doc->source_open)!=XR_XIR_OK);
    XrJsonValue *diagnostics=NULL;
    CHECK(xlsp_source_open_diagnostics_json(doc->source_open,doc->source_buffer,doc->syntax_snapshot,&diagnostics)==XR_XIR_OK);
    CHECK(diagnostics&&diagnostics->type==XR_JSON_ARRAY&&diagnostics->as.array.count>0);xjson_free(diagnostics);
}
static void rejected_open(XrLspServer *server,const XlspSourceDocument *input) {
    XrLspServer before=*server;XrLspDocTable table=*server->doc_table;
    size_t n=runtime_live,b=runtime_bytes,a=app_live,ab=app_bytes;
    CHECK(!xlsp_document_open(server,input));
    CHECK(!memcmp(&before,server,sizeof(before))&&!memcmp(&table,server->doc_table,sizeof(table)));
    CHECK(runtime_live==n&&runtime_bytes==b&&app_live==a&&app_bytes==ab);++rollbacks;
}
static void faults(XrLspServer *server,const char *extra) {
    XlspSourceDocument input={extra,extra_text,strlen(extra),sizeof(extra_text)-1,1};
    size_t start=runtime_attempts,app=app_attempts;CHECK(xlsp_document_open(server,&input));
    size_t prefixes=runtime_attempts-start,app_prefixes=app_attempts-app;CHECK(prefixes>100&&app_prefixes==2);
    xlsp_document_close(server,extra);
    const size_t points[]={0,prefixes/2,prefixes-1};
    for(size_t i=0;i<3;++i){runtime_fail_at=runtime_attempts+points[i];rejected_open(server,&input);CHECK(runtime_attempts>runtime_fail_at);runtime_fail_at=SIZE_MAX;}
    for(size_t i=0;i<app_prefixes;++i){app_fail=app_attempts+i;rejected_open(server,&input);CHECK(app_attempts>app_fail);app_fail=SIZE_MAX;}
    CHECK(xlsp_document_open(server,&input));xlsp_document_close(server,extra);
    fprintf(stderr,"PREFIXES Source=%zu focused=3 app=%zu\n",prefixes,app_prefixes);
}
static int run(int argc,char **argv) {
    CHECK(argc==5||argc==6);unsigned mode=(unsigned)strtoul(argv[4],NULL,10);CHECK(mode<6);
    (void)sdk_ledger;(void)sdk_stats;(void)sdk_unlimited;
    XrLspServer server={0};server.initialized=true;server.config.diagnostics_enabled=true;
    server.doc_table=xlsp_document_store_new(mode==3?baseline_destroy:xlsp_document_destroy);CHECK(server.doc_table);
    XrLspDocument *lib=open_owned(&server,argv[2],lib_text,7),*doc=open_owned(&server,argv[1],root_text,1);
    CHECK(server.source_revision==2&&server.doc_table->doc_count==2&&server.source_workspace);
    if(mode==2||mode==4||mode==5){
        server.transport=xlsp_transport_stdio();CHECK(server.transport);
        if(mode==4){baseline_clear_diagnostics(&server);CHECK(false);}
        size_t attempts=lsp_json_calls;
        if(mode==5){CHECK(argc==6);lsp_json_fail=lsp_json_calls+(size_t)strtoull(argv[5],NULL,10);}
        xlsp_clear_all_diagnostics(&server);
        if(mode==5){CHECK(lsp_json_injected&&server.exit_received);lsp_json_fail=SIZE_MAX;}
        else CHECK(!server.exit_received);
        CHECK(!lsp_json_live&&!lsp_json_bytes);
        fprintf(stderr,"CLEAR JSON=%zu failed=%u\n",lsp_json_calls-attempts,server.exit_received?1u:0u);
        xlsp_transport_free(server.transport);server.transport=NULL;
    }
    if(mode==5){
        xlsp_schedule_diagnostics(&server,doc);
        CHECK(server.exit_received&&!server.pending_diag_count&&!lib->diag_pending&&!doc->diag_pending);
        /* The independent queue-lifetime checks below start a fresh event
         * fixture only after terminal failure and admission are verified. */
        server.exit_received=false;
    }
    if(mode==0)edits(&server,doc);
    if(mode==1)faults(&server,argv[3]);
    if(mode==2){
        XlspSourceDocument duplicate={argv[1],root_text,strlen(argv[1]),sizeof(root_text)-1,1};rejected_open(&server,&duplicate);
        XrLspServer before=server;xlsp_document_close(&server,argv[3]);CHECK(!memcmp(&before,&server,sizeof(before)));
        server.source_revision=UINT64_MAX;rejected_open(&server,&duplicate);
        before=server;xlsp_document_close(&server,argv[1]);CHECK(!memcmp(&before,&server,sizeof(before))&&xlsp_document_get(&server,argv[1])==doc);
        XrJsonValue *change=json("[{\"text\":\"export fn other()->i64 { return 1 }\\n\"}]");
        CHECK(xlsp_document_apply_changes(doc,change,2)==XR_XIR_BUDGET);xjson_free(change);server.source_revision=2;
    }
    if(!doc->cached_imports)caches(doc);
    xlsp_schedule_diagnostics(&server,lib);xlsp_schedule_diagnostics(&server,doc);xlsp_schedule_diagnostics(&server,doc);
    CHECK(server.pending_diag_count==2&&lib->diag_pending&&doc->diag_pending);
    uint64_t revision=server.source_revision;xlsp_document_close(&server,argv[1]);
    CHECK(!xlsp_document_get(&server,argv[1])&&server.doc_table->doc_count==1&&server.source_revision==revision+1&&!server.source_workspace);
    bool detached=server.pending_diag_count==1&&server.pending_diag[0]==lib&&lib->diag_pending;
    xlsp_document_store_free(server.doc_table);server.doc_table=NULL;
    bool drained=!server.pending_diag_count;
    document_test_free(server.pending_diag);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!app_live&&!app_bytes);
    fprintf(stderr,"RESULT mode=%u closed_queue_detached=%u table_free_drained=%u rollbacks=%zu compiler_live=%zu json_live=%zu app_live=%zu\n",
        mode,detached?1u:0u,drained?1u:0u,rollbacks,runtime_live,lsp_json_live,app_live);
    return detached&&drained?0:1;
}
int wmain(int argc,wchar_t **wide) {
    XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);
    int result=run(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;
}
