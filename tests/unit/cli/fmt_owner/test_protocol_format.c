/* Exact formatter consumers, dispatch branch and real JSON/framing components. */
#include "base/xmalloc.h"
#include "base/xhash.h"
#include "base/xjson.h"
#include "base/xframing.h"
#include "base/xchecks.h"
#include "app/lsp/xlsp_server.h"
#include "app/lsp/xlsp_format.h"
#include "app/lsp/xlsp_transport.h"
#include "app/mcp/xmcp_tools_internal.h"
#include "app/mcp/xmcp_server.h"
#include "frontend/format/xfmt.h"
#include "frontend/parser/xparse.h"
#include <io.h>
#include <errno.h>
#include <fcntl.h>
#include "os/os_time.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"protocol:%d %s\n",__LINE__,#c);exit(1);}}while(0)
static size_t attempts,fail_at=SIZE_MAX,live;
static uint64_t limit_work=UINT64_MAX;
static XrLspServer *cancel_on_failure;
static void *test_malloc(size_t bytes) {
    if(attempts++==fail_at) {
        if(cancel_on_failure)cancel_on_failure->pending_requests.requests[0].cancelled=true;
        return NULL;
    }
    void *out=xr_malloc(bytes);if(out)++live;return out;
}
static void test_free(void *p){if(p){CHECK(live);--live;}xr_free(p);}
#undef xr_malloc
#undef xr_free
#define xr_malloc test_malloc
#define xr_free test_free
#define xr_compile_resources_new real_resources_new
#include "base/xcompile_resources.c"
#undef xr_compile_resources_new
#undef xr_malloc
#undef xr_free
#define xr_free free
XR_FUNC XrCompileResourceStatus xr_compile_resources_new(const XrCompileResourceLimits *limits,XrCompileResources **out) {
    CHECK(limits->allocated_bytes==UINT64_C(1073741824) && limits->live_bytes==UINT64_C(268435456) && limits->work==UINT64_C(8589934592));
    XrCompileResourceLimits bounded=*limits;if(limit_work!=UINT64_MAX)bounded.work=limit_work;
    return real_resources_new(&bounded,out);
}
static void fixture_log(const char *format,...){(void)format;}
#define lsp_log fixture_log
static XrJsonValue *fixture_other_handler(XrLspServer *server,XrJsonValue *params){(void)params;CHECK(!server->formatting_failure.stage);return xjson_new_null();}
#include "protocol-selected.inc"
static void reset(void){CHECK(!live);attempts=0;fail_at=SIZE_MAX;limit_work=UINT64_MAX;cancel_on_failure=NULL;}
static XrJsonValue *mcp(const char *code,bool tabs) {
    XmcpServer server={0};XmcpCallContext context={0};XrJsonValue *args=xjson_new_object();
    XJSON_SET_STRING(args,"code",code);XJSON_SET_INT(args,"indentSize",2);XJSON_SET_BOOL(args,"useTabs",tabs);
    XrJsonValue *out=xmcp_tool_xray_format(&server,&context,args);xjson_free(args);CHECK(out && !live);return out;
}
static void mcp_cases(void){
    reset();XrJsonValue *out=mcp("var x=1\n",false);size_t count=attempts;
    XrJsonValue *s=xjson_get_object(out,"structuredContent");CHECK(s && xjson_get_bool(s,"ok") && xjson_get_bool(s,"changed"));
    CHECK(!strcmp(xjson_get_string(s,"formattedCode"),"var x = 1\n") && xjson_get_int(s,"indentSize")==2 && !xjson_get_bool(s,"useTabs"));
    CHECK(xjson_get_int(s,"diagnosticCount")==0 && !xjson_get_bool(s,"truncated"));
    CHECK(!strcmp(xjson_get_string(xjson_array_get(xjson_get_array(out,"content"),0),"text"),xjson_get_string(s,"formattedCode")));xjson_free(out);
    reset();out=mcp("var x = 1\n",false);CHECK(!xjson_get_bool(xjson_get_object(out,"structuredContent"),"changed"));xjson_free(out);
    reset();out=mcp("fn value(){return 1}\n",true);CHECK(strstr(xjson_get_string(xjson_get_object(out,"structuredContent"),"formattedCode"),"\treturn"));xjson_free(out);
    reset();out=mcp("var x = ;\n",false);s=xjson_get_object(out,"structuredContent");CHECK(xjson_get_bool(out,"isError") && s && !xjson_get_bool(s,"ok"));
    CHECK(xjson_get_int(s,"diagnosticCount")>0 && !strcmp(xjson_get_string(xjson_array_get(xjson_get_array(s,"diagnostics"),0),"source"),"parser"));xjson_free(out);
    for(size_t i=0;i<count;++i){reset();fail_at=i;out=mcp("var x=1\n",false);CHECK(xjson_get_bool(out,"isError") && !xjson_get_object(out,"structuredContent"));xjson_free(out);}
    reset();limit_work=500;out=mcp("var x=1\n",false);CHECK(xjson_get_bool(out,"isError"));xjson_free(out);
    printf("MCP actual format and JSON: %zu actual OOM points, budget, structured syntax/config/changed PASS\n",count);
}
static XrJsonValue *lsp_call(XrLspServer *server,XrJsonValue *params,bool formatting,bool cancelled){
    FILE *file=tmpfile();CHECK(file);XrLspTransport transport={.write_fd=_fileno(file),.connected=true};server->transport=&transport;
    server->pending_requests.count=1;server->pending_requests.requests[0].id=(XlspRequestId){.kind=XLSP_ID_NUMBER,.as.number=7};
    server->pending_requests.requests[0].cancelled=cancelled;
    selected_dispatch(server,params,formatting);CHECK(!live);
    CHECK(fflush(file)==0 && fseek(file,0,SEEK_SET)==0);char bytes[8192];size_t n=fread(bytes,1,sizeof(bytes)-1,file);bytes[n]=0;CHECK(fclose(file)==0);
    char *json=strstr(bytes,"\r\n\r\n");CHECK(json);json+=4;
    XrJsonValue *response=xjson_parse(json,strlen(json));CHECK(response);CHECK(xjson_get_int(response,"id")==7);return response;
}
static void lsp_cases(void){
    XrLspServer server={0};XrLspDocument doc={0};doc.server=&server;doc.uri="file:///test.xr";doc.content="var x=1\n";doc.length=8;doc.line_count=1;
    XrLspDocBucket bucket={.doc=&doc};XrLspDocBucket *buckets[]={&bucket};XrLspDocTable table={.buckets=buckets,.bucket_count=1,.doc_count=1};server.doc_table=&table;
    server.config.format_tab_size=4;server.config.format_insert_spaces=true;server.config.format_max_line_length=100;
    XrJsonValue *params=xjson_new_object(),*document=xjson_new_object();XJSON_SET_STRING(document,"uri",doc.uri);xjson_object_set(params,"textDocument",document);
    reset();server.formatting_failure.stage=99;XrJsonValue *out=lsp_call(&server,params,true,false);size_t count=attempts;
    CHECK(!server.formatting_failure.stage);XrJsonValue *edits=xjson_get_array(out,"result");CHECK(edits && xjson_array_len(edits)==1);
    CHECK(!strcmp(xjson_get_string(xjson_array_get(edits,0),"newText"),"var x = 1\n"));xjson_free(out);
    for(size_t i=0;i<count;++i){reset();fail_at=i;out=lsp_call(&server,params,true,false);CHECK(xjson_get_int(xjson_get_object(out,"error"),"code")==-32603);CHECK(server.formatting_failure.stage);xjson_free(out);}
    reset();limit_work=500;out=lsp_call(&server,params,true,false);CHECK(xjson_get_object(out,"error"));xjson_free(out);
    reset();fail_at=0;cancel_on_failure=&server;out=lsp_call(&server,params,true,false);CHECK(xjson_get_int(xjson_get_object(out,"error"),"code")==-32800);xjson_free(out);
    reset();out=lsp_call(&server,params,true,true);CHECK(!attempts && xjson_get_int(xjson_get_object(out,"error"),"code")==-32800);xjson_free(out);
    reset();server.formatting_failure.stage=99;out=lsp_call(&server,params,false,false);CHECK(!server.formatting_failure.stage && !xjson_get_object(out,"error"));xjson_free(out);
    reset();doc.content="var x = ;\n";out=lsp_call(&server,params,true,false);CHECK(xjson_array_len(xjson_get_array(out,"result"))==0 && !server.formatting_failure.stage);xjson_free(out);
    xjson_free(params);printf("LSP exact dispatch and framing: %zu OOM points, budget, cancel priority and reset PASS\n",count);
}
int main(void){mcp_cases();lsp_cases();CHECK(!live);return 0;}
