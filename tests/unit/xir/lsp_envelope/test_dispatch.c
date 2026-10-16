/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#define main envelope_component_main
#include "test_envelope.c"
#undef main
#include "app/lsp/xlsp_method_table.h"
#include "app/lsp/xlsp_lifecycle_state.h"
#include "app/lsp/xlsp_pending_requests.h"
#include "app/lsp/xlsp_request_cancel.h"
#include "app/lsp/xlsp_index_pool.h"
#include "app/lsp/xlsp_handlers_textdoc.h"
#include "base/xhash.h"
static void fixture_log(const char *format,...) {(void)format;}
#define lsp_log(...) fixture_log(__VA_ARGS__)
#define LSP_ERROR_REQUEST_CANCELLED -32800
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(n) id_malloc(n)
#define xr_free(p) id_free(p)
#include "app/lsp/xlsp_method_table.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
#include "app/lsp/xlsp_pending_requests.c"
#include "app/lsp/xlsp_request_cancel.c"
#include "app/lsp/xlsp_lifecycle_state.c"
static unsigned request_calls,notification_calls,cancel_calls,index_cancel_calls,error_calls,response_calls,source_calls,format_calls;
static volatile int fixture_source_mode=-1;
static uint64_t cancelled_task;
static XrJsonValue *wire;
XR_FUNC bool xlsp_async_cancel_task(XrLspAsync *async,uint64_t task_id) {(void)async;++cancel_calls;cancelled_task=task_id;return true;}
XR_FUNC void xlsp_index_pool_cancel_all(XrLspIndexPool *pool) {(void)pool;++index_cancel_calls;}
XR_FUNC XrJsonValue *xlsp_handle_td_formatting(XrLspServer *server,XrJsonValue *params) {(void)server;(void)params;++format_calls;return NULL;}
static XrJsonValue *probe_request(XrLspServer *server,XrJsonValue *params) {(void)server;(void)params;++request_calls;return xjson_new_number(42);}
static void probe_notification(XrLspServer *server,XrJsonValue *params) {(void)server;(void)params;++notification_calls;}
static const LspMethodEntry methods[]={
    {"probe/request",probe_request,NULL},{"probe/notice",NULL,probe_notification},
    {"shutdown",xlsp_handle_lc_shutdown,NULL},{"exit",NULL,xlsp_handle_lc_exit}
};
static void send_error(XrLspServer *server,const XlspRequestId *id,int code,const char *message) {
    (void)server;CHECK(!wire&&xlsp_protocol_error(id,code,message,&wire)==XLSP_PROTOCOL_OK);++error_calls;
}
static void send_response(XrLspServer *server,const XlspRequestId *id,XrJsonValue *result) {
    (void)server;CHECK(!wire&&xlsp_protocol_response(id,&result,&wire)==XLSP_PROTOCOL_OK&&!result);++response_calls;
}
static bool ensure_method_table(XrLspServer *server,const XlspRequestId *id) {
    if(server->method_table_initialized)return true;
    XrXirStatus status=xlsp_method_table_initialize(server,methods,sizeof(methods)/sizeof(methods[0]));
    if(status==XR_XIR_OK)return true;send_error(server,id,-32603,"Method table initialization failed");return false;
}
static int source_request_mode(const char *method) {(void)method;return fixture_source_mode;}
static bool source_request_submit(XrLspServer *server,const XlspRequestId *id,XrJsonValue *params,unsigned mode) {
    (void)server;(void)id;(void)params;(void)mode;++source_calls;return false;
}
static bool pending_request_add(XrLspServer *server,XlspRequestId id,const char *method) {
    return xlsp_pending_request_add(&server->pending_requests,id,method,1);
}
static void pending_request_remove(XrLspServer *server,const XlspRequestId *id) {(void)xlsp_pending_request_remove(&server->pending_requests,id);}
static bool pending_request_is_cancelled(XrLspServer *server,const XlspRequestId *id) {
    XlspPendingRequest *entry=xlsp_pending_request_find(&server->pending_requests,id);return entry&&entry->cancelled;
}
#include "envelope_dispatch.inc.c"
#include "before_dispatch.inc.c"
static void dispatch(XrLspServer *server,const char *text) {
    CHECK(!wire);XrJsonValue *input=parsed(text);handle_message(server,input);xjson_free(input);
}
static void wire_clear(void) {xjson_free(wire);wire=NULL;}
static void dispatch_invalid(void) {
    XrLspServer server={0};server.initialized=true;
    const char *cases[]={
        "{\"jsonrpc\":\"2.0\",\"method\":\"probe/request\",\"id\":1,\"id\":2}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"probe/request\",\"method\":\"probe/notice\",\"id\":1}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"probe/request\",\"id\":1,\"params\":7}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"probe/notice\",\"params\":7}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}",
        "{\"jsonrpc\":\"1.0\",\"id\":1,\"result\":null}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null,\"error\":{}}"
    };
    for(unsigned i=0;i<7;++i) {
        XrLspServer before=server;size_t live=id_live;dispatch(&server,cases[i]);
        CHECK(!memcmp(&server,&before,sizeof(server))&&!request_calls&&!notification_calls&&id_live==live);
        if(i<3)CHECK(wire&&xjson_get_int(xjson_get(wire,"error"),"code")==-32600&&xjson_get(wire,"id")->type==XR_JSON_NULL);
        else CHECK(!wire);wire_clear();
    }
    envelope_zero();
}
static void dispatch_calls(void) {
    XrLspServer server={0};server.initialized=true;server.next_request_id=71;
    dispatch(&server,"{\"jsonrpc\":\"2.0\",\"method\":\"probe/request\",\"id\":\"a\\u0000b\\ud83d\\ude00\",\"params\":{}}");
    CHECK(request_calls==1&&response_calls==1&&wire&&xjson_get_int(wire,"result")==42);
    CHECK(xjson_get(wire,"id")->string_len==sizeof(identity_bytes)&&!memcmp(xjson_get(wire,"id")->as.string,identity_bytes,sizeof(identity_bytes)));wire_clear();
    dispatch(&server,"{\"jsonrpc\":\"2.0\",\"method\":\"probe/notice\",\"params\":{}}");CHECK(notification_calls==1&&!wire);
    dispatch(&server,"{\"jsonrpc\":\"2.0\",\"method\":\"probe/notice\",\"id\":7}");CHECK(notification_calls==1&&wire);wire_clear();
    dispatch(&server,"{\"jsonrpc\":\"2.0\",\"method\":\"probe/request\"}");CHECK(request_calls==1&&!wire);
    dispatch(&server,"{\"jsonrpc\":\"2.0\",\"method\":\"shutdown\",\"id\":8}");CHECK(server.shutdown_received&&wire&&xjson_get(wire,"result")->type==XR_JSON_NULL);wire_clear();
    dispatch(&server,"{\"jsonrpc\":\"2.0\",\"method\":\"probe/request\",\"id\":9}");CHECK(request_calls==1&&wire);wire_clear();
    dispatch(&server,"{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");CHECK(server.exit_received&&!wire);
    CHECK(server.next_request_id==71&&!server.pending_requests.count);xlsp_method_table_free(&server);envelope_zero();
}
static void dispatch_cancellation(void) {
    XrLspServer server={0};server.initialized=true;server.async=(XrLspAsync *)&server;server.index_progress_token="token";
    XlspRequestId first={.kind=XLSP_ID_NUMBER,.as.number=1},second={.kind=XLSP_ID_NUMBER,.as.number=2};
    CHECK(xlsp_pending_request_add(&server.pending_requests,first,"first",1)&&xlsp_pending_request_add(&server.pending_requests,second,"second",2));
    xlsp_pending_request_find(&server.pending_requests,&first)->source_task_id=41;
    xlsp_pending_request_find(&server.pending_requests,&second)->source_task_id=42;
    dispatch(&server,"{\"jsonrpc\":\"2.0\",\"method\":\"$/cancelRequest\",\"params\":{\"id\":1,\"id\":2}}");
    CHECK(!cancel_calls&&!xlsp_pending_request_find(&server.pending_requests,&first)->cancelled&&!xlsp_pending_request_find(&server.pending_requests,&second)->cancelled);
    dispatch(&server,"{\"jsonrpc\":\"2.0\",\"method\":\"$/cancelRequest\",\"params\":{\"id\":1}}");CHECK(cancel_calls==1&&cancelled_task==41);
    dispatch(&server,"{\"jsonrpc\":\"2.0\",\"method\":\"window/workDoneProgress/cancel\",\"params\":{\"token\":\"token\\u0000suffix\"}}");CHECK(!index_cancel_calls&&!server.index_cancelled);
    dispatch(&server,"{\"jsonrpc\":\"2.0\",\"method\":\"window/workDoneProgress/cancel\",\"params\":{\"token\":\"token\",\"token\":\"other\"}}");CHECK(!index_cancel_calls&&!server.index_cancelled);
    dispatch(&server,"{\"jsonrpc\":\"2.0\",\"method\":\"window/workDoneProgress/cancel\",\"params\":{\"token\":\"token\"}}");CHECK(index_cancel_calls==1&&server.index_cancelled&&!wire);
    CHECK(xlsp_pending_request_remove(&server.pending_requests,&first)&&xlsp_pending_request_remove(&server.pending_requests,&second));xlsp_method_table_free(&server);envelope_zero();
}
static int progress_before(void) {
    XrLspServer server={0};server.initialized=true;server.index_progress_token="token";
    XrJsonValue *input=parsed("{\"jsonrpc\":\"2.0\",\"method\":\"window/workDoneProgress/cancel\",\"params\":{\"token\":\"token\\u0000suffix\"}}");
    before_handle_message(&server,input);xjson_free(input);CHECK(server.index_cancelled&&index_cancel_calls==1&&!wire);
    xlsp_method_table_free(&server);envelope_zero();
    fputs("PROGRESS_BEFORE embedded_NUL_selected_prefix=1 callbacks=1 JSON0/0 id0/0\n",stderr);return 1;
}
int main(int argc,char **argv) {
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--before-progress")));if(argc==2)return progress_before();
    dispatch_invalid();dispatch_calls();dispatch_cancellation();CHECK(!source_calls&&!format_calls);
    printf("Extracted dispatcher malformed=7 calls=7 cancellation=5 callback_and_pending_owners_checked=1 id0/0 JSON0/0; SourceTask_transport_full_server_not_credited\n");return 0;
}
