/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "app/lsp/xlsp_source_workspace.h"
#include "app/cli/xcli_canonical_source.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
static const char *workspace_specifier="./lib";
static const char root_text[]="import { value } from \"%s\"\nexport fn result()->i64 { /* \xf0\x9f\x98\x80 */ return value() }\n";
static const char lib_text[]="/* \xf0\x9f\x98\x80 */ export fn value()->i64 { return 41 }\n";
static void facts(XrJsonValue *out,const char *uri) {
    CHECK(out&&out->type==XR_JSON_OBJECT&&!strcmp(xjson_get_string(out,"uri"),uri));
    XrJsonValue *range=xjson_get_object(out,"range"),*start=xjson_get_object(range,"start"),*end=xjson_get_object(range,"end");
    CHECK(xjson_get_int(start,"line")==0&&xjson_get_int(start,"character")==19&&
        xjson_get_int(end,"line")==0&&xjson_get_int(end,"character")==24);
}
static XrXirStatus once(const char *root_uri,const char *lib_uri,XrCompileResourceLimits limits,XrCompileResourceStats *stats) {
    XrCompileResources *resources=NULL;XlspSourceWorkspace *workspace=NULL;XrJsonValue *out=NULL;
    XrCompileResourceStatus made=xr_compile_resources_new(&limits,&resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};unsigned stage=0;
    char root[512],lib[sizeof(lib_text)];int root_length=snprintf(root,sizeof(root),root_text,workspace_specifier);
    CHECK(root_length>0&&(size_t)root_length<sizeof(root));memcpy(lib,lib_text,sizeof(lib));
    XlspSourceDocument documents[2]={{root_uri,root,strlen(root_uri),(size_t)root_length,1},
        {lib_uri,lib,strlen(lib_uri),sizeof(lib)-1,7}};
    XrXirStatus status=xlsp_source_workspace_build(&context,documents,2,&workspace,&stage,NULL);
    memset(root,'?',sizeof(root));memset(lib,'?',sizeof(lib));memset(documents,0,sizeof(documents));
    if(status==XR_XIR_OK) {
        CHECK(workspace&&stage==0);
        status=xlsp_source_workspace_navigation(workspace,root_uri,strlen(root_uri),(XrLspPosition){1,43},0,false,&out);
        if(status==XR_XIR_OK)facts(out,lib_uri);else CHECK(!out);
    } else CHECK(!workspace);
    if(stats)*stats=sdk_stats(resources);
    xr_compile_resources_release(resources);xlsp_source_workspace_free(workspace);
    CHECK(!runtime_live&&!runtime_bytes);
    if(out)facts(out,lib_uri);xjson_free(out);
    return status;
}
static int run(int argc,char **argv) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_ledger;(void)sdk_unlimited;
    CHECK(argc==4);workspace_specifier=argv[3];XrCompileResourceLimits limits=xr_cli_compile_default_resource_limits();
    size_t sites=0;XrCompileResourceStats stats={0};
    for(size_t probe=0;probe<=sites;++probe) {
        runtime_attempts=0;runtime_fail_at=probe?probe-1:SIZE_MAX;
        XrXirStatus status=once(argv[1],argv[2],limits,probe?NULL:&stats);
        if(!probe){CHECK(status==XR_XIR_OK);sites=runtime_attempts;CHECK(sites);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>=probe);
        CHECK(!runtime_live&&!runtime_bytes);
    }
    runtime_fail_at=SIZE_MAX;
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};
        uint64_t *n=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;
        CHECK(*n&&*n<INT64_MAX);*n=(uint64_t)((int64_t)*n+delta);
        CHECK(once(argv[1],argv[2],exact,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
    }
    printf("LSP actual stdlib/Catalog/TEXT authority/Session/overlay/query fresh sites=%zu, ledger physical=0/0\n",sites);
    return 0;
}
int wmain(int argc,wchar_t **wide) {
    XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);
    int result=run(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;
}
