/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_code_lens.h"
#include "api/xsource_compile.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"

static const char root_text[]=
    "import { target } from \"./lib\"\n"
    "fn local(target:i64)->i64 { return target }\n"
    "export fn call()->i64 { return target() + local(1) }\n";
static const char lib_text[]="/* \xf0\x9f\x98\x80 */ export fn target()->i64 { return 41 }\n";
static const char struct_text[]=
    "/* \xf0\x9f\x98\x80 */ struct S {\n"
    " value:i64\n"
    " ref bump(x:ref i64)->i64 { this.value += x; return this.value }\n"
    " static pick()->i64 { return 1 }\n"
    "}\n"
    "fn outer()->i64 { return 1 }\n";
static const char class_text[]="/* \xf0\x9f\x98\x80 */ final class C {}\n";
static const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,
    UINT64_C(8)*1024*1024,UINT64_C(128000000)};

static char *input_copy(const char *text) {
    size_t length=strlen(text)+1;char *copy=sdk_fixture_malloc(length);CHECK(copy);
    memcpy(copy,text,length);return copy;
}

static void lens_fact(XrJsonValue *array,int index,int line,int column,int length,const char *title) {
    CHECK(array&&array->type==XR_JSON_ARRAY);
    XrJsonValue *lens=xjson_array_get(array,index);CHECK(lens&&lens->type==XR_JSON_OBJECT);
    XrJsonValue *range=xjson_get_object(lens,"range"),*start=xjson_get_object(range,"start"),*end=xjson_get_object(range,"end");
    CHECK(start&&end&&xjson_get_int(start,"line")==line&&xjson_get_int(end,"line")==line&&
        xjson_get_int(start,"character")==column&&xjson_get_int(end,"character")==column+length);
    XrJsonValue *command=xjson_get_object(lens,"command");
    CHECK(command&&!strcmp(xjson_get_string(command,"title"),title)&&!strcmp(xjson_get_string(command,"command"),""));
}
static void facts(XrJsonValue *root,XrJsonValue *lib,unsigned mode) {
    if(mode==0) {
        CHECK(root->as.array.count==2&&lib->as.array.count==1);
        lens_fact(root,0,1,3,5,"1 reference");lens_fact(root,1,2,10,4,"0 references");
        /* The import closure appears in two query roots. The real call counts
         * once, and both occurrences of the unrelated parameter count zero. */
        lens_fact(lib,0,0,19,6,"1 reference");
    } else if(mode==1) {
        CHECK(root->as.array.count==4&&!lib);
        lens_fact(root,0,0,16,1,"0 references");lens_fact(root,1,2,5,4,"0 references");
        lens_fact(root,2,3,8,4,"0 references");lens_fact(root,3,5,3,5,"0 references");
    } else {CHECK(root->as.array.count==1&&!lib);lens_fact(root,0,0,21,1,"0 references");}
}
static XrXirStatus once(const char *root_uri,const char *lib_uri,unsigned mode,
    XrCompileResourceLimits caps,XrCompileResourceStats *stats,size_t json_fault,bool *injected) {
    XrCompileResources *resources=NULL;XlspSourceWorkspace *workspace=NULL;XrJsonValue *root=NULL,*lib=NULL;
    XrCompileResourceStatus made=xr_compile_resources_new(&caps,&resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};unsigned stage=0;
    const char *selected=mode==0?root_text:mode==1?struct_text:class_text;
    size_t length=strlen(selected);char *owned=input_copy(selected),*root_copy=input_copy(root_uri),*lib_copy=input_copy(lib_uri);
    char library[sizeof(lib_text)];memcpy(library,lib_text,sizeof(library));
    XlspSourceDocument documents[2]={{root_copy,owned,strlen(root_uri),length,4},
        {lib_copy,library,strlen(lib_uri),sizeof(library)-1,9}};
    XrXirStatus status=xlsp_source_workspace_build(&context,documents,mode?1:2,&workspace,&stage,NULL);
    memset(owned,'?',length+1);memset(root_copy,'?',strlen(root_uri)+1);memset(lib_copy,'?',strlen(lib_uri)+1);
    sdk_fixture_free(owned);sdk_fixture_free(root_copy);sdk_fixture_free(lib_copy);
    memset(library,'?',sizeof(library));memset(documents,0,sizeof(documents));
    if(status==XR_XIR_OK) {
        CHECK(workspace&&stage==0);lsp_json_calls=0;lsp_json_fail=json_fault;lsp_json_injected=false;
        status=xlsp_source_code_lens_json(resources,workspace,root_uri,strlen(root_uri),&root);
        if(status==XR_XIR_OK&&mode==0)status=xlsp_source_code_lens_json(resources,workspace,lib_uri,strlen(lib_uri),&lib);
        if(injected)*injected=lsp_json_injected;
        lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
        if(status==XR_XIR_OK) {
            facts(root,lib,mode);XrJsonValue *occupied=root;size_t before=runtime_attempts,json_before=lsp_json_calls;
            CHECK(xlsp_source_code_lens_json(resources,workspace,root_uri,strlen(root_uri),&occupied)==XR_XIR_BAD_STRUCTURE&&
                occupied==root&&runtime_attempts==before&&lsp_json_calls==json_before);
        }
    } else CHECK(!workspace);
    if(stats)*stats=sdk_stats(resources);
    xr_compile_resources_release(resources);xlsp_source_workspace_free(workspace);CHECK(!runtime_live&&!runtime_bytes);
    if(status==XR_XIR_OK)facts(root,lib,mode);
    xjson_free(root);xjson_free(lib);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static int run(int argc,char **argv) {
    (void)sdk_ledger;(void)sdk_unlimited;
    CHECK(argc==3||argc==4);bool full=argc==3;CHECK(full||!strcmp(argv[3],"--normal"));
    for(unsigned mode=0;mode<3;++mode) {
        XrCompileResourceStats measured={0};runtime_attempts=0;runtime_fail_at=SIZE_MAX;
        CHECK(once(argv[1],argv[2],mode,limits,&measured,SIZE_MAX,NULL)==XR_XIR_OK);
        size_t source_sites=runtime_attempts,json_sites=lsp_json_calls;CHECK(source_sites&&json_sites);
        if(full) {
            for(size_t fault=0;fault<source_sites;++fault) {
                runtime_attempts=0;runtime_fail_at=fault;
                CHECK(once(argv[1],argv[2],mode,limits,NULL,SIZE_MAX,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>fault);
            }
            runtime_fail_at=SIZE_MAX;
            for(size_t fault=0;fault<json_sites;++fault) {
                bool injected=false;
                CHECK(once(argv[1],argv[2],mode,limits,NULL,fault,&injected)==XR_XIR_OUT_OF_MEMORY&&injected);
            }
            for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
                XrCompileResourceLimits exact={measured.allocated_bytes,measured.peak_bytes,measured.work};
                uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;
                CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
                CHECK(once(argv[1],argv[2],mode,exact,NULL,SIZE_MAX,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
            }
        }
        printf("Source CodeLens mode=%u SourceFI=%zu JSONFI=%zu full=%u; binding counts, exactUTF16, producer death, physical=0/0\n",
            mode,source_sites,json_sites,full?1u:0u);
    }
    return 0;
}
#if XR_OS_WINDOWS
int wmain(int argc,wchar_t **wide) {
    XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);
    int result=run(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;
}
#else
int main(int argc,char **argv) {return run(argc,argv);}
#endif
