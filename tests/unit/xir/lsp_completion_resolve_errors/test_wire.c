/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "test_allocator.h"
#include "app/lsp/xlsp_handlers_textdoc.h"
#include "app/lsp/xlsp_handlers_lifecycle.h"
#include "app/lsp/xlsp_method_table.h"
#include "app/lsp/xlsp_protocol_output.h"
#include "app/lsp/xlsp_utils.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"resolve-wire:%d:%s\n",__LINE__,#c);exit(2);}}while(0)
#define LSP_ERROR_REQUEST_CANCELLED -32800
static bool cancelled;
static size_t cancellation_reads;
static volatile unsigned shutdown_calls,format_calls,other_calls;
static bool pending_request_is_cancelled(XrLspServer *server,const XlspRequestId *id) {
    CHECK(server&&id);++cancellation_reads;return cancelled;
}
static void fixture_log(const char *format,...) {(void)format;}
#define lsp_log(...) fixture_log(__VA_ARGS__)
XR_FUNC XrJsonValue *xlsp_handle_lc_shutdown(XrLspServer *server,XrJsonValue *params) {
    (void)server;(void)params;++shutdown_calls;return NULL;
}
XR_FUNC XrJsonValue *xlsp_handle_td_formatting(XrLspServer *server,XrJsonValue *params) {
    (void)server;(void)params;++format_calls;return NULL;
}
static XrJsonValue *other_handler(XrLspServer *server,XrJsonValue *params) {
    (void)server;(void)params;++other_calls;return NULL;
}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#undef xr_strdup
#define xr_malloc clone_test_malloc
#define xr_calloc clone_test_calloc
#define xr_realloc clone_test_realloc
#define xr_free clone_test_free
#define xr_strdup clone_test_strdup
#include "app/lsp/xlsp_protocol_message.c"
#include "app/lsp/xlsp_protocol_output.c"
#include "app/lsp/xlsp_transport.c"
#include "send.inc.c"
#ifdef ERRORS_BEFORE
#include "before_resolve.inc.c"
#include "before_finish.inc.c"
#else
#include "resolve.inc.c"
#include "finish.inc.c"
#endif
static XrJsonValue *input(unsigned mode) {
    const char *text="{\"label\":\"same\",\"documentation\":\"owned\",\"data\":{\"uri\":\"file:///closed.xr\",\"opaque\":[1,1.0,null]}}";
    if(mode==1)text="{}";
    if(mode==2)text="{\"label\":\"left\",\"label\":\"right\"}";
    if(mode==3)text="{\"label\":1}";
    if(mode==4)text="[]";
    if(mode==5)return NULL;
    if(mode==6)text="{\"label\\u0000suffix\":\"bad key\"}";
    if(mode==7)text="{\"label\":\"\",\"data\":{\"same\":1,\"same\":2}}";
    if(mode==9)text="{}";
    if(mode==14)text="{\"label\":\"a\\u0000b\\ud83d\\ude00\",\"detail\":\"owned\"}";
    XrJsonValue *value=xjson_parse(text,strlen(text));CHECK(value);return value;
}
int main(int argc,char **argv) {
    CHECK(argc==3);size_t fault=(size_t)strtoull(argv[1],NULL,10);unsigned mode=(unsigned)strtoul(argv[2],NULL,10);CHECK(mode<15);
    clone_fault(SIZE_MAX);XrLspServer server={0};server.initialized=true;server.source_revision=991;
    server.next_request_id=71;server.transport=xlsp_transport_stdio();CHECK(server.transport);
    XrJsonValue *params=input(mode);XlspRequestId id={.kind=XLSP_ID_STRING,.as.string="a\0b\xf0\x9f\x98\x80",.string_length=7};
    if(mode%2)id=(XlspRequestId){.kind=XLSP_ID_NUMBER,.as.number=INT32_MAX};
    XrLspServer saved=server;cancelled=mode==8||mode==9;LspRequestHandler handler=xlsp_handle_td_completion_resolve;
    clone_fault(fault);XrJsonValue *result=NULL;
    if(mode<10||mode==14)result=xlsp_handle_td_completion_resolve(&server,params);
    else if(mode==10){handler=other_handler;server.source_failure.stage=8;server.source_failure.status=XR_XIR_BAD_STRUCTURE;}
    else if(mode==11){handler=xlsp_handle_lc_shutdown;server.source_failure.stage=18;server.source_failure.status=XR_XIR_BAD_STRUCTURE;}
    else if(mode==12){handler=xlsp_handle_td_formatting;server.formatting_failure.stage=1;server.formatting_failure.status=XR_XIR_BUDGET;}
    else handler=other_handler;
    CloneStats after_handler=clone_stats();bool result_present=result!=NULL;
    /* Destroy the producer before publishing its independently owned result. */
    xjson_free(params);params=NULL;finish_request_handler(&server,&id,handler,result);
    CloneStats observed=clone_stats();CHECK(cancellation_reads==1);
    CHECK(!shutdown_calls&&!format_calls&&!other_calls);
    saved.source_failure=server.source_failure;saved.formatting_failure=server.formatting_failure;saved.exit_received=server.exit_received;
    CHECK(!memcmp(&saved,&server,sizeof(server)));
    bool exited=server.exit_received;unsigned stage=server.source_failure.stage,status=server.source_failure.status;
    clone_fault(SIZE_MAX);xlsp_transport_free(server.transport);CHECK(!clone_stats().live&&!clone_stats().bytes);
    fprintf(stderr,"RESOLVE_WIRE mode=%u sites=%zu handler_sites=%zu injected=%u exit=%u stage=%u status=%u result=%u JSON_and_transport0/0 dispatch_policy_actual=1 cancellation_spy=1\n",
        mode,observed.attempts,after_handler.attempts,observed.injected?1u:0u,exited?1u:0u,stage,status,result_present?1u:0u);
#ifdef ERRORS_BEFORE
    CHECK(mode==0&&fault==0&&observed.injected&&!exited&&!result_present&&!stage);
    fputs("BUG resolve_clone_OOM_published_success_NULL=1\n",stderr);return 1;
#else
    return 0;
#endif
}
