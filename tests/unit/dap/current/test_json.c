/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_json.c - Real DAP request syntax and cumulative JSON ownership failures
 */
#include "app/dap/xdap_json.h"
#include "base/xjson.h"
#include "../../test_win_compat.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../../xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
static const char request[]={
    "{\"seq\":7,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{"
    "\"clientID\":\"IDE\\uD83D\\uDE80\",\"supportsVariableType\":true,"
    "\"unknown\":[-2,1.25e-3,null,{\"x\":false}]}}"};
static XrCompileResources *owner_new(XrCompileResourceStats *baseline) {
    XrCompileResourceLimits limits={8388608,1048576,128000000};XrCompileResources *owner=NULL;
    CHECK(xr_compile_resources_new(&limits,&owner)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(owner,baseline)==XR_COMPILE_RESOURCE_OK);return owner;
}
static void owner_free(XrCompileResources *owner,XrCompileResourceStats baseline) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(owner,&stats)==XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes==baseline.live_bytes);xr_compile_resources_release(owner);
    CHECK(!runtime_live && !runtime_bytes);
}
static void requests_and_output(void) {
    XrCompileResourceStats baseline;XrCompileResources *owner=owner_new(&baseline);
    XdapJsonDocument d={0};CHECK(xdap_json_parse(owner,request,sizeof(request)-1,256,32,&d)==XR_XIR_OK);
    uint32_t seq=0,command=0,args=0,client=0;uint64_t value=0;bool equal=false;
    CHECK(xdap_json_member(&d,d.root,"seq",&seq)==XR_XIR_OK && seq);
    CHECK(xdap_json_u64(&d,seq,&value)==XR_XIR_OK && value==7);
    CHECK(xdap_json_member(&d,d.root,"command",&command)==XR_XIR_OK);
    CHECK(xdap_json_equal(&d,command,"initialize",&equal)==XR_XIR_OK && equal);
    CHECK(xdap_json_member(&d,d.root,"arguments",&args)==XR_XIR_OK);
    CHECK(xdap_json_member(&d,args,"clientID",&client)==XR_XIR_OK);
    CHECK(d.nodes[client].length==7 && !memcmp(d.nodes[client].text,"IDE\xf0\x9f\x9a\x80",7));
    XdapJsonDocument same=d;
    CHECK(xdap_json_parse(owner,request,sizeof(request)-1,256,32,&same)==XR_XIR_BAD_STRUCTURE && same.nodes==d.nodes);
    xdap_json_document_free(&d);
    XdapJsonWriter w=xdap_json_writer(owner,128);
    CHECK(xdap_json_text(&w,"{\"value\":",9));
    CHECK(xdap_json_quote(&w,"owned\0value",11));CHECK(xdap_json_text(&w,"}",1));
    XrJsonValue *output=xjson_parse(w.bytes,w.length);CHECK(output);
    XrJsonValue *string=xjson_get(output,"value");
    CHECK(string && string->string_len==11 && !memcmp(string->as.string,"owned\0value",11));
    xjson_free(output);xdap_json_writer_free(&w);
    owner_free(owner,baseline);
}
static void invalid_and_limits(void) {
    static const char *bad[]={"{\"seq\":1,\"\\u0073eq\":2}","[1,]","{\"x\":01}","{\"x\":1e}",
        "{\"x\":\"\\uD800\"}","{\"x\":\"\\u0000\"}","{}x","[true false]"};
    XrCompileResourceStats baseline;XrCompileResources *owner=owner_new(&baseline);
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        XdapJsonDocument d={0};CHECK(xdap_json_parse(owner,bad[i],strlen(bad[i]),256,32,&d)==XR_XIR_BAD_STRUCTURE);
        CHECK(!d.nodes && !d.bytes);
    }
    XdapJsonDocument d={0};CHECK(xdap_json_parse(owner,"[[[0]]]",7,64,2,&d)==XR_XIR_BUDGET);
    CHECK(!d.nodes && !d.bytes);
    CHECK(xdap_json_parse(owner,request,sizeof(request)-1,1,32,&d)==XR_XIR_BUDGET);
    XdapJsonWriter w=xdap_json_writer(owner,4);
    CHECK(!xdap_json_quote(&w,"abcde",5) && w.status==XR_XIR_BUDGET);xdap_json_writer_free(&w);
    w=xdap_json_writer(owner,64);
    const unsigned char invalid[]={0xc0,0x80};
    CHECK(!xdap_json_quote(&w,(const char *)invalid,sizeof(invalid)) && w.status==XR_XIR_BAD_STRUCTURE);
    xdap_json_writer_free(&w);owner_free(owner,baseline);
}
static size_t operation(unsigned mode,size_t fail) {
    XrCompileResourceStats baseline;XrCompileResources *owner=owner_new(&baseline);
    size_t begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XdapJsonDocument d={0};XdapJsonWriter w=xdap_json_writer(owner,4096);
    XrXirStatus status=XR_XIR_OK;
    if (!mode) status=xdap_json_parse(owner,request,sizeof(request)-1,256,32,&d);
    else { char data[700];memset(data,'a',sizeof(data));xdap_json_quote(&w,data,sizeof(data));status=w.status; }
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    CHECK(status==(fail==SIZE_MAX ? XR_XIR_OK : XR_XIR_OUT_OF_MEMORY));
    XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(owner,&spent)==XR_COMPILE_RESOURCE_OK);
    if (fail!=SIZE_MAX && !mode) {
        CHECK(!d.nodes && !d.bytes && spent.live_bytes==baseline.live_bytes);
        CHECK(xdap_json_parse(owner,request,sizeof(request)-1,256,32,&d)==XR_XIR_OK);
    }
    xdap_json_document_free(&d);xdap_json_writer_free(&w);
    if (fail!=SIZE_MAX && mode) {
        w=xdap_json_writer(owner,4096);char retry[700];memset(retry,'a',sizeof(retry));
        CHECK(xdap_json_quote(&w,retry,sizeof(retry)));xdap_json_writer_free(&w);
    }
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(owner,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.allocated_bytes>=spent.allocated_bytes && after.work>=spent.work);
    owner_free(owner,baseline);return sites;
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();
    if (argc==2 && !strcmp(argv[1],"--resources")) {
        for (unsigned mode=0;mode<2;++mode) { size_t sites=operation(mode,SIZE_MAX);CHECK(sites);
            for (size_t i=0;i<sites;++i) operation(mode,i);
            printf("JSON allocation slice %u: %zu sites PASS\n",mode,sites); }
    } else { CHECK(argc==1);requests_and_output();invalid_and_limits();puts("DAP JSON syntax, UTF-8/NUL output, duplicates and bounds: PASS"); }
    return 0;
}
