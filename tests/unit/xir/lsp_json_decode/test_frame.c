/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "../lsp_json_clone/test_allocator.h"
#include "base/xjson_parse.h"
#include "base/xmalloc.h"
#include "app/lsp/xlsp_protocol_output.h"
#include "app/lsp/xlsp_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"frame:%d:%s\n",__LINE__,#c);exit(2);}}while(0)
static size_t dispatches;
static void fixture_log(const char *format,...) {(void)format;}
#define lsp_log(...) fixture_log(__VA_ARGS__)
/* Only the decoded dispatch boundary is observed here; protocol output and
 * transport are the actual implementations and retain their allocations. */
static void handle_message(XrLspServer *server,XrJsonValue *message) {
    CHECK(server&&message&&message->type==XR_JSON_OBJECT);++dispatches;
    CHECK(!strcmp(xjson_get_string(message,"jsonrpc"),"2.0"));
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
#include "byte_frame.inc.c"
int main(int argc,char **argv) {
    CHECK(argc==3);size_t failure=(size_t)strtoull(argv[1],NULL,10);unsigned scenario=(unsigned)strtoul(argv[2],NULL,10);
    clone_fault(SIZE_MAX);XrLspServer server={0};server.initialized=true;server.next_request_id=71;
    server.transport=xlsp_transport_stdio();CHECK(server.transport);size_t sites=0,frames=0;bool hit=false;
    while(!server.exit_received){size_t length=0;bool block=false;char *text=xlsp_transport_try_read(server.transport,&length,&block);
        if(!text){CHECK(!block);break;}
        ++frames;bool inject=scenario==0?frames==2:frames==1;
        clone_fault(inject?failure:SIZE_MAX);handle_json_frame(&server,text,length);CloneStats stats=clone_stats();
        if(inject){sites=stats.attempts;hit=stats.injected;}
        memset(text,'?',length);clone_test_free(text);
    }
    bool exited=server.exit_received;CHECK(server.next_request_id==71&&!server.pending_requests.count);
    xlsp_transport_free(server.transport);CHECK(!clone_stats().live&&!clone_stats().bytes);
    fprintf(stderr,"FRAME scenario=%u frames=%zu dispatches=%zu sites=%zu injected=%u exit=%u JSON_and_transport0/0 dispatch_spy=1\n",
        scenario,frames,dispatches,sites,hit?1u:0u,exited?1u:0u);return 0;
}
