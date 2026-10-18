/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#define xr_compile_resources_new publication_resource_new_actual
#include "publication_fixture.h"
#undef xr_compile_resources_new
#include "app/lsp/xlsp_diagnostic_owner.h"
static XrCompileResources *publication_ledgers[4];
static size_t publication_ledger_count;
static XrCompileResourceLimits publication_limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
static bool publication_override;
XR_FUNC void publication_transport_fault(size_t);
XR_FUNC size_t publication_transport_writes(void);
XR_FUNC XrCompileResourceStatus xr_compile_resources_new(const XrCompileResourceLimits *limits,XrCompileResources **out) {
    CHECK(limits&&limits->allocated_bytes==UINT64_C(64)*1024*1024&&limits->live_bytes==UINT64_C(8)*1024*1024&&limits->work==UINT64_C(128000000));
    XrCompileResourceStatus status=publication_resource_new_actual(publication_override?&publication_limits:limits,out);
    if(status==XR_COMPILE_RESOURCE_OK){CHECK(publication_ledger_count<4);CHECK(xr_compile_resources_retain(*out)==XR_COMPILE_RESOURCE_OK);publication_ledgers[publication_ledger_count++]=*out;}
    return status;
}
typedef struct PublicationCase {
    const char *root,*library,*peer,*domain;unsigned mode;size_t fault;
    bool expected_failure;size_t source_start,json_start,app_start,wire_start;
} PublicationCase;
static const char publication_root[]="import {value} from \"./lib\"\nexport fn main()->i64{return value()}\n";
static const char publication_bad[]="/* \xf0\x9f\x98\x80 */ var *** = ;\n";
static XrLspDocument *publication_open(XrLspServer *server,const char *uri,const char *text,int version) {
    char copied_uri[4096],copied_text[512];CHECK(strlen(uri)<sizeof(copied_uri)&&strlen(text)<sizeof(copied_text));
    strcpy(copied_uri,uri);strcpy(copied_text,text);
    XlspSourceDocument input={copied_uri,copied_text,strlen(copied_uri),strlen(copied_text),version};
    XrLspDocument *doc=xlsp_document_open(server,&input);
    memset(copied_uri,'?',sizeof(copied_uri));memset(copied_text,'?',sizeof(copied_text));memset(&input,0,sizeof(input));return doc;
}
static void publication_arm(PublicationCase *input) {
    input->source_start=runtime_attempts;input->json_start=lsp_json_calls;input->app_start=app_attempts;input->wire_start=publication_transport_writes();
    if(!strcmp(input->domain,"source"))runtime_fail_at=runtime_attempts+input->fault;
    if(!strcmp(input->domain,"json"))lsp_json_fail=lsp_json_calls+input->fault;
    if(!strcmp(input->domain,"app"))app_fail=app_attempts+input->fault;
}
static bool publication_switch(XrLspServer *server,PublicationCase *input) {
    XrLspDocument *doc=publication_open(server,input->root,publication_root,1);
    if(!doc)return true;
    xlsp_publish_diagnostics(server,doc);if(server->exit_received)return true;
    const XrJsonValue *clear=xlsp_source_open_published_clear(doc->source_open);CHECK(clear);
    XlspSourceOpen *old=doc->source_open;uintptr_t old_address=(uintptr_t)old,clear_address=(uintptr_t)clear;
    const char *changes_text=input->mode==8?"[{\"text\":\"import {value} from \\\"./semantic\\\"\\nexport fn main()->i64{return value()}\\n\"}]":
        "[{\"text\":\"export fn main()->i64{return 1}\\n\"}]";
    XrJsonValue *changes=xjson_parse(changes_text,strlen(changes_text));CHECK(changes);
    publication_arm(input);
    XrXirStatus changed=xlsp_document_apply_changes(doc,changes,2);xjson_free(changes);
    if(changed!=XR_XIR_OK){CHECK(changed==XR_XIR_OUT_OF_MEMORY||changed==XR_XIR_BUDGET);CHECK(doc->version==1&&doc->source_open==old&&xlsp_source_open_published_clear(old)==clear);return true;}
    CHECK(doc->version==2&&(uintptr_t)doc->source_open!=old_address&&xlsp_source_open_published_clear(doc->source_open)==clear);
    if(input->mode==7)publication_transport_fault(1);
    xlsp_publish_diagnostics(server,doc);
    if(server->exit_received){CHECK(xlsp_source_open_published_clear(doc->source_open)==clear);return true;}
    if(input->mode==8){CHECK(!xlsp_source_open_published_clear(doc->source_open)&&publication_transport_writes()-input->wire_start==1);return false;}
    CHECK(xlsp_source_open_published_clear(doc->source_open)&&(uintptr_t)xlsp_source_open_published_clear(doc->source_open)!=clear_address);
    CHECK(publication_transport_writes()-input->wire_start==2);return false;
}
static bool publication_cleanup_cases(XrLspServer *server,PublicationCase *input) {
    XrLspDocument *doc=publication_open(server,input->root,publication_root,1);if(!doc)return true;
    xlsp_publish_diagnostics(server,doc);if(server->exit_received)return true;
    const XrJsonValue *clear=xlsp_source_open_published_clear(doc->source_open);CHECK(clear);
    XrLspDocument *peer=NULL;
    if(input->mode==11){peer=publication_open(server,input->peer,publication_root,1);if(!peer)return true;}
    publication_arm(input);
    if(input->mode==9)xlsp_document_close(server,input->root);
    else if(input->mode==10)xlsp_clear_all_diagnostics(server);
    else xlsp_publish_diagnostics(server,peer);
    size_t writes=publication_transport_writes()-input->wire_start;
    if(server->exit_received){
        CHECK(xlsp_document_get(server,input->root)==doc);
        if(input->mode==10&&writes)CHECK(writes==1&&!xlsp_source_open_published_clear(doc->source_open));
        else CHECK(!writes&&xlsp_source_open_published_clear(doc->source_open)==clear);
        return true;
    }
    if(input->mode==9)CHECK(writes==1&&!xlsp_document_get(server,input->root));
    else if(input->mode==10)CHECK(writes==2&&!xlsp_source_open_published_clear(doc->source_open));
    else CHECK(writes==1&&!xlsp_source_open_published_clear(doc->source_open)&&xlsp_source_open_published_clear(peer->source_open));
    return false;
}
static bool publication_owned_cases(XrLspServer *server,PublicationCase *input) {
    if(input->mode==4||input->mode==5)CHECK(publication_open(server,input->library,publication_bad,7));
    XrLspDocument *doc=publication_open(server,input->root,publication_root,1);CHECK(doc);
    if(input->mode==5) {
        XrLspDocument *lib=xlsp_document_get(server,input->library);CHECK(lib);
        const char change_text[]="[{\"text\":\"/* \xf0\x9f\x98\x80 */ var *** = ;\\n\"}]";
        XrJsonValue *change=xjson_parse(change_text,sizeof(change_text)-1);CHECK(change);
        CHECK(xlsp_document_apply_changes(lib,change,8)==XR_XIR_OK);xjson_free(change);
        size_t calls=lsp_json_calls,writes=publication_transport_writes();xlsp_publish_diagnostics(server,doc);
        CHECK(lsp_json_calls==calls&&publication_transport_writes()==writes&&!server->exit_received&&!xlsp_source_open_published_clear(doc->source_open));
        xlsp_publish_diagnostics(server,lib);CHECK(xlsp_source_open_published_clear(lib->source_open));return false;
    }
    xlsp_publish_diagnostics(server,doc);CHECK(!server->exit_received&&xlsp_source_open_published_clear(doc->source_open));
    if(input->mode==2) {
        XrLspDocument *peer=publication_open(server,input->peer,publication_root,1);CHECK(peer);
        xlsp_publish_diagnostics(server,peer);CHECK(!server->exit_received);
        CHECK(!xlsp_source_open_published_clear(doc->source_open)&&xlsp_source_open_published_clear(peer->source_open));
        size_t writes=publication_transport_writes();xlsp_document_close(server,input->root);CHECK(publication_transport_writes()==writes);
        xlsp_document_close(server,input->peer);CHECK(publication_transport_writes()==writes+1&&!server->doc_table->doc_count);
    }else if(input->mode==3) {
        xlsp_clear_all_diagnostics(server);CHECK(!server->exit_received&&!xlsp_source_open_published_clear(doc->source_open));
        xlsp_clear_all_diagnostics(server);CHECK(!server->exit_received);
    }else if(input->mode==4) {
        xlsp_document_close(server,input->library);CHECK(!server->exit_received&&!xlsp_source_open_published_clear(doc->source_open));
        size_t writes=publication_transport_writes();xlsp_publish_diagnostics(server,doc);CHECK(publication_transport_writes()==writes);
    }
    return false;
}
static void publication_pair_arguments(XrLspServer *server) {
    XrLspDocument invalid={.server=server};
    CHECK(xlsp_diagnostic_document_close(server,&invalid)==XR_XIR_BAD_STRUCTURE);
    CHECK(xlsp_diagnostic_document_close(server,NULL)==XR_XIR_BAD_STRUCTURE);
    server->next_request_id=17;size_t writes=publication_transport_writes();
    XrJsonValue *shared=xjson_new_object();CHECK(shared);
    CHECK(xlsp_protocol_send_notification_pair(server,"method",shared,shared)==XR_XIR_BAD_STRUCTURE);
    XrJsonValue *second=xjson_new_object();CHECK(second);
    CHECK(xlsp_protocol_send_notification_pair(server,"method",NULL,second)==XR_XIR_BAD_STRUCTURE);
    CHECK(!server->exit_received&&server->next_request_id==17&&publication_transport_writes()==writes);
}
static int publication_main(int argc,char **argv) {
    CHECK(argc==5||argc==8||argc==9);(void)sdk_ledger;(void)sdk_stats;(void)sdk_unlimited;
    PublicationCase input={.root=argv[1],.library=argv[2],.peer=argv[3],.mode=(unsigned)strtoul(argv[4],NULL,10),.domain="none",.fault=SIZE_MAX};CHECK(input.mode<12);
    if(argc==8){CHECK(!strcmp(argv[5],"--fault")&&(input.mode<2||input.mode>=8));input.domain=argv[6];input.fault=(size_t)strtoull(argv[7],NULL,10);input.expected_failure=true;}
    if(argc==9){CHECK(!strcmp(argv[5],"--limits")&&(input.mode<2||input.mode>=8));unsigned axis=(unsigned)strtoul(argv[6],NULL,10);CHECK(axis<3);uint64_t value=strtoull(argv[7],NULL,10);
        if(!axis)publication_limits.allocated_bytes=value;else if(axis==1)publication_limits.live_bytes=value;else publication_limits.work=value;
        publication_override=true;input.expected_failure=strtoul(argv[8],NULL,10)!=0;}
    XrLspServer server={0};server.initialized=true;server.config.diagnostics_enabled=true;
    server.doc_table=xlsp_document_store_new(xlsp_document_destroy);CHECK(server.doc_table);server.transport=xlsp_transport_stdio();CHECK(server.transport);
    bool failed=false;
    if(input.mode==1||input.mode>=7){input.source_start=runtime_attempts;input.json_start=lsp_json_calls;input.app_start=app_attempts;input.wire_start=publication_transport_writes();}
    else publication_arm(&input);
    if(input.mode==0){XrLspDocument *doc=publication_open(&server,input.root,publication_root,1);if(!doc)failed=true;else{xlsp_publish_diagnostics(&server,doc);failed=server.exit_received;if(!failed)CHECK(xlsp_source_open_published_clear(doc->source_open));}}
    else if(input.mode==1||input.mode==7||input.mode==8)failed=publication_switch(&server,&input);
    else if(input.mode>=9)failed=publication_cleanup_cases(&server,&input);
    else if(input.mode==6)publication_pair_arguments(&server);
    else failed=publication_owned_cases(&server,&input);
    bool source_hit=runtime_fail_at!=SIZE_MAX&&runtime_attempts>runtime_fail_at,json_hit=lsp_json_injected,app_hit=app_fail!=SIZE_MAX&&app_attempts>app_fail;
    size_t source_count=runtime_attempts-input.source_start,json_count=lsp_json_calls-input.json_start,app_count=app_attempts-input.app_start,writes=publication_transport_writes()-input.wire_start;
    if(input.mode==7)CHECK(failed&&server.exit_received&&writes==2);
    else CHECK(failed==input.expected_failure);
    if(argc==8){CHECK(source_hit||json_hit||app_hit);CHECK(input.mode==10?writes<=1:!writes);}
    XrCompileResourceStats maximum={0};
    for(size_t i=0;i<publication_ledger_count;++i){XrCompileResourceStats stats=sdk_stats(publication_ledgers[i]);if(stats.allocated_bytes>maximum.allocated_bytes)maximum.allocated_bytes=stats.allocated_bytes;if(stats.peak_bytes>maximum.peak_bytes)maximum.peak_bytes=stats.peak_bytes;if(stats.work>maximum.work)maximum.work=stats.work;}
    runtime_fail_at=SIZE_MAX;lsp_json_fail=SIZE_MAX;app_fail=SIZE_MAX;server.exit_received=true;
    xlsp_document_store_free(server.doc_table);xlsp_source_workspace_free(server.source_workspace);
    xlsp_transport_free(server.transport);document_test_free(server.pending_diag);
    for(size_t i=0;i<publication_ledger_count;++i)xr_compile_resources_release(publication_ledgers[i]);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!app_live&&!app_bytes);
    fprintf(stderr,"PUBLICATION mode=%u source=%zu json=%zu app=%zu source_hit=%u json_hit=%u app_hit=%u failed=%u write_calls=%zu allocated=%llu peak=%llu work=%llu physical0/0 JSON0/0 app0/0\n",
        input.mode,source_count,json_count,app_count,source_hit?1u:0u,json_hit?1u:0u,app_hit?1u:0u,failed?1u:0u,writes,
        (unsigned long long)maximum.allocated_bytes,(unsigned long long)maximum.peak_bytes,(unsigned long long)maximum.work);
    return 0;
}
int wmain(int argc,wchar_t **wide){XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);int result=publication_main(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;}
