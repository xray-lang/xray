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
 *   Compiler faults use the ordinary ledger implementation and physical owner.
 */
#include "app/mcp/xmcp_source.h"
#include "app/mcp/xmcp_tools_internal.h"
#include "app/mcp/xmcp_server.h"
#include "frontend/parser/xparse.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "../xir/xir_source_compile_owner.h"

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
    CHECK(xjson_get_int_or(d,"code",-1)==XR_XIR_BAD_TYPE);
    xjson_free(r);
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
    XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XmcpSourceOwner owner={0};XmcpSourceRequest request={text,sizeof(text)-1,"mcp-owner-test",&context};
    CHECK(xmcp_source_check(&request,&owner)==XR_XIR_OK);
    memset(text,'?',sizeof(text));memset(&request,0,sizeof(request));
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(owner.result.snapshot);
    CHECK(view && view->complete);
    bool found=false;for(uint32_t i=0;i<view->declaration_count;++i)
        if(!strcmp(view->declarations[i].name,"answer"))found=true;
    CHECK(found);xmcp_source_free(&owner);
    XrCompileResourceStats final={0};CHECK(xr_compile_resources_stats(context.resources,&final)==XR_COMPILE_RESOURCE_OK);
    CHECK(final.live_bytes==baseline.live_bytes && final.work>baseline.work && final.allocated_bytes>baseline.allocated_bytes);
    xr_compile_resources_release(context.resources);physical_zero();
}
static XrCompileResourceLimits resource_limits(void) {
    return (XrCompileResourceLimits){UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
}
static XrXirStatus helper_operation(XrCompileResourceLimits limits,XrCompileResourceStats *required) {
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    XrCompileResourceStatus made=xr_compile_resources_new(&limits,&context.resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    static const char code[]="export fn answer()->i64 { return 42 }\n";
    XmcpSourceRequest request={code,sizeof(code)-1,"mcp-resource-test",&context};
    XmcpSourceOwner owner={0};XrXirStatus status=xmcp_source_check(&request,&owner);
    if(required)CHECK(xr_compile_resources_stats(context.resources,required)==XR_COMPILE_RESOURCE_OK);
    if(status!=XR_XIR_OK)CHECK(!owner.result.checked);
    xmcp_source_free(&owner);
    XrCompileResourceStats final={0};CHECK(xr_compile_resources_stats(context.resources,&final)==XR_COMPILE_RESOURCE_OK);
    CHECK(final.live_bytes==baseline.live_bytes && final.work>=baseline.work && final.allocated_bytes>=baseline.allocated_bytes);
    xr_compile_resources_release(context.resources);physical_zero();return status;
}
static void same_ledger_faults(void) {
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        source_fixture_compile_fail_at=SIZE_MAX;
        XrCompileResourceLimits limits=resource_limits();XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
        CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
        static const char code[]="export fn answer()->i64 { return 42 }\n";
        XmcpSourceRequest request={code,sizeof(code)-1,"mcp-resource-test",&context};
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=probe?probe-1:SIZE_MAX;
        source_fixture_compile_injected=false;XmcpSourceOwner owner={0};
        XrXirStatus status=xmcp_source_check(&request,&owner);
        if(!probe){CHECK(status==XR_XIR_OK);sites=source_fixture_compile_attempts;CHECK(sites);}
        else{CHECK(source_fixture_compile_injected && status==XR_XIR_OUT_OF_MEMORY && !owner.result.checked);}
        xmcp_source_free(&owner);
        XrCompileResourceStats after_failure={0};CHECK(xr_compile_resources_stats(context.resources,&after_failure)==XR_COMPILE_RESOURCE_OK);
        CHECK(after_failure.live_bytes==baseline.live_bytes);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(xmcp_source_check(&request,&owner)==XR_XIR_OK);
        CHECK(xr_xir_compile_source_snapshot_view(owner.result.snapshot)->complete);
        xmcp_source_free(&owner);
        XrCompileResourceStats after_retry={0};CHECK(xr_compile_resources_stats(context.resources,&after_retry)==XR_COMPILE_RESOURCE_OK);
        CHECK(after_retry.live_bytes==baseline.live_bytes && after_retry.work>after_failure.work &&
            after_retry.allocated_bytes>after_failure.allocated_bytes && after_retry.work<=limits.work);
        xr_compile_resources_release(context.resources);physical_zero();
    }
    printf("MCP Source helper: %zu full allocation failures and same-ledger retries, physical0\n",sites);
}
static void resource_axes(void) {
    source_fixture_compile_fail_at=SIZE_MAX;
    XrCompileResourceStats required={0};CHECK(helper_operation(resource_limits(),&required)==XR_XIR_OK);
    CHECK(required.allocated_bytes && required.peak_bytes && required.work);
    for(unsigned axis=0;axis<3;++axis) {
        uint64_t exact=axis==0?required.allocated_bytes:axis==1?required.peak_bytes:required.work;
        for(int delta=-1;delta<=1;++delta) {
            XrCompileResourceLimits limits=resource_limits();
            if(axis==0)limits.allocated_bytes=exact+delta;
            else if(axis==1)limits.live_bytes=exact+delta;
            else limits.work=exact+delta;
            XrXirStatus status=helper_operation(limits,NULL);
            CHECK(status==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
        }
    }
    printf("MCP Source exact/minus1/plus1 allocated=%llu peak=%llu work=%llu, physical0\n",
        (unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,(unsigned long long)required.work);
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
static void resource_cases(void) {
    tool_faults(false,false);tool_faults(true,false);tool_faults(false,true);
    same_ledger_faults();resource_axes();owner_case();
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();
    if(argc==2 && !strcmp(argv[1],"--resources")){resource_cases();return 0;}
    CHECK(argc==1);analyze_cases();definition_cases();owner_case();
    puts("MCP analyze/source definition: independent normal, refusal, NUL, ambiguity, lifetime and recovery PASS");
    return 0;
}
