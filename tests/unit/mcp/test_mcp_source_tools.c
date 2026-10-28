/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_mcp_source_tools.c - Real source tools and owned query lifetime
 *
 * KEY CONCEPT:
 *   Production tool handlers produce independently expected JSON responses.
 *   Compiler faults use the ordinary memory implementation and physical owner.
 */
#include "app/mcp/xmcp_source.h"
#include "app/mcp/xmcp_tools_internal.h"
#include "app/mcp/xmcp_server.h"
#include "frontend/parser/xparse.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "mcp_compile_memory.h"

static void physical_zero(void) {
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static XrJsonValue *analysis(const char *text, size_t length, const char *mode) {
    XmcpServer server = {0}; XmcpCallContext context = {0};
    XrJsonValue *args = xjson_new_object();
    xjson_object_set(args, "code", xjson_new_string_n(text, length));
    XJSON_SET_STRING(args, "mode", mode);
    XrJsonValue *result = xmcp_tool_xray_analyze(&server, &context, args);
    xjson_free(args); CHECK(result); physical_zero(); return result;
}
static void analyze_cases(void) {
    static const char good[] = "var x:i64 = 41\nprint(x + 1)\n";
    for (unsigned i=0; i<3; ++i) {
        const char *mode = i==0 ? "syntax" : i==1 ? "semantic" : "full";
        XrJsonValue *r=analysis(good,sizeof(good)-1,mode);
        XrJsonValue *s=xjson_get_object(r,"structuredContent");
        CHECK(s && xjson_get_bool(s,"ok") && !xjson_get_bool(r,"isError"));
        CHECK(xjson_get_int_or(s,"diagnosticCount",-1)==0);
        size_t length=0;char *serialized=xjson_stringify(r,&length);
        CHECK(serialized && length && strstr(serialized,"structuredContent"));
        xr_free(serialized);xjson_free(r);
    }
    static const char invalid[]="var x:i64 = \"wrong\"\n";
    XrJsonValue *r=analysis(invalid,sizeof(invalid)-1,"semantic");
    XrJsonValue *s=xjson_get_object(r,"structuredContent");
    CHECK(s && !xjson_get_bool(s,"ok") && xjson_get_bool(r,"isError"));
    CHECK(xjson_get_int_or(s,"diagnosticCount",0)==1);
    XrJsonValue *d=xjson_array_get(xjson_get_array(s,"diagnostics"),0);
    CHECK(d && !strcmp(xjson_get_string(d,"source"),"source"));
    CHECK(xjson_get_int_or(d,"code",-1)==0); /* This producer has no canonical code yet. */
    CHECK(xjson_get_int_or(s,"sourceStatus",-1)==XR_XIR_BAD_TYPE);
    xjson_free(r);
    static const char duplicates[]="const alpha:i64=1\nconst alpha:i64=2\nconst beta:i64=3\nconst beta:i64=4\nprint(9)\n";
    for(unsigned mode=0;mode<2;++mode) {
        r=analysis(duplicates,sizeof(duplicates)-1,mode?"full":"semantic");s=xjson_get_object(r,"structuredContent");
        CHECK(s&&!xjson_get_bool(s,"ok")&&xjson_get_bool(r,"isError"));
        CHECK(xjson_get_int_or(s,"sourceStatus",-1)==XR_XIR_BAD_STRUCTURE);
        CHECK(xjson_get_int_or(s,"diagnosticCount",0)==2);
        for(unsigned i=0;i<2;++i) {
            d=xjson_array_get(xjson_get_array(s,"diagnostics"),(int)i);
            CHECK(d&&xjson_get_int_or(d,"code",-1)==302&&xjson_get_int_or(d,"line",0)==2+2*i);
            CHECK(!strcmp(xjson_get_string(d,"message"),"duplicate declaration name"));
            XrJsonValue *related=xjson_get_array(d,"related");CHECK(related&&xjson_array_len(related)==1);
            XrJsonValue *label=xjson_array_get(related,0);
            CHECK(label&&xjson_get_int_or(label,"line",0)==1+2*i&&!strcmp(xjson_get_string(label,"message"),"first declaration is here"));
        }
        xjson_free(r);physical_zero();
    }
    static const char bad_syntax[]="var x = ;\n";
    r=analysis(bad_syntax,sizeof(bad_syntax)-1,"syntax");
    CHECK(xjson_get_bool(r,"isError"));
    s=xjson_get_object(r,"structuredContent");
    CHECK(s && xjson_get_int_or(s,"diagnosticCount",0)>0);
    d=xjson_array_get(xjson_get_array(s,"diagnostics"),0);
    CHECK(d && !strcmp(xjson_get_string(d,"source"),"parser"));xjson_free(r);
    static const char nul[]="var x = 1\n\0print(x)\n";
    for(unsigned i=0;i<2;++i) {
        r=analysis(nul,sizeof(nul)-1,i?"full":"syntax");
        CHECK(xjson_get_bool(r,"isError"));xjson_free(r);
    }
    r=analysis(good,sizeof(good)-1,"full");
    CHECK(!xjson_get_bool(r,"isError"));xjson_free(r);
    XmcpServer server={0};XmcpCallContext context={0};
    XrJsonValue *args=xjson_new_object();
    xjson_object_set(args,"code",xjson_new_string_n(nul,sizeof(nul)-1));
    r=xmcp_tool_xray_format(&server,&context,args);xjson_free(args);
    CHECK(xjson_get_bool(r,"isError"));xjson_free(r);physical_zero();
}
static XrJsonValue *definition(const char *code,const char *symbol,const char *module_id) {
    XmcpServer server={0};XmcpCallContext context={0};
    XrJsonValue *args=xjson_new_object();XJSON_SET_STRING(args,"code",code);
    XJSON_SET_STRING(args,"symbol",symbol);
    if(module_id)XJSON_SET_STRING(args,"moduleId",module_id);
    XrJsonValue *r=xmcp_tool_xray_definition(&server,&context,args);
    xjson_free(args);CHECK(r);physical_zero();return r;
}
static void definition_cases(void) {
    static const char code[]="export fn answer()->i64 { return 42 }\n";
    XrJsonValue *r=definition(code,"answer","mcp-query-test");
    XrJsonValue *s=xjson_get_object(r,"structuredContent");
    CHECK(s && xjson_get_bool(s,"found") && xjson_get_bool(s,"complete"));
    CHECK(!strcmp(xjson_get_string(s,"kind"),"source"));
    CHECK(xjson_get_int_or(s,"line",-1)==1 && xjson_get_int_or(s,"column",-1)==11);
    CHECK(xjson_get_int_or(s,"endLine",-1)==1 && xjson_get_int_or(s,"endColumn",-1)==17);
    CHECK(strstr(xjson_get_string(s,"content"),"answer"));
    xjson_free(r);
    r=definition(code,"absent","mcp-query-test");s=xjson_get_object(r,"structuredContent");
    CHECK(s && !xjson_get_bool(s,"found") && xjson_get_bool(s,"complete") && !xjson_get_bool(r,"isError"));xjson_free(r);
    r=definition(code,"answer",NULL);CHECK(xjson_get_bool(r,"isError"));xjson_free(r);
    r=definition("var x:i64 = \"wrong\"\n","x","mcp-query-test");
    CHECK(xjson_get_bool(r,"isError"));s=xjson_get_object(r,"structuredContent");
    CHECK(s && !xjson_get_bool(s,"found") && !xjson_get_bool(s,"complete"));xjson_free(r);
    r=definition("fn first(value:i64)->i64 { return value }\nfn second(value:i64)->i64 { return value }\n","value","mcp-query-test");
    CHECK(xjson_get_bool(r,"isError"));s=xjson_get_object(r,"structuredContent");
    CHECK(s && !xjson_get_bool(s,"found") && xjson_get_bool(s,"complete"));xjson_free(r);physical_zero();
}
static void owner_case(void) {
    char text[]="export fn answer()->i64 { return 42 }\n";
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XmcpSourceOwner owner={0};XmcpSourceRequest request={text,sizeof(text)-1,"mcp-owner-test",&context,NULL};
    CHECK(xmcp_source_check(&request,&owner)==XR_XIR_OK);
    memset(text,'?',sizeof(text));memset(&request,0,sizeof(request));
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(owner.result.snapshot);
    CHECK(view && view->complete);
    bool found=false;for(uint32_t i=0;i<view->declaration_count;++i)
        if(!strcmp(view->declarations[i].name,"answer"))found=true;
    CHECK(found);xmcp_source_free(&owner);
    XrCompileResourceStats final={0};CHECK(xr_compile_resources_stats(context.resources,&final)==XR_COMPILE_RESOURCE_OK);
    CHECK(final.held_bytes==baseline.held_bytes && final.allocated_bytes>baseline.allocated_bytes);
    xr_compile_resources_release(context.resources);physical_zero();
}
static XrCompileResourceLimits resource_limits(void) {
    return (XrCompileResourceLimits){XR_COMPILE_MEMORY_ORDINARY,0,true};
}
static XrXirStatus helper_operation(XrCompileResourceLimits limits,XrCompileResourceStats *required) {
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    XrCompileResourceStatus made=xr_compile_resources_new(&limits,&context.resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return xmcp_source_resource_status(made);
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    static const char code[]="export fn answer()->i64 { return 42 }\n";
    XmcpSourceRequest request={code,sizeof(code)-1,"mcp-resource-test",&context,NULL};
    XmcpSourceOwner owner={0};XrXirStatus status=xmcp_source_check(&request,&owner);
    if(required)CHECK(xr_compile_resources_stats(context.resources,required)==XR_COMPILE_RESOURCE_OK);
    if(status!=XR_XIR_OK)CHECK(!owner.result.checked);
    xmcp_source_free(&owner);
    XrCompileResourceStats final={0};CHECK(xr_compile_resources_stats(context.resources,&final)==XR_COMPILE_RESOURCE_OK);
    CHECK(final.held_bytes==baseline.held_bytes && final.allocated_bytes>=baseline.allocated_bytes);
    xr_compile_resources_release(context.resources);physical_zero();return status;
}
static void same_owner_faults(void) {
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        source_fixture_compile_fail_at=SIZE_MAX;
        XrCompileResourceLimits limits=resource_limits();XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
        CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
        static const char code[]="export fn answer()->i64 { return 42 }\n";
        XmcpSourceRequest request={code,sizeof(code)-1,"mcp-resource-test",&context,NULL};
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=probe?probe-1:SIZE_MAX;
        source_fixture_compile_injected=false;XmcpSourceOwner owner={0};
        XrXirStatus status=xmcp_source_check(&request,&owner);
        if(!probe){CHECK(status==XR_XIR_OK);sites=source_fixture_compile_attempts;CHECK(sites);}
        else{CHECK(source_fixture_compile_injected && status==XR_XIR_OUT_OF_MEMORY && !owner.result.checked);}
        xmcp_source_free(&owner);
        XrCompileResourceStats after_failure={0};CHECK(xr_compile_resources_stats(context.resources,&after_failure)==XR_COMPILE_RESOURCE_OK);
        CHECK(after_failure.held_bytes==baseline.held_bytes);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(xmcp_source_check(&request,&owner)==XR_XIR_OK);
        CHECK(xr_xir_compile_source_snapshot_view(owner.result.snapshot)->complete);
        xmcp_source_free(&owner);
        XrCompileResourceStats after_retry={0};CHECK(xr_compile_resources_stats(context.resources,&after_retry)==XR_COMPILE_RESOURCE_OK);
        CHECK(after_retry.held_bytes==baseline.held_bytes &&
            after_retry.allocated_bytes>after_failure.allocated_bytes);
        xr_compile_resources_release(context.resources);physical_zero();
    }
    printf("MCP Source helper: %zu full allocation failures and same-owner retries, physical0\n",sites);
}
static void resource_axes(void) {
    source_fixture_compile_fail_at=SIZE_MAX;
    XrCompileResourceStats required={0};CHECK(helper_operation(resource_limits(),&required)==XR_XIR_OK);
    CHECK(required.allocated_bytes && required.peak_bytes);
    for(int delta=-1;delta<=1;++delta) {
        uint64_t boundary=delta<0?required.peak_bytes-1:required.peak_bytes+(unsigned)delta;
        XrCompileResourceLimits limits={XR_COMPILE_MEMORY_FINITE,boundary,true};
        CHECK(helper_operation(limits,NULL)==(delta<0?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));
    }
    printf("MCP Source held peak=%llu minus1/exact/plus1, physical0\n",
        (unsigned long long)required.peak_bytes);
}
static void tool_faults(bool syntax,bool query) {
    static const char good[]="export fn answer()->i64 { return 42 }\n";
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=probe?probe-1:SIZE_MAX;
        source_fixture_compile_injected=false;
        XrJsonValue *r=query?definition(good,"answer","mcp-fault-test"):analysis(good,sizeof(good)-1,syntax?"syntax":"full");
        if(!probe){CHECK(!xjson_get_bool(r,"isError"));sites=source_fixture_compile_attempts;CHECK(sites);}
        else {
            CHECK(source_fixture_compile_injected && xjson_get_bool(r,"isError"));
            XrJsonValue *s=xjson_get_object(r,"structuredContent");CHECK(s);
            if(query)CHECK(xjson_get_int_or(s,"sourceStatus",-1)==XR_XIR_OUT_OF_MEMORY && !xjson_get_bool(s,"found"));
            else {
                XrJsonValue *d=xjson_array_get(xjson_get_array(s,"diagnostics"),0);CHECK(d);
                CHECK(xjson_get_int_or(d,"code",-1)==(syntax?XR_PARSE_OUT_OF_MEMORY:XR_XIR_OUT_OF_MEMORY));
            }
        }
        xjson_free(r);physical_zero();
    }
    source_fixture_compile_fail_at=SIZE_MAX;
    printf("MCP %s: %zu complete compiler allocation failures, exact OOM and physical0\n",
        query?"source definition":syntax?"syntax analyze":"semantic analyze",sites);
}
typedef struct CancelProbe { size_t polls, cancel_at; } CancelProbe;
static bool cancel_probe(const void *context) {
    CancelProbe *probe=(CancelProbe *)context;
    return ++probe->polls>=probe->cancel_at;
}
static void request_control_cases(void) {
    XrCompileResourceLimits limits=resource_limits();
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    static const char code[]="export fn answer()->i64 { return 42 }\n";
    CancelProbe probe={0,SIZE_MAX};XrCompileRequestControl control={0};
    XrCompileRequestOptions options={cancel_probe,&probe,XR_COMPILE_DEADLINE_NONE,0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XmcpSourceRequest request={code,sizeof(code)-1,"mcp-control-test",&context,&control};
    XmcpSourceOwner owner={0};CHECK(xmcp_source_check(&request,&owner)==XR_XIR_OK);
    size_t polls=probe.polls;CHECK(polls>=3);
    memset(&control,0,sizeof(control));memset(&probe,0,sizeof(probe));
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(owner.result.snapshot);
    CHECK(view && view->complete);xmcp_source_free(&owner);
    size_t cuts[]={1,polls/2,polls};
    for(size_t i=0;i<3;++i) {
        probe=(CancelProbe){0,cuts[i]};control=(XrCompileRequestControl){0};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        CHECK(xmcp_source_check(&request,&owner)==XR_XIR_CANCELLED);
        CHECK(probe.polls==cuts[i] && !owner.result.checked && !owner.result.snapshot);
        xmcp_source_free(&owner);
        XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.held_bytes==baseline.held_bytes);
    }
    control=(XrCompileRequestControl){0};options=(XrCompileRequestOptions){NULL,NULL,XR_COMPILE_DEADLINE_AFTER,0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xmcp_source_check(&request,&owner)==XR_XIR_DEADLINE && !owner.result.checked && !owner.result.snapshot);
    xmcp_source_free(&owner);
    control=(XrCompileRequestControl){0};options=(XrCompileRequestOptions){0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xmcp_source_check(&request,&owner)==XR_XIR_OK);xmcp_source_free(&owner);
    xr_compile_resources_release(context.resources);physical_zero();
    XmcpServer server={0};server.shutdown=1;XmcpCallContext call={0};
    for(unsigned tool=0;tool<4;++tool) {
        XrJsonValue *args=xjson_new_object();XJSON_SET_STRING(args,"code",code);
        XJSON_SET_STRING(args,"mode","full");XJSON_SET_STRING(args,"symbol","answer");
        XJSON_SET_STRING(args,"moduleId","mcp-control-test");
        XrJsonValue *result=tool==0?xmcp_tool_xray_analyze(&server,&call,args):
            tool==1?xmcp_tool_xray_format(&server,&call,args):
            tool==2?xmcp_tool_xray_definition(&server,&call,args):xmcp_tool_xray_run(&server,&call,args);
        CHECK(result && xjson_get_bool(result,"isError"));
        if(tool==1) {
            XrJsonValue *content=xjson_array_get(xjson_get_array(result,"content"),0);
            CHECK(content && strstr(xjson_get_string(content,"text"),"domain=request"));
        }
        if(tool==3)CHECK(xjson_get_int_or(xjson_get_object(result,"structuredContent"),"sourceStatus",-1)==XR_XIR_CANCELLED);
        xjson_free(result);xjson_free(args);physical_zero();
    }
    printf("Source request cancel at first/middle/final %zu polls, zero deadline, owner reuse, request death and four shutdown handlers PASS\n",polls);
}
static void resource_cases(void) {
    tool_faults(false,false);tool_faults(true,false);tool_faults(false,true);
    same_owner_faults();resource_axes();owner_case();
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();
    if(argc==2 && !strcmp(argv[1],"--resources")){resource_cases();return 0;}
    CHECK(argc==1);request_control_cases();analyze_cases();definition_cases();owner_case();
    puts("MCP analyze/source definition: independent normal, refusal, NUL, ambiguity, lifetime and recovery PASS");
    return 0;
}
